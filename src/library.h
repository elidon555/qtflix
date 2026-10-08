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
    Q_PROPERTY(QStringList folders READ folders NOTIFY foldersChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(int titleCount READ titleCount NOTIFY libraryChanged)
    Q_PROPERTY(TitleModel *allTitles READ allTitles CONSTANT)   // everything, alphabetical
    Q_PROPERTY(TitleModel *movies READ movies CONSTANT)
    Q_PROPERTY(TitleModel *series READ series CONSTANT)
    Q_PROPERTY(TitleModel *myList READ myList CONSTANT)
    Q_PROPERTY(TitleModel *continueWatching READ continueWatching CONSTANT) // progress 1%..95%, most recent first
    Q_PROPERTY(RowsModel *homeRows READ homeRows CONSTANT)     // see rows() rules below
    Q_PROPERTY(RowsModel *movieRows READ movieRows CONSTANT)
    Q_PROPERTY(RowsModel *seriesRows READ seriesRows CONSTANT)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)
    Q_PROPERTY(TitleModel *searchResults READ searchResults CONSTANT) // case-insensitive substring on title/category/genres
    Q_PROPERTY(QVariantMap featured READ featured NOTIFY featuredChanged) // hero billboard title (same keys as TitleModel::get)

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
    Q_INVOKABLE void rescan();                 // async; emits scanningChanged, then libraryChanged
    Q_INVOKABLE void addFolder(const QUrl &dirUrl);   // accepts file:// url or plain path
    Q_INVOKABLE void removeFolder(const QString &dir);
    Q_INVOKABLE void pickFeatured();           // choose another random hero

    // --- title lookup ---
    Q_INVOKABLE QVariantMap title(const QString &id) const;    // same keys as TitleModel::get, empty map if unknown
    Q_INVOKABLE QVariantList seasons(const QString &id) const; // [1,2,5]
    // Episodes of a season: list of maps {season, episode, title, path, url, durationMs, positionMs, progress, thumb(QUrl image://thumbs/<id>/ep/<season>/<episode>)}
    Q_INVOKABLE QVariantList episodes(const QString &id, int season) const;
    // Info about a file: {id, title(series/movie title), episodeTitle, season, episode, isSeries, path, url, durationMs, positionMs, backdrop, nextPath, nextUrl, nextLabel("S1:E4 Episode name")}
    Q_INVOKABLE QVariantMap fileInfo(const QString &path) const;
    Q_INVOKABLE QString titleIdForPath(const QString &path) const;
    // Sidecar subtitles found next to a file: list of {path, label} (*.srt, *.vtt, *.ass with same stem or stem.lang.srt)
    Q_INVOKABLE QVariantList sidecarSubtitles(const QString &path) const;

    // --- watch state (persisted in QStandardPaths::AppDataLocation/watchstate.json) ---
    Q_INVOKABLE void setProgress(const QString &path, qint64 positionMs, qint64 durationMs); // throttle-safe, call every ~5s
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
    void saveWatchState();
    void rebuildModels();
    void rebuildContinue();
    void rebuildMyList();
    void rebuildSearch();
    // --- private helpers (backend implementation detail) ---
    void onScanFinished();
    void rebuildRows();
    void refreshTitle(const QString &id);
    QList<TitleModel *> allModels() const;
    void scheduleSave();
    QVariant roleData(const Title &t, int role, int rank) const;
    int resumeIndex(const Title &t) const;      // index into t.files of the file PathRole points at
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
    QFutureWatcher<QVector<Title>> *m_watcher = nullptr;
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
    bool applyTitles(QHash<QString, Title> base, bool fromCache); // true if anything changed
    void flushRefresh();
    void restoreScanCache();
    void saveScanCache();
    void updateWatcher(const QStringList &dirs);
    void queueWarmUp();
    void pruneProgress();
    QString imageUrl(const Title &t, const char *kind) const;
    QVariantMap episodeOverlay(const QString &id, int season, int episode) const;
    static inline Library *s_instance = nullptr;
};
