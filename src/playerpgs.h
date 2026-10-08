#pragma once
#include <QQuickPaintedItem>
#include <QImage>
#include <QRect>
#include <QVector>
#include <QVariantList>
#include <QPointer>
#include <qqml.h>

class QProcess;

// One object placed on the PGS canvas, still RLE-encoded (decoded lazily when its cue is shown).
struct PgsObject {
    int x = 0, y = 0;          // position on the canvas
    int width = 0, height = 0; // object size
    bool cropped = false;
    QRect crop;                // relative to the object, when cropped
    QByteArray rle;
};

// One display set that puts something on screen, from `startMs` until `endMs`.
struct PgsCue {
    qint64 startMs = 0;
    qint64 endMs = 0;
    int canvasWidth = 1920;
    int canvasHeight = 1080;
    QVector<QRgb> palette;     // 256 premultiplied ARGB entries
    QVector<PgsObject> objects;
};

// Renders Blu-ray PGS ("hdmv_pgs_subtitle") image subtitles, which Qt Multimedia cannot draw.
// QML type `PgsSubtitles` in module QtFlix. Fill the player with it and keep `videoRect` in sync with
// VideoOutput.contentRect.
//
// - Setting `file` probes the subtitle streams with ffprobe (async) and fills `streams`.
// - Setting `stream` (ordinal among the file's subtitle streams, -1 = off) copies that stream out with
//   `ffmpeg -map 0:s:<n> -c copy -f sup` (no decoding, cached under CacheLocation/pgs/), parses the PGS
//   segments on a worker thread and draws the cue for `positionMs`.
// - The PGS canvas (usually 1920x1080) is fitted to the video width and centred on the video, the way a
//   Blu-ray player shows it; `lift` raises a cue only when it would sit under the player controls.
class PgsSubtitles : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString file READ file WRITE setFile NOTIFY fileChanged)
    Q_PROPERTY(int stream READ stream WRITE setStream NOTIFY streamChanged)
    Q_PROPERTY(qint64 positionMs READ positionMs WRITE setPositionMs NOTIFY positionMsChanged)
    Q_PROPERTY(QRectF videoRect READ videoRect WRITE setVideoRect NOTIFY videoRectChanged)
    Q_PROPERTY(qreal lift READ lift WRITE setLift NOTIFY liftChanged)
    Q_PROPERTY(bool streamsKnown READ streamsKnown NOTIFY streamsChanged)
    Q_PROPERTY(QVariantList streams READ streams NOTIFY streamsChanged) // [{codec, language, title, pgs}]
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(int cueCount READ cueCount NOTIFY stateChanged)
public:
    explicit PgsSubtitles(QQuickItem *parent = nullptr);
    ~PgsSubtitles() override;

    QString file() const { return m_file; }
    void setFile(const QString &f);
    int stream() const { return m_stream; }
    void setStream(int s);
    qint64 positionMs() const { return m_pos; }
    void setPositionMs(qint64 ms);
    QRectF videoRect() const { return m_videoRect; }
    void setVideoRect(const QRectF &r);
    qreal lift() const { return m_lift; }
    void setLift(qreal l);
    bool streamsKnown() const { return m_streamsKnown; }
    QVariantList streams() const { return m_streams; }
    bool loading() const { return m_loading; }
    bool ready() const { return !m_cues.isEmpty(); }
    int cueCount() const { return int(m_cues.size()); }

    // True when subtitle stream `ordinal` is PGS and this item will render it.
    Q_INVOKABLE bool isPgs(int ordinal) const;

    void paint(QPainter *painter) override;

    // Exposed for tests.
    static QVector<PgsCue> parseSup(const QByteArray &data);
    static QImage decodeObject(const PgsObject &obj, const QVector<QRgb> &palette);

signals:
    void fileChanged();
    void streamChanged();
    void positionMsChanged();
    void videoRectChanged();
    void liftChanged();
    void streamsChanged();
    void stateChanged();

private:
    void probe();
    void load();
    void stopProcesses();
    void setCues(QVector<PgsCue> cues, quint64 gen);
    void updateCurrent();
    QString cachePathFor(int ordinal) const;

    QString m_file;
    int m_stream = -1;
    qint64 m_pos = 0;
    QRectF m_videoRect;
    qreal m_lift = 0;
    bool m_streamsKnown = false;
    QVariantList m_streams;
    bool m_loading = false;
    quint64 m_gen = 0;                // bumps on every file/stream change; stale results are dropped
    QPointer<QProcess> m_probe;
    QPointer<QProcess> m_extract;
    QVector<PgsCue> m_cues;
    int m_current = -1;
    QVector<QPair<QRect, QImage>> m_images; // decoded objects of the current cue, in canvas coordinates
};
