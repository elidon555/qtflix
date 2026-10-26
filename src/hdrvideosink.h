#pragma once
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QVideoSink>
#include <qqml.h>

class QVideoFrame;

// Sits between a MediaPlayer and a VideoOutput and fixes HDR10 (PQ) frames before Qt Multimedia draws
// them (QML type `HdrVideoSink` in module QtFlix). Everything else is passed through untouched.
//
//   MediaPlayer { videoOutput: HdrVideoSink { output: vout } }
//   VideoOutput { id: vout }
//
// Why: Qt 6.10 tone-maps PQ to SDR with a BT.2390 curve whose source peak is the frame's maxLuminance and
// whose target is fixed at 100 nits, in PQ units normalized to 10000 nits. Two things go wrong:
//  - its FFmpeg backend multiplies the mastering display's max_luminance (already in nits) by 10000, so a
//    1000-nit master reads as 10^7 nits; the curve then barely compresses and everything above ~100 nits
//    (skies, daylight) clips to white;
//  - with the true value the curve's target stays normalized to 10000 nits, so a 1000-nit master comes
//    out dim and grey (peak ~25 nits).
// The curve is only consistent for a 10000-nit source (what Qt assumes when a file has no metadata), so
// PQ frames are relabeled to that. A frame's format can only be changed by building a new frame, so the
// source is mapped (hardware frames are downloaded) on the backend's delivery thread and its planes are
// reused without copying. Software-decoded 10-bit 4:2:0 (yuv420p10) frames are repacked to P010, since
// Qt only tone-maps P010/P016.
//
// Logging: QT_LOGGING_RULES="qtflix.hdr.debug=true" prints the per-frame cost once a second.
class HdrVideoSink : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *output READ output WRITE setOutput NOTIFY outputChanged FINAL) // VideoOutput or QVideoSink
public:
    explicit HdrVideoSink(QObject *parent = nullptr);
    ~HdrVideoSink() override;

    QObject *output() const { return m_output; }
    void setOutput(QObject *output);

    // what MediaPlayer.videoOutput looks up on a non-sink object
    Q_INVOKABLE QVideoSink *videoSink() const { return m_input; }

signals:
    void outputChanged();

private:
    void setTarget(QVideoSink *target);
    void deliver(const QVideoFrame &frame);          // backend thread
    void deliverSubtitle(const QString &text);       // backend thread

    QVideoSink *m_input = nullptr;
    QPointer<QObject> m_output;
    QMutex m_targetMutex;                            // guards m_target against the backend thread
    QVideoSink *m_target = nullptr;
    QMetaObject::Connection m_targetGone;
    QMetaObject::Connection m_outputGone;
};
