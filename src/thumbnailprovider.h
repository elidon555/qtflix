#pragma once
#include <QQuickAsyncImageProvider>
#include <QThreadPool>
#include <QStringList>
#include <memory>
#include <functional>
#include <QVariantMap>

class Library;

// image://thumbs/<titleId>/card            -> portrait 2:3  (default 300x450)
// image://thumbs/<titleId>/backdrop        -> 16:9          (default 1280x720)
// image://thumbs/<titleId>/ep/<s>/<e>      -> 16:9 episode frame (default 400x225)
//
// Uses explicit artwork (Title::posterFile / backdropFile) if present, else grabs a frame with
// `ffmpeg` (at ~20% into the file for card/backdrop, ~35% for episodes). Portrait cards are made by
// center-cropping the frame. Results cached as JPEG in QStandardPaths::CacheLocation/thumbs/.
// Returns a dark placeholder with the title text if ffmpeg fails.
class ThumbnailProvider : public QQuickAsyncImageProvider
{
public:
    explicit ThumbnailProvider(Library *lib);
    ~ThumbnailProvider() override;
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;
private:
    Library *m_lib;
    QThreadPool m_pool;

    // --- backend implementation detail (used by Library, which is a friend) ---
    friend class Library;
    // Artwork of a title changed (external metadata): forget queued warm-up work and negative-cache entries
    // for it. Only ffmpeg frame grabs are cached on disk; artwork files are decoded directly, so nothing on
    // disk goes stale. QML refetches because Library bumps the ?v=<generation> query of the image URLs.
    void invalidate(const QString &titleId);
    // Queue low-priority thumbnail generation ("<id>/backdrop", "<id>/card", "<id>/ep/<s>/<e>"), replacing
    // any previous warm-up queue. Runs only while no live QML request is pending, at most 2 jobs at a time.
    void warmUp(const QStringList &jobIds);
    std::function<QVariantMap(const QString &, int, int)> infoFn() const; // binds Library::thumbInfo
    struct Shared;
    std::shared_ptr<Shared> m_shared;
};
