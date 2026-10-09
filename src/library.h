#pragma once
#include <QObject>
#include <qqml.h>
#include <QQmlEngine>
#include <QJSEngine>
#include <QHash>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>
#include <QFutureWatcher>
#include <QMutex>
#include <QSet>
#include <QCollator>
#include <QThreadPool>
#include <functional>
#include <memory>

class QTimer;
class QFileSystemWatcher;
class ThumbnailProvider;
#include "titlemodel.h"

// Exposed to QML as context property `Library`.
class Library : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QStringList folders READ folders NOTIFY foldersChanged FINAL)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged FINAL)
    Q_PROPERTY(int titleCount READ titleCount NOTIFY libraryChanged FINAL)
    Q_PROPERTY(TitleModel *allTitles READ allTitles CONSTANT FINAL)   // everything, alphabetical
    Q_PROPERTY(TitleModel *movies READ movies CONSTANT FINAL)
    Q_PROPERTY(TitleModel *series READ series CONSTANT FINAL)
    Q_PROPERTY(TitleModel *myList READ myList CONSTANT FINAL)
    Q_PROPERTY(TitleModel *continueWatching READ continueWatching CONSTANT FINAL) // progress 1%..95%, most recent first
    Q_PROPERTY(RowsModel *homeRows READ homeRows CONSTANT FINAL)     // see rows() rules below
    Q_PROPERTY(RowsModel *movieRows READ movieRows CONSTANT FINAL)
    Q_PROPERTY(RowsModel *seriesRows READ seriesRows CONSTANT FINAL)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged FINAL)
    Q_PROPERTY(TitleModel *searchResults READ searchResults CONSTANT FINAL) // case-insensitive substring on title/category/genres
    Q_PROPERTY(QVariantMap featured READ featured NOTIFY featuredChanged FINAL) // hero billboard title (same keys as TitleModel::get)

