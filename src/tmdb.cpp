#include "tmdb.h"
#include "library.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>
#include <QSet>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <tuple>
#include <utility>

Q_LOGGING_CATEGORY(lcTmdb, "qtflix.tmdb", QtWarningMsg)

namespace {
constexpr int kMaxApiInFlight = 6;       // API requests in flight (TMDB allows ~20 connections, ~50 req/s)
constexpr int kMaxImageInFlight = 8;     // image CDN downloads in flight
constexpr int kMaxAppend = 20;           // append_to_response entries TMDB accepts per request
constexpr int kTimeoutMs = 15000;
constexpr int kMaxJobs = 8;              // titles processed concurrently
constexpr int kMetaVersion = 1;
constexpr qint64 kNotFoundRetrySecs = 7 * 24 * 3600;
constexpr int kOfflineRetryMs = 60 * 1000;

QString yearOf(const QString &date) { return date.size() >= 4 ? date.left(4) : QString(); }

QString cleanQuery(QString s)
{
    static const QRegularExpression separators(QStringLiteral("[._]+"));
    static const QRegularExpression brackets(QStringLiteral("[\\[\\](){}]"));
    s.replace(separators, QStringLiteral(" "));
    s.replace(brackets, QStringLiteral(" "));
    return s.simplified();
}

// Any thread. Temp file + rename: readers never see a partial file, and unlike QSaveFile there is no fsync
// (a cache can be rebuilt; syncing each image stalls on busy disks).
bool writeFileAtomic(const QString &path, const QByteArray &data)
{
    const QString tmp = path + QStringLiteral(".part");
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    }
    const bool ok = f.write(data) == data.size();
    f.close();
    // rename(2) replaces the target atomically (QFile::rename refuses to overwrite)
    if (!ok || std::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(path).constData()) != 0) {
        QFile::remove(tmp);
        return false;
    }
    return true;
}

// Any thread: every cached meta file, by library id.
QHash<QString, QJsonObject> readAllMeta(const QString &dir)
{
    QHash<QString, QJsonObject> all;
    const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files);
    for (const QFileInfo &fi : files) {
        QFile f(fi.filePath());
        if (f.open(QIODevice::ReadOnly)) all.insert(fi.completeBaseName(), QJsonDocument::fromJson(f.readAll()).object());
    }
    return all;
}

// Rank image candidates: language preference, then (optionally) PNG, then votes, then width.
QString pickImage(const QJsonArray &arr, const QStringList &langPref, bool preferPng)
{
    QString best;
    std::tuple<int, int, double, int> bestKey{INT_MAX, INT_MAX, 0, 0};
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        const QString fp = o.value(QStringLiteral("file_path")).toString();
        if (fp.isEmpty()) continue;
        const QString lang = o.value(QStringLiteral("iso_639_1")).toString(); // null -> ""
        int lr = langPref.indexOf(lang);
        if (lr < 0) lr = langPref.size();
        const int pr = preferPng ? (fp.endsWith(QLatin1String(".png"), Qt::CaseInsensitive) ? 0 : 1) : 0;
        const double vote = o.value(QStringLiteral("vote_average")).toDouble();
        const int width = o.value(QStringLiteral("width")).toInt();
        // smaller is better for the first two; larger for the rest
        const auto key = std::make_tuple(lr, pr, -vote, -width);
        if (best.isEmpty() || key < bestKey) { best = fp; bestKey = key; }
    }
    return best;
}

QString extOf(const QString &remotePath, const QString &fallback)
{
    const QString suf = QFileInfo(remotePath).suffix().toLower();
    return suf.isEmpty() ? fallback : suf;
}

QVariantMap toApplyMap(const QJsonObject &meta)
{
    QVariantMap m;
    for (auto it = meta.begin(); it != meta.end(); ++it) {
        const QString k = it.key();
        if (k == QLatin1String("genres")) {
            QStringList g;
            for (const QJsonValue &v : it.value().toArray()) g << v.toString();
            if (!g.isEmpty()) m.insert(k, g);
        } else if (k == QLatin1String("year") || k == QLatin1String("match")) {
            if (it.value().toInt() > 0) m.insert(k, it.value().toInt());
        } else if (k.endsWith(QLatin1String("File"))) {
            const QString f = it.value().toString();
            if (!f.isEmpty() && QFileInfo::exists(f)) m.insert(k, f);
        } else {
            const QString s = it.value().toString();
            if (!s.isEmpty()) m.insert(k, s);
        }
    }
    return m;
}
} // namespace

