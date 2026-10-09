import QtQuick
import QtQuick.Controls
import QtFlix

// Dev harness for the browse UI. Optional command-line switches (read via Qt.application.arguments):
//   --page=<home|tv|movies|new|mylist|search|settings|intro|profiles>  start page (default home)
//   --detail            open the detail modal on the first title once the library is loaded
//   --detail-series     open the detail modal on the first series
//   --preview=<row>     open the hover preview on the 2nd card of row <row> of the home page
//   --search=<text>     type a search query
//   --scroll=<px>       scroll the page
//   --hover-nav         open the profile dropdown
//   --modal-scroll=<px> scroll the detail modal
//   --size=WxH          initial window size
//   --next-page=<row>   page the given home row forward once
//   --add-mylist        put the first 8 titles in My List
//   --open-detail-hero  open the detail modal on the hero title
//   --count=<ms>        after <ms>, log "[count] items=N cards=M" (visual items in the scene, TitleCards)
ApplicationWindow {
    id: win
    width: 1600; height: 900
    minimumWidth: 1100; minimumHeight: 650
    visible: true
    title: "QtFlix (browse harness)"
    color: Theme.bg
    font.family: Theme.font

    function arg(name) {
        const a = Qt.application.arguments
        for (let i = 0; i < a.length; ++i) {
            if (a[i] === "--" + name) return "1"
            if (a[i].indexOf("--" + name + "=") === 0) return a[i].substring(name.length + 3)
        }
        return ""
    }

    Component.onCompleted: {
        Nav.page = arg("page") || "home"
        const sz = arg("size")
        if (sz) { const p = sz.split("x"); width = parseInt(p[0]); height = parseInt(p[1]) }
    }

    readonly property bool browsing: Nav.page !== "intro" && Nav.page !== "profiles"

    Loader {
        id: pageLoader
        anchors.fill: parent
        visible: win.browsing
        sourceComponent: {
            switch (Nav.page) {
            case "home": return homeC
            case "tv": return tvC
            case "movies": return moviesC
            case "new": return newC
            case "mylist": return myListC
            case "search": return searchC
            case "settings": return settingsC
            default: return null
            }
        }
        onLoaded: (item as Item).forceActiveFocus()
    }
    Component { id: homeC; HomePage {} }
    Component { id: tvC; BrowsePage { heading: "TV Shows"; rows: Library.seriesRows; featuredFrom: Library.series } }
    Component { id: moviesC; BrowsePage { heading: "Movies"; rows: Library.movieRows; featuredFrom: Library.movies } }
    Component {
        id: newC
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
    Component { id: myListC; BrowsePage { heading: "My List"; grid: Library.myList } }
    Component { id: searchC; SearchPage {} }
    Component { id: settingsC; SettingsPage {} }

    NavBar {
        id: nav
        visible: win.browsing
        // the page types share no common base; each exposes scrollY
        scrollY: (pageLoader.item as HomePage)?.scrollY ?? (pageLoader.item as BrowsePage)?.scrollY
                 ?? (pageLoader.item as SearchPage)?.scrollY ?? (pageLoader.item as SettingsPage)?.scrollY ?? 0
    }

    DetailModal { id: modal }

    Loader {
        anchors.fill: parent
        active: Nav.page === "profiles"
        sourceComponent: ProfilesScreen { onChosen: Nav.go("home") }
        onLoaded: (item as Item).forceActiveFocus()
    }
    Loader {
        anchors.fill: parent
        active: Nav.page === "intro"
        sourceComponent: IntroScreen { onFinished: Nav.go("profiles") }
        onLoaded: (item as Item).forceActiveFocus()
    }

    // ---- scripted dev states ----
    Timer {
        id: scripted
        interval: 3500
        running: true
        onTriggered: {
            if (win.arg("open-detail-hero") && Library.featured.id) Nav.openDetail(Library.featured.id)
            if (win.arg("search")) Library.searchQuery = win.arg("search")
            if (win.arg("hover-nav")) nav.menuOpen = true
            if (win.arg("add-mylist")) for (let k = 0; k < Math.min(8, Library.allTitles.count); ++k) if (!Library.inMyList(Library.allTitles.get(k).id)) Library.toggleMyList(Library.allTitles.get(k).id)
            if (win.arg("scroll") && pageLoader.item) {
                const f = win.findScroller(pageLoader.item)
                if (f) f.scrollTo(parseInt(win.arg("scroll")), false)
            }
            if (win.arg("detail") && Library.allTitles.count > 0) Nav.openDetail(Library.allTitles.get(0).id)
            if (win.arg("detail-series")) {
                for (let i = 0; i < Library.allTitles.count; ++i) {
                    const t = Library.allTitles.get(i)
                    if (t.isSeries) { Nav.openDetail(t.id); break }
                }
            }
            if (win.arg("modal-scroll")) modalScroll.start()
            if (win.arg("next-page") && pageLoader.item) {
                const rows = win.findAll(pageLoader.item, (o) => o.goPage !== undefined && o.cardW !== undefined, [])
                if (rows.length) rows[Math.min(parseInt(win.arg("next-page")) || 0, rows.length - 1)].goPage(1)
            }
            if (win.arg("preview") !== "" && pageLoader.item) win.simulatePreview(parseInt(win.arg("preview")) || 0)
        }
    }
    Timer {
        interval: parseInt(win.arg("count")) || 1
        running: win.arg("count") !== ""
        onTriggered: {
            const all = win.findAll(win.contentItem, () => true, [])
            const cards = all.filter((o) => o.globalRect !== undefined && o.hasItem !== undefined)
            console.log("[count] items=" + all.length + " cards=" + cards.length)
        }
    }
    Timer { id: modalScroll; interval: 1500; onTriggered: { const f = win.findScroller(modal); if (f) f.scrollTo(parseInt(win.arg("modal-scroll")), false) } }
    function findScroller(it) {
        for (let i = 0; i < it.children.length; ++i) if (it.children[i].scrollTo) return it.children[i]
        return null
    }
    function findAll(it, pred, out) {
        if (pred(it)) out.push(it)
        const ch = it.children || []
        for (let i = 0; i < ch.length; ++i) findAll(ch[i], pred, out)
        if (it.contentItem && it.contentItem !== it && ch.indexOf(it.contentItem) < 0) findAll(it.contentItem, pred, out)
        return out
    }
    function simulatePreview(rowIndex) {
        const rows = findAll(pageLoader.item, (o) => o.goPage !== undefined && o.cardW !== undefined, [])
        const r = rows.length ? rows[Math.min(rowIndex, rows.length - 1)] : pageLoader.item
        const cards = findAll(r, (o) => o.globalRect !== undefined && o.hasItem, [])
        cards.sort((a, b) => a.mapToItem(null, 0, 0).x - b.mapToItem(null, 0, 0).x)
        const c = cards[Math.min(1, cards.length - 1)]
        if (c) c.preview(c.item, c.globalRect())
    }
}
