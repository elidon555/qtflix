#pragma once
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantMap>
#include <QElapsedTimer>
#include <qqml.h>

class QProcess;
class QTimer;

// Timeline hover thumbnails ("trickplay") for the player (QML type `Trickplay` in module QtFlix).
//
// One ffmpeg pass per file decodes only key frames, samples one frame every `intervalMs`, scales it
// to 240 px wide and tiles 10x10 frames per JPEG sprite sheet:
//   ffmpeg -hwaccel auto -skip_frame nokey -i <file> -vf fps=1/10,scale=240:-2,tile=10x10 -q:v 5 <dir>/%03d.jpg
// (several 2400 px wide sheets instead of one very tall image so every sheet stays well below the GPU
// texture size limit). Sheets are cached in QStandardPaths::CacheLocation/trick/<sha1(path)>/.
// Generation runs at nice 19, one ffmpeg process at a time for the whole app, starts `startDelayMs`
// after `file` is set and never blocks the UI thread. The running process is killed when the object
// is destroyed or `file` changes.
//
//   Trickplay { id: trick; file: "/abs/movie.mkv"; durationMs: player.duration }
//   // f = trick.frameFor(ms) -> {source, x, y, index}; show `source` in a clipped Image at (-x, -y)
class Trickplay : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString file READ file WRITE setFile NOTIFY fileChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs WRITE setDurationMs NOTIFY durationMsChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(int startDelayMs MEMBER m_startDelayMs NOTIFY startDelayMsChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool generating READ generating NOTIFY generatingChanged)
    Q_PROPERTY(QUrl source READ source NOTIFY readyChanged)          // first sprite sheet
    Q_PROPERTY(int intervalMs READ intervalMs NOTIFY readyChanged)
    Q_PROPERTY(int columns READ columns NOTIFY readyChanged)
    Q_PROPERTY(int rows READ rows NOTIFY readyChanged)                // rows per sheet
    Q_PROPERTY(int frameWidth READ frameWidth NOTIFY readyChanged)
    Q_PROPERTY(int frameHeight READ frameHeight NOTIFY readyChanged)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY readyChanged)
    Q_PROPERTY(int sheetCount READ sheetCount NOTIFY readyChanged)
    Q_PROPERTY(qint64 lastGenerationMs READ lastGenerationMs NOTIFY readyChanged) // wall time of the last run, 0 = cache hit
public:
    explicit Trickplay(QObject *parent = nullptr);
    ~Trickplay() override;

    QString file() const { return m_file; }
    void setFile(const QString &f);
    qint64 durationMs() const { return m_duration; }
    void setDurationMs(qint64 ms);
    bool enabled() const { return m_enabled; }
    void setEnabled(bool e);
    bool ready() const { return m_ready; }
    bool generating() const { return m_proc != nullptr; }
    QUrl source() const { return m_ready ? sheetUrl(0) : QUrl(); }
    int intervalMs() const { return m_interval; }
    int columns() const { return m_columns; }
    int rows() const { return m_rows; }
    int frameWidth() const { return m_frameW; }
    int frameHeight() const { return m_frameH; }
    int frameCount() const { return m_frameCount; }
    int sheetCount() const { return m_sheets; }
    qint64 lastGenerationMs() const { return m_genMs; }

    // {source(url), x, y, index} of the tile nearest to `ms`; empty map when not ready.
    Q_INVOKABLE QVariantMap frameFor(qint64 ms) const;

signals:
    void fileChanged();
    void durationMsChanged();
    void enabledChanged();
    void startDelayMsChanged();
    void readyChanged();
    void generatingChanged();

private:
    friend class TrickplayQueue;
    QUrl sheetUrl(int sheet) const;
    QString cacheDir() const;            // <cache>/trick/<sha1>
    void reset();
    void schedule();
    bool loadMeta();
    void startProcess();                 // called by the queue when it is our turn
    void onFinished(int exitCode, bool crashed);
    void cancel();

    QString m_file;
    qint64 m_duration = 0;
    bool m_enabled = true;
    int m_startDelayMs = 1500;
    bool m_ready = false;
    int m_interval = 10000;
    int m_columns = 10, m_rows = 10;
    int m_frameW = 0, m_frameH = 0, m_frameCount = 0, m_sheets = 0;
    qint64 m_genMs = 0;
    bool m_hwaccel = true;
    bool m_queued = false;
    QProcess *m_proc = nullptr;
    QTimer *m_delay = nullptr;
    QElapsedTimer m_clock;
};
