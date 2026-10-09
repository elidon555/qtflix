#include "playertrickplay.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QtMath>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif
#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

namespace {
constexpr int kMetaVersion = 2;    // 2: 5x5 sheets written progressively (1: 10x10, written at the end)
constexpr int kTileWidth = 240;
constexpr int kColumns = 5, kRows = 5;
// Hardware decoding first (software retry when it fails): with key frames only it was ~1.7x faster than
// software on 1080p H.264 / HEVC 10-bit test files and uses a third of the CPU time. One decoder thread:
// frame threading only adds latency when every decoded frame is a key frame (faster both ways).
constexpr bool kHwaccel = true;

QString sheetName(int index)
{
    return QStringLiteral("/%1.jpg").arg(index + 1, 3, 10, QLatin1Char('0'));
}
}

void setBackgroundPriority(QProcess *proc)
{
#ifdef Q_OS_UNIX
    proc->setChildProcessModifier([] {
        if (::nice(19) == -1) { /* best effort */ }
#ifdef Q_OS_LINUX
        // idle I/O class: the disk only serves us when nobody else wants it (no glibc wrapper for ioprio_set)
        constexpr int kWhoProcess = 1, kClassIdle = 3, kClassShift = 13;
        ::syscall(SYS_ioprio_set, kWhoProcess, 0, kClassIdle << kClassShift);
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);   // never outlive the app (crash / kill)
#endif
    });
#else
    Q_UNUSED(proc);
#endif
}

// One ffmpeg pass for one file. Owned by the queue; it outlives the Trickplay objects that follow it once
// it has started.
struct TrickplayJob {
    QString file, dir;
    int interval = 10000;
    bool hwaccel = kHwaccel;
    QProcess *proc = nullptr;
    QElapsedTimer clock;
    int sheets = 0;                       // complete sheets on disk
    QList<QPointer<Trickplay>> owners;
};

// App-wide FIFO so that at most one ffmpeg trickplay process runs at a time.
class TrickplayQueue
{
public:
    static TrickplayQueue &instance() { static TrickplayQueue q; return q; }

