import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtFlix

ApplicationWindow {
    id: win
    width: 1600; height: 900
    minimumWidth: 1100; minimumHeight: 650
    visible: true
    title: "QtFlix"
    color: Theme.bg
    font.family: Theme.font

    readonly property bool browsing: Nav.page !== "intro" && Nav.page !== "profiles"

    // ---------- Browse pages ----------
    Item {
        id: browse
        anchors.fill: parent
        visible: win.browsing && Nav.playerPath === ""
        enabled: visible

        Loader {
            id: pageLoader
            anchors.fill: parent
            sourceComponent: {
                switch (Nav.page) {
                case "home":     return homeComp
                case "tv":       return tvComp
                case "movies":   return moviesComp
                case "new":      return newComp
                case "mylist":   return myListComp
                case "search":   return searchComp
                case "settings": return settingsComp
                default:         return homeComp
                }
            }
            onLoaded: (item as Item).forceActiveFocus()
        }

        Component { id: homeComp;     HomePage {} }
        Component { id: tvComp;       BrowsePage { heading: "TV Shows"; rows: Library.seriesRows; featuredFrom: Library.series } }
        Component { id: moviesComp;   BrowsePage { heading: "Movies"; rows: Library.movieRows; featuredFrom: Library.movies } }
        Component {
            id: newComp
            BrowsePage {
                heading: "New & Popular"
                showBillboard: false
                rows: [ { name: "New on QtFlix", kind: "normal", model: Library.allTitles },
                        { name: "Top 10 TV Shows Today", kind: "top10", model: Library.series },
                        { name: "Top 10 Movies Today", kind: "top10", model: Library.movies } ]
                grid: Library.allTitles
                gridHeading: "Everything on QtFlix"
            }
        }
        Component { id: myListComp;   BrowsePage { heading: "My List"; grid: Library.myList } }
        Component { id: searchComp;   SearchPage {} }
        Component { id: settingsComp; SettingsPage {} }

        NavBar {
            id: navBar
            anchors { left: parent.left; right: parent.right; top: parent.top }
            // the page types share no common base; each exposes scrollY
            scrollY: (pageLoader.item as HomePage)?.scrollY ?? (pageLoader.item as BrowsePage)?.scrollY
                     ?? (pageLoader.item as SearchPage)?.scrollY ?? (pageLoader.item as SettingsPage)?.scrollY ?? 0
            z: 50
        }
    }

    // ---------- Detail modal ----------
    Loader {
        anchors.fill: parent
        active: Nav.detailId !== "" && Nav.playerPath === ""
        z: 100
        sourceComponent: DetailModal {}
    }

    // ---------- Player ----------
    Loader {
        id: playerLoader
        anchors.fill: parent
        active: Nav.playerPath !== ""
        z: 200
        sourceComponent: Player {
            path: Nav.playerPath
            onClosed: {
                if (Nav.playerFromDetail) {
                    var info = Library.fileInfo(Nav.playerPath)
                    Nav.closePlayer()
                    if (info.id) Nav.openDetail(info.id)
                } else {
                    Nav.closePlayer()
                }
            }
        }
    }

    // ---------- Profiles ----------
    Loader {
        anchors.fill: parent
        active: Nav.page === "profiles"
        z: 300
        sourceComponent: ProfilesScreen { onChosen: Nav.go("home") }
    }

    // ---------- Intro ----------
    Loader {
        anchors.fill: parent
        active: Nav.page === "intro"
        z: 400
        sourceComponent: IntroScreen { onFinished: Nav.go("profiles") }
    }

    Shortcut {
        sequence: "Escape"
        enabled: Nav.playerPath === "" && Nav.detailId === "" && Nav.page === "search"
        onActivated: { Library.searchQuery = ""; Nav.go(Nav.previousPage) }
    }

    Component.onCompleted: {
        var args = Qt.application.arguments
        var playArg = args.filter(function (a) { return a.indexOf("--play=") === 0 })
        if (playArg.length) {
            Nav.page = "home"
            Nav.play(playArg[0].substring(7))
            return
        }
        Nav.page = args.indexOf("--no-intro") >= 0 ? "home" : "intro"
    }
}
