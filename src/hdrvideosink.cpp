#include "hdrvideosink.h"

#include <QAbstractVideoBuffer>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <vector>

namespace {
Q_LOGGING_CATEGORY(lcHdr, "qtflix.hdr", QtWarningMsg)

// the only source peak Qt's PQ tone curve handles consistently (see the header)
constexpr float kPqPeakNits = 10000.f;

// Planes of a frame presented under another format: either a source frame kept mapped for the buffer's
// lifetime (no copy) or planes we own.
class RelabeledBuffer : public QAbstractVideoBuffer
{
public:
    RelabeledBuffer(QVideoFrame mappedSource, const QVideoFrameFormat &format)
        : m_source(std::move(mappedSource)), m_format(format)
    {
        m_data.planeCount = m_source.planeCount();
        for (int i = 0; i < m_data.planeCount; ++i) {
            m_data.data[i] = m_source.bits(i);
            m_data.bytesPerLine[i] = m_source.bytesPerLine(i);
            m_data.dataSize[i] = m_source.mappedBytes(i);
        }
    }
    RelabeledBuffer(std::vector<quint16> storage, const MapData &data, const QVideoFrameFormat &format)
        : m_storage(std::move(storage)), m_format(format), m_data(data)
    {
    }
    ~RelabeledBuffer() override
    {
        if (m_source.isMapped())
            m_source.unmap();
    }

    MapData map(QVideoFrame::MapMode) override { return m_data; }
    QVideoFrameFormat format() const override { return m_format; }

private:
    QVideoFrame m_source;
    std::vector<quint16> m_storage;
    QVideoFrameFormat m_format;
    MapData m_data;
};

bool needsFix(const QVideoFrame &frame)
{
    if (!frame.isValid() || frame.surfaceFormat().colorTransfer() != QVideoFrameFormat::ColorTransfer_ST2084)
        return false;
    switch (frame.pixelFormat()) {
    case QVideoFrameFormat::Format_P010:
    case QVideoFrameFormat::Format_P016:
        return frame.surfaceFormat().maxLuminance() != kPqPeakNits;
    case QVideoFrameFormat::Format_YUV420P10:
        return true;
    default:
        return false;
    }
}

// yuv420p10 (10 bits in the low bits of three planes) -> P010 (high bits, interleaved chroma)
std::unique_ptr<QAbstractVideoBuffer> repackToP010(const QVideoFrame &src, QVideoFrameFormat format)
{
    const int w = src.width(), h = src.height(), cw = (w + 1) / 2, ch = (h + 1) / 2;
    std::vector<quint16> storage(size_t(w) * h + size_t(cw) * 2 * ch);
    quint16 *y = storage.data();
    quint16 *uv = y + size_t(w) * h;
    for (int r = 0; r < h; ++r) {
        const auto *in = reinterpret_cast<const quint16 *>(src.bits(0) + qsizetype(r) * src.bytesPerLine(0));
        quint16 *out = y + size_t(r) * w;
        for (int x = 0; x < w; ++x)
            out[x] = quint16(in[x] << 6);
    }
    for (int r = 0; r < ch; ++r) {
        const auto *u = reinterpret_cast<const quint16 *>(src.bits(1) + qsizetype(r) * src.bytesPerLine(1));
        const auto *v = reinterpret_cast<const quint16 *>(src.bits(2) + qsizetype(r) * src.bytesPerLine(2));
        quint16 *out = uv + size_t(r) * cw * 2;
        for (int x = 0; x < cw; ++x) {
            out[2 * x] = quint16(u[x] << 6);
            out[2 * x + 1] = quint16(v[x] << 6);
        }
    }
    QAbstractVideoBuffer::MapData data;
    data.planeCount = 2;
    data.data[0] = reinterpret_cast<uchar *>(y);
    data.data[1] = reinterpret_cast<uchar *>(uv);
    data.bytesPerLine[0] = w * 2;
    data.bytesPerLine[1] = cw * 4;
    data.dataSize[0] = data.bytesPerLine[0] * h;
    data.dataSize[1] = data.bytesPerLine[1] * ch;
    return std::make_unique<RelabeledBuffer>(std::move(storage), data, format);
}

// The same picture labeled as a 10000-nit PQ source; an invalid frame when the source cannot be mapped.
QVideoFrame relabeled(QVideoFrame src)
{
    if (!src.map(QVideoFrame::ReadOnly))
        return {};
    const QVideoFrameFormat in = src.surfaceFormat();
    std::unique_ptr<QAbstractVideoBuffer> buffer;
    if (src.pixelFormat() == QVideoFrameFormat::Format_YUV420P10) {
        QVideoFrameFormat format(in.frameSize(), QVideoFrameFormat::Format_P010);
        format.setViewport(in.viewport());
        format.setScanLineDirection(in.scanLineDirection());
        format.setStreamFrameRate(in.streamFrameRate());
        format.setColorSpace(in.colorSpace());
        format.setColorTransfer(in.colorTransfer());
        format.setColorRange(in.colorRange());
        format.setRotation(in.rotation());
        format.setMirrored(in.isMirrored());
        format.setMaxLuminance(kPqPeakNits);
        buffer = repackToP010(src, format);
        src.unmap();
    } else {
        QVideoFrameFormat format = in;
        format.setMaxLuminance(kPqPeakNits);
        buffer = std::make_unique<RelabeledBuffer>(src, format);
    }
    QVideoFrame out(std::move(buffer));
    out.setStartTime(src.startTime());
    out.setEndTime(src.endTime());
    out.setRotation(src.rotation());
    out.setMirrored(src.mirrored());
    out.setSubtitleText(src.subtitleText());
    return out;
}

// qtflix.hdr.debug: average relabel cost per frame, once a second (frames arrive on one thread at a time)
struct Stats
{
    QElapsedTimer clock;
    qint64 ns = 0;
    int frames = 0;