struct Tmdb::Reply {
    int http = 0;                  // HTTP status, 0 when the request never got a response
    QByteArray body;
    bool netError = false;         // no response at all (DNS, refused, timeout, offline)
    QString error;
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

struct Tmdb::Request {
    QUrl url;
    bool api = true;               // add credentials
    bool interactive = false;      // search/assign: queued first, survives abortAll()
    quint64 gen = 0;
    int attempts = 0;
    std::function<void(const Reply &)> cb;
};

struct Tmdb::Job {
    QString id;
    QString query;                 // parsed title
    int year = 0;
    bool series = false;
    int tmdbId = 0;
    bool explicitMap = false;
    bool force = false;            // re-download images
    bool started = false;
    quint64 gen = 0;
    QList<QPair<QString, int>> attempts; // (query, year or 0)
    int attempt = 0;
    QJsonObject details, ratings, images;
    QList<int> seasons;            // seasons present in the library
    QHash<QString, QSet<int>> localEpisodes; // season -> episode numbers present
    QJsonObject meta;
    QJsonArray episodes;
    int outstanding = 0;
    int detailsHttp = 0;
    bool netError = false;
};

// ---------------------------------------------------------------------------------------------------------------

Tmdb::Tmdb(Library *lib, QObject *parent) : QObject(parent), m_lib(lib)
{
    s_instance = this;
    m_apiBase = qEnvironmentVariable("QTFLIX_TMDB_BASE", QStringLiteral("https://api.themoviedb.org/3"));
    m_imageBase = qEnvironmentVariable("QTFLIX_TMDB_IMAGE_BASE", QStringLiteral("https://image.tmdb.org/t/p"));
    while (m_apiBase.endsWith(u'/')) m_apiBase.chop(1);
    while (m_imageBase.endsWith(u'/')) m_imageBase.chop(1);

    m_nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    m_nam.setTransferTimeout(kTimeoutMs);

    m_apiPauseTimer = new QTimer(this);
    m_apiPauseTimer->setSingleShot(true);
    connect(m_apiPauseTimer, &QTimer::timeout, this, &Tmdb::pump);
    m_ioPool.setMaxThreadCount(1); // in order: a meta file is never overwritten by an older write
    m_ioPool.setExpiryTimeout(10000);

    m_syncTimer = new QTimer(this);
    m_syncTimer->setSingleShot(true);
    m_syncTimer->setInterval(0);
    connect(m_syncTimer, &QTimer::timeout, this, &Tmdb::onLibraryChanged);

    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    m_retryTimer->setInterval(kOfflineRetryMs);
    connect(m_retryTimer, &QTimer::timeout, this, [this]() {
        if (enabled() && (m_keyState == KeyState::Offline || m_keyState == KeyState::Valid)) validateKey();
    });

    loadOverrides();
    m_apiKey = QSettings().value(QStringLiteral("tmdb/apiKey")).toString().trimmed();

    connect(m_lib, &Library::libraryChanged, this, &Tmdb::scheduleSync);
    if (enabled()) validateKey();
    else setStatus(QStringLiteral("Not configured — add an API key"));
    scheduleSync(); // apply the on-disk cache right away (offline, before any network)
    // ... once it is read, off the UI thread
    const QString dir = cacheDir() + QStringLiteral("/meta");
    m_ioPool.start([this, dir]() {
        const QHash<QString, QJsonObject> all = readAllMeta(dir);
        QMetaObject::invokeMethod(this, [this, all]() { metaLoaded(all); }, Qt::QueuedConnection);
    });
}

void Tmdb::metaLoaded(const QHash<QString, QJsonObject> &all)
{
    if (m_metaLoaded) return; // clearCache() came first: what was read is gone
    for (auto it = all.cbegin(); it != all.cend(); ++it)
        if (!m_cache.contains(it.key())) m_cache.insert(it.key(), it.value()); // in-session results are newer
    m_metaLoaded = true;
    qCDebug(lcTmdb) << "meta cache loaded:" << all.size() << "titles";
    if (std::exchange(m_syncWanted, false)) onLibraryChanged();
    if (m_jobs.isEmpty()) emit idle(); // expectsArtwork() answers from the cache from now on
}

void Tmdb::ioThen(std::function<bool()> work, std::function<void(bool)> then)
{
    m_ioPool.start([this, work = std::move(work), then = std::move(then)]() {
        const bool ok = work();
        if (then) QMetaObject::invokeMethod(this, [then, ok]() { then(ok); }, Qt::QueuedConnection);
    });
}

Tmdb::~Tmdb()
{
    m_ioPool.waitForDone(); // pending cache writes; their main-thread continuations are dropped with us
    for (auto it = m_inFlight.begin(); it != m_inFlight.end(); ++it) {
        it.key()->disconnect(this);
        it.key()->abort();
        it.key()->deleteLater();
    }
    m_inFlight.clear();
}

// ---- properties -----------------------------------------------------------------------------------------------

void Tmdb::setApiKey(const QString &rawKey)
{
    const QString key = rawKey.trimmed();
    if (key == m_apiKey) {
        // re-entering the same key after an error retries validation
        if (enabled() && (m_keyState == KeyState::Invalid || m_keyState == KeyState::Offline)) validateKey();
        return;
    }
    m_apiKey = key;
    QSettings().setValue(QStringLiteral("tmdb/apiKey"), key);
    emit apiKeyChanged();
    abortAll();
    m_jobs.clear();
    m_waiting.clear();
    m_batchTotal = m_batchDone = m_batchNetFail = 0;
    if (!enabled()) {
        m_keyState = KeyState::None;
        setBusy();
        setStatus(QStringLiteral("Not configured — add an API key"));
        return;
    }
    validateKey();
}

void Tmdb::setStatus(const QString &s)
{
    if (s == m_status) return;
    m_status = s;
    qCDebug(lcTmdb) << "status:" << s;
    emit statusChanged();
}

void Tmdb::setBusy()
{
    const bool b = m_keyState == KeyState::Validating || !m_jobs.isEmpty();
    const int p = m_jobs.size();
    if (b == m_busy && p == m_pending) return;
    m_busy = b;
    m_pending = p;
    emit busyChanged();
}

void Tmdb::updateStats()
{
    int matched = 0, unmatched = 0, skipped = 0;
    for (const QString &id : std::as_const(m_ids)) {
        const QString s = m_state.value(id);
        if (s == QLatin1String("matched")) ++matched;
        else if (s == QLatin1String("notFound")) ++unmatched;
        else if (s == QLatin1String("skipped") || s == QLatin1String("ignored")) ++skipped;
    }
    m_skipped = skipped;
    m_matched = matched;
    m_unmatched = unmatched;
    emit statsChanged(); // also lets the settings UI refresh per-title states
}

void Tmdb::updateStatus()
{
    setBusy();
    switch (m_keyState) {
    case KeyState::None: setStatus(QStringLiteral("Not configured — add an API key")); return;
    case KeyState::Validating: setStatus(QStringLiteral("Checking API key…")); return;
    case KeyState::Invalid: setStatus(QStringLiteral("Invalid API key")); return;
    case KeyState::Offline: setStatus(QStringLiteral("No network")); return;
    case KeyState::Valid: break;
    }
    if (!m_jobs.isEmpty()) {
        const int total = std::max(m_batchTotal, 1);
        setStatus(QStringLiteral("Fetching %1 of %2…").arg(std::min(m_batchDone + 1, total)).arg(total));
        return;
    }
    if (m_batchNetFail > 0) {
        setStatus(QStringLiteral("No network (%1 %2 not updated)")
                      .arg(m_batchNetFail).arg(m_batchNetFail == 1 ? QStringLiteral("title") : QStringLiteral("titles")));
        return;
    }
    if (m_ids.isEmpty()) {
        setStatus(m_lib->scanning() ? QStringLiteral("Waiting for the library scan…") : QStringLiteral("Library is empty"));
        return;
    }
    QString s = QStringLiteral("Up to date (%1 matched, %2 not found").arg(m_matched).arg(m_unmatched);
    if (m_skipped > 0) s += QStringLiteral(", %1 skipped").arg(m_skipped);
    setStatus(s + u')');
}

// ---- network ----------------------------------------------------------------------------------------------------

QUrl Tmdb::apiUrl(const QString &path, const QList<QPair<QString, QString>> &query) const
{
    QUrl url(m_apiBase + path);
    QUrlQuery q;
    for (const auto &kv : query) q.addQueryItem(kv.first, QString::fromUtf8(QUrl::toPercentEncoding(kv.second)));
    if (!m_apiKey.startsWith(QLatin1String("eyJ"))) q.addQueryItem(QStringLiteral("api_key"), m_apiKey);
    url.setQuery(q);
    return url;
}

void Tmdb::apiGet(const QString &path, const QList<QPair<QString, QString>> &query, bool interactive,
                  std::function<void(const Reply &)> cb)
{
    auto r = std::make_shared<Request>();
    r->url = apiUrl(path, query);
    r->api = true;
    r->interactive = interactive;
    r->gen = m_gen;
    r->cb = std::move(cb);
    enqueue(r);
}

void Tmdb::fetchFile(const QUrl &url, std::function<void(const Reply &)> cb)
{
    auto r = std::make_shared<Request>();
    r->url = url;
    r->api = false;
    r->gen = m_gen;
    r->cb = std::move(cb);
    enqueue(r);
}

void Tmdb::enqueue(const RequestPtr &r)
{
    if (!r->api) {
        m_imgQueue.append(r);
    } else if (r->interactive) {
        int i = 0;
        while (i < m_queue.size() && m_queue.at(i)->interactive) ++i;
        m_queue.insert(i, r);
    } else {
        m_queue.append(r);
    }
    pump();
}

void Tmdb::pump()
{
    while (m_apiInFlight < kMaxApiInFlight && !m_queue.isEmpty() && !m_apiPauseTimer->isActive())
        startRequest(m_queue.takeFirst());
    while (m_imgInFlight < kMaxImageInFlight && !m_imgQueue.isEmpty())
        startRequest(m_imgQueue.takeFirst());
}

void Tmdb::startRequest(const RequestPtr &r)
{
    QNetworkRequest req(r->url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(kTimeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArrayLiteral("QtFlix/1.0"));
    if (r->api) {
        req.setRawHeader("Accept", "application/json");
        if (m_apiKey.startsWith(QLatin1String("eyJ")))
            req.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());
    }
    QNetworkReply *reply = m_nam.get(req);
    m_inFlight.insert(reply, r);
    ++(r->api ? m_apiInFlight : m_imgInFlight);
    ++r->attempts;
    qCDebug(lcTmdb) << "GET" << r->url.toString(QUrl::RemoveQuery) << "in flight" << m_apiInFlight << m_imgInFlight;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const RequestPtr fin = m_inFlight.take(reply);
        reply->deleteLater();
        if (!fin) return;
        --(fin->api ? m_apiInFlight : m_imgInFlight);
        if (!fin->interactive && fin->gen != m_gen) { pump(); return; } // stale (aborted generation)

        Reply rep;
        rep.http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        rep.body = reply->readAll();
        rep.error = reply->errorString();
        rep.netError = rep.http == 0 && reply->error() != QNetworkReply::NoError;

        if (rep.http == 429 && fin->attempts < 4) {   // rate limited: retry after the advertised delay
            int secs = reply->rawHeader("Retry-After").toInt();
            if (secs <= 0) secs = 2;
            secs = std::min(secs, 30);
            if (fin->api) {
                // the whole API lane backs off; the request goes back to the front of its queue
                qCInfo(lcTmdb) << "rate limited, pausing API requests for" << secs << "s";
                if (!m_apiPauseTimer->isActive() || m_apiPauseTimer->remainingTime() < secs * 1000)
                    m_apiPauseTimer->start(secs * 1000);
                int i = 0;
                if (!fin->interactive)
                    while (i < m_queue.size() && m_queue.at(i)->interactive) ++i;
                m_queue.insert(i, fin);
            } else {
                const quint64 gen = m_gen;
                QTimer::singleShot(secs * 1000, this, [this, fin, gen]() { if (fin->gen == gen) enqueue(fin); });
            }
            pump();
            return;
        }
        pump(); // refill the free slot right away
        if (rep.http >= 500 && fin->attempts < 2) {   // transient server error: one retry
            QTimer::singleShot(1500, this, [this, fin]() { if (fin->interactive || fin->gen == m_gen) enqueue(fin); });
            return;
        }
        if (rep.netError) qCInfo(lcTmdb) << "network error" << fin->url.toString(QUrl::RemoveQuery) << rep.error;
        fin->cb(rep);
    });
}

