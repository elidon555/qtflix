#pragma once
#include <QObject>
#include <QObject>
#include <qqml.h>

// Dev aid for the DevPlayer harness (QML type `DecoderProbe` in module QtFlix): which video decoder Qt
// Multimedia's FFmpeg backend picked. Qt has no public API for that, so while `active` it listens to the
// backend's own debug output (categories qt.multimedia.ffmpeg.hwaccel / .playbackengine, swallowed unless
// QT_LOGGING_RULES asks for them).
//
//   DecoderProbe { id: probe; active: true }
//   console.log(probe.decoder, probe.describeFrame(videoOutput.videoSink))   // "hw (vaapi)", "cpu nv12"
class DecoderProbe : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)
    Q_PROPERTY(QString decoder READ decoder NOTIFY decoderChanged FINAL) // "hw (<device>)" | "sw" | "" (unknown yet)
public:
    explicit DecoderProbe(QObject *parent = nullptr);
    ~DecoderProbe() override;

    bool active() const { return m_active; }
    void setActive(bool a);
    QString decoder() const { return m_decoder; }

    // Where the sink's current frame lives and its pixel format, e.g. "gpu texture nv12" or "cpu nv12".
    Q_INVOKABLE QString describeFrame(QObject *videoSink) const;

signals:
    void activeChanged();
    void decoderChanged();

private:
    static void handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg);
    void setDecoder(const QString &d);

    bool m_active = false;
    QString m_decoder;
};