    TrickplayJob *find(const QString &file) const
    {
        if (m_running && m_running->file == file)
            return m_running;
        for (TrickplayJob *j : m_waiting)
            if (j->file == file)
                return j;
        return nullptr;
    }
    TrickplayJob *enqueue(Trickplay *t, const QString &file, const QString &dir, qint64 durationMs)
    {
        if (TrickplayJob *j = find(file)) {
            j->owners.append(t);
            return j;
        }
        auto *j = new TrickplayJob;
        j->file = file;
        j->dir = dir;
        j->interval = durationMs > 4 * 3600 * 1000LL ? 15000 : 10000; // long files: fewer sheets
        j->owners.append(t);
        m_waiting.append(j);
        pump();
        return j;
    }
    // `t` no longer follows `j`: a waiting job nobody follows is dropped, a running one carries on.
    void abandon(TrickplayJob *j, Trickplay *t)
    {
        j->owners.removeAll(t);
        j->owners.removeAll(nullptr);
        if (j != m_running && j->owners.isEmpty() && m_waiting.removeAll(j))
            delete j;
    }

private:
    TrickplayQueue()
    {
        if (QCoreApplication *app = QCoreApplication::instance())
            QObject::connect(app, &QCoreApplication::aboutToQuit, app, [this] { killRunning(); });
    }
    void killRunning()
    {
        if (!m_running || !m_running->proc)
            return;
        m_running->proc->disconnect();
        m_running->proc->kill(); // no wait; the half-written set has no meta.json and is redone next time
        m_running = nullptr;
    }
    void pump()
    {
        if (!m_running && !m_waiting.isEmpty())
            start(m_waiting.takeFirst());
    }
    void start(TrickplayJob *j)
    {
        m_running = j;
        if (!QFileInfo::exists(j->file)) {
            finish(j, false);
            return;
        }
        QDir(j->dir).removeRecursively();
        QDir().mkpath(j->dir);
        j->sheets = 0;
        QStringList args { QStringLiteral("-nostdin"), QStringLiteral("-v"), QStringLiteral("error"),
                           QStringLiteral("-progress"), QStringLiteral("pipe:1") };  // wakes us up as it goes
        if (j->hwaccel)
            args << QStringLiteral("-hwaccel") << QStringLiteral("auto");
        args << QStringLiteral("-threads") << QStringLiteral("1")
             << QStringLiteral("-skip_frame") << QStringLiteral("nokey")
             << QStringLiteral("-i") << j->file
             << QStringLiteral("-an") << QStringLiteral("-sn") << QStringLiteral("-dn")
             << QStringLiteral("-vf")
             << QStringLiteral("fps=1000/%1,scale=%2:-2,tile=%3x%4").arg(j->interval).arg(kTileWidth).arg(kColumns).arg(kRows)
             << QStringLiteral("-q:v") << QStringLiteral("5")
             << QStringLiteral("-atomic_writing") << QStringLiteral("1")  // a sheet appears complete or not at all
             << QStringLiteral("-y") << j->dir + QStringLiteral("/%03d.jpg");
        j->proc = new QProcess;
        j->proc->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        setBackgroundPriority(j->proc);
        QObject::connect(j->proc, &QProcess::readyReadStandardOutput, j->proc, [this, j] {
            j->proc->readAllStandardOutput();
            scan(j);
        });
        QObject::connect(j->proc, &QProcess::finished, j->proc, [this, j](int code, QProcess::ExitStatus st) {
            onExit(j, code, st == QProcess::CrashExit);
        });
        QObject::connect(j->proc, &QProcess::errorOccurred, j->proc, [this, j](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart)
                onExit(j, -1, true);
        });
        j->clock.start();
        j->proc->start(QStringLiteral("ffmpeg"), args);
    }
    // Picks up the sheets ffmpeg has written since the last look.
    void scan(TrickplayJob *j)
    {
        const int before = j->sheets;
        while (QFileInfo::exists(j->dir + sheetName(j->sheets)))
            ++j->sheets;
        if (j->sheets != before)
            for (const QPointer<Trickplay> &t : std::as_const(j->owners))
                if (t)
                    t->jobProgress(j->sheets);
    }
    void onExit(TrickplayJob *j, int exitCode, bool crashed)
    {
        if (!j->proc)
            return;
        j->proc->disconnect();
        j->proc->deleteLater();
        j->proc = nullptr;
        const bool ok = !crashed && exitCode == 0 && QFileInfo::exists(j->dir + sheetName(0));
        if (!ok && j->hwaccel && exitCode != -1) {
            // hardware decoding unavailable / broken: retry once in software (keeps our queue slot)
            j->hwaccel = false;
            for (const QPointer<Trickplay> &t : std::as_const(j->owners))
                if (t)
                    t->jobProgress(0);
            start(j);
            return;
        }
        if (!ok)
            qWarning("Trickplay: ffmpeg failed for %s (exit %d)", qPrintable(j->file), exitCode);
        scan(j);
        finish(j, ok);
    }
    void finish(TrickplayJob *j, bool ok)
    {
        const qint64 took = j->clock.isValid() ? j->clock.elapsed() : 0;
        if (ok) {
            const QSize sz = QImageReader(j->dir + sheetName(0)).size();
            const QFileInfo src(j->file);
            QJsonObject o {
                { QStringLiteral("version"), kMetaVersion },
                { QStringLiteral("file"), j->file },
                { QStringLiteral("size"), src.size() },
                { QStringLiteral("mtime"), src.lastModified().toMSecsSinceEpoch() },
                { QStringLiteral("intervalMs"), j->interval },
                { QStringLiteral("columns"), kColumns },
                { QStringLiteral("rows"), kRows },
                { QStringLiteral("frameWidth"), sz.width() / kColumns },
                { QStringLiteral("frameHeight"), sz.height() / kRows },
                { QStringLiteral("sheets"), j->sheets },
                { QStringLiteral("generationMs"), took },
                { QStringLiteral("hwaccel"), j->hwaccel },
            };
            QFile mf(j->dir + QStringLiteral("/meta.json.tmp"));
            if (mf.open(QIODevice::WriteOnly))
                mf.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
            mf.close();
            QFile::rename(mf.fileName(), j->dir + QStringLiteral("/meta.json"));
            qInfo("Trickplay: %d sheet(s) for %s in %lld ms (%s)", j->sheets, qPrintable(src.fileName()), took,
                  j->hwaccel ? "hwaccel auto" : "software");
        }
        m_running = nullptr;
        const QList<QPointer<Trickplay>> owners = j->owners;
        delete j;
        for (const QPointer<Trickplay> &t : owners)
            if (t)
                t->jobFinished(ok, took);
        pump();
    }

    QList<TrickplayJob *> m_waiting;
    TrickplayJob *m_running = nullptr;
};

