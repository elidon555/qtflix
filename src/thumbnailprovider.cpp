#include "thumbnailprovider.h"
#include "library.h"

#include <QAtomicInt>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QImageReader>
#include <QLinearGradient>
#include <QMutex>
#include <QPainter>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <deque>
#include <functional>

#ifdef __GLIBC__
#include <malloc.h>
#endif
#ifdef Q_OS_LINUX
#include <sys/syscall.h>
#include <unistd.h>
#endif

// The provider runs requestImageResponse() on QML's image reader thread and the actual work on two pools.
// Library data is only read through Library::thumbInfo(), which copies the needed fields under
// Library::m_mutex.
//  - m_pool (decode pool, ~one thread per core): every live QML request starts here and is answered from
//    artwork files, the disk cache or a remembered failure, without ever waiting for ffmpeg;
//  - m_grabPool (3 threads, so at most 3 ffmpeg processes run at once): frame grabs. A live request that
//    misses the cache is handed over (priority 10). Requests for a frame that is already being grabbed
//    don't block a thread: they are attached to the grab in flight and answered when it completes.
//    Warm-up jobs queued by the Library after a scan run here too: started one by one, only while no
//    live request is pending, and at most 2 at a time, so a live grab always finds a free thread.
// Only ffmpeg frame grabs are cached on disk (CacheLocation/thumbs/<id>-<kind>.jpg); artwork files
// (posterFile/backdropFile/episode stills) are decoded directly at the requested size.

class ThumbResponse : public QQuickImageResponse
{
public:
    QQuickTextureFactory *textureFactory() const override
    {
        // hand the pixels over: the provider keeps no reference once QML has the texture factory
        QImage img;
        img.swap(m_image);
        return QQuickTextureFactory::textureFactoryForImage(img);
    }
    void cancel() override { m_cancelled.storeRelaxed(1); }

    mutable QImage m_image;
    QAtomicInt m_cancelled{0};
};

struct Request {
    QString titleId;
    QString kind = QStringLiteral("card"); // card | backdrop | ep
    int season = -1, episode = -1;
};

// a live request waiting for its image
struct Pending {
    ThumbResponse *resp = nullptr;
    Request req;
    QSize requested;
    QElapsedTimer timer;
};

using InfoFn = std::function<QVariantMap(const QString &, int, int)>;

struct ThumbShared {
    QMutex mutex;
    std::deque<QString> warmQueue;
    int live = 0;        // live requests queued, running or waiting for a grab
    int warmActive = 0;  // warm-up jobs running
    int warmTotal = 0, warmDone = 0;
    QElapsedTimer warmTimer;
    // frame grabs in flight by lock key (card and backdrop share "<id>-frame"), with the live requests
    // waiting for them
    QHash<QString, QList<Pending>> inFlight;

    InfoFn info;
    QThreadPool *decodePool = nullptr;
    QThreadPool *grabPool = nullptr;
};
struct ThumbnailProvider::Shared : ThumbShared {};

namespace {

QAtomicInt g_shuttingDown(0);

bool timing()
{
    static const bool on = qEnvironmentVariableIntValue("QTFLIX_TIMING") > 0;
    return on;
}

enum class GrabStatus { Ok, NoFrame, Error, Timeout };

QMutex g_failedMutex;
QHash<QString, GrabStatus> g_failedFrames; // "path@sec.ss" that ffmpeg could not decode in this session

QString thumbsDir()
{
    static const QString dir = [] {
        const QString d = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/thumbs");
        QDir().mkpath(d);
        return d;
    }();
    return dir;
}

// Markers of files ffmpeg could not get a frame from, so broken files aren't retried on every launch.
// Named by a hash of path + size + mtime + grab position (a changed file gets a new chance), the content
// is the video path (for pruning). They expire after 30 days (e.g. a newer ffmpeg may handle the file).
QString failedDir()
{
    static const QString dir = [] {
        const QString d = thumbsDir() + QStringLiteral("/failed");
        QDir().mkpath(d);
        return d;
    }();
    return dir;
}

constexpr qint64 kFailedMarkerSecs = 30 * 24 * 3600;

QString failMarker(const QString &file, double fraction)
{
    const QFileInfo fi(file);
    if (file.isEmpty() || !fi.exists())
        return {};
    const QByteArray key = file.toUtf8() + '\n' + QByteArray::number(fi.size()) + '\n'
                           + QByteArray::number(fi.lastModified().toMSecsSinceEpoch()) + '\n'
                           + QByteArray::number(fraction, 'f', 2);
    return failedDir() + QLatin1Char('/')
           + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(24))
           + QStringLiteral(".fail");
}

bool grabFailedBefore(const QString &file, double fraction)
{
    const QString m = failMarker(file, fraction);
    if (m.isEmpty())
        return false;
    const QFileInfo mi(m);
    return mi.exists() && mi.lastModified().secsTo(QDateTime::currentDateTime()) < kFailedMarkerSecs;
}

