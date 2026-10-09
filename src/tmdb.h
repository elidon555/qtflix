#pragma once
#include <QObject>
#include <qqml.h>
#include <QQmlEngine>
#include <QJSEngine>
#include <QString>
#include <QVariantMap>
#include <functional>
#include <QNetworkAccessManager>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QThreadPool>
#include <memory>

class QNetworkReply;
class QTimer;

class Library;

// TMDB metadata service, exposed to QML as context property `Tmdb`.
// - apiKey persisted in QSettings ("tmdb/apiKey"); enabled when non-empty.
// - After every Library::libraryChanged(), matches each title (Library::parsedIdentity) against TMDB
//   (search/movie or search/tv with year), downloads poster (w500), backdrop (w1280), title logo
//   (images endpoint, prefer PNG with iso_639_1 == "en" or null), and for series the episode stills + overviews
//   per season. Everything is cached under QStandardPaths::CacheLocation/tmdb/ (JSON per title id + images),
//   so later launches apply instantly offline without network access. The cache is read and written on a
//   background thread (no fsync; temp file + rename).
// - Applies results through Library::setExternalMetadata / setExternalEpisodeMetadata.
// - Rate-limited (max 6 API requests + 8 image downloads in flight, honours 429 Retry-After), resilient to
//   errors, never blocks the UI.
class Tmdb : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString apiKey READ apiKey WRITE setApiKey NOTIFY apiKeyChanged)
    Q_PROPERTY(bool enabled READ enabled NOTIFY apiKeyChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int pending READ pending NOTIFY busyChanged)       // titles still to fetch
    Q_PROPERTY(int matched READ matched NOTIFY statsChanged)      // titles with metadata applied
    Q_PROPERTY(int unmatched READ unmatched NOTIFY statsChanged)  // titles TMDB could not find
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)   // human readable ("Fetching 12 of 48…", "Invalid API key", "Up to date")
public:
    // QML singleton plumbing: main.cpp constructs the instance; QML (import QtFlix) resolves `Tmdb` to it.
    static Tmdb *instance() { return s_instance; }
    static Tmdb *create(QQmlEngine *, QJSEngine *engine)
    {
        Q_ASSERT(s_instance);
        Q_ASSERT(engine->thread() == s_instance->thread());
        QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
        return s_instance;
    }
    explicit Tmdb(Library *lib, QObject *parent = nullptr);
    QString apiKey() const { return m_apiKey; }
    void setApiKey(const QString &key);
    bool enabled() const { return !m_apiKey.isEmpty(); }
    bool busy() const { return m_busy; }
    int pending() const { return m_pending; }
    int matched() const { return m_matched; }
    int unmatched() const { return m_unmatched; }
    QString status() const { return m_status; }

    Q_INVOKABLE void refreshAll();                       // ignore cache, refetch everything
    Q_INVOKABLE void refreshTitle(const QString &id);    // refetch one
    Q_INVOKABLE void clearCache();
    // Manual fix-up: user picks a different TMDB result. results = search(query) -> list of {tmdbId, title, year, isSeries, posterUrl}
    Q_INVOKABLE void search(const QString &query, bool series);  // emits searchResults
    Q_INVOKABLE void assign(const QString &id, int tmdbId, bool series);
    // Addition: tmdbId 0 in assign() means "not a movie/show": never look this title up again (until reassigned).
    // Per-title state for the settings UI: {state: "matched"|"notFound"|"skipped"|"ignored"|"pending"|"new",
    //   tmdbId, series, query (parsed title), year, explicit (bool, user-assigned)}
    Q_INVOKABLE QVariantMap titleState(const QString &id) const;
    // C++ only (thumbnail warm-up): true if this title has or is about to get TMDB artwork, so grabbing a
    // video frame for its poster/backdrop would be wasted. Best effort: false while unknown.
    bool expectsArtwork(const QString &id) const;
    ~Tmdb() override;

signals:
    void apiKeyChanged();
    void busyChanged();
    void statsChanged();
    void statusChanged();
    void searchResults(const QVariantList &results);
    void idle(); // C++ only: a fetch batch finished (nothing queued or running any more)

