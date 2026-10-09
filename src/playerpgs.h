#pragma once
#include <QQuickItem>
#include <QHash>
#include <QImage>
#include <QRect>
#include <QVector>
#include <QVariantList>
#include <QPointer>
#include <memory>
#include <qqml.h>

class QProcess;
class QTimer;
class PgsParser;

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
// - Setting `stream` (ordinal among the file's subtitle streams, -1 = off) copies every PGS stream of the
//   file out in one pass, `ffmpeg -map 0:s:<a> -c copy -f sup a.part -map 0:s:<b> ...` (no decoding, idle
//   I/O priority, cached under CacheLocation/pgs/), and parses the selected one on a worker thread while
//   it is still being written, so the first cues show long before the pass reaches the end of the file.
// - Each object of the current cue becomes one scene graph texture of its own size, scaled on the GPU;
//   no node at all while no cue is on screen.
// - The PGS canvas (usually 1920x1080) is fitted to the video width and centred on the video, the way a
//   Blu-ray player shows it; `lift` raises a cue only when it would sit under the player controls.
class PgsSubtitles : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString file READ file WRITE setFile NOTIFY fileChanged FINAL)
    Q_PROPERTY(int stream READ stream WRITE setStream NOTIFY streamChanged FINAL)
    Q_PROPERTY(qint64 positionMs READ positionMs WRITE setPositionMs NOTIFY positionMsChanged FINAL)
    Q_PROPERTY(QRectF videoRect READ videoRect WRITE setVideoRect NOTIFY videoRectChanged FINAL)
    Q_PROPERTY(qreal lift READ lift WRITE setLift NOTIFY liftChanged FINAL)
    Q_PROPERTY(bool streamsKnown READ streamsKnown NOTIFY streamsChanged FINAL)
    Q_PROPERTY(QVariantList streams READ streams NOTIFY streamsChanged FINAL) // [{codec, language, title, pgs}]
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged FINAL)
    Q_PROPERTY(int cueCount READ cueCount NOTIFY stateChanged FINAL)
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

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void probe();
    void load();
    void extract(const QList<int> &ordinals);
    void stopProcesses();
    void resetCues();
    void parseMore();
    void setCues(QVector<PgsCue> cues);
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
    QPointer<QProcess> m_extract;     // one pass for all PGS streams of m_file
    QHash<int, QString> m_extracting; // ordinal -> .sup path being written by m_extract
    QTimer *m_poll = nullptr;         // re-reads the growing .part of the selected stream

    // Incremental parse of the selected stream (one worker task at a time).
    std::shared_ptr<PgsParser> m_parser;
    QString m_parsePath;              // file the parser reads from (.part while extracting)
    qint64 m_parseOffset = 0;         // bytes of m_parsePath already fed to the parser
    bool m_parseBusy = false;
    bool m_parseAgain = false;

    QVector<PgsCue> m_cues;
    int m_current = -1;
    qint64 m_currentStart = -1;       // startMs of the cue in m_images (survives cue list updates)
    QVector<QPair<QRect, QImage>> m_images; // decoded objects of the current cue, in canvas coordinates
    QSize m_canvas;                   // canvas of the current cue
    bool m_imagesDirty = false;       // m_images changed since the last updatePaintNode()
};
