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
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <algorithm>

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

// ---------------------------------------------------------------------------------- directory walk

struct FoundFile {
    QString path;   // absolute
    QString root;   // configured folder containing it
    QFileInfo info;
};

void collectVideos(const QString &dir, const QString &root, QSet<QString> &visitedDirs, QSet<QString> &seenFiles,
                   QVector<FoundFile> &out, int depth, ScanInfo *info)
{
    if (depth > 20)
        return;
    if (info && info->dirs.size() < kMaxWatchedDirs)
        info->dirs << dir;
    QDir d(dir);
    const QFileInfoList entries = d.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable,
                                                  QDir::Name);
    for (const QFileInfo &fi : entries) {
        if (fi.fileName().startsWith(QLatin1Char('.')))
            continue;
        if (fi.isDir()) {
            const QString canon = fi.canonicalFilePath();
            if (canon.isEmpty() || visitedDirs.contains(canon))
                continue;
            visitedDirs.insert(canon);
            collectVideos(fi.absoluteFilePath(), root, visitedDirs, seenFiles, out, depth + 1, info);
        } else if (fi.isFile() && videoExtensions().contains(fi.suffix().toLower())) {
            if (fi.size() <= 0)
                continue;
            if (!fi.isReadable()) {
                if (info) ++info->rejected;
                continue;
            }
            // still being copied / downloaded: skip until it settles (the caller schedules a rescan)
            if (std::llabs(fi.lastModified().msecsTo(QDateTime::currentDateTime())) < 5000) {
                if (info) ++info->unstable;
                continue;
            }
            static const QRegularExpression sample(QStringLiteral("(?<![A-Za-z0-9])sample(?![A-Za-z0-9])"),
                                                   QRegularExpression::CaseInsensitiveOption);
            if (fi.size() < 300LL * 1024 * 1024 && sample.match(fi.completeBaseName()).hasMatch())
                continue;
            const QString canon = fi.canonicalFilePath();
            if (canon.isEmpty() || seenFiles.contains(canon))
                continue;
            seenFiles.insert(canon);
            out.append({fi.absoluteFilePath(), root, fi});
        }
    }
}

// ---------------------------------------------------------------------------------- ffprobe

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
};

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

QHash<QString, Probe> probeAll(const QVector<FoundFile> &files)
{
    const QString cacheFile = cachePath(QStringLiteral("probe-cache.json"));
    QJsonObject cache;
    {
        QFile f(cacheFile);
        if (f.open(QIODevice::ReadOnly))
            cache = QJsonDocument::fromJson(f.readAll()).object();
    }
    QVector<Probe> probes;
    probes.reserve(files.size());
    for (const FoundFile &ff : files) {
        Probe p;
        p.path = ff.path;
        p.mtime = ff.info.lastModified().toMSecsSinceEpoch();
        p.size = ff.info.size();
        const QJsonObject c = cache.value(ff.path).toObject();
        if (!c.isEmpty() && qint64(c.value(QStringLiteral("m")).toDouble()) == p.mtime
            && qint64(c.value(QStringLiteral("s")).toDouble()) == p.size) {
            p.durationMs = qint64(c.value(QStringLiteral("d")).toDouble());
            p.width = c.value(QStringLiteral("w")).toInt();
            p.height = c.value(QStringLiteral("h")).toInt();
            p.codec = c.value(QStringLiteral("c")).toString();
            p.bad = c.value(QStringLiteral("b")).toBool();
            p.video = c.value(QStringLiteral("v")).toBool(true);
            p.cached = true;
            p.ok = !p.bad;
        }
        probes.append(p);
    }

    const QString exe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    bool anyNew = false;
    if (!exe.isEmpty()) {
        QVector<Probe *> todo;
        for (Probe &p : probes)
            if (!p.cached)
                todo.append(&p);
        if (!todo.isEmpty()) {
            QThreadPool pool;
            pool.setMaxThreadCount(4);
            QtConcurrent::blockingMap(&pool, todo, [&exe](Probe *p) { runProbe(*p, exe); });
            anyNew = true;
            // a file whose size changed while we probed it is still being written
            for (Probe *p : std::as_const(todo))
                if (QFileInfo(p->path).size() != p->size)
                    p->unstable = true;
        }
    }

    QHash<QString, Probe> result;
    QJsonObject newCache;
    for (const Probe &p : probes) {
        result.insert(p.path, p);
        if ((p.ok || p.bad) && !p.unstable) {
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
            newCache.insert(p.path, c);
        }
    }
    // keep entries of files outside the current folders too (folder may be re-added later), but cap size
    if (anyNew || newCache.size() != cache.size()) {
        for (auto it = cache.begin(); it != cache.end() && newCache.size() < 20000; ++it)
            if (!newCache.contains(it.key()) && QFileInfo::exists(it.key()))
                newCache.insert(it.key(), it.value());
        QSaveFile f(cacheFile);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(newCache).toJson(QJsonDocument::Compact));
            f.commit();
        }
    }
    return result;
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