Trickplay::Trickplay(QObject *parent) : QObject(parent)
{
    m_delay = new QTimer(this);
    m_delay->setSingleShot(true);
    connect(m_delay, &QTimer::timeout, this, [this] {
        if (m_complete || m_job || m_file.isEmpty() || !m_enabled)
            return;
        m_job = TrickplayQueue::instance().enqueue(this, m_file, m_dir, m_duration);
        emit generatingChanged();
        if (m_job->sheets > 0)
            jobProgress(m_job->sheets);
    });
}

Trickplay::~Trickplay()
{
    detach();
}

void Trickplay::setFile(const QString &f)
{
    QString p = f;
    if (p.startsWith(QLatin1String("file:")))
        p = QUrl(p).toLocalFile();
    if (p == m_file)
        return;
    const bool wasGenerating = m_job;
    detach();
    m_file = p;
    m_dir = p.isEmpty() ? QString()
                        : QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/trick/")
                              + QString::fromLatin1(QCryptographicHash::hash(p.toUtf8(), QCryptographicHash::Sha1).toHex());
    reset();
    emit fileChanged();
    if (wasGenerating)
        emit generatingChanged();
    schedule();
}

void Trickplay::setDurationMs(qint64 ms)
{
    if (ms == m_duration)
        return;
    m_duration = ms;
    emit durationMsChanged();
    if (ready()) {
        const int before = m_frameCount;
        updateFrameCount();
        if (m_frameCount != before)
            emit readyChanged();
    }
}

void Trickplay::setEnabled(bool e)
{
    if (e == m_enabled)
        return;
    m_enabled = e;
    emit enabledChanged();
    if (!e) {
        const bool wasGenerating = m_job;
        detach(); // a pass that has started carries on in the background
        if (wasGenerating)
            emit generatingChanged();
    } else {
        schedule();
    }
}

void Trickplay::detach()
{
    m_delay->stop();
    if (m_job) {
        TrickplayQueue::instance().abandon(m_job, this);
        m_job = nullptr;
    }
}

void Trickplay::reset()
{
    const bool wasReady = ready();
    m_complete = false;
    m_urls.clear();
    m_frameW = m_frameH = m_frameCount = 0;
    m_columns = kColumns;
    m_rows = kRows;
    m_genMs = 0;
    if (wasReady)
        emit readyChanged();
}

void Trickplay::updateFrameCount()
{
    int n = int(m_urls.size()) * m_columns * m_rows;
    if (m_duration > 0)
        n = std::min<int>(n, int(qRound64(double(m_duration) / m_interval)) + 1);
    m_frameCount = n;
}

