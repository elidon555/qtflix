#include "playertrickplay.h"

#include <QCryptographicHash>
#include <QDir>
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
#endif

namespace {
constexpr int kMetaVersion = 1;
constexpr int kTileWidth = 240;
}

// App-wide FIFO so that at most one ffmpeg trickplay process runs at a time.
class TrickplayQueue
{
public:
    static TrickplayQueue &instance() { static TrickplayQueue q; return q; }
    void enqueue(Trickplay *t)
    {
        if (!m_waiting.contains(t) && m_running != t)
            m_waiting.append(t);
        pump();
    }
    void remove(Trickplay *t)
    {
        m_waiting.removeAll(t);
        if (m_running == t) {
            m_running = nullptr;
            QTimer::singleShot(0, [] { TrickplayQueue::instance().pump(); });
        }
    }
    void finished(Trickplay *t)
    {
        if (m_running == t)
            m_running = nullptr;
        pump();
    }
    void pump()
    {
        while (!m_running && !m_waiting.isEmpty()) {
            QPointer<Trickplay> next = m_waiting.takeFirst();
            if (!next)
                continue;
            m_running = next;
            next->startProcess();
        }
    }
private:
    QList<QPointer<Trickplay>> m_waiting;
    QPointer<Trickplay> m_running;
};

Trickplay::Trickplay(QObject *parent) : QObject(parent)
{
    m_delay = new QTimer(this);
    m_delay->setSingleShot(true);
    connect(m_delay, &QTimer::timeout, this, [this] {
        if (m_ready || m_proc || m_file.isEmpty() || !m_enabled)
            return;
        m_queued = true;
        TrickplayQueue::instance().enqueue(this);
    });
}

Trickplay::~Trickplay()
{
    cancel();
}

void Trickplay::setFile(const QString &f)
{
    QString p = f;
    if (p.startsWith(QLatin1String("file:")))
        p = QUrl(p).toLocalFile();
    if (p == m_file)
        return;
    cancel();
    m_file = p;
    reset();
    emit fileChanged();
    schedule();
}

void Trickplay::setDurationMs(qint64 ms)
{
    if (ms == m_duration)
        return;
    m_duration = ms;
    emit durationMsChanged();
    if (m_ready && m_duration > 0) {
        const int n = std::min<int>(m_sheets * m_columns * m_rows, int(qRound64(double(m_duration) / m_interval)) + 1);
        if (n != m_frameCount) {
            m_frameCount = n;
            emit readyChanged();
        }
    }
}

void Trickplay::setEnabled(bool e)
{
    if (e == m_enabled)
        return;
    m_enabled = e;
    emit enabledChanged();
    if (!e)
        cancel();
    else
        schedule();
}

void Trickplay::reset()
{
    const bool wasReady = m_ready;
    m_ready = false;
    m_frameW = m_frameH = m_frameCount = m_sheets = 0;
    m_genMs = 0;
    m_hwaccel = true;
    if (wasReady)
        emit readyChanged();
}

QString Trickplay::cacheDir() const
{
    const QByteArray h = QCryptographicHash::hash(m_file.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/trick/")
           + QString::fromLatin1(h);
}

QUrl Trickplay::sheetUrl(int sheet) const
{
    return QUrl::fromLocalFile(cacheDir() + QStringLiteral("/%1.jpg").arg(sheet + 1, 3, 10, QLatin1Char('0')));
}

void Trickplay::schedule()
{
    if (m_file.isEmpty() || !m_enabled || m_ready || m_proc)
        return;
    if (loadMeta())         // cache hit: a small JSON read + one JPEG header, no decoding
        return;
    m_delay->start(m_startDelayMs);
}

bool Trickplay::loadMeta()
{
    const QString dir = cacheDir();
    QFile f(dir + QStringLiteral("/meta.json"));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const QFileInfo src(m_file);
    if (o.value(QStringLiteral("version")).toInt() != kMetaVersion
        || o.value(QStringLiteral("size")).toInteger() != src.size()
        || o.value(QStringLiteral("mtime")).toInteger() != src.lastModified().toMSecsSinceEpoch()) {
        QDir(dir).removeRecursively();
        return false;
    }
    m_interval = o.value(QStringLiteral("intervalMs")).toInt(10000);
    m_columns = o.value(QStringLiteral("columns")).toInt(10);
    m_rows = o.value(QStringLiteral("rows")).toInt(10);
    m_frameW = o.value(QStringLiteral("frameWidth")).toInt();
    m_frameH = o.value(QStringLiteral("frameHeight")).toInt();
    m_sheets = o.value(QStringLiteral("sheets")).toInt();
    m_frameCount = m_sheets * m_columns * m_rows;
    if (m_duration > 0)
        m_frameCount = std::min<int>(m_frameCount, int(qRound64(double(m_duration) / m_interval)) + 1);
    if (m_sheets <= 0 || m_frameW <= 0 || m_frameH <= 0)
        return false;
    m_ready = true;
    emit readyChanged();
    return true;
}

void Trickplay::startProcess()
{
    m_queued = false;
    if (m_file.isEmpty() || !m_enabled || m_ready || !QFileInfo::exists(m_file)) {
        TrickplayQueue::instance().finished(this);
        return;
    }
    const QString part = cacheDir() + QStringLiteral(".part");
    QDir(part).removeRecursively();
    QDir().mkpath(part);

    // Long files: sample less often so a sheet set stays small.
    m_interval = (m_duration > 4 * 3600 * 1000LL) ? 15000 : 10000;
    QStringList args { QStringLiteral("-nostdin"), QStringLiteral("-v"), QStringLiteral("error") };
    if (m_hwaccel)
        args << QStringLiteral("-hwaccel") << QStringLiteral("auto");
    args << QStringLiteral("-skip_frame") << QStringLiteral("nokey")
         << QStringLiteral("-i") << m_file
         << QStringLiteral("-an") << QStringLiteral("-sn") << QStringLiteral("-dn")
         << QStringLiteral("-vf")
         << QStringLiteral("fps=1000/%1,scale=%2:-2,tile=%3x%4").arg(m_interval).arg(kTileWidth).arg(m_columns).arg(m_rows)
         << QStringLiteral("-q:v") << QStringLiteral("5")
         << QStringLiteral("-y") << part + QStringLiteral("/%03d.jpg");

    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    m_proc->setStandardOutputFile(QProcess::nullDevice());
#ifdef Q_OS_UNIX
    m_proc->setChildProcessModifier([] {
        if (::nice(19) == -1) { /* best effort */ }
#ifdef Q_OS_LINUX
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);   // never outlive the app (crash / kill)
#endif
    });
#endif
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        onFinished(code, st == QProcess::CrashExit);
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            onFinished(-1, true);
    });
    m_clock.start();
    m_proc->start(QStringLiteral("ffmpeg"), args);
    emit generatingChanged();
}