public:
    // QML singleton plumbing: main.cpp constructs the instance; QML (import QtFlix) resolves `Library` to it.
    static Library *instance() { return s_instance; }
    static Library *create(QQmlEngine *, QJSEngine *engine)
    {
        Q_ASSERT(s_instance);
        Q_ASSERT(engine->thread() == s_instance->thread());
        QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
        return s_instance;
    }
    // No default argument on purpose: a default-constructible QML_SINGLETON would be instantiated
    // by the QML engine instead of going through create() (see QQmlPrivate::singletonConstructionMode).
    explicit Library(QObject *parent);
    ~Library() override;

    QStringList folders() const { return m_folders; }
    bool scanning() const { return m_scanning; }
    int titleCount() const { return m_titles.size(); }
    TitleModel *allTitles() const { return m_all; }
    TitleModel *movies() const { return m_movies; }
    TitleModel *series() const { return m_series; }
    TitleModel *myList() const { return m_myList; }
    TitleModel *continueWatching() const { return m_continue; }
    RowsModel *homeRows() const { return m_homeRows; }
    RowsModel *movieRows() const { return m_movieRows; }
    RowsModel *seriesRows() const { return m_seriesRows; }
    QString searchQuery() const { return m_searchQuery; }
    void setSearchQuery(const QString &q);
    TitleModel *searchResults() const { return m_search; }
    QVariantMap featured() const;

    // --- folders / scanning ---
    Q_INVOKABLE void rescan();                 // async full rescan; emits scanningChanged, then libraryChanged
    Q_INVOKABLE void addFolder(const QUrl &dirUrl);   // accepts file:// url or plain path
    Q_INVOKABLE void removeFolder(const QString &dir);
    Q_INVOKABLE void pickFeatured();           // choose another random hero

    // --- title lookup ---
    Q_INVOKABLE QVariantMap title(const QString &id) const;    // same keys as TitleModel::get, empty map if unknown
    Q_INVOKABLE QVariantList seasons(const QString &id) const; // [1,2,5]
    // Up to n title maps (same keys as title()), newest `added` first (ties: alphabetical order).
    Q_INVOKABLE QVariantList recentTitles(int n) const;
    // Number of titles whose `isRecent` is true (added less than 7 days ago). Re-query on libraryChanged.
    Q_INVOKABLE int recentCount() const;
    // == episodes(id, season).length without building the maps (series: files of that season; movie: 1).
    Q_INVOKABLE int episodeCount(const QString &id, int season) const;
    // Pre-generates the episode stills of one season in the background (low priority, behind live image
    // requests). Call when a season is shown (detail modal); repeated calls for the same season are no-ops,
    // and episodes that already have a cached frame or a metadata still cost nothing.
    Q_INVOKABLE void warmSeason(const QString &id, int season);
    // Episodes of a season: list of maps {season, episode, title, path, url, durationMs, positionMs, progress, thumb(QUrl image://thumbs/<id>/ep/<season>/<episode>)}
    Q_INVOKABLE QVariantList episodes(const QString &id, int season) const;
    // Info about a file: {id, title(series/movie title), episodeTitle, season, episode, isSeries, path, url, durationMs, positionMs, backdrop, nextPath, nextUrl, nextLabel("S1:E4 Episode name")}
    Q_INVOKABLE QVariantMap fileInfo(const QString &path) const;
    Q_INVOKABLE QString titleIdForPath(const QString &path) const;
    // Sidecar subtitles found next to a file: list of {path, label} (*.srt, *.vtt, *.ass with same stem or stem.lang.srt)
    Q_INVOKABLE QVariantList sidecarSubtitles(const QString &path) const;

    // --- watch state (persisted in QStandardPaths::AppDataLocation/watchstate.json) ---
    // throttle-safe, call every ~5s. Updates the models right away; the file is written at most every ~30 s
    // (and on flushProgress() / quit), off the GUI thread.
    Q_INVOKABLE void setProgress(const QString &path, qint64 positionMs, qint64 durationMs);
    // Writes pending watch state now (asynchronously). Call when playback stops / the player closes.
    Q_INVOKABLE void flushProgress();
    Q_INVOKABLE qint64 position(const QString &path) const;
    Q_INVOKABLE void markWatched(const QString &path);           // sets progress to 100%
    Q_INVOKABLE void clearProgress(const QString &path);
    Q_INVOKABLE void toggleMyList(const QString &id);
    Q_INVOKABLE bool inMyList(const QString &id) const;

    // --- external metadata (TMDB etc.). Overlays persist only in memory; the metadata service re-applies
    //     them after every libraryChanged(). Keys (all optional): title, year, description, genres(QStringList),
    //     rating, match, posterFile, backdropFile, logoFile (local absolute paths of downloaded images).
    //     Applying must refresh the title in every model, invalidate its thumbnail cache entries
    //     (ThumbnailProvider reads posterFile/backdropFile) and re-emit featuredChanged if it is the hero.
    Q_INVOKABLE void setExternalMetadata(const QString &id, const QVariantMap &meta);
    // Per-episode overlay: keys title, description, stillFile. Surfaced by episodes() as "title", "description",
    // "still" (QUrl file:// of stillFile, else image://thumbs/<id>/ep/<s>/<e>) and by fileInfo() as episodeTitle.
    Q_INVOKABLE void setExternalEpisodeMetadata(const QString &id, int season, int episode, const QVariantMap &meta);
    // Search key for metadata lookup: {title, year, isSeries} as parsed from the filename (before overlays).
    Q_INVOKABLE QVariantMap parsedIdentity(const QString &id) const;

    // --- for TitleModel / ThumbnailProvider (not for QML) ---
    const Title *findTitle(const QString &id) const;
    QVariantMap titleToMap(const Title &t, int rank = 0) const;
    qint64 positionFor(const QString &path) const;
    qint64 durationFor(const QString &path) const;
    // Backend wiring (not for QML): the ThumbnailProvider registers itself from its constructor (and
    // unregisters with nullptr from its destructor) so the Library can invalidate artwork and queue
    // thumbnail warm-up after scans.
    void setThumbnailProvider(ThumbnailProvider *provider);
    // Backend wiring for the metadata service (not for QML): thumbnail warm-up skips the backdrop (and card)
    // frame grabs of titles for which skip(id) returns true (e.g. titles that will get TMDB artwork).
    // Called on the GUI thread while the warm-up list is built. Call requeueWarmUp() after the answer changed.
    void setWarmUpSkip(std::function<bool(const QString &id)> skip);

public slots:
    // Rebuilds and re-queues the thumbnail warm-up list (no-op before the first scan of the session).
    void requeueWarmUp();

signals:
    void foldersChanged();
    void scanningChanged();
    void libraryChanged();     // after a scan: models rebuilt
    void searchQueryChanged();
    void featuredChanged();
    void progressChanged(const QString &path);
    void myListChanged(const QString &id);