void Tmdb::abortAll()
{
    ++m_gen;
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(), [](const RequestPtr &r) { return !r->interactive; }),
                  m_queue.end());
    m_imgQueue.clear();
    const auto replies = m_inFlight.keys();
    for (QNetworkReply *reply : replies) {
        const RequestPtr r = m_inFlight.value(reply);
        if (r->interactive) continue;
        --(r->api ? m_apiInFlight : m_imgInFlight);
        m_inFlight.remove(reply);
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    pump();
}

// ---- key validation ---------------------------------------------------------------------------------------------

void Tmdb::validateKey()
{
    if (!enabled()) return;
    m_retryTimer->stop();
    m_keyState = KeyState::Validating;
    updateStatus();
    const QString key = m_apiKey;
    apiGet(QStringLiteral("/configuration"), {}, true, [this, key](const Reply &r) {
        if (key != m_apiKey) return; // key changed meanwhile
        if (r.http == 200) {
            m_keyState = KeyState::Valid;
            m_batchTotal = m_batchDone = m_batchNetFail = 0;
            updateStatus();
            onLibraryChanged(); // re-apply + fetch whatever is missing
        } else if (r.http == 401 || r.http == 403) {
            m_keyState = KeyState::Invalid;
            updateStatus();
        } else if (r.netError) {
            m_keyState = KeyState::Offline;
            updateStatus();
            m_retryTimer->start();
        } else {
            m_keyState = KeyState::Offline;
            setBusy();
            setStatus(QStringLiteral("TMDB unavailable (HTTP %1)").arg(r.http));
            m_retryTimer->start();
        }
    });
}

void Tmdb::onAuthFailed()
{
    abortAll();
    m_jobs.clear();
    m_waiting.clear();
    m_keyState = KeyState::Invalid;
    updateStatus();
}

// ---- cache ------------------------------------------------------------------------------------------------------

QString Tmdb::cacheDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/tmdb");
}

QString Tmdb::metaFile(const QString &id) const { return cacheDir() + QStringLiteral("/meta/") + id + QStringLiteral(".json"); }

QJsonObject Tmdb::loadMeta(const QString &id)
{
    auto it = m_cache.constFind(id);
    if (it != m_cache.constEnd()) return it.value();
    if (m_metaLoaded) return {}; // m_cache holds everything on disk (writes go through it)
    QJsonObject o;               // only before the background load finished
    QFile f(metaFile(id));
    if (f.open(QIODevice::ReadOnly)) o = QJsonDocument::fromJson(f.readAll()).object();
    m_cache.insert(id, o);
    return o;
}

void Tmdb::saveMeta(const QString &id, const QJsonObject &o)
{
    m_cache.insert(id, o);
    const QString file = metaFile(id);
    ioThen([file, o]() { return writeFileAtomic(file, QJsonDocument(o).toJson(QJsonDocument::Indented)); });
}

void Tmdb::loadOverrides()
{
    QFile f(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/tmdb_overrides.json"));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = o.begin(); it != o.end(); ++it) m_overrides.insert(it.key(), it.value().toObject());
}

void Tmdb::saveOverrides()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QJsonObject o;
    for (auto it = m_overrides.begin(); it != m_overrides.end(); ++it) o.insert(it.key(), it.value());
    const QString file = dir + QStringLiteral("/tmdb_overrides.json");
    ioThen([file, o]() { return writeFileAtomic(file, QJsonDocument(o).toJson(QJsonDocument::Indented)); });
}

