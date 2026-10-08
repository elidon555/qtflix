pragma Singleton
import QtQuick

// Global navigation state shared by all screens.
QtObject {
    // "intro" | "profiles" | "home" | "tv" | "movies" | "new" | "mylist" | "search" | "settings"
    property string page: "intro"
    property string previousPage: "home"
    // Detail modal
    property string detailId: ""          // non-empty => DetailModal visible
    // Player
    property string playerPath: ""        // non-empty => Player visible (full window)
    property bool playerFromDetail: false
    // Chosen profile
    property string profileName: "Me"
    property color profileColor: "#1CE783"
    property int profileAvatar: 0

    function go(p) { if (p !== page) { previousPage = page; page = p } }
    function openDetail(id) { detailId = id }
    function closeDetail() { detailId = "" }
    function play(path) { playerFromDetail = detailId !== ""; detailId = ""; playerPath = path }
    function closePlayer() { playerPath = "" }
}