private:
    friend class TitleModel;
    friend class ThumbnailProvider;
    void loadSettings();
    void saveSettings();
    void loadWatchState();
    void saveWatchState(bool sync = false);
    void rebuildModels();
    bool rebuildContinue();                 // true if the Continue Watching ids changed
    void rebuildMyList();
    void rebuildSearch();
    // --- private helpers (backend implementation detail) ---
    void startScan();
    void onScanResult(int index);
    void onScanFinished();
    void rebuildRows();
    void refreshTitle(const QString &id, const QList<int> &roles = {});
    void progressUpdated(const QString &path);
    const QList<TitleModel *> &allModels() const { return m_models; }
    void scheduleSave(int delayMs);
    QVariant roleData(const Title &t, int role, int rank) const;
    int resumeIndex(const Title &t) const;      // index into t.files of the file PathRole points at (memoized)
    double progressFor(const QString &path) const;
    // Thread-safe snapshot for ThumbnailProvider: {title, path, durationMs, poster, backdrop, episodeTitle}
    QVariantMap thumbInfo(const QString &id, int season, int episode) const;

    QStringList m_folders;
    bool m_scanning = false;
    QHash<QString, Title> m_titles;       // id -> title
    QHash<QString, QString> m_pathToId;   // file path -> title id
    QStringList m_orderedIds;             // alphabetical
    QStringList m_myListIds;              // insertion order
    struct Progress { qint64 positionMs = 0; qint64 durationMs = 0; QDateTime lastPlayed; };
    QHash<QString, Progress> m_progress;  // path -> progress
    QString m_featuredId;
    QString m_searchQuery;
    QHash<QString, QVariantMap> m_extMeta;                 // id -> overlay
    QHash<QString, QVariantMap> m_extEpisodeMeta;          // id/season/episode -> overlay
    QHash<QString, QVariantMap> m_parsedIdentity;          // id -> {title, year, isSeries} before overlays

    TitleModel *m_all, *m_movies, *m_series, *m_myList, *m_continue, *m_search;
    RowsModel *m_homeRows, *m_movieRows, *m_seriesRows;
    struct ScanResult;                       // one scan delivers partial results, then a final one
    QFutureWatcher<ScanResult> *m_watcher = nullptr;
    QTimer *m_saveTimer = nullptr;
    bool m_rescanPending = false;
    mutable QMutex m_mutex;               // guards m_titles/m_pathToId writes vs. thumbInfo() reads

    // --- round 2 implementation detail ---
    struct ScanExtras;                       // side results of a scan (watched dirs, unstable files)
    std::shared_ptr<ScanExtras> m_scanExtras;
    QHash<QString, Title> m_baseTitles;      // scan results before overlays (overlays are applied on top)
    QHash<QString, int> m_artGen;            // id -> artwork generation, exposed as ?v=N in image URLs
    ThumbnailProvider *m_thumbs = nullptr;
    QFileSystemWatcher *m_fsWatcher = nullptr;
    QTimer *m_rescanTimer = nullptr;         // debounced auto-rescan
    QTimer *m_refreshTimer = nullptr;        // coalesces model refreshes after metadata overlays
    QTimer *m_cacheSaveTimer = nullptr;      // debounced scan cache write
    QSet<QString> m_pendingRefresh;
    bool m_pendingResort = false;
    bool m_haveScanned = false;              // a real scan finished in this session
    qint64 m_firstChangeMs = 0;              // first folder change of the pending auto-rescan
    void applyOverlay(Title &t) const;       // t holds the scanned values on entry
    // true if anything changed; *structural: titles were added / removed
    bool applyTitles(QHash<QString, Title> base, bool fromCache, bool partial = false, bool *structural = nullptr);
    void flushRefresh();
    void restoreScanCache();
    void saveScanCache(bool sync = false);
    void scheduleCacheSave();
    bool updateWatcher(const QStringList &dirs); // false if some folder could not be watched
    void queueWarmUp();
    void pushWarmUp();
    void pruneProgress();
    QString imageUrl(const Title &t, const char *kind) const;
    QVariantMap episodeOverlay(const QString &id, int season, int episode) const;

    // --- performance pass ---
    struct ScanMemory;                       // directory listings + probe cache kept between scans
    std::shared_ptr<ScanMemory> m_scanMemory;
    QSet<QString> m_dirtyDirs;               // folders reported by the watcher since the last scan
    bool m_fullRescan = true;                // next scan re-lists every folder
    bool m_dirCacheComplete = false;         // every scanned folder is watched -> incremental rescans are safe
    bool m_publishedThisScan = false;        // a partial result of the running scan reached the models
    bool m_featuredProvisional = false;      // hero picked before durations were known
    bool m_baseUnprobed = false;             // m_baseTitles holds files not probed yet (never saved to the cache)
    void improveFeatured();
    struct OverlayCheck { QHash<QString, QVariantMap> ext, extEp; };
    std::shared_ptr<OverlayCheck> m_overlayCheck; // restored overlays whose files the first scan verifies
    void dropOverlays(const QSet<QString> &ids, const OverlayCheck &snapshot);
    void reapplyOverlay(const QString &id);  // m_extMeta[id] changed: refresh the applied title
    QList<TitleModel *> m_models;            // every TitleModel (fixed ones + row models), for refreshes
    QHash<RowsModel *, QList<TitleModel *>> m_rowModels;
    QCollator m_collator;
    void prepare(Title &t) const;            // fills Title's derived fields
    mutable QHash<QString, int> m_resumeCache; // id -> resumeIndex(), dropped when progress / titles change
    bool m_watchDirty = false;
    qint64 m_cacheDirtySince = 0;
    QThreadPool m_ioPool;                    // one thread: JSON serialization + file writes, in order
    std::function<bool(const QString &)> m_warmSkip;
    QStringList m_warmSeasonJobs;            // episode stills asked for by warmSeason(), newest first
    QSet<QString> m_warmedSeasons;           // "id/season" already queued this session
    static inline Library *s_instance = nullptr;
};
