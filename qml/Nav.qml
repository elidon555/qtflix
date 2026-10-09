pragma Singleton
import QtQuick

// Global navigation state shared by all screens.
QtObject {
    // "home" | "tv" | "movies" | "new" | "mylist" | "search" | "settings"
    property string page: "home"
    property string previousPage: "home"
    // Detail modal
    property string detailId: ""          // non-empty => DetailModal visible
    // Player
    property string playerPath: ""        // non-empty => Player visible (full window)
    property bool playerFromDetail: false
    // Account avatar in the nav bar
    readonly property color profileColor: "#0071EB"
    readonly property int profileAvatar: 0

    function go(p) { if (p !== page) { previousPage = page; page = p } }
    function openDetail(id) { detailId = id }
    function closeDetail() { detailId = "" }
    function play(path) { playerFromDetail = detailId !== ""; detailId = ""; playerPath = path }
    function closePlayer() { playerPath = "" }
}
