#include "playerdiagnostics.h"

#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

namespace {
QtMessageHandler s_previous = nullptr;
QPointer<DecoderProbe> s_probe;
bool s_forward = false;            // QT_LOGGING_RULES is set: the user wants the backend's output too

// AVHWDeviceType values as printed by the backend ("Selected format 44 for hw 3")
QString deviceName(int type)
{
    static const char *const names[] = {"none", "vdpau", "cuda", "vaapi", "dxva2", "qsv", "videotoolbox",
                                        "d3d11va", "drm", "opencl", "mediacodec", "vulkan", "d3d12va"};
    return type >= 0 && type < int(std::size(names)) ? QString::fromLatin1(names[type]) : QString::number(type);
}
}

DecoderProbe::DecoderProbe(QObject *parent) : QObject(parent) {}

DecoderProbe::~DecoderProbe()
{
    setActive(false);
}

void DecoderProbe::setActive(bool a)
{
    if (a == m_active)
        return;
    if (a && s_probe)
        return; // one probe at a time
    m_active = a;
    if (a) {
        s_probe = this;
        s_forward = qEnvironmentVariableIsSet("QT_LOGGING_RULES");
        QLoggingCategory::setFilterRules(QStringLiteral("qt.multimedia.ffmpeg.hwaccel.debug=true\n"
                                                        "qt.multimedia.ffmpeg.playbackengine.debug=true"));
        s_previous = qInstallMessageHandler(handler);
    } else {
        qInstallMessageHandler(s_previous);
        QLoggingCategory::setFilterRules(QString());
        s_probe = nullptr;
    }
    emit activeChanged();
}

void DecoderProbe::setDecoder(const QString &d)
{
    if (d == m_decoder)
        return;
    m_decoder = d;
    emit decoderChanged();
}

void DecoderProbe::handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    const QLatin1StringView cat(ctx.category ? ctx.category : "");
    const bool backend = type == QtDebugMsg && cat.startsWith(QLatin1String("qt.multimedia.ffmpeg."));
    if (backend) {
        QString decoder;
        if (cat == QLatin1String("qt.multimedia.ffmpeg.playbackengine")
            && msg.contains(QLatin1String("Create codec")) && msg.contains(QLatin1String("VideoStream"))) {
            decoder = QStringLiteral("sw"); // until the codec negotiates a hardware format
        } else if (cat == QLatin1String("qt.multimedia.ffmpeg.hwaccel")) {
            static const QRegularExpression hw(QStringLiteral("Selected format \\S+ for hw (\\d+)"));
            const QRegularExpressionMatch m = hw.match(msg);
            if (m.hasMatch())
                decoder = QStringLiteral("hw (%1)").arg(deviceName(m.captured(1).toInt()));
        }
        // messages come from the backend's threads
        if (!decoder.isEmpty() && s_probe)
            QMetaObject::invokeMethod(s_probe.data(), [decoder] { if (s_probe) s_probe->setDecoder(decoder); },
                                      Qt::QueuedConnection);
        if (!s_forward)
            return;
    }
    if (s_previous)
        s_previous(type, ctx, msg);
}

QString DecoderProbe::describeFrame(QObject *videoSink) const
{
    auto *sink = qobject_cast<QVideoSink *>(videoSink);
    const QVideoFrame frame = sink ? sink->videoFrame() : QVideoFrame();
    if (!frame.isValid())
        return QStringLiteral("none");
    const QString where = frame.handleType() == QVideoFrame::RhiTextureHandle ? QStringLiteral("gpu texture")
                                                                              : QStringLiteral("cpu");
    return where + QLatin1Char(' ') + QVideoFrameFormat::pixelFormatToString(frame.pixelFormat()).toLower();
}
