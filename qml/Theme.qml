pragma Singleton
import QtQuick
import QtCore

QtObject {
    // Netflix palette
    readonly property color red: "#E50914"
    readonly property color redHover: "#F40612"
    readonly property color bg: "#141414"
    readonly property color bgElevated: "#181818"
    readonly property color surface: "#2F2F2F"
    readonly property color surfaceHover: "#404040"
    readonly property color text: "#FFFFFF"
    readonly property color textMuted: "#B3B3B3"
    readonly property color textDim: "#808080"
    readonly property color border: "#333333"
    readonly property color green: "#46D369"     // "97% Match"
    readonly property color navBg: "#141414"

    readonly property string font: "Inter"
    readonly property int navHeight: 68
    readonly property int sidePadding: 60     // 4% of 1500 ~ Netflix gutter
    readonly property int cardSpacing: 6
    readonly property int rowSpacing: 40
    readonly property int animFast: 150
    readonly property int animNormal: 250
    readonly property int animSlow: 400

    // --- extra Netflix tokens used by the browse UI ---
    readonly property color textSoft: "#E5E5E5"     // nav links, row headers
    readonly property color cyan: "#54B9C5"         // "Explore All"
    readonly property color modalBg: "#181818"
    readonly property color divider: "#404040"

    // ---------------------------------------------------------------------------------------------
    // Viewport-relative sizing (Netflix sizes its browse UI in vw).
    // `windowWidth` is fed by NavBar (always present, anchored to the window's full width) through a
    // Binding on its own width, so every component can use Theme.vw(x) without knowing the window.
    // ---------------------------------------------------------------------------------------------
    property real windowWidth: 1600
    function vw(x: real): real { return windowWidth * x / 100 }
    function clamp(v: real, lo: real, hi: real): real { return Math.max(lo, Math.min(hi, v)) }
    readonly property real gutter: Math.round(Math.max(40, vw(4)))                 // 4vw, min 40px
    readonly property real cardGap: Math.max(4, vw(0.4))                            // 2 x .2vw
    readonly property int navH: Math.round(clamp(vw(4.25), 68, 104))                // 68px at <=1600
    readonly property real navFont: Math.round(clamp(vw(0.9), 13, 20))              // nav links
    readonly property real logoH: Math.round(clamp(vw(1.6), 22, 40))                // wordmark height
    readonly property real rowHeaderFont: Math.round(Math.max(12, vw(1.4)))         // row titles 1.4vw
    // cards per page: 6 >= 1400, 5 >= 1100, 4 >= 800 (Netflix breakpoints), 3 below
    function cardsPerPage(w: real): int { return w >= 1400 ? 6 : (w >= 1100 ? 5 : (w >= 800 ? 4 : 3)) }
    // Image.sourceSize for thumbnails shown `w` px wide: the width rounded up to 80 px steps (1920 max), height from
    // the image's aspect. Layout passes through transient widths (0, negative, half laid out); quantizing keeps
    // those from each requesting (and caching) another decode of the same picture.
    function thumbSize(w: real): size { return Qt.size(Math.min(1920, Math.max(80, Math.ceil(w / 80) * 80)), 0) }

    // Netflix easing curves
    readonly property var easePage: [0.5, 0, 0.1, 1, 1, 1]                          // row paging (750 ms)

    // ---------------------------------------------------------------------------------------------
    // Persisted UI preferences (QSettings, ~/.config/qtflix/qtflix.conf)
    // ---------------------------------------------------------------------------------------------
    property Settings uiSettings: Settings {
        id: uiSettings
        category: "ui"
        property bool autoplayPreviews: true
        property bool previewMuted: true
    }
    property alias autoplayPreviews: uiSettings.autoplayPreviews
    property alias previewMuted: uiSettings.previewMuted


    function relativeDate(added: var): string {
        const d = new Date(added)
        if (!added || isNaN(d.getTime())) return ""
        const days = Math.floor((Date.now() - d.getTime()) / (24 * 3600 * 1000))
        if (days <= 0) return "Today"
        if (days === 1) return "Yesterday"
        if (days < 7) return days + " days ago"
        if (days < 14) return "1 week ago"
        if (days < 31) return Math.floor(days / 7) + " weeks ago"
        if (days < 61) return "1 month ago"
        if (days < 365) return Math.floor(days / 30) + " months ago"
        return Math.floor(days / 365) === 1 ? "1 year ago" : Math.floor(days / 365) + " years ago"
    }

    // --- formatting helpers ---
    function duration(ms: real): string {
        const m = Math.round((ms || 0) / 60000)
        if (m <= 0) return ""
        const h = Math.floor(m / 60), r = m % 60
        return h > 0 ? (r > 0 ? h + "h " + r + "m" : h + "h") : r + "m"
    }
    function seasonsLabel(n: int): string { return n === 1 ? "1 Season" : n + " Seasons" }
    function episodesLabel(n: int): string { return n === 1 ? "1 Episode" : n + " Episodes" }
    // "5 Seasons" for multi-season shows, "8 Episodes" for a single season, "2h 5m" for movies
    function lengthLabel(isSeries: bool, seasonCount: int, episodeCount: int, durationMs: real): string {
        if (isSeries) return seasonCount > 1 ? seasonsLabel(seasonCount) : episodesLabel(episodeCount)
        return duration(durationMs)
    }
    function remaining(posMs: real, durMs: real): string {
        const left = Math.max(0, (durMs || 0) - (posMs || 0))
        const m = Math.round(left / 60000)
        if (m <= 0) return ""
        const h = Math.floor(m / 60), r = m % 60
        return (h > 0 ? h + "h " + r + "m" : r + "m") + " left"
    }
}