void Tmdb::applyMeta(const QString &id, const QJsonObject &o, bool force)
{
    if (!o.value(QStringLiteral("matched")).toBool()) return;
    // Skip titles whose overlay the Library still holds (avoids churn on every libraryChanged).
    if (!force) {
        const Title *t = m_lib->findTitle(id);
        if (t && t->hasExternalMeta) return;
    }
    const QVariantMap meta = toApplyMap(o.value(QStringLiteral("meta")).toObject());
    m_applying = true;
    m_lib->setExternalMetadata(id, meta);
    for (const QJsonValue &v : o.value(QStringLiteral("episodes")).toArray()) {
        const QJsonObject e = v.toObject();
        QVariantMap em;
        if (!e.value(QStringLiteral("title")).toString().isEmpty()) em.insert(QStringLiteral("title"), e.value(QStringLiteral("title")).toString());
        if (!e.value(QStringLiteral("description")).toString().isEmpty()) em.insert(QStringLiteral("description"), e.value(QStringLiteral("description")).toString());
        const QString still = e.value(QStringLiteral("stillFile")).toString();
        if (!still.isEmpty() && QFileInfo::exists(still)) em.insert(QStringLiteral("stillFile"), still);
        if (!em.isEmpty())
            m_lib->setExternalEpisodeMetadata(id, e.value(QStringLiteral("season")).toInt(), e.value(QStringLiteral("episode")).toInt(), em);
    }
    m_applying = false;
}

void Tmdb::revertMeta(const QString &id, const QJsonObject &o)
{
    m_applying = true;
    m_lib->setExternalMetadata(id, {}); // empty map == clear overlay (see CONTRACT.md, Round 2)
    for (const QJsonValue &v : o.value(QStringLiteral("episodes")).toArray()) {
        const QJsonObject e = v.toObject();
        m_lib->setExternalEpisodeMetadata(id, e.value(QStringLiteral("season")).toInt(), e.value(QStringLiteral("episode")).toInt(), {});
    }
    m_applying = false;
}

