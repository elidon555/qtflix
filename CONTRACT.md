# QtFlix — integration contract

Qt 6.10, QML module URI `QtFlix` (all files in `qml/`, flat, every `.qml` auto-registered by CMake glob; no
subdirectories). Assets live in `assets/` and are available as `qrc:/assets/<file>`. C++ sources in `src/`
(auto-globbed; no CMake edits needed). Build: `cmake -S . -B build -G Ninja && ninja -C build && ./build/qtflix`.

## Backend objects visible from QML
- `Library` (context property) — see `src/library.h` for every property/invokable and the role list in
  `src/titlemodel.h`. Models expose roles by name (`model.title`, `model.cardImage`, `model.progress`, ...).
  `TitleModel.get(row)` and `Library.title(id)` return maps with the same keys as the role names
  (`id, title, year, path, sourceUrl, isSeries, seasonCount, episodeCount, durationMs, quality, cardImage,
  backdropImage, positionMs, progress, inMyList, added, category, description, rating, genres, match, rank`).
- `RowsModel` roles: `name`, `kind` ("continue" | "mylist" | "top10" | "normal"), `model` (a TitleModel).
- `SubtitleTrack` QML type from `import QtFlix` — see `src/subtitleparser.h`.
- Images: `image://thumbs/<id>/card` (2:3), `image://thumbs/<id>/backdrop` (16:9), `image://thumbs/<id>/ep/<s>/<e>`.

## QML singletons (already written)
- `Theme` — colors, font ("Inter"), metrics. Use it everywhere; no hardcoded colors except gradients.
- `Nav` — global navigation state. `Nav.page`, `Nav.openDetail(id)`, `Nav.play(path)`, `Nav.closePlayer()`.

## Screens / components and who owns them
- `Main.qml` (integration, written by lead): `ApplicationWindow` 1600x900 min 1100x650, background Theme.bg.
  Opens straight on the browse pages inside a `Flickable`/`ScrollView` under a `NavBar`;
  `DetailModal` overlay when `Nav.detailId !== ""`; `Player` overlay when `Nav.playerPath !== ""`.
- Browse agent owns: `NavBar.qml`, `Billboard.qml`, `TitleRow.qml`,
  `TitleCard.qml`, `HoverPreview.qml`, `Top10Card.qml`, `HomePage.qml`, `BrowsePage.qml` (tv/movies/new/mylist
  — takes a `RowsModel` or a `TitleModel` grid), `SearchPage.qml`, `SettingsPage.qml`, `DetailModal.qml`,
  `BrandLogo.qml` (qtflix ring mark + wordmark, `assets/mark.svg`), plus any assets they need.
  Public API each must expose is listed in that agent's brief.
- Player agent owns: `Player.qml` (+ helpers prefixed `Player*.qml`), `src/subtitleparser.cpp`.
- Backend agent owns: `src/library.cpp`, `src/titlemodel.cpp`, `src/thumbnailprovider.cpp`.

## Round 2 additions
- `Tmdb` context property (src/tmdb.h): metadata service. Settings UI lives in `SettingsMetadata.qml` (owned by the
  metadata agent) and is embedded by `SettingsPage.qml` as `SettingsMetadata { width: parent.width }`.
- New TitleModel roles: `logoImage` (QUrl, empty when none) and `hasMeta` (bool). When `logoImage` is non-empty,
  Billboard and DetailModal draw the logo image instead of the title text (Netflix style, max height ~ 180px,
  max width 40% of the billboard, left aligned, bottom-left anchored). `description` becomes the real synopsis.
- `Library.episodes()` entries now also carry `description` and `still` (QUrl; local still if available).
- Hooks in `src/library.h`: `setExternalMetadata`, `setExternalEpisodeMetadata`, `parsedIdentity`.
- `Theme` singleton may gain a `vw(x)` helper: returns x% of the window width; browse components scale with it.
- Settings keys (QSettings): `ui/autoplayPreviews` (bool, default true), `ui/previewMuted` (bool, default true),
  `tmdb/apiKey` (string), `player/volume`, `player/speed`, `player/subtitleStyle`.
