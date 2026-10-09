import QtQuick

// Typed copy of a title map (TitleModel.get() / Library.title() keys), so bindings that show a title read typed
// properties (compiled ahead of time) instead of looking keys up in a QVariantMap. `titleId` is the map's "id".
QtObject {
    property string titleId
    property string title
    property int year
    property string path
    property url sourceUrl
    property bool isSeries
    property int seasonCount
    property int episodeCount
    property real durationMs
    property string quality
    property url backdropImage
    property url logoImage
    property real positionMs
    property real progress
    property bool inMyList
    property string category
    property string description
    property string rating
    property list<string> genres
    property int match
    property bool hasMeta
    readonly property bool valid: titleId !== ""
    readonly property bool hasLogo: logoImage.toString() !== ""
    readonly property bool hasSource: sourceUrl.toString() !== ""
    readonly property string genreText: genres.join(", ")                // "Sci-Fi, Drama, Mystery"
    readonly property string mainGenres: genres.slice(0, 2).join(", ")   // the first two

    function assign(m: var) {
        const t = m ?? {}
        titleId = t.id ?? ""; title = t.title ?? ""; year = t.year ?? 0; path = t.path ?? ""
        sourceUrl = t.sourceUrl ?? ""; isSeries = t.isSeries === true; seasonCount = t.seasonCount ?? 0
        episodeCount = t.episodeCount ?? 0; durationMs = t.durationMs ?? 0; quality = t.quality ?? ""
        backdropImage = t.backdropImage ?? ""; logoImage = t.logoImage ?? ""; positionMs = t.positionMs ?? 0
        progress = t.progress ?? 0; inMyList = t.inMyList === true; category = t.category ?? ""
        description = t.description ?? ""; rating = t.rating ?? ""; genres = t.genres ?? []; match = t.match ?? 0
        hasMeta = t.hasMeta === true
    }
}