private:
    Library *m_lib;
    QNetworkAccessManager m_nam;
    QString m_apiKey;
    bool m_busy = false;
    int m_pending = 0, m_matched = 0, m_unmatched = 0;
    QString m_status;
    // implementation details are up to tmdb.cpp (private helpers/members may be added here)
    struct Reply;
    struct Request;
    struct Job;
    using JobPtr = std::shared_ptr<Job>;
    using RequestPtr = std::shared_ptr<Request>;
    enum class KeyState { None, Validating, Valid, Invalid, Offline };

    // network plumbing (separate in-flight limits for the API and the image CDN)
    void apiGet(const QString &path, const QList<QPair<QString, QString>> &query, bool interactive,
                std::function<void(const Reply &)> cb);
    void fetchFile(const QUrl &url, std::function<void(const Reply &)> cb);
    void enqueue(const RequestPtr &r);
    void pump();
    void startRequest(const RequestPtr &r);
    void abortAll();          // drops queued + in-flight non-interactive requests, bumps the generation
    QUrl apiUrl(const QString &path, const QList<QPair<QString, QString>> &query) const;

    // key validation
    void validateKey();
    void onAuthFailed();

    // library sync
    void scheduleSync();
    void onLibraryChanged();
    void queueJob(const QString &id, bool force, bool front, int tmdbId = 0, bool series = false, bool explicitMap = false);
    void startMoreJobs();
    bool alive(const JobPtr &job) const;
    void runSearch(const JobPtr &job);
    void fetchDetails(const JobPtr &job);
    void detailsDone(const JobPtr &job);
    void addSeason(const JobPtr &job, int season, const QJsonObject &data);
    void download(const JobPtr &job, const QString &remotePath, const QString &size, const QString &file,
                  const QString &fallbackRemotePath, std::function<void(bool)> done);
    void jobStep(const JobPtr &job);   // decrements outstanding, finishes when 0
    void finishJob(const JobPtr &job);
    void finishNotFound(const JobPtr &job);
    void failJob(const JobPtr &job, const Reply &r);
    void endJob(const JobPtr &job);

    // cache / overlays
    void ioThen(std::function<bool()> work, std::function<void(bool)> then = {}); // work on m_ioPool, then main
    void metaLoaded(const QHash<QString, QJsonObject> &all);
    QString cacheDir() const;
    QString metaFile(const QString &id) const;
    QJsonObject loadMeta(const QString &id);
    void saveMeta(const QString &id, const QJsonObject &o);
    void applyMeta(const QString &id, const QJsonObject &o, bool force);
    void revertMeta(const QString &id, const QJsonObject &o);
    bool isJunk(const QString &id, const QVariantMap &ident) const;
    bool needsFetch(const QString &id, const QVariantMap &ident, const QJsonObject &meta) const;
    void loadOverrides();
    void saveOverrides();
    void updateStats();
    void updateStatus();
    void setStatus(const QString &s);
    void setBusy();

    KeyState m_keyState = KeyState::None;
    quint64 m_gen = 1;                       // bumped by abortAll(); stale callbacks are dropped
    quint64 m_searchSeq = 0;
    QList<RequestPtr> m_queue;               // API requests (interactive ones first)
    QList<RequestPtr> m_imgQueue;            // image downloads
    int m_apiInFlight = 0, m_imgInFlight = 0;
    QHash<QNetworkReply *, RequestPtr> m_inFlight;
    QTimer *m_apiPauseTimer = nullptr;       // running while the API asked us to back off (HTTP 429)
    QTimer *m_syncTimer = nullptr;
    QTimer *m_retryTimer = nullptr;
    bool m_applying = false;
    QHash<QString, JobPtr> m_jobs;           // queued + running, by library id
    QStringList m_waiting;                   // queued job ids in start order
    int m_batchTotal = 0, m_batchDone = 0, m_batchNetFail = 0;
    int m_skipped = 0;
    QStringList m_ids;                       // library ids at last sync
    QHash<QString, QJsonObject> m_cache;     // id -> meta json (as on disk; complete once m_metaLoaded)
    bool m_metaLoaded = false;               // the on-disk cache was read (background thread at startup)
    bool m_syncWanted = false;               // a sync was requested before that
    mutable QHash<QString, QPair<QString, bool>> m_junk; // id -> (title + file name, isJunk) memo
    QHash<QString, QString> m_state;         // id -> state string
    QHash<QString, QJsonObject> m_overrides; // id -> {tmdbId, series} explicit user mapping
    QString m_imageBase;
    QString m_apiBase;
    QThreadPool m_ioPool;                    // one thread: cache reads/writes, in order
    static inline Tmdb *s_instance = nullptr;
};