void Trickplay::schedule()
{
    if (m_file.isEmpty() || !m_enabled || m_complete || m_job)
        return;
    if (TrickplayJob *running = TrickplayQueue::instance().find(m_file)) {
        // still being generated (e.g. the player was closed and reopened): follow it right away
        m_job = TrickplayQueue::instance().enqueue(this, m_file, m_dir, m_duration);
        Q_ASSERT(m_job == running);
        emit generatingChanged();
        if (m_job->sheets > 0)
            jobProgress(m_job->sheets);
        return;
    }
    if (loadMeta())         // cache hit: a small JSON read, no image decoding
        return;
    m_delay->start(m_startDelayMs);
}

bool Trickplay::loadMeta()
{
    QFile f(m_dir + QStringLiteral("/meta.json"));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const QFileInfo src(m_file);
    if (o.value(QStringLiteral("version")).toInt() != kMetaVersion
        || o.value(QStringLiteral("size")).toInteger() != src.size()
        || o.value(QStringLiteral("mtime")).toInteger() != src.lastModified().toMSecsSinceEpoch()) {
        QDir(m_dir).removeRecursively(); // other layout or stale: regenerate
        return false;
    }
    const int sheets = o.value(QStringLiteral("sheets")).toInt();
    m_interval = o.value(QStringLiteral("intervalMs")).toInt(10000);
    m_columns = o.value(QStringLiteral("columns")).toInt(kColumns);
    m_rows = o.value(QStringLiteral("rows")).toInt(kRows);
    m_frameW = o.value(QStringLiteral("frameWidth")).toInt();
    m_frameH = o.value(QStringLiteral("frameHeight")).toInt();
    if (sheets <= 0 || m_frameW <= 0 || m_frameH <= 0 || m_columns <= 0 || m_rows <= 0 || m_interval <= 0) {
        m_frameW = m_frameH = 0;
        return false;
    }
    m_urls.clear();
    for (int i = 0; i < sheets; ++i)
        m_urls.append(QUrl::fromLocalFile(m_dir + sheetName(i)));
    m_complete = true;
    updateFrameCount();
    emit readyChanged();
    return true;
}

void Trickplay::jobProgress(int sheets)
{
    if (!m_job)
        return;
    if (sheets < m_urls.size()) { // the pass restarted (software retry)
        reset();
        return;
    }
    if (sheets == m_urls.size())
        return;
    if (m_frameW <= 0) {
        const QSize sz = QImageReader(m_dir + sheetName(0)).size(); // header only
        if (!sz.isValid())
            return;
        m_interval = m_job->interval;
        m_columns = kColumns;
        m_rows = kRows;
        m_frameW = sz.width() / m_columns;
        m_frameH = sz.height() / m_rows;
    }
    while (m_urls.size() < sheets)
        m_urls.append(QUrl::fromLocalFile(m_dir + sheetName(int(m_urls.size()))));
    updateFrameCount();
    emit readyChanged();
}

void Trickplay::jobFinished(bool ok, qint64 tookMs)
{
    m_job = nullptr;
    emit generatingChanged();
    if (!ok)
        return;  // keep whatever sheets were shown; no meta.json, so the next session starts over
    m_complete = false;
    if (loadMeta())
        m_genMs = tookMs;
    emit readyChanged();
}

QVariantMap Trickplay::frameFor(qint64 ms) const
{
    if (!ready() || m_frameCount <= 0)
        return {};
    int idx = int(qRound64(double(std::max<qint64>(0, ms)) / m_interval));
    if (idx >= m_frameCount) {
        if (!m_complete)
            return {};   // not generated yet
        idx = m_frameCount - 1;
    }
    const int perSheet = m_columns * m_rows;
    const int sheet = idx / perSheet, in = idx % perSheet;
    return {
        { QStringLiteral("source"), m_urls.at(sheet) },
        { QStringLiteral("sheet"), sheet },
        { QStringLiteral("x"), (in % m_columns) * m_frameW },
        { QStringLiteral("y"), (in / m_columns) * m_frameH },
        { QStringLiteral("index"), idx },
    };
}