// Screen recordings, dated captures and ticket-style clip names: never sent to TMDB.
bool Tmdb::isJunk(const QString &id, const QVariantMap &ident) const
{
    // recording words and dates in one pattern (the date part has no letters, case doesn't matter)
    static const QRegularExpression wordsOrDate(QStringLiteral(
        "record|screencast|screen|kooha|capture|obs[ _-]|vokoscreen"
        "|(\\b|_)(19|20)\\d\\d[-_.](0?[1-9]|1[0-2])[-_.](0?[1-9]|[12]\\d|3[01])"  // 2026-08-05
        "|(\\b|_)(0?[1-9]|1[0-2])[-_./](0?[1-9]|[12]\\d|3[01])[-_./](19|20)\\d\\d" // 6_7_2026
        "|(19|20)\\d{6}[-_ ]?\\d{4,6}"),                                          // 20260607_112735
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ticket(QStringLiteral("^[A-Z]{2,}-\\d+"));
    const QString t = ident.value(QStringLiteral("title")).toString();
    if (t.trimmed().isEmpty()) return true;
    // the file the title plays first (movie / first regular episode)
    QString file;
    if (const Title *title = m_lib->findTitle(id)) {
        for (const MediaFile &f : title->files)
            if (!title->isSeries || f.season != 0) { file = f.path; break; }
        if (file.isEmpty() && !title->files.isEmpty()) file = title->files.first().path;
    }
    file = QFileInfo(file).fileName();
    // memo: the regexes only run when a title or its file name changes
    const QString memoKey = t + u'\n' + file;
    const auto memo = m_junk.constFind(id);
    if (memo != m_junk.cend() && memo->first == memoKey) return memo->second;
    auto junk = [](const QString &s) { return wordsOrDate.match(s).hasMatch() || ticket.match(s).hasMatch(); };
    const bool result = junk(t) || junk(file);
    m_junk.insert(id, {memoKey, result});
    return result;
}

bool Tmdb::needsFetch(const QString &id, const QVariantMap &ident, const QJsonObject &o) const
{
    if (o.isEmpty() || o.value(QStringLiteral("version")).toInt() != kMetaVersion) return true;
    const auto ov = m_overrides.constFind(id);
    if (ov != m_overrides.constEnd()) {
        if (ov->value(QStringLiteral("tmdbId")).toInt() != o.value(QStringLiteral("tmdbId")).toInt()
            || ov->value(QStringLiteral("series")).toBool() != o.value(QStringLiteral("series")).toBool())
            return true;
    } else {
        if (o.value(QStringLiteral("explicit")).toBool()) return true; // mapping was removed
        const QJsonObject idj = o.value(QStringLiteral("identity")).toObject();
        if (idj.value(QStringLiteral("title")).toString() != ident.value(QStringLiteral("title")).toString()
            || idj.value(QStringLiteral("year")).toInt() != ident.value(QStringLiteral("year")).toInt()
            || idj.value(QStringLiteral("isSeries")).toBool() != ident.value(QStringLiteral("isSeries")).toBool())
            return true;
    }
    if (!o.value(QStringLiteral("matched")).toBool()) {
        const qint64 checked = o.value(QStringLiteral("checked")).toInteger();
        return QDateTime::currentSecsSinceEpoch() - checked > kNotFoundRetrySecs;
    }
    if (o.value(QStringLiteral("series")).toBool()) {
        QSet<int> have;
        for (const QJsonValue &v : o.value(QStringLiteral("seasons")).toArray()) have.insert(v.toInt());
        for (const QVariant &s : m_lib->seasons(id))
            if (s.toInt() > 0 && !have.contains(s.toInt())) return true;
    }
    return false;
}

// ---- library sync -----------------------------------------------------------------------------------------------

void Tmdb::scheduleSync()
{
    if (m_applying) return; // our own overlay application
    m_syncTimer->start();
}

void Tmdb::onLibraryChanged()
{
    QElapsedTimer passTimer;
    passTimer.start();
    if (!m_metaLoaded) { m_syncWanted = true; return; } // metaLoaded() syncs
    m_ids = m_lib->allTitles()->ids();
    const bool canFetch = enabled() && m_keyState == KeyState::Valid;
    // Featured title first, so the billboard updates early.
    QStringList order = m_ids;
    const QString featured = m_lib->featured().value(QStringLiteral("id")).toString();
    if (order.removeOne(featured)) order.prepend(featured);

    for (const QString &id : std::as_const(order)) {
        const QVariantMap ident = m_lib->parsedIdentity(id);
        const QJsonObject o = loadMeta(id);
        const auto ov = m_overrides.constFind(id);
        const bool hasOverride = ov != m_overrides.constEnd();
        if (hasOverride && ov->value(QStringLiteral("tmdbId")).toInt() == 0) { m_state[id] = QStringLiteral("ignored"); continue; }
        if (!hasOverride && isJunk(id, ident)) {
            if (m_state.value(id) != QLatin1String("skipped")) qCDebug(lcTmdb) << "skipping (looks like a recording):" << ident.value(QStringLiteral("title")).toString();
            m_state[id] = QStringLiteral("skipped");
            continue;
        }

        if (o.value(QStringLiteral("matched")).toBool()) {
            applyMeta(id, o, false);
            m_state[id] = QStringLiteral("matched");
        } else if (!o.isEmpty()) {
            m_state[id] = QStringLiteral("notFound");
        } else if (!m_jobs.contains(id)) {
            m_state[id] = QStringLiteral("new");
        }
        if (canFetch && !m_jobs.contains(id) && needsFetch(id, ident, o)) {
            if (hasOverride) queueJob(id, false, false, ov->value(QStringLiteral("tmdbId")).toInt(), ov->value(QStringLiteral("series")).toBool(), true);
            else queueJob(id, false, false);
        }
    }
    updateStats();
    startMoreJobs();
    updateStatus();
    qCDebug(lcTmdb, "sync pass: %d titles in %.2f ms (UI thread)", int(m_ids.size()), passTimer.nsecsElapsed() / 1e6);
}

void Tmdb::queueJob(const QString &id, bool force, bool front, int tmdbId, bool series, bool explicitMap)
{
    if (m_jobs.isEmpty()) m_batchTotal = m_batchDone = m_batchNetFail = 0;
    if (m_jobs.contains(id)) {           // replace an older job for the same title
        m_jobs.remove(id);
        m_waiting.removeAll(id);
        ++m_batchDone;
    }
    const QVariantMap ident = m_lib->parsedIdentity(id);
    auto job = std::make_shared<Job>();
    job->id = id;
    job->query = cleanQuery(ident.value(QStringLiteral("title")).toString());
    job->year = ident.value(QStringLiteral("year")).toInt();
    job->series = explicitMap ? series : ident.value(QStringLiteral("isSeries")).toBool();
    job->tmdbId = tmdbId;
    job->explicitMap = explicitMap;
    job->force = force;
    job->gen = m_gen;
    if (job->series || ident.value(QStringLiteral("isSeries")).toBool()) {
        for (const QVariant &s : m_lib->seasons(id)) {
            if (s.toInt() <= 0) continue; // local "season 0" = featurettes/extras, not TMDB specials
            job->seasons << s.toInt();
            QSet<int> eps;
            for (const QVariant &e : m_lib->episodes(id, s.toInt()))
                eps.insert(e.toMap().value(QStringLiteral("episode")).toInt());
            job->localEpisodes.insert(QString::number(s.toInt()), eps);
        }
    }
    // search attempts: with year, without year, then dropping trailing words (keep >= 2 words)
    if (!job->query.isEmpty()) {
        if (job->year > 0) job->attempts.append({job->query, job->year});
        job->attempts.append({job->query, 0});
        QStringList words = job->query.split(u' ', Qt::SkipEmptyParts);
        while (words.size() > 2) {
            words.removeLast();
            job->attempts.append({words.join(u' '), 0});
        }
    }
    m_jobs.insert(id, job);
    if (front) m_waiting.prepend(id);
    else m_waiting.append(id);
    m_state[id] = QStringLiteral("pending");
    ++m_batchTotal;
}

void Tmdb::startMoreJobs()
{
    if (m_keyState != KeyState::Valid) return;
    int running = 0;
    for (const JobPtr &j : std::as_const(m_jobs)) running += j->started ? 1 : 0;
    while (running < kMaxJobs && !m_waiting.isEmpty()) {
        const JobPtr job = m_jobs.value(m_waiting.takeFirst());
        if (!job) continue;
        job->started = true;
        ++running;
        if (job->tmdbId > 0) fetchDetails(job);
        else runSearch(job);
    }
    setBusy();
}

bool Tmdb::alive(const JobPtr &job) const
{
    return job->gen == m_gen && m_jobs.value(job->id) == job;
}

void Tmdb::runSearch(const JobPtr &job)
{
    if (job->attempt >= job->attempts.size()) { finishNotFound(job); return; }
    const auto [q, year] = job->attempts.at(job->attempt);
    QList<QPair<QString, QString>> query{{QStringLiteral("query"), q}, {QStringLiteral("include_adult"), QStringLiteral("false")},
                                         {QStringLiteral("language"), QStringLiteral("en-US")}};
    if (year > 0) query.append({job->series ? QStringLiteral("first_air_date_year") : QStringLiteral("year"), QString::number(year)});
    apiGet(job->series ? QStringLiteral("/search/tv") : QStringLiteral("/search/movie"), query, job->explicitMap,
           [this, job](const Reply &r) {
        if (!alive(job)) return;
        if (r.http != 200) { failJob(job, r); return; }
        const QJsonArray results = r.json().value(QStringLiteral("results")).toArray();
        if (!results.isEmpty()) {
            job->tmdbId = results.first().toObject().value(QStringLiteral("id")).toInt();
            qCDebug(lcTmdb) << "matched" << job->query << "->" << job->tmdbId << "attempt" << job->attempt;
            fetchDetails(job);
            return;
        }
        ++job->attempt;
        runSearch(job);
    });
}

// One request for details + certification + images (+ the library's seasons, as many as fit) using
// append_to_response; seasons that don't fit are fetched separately in detailsDone().
void Tmdb::fetchDetails(const JobPtr &job)
{
    const QString base = (job->series ? QStringLiteral("/tv/") : QStringLiteral("/movie/")) + QString::number(job->tmdbId);
    const QString ratingsKey = job->series ? QStringLiteral("content_ratings") : QStringLiteral("release_dates");
    QStringList append{ratingsKey, QStringLiteral("images")};
    for (int season : std::as_const(job->seasons))
        if (append.size() < kMaxAppend) append << QStringLiteral("season/%1").arg(season);
    const QList<QPair<QString, QString>> query{{QStringLiteral("language"), QStringLiteral("en-US")},
                                               {QStringLiteral("append_to_response"), append.join(u',')},
                                               {QStringLiteral("include_image_language"), QStringLiteral("en,null")}};
    apiGet(base, query, job->explicitMap, [this, job, ratingsKey](const Reply &r) {
        if (!alive(job)) return;
        job->detailsHttp = r.http;
        if (r.http == 200) {
            job->details = r.json();
            job->ratings = job->details.value(ratingsKey).toObject();
            job->images = job->details.value(QStringLiteral("images")).toObject();
        } else if (r.http == 401) {
            onAuthFailed();
            return;
        } else if (r.netError) {
            job->netError = true;
        } else {
            qCInfo(lcTmdb) << "details failed" << job->tmdbId << r.http;
        }
        detailsDone(job);
    });
}

// Records a season's episodes that exist locally and downloads their stills. Caller holds a jobStep guard.
void Tmdb::addSeason(const JobPtr &job, int season, const QJsonObject &data)
{
    const QString key = (job->series ? QStringLiteral("tv-") : QStringLiteral("movie-")) + QString::number(job->tmdbId);
    const QString imgDir = cacheDir() + QStringLiteral("/images/");
    const QSet<int> local = job->localEpisodes.value(QString::number(season));
    for (const QJsonValue &v : data.value(QStringLiteral("episodes")).toArray()) {
        const QJsonObject e = v.toObject();
        const int ep = e.value(QStringLiteral("episode_number")).toInt();
        if (!local.contains(ep)) continue;
        QJsonObject em{{QStringLiteral("season"), season}, {QStringLiteral("episode"), ep},
                       {QStringLiteral("title"), e.value(QStringLiteral("name")).toString()},
                       {QStringLiteral("description"), e.value(QStringLiteral("overview")).toString()}};
        const int idx = job->episodes.size();
        job->episodes.append(em);
        const QString still = e.value(QStringLiteral("still_path")).toString();
        if (still.isEmpty()) continue;
        const QString file = imgDir + key + QStringLiteral("-s%1e%2.").arg(season, 2, 10, QChar(u'0')).arg(ep, 2, 10, QChar(u'0'))
                             + extOf(still, QStringLiteral("jpg"));
        ++job->outstanding;
        // shown at ~400x225 (episode list); w300 looked soft
        download(job, still, QStringLiteral("w500"), file, QString(), [this, job, idx, file](bool ok) {
            if (ok) {
                QJsonObject epObj = job->episodes.at(idx).toObject();
                epObj.insert(QStringLiteral("stillFile"), file);
                job->episodes.replace(idx, epObj);
            }
            jobStep(job);
        });
    }
}

void Tmdb::detailsDone(const JobPtr &job)
{
    if (job->netError) { Reply r; r.netError = true; failJob(job, r); return; }
    const QJsonObject &d = job->details;
    if (job->detailsHttp == 404) { finishNotFound(job); return; } // id vanished
    if (d.isEmpty()) { Reply r; r.http = job->detailsHttp; failJob(job, r); return; }

    QJsonObject meta;
    meta.insert(QStringLiteral("title"), d.value(job->series ? QStringLiteral("name") : QStringLiteral("title")).toString());
    const QString date = d.value(job->series ? QStringLiteral("first_air_date") : QStringLiteral("release_date")).toString();
    if (!yearOf(date).isEmpty()) meta.insert(QStringLiteral("year"), yearOf(date).toInt());
    meta.insert(QStringLiteral("description"), d.value(QStringLiteral("overview")).toString());
    QJsonArray genres;
    for (const QJsonValue &g : d.value(QStringLiteral("genres")).toArray()) {
        const QString n = g.toObject().value(QStringLiteral("name")).toString();
        if (!n.isEmpty()) genres.append(n);
    }
    meta.insert(QStringLiteral("genres"), genres);
    // certification (US)
    QString rating;
    for (const QJsonValue &v : job->ratings.value(QStringLiteral("results")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("iso_3166_1")).toString() != QLatin1String("US")) continue;
        if (job->series) {
            rating = o.value(QStringLiteral("rating")).toString();
        } else {
            // prefer the theatrical release (type 3), else any non-empty certification
            QString any;
            for (const QJsonValue &rv : o.value(QStringLiteral("release_dates")).toArray()) {
                const QJsonObject rd = rv.toObject();
                const QString c = rd.value(QStringLiteral("certification")).toString().trimmed();
                if (c.isEmpty()) continue;
                if (rd.value(QStringLiteral("type")).toInt() == 3) { rating = c; break; }
                if (any.isEmpty()) any = c;
            }
            if (rating.isEmpty()) rating = any;
        }
        break;
    }
    meta.insert(QStringLiteral("rating"), rating);
    const double vote = d.value(QStringLiteral("vote_average")).toDouble();
    if (vote > 0) meta.insert(QStringLiteral("match"), std::clamp(int(std::lround(80 + vote * 2)), 85, 99));
    job->meta = meta;

    // artwork
    QString poster = pickImage(job->images.value(QStringLiteral("posters")).toArray(), {QStringLiteral("en"), QString()}, false);
    QString backdrop = pickImage(job->images.value(QStringLiteral("backdrops")).toArray(), {QString(), QStringLiteral("en")}, false);
    const QString logo = pickImage(job->images.value(QStringLiteral("logos")).toArray(), {QStringLiteral("en"), QString()}, true);
    if (poster.isEmpty()) poster = d.value(QStringLiteral("poster_path")).toString();
    if (backdrop.isEmpty()) backdrop = d.value(QStringLiteral("backdrop_path")).toString();

    const QString key = (job->series ? QStringLiteral("tv-") : QStringLiteral("movie-")) + QString::number(job->tmdbId);
    const QString imgDir = cacheDir() + QStringLiteral("/images/"); // created by the first write

    job->outstanding = 1; // guard while scheduling
    auto addImage = [&](const QString &remote, const QString &size, const QString &kind, const QString &metaKey) {
        if (remote.isEmpty()) return;
        QString fetchPath = remote, fallback;
        QString ext = extOf(remote, QStringLiteral("jpg"));
        if (kind == QLatin1String("logo") && ext == QLatin1String("svg")) {
            // TMDB renders SVG logos as PNG when asked for .png; keep the SVG as fallback
            fallback = remote;
            fetchPath = remote.left(remote.size() - 3) + QStringLiteral("png");
            ext = QStringLiteral("png");
        }
        const QString file = imgDir + key + u'-' + kind + u'.' + ext;
        ++job->outstanding;
        download(job, fetchPath, size, file, fallback, [this, job, metaKey, file](bool ok) {
            if (ok) job->meta.insert(metaKey, file);
            jobStep(job);
        });
    };
    addImage(poster, QStringLiteral("w500"), QStringLiteral("poster"), QStringLiteral("posterFile"));
    addImage(backdrop, QStringLiteral("w1280"), QStringLiteral("backdrop"), QStringLiteral("backdropFile"));
    addImage(logo, QStringLiteral("w500"), QStringLiteral("logo"), QStringLiteral("logoFile"));

    if (job->series) {
        for (int season : std::as_const(job->seasons)) {
            const QString appended = QStringLiteral("season/%1").arg(season);
            if (d.contains(appended)) { // came with the details
                addSeason(job, season, d.value(appended).toObject());
                continue;
            }
            if (job->seasons.indexOf(season) + 2 < kMaxAppend) continue; // was asked for: TMDB doesn't know it
            ++job->outstanding;
            apiGet(QStringLiteral("/tv/%1/season/%2").arg(job->tmdbId).arg(season), {{QStringLiteral("language"), QStringLiteral("en-US")}},
                   job->explicitMap, [this, job, season](const Reply &r) {
                if (!alive(job)) return;
                if (r.http == 401) { onAuthFailed(); return; }
                if (r.netError) job->netError = true;
                if (r.http == 200) addSeason(job, season, r.json());
                jobStep(job);
            });
        }
    }
    jobStep(job); // release the guard
}