    void add(qint64 frameNs, const QVideoFrame &frame)
    {
        if (!clock.isValid())
            clock.start();
        ns += frameNs;
        ++frames;
        if (clock.elapsed() < 1000)
            return;
        qCDebug(lcHdr, "relabeled %d %s frame(s) %dx%d, %.1f ms each", frames,
                qPrintable(QVideoFrameFormat::pixelFormatToString(frame.pixelFormat())), frame.width(),
                frame.height(), ns / 1e6 / frames);
        clock.restart();
        ns = 0;
        frames = 0;
    }
};
thread_local Stats t_stats;
}

HdrVideoSink::HdrVideoSink(QObject *parent) : QObject(parent), m_input(new QVideoSink(this))
{
    connect(m_input, &QVideoSink::videoFrameChanged, this, &HdrVideoSink::deliver, Qt::DirectConnection);
    connect(m_input, &QVideoSink::subtitleTextChanged, this, &HdrVideoSink::deliverSubtitle, Qt::DirectConnection);
}

HdrVideoSink::~HdrVideoSink()
{
    setTarget(nullptr);
}

void HdrVideoSink::setOutput(QObject *output)
{
    if (output == m_output)
        return;
    disconnect(m_outputGone);
    m_output = output;
    QVideoSink *sink = qobject_cast<QVideoSink *>(output);
    if (!sink && output)
        QMetaObject::invokeMethod(output, "videoSink", Q_RETURN_ARG(QVideoSink *, sink));
    if (output)
        m_outputGone = connect(output, &QObject::destroyed, this, [this] { setTarget(nullptr); },
                               Qt::DirectConnection);
    setTarget(sink);
    emit outputChanged();
}

void HdrVideoSink::setTarget(QVideoSink *target)
{
    disconnect(m_targetGone);
    {
        QMutexLocker lock(&m_targetMutex);
        m_target = target;
    }
    if (target)
        m_targetGone = connect(target, &QObject::destroyed, this, [this] { setTarget(nullptr); },
                               Qt::DirectConnection);
}

void HdrVideoSink::deliver(const QVideoFrame &frame)
{
    QVideoFrame out = frame;
    if (needsFix(frame)) {
        QElapsedTimer t;
        t.start();
        if (QVideoFrame fixed = relabeled(frame); fixed.isValid())
            out = fixed;
        if (lcHdr().isDebugEnabled())
            t_stats.add(t.nsecsElapsed(), frame);
    }
    QMutexLocker lock(&m_targetMutex);
    if (m_target)
        m_target->setVideoFrame(out);
}

void HdrVideoSink::deliverSubtitle(const QString &text)
{
    QMutexLocker lock(&m_targetMutex);
    if (m_target)
        m_target->setSubtitleText(text);
}
