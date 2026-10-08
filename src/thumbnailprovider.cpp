#include "thumbnailprovider.h"
#include "library.h"

#include <QAtomicInt>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QImageReader>
#include <QLinearGradient>
#include <QMutex>
#include <QPainter>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QWaitCondition>
#include <deque>
#include <functional>

#ifdef __GLIBC__
#include <malloc.h>
#endif

// The provider runs requestImageResponse() on QML's image reader thread and the actual work on m_pool
// (max 3 threads, so at most 3 ffmpeg processes run at once). Library data is only read through
// Library::thumbInfo(), which copies the needed fields under Library::m_mutex.
//
// Two kinds of work share the pool:
//  - live QML requests (priority 10), started immediately;
//  - warm-up jobs queued by the Library after a scan. They are started one by one, only while no live
//    request is pending, and at most 2 at a time, so a live request always finds a free thread.
// Only ffmpeg frame grabs are cached on disk (CacheLocation/thumbs/<id>-<kind>.jpg); artwork files
// (posterFile/backdropFile/episode stills) are decoded directly at the requested size.

struct ThumbShared {
    QMutex mutex;
    std::deque<QString> warmQueue;
    int live = 0;        // live requests queued or running
    int warmActive = 0;  // warm-up jobs running
    int warmTotal = 0, warmDone = 0;
    QElapsedTimer warmTimer;

    // per-cache-key exclusion, so a live request waits for a warm-up job producing the same frame
    QMutex keyMutex;
    QWaitCondition keyCond;
    QSet<QString> busyKeys;
};
struct ThumbnailProvider::Shared : ThumbShared {};