void Tmdb::download(const JobPtr &job, const QString &remotePath, const QString &size, const QString &file,
                    const QString &fallbackRemotePath, std::function<void(bool)> done)
{
    if (!job->force && QFileInfo(file).size() > 0) { done(true); return; }
    const QUrl url(m_imageBase + u'/' + size + remotePath);
    fetchFile(url, [this, job, file, fallbackRemotePath, done](const Reply &r) {
        if (!alive(job)) return;
        if (r.http == 200 && !r.body.isEmpty()) {
            QString target = file;
            if (r.body.startsWith("<?xml") || r.body.startsWith("<svg")) // server sent the SVG itself
                target = file.left(file.lastIndexOf(u'.')) + QStringLiteral(".svg");
            const QByteArray body = r.body;
            ioThen([target, file, body]() {
                if (!writeFileAtomic(target, body)) return false;
                if (target != file) QFile::remove(file); // finishJob() points the overlay at the .svg
                return true;
            }, [this, job, done](bool ok) {
                if (alive(job)) done(ok);
            });
            return;
        }
        if (r.netError) job->netError = true;
        if (!r.netError && !fallbackRemotePath.isEmpty()) {
            const QString svgFile = file.left(file.lastIndexOf(u'.')) + QStringLiteral(".svg");
            download(job, fallbackRemotePath, QStringLiteral("original"), svgFile, QString(), done);
            return;
        }
        done(false);
    });
}

