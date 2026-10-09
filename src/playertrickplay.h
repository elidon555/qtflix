#pragma once
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantMap>
#include <QVector>
#include <qqml.h>

class QProcess;
class QTimer;
struct TrickplayJob;

// Child-process setup for background helpers (trickplay, PGS extraction): nice 19, idle I/O class, and
// on Linux killed together with the app.
void setBackgroundPriority(QProcess *proc);

// Timeline hover thumbnails ("trickplay") for the player (QML type `Trickplay` in module QtFlix).
//
// One ffmpeg pass per file decodes only key frames, samples one frame every `intervalMs`, scales it
// to 240 px wide and tiles 5x5 frames per JPEG sprite sheet:
//   ffmpeg -hwaccel auto -threads 1 -skip_frame nokey -i <file> -vf fps=1/10,scale=240:-2,tile=5x5 -q:v 5
//          -atomic_writing 1 <dir>/%03d.jpg
// (small sheets: ~1200x675, a few MB decoded, quick to load while scrubbing). Sheets are cached in
// QStandardPaths::CacheLocation/trick/<sha1(path)>/; meta.json (versioned) marks a complete set.
// Sheets become usable as soon as ffmpeg has written them, so previews cover the start of the timeline
// long before the pass ends. Generation runs at nice 19 / idle I/O, one ffmpeg process at a time for the
// whole app, starts `startDelayMs` after `file` is set and never blocks the UI thread. A pass that has
// started keeps running in the background when its Trickplay goes away (player closed, next episode), so
// the work is not lost; it is killed when the app exits. A request that is still waiting is dropped.
//
//   Trickplay { id: trick; file: "/abs/movie.mkv"; durationMs: player.duration }
//   // f = trick.frameFor(ms) -> {source, x, y, index}; show `source` in a clipped Image at (-x, -y)
class Trickplay : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString file READ file WRITE setFile NOTIFY fileChanged FINAL)
    Q_PROPERTY(qint64 durationMs READ durationMs WRITE setDurationMs NOTIFY durationMsChanged FINAL)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged FINAL)
    Q_PROPERTY(int startDelayMs MEMBER m_startDelayMs NOTIFY startDelayMsChanged FINAL)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged FINAL)             // at least one sheet is usable
    Q_PROPERTY(bool complete READ complete NOTIFY readyChanged FINAL)       // every sheet is there
    Q_PROPERTY(bool generating READ generating NOTIFY generatingChanged FINAL)
    Q_PROPERTY(QUrl source READ source NOTIFY readyChanged FINAL)          // first sprite sheet
    Q_PROPERTY(int intervalMs READ intervalMs NOTIFY readyChanged FINAL)
    Q_PROPERTY(int columns READ columns NOTIFY readyChanged FINAL)
    Q_PROPERTY(int rows READ rows NOTIFY readyChanged FINAL)                // rows per sheet
    Q_PROPERTY(int frameWidth READ frameWidth NOTIFY readyChanged FINAL)
    Q_PROPERTY(int frameHeight READ frameHeight NOTIFY readyChanged FINAL)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY readyChanged FINAL)    // frames usable so far
    Q_PROPERTY(int sheetCount READ sheetCount NOTIFY readyChanged FINAL)
    Q_PROPERTY(qint64 lastGenerationMs READ lastGenerationMs NOTIFY readyChanged FINAL) // wall time of the last run, 0 = cache hit
public:
    explicit Trickplay(QObject *parent = nullptr);
    ~Trickplay() override;

    QString file() const { return m_file; }
    void setFile(const QString &f);
    qint64 durationMs() const { return m_duration; }
    void setDurationMs(qint64 ms);
    bool enabled() const { return m_enabled; }
    void setEnabled(bool e);
    bool ready() const { return !m_urls.isEmpty() && m_frameW > 0; }
    bool complete() const { return m_complete; }
    bool generating() const { return m_job != nullptr; }
    QUrl source() const { return m_urls.value(0); }
    int intervalMs() const { return m_interval; }
    int columns() const { return m_columns; }
    int rows() const { return m_rows; }
    int frameWidth() const { return m_frameW; }
    int frameHeight() const { return m_frameH; }
    int frameCount() const { return m_frameCount; }
    int sheetCount() const { return int(m_urls.size()); }
    qint64 lastGenerationMs() const { return m_genMs; }

    // {source(url), x, y, index} of the tile nearest to `ms`; empty map when that part of the timeline
    // has no sheet (yet).
    Q_INVOKABLE QVariantMap frameFor(qint64 ms) const;
    // URL of sheet `index` (0-based) when it exists, else empty: lets QML preload neighbouring sheets.
    Q_INVOKABLE QUrl sheetSource(int index) const { return m_urls.value(index); }

signals:
    void fileChanged();
    void durationMsChanged();
    void enabledChanged();
    void startDelayMsChanged();
    void readyChanged();
    void generatingChanged();

private:
    friend class TrickplayQueue;
    void reset();
    void schedule();
    bool loadMeta();
    void detach();                       // stop following m_job (it keeps running if it has started)
    void jobProgress(int sheets);        // called by the queue: `sheets` sheets are on disk
    void jobFinished(bool ok, qint64 tookMs);
    void updateFrameCount();

    QString m_file;
    QString m_dir;                       // <cache>/trick/<sha1(m_file)>, computed once per file
    qint64 m_duration = 0;
    bool m_enabled = true;
    int m_startDelayMs = 1500;
    bool m_complete = false;
    int m_interval = 10000;
    int m_columns = 5, m_rows = 5;
    int m_frameW = 0, m_frameH = 0, m_frameCount = 0;
    QVector<QUrl> m_urls;                // one per usable sheet
    qint64 m_genMs = 0;
    TrickplayJob *m_job = nullptr;       // owned by the queue
    QTimer *m_delay = nullptr;
};