void Trickplay::onFinished(int exitCode, bool crashed)
{
    if (!m_proc)
        return;
    m_proc->deleteLater();
    m_proc = nullptr;
    const qint64 took = m_clock.elapsed();
    const QString dir = cacheDir();
    const QString part = dir + QStringLiteral(".part");
    const bool ok = !crashed && exitCode == 0 && QFileInfo::exists(part + QStringLiteral("/001.jpg"));
    if (!ok) {
        QDir(part).removeRecursively();
        if (m_hwaccel && exitCode != -1) {
            // hardware decoding unavailable / broken: retry once in software (keeps our queue slot)
            m_hwaccel = false;
            emit generatingChanged();
            startProcess();
            return;
        }
        qWarning("Trickplay: ffmpeg failed for %s (exit %d)", qPrintable(m_file), exitCode);
        emit generatingChanged();
        TrickplayQueue::instance().finished(this);
        return;
    }

    int sheets = 0;
    while (QFileInfo::exists(part + QStringLiteral("/%1.jpg").arg(sheets + 1, 3, 10, QLatin1Char('0'))))
        ++sheets;
    const QSize sz = QImageReader(part + QStringLiteral("/001.jpg")).size();
    const QFileInfo src(m_file);
    QJsonObject o {
        { QStringLiteral("version"), kMetaVersion },
        { QStringLiteral("file"), m_file },
        { QStringLiteral("size"), src.size() },
        { QStringLiteral("mtime"), src.lastModified().toMSecsSinceEpoch() },
        { QStringLiteral("intervalMs"), m_interval },
        { QStringLiteral("columns"), m_columns },
        { QStringLiteral("rows"), m_rows },
        { QStringLiteral("frameWidth"), sz.width() / m_columns },
        { QStringLiteral("frameHeight"), sz.height() / m_rows },
        { QStringLiteral("sheets"), sheets },
        { QStringLiteral("generationMs"), took },
        { QStringLiteral("hwaccel"), m_hwaccel },
    };
    QFile mf(part + QStringLiteral("/meta.json"));
    if (mf.open(QIODevice::WriteOnly))
        mf.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    mf.close();
    QDir(dir).removeRecursively();
    QDir().rename(part, dir);

    qInfo("Trickplay: %d sheet(s) for %s in %lld ms (%s)", sheets, qPrintable(src.fileName()), took,
          m_hwaccel ? "hwaccel auto" : "software");
    emit generatingChanged();
    TrickplayQueue::instance().finished(this);
    if (loadMeta()) {
        m_genMs = took;
        emit readyChanged();
    }
}

void Trickplay::cancel()
{
    m_delay->stop();
    if (m_queued) {
        m_queued = false;
        TrickplayQueue::instance().remove(this);
    }
    if (m_proc) {
        QProcess *p = m_proc;
        m_proc = nullptr;
        p->disconnect(this);
        p->kill();
        p->waitForFinished(1000);
        delete p;
        QDir(cacheDir() + QStringLiteral(".part")).removeRecursively();
        TrickplayQueue::instance().remove(this);
        emit generatingChanged();
    }
}

QVariantMap Trickplay::frameFor(qint64 ms) const
{
    if (!m_ready || m_frameCount <= 0)
        return {};
    const int perSheet = m_columns * m_rows;
    const int idx = std::clamp<int>(int(qRound64(double(std::max<qint64>(0, ms)) / m_interval)), 0, m_frameCount - 1);
    const int sheet = idx / perSheet, in = idx % perSheet;
    return {
        { QStringLiteral("source"), sheetUrl(sheet) },
        { QStringLiteral("x"), (in % m_columns) * m_frameW },
        { QStringLiteral("y"), (in / m_columns) * m_frameH },
        { QStringLiteral("index"), idx },
    };
}