namespace {

QAtomicInt g_shuttingDown(0);

bool timing()
{
    static const bool on = qEnvironmentVariableIntValue("QTFLIX_TIMING") > 0;
    return on;
}

QMutex g_failedMutex;
QSet<QString> g_failedFrames; // "path@sec" that ffmpeg could not decode in this session

QString thumbsDir()
{
    static const QString dir = [] {
        const QString d = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/thumbs");
        QDir().mkpath(d);
        return d;
    }();
    return dir;
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

enum class GrabStatus { Ok, NoFrame, Error };

// One ffmpeg run. NoFrame = ffmpeg ran fine but nothing decodable at that position (e.g. a webm without
// cues seeked past its only keyframe); Error = ffmpeg failed / timed out (worth retrying without VA-API).
QImage runFfmpeg(const QString &exe, const QString &file, double sec, bool hw, GrabStatus *status)
{
    *status = GrabStatus::Error;
    QStringList args = {QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-v"),
                        QStringLiteral("error")};
    if (hw)
        args << QStringLiteral("-hwaccel") << QStringLiteral("vaapi") << QStringLiteral("-hwaccel_device")
             << vaapiDevice();
    // keyframe-only decoding + fast (non-accurate) input seek: we get the keyframe at/before `sec`
    args << QStringLiteral("-skip_frame") << QStringLiteral("nokey") << QStringLiteral("-threads")
         << QStringLiteral("2") << QStringLiteral("-noaccurate_seek") << QStringLiteral("-ss")
         << QString::number(sec, 'f', 2) << QStringLiteral("-i") << file << QStringLiteral("-an")
         << QStringLiteral("-sn") << QStringLiteral("-dn") << QStringLiteral("-frames:v") << QStringLiteral("1")
         << QStringLiteral("-vf") << QStringLiteral("scale=1280:-2") << QStringLiteral("-f")
         << QStringLiteral("image2pipe") << QStringLiteral("-c:v") << QStringLiteral("ppm") << QStringLiteral("pipe:1");
    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.setStandardErrorFile(QProcess::nullDevice());
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
QImage grabFrame(const QString &file, double sec, bool hwFirst)
{
    static const QString exe = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (exe.isEmpty() || file.isEmpty() || !QFileInfo::exists(file))
        return {};
    const QString key = file + QLatin1Char('@') + QString::number(int(sec));
    {
        QMutexLocker l(&g_failedMutex);
        if (g_failedFrames.contains(key))
            return {};
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
    if (st == GrabStatus::Error && !g_shuttingDown.loadRelaxed()) {
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
        g_failedFrames.insert(key);
    }
    return img;
}

QImage grabAt(const QString &file, qint64 durationMs, double fraction, bool hw)
{
    double sec = 60.0;
    if (durationMs > 0)
        sec = durationMs < 120000 ? durationMs * 0.10 / 1000.0 : durationMs * fraction / 1000.0;
    QImage img = grabFrame(file, sec, hw);
    // a fade / black scene would make a useless thumbnail (and fool the black-bar trimming): retry later once
    if (!img.isNull() && durationMs > 0 && !g_shuttingDown.loadRelaxed() && meanLuma(img) < 16) {
        const double later = sec + durationMs * 0.10 / 1000.0;
        if (later < durationMs / 1000.0 - 1.0) {
            const QImage again = grabFrame(file, later, hw);
            if (!again.isNull() && meanLuma(again) > meanLuma(img))
                img = again;
        }
    }
    if (img.isNull() && sec > 0.5 && !g_shuttingDown.loadRelaxed())
        img = grabFrame(file, durationMs > 0 ? 0.0 : 3.0, hw); // short/unknown-length file: try near the start
    if (img.isNull() && !g_shuttingDown.loadRelaxed() && durationMs <= 0)
        img = grabFrame(file, 0.0, hw);
    return trimBlackBorders(img);
}

void saveCache(const QImage &img, const QString &path)
{
    if (img.isNull())
        return;
    const QString tmp = path + QStringLiteral(".part");
    if (img.save(tmp, "JPG", 88)) {
        QFile::remove(path);
        QFile::rename(tmp, path);
    } else {
        QFile::remove(tmp);
    }
}

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

struct KeyLock {
    ThumbShared *s;
    QString key;
    KeyLock(ThumbShared *sh, const QString &k) : s(sh), key(k)
    {
        QMutexLocker l(&s->keyMutex);
        while (s->busyKeys.contains(key))
            s->keyCond.wait(&s->keyMutex);
        s->busyKeys.insert(key);
    }
    ~KeyLock()
    {
        QMutexLocker l(&s->keyMutex);
        s->busyKeys.remove(key);
        s->keyCond.wakeAll();
    }
};

using InfoFn = std::function<QVariantMap(const QString &, int, int)>;

bool cacheValid(const QString &file, const QDateTime &mtime)
{
    const QFileInfo fi(file);
    return fi.exists() && fi.size() > 0 && (!mtime.isValid() || fi.lastModified() >= mtime);
}

QString frameCacheFile(const Request &r)
{
    QString name = r.titleId + QLatin1Char('-') + r.kind;
    if (r.kind == QLatin1String("ep"))
        name += QStringLiteral("-%1-%2").arg(r.season).arg(r.episode);
    return thumbsDir() + QLatin1Char('/') + name + QStringLiteral(".jpg");
}

// Produces the image for a request. With out == QSize() (warm-up) it only makes sure the disk cache is
// filled and returns a null image.
QImage produce(ThumbShared *shared, const Request &r, const QSize &out, const QAtomicInt *cancelled,
               const InfoFn &info)
{
    const bool warm = !out.isValid();
    const QVariantMap ti = info(r.titleId, r.kind == QLatin1String("ep") ? r.season : -1, r.episode);
    if (ti.isEmpty())
        return warm ? QImage() : placeholder(QString(), out);
    const QString title = ti.value(QStringLiteral("title")).toString();
    auto aborted = [&] { return g_shuttingDown.loadRelaxed() || (cancelled && cancelled->loadRelaxed()); };
    if (aborted())
        return warm ? QImage() : placeholder(title, out);

    const QString path = ti.value(QStringLiteral("path")).toString();
    const qint64 dur = ti.value(QStringLiteral("durationMs")).toLongLong();
    const QString poster = ti.value(QStringLiteral("poster")).toString();
    const QString backdrop = ti.value(QStringLiteral("backdrop")).toString();
    const QString still = ti.value(QStringLiteral("still")).toString();
    const QDateTime mtime = ti.value(QStringLiteral("mtime")).toDateTime();
    const bool hw = preferHw(ti.value(QStringLiteral("codec")).toString(), ti.value(QStringLiteral("height")).toInt());

    // 1) artwork files: decode directly at the requested size
    QStringList art;
    if (r.kind == QLatin1String("card"))
        art << poster << backdrop;
    else if (r.kind == QLatin1String("backdrop"))
        art << backdrop;
    else
        art << still;
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
    const QString cacheFile = frameCacheFile(r);
    if (cacheValid(cacheFile, mtime))
        return warm ? QImage() : loadCover(cacheFile, out);

    // 3) grab a frame (card and backdrop share the same frame and lock)
    QImage base;
    {
        const bool frameKind = r.kind != QLatin1String("ep");
        KeyLock lock(shared, frameKind ? r.titleId + QStringLiteral("-frame") : cacheFile);
        if (cacheValid(cacheFile, mtime)) // somebody else made it while we waited
            return warm ? QImage() : loadCover(cacheFile, out);
        if (aborted())
            return warm ? QImage() : placeholder(title, out);
        const QSize cs = cacheSize(r.kind);
        if (r.kind == QLatin1String("card")) {
            Request bdReq = r;
            bdReq.kind = QStringLiteral("backdrop");
            const QString bdCache = frameCacheFile(bdReq);
            QImage src;
            if (cacheValid(bdCache, mtime))
                src = loadImage(bdCache);
            if (src.isNull()) {
                src = grabAt(path, dur, 0.20, hw);
                if (!src.isNull()) // also seed the backdrop cache from the same frame
                    saveCache(coverCrop(src, cacheSize(QStringLiteral("backdrop"))), bdCache);
            }
            if (!src.isNull())
                base = coverCrop(src, cs);
        } else if (r.kind == QLatin1String("backdrop")) {
            QImage src = grabAt(path, dur, 0.20, hw);
            if (src.isNull() && !poster.isEmpty() && !warm)
                return loadCover(poster, out);
            if (!src.isNull())
                base = coverCrop(src, cs);
        } else { // episode
            const QImage src = grabAt(path, dur, 0.35, hw);
            if (!src.isNull())
                base = coverCrop(src, cs);
        }
        if (!base.isNull())
            saveCache(base, cacheFile);
    }
    if (warm)
        return {};

    if (base.isNull()) {
        QString text = title;
        if (r.kind == QLatin1String("ep")) {
            const QString et = ti.value(QStringLiteral("episodeTitle")).toString();
            text = r.season > 0 ? QStringLiteral("S%1:E%2").arg(r.season).arg(r.episode) : QStringLiteral("Extra");
            if (!et.isEmpty())
                text += QLatin1Char('\n') + et;
        }
        return placeholder(text, out);
    }
    return coverCropNoUpscale(base, out);
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
    m_pool.setMaxThreadCount(3);
    m_pool.setExpiryTimeout(30000);
    if (m_lib)
        m_lib->setThumbnailProvider(this);
}

ThumbnailProvider::~ThumbnailProvider()
{
    g_shuttingDown.storeRelaxed(1);
    {
        QMutexLocker l(&m_shared->mutex);
        m_shared->warmQueue.clear();
    }
    m_pool.clear();       // drop queued requests
    m_pool.waitForDone(); // running ones abort their ffmpeg quickly (g_shuttingDown)
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

namespace {
void pumpWarm(const std::shared_ptr<ThumbShared> &shared, QThreadPool *pool, const InfoFn &info);
} // namespace

void ThumbnailProvider::warmUp(const QStringList &jobIds)
{
    {
        QMutexLocker l(&m_shared->mutex);
        m_shared->warmQueue.assign(jobIds.cbegin(), jobIds.cend());
        m_shared->warmTotal = int(jobIds.size());
        m_shared->warmDone = 0;
        m_shared->warmTimer.start();
    }
    pumpWarm(m_shared, &m_pool, infoFn());
}

// Library::thumbInfo is private; ThumbnailProvider is a friend, so bind it here for the workers.
std::function<QVariantMap(const QString &, int, int)> ThumbnailProvider::infoFn() const
{
    Library *lib = m_lib;
    return [lib](const QString &tid, int s, int e) { return lib ? lib->thumbInfo(tid, s, e) : QVariantMap(); };
}

namespace {

void pumpWarm(const std::shared_ptr<ThumbShared> &shared, QThreadPool *pool, const InfoFn &info)
{
    QMutexLocker l(&shared->mutex);
    while (shared->live == 0 && shared->warmActive < 2 && !shared->warmQueue.empty()
           && !g_shuttingDown.loadRelaxed()) {
        const QString job = shared->warmQueue.front();
        shared->warmQueue.pop_front();
        ++shared->warmActive;
        pool->start([shared, pool, info, job]() {
            const Request req = parseId(job);
            produce(shared.get(), req, QSize(), nullptr, info);
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
            pumpWarm(shared, pool, info);
        }, 0);
    }
}

} // namespace

QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id, const QSize &requestedSize)
{
    auto *resp = new ThumbResponse;
    const Request req = parseId(id);
    const InfoFn info = infoFn();
    std::shared_ptr<ThumbShared> shared = m_shared;
    QThreadPool *pool = &m_pool;
    {
        QMutexLocker l(&shared->mutex);
        ++shared->live;
    }
    pool->start([resp, req, requestedSize, info, shared, pool]() {
        QElapsedTimer t;
        t.start();
        const QSize out = targetSize(requestedSize, req.kind);
        QImage img;
        if (resp->m_cancelled.loadRelaxed() || g_shuttingDown.loadRelaxed())
            img = QImage(2, 2, QImage::Format_RGB32);
        else
            img = produce(shared.get(), req, out, &resp->m_cancelled, info);
        if (img.isNull())
            img = placeholder(QString(), out);
        if (timing())
            qDebug("[thumbs] %s/%s req %dx%d -> %dx%d in %lld ms", qPrintable(req.titleId), qPrintable(req.kind),
                   requestedSize.width(), requestedSize.height(), img.width(), img.height(), qint64(t.elapsed()));
        resp->m_image = std::move(img);
        {
            QMutexLocker l(&shared->mutex);
            --shared->live;
        }
        emit resp->finished(); // the engine deletes resp after this; don't touch it afterwards
        pumpWarm(shared, pool, info);
    }, 10);
    return resp;
}