void Tmdb::jobStep(const JobPtr &job)
{
    if (!alive(job)) return;
    if (--job->outstanding == 0) finishJob(job);
}

void Tmdb::finishJob(const JobPtr &job)
{
    // Normalize image paths: an SVG logo may have been stored under .svg instead of .png.
    for (const QString &k : {QStringLiteral("posterFile"), QStringLiteral("backdropFile"), QStringLiteral("logoFile")}) {
        const QString f = job->meta.value(k).toString();
        if (f.isEmpty() || QFileInfo::exists(f)) continue;
        const QString svg = f.left(f.lastIndexOf(u'.')) + QStringLiteral(".svg");
        if (QFileInfo::exists(svg)) job->meta.insert(k, svg);
        else job->meta.remove(k);
    }
    const QVariantMap ident = m_lib->parsedIdentity(job->id);
    QJsonArray seasons;
    for (int s : std::as_const(job->seasons)) seasons.append(s);
    QJsonObject o{{QStringLiteral("version"), kMetaVersion},
                  {QStringLiteral("matched"), true},
                  {QStringLiteral("tmdbId"), job->tmdbId},
                  {QStringLiteral("series"), job->series},
                  {QStringLiteral("explicit"), job->explicitMap},
                  {QStringLiteral("checked"), QDateTime::currentSecsSinceEpoch()},
                  {QStringLiteral("identity"), QJsonObject::fromVariantMap(ident)},
                  {QStringLiteral("seasons"), seasons},
                  {QStringLiteral("meta"), job->meta},
                  {QStringLiteral("episodes"), job->episodes}};
    if (job->netError) {
        // Partial result (network dropped mid-way): show it now, but don't persist so the next sync retries.
        m_cache.insert(job->id, o);
        ++m_batchNetFail;
    } else {
        saveMeta(job->id, o);
    }
    applyMeta(job->id, o, true);
    m_state[job->id] = QStringLiteral("matched");
    endJob(job);
}

void Tmdb::finishNotFound(const JobPtr &job)
{
    const QJsonObject old = loadMeta(job->id);
    if (old.value(QStringLiteral("matched")).toBool()) revertMeta(job->id, old);
    const QVariantMap ident = m_lib->parsedIdentity(job->id);
    QJsonObject o{{QStringLiteral("version"), kMetaVersion},
                  {QStringLiteral("matched"), false},
                  {QStringLiteral("tmdbId"), job->explicitMap ? job->tmdbId : 0},
                  {QStringLiteral("series"), job->series},
                  {QStringLiteral("explicit"), job->explicitMap},
                  {QStringLiteral("checked"), QDateTime::currentSecsSinceEpoch()},
                  {QStringLiteral("identity"), QJsonObject::fromVariantMap(ident)}};
    saveMeta(job->id, o);
    m_state[job->id] = QStringLiteral("notFound");
    qCDebug(lcTmdb) << "not found:" << job->query << job->year;
    endJob(job);
}

void Tmdb::failJob(const JobPtr &job, const Reply &r)
{
    if (r.http == 401) { onAuthFailed(); return; }
    if (r.http == 404 && job->tmdbId > 0 && job->detailsHttp == 404) { finishNotFound(job); return; }
    if (r.netError) ++m_batchNetFail;
    else qCInfo(lcTmdb) << "request failed for" << job->query << "HTTP" << r.http;
    // keep whatever state the title had (cached overlay stays applied)
    const QJsonObject o = loadMeta(job->id);
    m_state[job->id] = o.value(QStringLiteral("matched")).toBool() ? QStringLiteral("matched")
                       : o.isEmpty()                                ? QStringLiteral("new")
                                                                    : QStringLiteral("notFound");
    endJob(job);
    if (r.netError && !m_retryTimer->isActive()) m_retryTimer->start();
}

void Tmdb::endJob(const JobPtr &job)
{
    if (m_jobs.value(job->id) == job) {
        m_jobs.remove(job->id);
        ++m_batchDone;
    }
    updateStats();
    startMoreJobs();
    updateStatus();
    if (m_jobs.isEmpty()) emit idle(); // batch done: every title has its final artwork state
}

// ---- invokables -------------------------------------------------------------------------------------------------

void Tmdb::refreshAll()
{
    if (!enabled()) { setStatus(QStringLiteral("Add an API key first")); return; }
    abortAll();
    m_jobs.clear();
    m_waiting.clear();
    m_batchTotal = m_batchDone = m_batchNetFail = 0;
    if (m_keyState != KeyState::Valid) { validateKey(); return; }
    m_ids = m_lib->allTitles()->ids();
    for (const QString &id : std::as_const(m_ids)) {
        const QVariantMap ident = m_lib->parsedIdentity(id);
        const auto ov = m_overrides.constFind(id);
        if (ov != m_overrides.constEnd()) {
            const int tid = ov->value(QStringLiteral("tmdbId")).toInt();
            if (tid > 0) queueJob(id, true, false, tid, ov->value(QStringLiteral("series")).toBool(), true);
            continue;
        }
        if (isJunk(id, ident)) continue;
        queueJob(id, true, false);
    }
    updateStats();
    startMoreJobs();
    updateStatus();
}

void Tmdb::refreshTitle(const QString &id)
{
    if (!enabled() || m_lib->title(id).isEmpty()) return;
    if (m_keyState != KeyState::Valid) { validateKey(); return; }
    const auto ov = m_overrides.constFind(id);
    if (ov != m_overrides.constEnd() && ov->value(QStringLiteral("tmdbId")).toInt() > 0)
        queueJob(id, true, true, ov->value(QStringLiteral("tmdbId")).toInt(), ov->value(QStringLiteral("series")).toBool(), true);
    else
        queueJob(id, true, true);
    updateStats();
    startMoreJobs();
    updateStatus();
}