void rememberGrabFailed(const QString &file, double fraction)
{
    const QString m = failMarker(file, fraction);
    QFile f(m);
    if (!m.isEmpty() && f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(file.toUtf8());
}

// the position of a kind's frame (card and backdrop use the same frame)
double grabFraction(const QString &kind)
{
    return kind == QLatin1String("ep") ? 0.35 : 0.20;
}

QSize defaultSize(const QString &kind)
{
    if (kind == QLatin1String("card")) return {300, 450};
    if (kind == QLatin1String("ep")) return {400, 225};
    return {1280, 720};
}

// size stored in the disk cache (requests are scaled down from this, never up)
QSize cacheSize(const QString &kind)
{
    if (kind == QLatin1String("card")) return {400, 600};
    if (kind == QLatin1String("ep")) return {640, 360};
    return {1280, 720};
}

// shrink `want` (keeping its aspect) so it fits into `avail`; never enlarges
QSize fitNoUpscale(const QSize &want, const QSize &avail)
{
    if (!want.isValid() || !avail.isValid() || (want.width() <= avail.width() && want.height() <= avail.height()))
        return want;
    return want.scaled(avail, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

// scale to cover `size` then center-crop
QImage coverCrop(const QImage &src, const QSize &size)
{
    if (src.isNull() || size.isEmpty())
        return src;
    if (src.size() == size)
        return src;
    const QImage scaled = src.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int x = (scaled.width() - size.width()) / 2;
    const int y = (scaled.height() - size.height()) / 2;
    if (x == 0 && y == 0 && scaled.size() == size)
        return scaled;
    return scaled.copy(x, y, size.width(), size.height());
}

// cover-crop to `out` without ever upscaling (the result keeps out's aspect, may be smaller)
QImage coverCropNoUpscale(const QImage &src, const QSize &out)
{
    if (src.isNull() || out.isEmpty())
        return src;
    QSize crop = out.scaled(src.size(), Qt::KeepAspectRatio); // largest out-shaped area inside src
    return coverCrop(src, fitNoUpscale(out, crop));
}

// mean luma (0..255) sampled on a coarse grid
int meanLuma(const QImage &img)
{
    if (img.isNull())
        return 0;
    const int w = img.width(), h = img.height();
    qint64 sum = 0;
    int n = 0;
    for (int gy = 0; gy < 24; ++gy)
        for (int gx = 0; gx < 32; ++gx) {
            const QRgb c = img.pixel(std::min(w - 1, gx * w / 32 + w / 64), std::min(h - 1, gy * h / 24 + h / 48));
            sum += (qRed(c) * 3 + qGreen(c) * 6 + qBlue(c)) / 10;
            ++n;
        }
    return n ? int(sum / n) : 0;
}

// Remove letterbox / pillarbox bars (near-black rows/columns at the edges), at most 30% per side.
QImage trimBlackBorders(const QImage &in)
{
    if (in.isNull() || in.width() < 16 || in.height() < 16)
        return in;
    const QImage img = in.format() == QImage::Format_RGB32 ? in : in.convertToFormat(QImage::Format_RGB32);
    const int w = img.width(), h = img.height();
    auto dark = [&](int x0, int y0, int dx, int dy, int n) {
        int sum = 0, mx = 0;
        const int step = std::max(1, n / 64);
        int cnt = 0;
        for (int i = 0; i < n; i += step, ++cnt) {
            const QRgb c = img.pixel(x0 + dx * i, y0 + dy * i);
            const int l = (qRed(c) * 3 + qGreen(c) * 6 + qBlue(c)) / 10;
            sum += l;
            mx = std::max(mx, l);
        }
        return cnt > 0 && sum / cnt < 18 && mx < 48;
    };
    int top = 0, bottom = h - 1, left = 0, right = w - 1;
    while (top < h * 3 / 10 && dark(0, top, 1, 0, w)) ++top;
    while (bottom > h * 7 / 10 && dark(0, bottom, 1, 0, w)) --bottom;
    while (left < w * 3 / 10 && dark(left, top, 0, 1, bottom - top + 1)) ++left;
    while (right > w * 7 / 10 && dark(right, top, 0, 1, bottom - top + 1)) --right;
    if (top == 0 && left == 0 && bottom == h - 1 && right == w - 1)
        return img;
    return img.copy(left, top, right - left + 1, bottom - top + 1);
}

QImage placeholder(const QString &text, const QSize &size)
{
    QImage img(size, QImage::Format_RGB32);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    QLinearGradient g(0, 0, 0, size.height());
    g.setColorAt(0.0, QColor(0x23, 0x23, 0x23));
    g.setColorAt(1.0, QColor(0x14, 0x14, 0x14));
    p.fillRect(img.rect(), g);
    // thin red accent like the Netflix "N" ribbon
    p.fillRect(QRect(0, 0, std::max(3, size.width() / 60), size.height()), QColor(0xE5, 0x09, 0x14));

    if (!text.isEmpty()) {
        QFont f(QStringLiteral("Inter"));
        f.setBold(true);
        const int base = std::min(size.width(), size.height());
        f.setPixelSize(std::max(10, size.width() < size.height() ? base / 8 : base / 10));
        p.setFont(f);
        p.setPen(Qt::white);
        const QRect r = img.rect().adjusted(size.width() / 10, size.height() / 10, -size.width() / 10,
                                            -size.height() / 10);
        // shrink until it fits
        for (int i = 0; i < 6; ++i) {
            const QRect need = p.fontMetrics().boundingRect(r, Qt::AlignCenter | Qt::TextWordWrap, text);
            if (need.width() <= r.width() && need.height() <= r.height())
                break;
            f.setPixelSize(std::max(8, int(f.pixelSize() * 0.85)));
            p.setFont(f);
        }
        p.drawText(r, Qt::AlignCenter | Qt::TextWordWrap, text);
    }
    p.end();
    return img;
}

QImage loadImage(const QString &path)
{
    QImageReader r(path);
    r.setAutoTransform(true);
    return r.read();
}

// Decode `path` cover-cropped to `out` (never upscaled). JPEGs are clipped and scaled while decoding,
// so a 1280x720 cache entry requested at 300x169 never materialises at full size.
QImage loadCover(const QString &path, const QSize &out)
{
    QImageReader r(path);
    const QSize src = r.size();
    if (!src.isValid() || out.isEmpty() || r.transformation() != QImageIOHandler::TransformationNone)
        return coverCropNoUpscale(loadImage(path), out);
    const QSize cropSize = out.scaled(src, Qt::KeepAspectRatio);
    const QRect crop(QPoint((src.width() - cropSize.width()) / 2, (src.height() - cropSize.height()) / 2), cropSize);
    const QSize dst = fitNoUpscale(out, cropSize);
    if (crop.size() != src)
        r.setClipRect(crop);
    if (dst != crop.size())
        r.setScaledSize(dst);
    QImage img = r.read();
    if (img.isNull())
        return {};
    if (img.size() != dst) // handler ignored an option
        img = coverCropNoUpscale(img, dst);
    return img;
}

// ------------------------------------------------------------------------------------- ffmpeg grabs

// VA-API decision for the session: 0 = untested, 1 = works, -1 = use software
QAtomicInt g_hwMode(0);

QString vaapiDevice()
{
    static const QString dev = [] {
        if (qEnvironmentVariableIsSet("QTFLIX_HWACCEL") && qEnvironmentVariableIntValue("QTFLIX_HWACCEL") == 0)
            return QString();
        const QString d = QStringLiteral("/dev/dri/renderD128");
        return QFileInfo(d).isReadable() ? d : QString();
    }();
    return dev;
}

// One ffmpeg run. NoFrame = ffmpeg ran fine but nothing decodable at that position (e.g. a webm without
// cues seeked past its only keyframe); Error = ffmpeg failed (worth retrying without VA-API); Timeout =
// ffmpeg did not start or finish in time / shutdown (says nothing about the file).
QImage runFfmpeg(const QString &exe, const QString &file, double sec, bool hw, GrabStatus *status)
{
    *status = GrabStatus::Timeout;
    QStringList args = {QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-v"),
                        QStringLiteral("error")};
    if (hw)
        args << QStringLiteral("-hwaccel") << QStringLiteral("vaapi") << QStringLiteral("-hwaccel_device")
             << vaapiDevice();
    // keyframe-only decoding + fast (non-accurate) input seek: we get the keyframe at/before `sec`;
    // frames wider than 1280 are scaled down, smaller ones are kept as they are (never upscaled)
    args << QStringLiteral("-skip_frame") << QStringLiteral("nokey") << QStringLiteral("-threads")
         << QStringLiteral("2") << QStringLiteral("-noaccurate_seek") << QStringLiteral("-ss")
         << QString::number(sec, 'f', 2) << QStringLiteral("-i") << file << QStringLiteral("-an")
         << QStringLiteral("-sn") << QStringLiteral("-dn") << QStringLiteral("-frames:v") << QStringLiteral("1")
         << QStringLiteral("-vf") << QStringLiteral("scale='min(1280,iw)':-2") << QStringLiteral("-f")
         << QStringLiteral("image2pipe") << QStringLiteral("-c:v") << QStringLiteral("ppm") << QStringLiteral("pipe:1");
    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_LINUX
    // Background work: idle I/O class (only gets the disk when nobody else wants it, e.g. a playing video)
    // and a lower CPU priority. Runs in the child between fork and exec: plain syscalls only.
    proc.setChildProcessModifier([] {
        constexpr int ioprioWhoProcess = 1, ioprioClassIdle = 3, ioprioClassShift = 13;
        ::syscall(SYS_ioprio_set, ioprioWhoProcess, 0, ioprioClassIdle << ioprioClassShift);
        if (::nice(10) == -1) {
        }
    });
#endif
    proc.start(exe, args);
    if (!proc.waitForStarted(5000))
        return {};
    QElapsedTimer timer;
    timer.start();
    bool done = false;
    while (!(done = proc.waitForFinished(200))) {
        if (proc.state() == QProcess::NotRunning) { done = true; break; }
        if (timer.elapsed() > 10000 || g_shuttingDown.loadRelaxed())
            break;
    }
    if (!done) {
        proc.kill();
        proc.waitForFinished(1000);
        return {};
    }
    *status = GrabStatus::Error;
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return {};
    const QByteArray data = proc.readAllStandardOutput();
    if (data.isEmpty()) {
        *status = GrabStatus::NoFrame;
        return {};
    }
    QImage img;
    img.loadFromData(data, "PPM");
    if (!img.isNull() && img.format() != QImage::Format_RGB32)
        img = img.convertToFormat(QImage::Format_RGB32);
    if (!img.isNull())
        *status = GrabStatus::Ok;
    return img;
}

// VA-API pays ~30-40 ms of device setup per ffmpeg run; that only wins for heavy streams. Measured on
// this library (keyframe grabs): HEVC 1080p 95 ms VA-API vs 115 ms software, small H.264/VP8 screen
// recordings 99 ms VA-API vs 73 ms software.
bool preferHw(const QString &codec, int height)
{
    static const QStringList heavy = {QStringLiteral("hevc"), QStringLiteral("h265"), QStringLiteral("av1"),
                                      QStringLiteral("vp9")};
    return heavy.contains(codec.toLower()) || height >= 1440;
}

// Grab one frame at `sec` seconds. Heavy streams try VA-API first (if it fails outright once while software
// works, it is disabled for the rest of the session); everything else decodes in software.
QImage grabFrame(const QString &file, double sec, bool hwFirst, GrabStatus *status)
{
    static const QString exe = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    *status = GrabStatus::Timeout; // no ffmpeg / no file: nothing learned about the file
    if (exe.isEmpty() || file.isEmpty() || !QFileInfo::exists(file))
        return {};
    const QString key = file + QLatin1Char('@') + QString::number(sec, 'f', 2);
    {
        QMutexLocker l(&g_failedMutex);
        const auto it = g_failedFrames.constFind(key);
        if (it != g_failedFrames.cend()) {
            *status = it.value();
            return {};
        }
    }
    QElapsedTimer t;
    t.start();
    QImage img;
    GrabStatus st = GrabStatus::Error;
    bool usedHw = false;
    const int mode = g_hwMode.loadRelaxed();
    const bool tryHw = hwFirst && mode >= 0 && !vaapiDevice().isEmpty();
    if (tryHw) {
        img = runFfmpeg(exe, file, sec, true, &st);
        usedHw = st == GrabStatus::Ok;
        if (mode == 0 && st == GrabStatus::Ok)
            g_hwMode.storeRelaxed(1);
    }
    // software: VA-API disabled/unavailable, or the VA-API run failed outright (not just "no frame here")
    if ((st == GrabStatus::Error || st == GrabStatus::Timeout) && !g_shuttingDown.loadRelaxed()) {
        img = runFfmpeg(exe, file, sec, false, &st);
        if (tryHw && mode == 0 && st == GrabStatus::Ok) { // only blame VA-API if software works
            g_hwMode.storeRelaxed(-1);
            if (timing())
                qDebug("[thumbs] VA-API frame grab failed, using software decoding for this session");
        }
    }
    if (timing())
        qDebug("[thumbs] grab %s %.1fs: %lld ms (%s)%s", qPrintable(QFileInfo(file).fileName()), sec,
               qint64(t.elapsed()), usedHw ? "vaapi" : "sw", img.isNull() ? " FAILED" : "");
    if (img.isNull() && !g_shuttingDown.loadRelaxed()) {
        QMutexLocker l(&g_failedMutex);
        g_failedFrames.insert(key, st);
    }
    *status = img.isNull() ? st : GrabStatus::Ok;
    return img;
}

// *definite is set when a null result says something about the file (ffmpeg ran and found nothing
// usable), as opposed to timeouts / a missing ffmpeg / shutdown.
QImage grabAt(const QString &file, qint64 durationMs, double fraction, bool hw, bool *definite)
{
    bool transient = false;
    auto grab = [&](double at) {
        GrabStatus st;
        QImage img = grabFrame(file, at, hw, &st);
        transient |= st == GrabStatus::Timeout;
        return img;
    };
    double sec = 60.0;
    if (durationMs > 0)
        sec = durationMs < 120000 ? durationMs * 0.10 / 1000.0 : durationMs * fraction / 1000.0;
    QImage img = grab(sec);
    // A fade / black scene would make a useless thumbnail (and fool the black-bar trimming): retry later once.
    // (A single ffmpeg run producing both candidates would decode two frames for every grab; this second
    // process only runs for the rare dark frame.)
    if (!img.isNull() && durationMs > 0 && !g_shuttingDown.loadRelaxed() && meanLuma(img) < 16) {
        const double later = sec + durationMs * 0.10 / 1000.0;
        if (later < durationMs / 1000.0 - 1.0) {
            const QImage again = grab(later);
            if (!again.isNull() && meanLuma(again) > meanLuma(img))
                img = again;
        }
    }
    // nothing at that position (e.g. a short clip whose only keyframe is at 0): try near the start
    if (img.isNull() && sec > 0.0 && !g_shuttingDown.loadRelaxed())
        img = grab(durationMs > 0 ? 0.0 : 3.0);
    if (img.isNull() && !g_shuttingDown.loadRelaxed() && durationMs <= 0)
        img = grab(0.0);
    *definite = img.isNull() && !transient && !g_shuttingDown.loadRelaxed();
    return trimBlackBorders(img);
}

void saveCache(const QImage &img, const QString &path)
{
    if (img.isNull())
        return;
    // per-thread temp name: a card may be derived from the backdrop cache by two threads at once
    const QString tmp = path + QStringLiteral(".%1.part").arg(quintptr(QThread::currentThreadId()), 0, 16);
    if (img.save(tmp, "JPG", 88)) {
        QFile::remove(path);
        QFile::rename(tmp, path);
    } else {
        QFile::remove(tmp);
    }
}

Request parseId(const QString &rawId)
{
    QString id = rawId;
    const int q = int(id.indexOf(QLatin1Char('?')));
    if (q >= 0)
        id.truncate(q);
    const QStringList parts = id.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    Request r;
    if (parts.isEmpty())
        return r;
    r.titleId = parts.at(0);
    if (parts.size() >= 2)
        r.kind = parts.at(1);
    if (r.kind == QLatin1String("ep")) {
        r.season = parts.value(2).toInt();
        r.episode = parts.value(3).toInt();
    } else if (r.kind != QLatin1String("backdrop")) {
        r.kind = QStringLiteral("card");
    }
    return r;
}

QSize targetSize(const QSize &requested, const QString &kind)
{
    const QSize def = defaultSize(kind);
    const double aspect = double(def.width()) / def.height();
    // negative sizes come from delegates that are not laid out yet (sourceSize bound to width * k); they
    // re-request once laid out, so don't decode a full-size image for them
    if (requested.width() < 0 || requested.height() < 0)
        return def / 4;
    if (requested.width() > 0 && requested.height() > 0)
        return requested;
    if (requested.width() > 0)
        return {requested.width(), std::max(1, int(requested.width() / aspect + 0.5))};
    if (requested.height() > 0)
        return {std::max(1, int(requested.height() * aspect + 0.5)), requested.height()};
    return def;
}

bool cacheValid(const QString &file, const QDateTime &mtime)
{
    const QFileInfo fi(file);
    return fi.exists() && fi.size() > 0 && (!mtime.isValid() || fi.lastModified() >= mtime);
}

QString frameCacheName(const QString &titleId, const QString &kind, int season, int episode)
{
    QString name = titleId + QLatin1Char('-') + kind;
    if (kind == QLatin1String("ep"))
        name += QStringLiteral("-%1-%2").arg(season).arg(episode);
    return name + QStringLiteral(".jpg");
}

QString frameCacheFile(const Request &r)
{
    return thumbsDir() + QLatin1Char('/') + frameCacheName(r.titleId, r.kind, r.season, r.episode);
}

// one frame grab per key: card and backdrop come from the same frame
QString grabKey(const Request &r)
{
    return r.kind == QLatin1String("ep") ? frameCacheFile(r) : r.titleId + QStringLiteral("-frame");
}

// What a request gets when no frame can be grabbed: a backdrop falls back to the poster, everything
// else to a placeholder with the title (episodes: "S1:E2" + episode title).
QImage fallbackImage(const Request &r, const QVariantMap &ti, const QSize &out)
{
    const QString poster = ti.value(QStringLiteral("poster")).toString();
    if (r.kind == QLatin1String("backdrop") && !poster.isEmpty()) {
        const QImage img = loadCover(poster, out);
        if (!img.isNull())
            return img;
    }
    QString text = ti.value(QStringLiteral("title")).toString();
    if (r.kind == QLatin1String("ep")) {
        const QString et = ti.value(QStringLiteral("episodeTitle")).toString();
        text = r.season > 0 ? QStringLiteral("S%1:E%2").arg(r.season).arg(r.episode) : QStringLiteral("Extra");
        if (!et.isEmpty())
            text += QLatin1Char('\n') + et;
    }
    return placeholder(text, out);
}

// Everything that needs no ffmpeg: artwork files, the frame cache (a card is cut from a cached backdrop
// frame) and remembered failures. Sets *hit = false when a frame grab is needed. With out == QSize()
// (warm-up / re-check) nothing is decoded except to derive a card cache entry, and the image is null.
QImage fromCache(const Request &r, const QVariantMap &ti, const QSize &out, bool *hit)
{
    *hit = true;
    const bool warm = !out.isValid();
    // 1) artwork files: decode directly at the requested size
    QStringList art;
    if (r.kind == QLatin1String("card"))
        art << ti.value(QStringLiteral("poster")).toString() << ti.value(QStringLiteral("backdrop")).toString();
    else if (r.kind == QLatin1String("backdrop"))
        art << ti.value(QStringLiteral("backdrop")).toString();
    else
        art << ti.value(QStringLiteral("still")).toString();
    for (const QString &a : std::as_const(art)) {
        if (a.isEmpty())
            continue;
        if (warm)
            return {}; // nothing to pre-generate
        const QImage img = loadCover(a, out);
        if (!img.isNull())
            return img;
    }

    // 2) cached frame grab
    const QDateTime mtime = ti.value(QStringLiteral("mtime")).toDateTime();
    const QString cacheFile = frameCacheFile(r);
    if (cacheValid(cacheFile, mtime))
        return warm ? QImage() : loadCover(cacheFile, out);
    if (r.kind == QLatin1String("card")) {
        Request bdReq = r;
        bdReq.kind = QStringLiteral("backdrop");
        const QString bdCache = frameCacheFile(bdReq);
        if (cacheValid(bdCache, mtime)) {
            const QImage base = coverCropNoUpscale(loadImage(bdCache), cacheSize(r.kind));
            if (!base.isNull()) {
                saveCache(base, cacheFile);
                return warm ? QImage() : coverCropNoUpscale(base, out);
            }
        }
    }

    // 3) ffmpeg found nothing in this file before
    if (grabFailedBefore(ti.value(QStringLiteral("path")).toString(), grabFraction(r.kind)))
        return warm ? QImage() : fallbackImage(r, ti, out);
    *hit = false;
    return {};
}

// Grabs the frame for `r` with ffmpeg and fills the disk cache. Returns the cached image (cacheSize of
// the kind, smaller for small sources) or null.
QImage grabIntoCache(const Request &r, const QVariantMap &ti)
{
    const QString path = ti.value(QStringLiteral("path")).toString();
    const qint64 dur = ti.value(QStringLiteral("durationMs")).toLongLong();
    const bool hw = preferHw(ti.value(QStringLiteral("codec")).toString(), ti.value(QStringLiteral("height")).toInt());
    const double fraction = grabFraction(r.kind);
    bool definite = false;
    const QImage src = grabAt(path, dur, fraction, hw, &definite);
    if (src.isNull()) {
        if (definite)
            rememberGrabFailed(path, fraction);
        return {};
    }
    if (r.kind == QLatin1String("card")) { // also seed the backdrop cache from the same frame
        Request bdReq = r;
        bdReq.kind = QStringLiteral("backdrop");
        saveCache(coverCropNoUpscale(src, cacheSize(bdReq.kind)), frameCacheFile(bdReq));
    }
    const QImage base = coverCropNoUpscale(src, cacheSize(r.kind));
    saveCache(base, frameCacheFile(r));
    return base;
}

void pumpWarm(const std::shared_ptr<ThumbShared> &shared);

// Answers a live request (on the QML side, the engine deletes the response after finished()).
void deliver(const std::shared_ptr<ThumbShared> &shared, const Pending &p, QImage img)
{
    if (img.isNull())
        img = placeholder(QString(), targetSize(p.requested, p.req.kind));
    if (timing())
        qDebug("[thumbs] %s/%s req %dx%d -> %dx%d in %lld ms", qPrintable(p.req.titleId), qPrintable(p.req.kind),
               p.requested.width(), p.requested.height(), img.width(), img.height(), qint64(p.timer.elapsed()));
    p.resp->m_image = std::move(img);
    {
        QMutexLocker l(&shared->mutex);
        --shared->live;
    }
    emit p.resp->finished(); // the engine deletes resp after this; don't touch it afterwards
    pumpWarm(shared);
}

void runGrab(const std::shared_ptr<ThumbShared> &shared, const QString &key, const Request &r, bool warm);

// Decode pool. afterGrab: the frame grab for this request already ran (`grabbed` is its result if it was
// for the same image) -- answer with whatever exists now, never queue another grab.
void serveLive(const std::shared_ptr<ThumbShared> &shared, const Pending &p, const QImage &grabbed, bool afterGrab)
{
    const Request &r = p.req;
    const QSize out = targetSize(p.requested, r.kind);
    if (p.resp->m_cancelled.loadRelaxed() || g_shuttingDown.loadRelaxed()) {
        deliver(shared, p, QImage(2, 2, QImage::Format_RGB32));
        return;
    }
    const QVariantMap ti = shared->info(r.titleId, r.kind == QLatin1String("ep") ? r.season : -1, r.episode);
    if (ti.isEmpty()) {
        deliver(shared, p, placeholder(QString(), out));
        return;
    }
    if (!grabbed.isNull()) {
        deliver(shared, p, coverCropNoUpscale(grabbed, out));
        return;
    }
    bool hit = false;
    QImage img = fromCache(r, ti, out, &hit);
    if (hit || afterGrab) {
        deliver(shared, p, hit ? std::move(img) : fallbackImage(r, ti, out));
        return;
    }
    // needs ffmpeg: join the grab in flight for this frame, or start one
    const QString key = grabKey(r);
    {
        QMutexLocker l(&shared->mutex);
        const auto it = shared->inFlight.find(key);
        if (it != shared->inFlight.end()) {
            it->append(p);
            return;
        }
        shared->inFlight.insert(key, {p});
    }
    shared->grabPool->start([shared, key, r]() { runGrab(shared, key, r, false); }, 10);
}

// Grab pool. The caller registered `key` in inFlight; the requests waiting there are answered afterwards
// on the decode pool.
void runGrab(const std::shared_ptr<ThumbShared> &shared, const QString &key, const Request &r, bool warm)
{
    const QVariantMap ti = shared->info(r.titleId, r.kind == QLatin1String("ep") ? r.season : -1, r.episode);
    bool skip = ti.isEmpty() || g_shuttingDown.loadRelaxed();
    if (!skip) { // made (or found broken) since the request missed the cache?
        bool hit = false;
        fromCache(r, ti, QSize(), &hit);
        skip = hit;
    }
    if (!skip && !warm) { // nobody wants it any more
        QMutexLocker l(&shared->mutex);
        const QList<Pending> &waiting = shared->inFlight[key];
        skip = std::all_of(waiting.cbegin(), waiting.cend(),
                           [](const Pending &p) { return p.resp->m_cancelled.loadRelaxed() != 0; });
    }
    const QImage base = skip ? QImage() : grabIntoCache(r, ti);
    QList<Pending> waiting;
    {
        QMutexLocker l(&shared->mutex);
        waiting = shared->inFlight.take(key);
    }
    for (const Pending &p : std::as_const(waiting)) {
        const bool same = p.req.kind == r.kind && p.req.season == r.season && p.req.episode == r.episode;
        const QImage mine = same ? base : QImage();
        shared->decodePool->start([shared, p, mine]() { serveLive(shared, p, mine, true); }, 10);
    }
}

void warmJob(const std::shared_ptr<ThumbShared> &shared, const QString &job)
{
    const Request r = parseId(job);
    const QVariantMap ti = shared->info(r.titleId, r.kind == QLatin1String("ep") ? r.season : -1, r.episode);
    bool hit = true;
    if (!ti.isEmpty() && !g_shuttingDown.loadRelaxed())
        fromCache(r, ti, QSize(), &hit);
    if (hit)
        return;
    const QString key = grabKey(r);
    {
        QMutexLocker l(&shared->mutex);
        if (shared->inFlight.contains(key))
            return; // somebody is grabbing this frame already
        shared->inFlight.insert(key, {});
    }
    runGrab(shared, key, r, true);
}

void pumpWarm(const std::shared_ptr<ThumbShared> &shared)
{
    QMutexLocker l(&shared->mutex);
    while (shared->live == 0 && shared->warmActive < 2 && !shared->warmQueue.empty()
           && !g_shuttingDown.loadRelaxed()) {
        const QString job = shared->warmQueue.front();
        shared->warmQueue.pop_front();
        ++shared->warmActive;
        shared->grabPool->start([shared, job]() {
            warmJob(shared, job);
            {
                QMutexLocker l2(&shared->mutex);
                --shared->warmActive;
                ++shared->warmDone;
                if (shared->warmQueue.empty() && shared->warmActive == 0 && shared->warmTotal > 0) {
                    if (timing())
                        qDebug("[thumbs] warm-up of %d items finished in %lld ms", shared->warmTotal,
                               qint64(shared->warmTimer.elapsed()));
                    shared->warmTotal = 0;
#ifdef __GLIBC__
                    malloc_trim(0);
#endif
                }
            }
            pumpWarm(shared);
        }, 0);
    }
}

// Low priority (SCHED_IDLE thread, idle I/O class): delete cache files that no current title/episode can
// use and failure markers of files that are gone or expired. `keep` holds the valid cache file names.
void pruneCache(const QSet<QString> &keep)
{
#ifdef Q_OS_LINUX
    ::syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS (this thread) */, 0, 3 << 13 /* IOPRIO_CLASS_IDLE */);
#endif
    QElapsedTimer t;
    t.start();
    int removed = 0;
    const QDateTime now = QDateTime::currentDateTime();
    const QFileInfoList files = QDir(thumbsDir()).entryInfoList(QDir::Files);
    for (const QFileInfo &fi : files) {
        if (g_shuttingDown.loadRelaxed())
            return;
        const QString name = fi.fileName();
        if (name.startsWith(QLatin1Char('.')))
            continue;
        const bool stalePart = name.endsWith(QLatin1String(".part")) && fi.lastModified().secsTo(now) > 3600;
        if ((name.endsWith(QLatin1String(".jpg")) && !keep.contains(name)) || stalePart)
            removed += QFile::remove(fi.filePath()) ? 1 : 0;
    }
    const QFileInfoList markers = QDir(failedDir()).entryInfoList({QStringLiteral("*.fail")}, QDir::Files);
    for (const QFileInfo &fi : markers) {
        if (g_shuttingDown.loadRelaxed())
            return;
        bool drop = fi.lastModified().secsTo(now) >= kFailedMarkerSecs;
        if (!drop) {
            QFile f(fi.filePath());
            drop = !f.open(QIODevice::ReadOnly) || !QFileInfo::exists(QString::fromUtf8(f.readAll()));
        }
        if (drop)
            removed += QFile::remove(fi.filePath()) ? 1 : 0;
    }
    if (timing())
        qDebug("[thumbs] cache prune: %lld files checked, %d removed in %lld ms",
               qint64(files.size() + markers.size()), removed, qint64(t.elapsed()));
}

} // namespace

ThumbnailProvider::ThumbnailProvider(Library *lib)
    : QQuickAsyncImageProvider(), m_lib(lib), m_shared(std::make_shared<Shared>())
{
#ifdef __GLIBC__
    // Decoded images are short-lived multi-MB buffers allocated on several threads. With glibc's dynamic
    // mmap threshold they end up in per-thread arenas that never shrink; a fixed threshold keeps them
    // mmap()ed so freeing returns the memory to the OS.
    mallopt(M_MMAP_THRESHOLD, 256 * 1024);
#endif
    m_pool.setMaxThreadCount(std::clamp(QThread::idealThreadCount(), 2, 8));
    m_pool.setExpiryTimeout(30000);
    m_grabPool.setMaxThreadCount(3);
    m_grabPool.setExpiryTimeout(30000);
    m_shared->info = infoFn();
    m_shared->decodePool = &m_pool;
    m_shared->grabPool = &m_grabPool;
    if (m_lib) {
        m_lib->setThumbnailProvider(this);
        // prune the disk cache a while after the library settled (at most once a day, see prune())
        m_pruneTimer = new QTimer(this);
        m_pruneTimer->setSingleShot(true);
        m_pruneTimer->setInterval(60000);
        QObject::connect(m_pruneTimer, &QTimer::timeout, this, [this]() { prune(); });
        QObject::connect(m_lib, &Library::libraryChanged, m_pruneTimer, qOverload<>(&QTimer::start));
    }
}

ThumbnailProvider::~ThumbnailProvider()
{
    g_shuttingDown.storeRelaxed(1);
    {
        QMutexLocker l(&m_shared->mutex);
        m_shared->warmQueue.clear();
    }
    m_grabPool.clear(); // drop queued requests
    m_pool.clear();
    m_grabPool.waitForDone(); // running ones abort their ffmpeg quickly (g_shuttingDown)
    m_pool.waitForDone();
    if (m_pruneThread) {
        m_pruneThread->wait();
        delete m_pruneThread;
    }
    if (m_lib)
        m_lib->setThumbnailProvider(nullptr);
}

void ThumbnailProvider::invalidate(const QString &titleId)
{
    QMutexLocker l(&m_shared->mutex);
    const QString prefix = titleId + QLatin1Char('/');
    std::erase_if(m_shared->warmQueue, [&](const QString &job) {
        return job.startsWith(prefix) && !job.startsWith(prefix + QStringLiteral("ep/"));
    });
}

void ThumbnailProvider::warmUp(const QStringList &jobIds)
{
    {
        QMutexLocker l(&m_shared->mutex);
        m_shared->warmQueue.assign(jobIds.cbegin(), jobIds.cend());
        m_shared->warmTotal = int(jobIds.size());
        m_shared->warmDone = 0;
        m_shared->warmTimer.start();
    }
    pumpWarm(m_shared);
}

void ThumbnailProvider::prependWarmUp(const QStringList &jobIds)
{
    {
        QMutexLocker l(&m_shared->mutex);
        QSet<QString> fresh;
        QStringList front;
        for (const QString &job : jobIds) {
            if (fresh.contains(job) || m_shared->inFlight.contains(grabKey(parseId(job))))
                continue;
            fresh.insert(job);
            front << job;
        }
        // jobs that were queued already only move
        const auto moved = std::erase_if(m_shared->warmQueue, [&](const QString &job) { return fresh.contains(job); });
        m_shared->warmQueue.insert(m_shared->warmQueue.begin(), front.cbegin(), front.cend());
        if (m_shared->warmTotal == 0) { // the previous warm-up had finished: a new (small) batch
            m_shared->warmDone = 0;
            m_shared->warmTimer.start();
        }
        m_shared->warmTotal += int(front.size() - qsizetype(moved));
    }
    pumpWarm(m_shared);
}

// Library::thumbInfo is private; ThumbnailProvider is a friend, so bind it here for the workers.
std::function<QVariantMap(const QString &, int, int)> ThumbnailProvider::infoFn() const
{
    Library *lib = m_lib;
    return [lib](const QString &tid, int s, int e) { return lib ? lib->thumbInfo(tid, s, e) : QVariantMap(); };
}

// Main thread, a minute after the library changed. Only runs when the library looks complete (not scanning,
// every library folder present -- an unmounted drive must not cost its thumbnails) and the last prune is
// more than a day old (stamp file thumbs/.pruned). Reads the library only through its public C++ API.
void ThumbnailProvider::prune()
{
    if (!m_lib || m_pruneThread || g_shuttingDown.loadRelaxed())
        return;
    if (m_lib->scanning()) {
        m_pruneTimer->start();
        return;
    }
    const QString stamp = thumbsDir() + QStringLiteral("/.pruned");
    const QFileInfo si(stamp);
    if (si.exists() && si.lastModified().secsTo(QDateTime::currentDateTime()) < 24 * 3600)
        return;
    const QStringList folders = m_lib->folders();
    const QStringList ids = m_lib->allTitles()->ids();
    if (ids.isEmpty() || !std::all_of(folders.cbegin(), folders.cend(), [](const QString &dir) { return QFileInfo::exists(dir); }))
        return;
    QSet<QString> keep;
    for (const QString &id : ids) {
        const Title *t = m_lib->findTitle(id);
        if (!t)
            continue;
        keep << frameCacheName(id, QStringLiteral("card"), 0, 0) << frameCacheName(id, QStringLiteral("backdrop"), 0, 0);
        for (const MediaFile &f : t->files)
            keep << frameCacheName(id, QStringLiteral("ep"), f.season, f.episode);
    }
    QFile f(stamp);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.close();
    m_pruneThread = QThread::create([keep]() { pruneCache(keep); });
    QObject::connect(m_pruneThread, &QThread::finished, this, [this]() {
        m_pruneThread->deleteLater();
        m_pruneThread = nullptr;
    });
    m_pruneThread->start(QThread::IdlePriority);
}

QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id, const QSize &requestedSize)
{
    auto *resp = new ThumbResponse;
    Pending p;
    p.resp = resp;
    p.req = parseId(id);
    p.requested = requestedSize;
    p.timer.start();
    std::shared_ptr<ThumbShared> shared = m_shared;
    {
        QMutexLocker l(&shared->mutex);
        ++shared->live;
    }
    m_pool.start([shared, p]() { serveLive(shared, p, QImage(), false); }, 10);
    return resp;
}