- Clearing overlays: `Library.setExternalMetadata(id, {})` (empty map) clears that title's overlay, and
  `setExternalEpisodeMetadata(id, s, e, {})` clears one episode's. `Tmdb.clearCache()` uses this to revert everything.
- Tmdb extras: `Tmdb.titleState(id)` -> {state: "matched"|"notFound"|"skipped"|"ignored"|"pending"|"new", tmdbId,
  series, query, year, explicit}; `Tmdb.assign(id, 0, false)` marks a title as "not a movie/show" (never looked up).
  Cache: CacheLocation/tmdb/{meta/<id>.json, images/}. User-picked mappings: AppDataLocation/tmdb_overrides.json
  (kept by clearCache). Dev env overrides: `QTFLIX_TMDB_BASE`, `QTFLIX_TMDB_IMAGE_BASE`; logging `qtflix.tmdb.debug=true`.
- Backend (round 2): `Library::setThumbnailProvider(ThumbnailProvider*)` (C++ only, not for QML) — the provider
  registers itself from its constructor. Overlays are idempotent (re-applying the same map is a no-op) and are
  persisted with the scan cache (CacheLocation/library-cache.json; dropped on load if a referenced file is gone), so
  titles start with their metadata already applied. When a title's poster/backdrop changes, `cardImage` /
  `backdropImage` gain a `?v=<n>` query so QML refetches. The models are restored from the scan cache before the first
  rescan finishes; rescans (auto: library folders are watched, 3 s debounce) only apply differences (row
  inserts/removes/moves, `featuredChanged` only when the hero's data changed, `libraryChanged` only when something
  changed or after the first scan of a session). Dev env: `QTFLIX_TIMING=1` (startup/scan/thumbnail timings),
  `QTFLIX_HWACCEL=0` (no VA-API frame grabs), `QTFLIX_NO_WARMUP=1` (no thumbnail pre-generation).

## Performance pass (backend)
- New TitleModel role `isRecent` (bool): `added` is less than 7 days ago (same rule as `Theme.isRecent`). Also a key of
  `TitleModel.get()` / `Library.title()` / `Library.featured` maps.
- `Library.recentTitles(n)` -> up to n title maps (same keys as `title()`), newest `added` first (ties: alphabetical).
- `Library.recentCount()` -> number of titles with `isRecent`. Plain invokables: re-query on `libraryChanged`.
- `Library.episodeCount(id, season)` -> `episodes(id, season).length` without building the maps.
- `Library.warmSeason(id, season)`: pre-generates that season's episode stills in the background (behind live image
  requests). Call when the detail modal shows a season; repeated calls are no-ops. The scan warm-up no longer queues
  episode stills or portrait `card` images (the `cardImage` role stays) — only backdrops.
- `Library.flushProgress()`: writes pending watch state now (async). `setProgress()` updates the models at once but the
  file is written at most every ~30 s, on `flushProgress()`, and synchronously on quit. The player should call
  `flushProgress()` when it closes / stops.
- Library / TitleModel Q_PROPERTYs are FINAL.
- Episode `thumb` / `still` URLs may carry `?v=<n>` like `cardImage` / `backdropImage` (new generation = refetch).
- First scan is progressive: titles appear right after the folder walk (durations / quality / 4K badge fill in as
  ffprobe results arrive, in batches every ~1.5 s); frames of files not probed yet are placeholders and their image
  URLs change (`?v=`) once probed. `libraryChanged` fires on the first publish and when titles are added / removed.
- Auto-rescans read only the folders the watcher reported (plus new subfolders); manual `rescan()`, folder changes and
  the first scan of a session read everything. Metadata overlay refreshes are coalesced (250 ms).
- C++ only (metadata service): `Library::setWarmUpSkip(std::function<bool(const QString &id)>)` (warm-up skips frame
  grabs of those titles) and slot `Library::requeueWarmUp()` (rebuild the warm-up list after the answer changed).
- Dev: `QTFLIX_SELFTEST=progress` (run twice, headless) checks progress saving / restoring; `QTFLIX_TIMING=1` adds
  "first titles published", scan breakdown (walk / folders read / files probed) and warm-up job counts.