void Tmdb::clearCache()
{
    m_ioPool.waitForDone(); // pending cache writes must not recreate files after the removal
    abortAll();
    m_jobs.clear();
    m_waiting.clear();
    m_batchTotal = m_batchDone = m_batchNetFail = 0;
    // revert every overlay we applied (cached on disk or only in memory)
    QDir metaDir(cacheDir() + QStringLiteral("/meta"));
    const auto files = metaDir.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString &f : files) {
        const QString id = f.chopped(5);
        const QJsonObject o = loadMeta(id);
        if (o.value(QStringLiteral("matched")).toBool()) revertMeta(id, o);
    }
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
        if (it.value().value(QStringLiteral("matched")).toBool() && !files.contains(it.key() + QStringLiteral(".json")))
            revertMeta(it.key(), it.value());
    QDir(cacheDir()).removeRecursively();
    m_cache.clear();
    m_metaLoaded = true; // the (now empty) cache is complete
    for (auto it = m_state.begin(); it != m_state.end(); ++it)
        if (it.value() == QLatin1String("matched") || it.value() == QLatin1String("notFound") || it.value() == QLatin1String("pending"))
            it.value() = QStringLiteral("new");
    updateStats();
    setBusy();
    setStatus(enabled() ? QStringLiteral("Cache cleared — use Refresh all to download again")
                        : QStringLiteral("Cache cleared"));
}

void Tmdb::search(const QString &rawQuery, bool series)
{
    const quint64 seq = ++m_searchSeq;
    QString q = rawQuery.simplified();
    if (!enabled() || q.isEmpty() || m_keyState == KeyState::Invalid) { emit searchResults({}); return; }
    // "Minority Report 2002" / "Minority Report (2002)" -> year filter
    QString year;
    static const QRegularExpression yearRe(QStringLiteral("\\s*\\(?((?:19|20)\\d\\d)\\)?$"));
    const auto m = yearRe.match(q);
    if (m.hasMatch() && m.capturedStart() > 0) { year = m.captured(1); q = q.left(m.capturedStart()).trimmed(); }
    QList<QPair<QString, QString>> query{{QStringLiteral("query"), q}, {QStringLiteral("include_adult"), QStringLiteral("false")},
                                         {QStringLiteral("language"), QStringLiteral("en-US")}};
    if (!year.isEmpty()) query.append({series ? QStringLiteral("first_air_date_year") : QStringLiteral("year"), year});
    apiGet(series ? QStringLiteral("/search/tv") : QStringLiteral("/search/movie"), query, true, [this, seq, series](const Reply &r) {
        if (seq != m_searchSeq) return; // superseded by a newer search
        QVariantList out;
        if (r.http == 401) { onAuthFailed(); emit searchResults(out); return; }
        if (r.netError) setStatus(QStringLiteral("No network"));
        for (const QJsonValue &v : r.json().value(QStringLiteral("results")).toArray()) {
            const QJsonObject o = v.toObject();
            const QString poster = o.value(QStringLiteral("poster_path")).toString();
            out.append(QVariantMap{
                {QStringLiteral("tmdbId"), o.value(QStringLiteral("id")).toInt()},
                {QStringLiteral("title"), o.value(series ? QStringLiteral("name") : QStringLiteral("title")).toString()},
                {QStringLiteral("year"), yearOf(o.value(series ? QStringLiteral("first_air_date") : QStringLiteral("release_date")).toString()).toInt()},
                {QStringLiteral("isSeries"), series},
                {QStringLiteral("posterUrl"), poster.isEmpty() ? QString() : m_imageBase + QStringLiteral("/w185") + poster},
                {QStringLiteral("overview"), o.value(QStringLiteral("overview")).toString()}});
            if (out.size() >= 20) break;
        }
        emit searchResults(out);
    });
}

void Tmdb::assign(const QString &id, int tmdbId, bool series)
{
    if (m_lib->title(id).isEmpty()) return;
    m_overrides.insert(id, QJsonObject{{QStringLiteral("tmdbId"), tmdbId}, {QStringLiteral("series"), series}});
    saveOverrides();
    if (tmdbId <= 0) {
        // "Not a movie or show": drop the overlay and never look it up again
        if (m_jobs.remove(id)) { m_waiting.removeAll(id); ++m_batchDone; }
        const QJsonObject o = loadMeta(id);
        if (o.value(QStringLiteral("matched")).toBool()) revertMeta(id, o);
        m_cache.insert(id, QJsonObject()); // known: no meta file (m_cache is authoritative once loaded)
        const QString file = metaFile(id);
        ioThen([file]() { return QFile::remove(file); });
        m_state[id] = QStringLiteral("ignored");
        updateStats();
        startMoreJobs();
        updateStatus();
        return;
    }
    if (!enabled()) return;
    if (m_keyState != KeyState::Valid) { validateKey(); return; } // applied after validation (needsFetch sees the override)
    queueJob(id, false, true, tmdbId, series, true);
    updateStats();
    startMoreJobs();
    updateStatus();
}

bool Tmdb::expectsArtwork(const QString &id) const
{
    if (!m_metaLoaded) return false;
    const QJsonObject o = m_cache.value(id);
    if (o.value(QStringLiteral("matched")).toBool()) {
        const QJsonObject meta = o.value(QStringLiteral("meta")).toObject();
        return !meta.value(QStringLiteral("posterFile")).toString().isEmpty()
               || !meta.value(QStringLiteral("backdropFile")).toString().isEmpty();
    }
    if (m_jobs.contains(id)) return true;
    // not looked up yet but will be once the key works (onLibraryChanged queues "new" titles)
    return enabled() && m_keyState != KeyState::Invalid && m_state.value(id) == QLatin1String("new");
}

QVariantMap Tmdb::titleState(const QString &id) const
{
    const QVariantMap ident = m_lib->parsedIdentity(id);
    const QJsonObject o = m_cache.value(id);
    const auto ov = m_overrides.constFind(id);
    return {{QStringLiteral("state"), m_state.value(id, QStringLiteral("new"))},
            {QStringLiteral("tmdbId"), ov != m_overrides.constEnd() ? ov->value(QStringLiteral("tmdbId")).toInt()
                                                                    : o.value(QStringLiteral("tmdbId")).toInt()},
            {QStringLiteral("series"), ov != m_overrides.constEnd() ? ov->value(QStringLiteral("series")).toBool()
                                                                    : ident.value(QStringLiteral("isSeries")).toBool()},
            {QStringLiteral("query"), cleanQuery(ident.value(QStringLiteral("title")).toString())},
            {QStringLiteral("year"), ident.value(QStringLiteral("year")).toInt()},
            {QStringLiteral("explicit"), ov != m_overrides.constEnd()}};
}
