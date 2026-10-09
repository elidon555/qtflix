#include "library.h"
#include "thumbnailprovider.h"

#include <QtConcurrent/QtConcurrent>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQuickWindow>
#include <QCollator>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSemaphore>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cstdio>
#include <unistd.h>

// =====================================================================================================
//  Filename parsing (pure functions, run on the scan thread)
// =====================================================================================================
namespace {

bool timingOn()
{
    static const bool on = qEnvironmentVariableIntValue("QTFLIX_TIMING") > 0;
    return on;
}

// milliseconds since the process was started (kernel start time, so it includes dynamic loading)
qint64 msSinceStart()
{
    static const double startSec = [] {
        QFile f(QStringLiteral("/proc/self/stat"));
        if (!f.open(QIODevice::ReadOnly))
            return -1.0;
        const QByteArray st = f.readAll();
        const QList<QByteArray> fields = st.mid(st.lastIndexOf(')') + 2).split(' ');
        // fields[0] is field 3 (state); starttime is field 22
        const double ticks = fields.value(19).toDouble();
        return ticks / double(sysconf(_SC_CLK_TCK));
    }();
    QFile up(QStringLiteral("/proc/uptime"));
    if (startSec < 0 || !up.open(QIODevice::ReadOnly))
        return -1;
    const double now = up.readAll().split(' ').value(0).toDouble();
    return qint64((now - startSec) * 1000.0);
}

#define TIMING(...) do { if (timingOn()) qDebug("[timing] %6lld ms  %s", msSinceStart(), qPrintable(QString::asprintf(__VA_ARGS__))); } while (0)

// side results of one scan
struct ScanInfo {
    QStringList dirs;     // directories to watch (capped)
    int unstable = 0;     // files skipped because they are still being written
    int rejected = 0;     // unreadable / not a video / ffprobe error
    qint64 ms = 0;
    qint64 walkMs = 0, probeMs = 0;
    int totalDirs = 0;    // folders walked
    int listedDirs = 0;   // ... of which read from disk (the rest re-used the last listing)
    QStringList newDirs;  // folders read for the first time by an incremental scan
    int probed = 0;       // files run through ffprobe
};
constexpr int kMaxWatchedDirs = 500;

const QStringList &videoExtensions()
{
    static const QStringList l = {"mkv", "mp4", "avi", "mov", "webm", "m4v", "wmv",
                                  "flv", "ts", "m2ts", "mpg", "mpeg"};
    return l;
}

const QStringList &subtitleExtensions()
{
    static const QStringList l = {"srt", "vtt", "ass", "ssa"};
    return l;
}

int currentYear() { return QDate::currentDate().year(); }

// Screen recordings, phone clips, timestamps... -> titles are just the cleaned file name.
bool looksLikeRecording(const QString &stem)
{
    static const QRegularExpression kw(
        QStringLiteral("(screen[ _-]?rec|recording|screencast|screen[ _-]?capture|screenshot|recorder|"
                       "kooha|vokoscreen|obs[ _-]|peek[ _-]|rustdesk|^vid[_-]?\\d|^img[_-]\\d|^pxl[_-]\\d|"
                       "^mvi[_-]\\d|^dji[_-]\\d|^gopr\\d|^signal-\\d|^whatsapp)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression date(
        QStringLiteral("(?:^|\\D)(?:19|20)\\d{2}[-_.]\\d{1,2}[-_.]\\d{1,2}(?:\\D|$)"));
    static const QRegularExpression date2(
        QStringLiteral("(?:^|\\D)\\d{1,2}[-_.]\\d{1,2}[-_.](?:19|20)\\d{2}(?:\\D|$)"));
    static const QRegularExpression stamp(QStringLiteral("(?:19|20)\\d{6}[-_ T]?\\d{4,6}"));
    return kw.match(stem).hasMatch() || date.match(stem).hasMatch() || date2.match(stem).hasMatch()
           || stamp.match(stem).hasMatch();
}

// "CGPF-2891", "JIRA-12" style
bool looksLikeTicket(const QString &stem)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z]{2,8}-\\d{1,6}\\b"));
    return re.match(stem).hasMatch();
}

// "Movie.Name.2019.1080p.H.264-GRP" -> "Movie Name 2019 1080p H264-GRP" (keeps "5.1" / "7.1" style decimals)
QString sceneToSpaces(const QString &in)
{
    static const QRegularExpression hdot(QStringLiteral("\\b([HhXx])\\.(26[45])\\b"));
    QString s = in;
    s.replace(hdot, QStringLiteral("\\1\\2"));
    s.replace(QLatin1Char('_'), QLatin1Char(' '));
    const auto dots = s.count(QLatin1Char('.'));
    const auto spaces = s.count(QLatin1Char(' '));
    if (dots >= 2 && dots > spaces) {
        for (int i = 0; i < s.size(); ++i) {
            if (s.at(i) != QLatin1Char('.'))
                continue;
            // keep single-digit decimals like 5.1 / 2.0
            const bool decimal = i >= 1 && i + 1 < s.size() && s.at(i - 1).isDigit() && s.at(i + 1).isDigit()
                                 && (i < 2 || !s.at(i - 2).isLetterOrNumber())
                                 && (i + 2 >= s.size() || !s.at(i + 2).isLetterOrNumber());
            if (!decimal)
                s[i] = QLatin1Char(' ');
        }
    }
    return s.simplified();
}

// removes "[Group] " prefixes and "www.site.com - " prefixes. Sets hadGroup if a [..] prefix was removed.
QString stripLeadingJunk(const QString &in, bool *hadGroup = nullptr)
{
    static const QRegularExpression group(QStringLiteral("^\\s*(?:\\[[^\\]]*\\]\\s*)+"));
    static const QRegularExpression site(
        QStringLiteral("^\\s*(?:www\\.)?[\\w-]+\\.(?:com|org|net|to|io|me|cc|tv)\\s*[-\\x{2013}]\\s*"),
        QRegularExpression::CaseInsensitiveOption);
    QString s = in;
    auto m = group.match(s);
    if (m.hasMatch() && m.capturedLength() < s.size()) {
        s = s.mid(m.capturedLength());
        if (hadGroup)
            *hadGroup = true;
    }
    m = site.match(s);
    if (m.hasMatch() && m.capturedLength() < s.size())
        s = s.mid(m.capturedLength());
    return s.trimmed();
}