QString findArtwork(const QStringList &dirs, const QStringList &baseNames)
{
    static const QStringList exts = {"jpg", "jpeg", "png", "webp"};
    for (const QString &d : dirs) {
        if (d.isEmpty())
            continue;
        for (const QString &b : baseNames)
            for (const QString &e : exts) {
                const QString p = d + QLatin1Char('/') + b + QLatin1Char('.') + e;
                if (QFileInfo::exists(p))
                    return p;
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

QVector<Title> scanFolders(const QStringList &folders, ScanInfo *info)
{
    QElapsedTimer timer;
    timer.start();
    // 1) walk (deepest configured roots first so they claim their files)
    QStringList roots;
    for (const QString &f : folders) {
        const QString c = QDir::cleanPath(f);
        if (QFileInfo(c).isDir() && !roots.contains(c))
            roots << c;
    }
    std::sort(roots.begin(), roots.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
    QSet<QString> visitedDirs, seenFiles;
    QVector<FoundFile> found;
    for (const QString &r : roots) {
        const QString canon = QFileInfo(r).canonicalFilePath();
        if (canon.isEmpty() || visitedDirs.contains(canon))
            continue;
        visitedDirs.insert(canon);
        collectVideos(r, r, visitedDirs, seenFiles, found, 0, info);
    }

    // 2) probe; drop files that are not videos, broken, or still growing
    const QHash<QString, Probe> probes = probeAll(found);
    found.removeIf([&](const FoundFile &ff) {
        const Probe p = probes.value(ff.path);
        if (p.unstable) {
            ++info->unstable;
            return true;
        }
        if (p.bad || (p.ok && !p.video)) {
            ++info->rejected;
            return true;
        }
        return false;
    });

    // 3) parse
    QVector<ParsedFile> parsed;
    parsed.reserve(found.size());
    for (const FoundFile &ff : found)
        parsed.append(parseFile(ff));

    // series folders known from real episodes -> featurettes / extras inside them join the series
    QHash<QString, int> seriesDirOwner; // dir -> index of an episode file
    for (int i = 0; i < parsed.size(); ++i)
        if (parsed[i].isEpisode && !parsed[i].seriesDir.isEmpty() && !seriesDirOwner.contains(parsed[i].seriesDir))
            seriesDirOwner.insert(parsed[i].seriesDir, i);
    QHash<QString, int> extraCounter;
    std::sort(parsed.begin(), parsed.end(), [](const ParsedFile &a, const ParsedFile &b) {
        return QString::compare(a.ff.path, b.ff.path, Qt::CaseInsensitive) < 0;
    });
    // indices changed after the sort: rebuild owner map
    seriesDirOwner.clear();
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
        const Probe pr = probes.value(pf.ff.path);
        mf.durationMs = pr.durationMs;
        mf.width = pr.width;
        mf.height = pr.height;
        mf.videoCodec = pr.codec;
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
            t.posterFile = findArtwork(artDirs, {"poster", "folder", "cover", "show"});
            t.backdropFile = findArtwork(artDirs, {"fanart", "backdrop", "background"});
        }
        if (t.posterFile.isEmpty()) {
            const QString stem = QFileInfo(t.files.first().path).completeBaseName();
            t.posterFile = findArtwork({fileDir}, {stem + QStringLiteral("-poster"), stem});
        }
        if (t.backdropFile.isEmpty()) {
            const QString stem = QFileInfo(t.files.first().path).completeBaseName();
            t.backdropFile = findArtwork({fileDir}, {stem + QStringLiteral("-fanart")});
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
        t.posterFile = findArtwork({dir}, posterNames);
        t.backdropFile = findArtwork({dir}, backdropNames);
        fillCosmetics(t, pf.recording, rootNameOf(pf.ff.root));
        fillDescription(t, pf.recording, rootNameOf(pf.ff.root));
        titles.append(t);
    }
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

    m_watcher = new QFutureWatcher<QVector<Title>>(this);
    connect(m_watcher, &QFutureWatcherBase::finished, this, &Library::onScanFinished);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(2000);
    connect(m_saveTimer, &QTimer::timeout, this, &Library::saveWatchState);

    // Qt 6.10's FFmpeg media backend (hero preview / player) attaches a continuation to a QFuture that
    // reports two results and logs "Parent future has 2 result(s)" on every media load. It is harmless
    // and not actionable from application code, so mute just that category.
    QLoggingCategory::setFilterRules(QStringLiteral("qt.core.qfuture.continuations=false"));

    // auto-rescan: folder changes are debounced for 3 s
    m_rescanTimer = new QTimer(this);
    m_rescanTimer->setSingleShot(true);
    m_rescanTimer->setInterval(3000);
    connect(m_rescanTimer, &QTimer::timeout, this, &Library::rescan);
    m_fsWatcher = new QFileSystemWatcher(this);
    connect(m_fsWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &dir) {
        TIMING("folder changed: %s", qPrintable(dir));
        // debounce, but don't let a constant stream of events (an active download) postpone it forever
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!m_rescanTimer->isActive())
            m_firstChangeMs = now;
        if (now - m_firstChangeMs < 15000)
            m_rescanTimer->start(3000);
    });

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(0);
    connect(m_refreshTimer, &QTimer::timeout, this, &Library::flushRefresh);

    m_cacheSaveTimer = new QTimer(this);
    m_cacheSaveTimer->setSingleShot(true);
    m_cacheSaveTimer->setInterval(1500);
    connect(m_cacheSaveTimer, &QTimer::timeout, this, &Library::saveScanCache);

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

Library::~Library()
{
    if (s_instance == this) s_instance = nullptr;
    saveWatchState();
    if (m_cacheSaveTimer && m_cacheSaveTimer->isActive())
        saveScanCache();
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

void Library::saveWatchState()
{
    if (m_saveTimer)
        m_saveTimer->stop();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    QJsonObject prog;
    for (auto it = m_progress.cbegin(); it != m_progress.cend(); ++it) {
        QJsonObject o;
        o.insert(QStringLiteral("positionMs"), double(it->positionMs));
        o.insert(QStringLiteral("durationMs"), double(it->durationMs));
        o.insert(QStringLiteral("lastPlayed"), it->lastPlayed.toString(Qt::ISODateWithMs));
        prog.insert(it.key(), o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("progress"), prog);
    root.insert(QStringLiteral("myList"), QJsonArray::fromStringList(m_myListIds));
    QSaveFile f(dir + QStringLiteral("/watchstate.json"));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        f.commit();
    }
}

void Library::scheduleSave()
{
    if (!m_saveTimer->isActive())
        m_saveTimer->start();
}

// ---------------------------------------------------------------- folders / scanning

void Library::rescan()
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
    const QStringList folders = m_folders;
    auto extras = std::make_shared<ScanExtras>();
    m_scanExtras = extras;
    TIMING("scan started");
    m_watcher->setFuture(QtConcurrent::run([folders, extras]() { return scanFolders(folders, extras.get()); }));
}

void Library::onScanFinished()
{
    QVector<Title> result = m_watcher->future().resultCount() ? m_watcher->result() : QVector<Title>();
    const std::shared_ptr<ScanExtras> extras = m_scanExtras;
    QHash<QString, Title> base;
    base.reserve(result.size());
    for (Title &t : result) {
        const QString id = t.id;
        base.insert(id, std::move(t));
    }
    if (extras)
        TIMING("scan finished: %d titles in %lld ms (%d still being written, %d rejected)", int(base.size()),
               extras->ms, extras->unstable, extras->rejected);
    const bool first = !m_haveScanned;
    m_haveScanned = true;
    const bool changed = applyTitles(std::move(base), false);
    if (changed || first)
        emit libraryChanged();
    if (extras)
        updateWatcher(extras->dirs);
    if (changed)
        saveScanCache();
    queueWarmUp();

    if (m_rescanPending) {
        rescan();
        return;
    }
    if (extras && extras->unstable > 0 && !m_rescanTimer->isActive())
        m_rescanTimer->start(8000); // pick up files that are still being written once they settle
    m_scanning = false;
    emit scanningChanged();
}

// Installs a new set of scanned titles (from a scan or the scan cache), re-applies the external
// metadata overlays, and updates the models incrementally. Returns true if anything changed.
bool Library::applyTitles(QHash<QString, Title> base, bool fromCache)
{
    QHash<QString, Title> titles;
    QHash<QString, QString> pathToId;
    QHash<QString, QVariantMap> ident;
    titles.reserve(base.size());
    for (auto it = base.cbegin(); it != base.cend(); ++it) {
        const Title &b = it.value();
        ident.insert(b.id, {{QStringLiteral("title"), b.title}, {QStringLiteral("year"), b.year},
                            {QStringLiteral("isSeries"), b.isSeries}});
        Title t = b;
        applyOverlay(t);
        for (const MediaFile &f : t.files)
            pathToId.insert(f.path, t.id);
        titles.insert(t.id, std::move(t));
    }

    QSet<QString> changedIds;
    bool structural = titles.size() != m_titles.size();
    for (auto it = titles.cbegin(); it != titles.cend(); ++it) {
        const auto old = m_titles.constFind(it.key());
        if (old == m_titles.cend())
            structural = true;
        else if (!sameTitle(old.value(), it.value()))
            changedIds.insert(it.key());
    }
    const bool changed = structural || !changedIds.isEmpty();
    m_baseTitles = std::move(base);
    m_parsedIdentity = std::move(ident);
    if (!changed) {
        TIMING("library unchanged");
        return false;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_titles = std::move(titles);
        m_pathToId = std::move(pathToId);
    }
    if (!fromCache)
        pruneProgress();
    rebuildModels(); // diffed: unchanged models keep their rows
    for (const QString &id : std::as_const(changedIds))
        for (TitleModel *m : allModels())
            m->refresh(id);
    if (m_featuredId.isEmpty() || !m_titles.contains(m_featuredId))
        pickFeatured();
    else if (changedIds.contains(m_featuredId))
        emit featuredChanged();
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

void Library::rebuildModels()
{
    QCollator coll(QLocale(QLocale::English));
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    coll.setNumericMode(true);
    m_orderedIds = m_titles.keys();
    std::sort(m_orderedIds.begin(), m_orderedIds.end(), [&](const QString &a, const QString &b) {
        const Title &ta = m_titles[a], &tb = m_titles[b];
        auto key = [](const Title &t) {
            QString s = t.title;
            if (s.startsWith(QLatin1String("The "), Qt::CaseInsensitive))
                s = s.mid(4);
            return s;
        };
        const int c = coll.compare(key(ta), key(tb));
        if (c != 0) return c < 0;
        if (ta.year != tb.year) return ta.year < tb.year;
        return a < b;
    });

    QStringList movies, series;
    for (const QString &id : std::as_const(m_orderedIds))
        (m_titles[id].isSeries ? series : movies) << id;
    m_all->setIds(m_orderedIds);
    m_movies->setIds(movies);
    m_series->setIds(series);
    rebuildContinue();
    rebuildMyList();
    rebuildSearch();
    rebuildRows();
}

QList<TitleModel *> Library::allModels() const
{
    QList<TitleModel *> list = {m_all, m_movies, m_series, m_myList, m_continue, m_search};
    for (RowsModel *rm : {m_homeRows, m_movieRows, m_seriesRows})
        for (TitleModel *m : rm->findChildren<TitleModel *>(QString(), Qt::FindDirectChildrenOnly))
            if (!list.contains(m))
                list << m;
    return list;
}

void Library::refreshTitle(const QString &id)
{
    if (id.isEmpty())
        return;
    for (TitleModel *m : allModels())
        m->refresh(id);
    if (id == m_featuredId)
        emit featuredChanged();
}

void Library::rebuildContinue()
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
    for (const E &e : list)
        if (!ids.contains(e.id))
            ids << e.id;
    m_continue->setIds(ids);
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
        const Title &t = m_titles[id];
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
    // one persistent TitleModel per (RowsModel, kind, name)
    auto rowModel = [this](RowsModel *rm, const QString &key) {
        for (TitleModel *m : rm->findChildren<TitleModel *>(QString(), Qt::FindDirectChildrenOnly))
            if (m->objectName() == key)
                return m;
        auto *m = new TitleModel(this, rm);
        m->setObjectName(key);
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

    auto recent = [this](const QStringList &ids, int n) {
        QStringList l = ids;
        std::stable_sort(l.begin(), l.end(), [this](const QString &a, const QString &b) {
            const Title &ta = m_titles[a], &tb = m_titles[b];
            if (ta.added != tb.added) return ta.added > tb.added;
            return a < b;
        });
        return l.mid(0, n);
    };
    // categories: ordered by size desc, then name
    auto byCategory = [this](const QStringList &ids) {
        QMap<QString, QStringList> cats;
        for (const QString &id : ids)
            cats[m_titles[id].category] << id;
        QList<QPair<QString, QStringList>> list;
        for (auto it = cats.cbegin(); it != cats.cend(); ++it)
            list.append({it.key(), it.value()});
        std::stable_sort(list.begin(), list.end(), [](const auto &a, const auto &b) {
            return a.second.size() > b.second.size();
        });
        return list;
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

        QStringList top = m_orderedIds;
        auto total = [this](const QString &id) {
            qint64 s = 0;
            for (const MediaFile &f : m_titles[id].files) s += f.durationMs;
            return s;
        };
        std::stable_sort(top.begin(), top.end(), [&](const QString &a, const QString &b) {
            const qint64 da = total(a), db = total(b);
            if (da != db) return da > db;
            const auto ea = m_titles[a].files.size(), eb = m_titles[b].files.size();
            if (ea != eb) return ea > eb;
            return a < b;
        });
        addRow(rows, m_homeRows, QStringLiteral("top10"), QStringLiteral("Top 10 in Your Library"), top.mid(0, 10));
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
        // rows that disappeared keep their (persistent) model; empty it so it holds no stale ids
        for (TitleModel *m : rm->findChildren<TitleModel *>(QString(), Qt::FindDirectChildrenOnly)) {
            bool used = false;
            for (const auto &r : rows) used |= r.model == m;
            if (!used) m->setIds({});
        }
    };
    pageRows(m_movieRows, movies, contMovies);
    pageRows(m_seriesRows, series, contSeries);
    for (TitleModel *m : m_homeRows->findChildren<TitleModel *>(QString(), Qt::FindDirectChildrenOnly)) {
        bool used = false;
        for (const auto &r : m_homeRows->rows()) used |= r.model == m;
        if (!used) m->setIds({});
    }
}

// ---------------------------------------------------------------- featured

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
    for (auto it = m_titles.cbegin(); it != m_titles.cend(); ++it) {
        const Title &t = it.value();
        const QString stem = QFileInfo(t.files.first().path).completeBaseName();
        const bool recording = !t.isSeries && (looksLikeRecording(stem) || looksLikeTicket(stem));
        qint64 dur = 0;
        for (const MediaFile &f : t.files) dur = std::max(dur, f.durationMs);
        const bool longEnough = dur > 20 * 60 * 1000;
        const bool real = (t.isSeries || t.year > 0) && !recording;
        if (real && longEnough) tiers[0] << it.key();
        else if (real) tiers[1] << it.key();
        else if (longEnough && !recording) tiers[2] << it.key();
        else tiers[3] << it.key();
    }
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

int Library::resumeIndex(const Title &t) const
{
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
    case TitleModel::SeasonCountRole: {
        if (!t.isSeries) return 0;
        QSet<int> s;
        for (const MediaFile &f : t.files) if (f.season > 0) s.insert(f.season);
        return int(s.isEmpty() ? 1 : s.size());
    }
    case TitleModel::EpisodeCountRole: return int(t.isSeries ? t.files.size() : 0);
    case TitleModel::DurationMsRole: {
        auto f = file();
        if (!f) return qint64(0);
        if (f->durationMs > 0) return f->durationMs;
        return m_progress.value(f->path).durationMs;
    }
    case TitleModel::QualityRole: {
        int w = 0, h = 0;
        for (const MediaFile &f : t.files) { w = std::max(w, f.width); h = std::max(h, f.height); }
        return qualityLabel(w, h);
    }
    case TitleModel::CardImageRole: return QUrl(imageUrl(t, "card"));
    case TitleModel::BackdropImageRole: return QUrl(imageUrl(t, "backdrop"));
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
    case TitleModel::LogoImageRole: return t.logoFile.isEmpty() ? QUrl() : QUrl::fromLocalFile(t.logoFile);
    case TitleModel::HasMetaRole: return t.hasExternalMeta;
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
        const QUrl thumbUrl(QStringLiteral("image://thumbs/%1/ep/%2/%3").arg(t->id).arg(f.season).arg(f.episode));
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
    scheduleSave();
    emit progressChanged(p);
    rebuildContinue();
    rebuildRows();
    refreshTitle(m_pathToId.value(p));
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
    scheduleSave();
    emit progressChanged(p);
    rebuildContinue();
    rebuildRows();
    refreshTitle(m_pathToId.value(p));
}

void Library::clearProgress(const QString &path)
{
    const QString p = normPath(path);
    if (!m_progress.remove(p))
        return;
    scheduleSave();
    emit progressChanged(p);
    rebuildContinue();
    rebuildRows();
    refreshTitle(m_pathToId.value(p));
}

void Library::toggleMyList(const QString &id)
{
    if (id.isEmpty())
        return;
    if (!m_myListIds.removeAll(id))
        m_myListIds << id;
    scheduleSave();
    emit myListChanged(id);
    rebuildMyList();
    rebuildRows();
    refreshTitle(id);
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
    m_cacheSaveTimer->start();
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
    {
        QMutexLocker lock(&m_mutex);
        cur.value() = std::move(t);
    }
    if (artChanged) {
        ++m_artGen[id];
        if (m_thumbs)
            m_thumbs->invalidate(id);
    }
    m_pendingRefresh.insert(id);
    m_pendingResort |= resort;
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
    m_cacheSaveTimer->start();
    if (m_titles.contains(id)) {
        // lets views that show episode data (detail modal / player) re-query episodes()
        m_pendingRefresh.insert(id);
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
    for (const QString &id : ids)
        refreshTitle(id); // also re-emits featuredChanged for the hero
}

// ---------------------------------------------------------------- scan cache

void Library::saveScanCache()
{
    m_cacheSaveTimer->stop();
    QJsonArray titles;
    QStringList ids = m_baseTitles.keys();
    std::sort(ids.begin(), ids.end());
    for (const QString &id : std::as_const(ids))
        titles.append(titleToJson(m_baseTitles.value(id)));
    QJsonObject ext, extEp;
    for (auto it = m_extMeta.cbegin(); it != m_extMeta.cend(); ++it)
        ext.insert(it.key(), QJsonObject::fromVariantMap(it.value()));
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_extEpisodeMeta.cbegin(); it != m_extEpisodeMeta.cend(); ++it)
            extEp.insert(it.key(), QJsonObject::fromVariantMap(it.value()));
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("folders"), QJsonArray::fromStringList(m_folders));
    root.insert(QStringLiteral("titles"), titles);
    root.insert(QStringLiteral("ext"), ext);
    root.insert(QStringLiteral("extEpisodes"), extEp);
    QSaveFile f(cachePath(QStringLiteral("library-cache.json")));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

// Populates the models from the last scan before the first (async) rescan finishes; the rescan then
// only applies the differences.
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

    // overlays: drop a title's overlays when any file they reference is gone (the metadata service then
    // re-applies them from its own cache, or not at all)
    QHash<QString, QVariantMap> ext, extEp;
    QSet<QString> broken;
    auto filesOk = [](const QVariantMap &m) {
        for (const QString &k : overlayFileKeys()) {
            const QString p = m.value(k).toString();
            if (!p.isEmpty() && !QFileInfo::exists(p))
                return false;
        }
        return true;
    };
    const QJsonObject extObj = root.value(QStringLiteral("ext")).toObject();
    for (auto it = extObj.begin(); it != extObj.end(); ++it) {
        QVariantMap m = it.value().toObject().toVariantMap();
        if (m.contains(QStringLiteral("genres"))) // QStringList round-trips as a QVariantList
            m.insert(QStringLiteral("genres"), m.value(QStringLiteral("genres")).toStringList());
        if (!filesOk(m))
            broken.insert(it.key());
        ext.insert(it.key(), m);
    }
    const QJsonObject epObj = root.value(QStringLiteral("extEpisodes")).toObject();
    for (auto it = epObj.begin(); it != epObj.end(); ++it) {
        const QVariantMap m = it.value().toObject().toVariantMap();
        if (!filesOk(m))
            broken.insert(it.key().section(QLatin1Char('/'), 0, 0));
        extEp.insert(it.key(), m);
    }
    for (auto it = ext.begin(); it != ext.end();)
        it = broken.contains(it.key()) ? ext.erase(it) : std::next(it);
    for (auto it = extEp.begin(); it != extEp.end();)
        it = broken.contains(it.key().section(QLatin1Char('/'), 0, 0)) ? extEp.erase(it) : std::next(it);
    m_extMeta = ext;
    {
        QMutexLocker lock(&m_mutex);
        m_extEpisodeMeta = extEp;
    }

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

void Library::updateWatcher(const QStringList &dirs)
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
    if (!add.isEmpty())
        m_fsWatcher->addPaths(add); // unwatchable dirs (permissions, inotify limit) are skipped
    TIMING("watching %d folders", int(m_fsWatcher->directories().size()));
}

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
    QStringList jobs;
    for (const QString &id : std::as_const(order))
        jobs << id + QStringLiteral("/backdrop");
    for (const QString &id : std::as_const(order))
        jobs << id + QStringLiteral("/card");
    for (const QString &id : std::as_const(order)) {
        const Title &t = m_titles[id];
        if (!t.isSeries)
            continue;
        for (const MediaFile &f : t.files)
            jobs << QStringLiteral("%1/ep/%2/%3").arg(id).arg(f.season).arg(f.episode);
    }
    TIMING("thumbnail warm-up queued: %d jobs", int(jobs.size()));
    m_thumbs->warmUp(jobs);
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
    if (removed)
        scheduleSave();
}