const QRegularExpression &hardJunkRe()
{
    static const QRegularExpression re(
        QStringLiteral(
            "(?<![A-Za-z0-9])(?:"
            "\\d{3,4}[pi]|4k|8k|uhd|hdr10\\+?|hdr|sdr|dovi|"
            "blu-?ray|bdrip|brrip|bdremux|remux|web-?dl|web-?rip|hdtv|pdtv|dvdrip|dvdscr|hdrip|hdcam|camrip|"
            "x26[45]|h26[45]|hevc|avc|xvid|divx|av1|vp9|10-?bit|8-?bit|12-?bit|"
            "aac(?:2\\.0|5\\.1)?|e?ac-?3|ddp?\\+?(?:5\\.1|7\\.1|2\\.0)|dts(?:-?hd|-?x|-?ma)?|truehd|atmos|flac|"
            "[257]\\.[01]|[268]ch|amzn|dsnp|hmax|atvp|pcok|"
            "director'?s[ .]cut"
            ")(?![A-Za-z0-9])"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// words that are junk only when ALL CAPS (e.g. "REMASTERED") or when trailing the title
const QStringList &softJunkWords()
{
    static const QStringList l = {"remastered", "proper", "repack", "rerip", "extended", "unrated", "uncut",
                                  "limited", "internal", "imax", "criterion", "multi", "dual", "dubbed",
                                  "subbed", "multisub", "complete", "theatrical", "web", "dv", "nf", "hulu",
                                  "dvd", "bd", "edition", "collection", "season", "series"};
    return l;
}

int firstJunkPos(const QString &s)
{
    int pos = int(s.size());
    auto m = hardJunkRe().match(s);
    if (m.hasMatch())
        pos = int(m.capturedStart());
    // ALL CAPS soft words also cut (REMASTERED, PROPER, WEB, ...)
    static const QRegularExpression word(QStringLiteral("(?<![A-Za-z0-9])([A-Z]{2,})(?![A-Za-z0-9])"));
    auto it = word.globalMatch(s);
    while (it.hasNext()) {
        auto wm = it.next();
        if (wm.capturedStart() >= pos)
            break;
        if (wm.capturedStart() > 0 && softJunkWords().contains(wm.captured(1).toLower())) {
            pos = int(wm.capturedStart());
            break;
        }
    }
    return pos;
}

QString titleCaseIfLower(QString s)
{
    bool hasUpper = false, hasLetter = false;
    for (const QChar c : s) {
        hasUpper |= c.isUpper();
        hasLetter |= c.isLetter();
    }
    if (hasLetter && !hasUpper) {
        bool start = true;
        for (int i = 0; i < s.size(); ++i) {
            if (start && s.at(i).isLetter())
                s[i] = s.at(i).toUpper();
            start = s.at(i).isSpace() || s.at(i) == QLatin1Char('-') || s.at(i) == QLatin1Char('(');
        }
    } else if (!s.isEmpty() && s.at(0).isLower()) {
        s[0] = s.at(0).toUpper();
    }
    return s;
}

QString trimSeparators(QString s, const QString &chars = QStringLiteral(" -_,;:~\x{2013}"))
{
    int a = 0, b = int(s.size());
    while (a < b && (chars.contains(s.at(a)) || s.at(a).isSpace()))
        ++a;
    while (b > a && (chars.contains(s.at(b - 1)) || s.at(b - 1).isSpace()))
        --b;
    return s.mid(a, b - a).simplified();
}

struct NameYear { QString name; int year = 0; };

// "Minority Report 2002 REMASTERED 1080p (Multi) BluRay" -> {"Minority Report", 2002}
NameYear cleanTitle(const QString &in)
{
    static const QRegularExpression yearRe(
        QStringLiteral("(?<![A-Za-z0-9])[\\(\\[]?((?:19|20)\\d{2})[\\)\\]]?(?![A-Za-z0-9])(?!-\\d)"));
    static const QRegularExpression bracketRe(QStringLiteral("[\\[\\(\\{]"));

    const QString s = in.simplified();
    int junk = firstJunkPos(s);

    // first bracket (at index >= 1) that is not a pure year
    int bracket = int(s.size());
    for (int i = int(s.indexOf(bracketRe, 1)); i >= 0 && i < s.size(); i = int(s.indexOf(bracketRe, i + 1))) {
        static const QRegularExpression pureYear(QStringLiteral("^[\\(\\[](?:19|20)\\d{2}[\\)\\]]"));
        if (!pureYear.match(s.mid(i, 6)).hasMatch()) {
            bracket = i;
            break;
        }
    }
    const int limit = std::min(junk, bracket);

    NameYear r;
    int yearPos = -1;
    auto it = yearRe.globalMatch(s);
    while (it.hasNext()) {
        auto m = it.next();
        if (m.capturedStart() >= limit)
            break;
        const int y = m.captured(1).toInt();
        if (m.capturedStart() == 0 || y < 1920 || y > currentYear() + 1)
            continue;
        yearPos = int(m.capturedStart());
        r.year = y;
    }
    int cut = limit;
    if (yearPos > 0)
        cut = std::min(cut, yearPos);

    QString name = trimSeparators(s.left(cut));
    // drop trailing soft junk words ("Movie Extended" -> "Movie")
    for (;;) {
        const int sp = int(name.lastIndexOf(QLatin1Char(' ')));
        if (sp <= 0)
            break;
        const QString last = name.mid(sp + 1).toLower();
        if (!softJunkWords().contains(last) || last == QLatin1String("web") || last == QLatin1String("season")
            || last == QLatin1String("series"))
            break;
        name = trimSeparators(name.left(sp));
    }
    if (name.isEmpty()) {
        name = trimSeparators(s.left(std::max(limit, 1)));
        if (name.isEmpty())
            name = s;
    }
    r.name = titleCaseIfLower(name);
    return r;
}

// text after the SxxEyy marker -> episode title
QString cleanEpisodeTitle(const QString &after)
{
    static const QRegularExpression bracketRe(QStringLiteral("[\\[\\(\\{]"));
    QString s = after;
    // leading separators (but keep a leading '.' that belongs to the title, e.g. ".exe")
    int a = 0;
    while (a < s.size() && (s.at(a).isSpace() || s.at(a) == QLatin1Char('-') || s.at(a) == QLatin1Char(':')
                            || s.at(a) == QLatin1Char('_') || s.at(a) == QChar(0x2013)))
        ++a;
    s = s.mid(a);
    int cut = std::min(firstJunkPos(s), int(s.size()));
    const int b = int(s.indexOf(bracketRe));
    if (b >= 0)
        cut = std::min(cut, b);
    s = trimSeparators(s.left(cut), QStringLiteral(" -_,;:\x{2013}"));
    return s;
}

QString cleanRecordingTitle(const QString &stem)
{
    static const QRegularExpression domainParen(
        QStringLiteral("\\s*[\\(\\[][^\\)\\]]*(?:www\\.|\\.com|\\.org|\\.net|\\.io)[^\\)\\]]*[\\)\\]]"),
        QRegularExpression::CaseInsensitiveOption);
    QString s = stem;
    s.remove(domainParen);
    s.replace(QLatin1Char('_'), QLatin1Char(' '));
    for (int i = 0; i < s.size(); ++i) {
        if (s.at(i) != QLatin1Char('.'))
            continue;
        const bool digits = i >= 1 && i + 1 < s.size() && s.at(i - 1).isDigit() && s.at(i + 1).isDigit();
        if (!digits)
            s[i] = QLatin1Char(' ');
    }
    s = trimSeparators(s);
    if (s.isEmpty())
        s = stem;
    if (!s.isEmpty() && s.at(0).isLower())
        s[0] = s.at(0).toUpper();
    return s;
}

QString cleanExtraTitle(const QString &stem)
{
    QString s = stripLeadingJunk(sceneToSpaces(stem));
    const int junk = firstJunkPos(s);
    s = trimSeparators(s.left(junk));
    return s.isEmpty() ? stem : titleCaseIfLower(s);
}

struct EpMatch { bool ok = false; int season = 0, episode = 0, start = 0, end = 0; };

EpMatch findEpisodeMarker(const QString &s)
{
    static const QRegularExpression sxe(
        QStringLiteral("(?<![A-Za-z0-9])s(\\d{1,2})\\s?[ ._-]?\\s?e(\\d{1,3})(?:[ ._-]?(?:-|e)\\d{1,3}(?![0-9]))*(?![0-9])"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression nxn(QStringLiteral("(?<![A-Za-z0-9])(\\d{1,2})x(\\d{2,3})(?![0-9])"),
                                        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression words(
        QStringLiteral("(?<![A-Za-z0-9])season\\s*(\\d{1,2})\\s*[-,.:]?\\s*(?:episode|ep\\.?)\\s*(\\d{1,3})(?![0-9])"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QRegularExpression *re : {&sxe, &words, &nxn}) {
        auto m = re->match(s);
        if (m.hasMatch()) {
            EpMatch e;
            e.ok = true;
            e.season = m.captured(1).toInt();
            e.episode = m.captured(2).toInt();
            e.start = int(m.capturedStart());
            e.end = int(m.capturedEnd());
            return e;
        }
    }
    return {};
}

// "Season 01", "S2", "Staffel 3", "Specials" -> season number, -1 if not a season folder
int seasonFolderNumber(const QString &dirName)
{
    static const QRegularExpression re(
        QStringLiteral("^(?:season|series|staffel|saison|temporada|seizoen|sezona|sezoni|stagione|s)\\s*[._-]?\\s*(\\d{1,3})$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression specials(QStringLiteral("^specials?$"), QRegularExpression::CaseInsensitiveOption);
    const QString n = dirName.trimmed();
    auto m = re.match(n);
    if (m.hasMatch())
        return m.captured(1).toInt();
    if (specials.match(n).hasMatch())
        return 0;
    return -1;
}

bool isExtrasFolder(const QString &dirName)
{
    static const QRegularExpression re(
        QStringLiteral("^(?:featurettes?|extras?|bonus(?: features)?|behind[ ._-]the[ ._-]scenes|deleted[ ._-]scenes|"
                       "interviews?|trailers?|shorts?|making[ ._-]of|scenes|other|specials?)$"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(dirName.trimmed()).hasMatch();
}

// series name from a folder like "Person of Interest (2011) Season 5 S05 (1080p ...)" or "Show Name/"
NameYear seriesFromFolder(const QString &dirName)
{
    static const QRegularExpression seasonTail(
        QStringLiteral("(?<![A-Za-z0-9])(?:season|series|staffel|saison|temporada|complete|s\\d{1,2}(?:-s?\\d{1,2})?(?![A-Za-z0-9]))[\\s\\S]*$"),
        QRegularExpression::CaseInsensitiveOption);
    QString s = stripLeadingJunk(sceneToSpaces(dirName));
    NameYear ny = cleanTitle(s);
    QString n = ny.name;
    auto m = seasonTail.match(n);
    if (m.hasMatch() && m.capturedStart() > 0)
        n = trimSeparators(n.left(m.capturedStart()));
    ny.name = n;
    return ny;
}

QString normalizeKey(const QString &name)
{
    QString s = name.toLower();
    s.replace(QLatin1Char('&'), QStringLiteral(" and "));
    QString out;
    out.reserve(s.size());
    for (const QChar c : s)
        out += c.isLetterOrNumber() ? c : QLatin1Char(' ');
    return out.simplified();
}

QString sha1Id(const QString &key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

quint32 seedOf(const QString &id)
{
    bool ok = false;
    const quint32 v = id.left(8).toUInt(&ok, 16);
    return ok ? v : quint32(qHashBits(id.constData(), id.size() * sizeof(QChar), 7));
}

QString prettyCodec(const QString &c)
{
    const QString l = c.toLower();
    if (l == QLatin1String("hevc") || l == QLatin1String("h265")) return QStringLiteral("HEVC");
    if (l == QLatin1String("h264")) return QStringLiteral("H.264");
    if (l == QLatin1String("mpeg4")) return QStringLiteral("MPEG-4");
    if (l == QLatin1String("mpeg2video")) return QStringLiteral("MPEG-2");
    if (l.isEmpty()) return {};
    return l.toUpper();
}

QString resolutionLabel(int w, int h)
{
    if (w <= 0 && h <= 0) return {};
    if (h >= 2000 || w >= 3200) return QStringLiteral("4K");
    if (h >= 1000 || w >= 1800) return QStringLiteral("1080p");
    if (h >= 700 || w >= 1200) return QStringLiteral("720p");
    if (h >= 540) return QStringLiteral("576p");
    return QStringLiteral("SD");
}

QString qualityLabel(int w, int h)
{
    if (h >= 2000 || w >= 3200) return QStringLiteral("4K");
    if (h >= 700 || w >= 1200) return QStringLiteral("HD");
    return {};
}

QString humanDuration(qint64 ms)
{
    if (ms <= 0) return {};
    const qint64 totalMin = (ms + 30000) / 60000;
    if (totalMin < 1) return QStringLiteral("%1 sec").arg(ms / 1000);
    if (totalMin < 60) return QStringLiteral("%1 min").arg(totalMin);
    const qint64 h = totalMin / 60, m = totalMin % 60;
    return m ? QStringLiteral("%1 h %2 min").arg(h).arg(m) : QStringLiteral("%1 h").arg(h);
}

QString humanDate(const QDateTime &dt)
{
    return QLocale(QLocale::English).toString(dt.date(), QStringLiteral("d MMM yyyy"));
}

QString cachePath(const QString &file)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + file;
}

// ---------------------------------------------------------------------------------- probe results

struct Probe {
    QString path;
    qint64 mtime = 0;
    qint64 size = 0;
    qint64 durationMs = 0;
    int width = 0, height = 0;
    QString codec;
    bool cached = false;
    bool ok = false;       // probed successfully
    bool bad = false;      // ffprobe rejected the file (not a media file / corrupt)
    bool video = true;     // has a video stream
    bool unstable = false; // size changed while probing
    bool probed = true;    // false: not probed yet (provisional values of a progressive first scan)
};

// ---------------------------------------------------------------------------------- directory walk

const QStringList &artworkExtensions()
{
    static const QStringList l = {"jpg", "jpeg", "png", "webp"};
    return l;
}

// One directory as read from disk. Rescans re-use the listings of directories the folder watcher did not
// report, so an auto-rescan only reads the folders that changed.
struct DirListing {
    QString canon;            // canonical path of the directory itself
    QFileInfoList entries;    // subdirectories and video files (not hidden), by name
    QSet<QString> images;     // artwork candidates (jpg/jpeg/png/webp) by exact file name
    bool volatileDir = false; // held a file that was still being written: always read again
};
using DirCache = QHash<QString, DirListing>; // absolute path as reached by the walk -> listing

// Scan state kept between the scans of one session (owned by the Library, used by one scan at a time).
struct ScanMemoryData {
    DirCache dirs;                 // listings of the last walk
    QHash<QString, Probe> probes;  // probe cache (mirrors probe-cache.json)
    bool probesLoaded = false;
};

DirListing listDir(const QString &dir, const QString &canon)
{
    DirListing l;
    l.canon = canon;
    const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable,
                                                          QDir::Name);
    for (const QFileInfo &fi : entries) {
        const QString name = fi.fileName();
        if (name.startsWith(QLatin1Char('.')))
            continue;
        if (fi.isDir()) {
            l.entries << fi;
        } else if (fi.isFile()) {
            const QString ext = fi.suffix().toLower();
            if (videoExtensions().contains(ext))
                l.entries << fi;
            else if (artworkExtensions().contains(ext))
                l.images.insert(name);
        }
    }
    return l;
}

struct FoundFile {
    QString path;   // absolute
    QString root;   // configured folder containing it
    QFileInfo info;
};

struct Walk {
    const DirCache *old = nullptr; // listings of the previous walk (empty: read everything)
    QSet<QString> dirty;           // folders to read again although listed before
    DirCache next;                 // listings of this walk
    QSet<QString> visitedDirs, seenFiles;
    QVector<FoundFile> out;
    QStringList newDirs;           // read for the first time although `old` was not empty
    int listed = 0;                // folders read from disk
    QDateTime now;
    ScanInfo *info = nullptr;
};

QString childPath(const QString &canonDir, const QString &name)
{
    return canonDir.endsWith(QLatin1Char('/')) ? canonDir + name : canonDir + QLatin1Char('/') + name;
}

// Canonical paths are derived from the parent's (only symlinks are resolved), so the walk costs one
// directory read per folder instead of a realpath() per entry.
void collectVideos(Walk &w, const QString &dir, const QString &canon, const QString &root, int depth)
{
    if (depth > 20)
        return;
    if (w.info->dirs.size() < kMaxWatchedDirs)
        w.info->dirs << dir;
    ++w.info->totalDirs;
    DirListing l;
    const auto cached = w.old->constFind(dir);
    if (cached != w.old->cend() && !cached->volatileDir && cached->canon == canon && !w.dirty.contains(dir)) {
        l = cached.value();
    } else {
        if (cached == w.old->cend() && !w.old->isEmpty())
            w.newDirs << dir;
        l = listDir(dir, canon);
        ++w.listed;
    }
    for (const QFileInfo &fi : std::as_const(l.entries)) {
        if (fi.isDir()) {
            const QString c = fi.isSymLink() ? fi.canonicalFilePath() : childPath(canon, fi.fileName());
            if (c.isEmpty() || w.visitedDirs.contains(c))
                continue;
            w.visitedDirs.insert(c);
            collectVideos(w, fi.absoluteFilePath(), c, root, depth + 1);
            continue;
        }
        if (fi.size() <= 0)
            continue;
        if (!fi.isReadable()) {
            ++w.info->rejected;
            continue;
        }
        // still being copied / downloaded: skip until it settles (the caller schedules a rescan)
        if (std::llabs(fi.lastModified().msecsTo(w.now)) < 5000) {
            ++w.info->unstable;
            l.volatileDir = true;
            continue;
        }
        static const QRegularExpression sample(QStringLiteral("(?<![A-Za-z0-9])sample(?![A-Za-z0-9])"),
                                               QRegularExpression::CaseInsensitiveOption);
        if (fi.size() < 300LL * 1024 * 1024 && sample.match(fi.completeBaseName()).hasMatch())
            continue;
        const QString c = fi.isSymLink() ? fi.canonicalFilePath() : childPath(canon, fi.fileName());
        if (c.isEmpty() || w.seenFiles.contains(c))
            continue;
        w.seenFiles.insert(c);
        w.out.append({fi.absoluteFilePath(), root, fi});
    }
    w.next.insert(dir, l);
}

// ---------------------------------------------------------------------------------- ffprobe

void runProbe(Probe &p, const QString &exe)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(exe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
                     QStringLiteral("v:0"), QStringLiteral("-show_entries"),
                     QStringLiteral("stream=codec_name,width,height:format=duration"), QStringLiteral("-of"),
                     QStringLiteral("json"), p.path});
    if (!proc.waitForStarted(5000))
        return;
    if (!proc.waitForFinished(10000)) { // hung on a broken / network file: give up, retry next scan
        proc.kill();
        proc.waitForFinished(1000);
        return;
    }
    if (proc.exitStatus() != QProcess::NormalExit)
        return;
    if (proc.exitCode() != 0) {
        p.bad = true;
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(proc.readAllStandardOutput()).object();
    const QJsonArray streams = o.value(QStringLiteral("streams")).toArray();
    p.video = !streams.isEmpty();
    if (!streams.isEmpty()) {
        const QJsonObject st = streams.first().toObject();
        p.codec = st.value(QStringLiteral("codec_name")).toString();
        p.width = st.value(QStringLiteral("width")).toInt();
        p.height = st.value(QStringLiteral("height")).toInt();
    }
    const QString dur = o.value(QStringLiteral("format")).toObject().value(QStringLiteral("duration")).toString();
    bool ok = false;
    const double sec = dur.toDouble(&ok);
    if (ok && sec > 0)
        p.durationMs = qint64(sec * 1000.0);
    p.ok = true;
}

// Almost all of an ffprobe run is process start-up (~30 ms, independent of -probesize / -analyzeduration on
// local files), so throughput comes from running several at once.
int probeThreads()
{
    return std::clamp(QThread::idealThreadCount(), 2, 8);
}

// Writes `data` to a temp file next to `path` and renames it over `path` (atomic replace, no fsync).
// Callers serialize writes per file.
bool writeFileAtomic(const QString &path, const QByteArray &data)
{
    const QString tmp = path + QStringLiteral(".tmp-%1").arg(QCoreApplication::applicationPid());
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = f.write(data) == data.size();
    f.close();
    if (!ok || std::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(path).constData()) != 0) {
        QFile::remove(tmp);
        return false;
    }
    return true;
}

void loadProbeCache(ScanMemoryData &mem)
{
    mem.probesLoaded = true;
    QFile f(cachePath(QStringLiteral("probe-cache.json")));
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject cache = QJsonDocument::fromJson(f.readAll()).object();
    mem.probes.reserve(cache.size());
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        const QJsonObject c = it.value().toObject();
        Probe p;
        p.path = it.key();
        p.mtime = qint64(c.value(QStringLiteral("m")).toDouble());
        p.size = qint64(c.value(QStringLiteral("s")).toDouble());
        p.durationMs = qint64(c.value(QStringLiteral("d")).toDouble());
        p.width = c.value(QStringLiteral("w")).toInt();
        p.height = c.value(QStringLiteral("h")).toInt();
        p.codec = c.value(QStringLiteral("c")).toString();
        p.bad = c.value(QStringLiteral("b")).toBool();
        p.video = c.value(QStringLiteral("v")).toBool(true);
        p.cached = true;
        p.ok = !p.bad;
        mem.probes.insert(p.path, p);
    }
}

void saveProbeCache(const QHash<QString, Probe> &probes)
{
    QJsonObject cache;
    for (const Probe &p : probes) {
        QJsonObject c;
        c.insert(QStringLiteral("m"), double(p.mtime));
        c.insert(QStringLiteral("s"), double(p.size));
        c.insert(QStringLiteral("d"), double(p.durationMs));
        c.insert(QStringLiteral("w"), p.width);
        c.insert(QStringLiteral("h"), p.height);
        c.insert(QStringLiteral("c"), p.codec);
        if (p.bad)
            c.insert(QStringLiteral("b"), true);
        if (!p.video)
            c.insert(QStringLiteral("v"), false);
        cache.insert(p.path, c);
    }
    writeFileAtomic(cachePath(QStringLiteral("probe-cache.json")), QJsonDocument(cache).toJson(QJsonDocument::Compact));
}

// ---------------------------------------------------------------------------------- grouping

struct ParsedFile {
    FoundFile ff;
    bool recording = false;
    bool isEpisode = false;
    bool isExtra = false;      // featurette inside a series folder (becomes season 0)
    int season = 0, episode = 0;
    QString episodeTitle;
    QString name;              // movie title or series name
    int year = 0;
    QString seriesDir;         // folder that holds the whole series (if any)
};

ParsedFile parseFile(const FoundFile &ff)
{
    ParsedFile pf;
    pf.ff = ff;
    const QString stem = ff.info.completeBaseName();
    const QDir dir = ff.info.dir();
    const QString dirName = dir.dirName();
    const bool dirIsRoot = QDir::cleanPath(dir.absolutePath()) == QDir::cleanPath(ff.root);
    const int folderSeason = dirIsRoot ? -1 : seasonFolderNumber(dirName);

    pf.recording = looksLikeRecording(stem);
    if (pf.recording) {
        pf.name = cleanRecordingTitle(stem);
        return pf;
    }

    bool hadGroup = false;
    const QString s = stripLeadingJunk(sceneToSpaces(stem), &hadGroup);
    EpMatch em = findEpisodeMarker(s);

    if (!em.ok && folderSeason >= 0) {
        static const QRegularExpression epWord(
            QStringLiteral("(?:^|[^A-Za-z0-9])(?:e|ep\\.?|episode)\\s*(\\d{1,3})(?![0-9])"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression leadingNum(QStringLiteral("^(\\d{1,3})(?![0-9])"));
        auto m = epWord.match(s);
        if (!m.hasMatch())
            m = leadingNum.match(s);
        if (m.hasMatch()) {
            em.ok = true;
            em.season = folderSeason;
            em.episode = m.captured(1).toInt();
            em.start = int(m.capturedStart());
            em.end = int(m.capturedEnd());
        }
    }
    if (!em.ok && hadGroup) { // "[Group] Show Name - 05 [1080p]"
        static const QRegularExpression anime(
            QStringLiteral("\\s[-\\x{2013}]\\s(\\d{1,3})(?:v\\d)?(?=\\s*(?:[\\[\\(]|$))"));
        auto m = anime.match(s);
        if (m.hasMatch()) {
            em.ok = true;
            em.season = 1;
            em.episode = m.captured(1).toInt();
            em.start = int(m.capturedStart());
            em.end = int(m.capturedEnd());
        }
    }

    if (em.ok) {
        pf.isEpisode = true;
        pf.season = em.season;
        pf.episode = em.episode;
        pf.episodeTitle = cleanEpisodeTitle(s.mid(em.end));
        const NameYear fromFile = em.start > 0 ? cleanTitle(s.left(em.start)) : NameYear{};

        // where does the series live?
        QString seriesDir;
        if (!dirIsRoot) {
            if (folderSeason >= 0) {
                QDir p = dir;
                p.cdUp();
                if (QDir::cleanPath(p.absolutePath()) != QDir::cleanPath(ff.root))
                    seriesDir = p.absolutePath();
            } else {
                // a plain folder is the series' own folder only if its name matches the series
                // ("Anime/[Grp] Frieren - 05.mkv": "Anime" is a category, not the show folder)
                const NameYear f = seriesFromFolder(dirName);
                const QString a = normalizeKey(f.name), b = normalizeKey(fromFile.name);
                if (b.isEmpty() || (!a.isEmpty() && (a == b || a.startsWith(b + QLatin1Char(' '))
                                                     || b.startsWith(a + QLatin1Char(' ')))))
                    seriesDir = dir.absolutePath();
            }
        }
        pf.seriesDir = seriesDir;
        NameYear fromFolder;
        if (!seriesDir.isEmpty())
            fromFolder = seriesFromFolder(QFileInfo(seriesDir).fileName());

        if (folderSeason >= 0 && !fromFolder.name.isEmpty()) {
            pf.name = fromFolder.name;
            pf.year = fromFolder.year ? fromFolder.year : fromFile.year;
        } else if (!fromFile.name.isEmpty()) {
            pf.name = fromFile.name;
            pf.year = fromFile.year;
            if (!pf.year && normalizeKey(fromFolder.name) == normalizeKey(fromFile.name))
                pf.year = fromFolder.year;
            if (normalizeKey(fromFolder.name) == normalizeKey(fromFile.name) && fromFolder.name.size() > 0
                && fromFolder.name != fromFile.name && fromFile.name == fromFile.name.toLower())
                pf.name = fromFolder.name;
        } else if (!fromFolder.name.isEmpty()) {
            pf.name = fromFolder.name;
            pf.year = fromFolder.year;
        } else {
            pf.name = cleanTitle(s).name;
        }
        return pf;
    }

    const NameYear ny = cleanTitle(s);
    pf.name = ny.name;
    pf.year = ny.year;
    return pf;
}

// artwork lookup against the directory listings of the walk (falls back to the disk for unlisted folders)
QString findArtwork(const DirCache &listings, const QStringList &dirs, const QStringList &baseNames)
{
    for (const QString &d : dirs) {
        if (d.isEmpty())
            continue;
        const auto l = listings.constFind(d);
        for (const QString &b : baseNames)
            for (const QString &e : artworkExtensions()) {
                const QString name = b + QLatin1Char('.') + e;
                if (l != listings.cend() ? l->images.contains(name) : QFileInfo::exists(d + QLatin1Char('/') + name))
                    return d + QLatin1Char('/') + name;
            }
    }
    return {};
}

QString categoryFor(const QString &root, const QString &containerDir)
{
    const QString rootClean = QDir::cleanPath(root);
    QString rootName = QFileInfo(rootClean).fileName();
    if (rootName.isEmpty())
        rootName = QStringLiteral("Library");
    const QString rel = QDir(rootClean).relativeFilePath(containerDir);
    if (rel.isEmpty() || rel == QLatin1String(".") || rel.startsWith(QLatin1String("..")))
        return rootName;
    return rel.section(QLatin1Char('/'), 0, 0);
}

void fillCosmetics(Title &t, bool recording, const QString &rootName)
{
    static const QStringList ratings = {"TV-MA", "TV-14", "PG-13", "R", "PG"};
    static const QStringList pool = {"Drama", "Thriller", "Action", "Sci-Fi", "Comedy",
                                     "Suspenseful", "Gritty", "Cerebral", "Exciting", "Mystery"};
    const quint32 seed = seedOf(t.id);
    t.rating = ratings.at(seed % ratings.size());
    t.match = 90 + int((seed >> 8) % 10);

    QStringList g;
    if (t.isSeries)
        g << QStringLiteral("TV Show");
    if (recording)
        g << QStringLiteral("Screen Recording");
    if (!t.category.isEmpty() && t.category != rootName && !g.contains(t.category))
        g << t.category;
    const int want = recording ? 2 : 3;
    quint32 s = seed >> 12;
    int guard = 0;
    while (g.size() < want && guard++ < 40) {
        const QString pick = pool.at(s % pool.size());
        s = s * 1103515245u + 12345u;
        if (!g.contains(pick))
            g << pick;
    }
    t.genres = g;
}

void fillDescription(Title &t, bool recording, const QString &rootName)
{
    const QString where = t.category.isEmpty() ? rootName : t.category;
    if (t.isSeries) {
        QList<int> seasons;
        int regular = 0, extras = 0;
        qint64 total = 0;
        for (const MediaFile &f : t.files) {
            total += f.durationMs;
            if (f.season == 0) {
                ++extras;
                continue;
            }
            ++regular;
            if (!seasons.contains(f.season))
                seasons << f.season;
        }
        std::sort(seasons.begin(), seasons.end());
        QStringList facts;
        if (!seasons.isEmpty())
            facts << (seasons.size() == 1 ? QStringLiteral("1 season") : QStringLiteral("%1 seasons").arg(seasons.size()));
        if (regular)
            facts << (regular == 1 ? QStringLiteral("1 episode") : QStringLiteral("%1 episodes").arg(regular));
        if (extras)
            facts << (extras == 1 ? QStringLiteral("1 extra") : QStringLiteral("%1 extras").arg(extras));
        if (total > 0)
            facts << QStringLiteral("%1 in total").arg(humanDuration(total));

        QString seasonText;
        if (seasons.size() == 1)
            seasonText = QStringLiteral("Season %1 of ").arg(seasons.first());
        else if (seasons.size() > 1) {
            bool contiguous = seasons.last() - seasons.first() + 1 == seasons.size();
            if (contiguous)
                seasonText = QStringLiteral("Seasons %1–%2 of ").arg(seasons.first()).arg(seasons.last());
            else {
                QStringList n;
                for (int x : seasons) n << QString::number(x);
                seasonText = QStringLiteral("Seasons %1 of ").arg(n.join(QStringLiteral(", ")));
            }
        } else {
            seasonText = QStringLiteral("Specials of ");
        }
        const MediaFile &f = t.files.first();
        QString tech = resolutionLabel(f.width, f.height);
        const QString codec = prettyCodec(f.videoCodec);
        if (!codec.isEmpty())
            tech = tech.isEmpty() ? codec : tech + QLatin1Char(' ') + codec;
        QString line2 = seasonText + t.title + (t.year ? QStringLiteral(" (%1)").arg(t.year) : QString());
        if (!tech.isEmpty())
            line2 += QStringLiteral(", ") + tech;
        line2 += QStringLiteral(". In your %1 folder. Added %2.").arg(where, humanDate(t.added));
        t.description = facts.join(QStringLiteral(" · ")) + QLatin1Char('\n') + line2;
        return;
    }

    const MediaFile &f = t.files.first();
    QStringList facts;
    if (t.year)
        facts << QString::number(t.year);
    if (f.durationMs > 0)
        facts << humanDuration(f.durationMs);
    if (recording && f.width > 0)
        facts << QStringLiteral("%1×%2").arg(f.width).arg(f.height);
    else if (!resolutionLabel(f.width, f.height).isEmpty())
        facts << resolutionLabel(f.width, f.height);
    if (!prettyCodec(f.videoCodec).isEmpty())
        facts << prettyCodec(f.videoCodec);

    QString line2;
    if (recording)
        line2 = QStringLiteral("Screen recording captured %1, kept in your %2 folder.").arg(humanDate(f.modified), where);
    else if (t.year)
        line2 = QStringLiteral("%1 (%2), from your %3 folder. Added %4.").arg(t.title).arg(t.year).arg(where, humanDate(t.added));
    else
        line2 = QStringLiteral("A video from your %1 folder. Added %2.").arg(where, humanDate(t.added));
    t.description = facts.isEmpty() ? line2 : facts.join(QStringLiteral(" · ")) + QLatin1Char('\n') + line2;
}

bool episodeLess(const MediaFile &a, const MediaFile &b)
{
    const int sa = a.season == 0 ? 100000 : a.season; // specials/extras go last
    const int sb = b.season == 0 ? 100000 : b.season;
    if (sa != sb) return sa < sb;
    if (a.episode != b.episode) return a.episode < b.episode;
    return QString::compare(a.path, b.path, Qt::CaseInsensitive) < 0;
}

// Groups parsed files into titles. `parsedAll` is sorted by path (case-insensitive); files whose probe says
// "not a video" / broken / still growing are left out (counted in `info` when given).
QVector<Title> buildTitles(const QVector<ParsedFile> &parsedAll, const QHash<QString, Probe> &probes,
                           const DirCache &listings, ScanInfo *info)
{
    QVector<ParsedFile> parsed;
    parsed.reserve(parsedAll.size());
    for (const ParsedFile &pf : parsedAll) {
        const auto p = probes.constFind(pf.ff.path);
        if (p != probes.cend() && p->unstable) {
            if (info) ++info->unstable;
            continue;
        }
        if (p != probes.cend() && (p->bad || (p->ok && !p->video))) {
            if (info) ++info->rejected;
            continue;
        }
        parsed.append(pf);
    }

    // series folders known from real episodes -> featurettes / extras inside them join the series
    QHash<QString, int> seriesDirOwner; // dir -> index of an episode file
    QHash<QString, int> extraCounter;
    for (int i = 0; i < parsed.size(); ++i)
        if (parsed[i].isEpisode && !parsed[i].seriesDir.isEmpty() && !seriesDirOwner.contains(parsed[i].seriesDir))
            seriesDirOwner.insert(parsed[i].seriesDir, i);
    for (ParsedFile &pf : parsed) {
        if (pf.isEpisode)
            continue;
        const QDir dir = pf.ff.info.dir();
        if (!isExtrasFolder(dir.dirName()))
            continue;
        QDir parent = dir;
        parent.cdUp();
        QString owner = parent.absolutePath();
        if (!seriesDirOwner.contains(owner)) { // Show/Season 1/Extras
            QDir gp = parent;
            gp.cdUp();
            if (seasonFolderNumber(parent.dirName()) >= 0 && seriesDirOwner.contains(gp.absolutePath()))
                owner = gp.absolutePath();
            else
                continue;
        }
        const ParsedFile &ep = parsed.at(seriesDirOwner.value(owner));
        pf.isEpisode = true;
        pf.isExtra = true;
        pf.season = 0;
        pf.episode = ++extraCounter[owner];
        pf.episodeTitle = cleanExtraTitle(pf.ff.info.completeBaseName());
        pf.name = ep.name;
        pf.year = ep.year;
        pf.seriesDir = owner;
        pf.recording = false;
    }

    // 4) group series by normalized name (+year); merge year-less keys into a unique year-ful sibling
    QHash<QString, QVector<int>> seriesGroups;
    QHash<QString, QSet<int>> yearsByName;
    for (int i = 0; i < parsed.size(); ++i) {
        const ParsedFile &pf = parsed[i];
        if (!pf.isEpisode)
            continue;
        const QString n = normalizeKey(pf.name);
        if (pf.year)
            yearsByName[n].insert(pf.year);
    }
    for (int i = 0; i < parsed.size(); ++i) {
        ParsedFile &pf = parsed[i];
        if (!pf.isEpisode)
            continue;
        const QString n = normalizeKey(pf.name);
        int y = pf.year;
        if (!y && yearsByName.value(n).size() == 1)
            y = *yearsByName.value(n).begin();
        pf.year = y;
        seriesGroups[QStringLiteral("series:%1:%2").arg(n).arg(y)].append(i);
    }

    QVector<Title> titles;
    auto makeFile = [&](const ParsedFile &pf) {
        MediaFile mf;
        mf.path = pf.ff.path;
        mf.season = pf.isEpisode ? pf.season : 0;
        mf.episode = pf.isEpisode ? pf.episode : 0;
        mf.episodeTitle = pf.episodeTitle;
        const auto pr = probes.constFind(pf.ff.path);
        if (pr != probes.cend()) {
            mf.durationMs = pr->durationMs;
            mf.width = pr->width;
            mf.height = pr->height;
            mf.videoCodec = pr->codec;
            mf.probed = pr->probed;
        }
        mf.modified = pf.ff.info.lastModified();
        return mf;
    };
    auto rootNameOf = [](const QString &root) {
        const QString n = QFileInfo(QDir::cleanPath(root)).fileName();
        return n.isEmpty() ? QStringLiteral("Library") : n;
    };

    for (auto it = seriesGroups.cbegin(); it != seriesGroups.cend(); ++it) {
        const QVector<int> &idx = it.value();
        Title t;
        t.id = sha1Id(it.key());
        t.isSeries = true;
        const ParsedFile &first = parsed[idx.first()];
        // prefer the most common spelling of the name (prefer one with capitals)
        QHash<QString, int> spellings;
        for (int i : idx)
            if (!parsed[i].isExtra)
                spellings[parsed[i].name]++;
        QString best = first.name;
        int bestCount = -1;
        for (auto s = spellings.cbegin(); s != spellings.cend(); ++s)
            if (s.value() > bestCount || (s.value() == bestCount && s.key() < best)) {
                best = s.key();
                bestCount = s.value();
            }
        t.title = best;
        t.year = first.year;
        QString seriesDir;
        for (int i : idx) {
            t.files.append(makeFile(parsed[i]));
            if (seriesDir.isEmpty())
                seriesDir = parsed[i].seriesDir;
            if (!t.added.isValid() || parsed[i].ff.info.lastModified() > t.added)
                t.added = parsed[i].ff.info.lastModified();
        }
        std::sort(t.files.begin(), t.files.end(), episodeLess);
        const QString root = first.ff.root;
        QString container;
        if (!seriesDir.isEmpty()) {
            QDir p(seriesDir);
            p.cdUp();
            container = p.absolutePath();
        } else {
            container = first.ff.info.absolutePath();
        }
        t.category = categoryFor(root, container);
        QStringList artDirs;
        if (!seriesDir.isEmpty())
            artDirs << seriesDir;
        const QString fileDir = first.ff.info.absolutePath();
        if (fileDir != seriesDir && QDir::cleanPath(fileDir) != QDir::cleanPath(root))
            artDirs << fileDir;
        if (!seriesDir.isEmpty()) {
            t.posterFile = findArtwork(listings, artDirs, {"poster", "folder", "cover", "show"});
            t.backdropFile = findArtwork(listings, artDirs, {"fanart", "backdrop", "background"});
        }
        if (t.posterFile.isEmpty()) {
            const QString stem = QFileInfo(t.files.first().path).completeBaseName();
            t.posterFile = findArtwork(listings, {fileDir}, {stem + QStringLiteral("-poster"), stem});
        }
        if (t.backdropFile.isEmpty()) {
            const QString stem = QFileInfo(t.files.first().path).completeBaseName();
            t.backdropFile = findArtwork(listings, {fileDir}, {stem + QStringLiteral("-fanart")});
        }
        fillCosmetics(t, false, rootNameOf(root));
        fillDescription(t, false, rootNameOf(root));
        titles.append(t);
    }

    // movies: count main videos per dir to decide if a folder is the movie's own folder
    QHash<QString, int> videosPerDir;
    for (const ParsedFile &pf : parsed)
        if (!pf.isEpisode)
            videosPerDir[pf.ff.info.absolutePath()]++;

    for (const ParsedFile &pf : parsed) {
        if (pf.isEpisode)
            continue;
        Title t;
        t.id = sha1Id(pf.ff.path);
        t.title = pf.name;
        t.year = pf.year;
        t.isSeries = false;
        t.files.append(makeFile(pf));
        t.added = pf.ff.info.lastModified();
        const QString dir = pf.ff.info.absolutePath();
        const bool dirIsRoot = QDir::cleanPath(dir) == QDir::cleanPath(pf.ff.root);
        bool ownFolder = false;
        if (!dirIsRoot && videosPerDir.value(dir) == 1 && !pf.recording) {
            const NameYear folder = cleanTitle(stripLeadingJunk(sceneToSpaces(QFileInfo(dir).fileName())));
            const QString a = normalizeKey(folder.name), b = normalizeKey(pf.name);
            if (!a.isEmpty() && (a == b || a.startsWith(b) || b.startsWith(a))) {
                ownFolder = true;
                if (!t.year)
                    t.year = folder.year;
            } else if (folder.year && !pf.year) {
                // "Inception (2010)/movie.mkv": the folder is the better name
                ownFolder = true;
                t.title = folder.name;
                t.year = folder.year;
            }
        }
        QString container = dir;
        if (ownFolder) {
            QDir p(dir);
            p.cdUp();
            container = p.absolutePath();
        }
        t.category = categoryFor(pf.ff.root, container);
        const QString stem = pf.ff.info.completeBaseName();
        QStringList posterNames = {stem + QStringLiteral("-poster"), stem};
        QStringList backdropNames = {stem + QStringLiteral("-fanart"), stem + QStringLiteral("-backdrop")};
        if (ownFolder) {
            posterNames << QStringLiteral("poster") << QStringLiteral("folder") << QStringLiteral("cover");
            backdropNames << QStringLiteral("fanart") << QStringLiteral("backdrop") << QStringLiteral("background");
        }
        t.posterFile = findArtwork(listings, {dir}, posterNames);
        t.backdropFile = findArtwork(listings, {dir}, backdropNames);
        fillCosmetics(t, pf.recording, rootNameOf(pf.ff.root));
        fillDescription(t, pf.recording, rootNameOf(pf.ff.root));
        titles.append(t);
    }
    return titles;
}

// What one scan is asked to do (built on the GUI thread).
struct ScanJob {
    QStringList folders;
    bool full = true;              // read every folder (else re-use listings of folders not in `dirty`)
    QSet<QString> dirty;           // folders the watcher reported since the last scan
    QHash<QString, Title> known;   // titles of the last scan: provisional values for a progressive publish
};
using Publish = std::function<void(QVector<Title> &&)>;
constexpr int kProgressiveMin = 24;   // unprobed files needed before titles are published ahead of ffprobe
constexpr int kPublishEveryMs = 1500; // while probing, publish what arrived at most this often

QVector<Title> scanFolders(const ScanJob &job, ScanMemoryData &mem, ScanInfo *info, const Publish &publish)
{
    QElapsedTimer timer;
    timer.start();
    // 1) walk (deepest configured roots first so they claim their files)
    QStringList roots;
    for (const QString &f : job.folders) {
        const QString c = QDir::cleanPath(f);
        if (QFileInfo(c).isDir() && !roots.contains(c))
            roots << c;
    }
    std::sort(roots.begin(), roots.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
    const DirCache none;
    Walk w;
    w.old = job.full ? &none : &mem.dirs;
    w.dirty = job.dirty;
    w.now = QDateTime::currentDateTime();
    w.info = info;
    for (const QString &r : roots) {
        const QString canon = QFileInfo(r).canonicalFilePath();
        if (canon.isEmpty() || w.visitedDirs.contains(canon))
            continue;
        w.visitedDirs.insert(canon);
        collectVideos(w, r, canon, r, 0);
    }
    mem.dirs = std::move(w.next);
    info->listedDirs = w.listed;
    info->newDirs = w.newDirs;
    info->walkMs = timer.elapsed();

    // 2) probe cache lookup
    if (!mem.probesLoaded)
        loadProbeCache(mem);
    QHash<QString, Probe> probes;
    probes.reserve(w.out.size());
    QVector<Probe> todo;
    for (const FoundFile &ff : std::as_const(w.out)) {
        Probe p;
        p.path = ff.path;
        p.mtime = ff.info.lastModified().toMSecsSinceEpoch();
        p.size = ff.info.size();
        const auto c = mem.probes.constFind(ff.path);
        if (c != mem.probes.cend() && c->mtime == p.mtime && c->size == p.size)
            probes.insert(p.path, c.value());
        else
            todo.append(p);
    }

    // 3) parse once (grouping is redone for every published result)
    QVector<ParsedFile> parsed;
    parsed.reserve(w.out.size());
    for (const FoundFile &ff : std::as_const(w.out))
        parsed.append(parseFile(ff));
    std::sort(parsed.begin(), parsed.end(), [](const ParsedFile &a, const ParsedFile &b) {
        return QString::compare(a.ff.path, b.ff.path, Qt::CaseInsensitive) < 0;
    });

    // 4) probe new / changed files in parallel. Many of them (first scan, new folder): publish the titles
    //    right away (durations of unchanged files from the last scan, else unknown) and then every 1.5 s.
    const QString exe = todo.isEmpty() ? QString() : QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    const bool progressive = publish && !exe.isEmpty() && todo.size() >= kProgressiveMin;
    for (const Probe &p : std::as_const(todo)) {
        Probe pending = p;
        pending.probed = !progressive; // without ffprobe the zeros are final
        probes.insert(p.path, pending);
    }
    if (progressive) {
        QHash<QString, const MediaFile *> knownFiles;
        for (const Title &t : job.known)
            for (const MediaFile &f : t.files)
                knownFiles.insert(f.path, &f);
        for (const Probe &p : std::as_const(todo)) {
            const MediaFile *f = knownFiles.value(p.path);
            if (!f || f->modified.toMSecsSinceEpoch() != p.mtime)
                continue;
            Probe &q = probes[p.path];
            q.durationMs = f->durationMs;
            q.width = f->width;
            q.height = f->height;
            q.codec = f->videoCodec;
        }
        publish(buildTitles(parsed, probes, mem.dirs, nullptr));
    }
    bool cacheDirty = false;
    if (!exe.isEmpty() && !todo.isEmpty()) {
        QThreadPool pool;
        pool.setMaxThreadCount(probeThreads());
        QMutex doneMutex;
        QVector<int> done;  // indices into todo, finished since the last publish
        QSemaphore finishedSem;
        Probe *base = todo.data();
        for (int i = 0; i < todo.size(); ++i) {
            pool.start([base, i, &exe, &doneMutex, &done, &finishedSem] {
                Probe &p = base[i];
                runProbe(p, exe);
                // a file whose size changed while we probed it is still being written
                if (QFileInfo(p.path).size() != p.size)
                    p.unstable = true;
                {
                    QMutexLocker l(&doneMutex);
                    done << i;
                }
                finishedSem.release();
            });
        }
        int finished = 0;
        QElapsedTimer sincePublish;
        sincePublish.start();
        while (finished < todo.size()) {
            if (finishedSem.tryAcquire(1, 200))
                ++finished;
            if (!progressive || finished == todo.size() || sincePublish.elapsed() < kPublishEveryMs)
                continue;
            QVector<int> batch;
            {
                QMutexLocker l(&doneMutex);
                batch.swap(done);
            }
            for (int i : std::as_const(batch))
                probes.insert(todo.at(i).path, todo.at(i));
            if (!batch.isEmpty())
                publish(buildTitles(parsed, probes, mem.dirs, nullptr));
            sincePublish.restart();
        }
        pool.waitForDone();
        for (const Probe &p : std::as_const(todo)) {
            probes.insert(p.path, p);
            if (p.unstable) { // read its folder again next time
                const auto l = mem.dirs.find(QFileInfo(p.path).absolutePath());
                if (l != mem.dirs.end())
                    l->volatileDir = true;
            }
        }
        cacheDirty = true;
        info->probed = int(todo.size());
    }
    info->probeMs = timer.elapsed() - info->walkMs;

    // 5) probe cache: entries of files outside the scanned folders are kept (a folder may be re-added later);
    //    inside them, the walk just showed which files still exist
    QHash<QString, Probe> keep;
    keep.reserve(probes.size());
    for (const Probe &p : std::as_const(probes))
        if ((p.ok || p.bad) && !p.unstable && p.probed)
            keep.insert(p.path, p);
    QStringList rootPrefixes;
    for (const QString &r : std::as_const(roots))
        rootPrefixes << (r.endsWith(QLatin1Char('/')) ? r : r + QLatin1Char('/'));
    QVector<QString> outside;
    for (auto it = mem.probes.cbegin(); it != mem.probes.cend(); ++it) {
        if (keep.contains(it.key()))
            continue;
        const bool inside = std::any_of(rootPrefixes.cbegin(), rootPrefixes.cend(),
                                        [&](const QString &r) { return it.key().startsWith(r); });
        if (inside)
            cacheDirty = true; // gone (or changed and not probed successfully)
        else if (keep.size() + outside.size() < 20000)
            outside << it.key();
    }
    if (cacheDirty) {
        for (const QString &path : std::as_const(outside))
            if (QFileInfo::exists(path))
                keep.insert(path, mem.probes.value(path));
        saveProbeCache(keep);
    } else {
        for (const QString &path : std::as_const(outside))
            keep.insert(path, mem.probes.value(path));
    }
    mem.probes = std::move(keep);

    // 6) group
    const QVector<Title> titles = buildTitles(parsed, probes, mem.dirs, info);
    info->ms = timer.elapsed();
    return titles;
}

QString normPath(const QString &p)
{
    if (p.startsWith(QLatin1String("file:")))
        return QUrl(p).toLocalFile();
    return p;
}

QString languageLabel(const QString &tag)
{
    static const QHash<QString, QString> map = {
        {"en", "English"}, {"eng", "English"}, {"english", "English"},
        {"de", "German"}, {"ger", "German"}, {"deu", "German"}, {"german", "German"},
        {"fr", "French"}, {"fre", "French"}, {"fra", "French"}, {"french", "French"},
        {"es", "Spanish"}, {"spa", "Spanish"}, {"spanish", "Spanish"},
        {"it", "Italian"}, {"ita", "Italian"}, {"italian", "Italian"},
        {"pt", "Portuguese"}, {"por", "Portuguese"}, {"portuguese", "Portuguese"},
        {"sq", "Albanian"}, {"alb", "Albanian"}, {"sqi", "Albanian"}, {"albanian", "Albanian"},
        {"nl", "Dutch"}, {"dut", "Dutch"}, {"nld", "Dutch"}, {"dutch", "Dutch"},
        {"ru", "Russian"}, {"rus", "Russian"}, {"russian", "Russian"},
        {"ja", "Japanese"}, {"jpn", "Japanese"}, {"japanese", "Japanese"},
        {"zh", "Chinese"}, {"chi", "Chinese"}, {"zho", "Chinese"}, {"chinese", "Chinese"},
        {"ko", "Korean"}, {"kor", "Korean"}, {"korean", "Korean"},
        {"ar", "Arabic"}, {"ara", "Arabic"}, {"arabic", "Arabic"},
        {"tr", "Turkish"}, {"tur", "Turkish"}, {"turkish", "Turkish"},
        {"pl", "Polish"}, {"pol", "Polish"}, {"polish", "Polish"},
        {"sv", "Swedish"}, {"swe", "Swedish"}, {"swedish", "Swedish"},
        {"sr", "Serbian"}, {"srp", "Serbian"}, {"serbian", "Serbian"},
        {"hr", "Croatian"}, {"hrv", "Croatian"}, {"croatian", "Croatian"},
        {"el", "Greek"}, {"gre", "Greek"}, {"ell", "Greek"}, {"greek", "Greek"},
        {"ro", "Romanian"}, {"rum", "Romanian"}, {"ron", "Romanian"}, {"romanian", "Romanian"},
        {"hu", "Hungarian"}, {"hun", "Hungarian"}, {"hungarian", "Hungarian"},
    };
    return map.value(tag.toLower());
}

// "en.forced" / "2_English" / "eng.sdh" -> "English (Forced)"; empty if no language found
QString subtitleLabel(const QString &tagPart)
{
    const QStringList toks = tagPart.split(QRegularExpression(QStringLiteral("[._ \\-\\[\\]\\(\\)]+")), Qt::SkipEmptyParts);
    QString lang, code;
    QStringList flags;
    for (const QString &tk : toks) {
        const QString l = tk.toLower();
        if (l == QLatin1String("forced")) flags << QStringLiteral("Forced");
        else if (l == QLatin1String("sdh") || l == QLatin1String("cc") || l == QLatin1String("hi")) flags << QStringLiteral("SDH");
        else if (lang.isEmpty() && !languageLabel(l).isEmpty()) lang = languageLabel(l);
        else if (code.isEmpty() && (l.size() == 2 || l.size() == 3) && tk.at(0).isLetter()
                 && std::all_of(l.cbegin(), l.cend(), [](QChar c) { return c.isLetter(); }))
            code = tk.toUpper();
    }
    QString label = !lang.isEmpty() ? lang : code;
    if (label.isEmpty() && flags.isEmpty())
        return {};
    if (label.isEmpty())
        label = QStringLiteral("Subtitles");
    if (!flags.isEmpty())
        label += QStringLiteral(" (%1)").arg(flags.join(QStringLiteral(", ")));
    return label;
}

bool sameFile(const MediaFile &a, const MediaFile &b)
{
    return a.path == b.path && a.season == b.season && a.episode == b.episode && a.episodeTitle == b.episodeTitle
           && a.durationMs == b.durationMs && a.width == b.width && a.height == b.height
           && a.videoCodec == b.videoCodec && a.modified == b.modified;
}

bool sameTitle(const Title &a, const Title &b)
{
    if (a.id != b.id || a.title != b.title || a.year != b.year || a.isSeries != b.isSeries || a.category != b.category
        || a.description != b.description || a.rating != b.rating || a.genres != b.genres || a.match != b.match
        || a.posterFile != b.posterFile || a.backdropFile != b.backdropFile || a.logoFile != b.logoFile
        || a.hasExternalMeta != b.hasExternalMeta || a.added != b.added || a.files.size() != b.files.size())
        return false;
    for (int i = 0; i < a.files.size(); ++i)
        if (!sameFile(a.files.at(i), b.files.at(i)))
            return false;
    return true;
}

// ---------------------------------------------------------------------------------- scan cache (JSON)

QJsonObject titleToJson(const Title &t)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), t.id);
    o.insert(QStringLiteral("title"), t.title);
    o.insert(QStringLiteral("year"), t.year);
    o.insert(QStringLiteral("series"), t.isSeries);
    o.insert(QStringLiteral("category"), t.category);
    o.insert(QStringLiteral("description"), t.description);
    o.insert(QStringLiteral("rating"), t.rating);
    o.insert(QStringLiteral("genres"), QJsonArray::fromStringList(t.genres));
    o.insert(QStringLiteral("match"), t.match);
    o.insert(QStringLiteral("poster"), t.posterFile);
    o.insert(QStringLiteral("backdrop"), t.backdropFile);
    o.insert(QStringLiteral("added"), double(t.added.toMSecsSinceEpoch()));
    QJsonArray files;
    for (const MediaFile &f : t.files) {
        QJsonObject fo;
        fo.insert(QStringLiteral("p"), f.path);
        fo.insert(QStringLiteral("s"), f.season);
        fo.insert(QStringLiteral("e"), f.episode);
        fo.insert(QStringLiteral("t"), f.episodeTitle);
        fo.insert(QStringLiteral("d"), double(f.durationMs));
        fo.insert(QStringLiteral("w"), f.width);
        fo.insert(QStringLiteral("h"), f.height);
        fo.insert(QStringLiteral("c"), f.videoCodec);
        fo.insert(QStringLiteral("m"), double(f.modified.toMSecsSinceEpoch()));
        files.append(fo);
    }
    o.insert(QStringLiteral("files"), files);
    return o;
}

Title titleFromJson(const QJsonObject &o)
{
    Title t;
    t.id = o.value(QStringLiteral("id")).toString();
    t.title = o.value(QStringLiteral("title")).toString();
    t.year = o.value(QStringLiteral("year")).toInt();
    t.isSeries = o.value(QStringLiteral("series")).toBool();
    t.category = o.value(QStringLiteral("category")).toString();
    t.description = o.value(QStringLiteral("description")).toString();
    t.rating = o.value(QStringLiteral("rating")).toString();
    for (const QJsonValue &g : o.value(QStringLiteral("genres")).toArray())
        t.genres << g.toString();
    t.match = o.value(QStringLiteral("match")).toInt(95);
    t.posterFile = o.value(QStringLiteral("poster")).toString();
    t.backdropFile = o.value(QStringLiteral("backdrop")).toString();
    t.added = QDateTime::fromMSecsSinceEpoch(qint64(o.value(QStringLiteral("added")).toDouble()));
    for (const QJsonValue &v : o.value(QStringLiteral("files")).toArray()) {
        const QJsonObject fo = v.toObject();
        MediaFile f;
        f.path = fo.value(QStringLiteral("p")).toString();
        f.season = fo.value(QStringLiteral("s")).toInt();
        f.episode = fo.value(QStringLiteral("e")).toInt();
        f.episodeTitle = fo.value(QStringLiteral("t")).toString();
        f.durationMs = qint64(fo.value(QStringLiteral("d")).toDouble());
        f.width = fo.value(QStringLiteral("w")).toInt();
        f.height = fo.value(QStringLiteral("h")).toInt();
        f.videoCodec = fo.value(QStringLiteral("c")).toString();
        f.modified = QDateTime::fromMSecsSinceEpoch(qint64(fo.value(QStringLiteral("m")).toDouble()));
        if (!f.path.isEmpty())
            t.files.append(f);
    }
    return t;
}

QString episodeKey(const QString &id, int season, int episode)
{
    return QStringLiteral("%1/%2/%3").arg(id).arg(season).arg(episode);
}

const QStringList &overlayFileKeys()
{
    static const QStringList keys = {QStringLiteral("posterFile"), QStringLiteral("backdropFile"),
                                     QStringLiteral("logoFile"), QStringLiteral("stillFile")};
    return keys;
}

} // namespace

struct Library::ScanExtras : ScanInfo {};
struct Library::ScanMemory : ScanMemoryData {};
struct Library::ScanResult {
    enum Kind { OverlayCheck, Partial, Final } kind = Final;
    QVector<Title> titles;
    QSet<QString> brokenOverlays;               // OverlayCheck: titles whose restored overlays miss files
    std::shared_ptr<Library::OverlayCheck> check;
};

namespace {
constexpr int kProgressSaveMs = 30000; // playback progress ticks: watch state written at most this often
constexpr int kStateSaveMs = 2000;     // explicit actions (my list, watched, cleared)
constexpr int kCacheSaveMs = 1500;     // scan cache debounce ...
constexpr int kCacheSaveMaxWaitMs = 10000; // ... but written at least this often while overlays stream in
constexpr int kRefreshMs = 250;        // metadata overlays: model refreshes coalesced this long
constexpr int kBulkRefresh = 50;       // more changed titles than this: one refreshAll() per model
const QList<int> &progressRoles()
{
    static const QList<int> roles = {TitleModel::PathRole, TitleModel::SourceUrlRole, TitleModel::DurationMsRole,
                                     TitleModel::PositionMsRole, TitleModel::ProgressRole};
    return roles;
}
bool isRecentDate(const QDateTime &added, qint64 nowMs)
{
    return added.isValid() && nowMs - added.toMSecsSinceEpoch() < 7LL * 24 * 3600 * 1000;
}
bool hasUnprobed(const Title &t)
{
    return std::any_of(t.files.cbegin(), t.files.cend(), [](const MediaFile &f) { return !f.probed; });
}
} // namespace

// =====================================================================================================
//  Library
// =====================================================================================================

Library::Library(QObject *parent)
    : QObject(parent)
{
    s_instance = this;
    m_all = new TitleModel(this, this);
    m_movies = new TitleModel(this, this);
    m_series = new TitleModel(this, this);
    m_myList = new TitleModel(this, this);
    m_continue = new TitleModel(this, this);
    m_search = new TitleModel(this, this);
    m_homeRows = new RowsModel(this);
    m_movieRows = new RowsModel(this);
    m_seriesRows = new RowsModel(this);
    m_models = {m_all, m_movies, m_series, m_myList, m_continue, m_search};
    m_collator = QCollator(QLocale(QLocale::English));
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    m_collator.setNumericMode(true);
    m_ioPool.setMaxThreadCount(1); // writes stay in order and never interleave
    m_scanMemory = std::make_shared<ScanMemory>();

    m_watcher = new QFutureWatcher<ScanResult>(this);
    connect(m_watcher, &QFutureWatcherBase::resultReadyAt, this, &Library::onScanResult);
    connect(m_watcher, &QFutureWatcherBase::finished, this, &Library::onScanFinished);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(kStateSaveMs);
    connect(m_saveTimer, &QTimer::timeout, this, [this] { saveWatchState(); });

    // Qt 6.10's FFmpeg media backend (hero preview / player) attaches a continuation to a QFuture that
    // reports two results and logs "Parent future has 2 result(s)" on every media load. It is harmless
    // and not actionable from application code, so mute just that category.
    QLoggingCategory::setFilterRules(QStringLiteral("qt.core.qfuture.continuations=false"));

    // auto-rescan: folder changes are debounced for 3 s
    m_rescanTimer = new QTimer(this);
    m_rescanTimer->setSingleShot(true);
    m_rescanTimer->setInterval(3000);
    connect(m_rescanTimer, &QTimer::timeout, this, &Library::startScan); // only the reported folders are read again
    m_fsWatcher = new QFileSystemWatcher(this);
    connect(m_fsWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &dir) {
        TIMING("folder changed: %s", qPrintable(dir));
        m_dirtyDirs.insert(dir);
        // debounce, but don't let a constant stream of events (an active download) postpone it forever
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!m_rescanTimer->isActive())
            m_firstChangeMs = now;
        if (now - m_firstChangeMs < 15000)
            m_rescanTimer->start(3000);
    });

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(kRefreshMs);
    connect(m_refreshTimer, &QTimer::timeout, this, &Library::flushRefresh);

    m_cacheSaveTimer = new QTimer(this);
    m_cacheSaveTimer->setSingleShot(true);
    m_cacheSaveTimer->setInterval(kCacheSaveMs);
    connect(m_cacheSaveTimer, &QTimer::timeout, this, [this] { saveScanCache(); });

    TIMING("Library created");
    loadSettings();
    loadWatchState();
    restoreScanCache();

    if (timingOn()) {
        QTimer::singleShot(0, this, [] {
            const auto windows = QGuiApplication::topLevelWindows();
            for (QWindow *w : windows)
                if (auto *qw = qobject_cast<QQuickWindow *>(w)) {
                    QObject::connect(qw, &QQuickWindow::frameSwapped, qw, [] { TIMING("first frame swapped"); },
                                     Qt::ConnectionType(Qt::DirectConnection | Qt::SingleShotConnection));
                    break;
                }
        });
    }
}

void Library::setThumbnailProvider(ThumbnailProvider *provider)
{
    m_thumbs = provider;
}

void Library::setWarmUpSkip(std::function<bool(const QString &)> skip)
{
    m_warmSkip = std::move(skip);
}

// Pending writes happen synchronously here (after queued background writes, so the newest state wins).
Library::~Library()
{
    if (s_instance == this) s_instance = nullptr;
    if (m_watchDirty || (m_saveTimer && m_saveTimer->isActive()))
        saveWatchState(true);
    if (m_cacheSaveTimer && m_cacheSaveTimer->isActive())
        saveScanCache(true);
    m_ioPool.waitForDone();
}

// ---------------------------------------------------------------- settings / persistence

void Library::loadSettings()
{
    QSettings s;
    if (s.contains(QStringLiteral("library/folders"))) {
        m_folders = s.value(QStringLiteral("library/folders")).toStringList();
    } else {
        const QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        for (const QString &d : {videos, downloads})
            if (!d.isEmpty() && QFileInfo(d).isDir() && !m_folders.contains(d))
                m_folders << d;
        saveSettings();
    }
}

void Library::saveSettings()
{
    QSettings s;
    s.setValue(QStringLiteral("library/folders"), m_folders);
}

void Library::loadWatchState()
{
    const QString file = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                         + QStringLiteral("/watchstate.json");
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject prog = root.value(QStringLiteral("progress")).toObject();
    for (auto it = prog.begin(); it != prog.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        Progress p;
        p.positionMs = qint64(o.value(QStringLiteral("positionMs")).toDouble());
        p.durationMs = qint64(o.value(QStringLiteral("durationMs")).toDouble());
        p.lastPlayed = QDateTime::fromString(o.value(QStringLiteral("lastPlayed")).toString(), Qt::ISODateWithMs);
        m_progress.insert(it.key(), p);
    }
    for (const QJsonValue &v : root.value(QStringLiteral("myList")).toArray())
        if (!v.toString().isEmpty() && !m_myListIds.contains(v.toString()))
            m_myListIds << v.toString();
}

// Snapshot (implicitly shared, so O(1)) on the GUI thread; serialization and the file write run on the
// one-thread I/O pool, or inline when `sync` (destructor).
void Library::saveWatchState(bool sync)
{
    if (m_saveTimer)
        m_saveTimer->stop();
    m_watchDirty = false;
    const QHash<QString, Progress> progress = m_progress;
    const QStringList myList = m_myListIds;
    auto write = [progress, myList]() {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        QJsonObject prog;
        for (auto it = progress.cbegin(); it != progress.cend(); ++it) {
            QJsonObject o;
            o.insert(QStringLiteral("positionMs"), double(it->positionMs));
            o.insert(QStringLiteral("durationMs"), double(it->durationMs));
            o.insert(QStringLiteral("lastPlayed"), it->lastPlayed.toString(Qt::ISODateWithMs));
            prog.insert(it.key(), o);
        }
        QJsonObject root;
        root.insert(QStringLiteral("progress"), prog);
        root.insert(QStringLiteral("myList"), QJsonArray::fromStringList(myList));
        writeFileAtomic(dir + QStringLiteral("/watchstate.json"), QJsonDocument(root).toJson(QJsonDocument::Compact));
    };
    if (sync) {
        m_ioPool.waitForDone();
        write();
    } else {
        m_ioPool.start(write);
    }
}

// Marks the watch state dirty and makes sure it is written within delayMs (an earlier deadline is kept).
void Library::scheduleSave(int delayMs)
{
    m_watchDirty = true;
    if (!m_saveTimer->isActive() || m_saveTimer->remainingTime() > delayMs)
        m_saveTimer->start(delayMs);
}

void Library::flushProgress()
{
    if (m_watchDirty || m_saveTimer->isActive())
        saveWatchState();
}

// ---------------------------------------------------------------- folders / scanning

// Manual rescan / folder list changed: every folder is read again.
void Library::rescan()
{
    m_fullRescan = true;
    startScan();
}

// Auto-rescans (watcher) only read the folders reported as changed (plus new subfolders) and re-use the
// other listings of the previous walk; the result is otherwise identical to a full scan. Full scans happen
// when asked for, on the first scan of a session and when not every folder could be watched.
void Library::startScan()
{
    if (m_watcher->isRunning()) {
        m_rescanPending = true;
        return;
    }
    m_rescanPending = false;
    if (!m_scanning) {
        m_scanning = true;
        emit scanningChanged();
    }
    ScanJob job;
    job.folders = m_folders;
    job.full = m_fullRescan || !m_dirCacheComplete;
    job.dirty = std::exchange(m_dirtyDirs, {});
    job.known = m_baseTitles;
    m_fullRescan = false;
    auto extras = std::make_shared<ScanExtras>();
    m_scanExtras = extras;
    const std::shared_ptr<ScanMemory> mem = m_scanMemory;
    const std::shared_ptr<OverlayCheck> check = std::exchange(m_overlayCheck, nullptr);
    if (job.full)
        TIMING("scan started (full)");
    else
        TIMING("scan started (%d changed folders)", int(job.dirty.size()));
    m_watcher->setFuture(QtConcurrent::run([job, mem, extras, check](QPromise<ScanResult> &promise) {
        if (check) { // overlays restored from the scan cache: are their image files still there?
            ScanResult r;
            r.kind = ScanResult::OverlayCheck;
            r.check = check;
            auto verify = [&r](const QHash<QString, QVariantMap> &maps) {
                for (auto it = maps.cbegin(); it != maps.cend(); ++it)
                    for (const QString &k : overlayFileKeys()) {
                        const QString p = it.value().value(k).toString();
                        if (!p.isEmpty() && !QFileInfo::exists(p))
                            r.brokenOverlays.insert(it.key().section(QLatin1Char('/'), 0, 0));
                    }
            };
            verify(check->ext);
            verify(check->extEp);
            promise.addResult(std::move(r));
        }
        const Publish publish = [&promise](QVector<Title> &&titles) {
            ScanResult r;
            r.kind = ScanResult::Partial;
            r.titles = std::move(titles);
            promise.addResult(std::move(r));
        };
        ScanResult r;
        r.titles = scanFolders(job, *mem, extras.get(), publish);
        promise.addResult(std::move(r));
    }));
}

void Library::onScanResult(int index)
{
    ScanResult r = m_watcher->resultAt(index);
    if (r.kind == ScanResult::OverlayCheck) {
        if (!r.brokenOverlays.isEmpty() && r.check)
            dropOverlays(r.brokenOverlays, *r.check);
        return;
    }
    QHash<QString, Title> base;
    base.reserve(r.titles.size());
    for (Title &t : r.titles) {
        const QString id = t.id;
        base.insert(id, std::move(t));
    }

    if (r.kind == ScanResult::Partial) { // progressive first scan / new folder: titles before ffprobe finished
        bool structural = false;
        applyTitles(std::move(base), false, true, &structural);
        if (!m_publishedThisScan) {
            m_publishedThisScan = true;
            int pending = 0;
            for (const Title &t : std::as_const(m_titles))
                for (const MediaFile &f : t.files)
                    pending += f.probed ? 0 : 1;
            TIMING("first titles published: %d titles (%d files waiting for ffprobe)", int(m_titles.size()), pending);
        }
        if (structural)
            emit libraryChanged();
        return;
    }

    const std::shared_ptr<ScanExtras> extras = m_scanExtras;
    if (extras)
        TIMING("scan finished: %d titles in %lld ms (walk %lld ms, %d of %d folders read; %d files probed, probe+parse %lld ms;"
               " %d still being written, %d rejected)", int(base.size()), extras->ms, extras->walkMs,
               extras->listedDirs, extras->totalDirs, extras->probed, extras->probeMs, extras->unstable,
               extras->rejected);
    const bool first = !m_haveScanned;
    m_haveScanned = true;
    const bool changed = applyTitles(std::move(base), false);
    if (m_featuredProvisional) { // hero picked while durations were unknown: pick again if a better tier exists
        m_featuredProvisional = false;
        improveFeatured();
    }
    if (changed || first)
        emit libraryChanged();
    if (extras) {
        const bool allWatched = updateWatcher(extras->dirs);
        m_dirCacheComplete = allWatched && extras->totalDirs <= kMaxWatchedDirs;
        // folders seen for the first time were read before they were watched: read them once more, so files
        // created in between are not missed
        if (!extras->newDirs.isEmpty()) {
            for (const QString &d : std::as_const(extras->newDirs))
                m_dirtyDirs.insert(d);
            if (!m_rescanTimer->isActive())
                m_rescanTimer->start(1000);
        }
    }
    if (changed)
        saveScanCache();
    if (changed || first)
        queueWarmUp();
}

void Library::onScanFinished()
{
    const std::shared_ptr<ScanExtras> extras = m_scanExtras;
    m_publishedThisScan = false;
    if (m_rescanPending) {
        startScan();
        return;
    }
    if (extras && extras->unstable > 0 && !m_rescanTimer->isActive())
        m_rescanTimer->start(8000); // pick up files that are still being written once they settle
    m_scanning = false;
    emit scanningChanged();
}

// Installs a new set of scanned titles (from a scan or the scan cache), re-applies the external
// metadata overlays, and updates the models incrementally. Returns true if anything changed.
// `partial`: an intermediate result of a progressive scan (files may not be probed yet).
bool Library::applyTitles(QHash<QString, Title> base, bool fromCache, bool partial, bool *structuralOut)
{
    QHash<QString, Title> titles;
    QHash<QString, QString> pathToId;
    QHash<QString, QVariantMap> ident;
    titles.reserve(base.size());
    bool unprobed = false;
    for (auto it = base.cbegin(); it != base.cend(); ++it) {
        const Title &b = it.value();
        ident.insert(b.id, {{QStringLiteral("title"), b.title}, {QStringLiteral("year"), b.year},
                            {QStringLiteral("isSeries"), b.isSeries}});
        Title t = b;
        applyOverlay(t);
        for (const MediaFile &f : t.files) {
            pathToId.insert(f.path, t.id);
            unprobed |= !f.probed;
        }
        titles.insert(t.id, std::move(t));
    }

    QSet<QString> changedIds;
    bool structural = titles.size() != m_titles.size();
    for (auto it = titles.cbegin(); it != titles.cend(); ++it) {
        const auto old = m_titles.constFind(it.key());
        if (old == m_titles.cend()) {
            structural = true;
        } else if (!sameTitle(old.value(), it.value())) {
            changedIds.insert(it.key());
            // no frame could be grabbed before ffprobe ran (unknown duration): new image URLs refetch them
            if (hasUnprobed(old.value()) && !hasUnprobed(it.value()))
                ++m_artGen[it.key()];
        }
    }
    if (structuralOut)
        *structuralOut = structural;
    const bool changed = structural || !changedIds.isEmpty();
    m_baseTitles = std::move(base);
    m_baseUnprobed = unprobed;
    m_parsedIdentity = std::move(ident);
    if (!changed) {
        TIMING("library unchanged");
        return false;
    }
    for (Title &t : titles)
        prepare(t);
    {
        QMutexLocker lock(&m_mutex);
        m_titles = std::move(titles);
        m_pathToId = std::move(pathToId);
    }
    m_resumeCache.clear();
    if (!fromCache && !partial)
        pruneProgress();
    rebuildModels(); // diffed: unchanged models keep their rows
    if (changedIds.size() > kBulkRefresh) {
        for (TitleModel *m : allModels())
            m->refreshAll();
    } else {
        for (const QString &id : std::as_const(changedIds))
            for (TitleModel *m : allModels())
                m->refresh(id);
    }
    if (m_featuredId.isEmpty() || !m_titles.contains(m_featuredId)) {
        pickFeatured();
        m_featuredProvisional = partial;
    } else if (changedIds.contains(m_featuredId)) {
        emit featuredChanged();
    }
    TIMING("models updated: %d titles (%d changed%s)", int(m_titles.size()), int(changedIds.size()),
           structural ? ", structural" : "");
    return true;
}

void Library::addFolder(const QUrl &dirUrl)
{
    QString path = dirUrl.isLocalFile() ? dirUrl.toLocalFile() : dirUrl.toString();
    if (path.startsWith(QLatin1String("file:")))
        path = QUrl(path).toLocalFile();
    if (path.startsWith(QLatin1String("~/")))
        path = QDir::homePath() + path.mid(1);
    path = QDir::cleanPath(path);
    if (path.isEmpty() || !QFileInfo(path).isDir() || m_folders.contains(path))
        return;
    m_folders << path;
    saveSettings();
    emit foldersChanged();
    rescan();
}

void Library::removeFolder(const QString &dir)
{
    QString path = normPath(dir);
    if (!m_folders.removeAll(path) && !m_folders.removeAll(QDir::cleanPath(path)))
        return;
    saveSettings();
    emit foldersChanged();
    rescan();
}

// ---------------------------------------------------------------- models

// Derived per-title values, so role reads / sorts don't recompute them (GUI thread; after m_artGen changes).
void Library::prepare(Title &t) const
{
    QSet<int> seasons;
    int w = 0, h = 0;
    qint64 total = 0;
    for (const MediaFile &f : std::as_const(t.files)) {
        if (f.season > 0)
            seasons.insert(f.season);
        w = std::max(w, f.width);
        h = std::max(h, f.height);
        total += f.durationMs;
    }
    t.seasonCount = !t.isSeries ? 0 : int(seasons.isEmpty() ? 1 : seasons.size());
    t.quality = qualityLabel(w, h);
    t.totalMs = total;
    t.cardUrl = QUrl(imageUrl(t, "card"));
    t.backdropUrl = QUrl(imageUrl(t, "backdrop"));
    t.logoUrl = t.logoFile.isEmpty() ? QUrl() : QUrl::fromLocalFile(t.logoFile);
    QString key = t.title;
    if (key.startsWith(QLatin1String("The "), Qt::CaseInsensitive))
        key = key.mid(4);
    t.sortKey = m_collator.sortKey(key);
}

void Library::rebuildModels()
{
    m_orderedIds = m_titles.keys();
    std::sort(m_orderedIds.begin(), m_orderedIds.end(), [this](const QString &a, const QString &b) {
        const Title &ta = *findTitle(a), &tb = *findTitle(b);
        const int c = ta.sortKey && tb.sortKey ? ta.sortKey->compare(*tb.sortKey) : m_collator.compare(ta.title, tb.title);
        if (c != 0) return c < 0;
        if (ta.year != tb.year) return ta.year < tb.year;
        return a < b;
    });

    QStringList movies, series;
    for (const QString &id : std::as_const(m_orderedIds))
        (m_titles.constFind(id)->isSeries ? series : movies) << id;
    m_all->setIds(m_orderedIds);
    m_movies->setIds(movies);
    m_series->setIds(series);
    rebuildContinue();
    rebuildMyList();
    rebuildSearch();
    rebuildRows();
}

void Library::refreshTitle(const QString &id, const QList<int> &roles)
{
    if (id.isEmpty())
        return;
    for (TitleModel *m : allModels())
        m->refresh(id, roles);
    if (id == m_featuredId)
        emit featuredChanged();
}

bool Library::rebuildContinue()
{
    struct E { QString id; QDateTime t; };
    QVector<E> list;
    for (auto it = m_progress.cbegin(); it != m_progress.cend(); ++it) {
        const QString id = m_pathToId.value(it.key());
        if (id.isEmpty())
            continue;
        const double p = progressFor(it.key());
        if (p < 0.01 || p > 0.95)
            continue;
        list.append({id, it->lastPlayed});
    }
    std::sort(list.begin(), list.end(), [](const E &a, const E &b) {
        if (a.t != b.t) return a.t > b.t;
        return a.id < b.id;
    });
    QStringList ids;
    QSet<QString> seen;
    for (const E &e : list)
        if (!seen.contains(e.id)) {
            seen.insert(e.id);
            ids << e.id;
        }
    if (ids == m_continue->ids())
        return false;
    m_continue->setIds(ids);
    return true;
}

void Library::rebuildMyList()
{
    QStringList ids;
    for (const QString &id : std::as_const(m_myListIds))
        if (m_titles.contains(id))
            ids << id;
    m_myList->setIds(ids);
}

void Library::rebuildSearch()
{
    const QString q = m_searchQuery.trimmed();
    if (q.isEmpty()) {
        m_search->setIds({});
        return;
    }
    QStringList prefix, rest;
    for (const QString &id : std::as_const(m_orderedIds)) {
        const Title &t = *findTitle(id);
        if (t.title.startsWith(q, Qt::CaseInsensitive)) {
            prefix << id;
            continue;
        }
        bool hit = t.title.contains(q, Qt::CaseInsensitive) || t.category.contains(q, Qt::CaseInsensitive)
                   || (t.year && QString::number(t.year) == q);
        for (int i = 0; !hit && i < t.genres.size(); ++i)
            hit = t.genres.at(i).contains(q, Qt::CaseInsensitive);
        for (int i = 0; !hit && t.isSeries && i < t.files.size(); ++i)
            hit = t.files.at(i).episodeTitle.contains(q, Qt::CaseInsensitive);
        if (hit)
            rest << id;
    }
    m_search->setIds(prefix + rest);
}

void Library::setSearchQuery(const QString &q)
{
    if (q == m_searchQuery)
        return;
    m_searchQuery = q;
    emit searchQueryChanged();
    rebuildSearch();
}

void Library::rebuildRows()
{
    auto title = [this](const QString &id) -> const Title & { return *findTitle(id); };
    // one persistent TitleModel per (RowsModel, kind, name)
    auto rowModel = [this](RowsModel *rm, const QString &key) {
        QList<TitleModel *> &list = m_rowModels[rm];
        for (TitleModel *m : std::as_const(list))
            if (m->objectName() == key)
                return m;
        auto *m = new TitleModel(this, rm);
        m->setObjectName(key);
        list << m;
        m_models << m;
        return m;
    };
    // categories get their own key space so a folder called "Movies" can't collide with the "Movies" row
    auto addRow = [&](QVector<RowsModel::Row> &rows, RowsModel *rm, const QString &kind, const QString &name,
                      const QStringList &ids, bool isCategory = false) {
        if (ids.isEmpty())
            return;
        TitleModel *m = rowModel(rm, (isCategory ? QStringLiteral("cat") : kind) + QLatin1Char('|') + name);
        m->setIds(ids);
        rows.append({name, kind, m});
    };
    // the first n of `ids` in `less` order (a total order, so this equals a full stable sort's prefix)
    auto firstN = [](QStringList l, int n, const auto &less) {
        n = std::min(n, int(l.size()));
        std::partial_sort(l.begin(), l.begin() + n, l.end(), less);
        return l.mid(0, n);
    };
    auto recent = [&](const QStringList &ids, int n) {
        return firstN(ids, n, [&](const QString &a, const QString &b) {
            const Title &ta = title(a), &tb = title(b);
            if (ta.added != tb.added) return ta.added > tb.added;
            return a < b;
        });
    };
    // categories: ordered by size desc, then name
    auto byCategory = [&](const QStringList &ids) {
        QMap<QString, QStringList> cats;
        for (const QString &id : ids)
            cats[title(id).category] << id;
        QList<QPair<QString, QStringList>> list;
        for (auto it = cats.cbegin(); it != cats.cend(); ++it)
            list.append({it.key(), it.value()});
        std::stable_sort(list.begin(), list.end(), [](const auto &a, const auto &b) {
            return a.second.size() > b.second.size();
        });
        return list;
    };
    // rows that disappeared keep their (persistent) model; empty it so it holds no stale ids
    auto clearUnused = [this](RowsModel *rm) {
        for (TitleModel *m : std::as_const(m_rowModels[rm])) {
            bool used = false;
            for (const auto &r : rm->rows()) used |= r.model == m;
            if (!used) m->setIds({});
        }
    };
    const QStringList movies = m_movies->ids();
    const QStringList series = m_series->ids();
    const QStringList cont = m_continue->ids();
    QStringList contMovies, contSeries;
    for (const QString &id : cont)
        (m_titles.value(id).isSeries ? contSeries : contMovies) << id;

    // ---- home
    {
        QVector<RowsModel::Row> rows;
        if (m_continue->rowCount() > 0)
            rows.append({QStringLiteral("Continue Watching"), QStringLiteral("continue"), m_continue});
        if (m_myList->rowCount() > 0)
            rows.append({QStringLiteral("My List"), QStringLiteral("mylist"), m_myList});
        addRow(rows, m_homeRows, QStringLiteral("normal"), QStringLiteral("Recently Added"), recent(m_orderedIds, 20));
        const QStringList top = firstN(m_orderedIds, 10, [&](const QString &a, const QString &b) {
            const Title &ta = title(a), &tb = title(b);
            if (ta.totalMs != tb.totalMs) return ta.totalMs > tb.totalMs;
            if (ta.files.size() != tb.files.size()) return ta.files.size() > tb.files.size();
            return a < b;
        });
        addRow(rows, m_homeRows, QStringLiteral("top10"), QStringLiteral("Top 10 in Your Library"), top);
        for (const auto &c : byCategory(m_orderedIds))
            addRow(rows, m_homeRows, QStringLiteral("normal"), c.first, c.second, true);
        if (!movies.isEmpty() && !series.isEmpty()) {
            addRow(rows, m_homeRows, QStringLiteral("normal"), QStringLiteral("TV Shows"), series);
            addRow(rows, m_homeRows, QStringLiteral("normal"), QStringLiteral("Movies"), movies);
        }
        m_homeRows->setRows(rows);
    }
    // ---- movies / series pages
    auto pageRows = [&](RowsModel *rm, const QStringList &ids, const QStringList &contIds) {
        QVector<RowsModel::Row> rows;
        addRow(rows, rm, QStringLiteral("continue"), QStringLiteral("Continue Watching"), contIds);
        addRow(rows, rm, QStringLiteral("normal"), QStringLiteral("Recently Added"), recent(ids, 20));
        for (const auto &c : byCategory(ids))
            addRow(rows, rm, QStringLiteral("normal"), c.first, c.second, true);
        rm->setRows(rows);
        clearUnused(rm);
    };
    pageRows(m_movieRows, movies, contMovies);
    pageRows(m_seriesRows, series, contSeries);
    clearUnused(m_homeRows);
}

// ---------------------------------------------------------------- featured

namespace {
// 0 = best hero material (a real movie/show, long enough) .. 3 = recordings / clips
int featuredTier(const Title &t)
{
    const QString stem = QFileInfo(t.files.first().path).completeBaseName();
    const bool recording = !t.isSeries && (looksLikeRecording(stem) || looksLikeTicket(stem));
    qint64 dur = 0;
    for (const MediaFile &f : t.files) dur = std::max(dur, f.durationMs);
    const bool longEnough = dur > 20 * 60 * 1000;
    const bool real = (t.isSeries || t.year > 0) && !recording;
    if (real && longEnough) return 0;
    if (real) return 1;
    if (longEnough && !recording) return 2;
    return 3;
}
} // namespace

void Library::pickFeatured()
{
    if (m_titles.isEmpty()) {
        if (!m_featuredId.isEmpty()) {
            m_featuredId.clear();
            emit featuredChanged();
        }
        return;
    }
    QStringList tiers[4];
    for (auto it = m_titles.cbegin(); it != m_titles.cend(); ++it)
        tiers[featuredTier(it.value())] << it.key();
    for (QStringList &tier : tiers) {
        if (tier.isEmpty())
            continue;
        std::sort(tier.begin(), tier.end());
        if (tier.size() > 1)
            tier.removeAll(m_featuredId);
        m_featuredId = tier.at(QRandomGenerator::global()->bounded(int(tier.size())));
        emit featuredChanged();
        return;
    }
}

// The hero was picked from a progressive scan's titles (durations unknown): pick again only if a better tier
// than the current hero's exists now.
void Library::improveFeatured()
{
    const Title *cur = findTitle(m_featuredId);
    if (!cur) {
        pickFeatured();
        return;
    }
    const int tier = featuredTier(*cur);
    for (const Title &t : std::as_const(m_titles))
        if (featuredTier(t) < tier) {
            pickFeatured();
            return;
        }
}

QVariantMap Library::featured() const
{
    const Title *t = findTitle(m_featuredId);
    return t ? titleToMap(*t, 0) : QVariantMap();
}

// ---------------------------------------------------------------- title lookup

const Title *Library::findTitle(const QString &id) const
{
    auto it = m_titles.constFind(id);
    return it == m_titles.cend() ? nullptr : &it.value();
}

double Library::progressFor(const QString &path) const
{
    auto it = m_progress.constFind(path);
    if (it == m_progress.cend())
        return 0.0;
    const qint64 dur = it->durationMs > 0 ? it->durationMs : durationFor(path);
    if (dur <= 0)
        return 0.0;
    return std::clamp(double(it->positionMs) / double(dur), 0.0, 1.0);
}

// Memoized per title (roles read it up to five times per delegate); progressUpdated() / applyTitles() drop it.
int Library::resumeIndex(const Title &t) const
{
    const auto memo = m_resumeCache.constFind(t.id);
    if (memo != m_resumeCache.cend())
        return memo.value();
    auto compute = [&]() -> int {
        if (t.files.isEmpty())
            return -1;
        int best = -1;
        QDateTime bestTime;
        for (int i = 0; i < t.files.size(); ++i) {
            auto it = m_progress.constFind(t.files.at(i).path);
            if (it == m_progress.cend())
                continue;
            if (best < 0 || it->lastPlayed > bestTime) {
                best = i;
                bestTime = it->lastPlayed;
            }
        }
        if (best < 0) {
            for (int i = 0; i < t.files.size(); ++i)
                if (!t.isSeries || t.files.at(i).season != 0)
                    return i;
            return 0;
        }
        // finished an episode -> point at the next one
        if (t.isSeries && progressFor(t.files.at(best).path) >= 0.95 && best + 1 < t.files.size()
            && (t.files.at(best + 1).season == 0) == (t.files.at(best).season == 0))
            return best + 1;
        return best;
    };
    const int i = compute();
    m_resumeCache.insert(t.id, i);
    return i;
}

QVariant Library::roleData(const Title &t, int role, int rank) const
{
    auto file = [&]() -> const MediaFile * {
        const int i = resumeIndex(t);
        return i >= 0 ? &t.files.at(i) : nullptr;
    };
    switch (role) {
    case TitleModel::IdRole: return t.id;
    case TitleModel::TitleRole: return t.title;
    case TitleModel::YearRole: return t.year;
    case TitleModel::PathRole: { auto f = file(); return f ? f->path : QString(); }
    case TitleModel::SourceUrlRole: { auto f = file(); return f ? QUrl::fromLocalFile(f->path) : QUrl(); }
    case TitleModel::IsSeriesRole: return t.isSeries;
    case TitleModel::SeasonCountRole: return t.seasonCount;
    case TitleModel::EpisodeCountRole: return int(t.isSeries ? t.files.size() : 0);
    case TitleModel::DurationMsRole: {
        auto f = file();
        if (!f) return qint64(0);
        if (f->durationMs > 0) return f->durationMs;
        return m_progress.value(f->path).durationMs;
    }
    case TitleModel::QualityRole: return t.quality;
    case TitleModel::CardImageRole: return t.cardUrl;
    case TitleModel::BackdropImageRole: return t.backdropUrl;
    case TitleModel::PositionMsRole: { auto f = file(); return f ? positionFor(f->path) : qint64(0); }
    case TitleModel::ProgressRole: { auto f = file(); return f ? progressFor(f->path) : 0.0; }
    case TitleModel::InMyListRole: return m_myListIds.contains(t.id);
    case TitleModel::AddedRole: return t.added;
    case TitleModel::CategoryRole: return t.category;
    case TitleModel::DescriptionRole: return t.description;
    case TitleModel::RatingRole: return t.rating;
    case TitleModel::GenresRole: return t.genres;
    case TitleModel::MatchRole: return t.match;
    case TitleModel::RankRole: return rank;
    case TitleModel::LogoImageRole: return t.logoUrl;
    case TitleModel::HasMetaRole: return t.hasExternalMeta;
    case TitleModel::IsRecentRole: return isRecentDate(t.added, QDateTime::currentMSecsSinceEpoch());
    default: return {};
    }
}

QVariantMap Library::titleToMap(const Title &t, int rank) const
{
    const QHash<int, QByteArray> names = m_all->roleNames();
    QVariantMap m;
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        m.insert(QString::fromLatin1(it.value()), roleData(t, it.key(), rank));
    return m;
}

QVariantMap Library::title(const QString &id) const
{
    const Title *t = findTitle(id);
    return t ? titleToMap(*t, 0) : QVariantMap();
}

QVariantList Library::recentTitles(int n) const
{
    // (added, alphabetical position): newest first, ties in alphabetical order; unknown dates last
    QVector<QPair<qint64, int>> order;
    order.reserve(m_orderedIds.size());
    for (int i = 0; i < m_orderedIds.size(); ++i) {
        const QDateTime &added = m_titles.constFind(m_orderedIds.at(i))->added;
        order.append({added.isValid() ? added.toMSecsSinceEpoch() : std::numeric_limits<qint64>::min(), i});
    }
    n = std::clamp(n, 0, int(order.size()));
    std::partial_sort(order.begin(), order.begin() + n, order.end(), [](const auto &a, const auto &b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    QVariantList out;
    for (int k = 0; k < n; ++k)
        out << titleToMap(*m_titles.constFind(m_orderedIds.at(order.at(k).second)), 0);
    return out;
}

int Library::recentCount() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    return int(std::count_if(m_titles.cbegin(), m_titles.cend(), [now](const Title &t) { return isRecentDate(t.added, now); }));
}

int Library::episodeCount(const QString &id, int season) const
{
    const Title *t = findTitle(id);
    if (!t)
        return 0;
    if (!t->isSeries)
        return int(t->files.size());
    return int(std::count_if(t->files.cbegin(), t->files.cend(), [season](const MediaFile &f) { return f.season == season; }));
}

QVariantList Library::seasons(const QString &id) const
{
    const Title *t = findTitle(id);
    QVariantList out;
    if (!t || !t->isSeries)
        return out;
    QList<int> s;
    bool specials = false;
    for (const MediaFile &f : t->files) {
        if (f.season == 0) specials = true;
        else if (!s.contains(f.season)) s << f.season;
    }
    std::sort(s.begin(), s.end());
    for (int x : s) out << x;
    if (specials) out << 0; // season 0 = specials / featurettes, listed last
    return out;
}

QVariantList Library::episodes(const QString &id, int season) const
{
    const Title *t = findTitle(id);
    QVariantList out;
    if (!t)
        return out;
    const int gen = m_artGen.value(t->id);
    for (const MediaFile &f : t->files) {
        if (t->isSeries && f.season != season)
            continue;
        QVariantMap m;
        const QVariantMap ov = t->isSeries ? episodeOverlay(t->id, f.season, f.episode) : QVariantMap();
        m.insert(QStringLiteral("season"), f.season);
        m.insert(QStringLiteral("episode"), f.episode);
        QString name = ov.value(QStringLiteral("title")).toString();
        if (name.isEmpty())
            name = f.episodeTitle;
        if (name.isEmpty())
            name = t->isSeries ? (f.season == 0 ? QStringLiteral("Special %1").arg(f.episode)
                                                : QStringLiteral("Episode %1").arg(f.episode))
                               : t->title;
        m.insert(QStringLiteral("title"), name);
        QString desc = ov.value(QStringLiteral("description")).toString();
        if (desc.isEmpty())
            desc = !t->isSeries ? t->description
                   : f.season == 0 ? QStringLiteral("Bonus feature %1 of %2.").arg(f.episode).arg(t->title)
                                   : QStringLiteral("Episode %1 of Season %2.").arg(f.episode).arg(f.season);
        m.insert(QStringLiteral("description"), desc);
        QString thumb = QStringLiteral("image://thumbs/%1/ep/%2/%3").arg(t->id).arg(f.season).arg(f.episode);
        if (gen > 0) // see imageUrl()
            thumb += QStringLiteral("?v=%1").arg(gen);
        const QUrl thumbUrl(thumb);
        const QString still = ov.value(QStringLiteral("stillFile")).toString();
        m.insert(QStringLiteral("still"), still.isEmpty() ? thumbUrl : QUrl::fromLocalFile(still));
        m.insert(QStringLiteral("path"), f.path);
        m.insert(QStringLiteral("url"), QUrl::fromLocalFile(f.path));
        const qint64 dur = f.durationMs > 0 ? f.durationMs : m_progress.value(f.path).durationMs;
        m.insert(QStringLiteral("durationMs"), dur);
        m.insert(QStringLiteral("positionMs"), positionFor(f.path));
        m.insert(QStringLiteral("progress"), progressFor(f.path));
        m.insert(QStringLiteral("thumb"), thumbUrl);
        out << m;
    }
    return out;
}

QString Library::titleIdForPath(const QString &path) const
{
    const QString p = normPath(path);
    QString id = m_pathToId.value(p);
    if (id.isEmpty())
        id = m_pathToId.value(QDir::cleanPath(p));
    return id;
}

QVariantMap Library::fileInfo(const QString &path) const
{
    const QString p = normPath(path);
    QVariantMap m;
    m.insert(QStringLiteral("path"), p);
    m.insert(QStringLiteral("url"), QUrl::fromLocalFile(p));
    m.insert(QStringLiteral("positionMs"), positionFor(p));
    m.insert(QStringLiteral("nextPath"), QString());
    m.insert(QStringLiteral("nextUrl"), QUrl());
    m.insert(QStringLiteral("nextLabel"), QString());
    m.insert(QStringLiteral("episodeTitle"), QString());
    m.insert(QStringLiteral("season"), 0);
    m.insert(QStringLiteral("episode"), 0);

    const QString id = titleIdForPath(p);
    const Title *t = findTitle(id);
    if (!t) {
        m.insert(QStringLiteral("id"), QString());
        m.insert(QStringLiteral("title"), cleanRecordingTitle(QFileInfo(p).completeBaseName()));
        m.insert(QStringLiteral("isSeries"), false);
        m.insert(QStringLiteral("durationMs"), m_progress.value(p).durationMs);
        m.insert(QStringLiteral("backdrop"), QUrl());
        return m;
    }
    m.insert(QStringLiteral("id"), t->id);
    m.insert(QStringLiteral("title"), t->title);
    m.insert(QStringLiteral("isSeries"), t->isSeries);
    m.insert(QStringLiteral("backdrop"), QUrl(imageUrl(*t, "backdrop")));
    int idx = -1;
    for (int i = 0; i < t->files.size(); ++i)
        if (t->files.at(i).path == p || t->files.at(i).path == QDir::cleanPath(p)) { idx = i; break; }
    if (idx < 0)
        return m;
    const MediaFile &f = t->files.at(idx);
    m.insert(QStringLiteral("path"), f.path);
    m.insert(QStringLiteral("url"), QUrl::fromLocalFile(f.path));
    m.insert(QStringLiteral("durationMs"), f.durationMs > 0 ? f.durationMs : m_progress.value(f.path).durationMs);
    m.insert(QStringLiteral("positionMs"), positionFor(f.path));
    if (t->isSeries) {
        m.insert(QStringLiteral("season"), f.season);
        m.insert(QStringLiteral("episode"), f.episode);
        const QString ovTitle = episodeOverlay(t->id, f.season, f.episode).value(QStringLiteral("title")).toString();
        m.insert(QStringLiteral("episodeTitle"), ovTitle.isEmpty() ? f.episodeTitle : ovTitle);
        if (idx + 1 < t->files.size() && (t->files.at(idx + 1).season == 0) == (f.season == 0)) {
            const MediaFile &n = t->files.at(idx + 1);
            m.insert(QStringLiteral("nextPath"), n.path);
            m.insert(QStringLiteral("nextUrl"), QUrl::fromLocalFile(n.path));
            QString label = QStringLiteral("S%1:E%2").arg(n.season).arg(n.episode);
            QString nextTitle = episodeOverlay(t->id, n.season, n.episode).value(QStringLiteral("title")).toString();
            if (nextTitle.isEmpty())
                nextTitle = n.episodeTitle;
            if (!nextTitle.isEmpty())
                label += QLatin1Char(' ') + nextTitle;
            m.insert(QStringLiteral("nextLabel"), label);
        }
    }
    return m;
}

QVariantList Library::sidecarSubtitles(const QString &path) const
{
    const QFileInfo fi(normPath(path));
    const QString stem = fi.completeBaseName();
    const QDir dir = fi.dir();
    struct Sub { QString path, label; };
    QVector<Sub> subs;
    QSet<QString> seen;
    auto add = [&](const QFileInfo &sf, const QString &tagPart) {
        if (seen.contains(sf.absoluteFilePath()))
            return;
        seen.insert(sf.absoluteFilePath());
        QString label = subtitleLabel(tagPart);
        if (label.isEmpty())
            label = QStringLiteral("Subtitles");
        subs.append({sf.absoluteFilePath(), label});
    };

    QStringList nameFilters;
    for (const QString &e : subtitleExtensions())
        nameFilters << QStringLiteral("*.") + e;
    const QFileInfoList here = dir.entryInfoList(nameFilters, QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &sf : here) {
        const QString base = sf.completeBaseName();
        if (base == stem)
            add(sf, QString());
        else if (base.startsWith(stem + QLatin1Char('.')))
            add(sf, base.mid(stem.size() + 1));
    }
    const QFileInfoList subDirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable);
    for (const QFileInfo &d : subDirs) {
        const QString n = d.fileName().toLower();
        if (n != QLatin1String("subs") && n != QLatin1String("subtitles") && n != QLatin1String("sub"))
            continue;
        QDir sd(d.absoluteFilePath());
        for (const QFileInfo &sf : sd.entryInfoList(nameFilters, QDir::Files | QDir::Readable, QDir::Name)) {
            const QString base = sf.completeBaseName();
            const int at = int(base.indexOf(stem, 0, Qt::CaseInsensitive));
            if (at >= 0) {
                QString tag = base;
                tag.remove(at, stem.size());
                add(sf, tag);
            }
        }
        // Subs/<stem>/2_English.srt layout
        QDir per(sd.absoluteFilePath(stem));
        if (per.exists())
            for (const QFileInfo &sf : per.entryInfoList(nameFilters, QDir::Files | QDir::Readable, QDir::Name))
                add(sf, sf.completeBaseName());
    }

    // disambiguate equal labels
    QHash<QString, int> counts;
    for (const Sub &s : subs) counts[s.label]++;
    QHash<QString, int> used;
    QVariantList out;
    std::stable_sort(subs.begin(), subs.end(), [](const Sub &a, const Sub &b) {
        const bool ea = a.label.startsWith(QLatin1String("English")), eb = b.label.startsWith(QLatin1String("English"));
        if (ea != eb) return ea;
        return a.label < b.label;
    });
    for (const Sub &s : subs) {
        QString label = s.label;
        if (counts.value(s.label) > 1)
            label += QStringLiteral(" %1").arg(++used[s.label]);
        out << QVariantMap{{QStringLiteral("path"), s.path}, {QStringLiteral("label"), label}};
    }
    return out;
}

// ---------------------------------------------------------------- watch state

qint64 Library::positionFor(const QString &path) const
{
    return m_progress.value(path).positionMs;
}

qint64 Library::durationFor(const QString &path) const
{
    const Title *t = findTitle(m_pathToId.value(path));
    if (!t)
        return 0;
    for (const MediaFile &f : t->files)
        if (f.path == path)
            return f.durationMs;
    return 0;
}

qint64 Library::position(const QString &path) const
{
    return positionFor(normPath(path));
}

// Called every ~5 s during playback: only the progress roles of that title are refreshed, and the rows are
// rebuilt only when the Continue Watching list itself changes (a title enters / leaves / moves).
void Library::setProgress(const QString &path, qint64 positionMs, qint64 durationMs)
{
    const QString p = normPath(path);
    if (p.isEmpty())
        return;
    Progress &pr = m_progress[p];
    pr.positionMs = std::max<qint64>(0, positionMs);
    if (durationMs > 0)
        pr.durationMs = durationMs;
    pr.lastPlayed = QDateTime::currentDateTime();
    scheduleSave(kProgressSaveMs);
    emit progressChanged(p);
    progressUpdated(p);
}

void Library::progressUpdated(const QString &path)
{
    const QString id = m_pathToId.value(path);
    m_resumeCache.remove(id);
    if (rebuildContinue())
        rebuildRows();
    refreshTitle(id, progressRoles());
}

void Library::markWatched(const QString &path)
{
    const QString p = normPath(path);
    if (p.isEmpty())
        return;
    Progress &pr = m_progress[p];
    qint64 dur = pr.durationMs > 0 ? pr.durationMs : durationFor(p);
    if (dur <= 0)
        dur = 1;
    pr.durationMs = dur;
    pr.positionMs = dur;
    pr.lastPlayed = QDateTime::currentDateTime();
    scheduleSave(kStateSaveMs);
    emit progressChanged(p);
    progressUpdated(p);
}

void Library::clearProgress(const QString &path)
{
    const QString p = normPath(path);
    if (!m_progress.remove(p))
        return;
    scheduleSave(kStateSaveMs);
    emit progressChanged(p);
    progressUpdated(p);
}

void Library::toggleMyList(const QString &id)
{
    if (id.isEmpty())
        return;
    if (!m_myListIds.removeAll(id))
        m_myListIds << id;
    scheduleSave(kStateSaveMs);
    emit myListChanged(id);
    rebuildMyList();
    rebuildRows();
    refreshTitle(id, {TitleModel::InMyListRole});
}

bool Library::inMyList(const QString &id) const
{
    return m_myListIds.contains(id);
}

// ---------------------------------------------------------------- for ThumbnailProvider (any thread)

QVariantMap Library::thumbInfo(const QString &id, int season, int episode) const
{
    QMutexLocker lock(&m_mutex);
    auto it = m_titles.constFind(id);
    if (it == m_titles.cend() || it->files.isEmpty())
        return {};
    const Title &t = it.value();
    const MediaFile *f = nullptr;
    if (season >= 0) {
        for (const MediaFile &mf : t.files)
            if (mf.season == season && mf.episode == episode) { f = &mf; break; }
    }
    if (!f) {
        for (const MediaFile &mf : t.files)
            if (!t.isSeries || mf.season != 0) { f = &mf; break; }
        if (!f)
            f = &t.files.first();
    }
    // not probed yet (progressive first scan): without the duration a frame grab would pick the wrong spot and
    // be cached for good; the provider draws a placeholder, and the title's image URLs change once it is probed
    if (!f->probed)
        return {};
    QVariantMap m;
    m.insert(QStringLiteral("title"), t.title);
    m.insert(QStringLiteral("path"), f->path);
    m.insert(QStringLiteral("durationMs"), f->durationMs);
    m.insert(QStringLiteral("poster"), t.posterFile);
    m.insert(QStringLiteral("backdrop"), t.backdropFile);
    m.insert(QStringLiteral("episodeTitle"), f->episodeTitle);
    m.insert(QStringLiteral("mtime"), f->modified);
    m.insert(QStringLiteral("codec"), f->videoCodec);
    m.insert(QStringLiteral("height"), f->height);
    if (season >= 0 && t.isSeries) {
        const QVariantMap ov = m_extEpisodeMeta.value(episodeKey(t.id, f->season, f->episode));
        const QString still = ov.value(QStringLiteral("stillFile")).toString();
        if (!still.isEmpty())
            m.insert(QStringLiteral("still"), still);
        const QString et = ov.value(QStringLiteral("title")).toString();
        if (!et.isEmpty())
            m.insert(QStringLiteral("episodeTitle"), et);
    }
    return m;
}


// ---------------------------------------------------------------- external metadata overlays

QString Library::imageUrl(const Title &t, const char *kind) const
{
    const int gen = m_artGen.value(t.id);
    QString url = QStringLiteral("image://thumbs/%1/%2").arg(t.id, QLatin1String(kind));
    if (gen > 0) // new artwork -> new URL, so QML's pixmap cache can't serve the old image
        url += QStringLiteral("?v=%1").arg(gen);
    return url;
}

QVariantMap Library::episodeOverlay(const QString &id, int season, int episode) const
{
    return m_extEpisodeMeta.value(episodeKey(id, season, episode));
}

void Library::applyOverlay(Title &t) const
{
    t.logoFile.clear();
    const auto it = m_extMeta.constFind(t.id);
    t.hasExternalMeta = it != m_extMeta.cend() && !it->isEmpty();
    if (!t.hasExternalMeta)
        return;
    const QVariantMap &m = it.value();
    auto str = [&m](const char *key) { return m.value(QLatin1String(key)).toString().trimmed(); };
    if (!str("title").isEmpty())
        t.title = str("title");
    if (m.value(QStringLiteral("year")).toInt() > 0)
        t.year = m.value(QStringLiteral("year")).toInt();
    if (!str("description").isEmpty())
        t.description = str("description");
    QStringList genres = m.value(QStringLiteral("genres")).toStringList();
    genres.removeAll(QString());
    if (!genres.isEmpty())
        t.genres = genres;
    if (!str("rating").isEmpty())
        t.rating = str("rating");
    if (m.value(QStringLiteral("match")).toInt() > 0)
        t.match = std::clamp(m.value(QStringLiteral("match")).toInt(), 1, 100);
    if (!str("posterFile").isEmpty())
        t.posterFile = str("posterFile");
    if (!str("backdropFile").isEmpty())
        t.backdropFile = str("backdropFile");
    t.logoFile = str("logoFile");
}

void Library::setExternalMetadata(const QString &id, const QVariantMap &meta)
{
    if (id.isEmpty())
        return;
    // idempotent: the metadata service re-applies after every libraryChanged()
    const auto old = m_extMeta.constFind(id);
    if (meta.isEmpty()) { // empty map clears the overlay
        if (old == m_extMeta.cend())
            return;
        m_extMeta.erase(old);
    } else {
        if (old != m_extMeta.cend() && old.value() == meta)
            return;
        m_extMeta.insert(id, meta);
    }
    scheduleCacheSave();
    reapplyOverlay(id);
}

// m_extMeta[id] changed: rebuild the applied title from its scanned values. The model refresh is coalesced
// (kRefreshMs), so a burst of metadata replies costs one re-sort and one refresh pass.
void Library::reapplyOverlay(const QString &id)
{
    const auto base = m_baseTitles.constFind(id);
    auto cur = m_titles.find(id);
    if (base == m_baseTitles.cend() || cur == m_titles.end())
        return; // kept; applied when the title shows up in a scan
    Title t = base.value();
    applyOverlay(t);
    const bool artChanged = cur->posterFile != t.posterFile || cur->backdropFile != t.backdropFile;
    const bool resort = cur->title != t.title || cur->year != t.year;
    if (sameTitle(cur.value(), t))
        return;
    if (artChanged) {
        ++m_artGen[id];
        if (m_thumbs)
            m_thumbs->invalidate(id);
    }
    prepare(t);
    {
        QMutexLocker lock(&m_mutex);
        cur.value() = std::move(t);
    }
    m_pendingRefresh.insert(id);
    m_pendingResort |= resort;
    if (!m_refreshTimer->isActive())
        m_refreshTimer->start();
}

void Library::setExternalEpisodeMetadata(const QString &id, int season, int episode, const QVariantMap &meta)
{
    if (id.isEmpty())
        return;
    const QString key = episodeKey(id, season, episode);
    {
        QMutexLocker lock(&m_mutex); // thumbInfo() reads episode stills from the image threads
        const auto old = m_extEpisodeMeta.constFind(key);
        if (meta.isEmpty()) {
            if (old == m_extEpisodeMeta.cend())
                return;
            m_extEpisodeMeta.erase(old);
        } else {
            if (old != m_extEpisodeMeta.cend() && old.value() == meta)
                return;
            m_extEpisodeMeta.insert(key, meta);
        }
    }
    scheduleCacheSave();
    if (m_titles.contains(id)) {
        // lets views that show episode data (detail modal / player) re-query episodes()
        m_pendingRefresh.insert(id);
        if (!m_refreshTimer->isActive())
            m_refreshTimer->start();
    }
}

QVariantMap Library::parsedIdentity(const QString &id) const
{
    auto it = m_parsedIdentity.constFind(id);
    return it == m_parsedIdentity.cend() ? QVariantMap() : it.value();
}

void Library::flushRefresh()
{
    const QSet<QString> ids = std::exchange(m_pendingRefresh, {});
    if (m_pendingResort) {
        m_pendingResort = false;
        rebuildModels(); // order / search may change with a new title; rows are diffed, not reset
    }
    if (ids.size() > kBulkRefresh) { // e.g. the metadata service's first sync: one pass per model
        for (TitleModel *m : allModels())
            m->refreshAll();
        if (ids.contains(m_featuredId))
            emit featuredChanged();
        return;
    }
    for (const QString &id : ids)
        refreshTitle(id); // also re-emits featuredChanged for the hero
}

// Restored overlays that reference missing files (checked by the first scan, off the GUI thread) are dropped;
// the metadata service then re-applies them from its own cache, or not at all. Overlays that were replaced
// since the restore are left alone.
void Library::dropOverlays(const QSet<QString> &ids, const OverlayCheck &snapshot)
{
    int dropped = 0;
    for (const QString &id : ids) {
        const auto cur = m_extMeta.constFind(id);
        if (cur != m_extMeta.cend() && snapshot.ext.contains(id) && cur.value() == snapshot.ext.value(id)) {
            m_extMeta.erase(cur);
            ++dropped;
        }
        {
            QMutexLocker lock(&m_mutex);
            const QString prefix = id + QLatin1Char('/');
            for (auto it = m_extEpisodeMeta.begin(); it != m_extEpisodeMeta.end();) {
                const auto snap = snapshot.extEp.constFind(it.key());
                if (it.key().startsWith(prefix) && snap != snapshot.extEp.cend() && snap.value() == it.value())
                    it = m_extEpisodeMeta.erase(it);
                else
                    ++it;
            }
        }
        reapplyOverlay(id);
        if (m_titles.contains(id))
            m_pendingRefresh.insert(id); // episodes changed too
    }
    if (!m_pendingRefresh.isEmpty() && !m_refreshTimer->isActive())
        m_refreshTimer->start();
    scheduleCacheSave();
    TIMING("dropped overlays of %d titles (%d with missing files)", dropped, int(ids.size()));
}

// ---------------------------------------------------------------- scan cache

// Debounced (kCacheSaveMs after the last change), but written at least every kCacheSaveMaxWaitMs while
// changes keep coming (metadata replies stream in for a while).
void Library::scheduleCacheSave()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_cacheSaveTimer->isActive())
        m_cacheDirtySince = now;
    if (now - m_cacheDirtySince < kCacheSaveMaxWaitMs)
        m_cacheSaveTimer->start(kCacheSaveMs);
}

// The data is snapshotted here (implicitly shared containers); JSON building and the write run on the
// one-thread I/O pool (in order), or inline when `sync` (destructor).
void Library::saveScanCache(bool sync)
{
    m_cacheSaveTimer->stop();
    if (m_baseUnprobed)
        return; // a progressive scan is running: its final result is saved (unprobed values never are)
    const QHash<QString, Title> base = m_baseTitles;
    const QHash<QString, QVariantMap> extMeta = m_extMeta;
    QHash<QString, QVariantMap> extEpisodeMeta;
    {
        QMutexLocker lock(&m_mutex);
        extEpisodeMeta = m_extEpisodeMeta;
    }
    const QStringList folders = m_folders;
    auto write = [base, extMeta, extEpisodeMeta, folders]() {
        QJsonArray titles;
        QStringList ids = base.keys();
        std::sort(ids.begin(), ids.end());
        for (const QString &id : std::as_const(ids))
            titles.append(titleToJson(base.value(id)));
        QJsonObject ext, extEp;
        for (auto it = extMeta.cbegin(); it != extMeta.cend(); ++it)
            ext.insert(it.key(), QJsonObject::fromVariantMap(it.value()));
        for (auto it = extEpisodeMeta.cbegin(); it != extEpisodeMeta.cend(); ++it)
            extEp.insert(it.key(), QJsonObject::fromVariantMap(it.value()));
        QJsonObject root;
        root.insert(QStringLiteral("version"), 1);
        root.insert(QStringLiteral("folders"), QJsonArray::fromStringList(folders));
        root.insert(QStringLiteral("titles"), titles);
        root.insert(QStringLiteral("ext"), ext);
        root.insert(QStringLiteral("extEpisodes"), extEp);
        writeFileAtomic(cachePath(QStringLiteral("library-cache.json")), QJsonDocument(root).toJson(QJsonDocument::Compact));
    };
    if (sync) {
        m_ioPool.waitForDone();
        write();
    } else {
        m_ioPool.start(write);
    }
}

// Populates the models from the last scan before the first (async) rescan finishes; the rescan then
// only applies the differences. Overlay image files are not checked here (that would stat every poster
// before the first frame): the first scan verifies them in the background (dropOverlays()).
void Library::restoreScanCache()
{
    QFile f(cachePath(QStringLiteral("library-cache.json")));
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.value(QStringLiteral("version")).toInt() != 1)
        return;
    QStringList folders;
    for (const QJsonValue &v : root.value(QStringLiteral("folders")).toArray())
        folders << v.toString();
    if (folders != m_folders)
        return;

    QHash<QString, QVariantMap> ext, extEp;
    bool anyFiles = false;
    auto hasFiles = [](const QVariantMap &m) {
        return std::any_of(overlayFileKeys().cbegin(), overlayFileKeys().cend(),
                           [&m](const QString &k) { return !m.value(k).toString().isEmpty(); });
    };
    const QJsonObject extObj = root.value(QStringLiteral("ext")).toObject();
    for (auto it = extObj.begin(); it != extObj.end(); ++it) {
        QVariantMap m = it.value().toObject().toVariantMap();
        if (m.contains(QStringLiteral("genres"))) // QStringList round-trips as a QVariantList
            m.insert(QStringLiteral("genres"), m.value(QStringLiteral("genres")).toStringList());
        anyFiles |= hasFiles(m);
        ext.insert(it.key(), m);
    }
    const QJsonObject epObj = root.value(QStringLiteral("extEpisodes")).toObject();
    for (auto it = epObj.begin(); it != epObj.end(); ++it) {
        const QVariantMap m = it.value().toObject().toVariantMap();
        anyFiles |= hasFiles(m);
        extEp.insert(it.key(), m);
    }
    m_extMeta = ext;
    {
        QMutexLocker lock(&m_mutex);
        m_extEpisodeMeta = extEp;
    }
    if (anyFiles)
        m_overlayCheck = std::make_shared<OverlayCheck>(OverlayCheck{ext, extEp});

    QHash<QString, Title> base;
    for (const QJsonValue &v : root.value(QStringLiteral("titles")).toArray()) {
        Title t = titleFromJson(v.toObject());
        if (!t.id.isEmpty() && !t.files.isEmpty())
            base.insert(t.id, std::move(t));
    }
    applyTitles(std::move(base), true);
    TIMING("scan cache restored: %d titles, %d overlays", int(m_titles.size()), int(m_extMeta.size()));
}

// ---------------------------------------------------------------- auto-rescan / warm-up / cleanup

bool Library::updateWatcher(const QStringList &dirs)
{
    QSet<QString> want(dirs.cbegin(), dirs.cend());
    for (const QString &f : std::as_const(m_folders))
        if (QFileInfo(f).isDir() && want.size() < kMaxWatchedDirs)
            want.insert(QDir::cleanPath(f));
    const QStringList haveList = m_fsWatcher->directories();
    const QSet<QString> have(haveList.cbegin(), haveList.cend());
    QStringList remove, add;
    for (const QString &d : have)
        if (!want.contains(d))
            remove << d;
    for (const QString &d : std::as_const(want))
        if (!have.contains(d))
            add << d;
    if (!remove.isEmpty())
        m_fsWatcher->removePaths(remove);
    QStringList failed;
    if (!add.isEmpty())
        failed = m_fsWatcher->addPaths(add); // unwatchable dirs (permissions, inotify limit) are skipped
    TIMING("watching %d folders%s", int(m_fsWatcher->directories().size()),
           failed.isEmpty() ? "" : qPrintable(QStringLiteral(" (%1 could not be watched)").arg(failed.size())));
    return failed.isEmpty();
}

// Pre-generates backdrop frames (featured title first) after a scan that changed something, and after the
// first scan of a session. Portrait cards are not pre-generated (no view shows them up front) and episode
// stills only per season, on request (warmSeason()). Cached frames cost one stat() per job.
void Library::queueWarmUp()
{
    if (!m_thumbs || qEnvironmentVariableIntValue("QTFLIX_NO_WARMUP") > 0)
        return;
    QStringList order;
    if (m_titles.contains(m_featuredId))
        order << m_featuredId;
    for (const QString &id : std::as_const(m_orderedIds))
        if (id != m_featuredId)
            order << id;
    m_warmBase.clear();
    int skipped = 0;
    for (const QString &id : std::as_const(order)) {
        if (m_warmSkip && m_warmSkip(id)) {
            ++skipped;
            continue;
        }
        m_warmBase << id + QStringLiteral("/backdrop");
    }
    m_warmSeasonJobs.clear(); // files may have changed: seasons are queued again when shown
    m_warmedSeasons.clear();
    TIMING("thumbnail warm-up queued: %d jobs (%d titles skipped)", int(m_warmBase.size()), skipped);
    pushWarmUp();
}

void Library::requeueWarmUp()
{
    if (m_haveScanned)
        queueWarmUp();
}

// ThumbnailProvider::warmUp() replaces its queue, so season jobs go first, then the backdrop list (jobs that
// already ran find their frame cached and return at once).
void Library::pushWarmUp()
{
    if (m_thumbs)
        m_thumbs->warmUp(m_warmSeasonJobs + m_warmBase);
}

void Library::warmSeason(const QString &id, int season)
{
    if (!m_thumbs || qEnvironmentVariableIntValue("QTFLIX_NO_WARMUP") > 0)
        return;
    const Title *t = findTitle(id);
    if (!t || !t->isSeries)
        return;
    const QString key = id + QLatin1Char('/') + QString::number(season);
    if (m_warmedSeasons.contains(key))
        return;
    m_warmedSeasons.insert(key);
    QStringList jobs;
    for (const MediaFile &f : t->files) {
        if (f.season != season)
            continue;
        if (!episodeOverlay(id, f.season, f.episode).value(QStringLiteral("stillFile")).toString().isEmpty())
            continue; // metadata still: nothing to grab
        jobs << QStringLiteral("%1/ep/%2/%3").arg(id).arg(f.season).arg(f.episode);
    }
    if (jobs.isEmpty())
        return;
    m_warmSeasonJobs = jobs + m_warmSeasonJobs;
    if (m_warmSeasonJobs.size() > 300) // older requests drop out first
        m_warmSeasonJobs.resize(300);
    pushWarmUp();
}

// Progress entries of files that were removed from a library folder are dropped (entries of files outside
// the library, or on a folder that is currently missing / unmounted, are kept).
void Library::pruneProgress()
{
    QStringList roots;
    for (const QString &f : std::as_const(m_folders))
        if (QFileInfo(f).isDir())
            roots << QDir::cleanPath(f) + QLatin1Char('/');
    bool removed = false;
    for (auto it = m_progress.begin(); it != m_progress.end();) {
        const QString &p = it.key();
        const bool inLibrary = std::any_of(roots.cbegin(), roots.cend(), [&p](const QString &r) { return p.startsWith(r); });
        if (inLibrary && !m_pathToId.contains(p) && !QFileInfo::exists(p)) {
            it = m_progress.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed) {
        m_resumeCache.clear();
        scheduleSave(kStateSaveMs);
    }
}
