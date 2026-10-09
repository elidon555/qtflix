import QtQuick
import QtQuick.Controls
import QtTest
import QtFlix

// Real-pointer verification for the browse UI. Mounts the same content as Main/DevBrowse and drives it with
// QtTest's synthetic mouse/key events (TestCase with `when: false` is only used as an event injector).
// Prints PASS/FAIL lines and saves screenshots of key moments to $QTFLIX_SHOTS (default /tmp).
//   QTFLIX_ROOT=DevPointer ./build-browse/qtflix [--size=WxH] [--query=<lowercase text to type, default min>]
ApplicationWindow {
    id: win
    width: 1600; height: 900
    visible: true
    title: "QtFlix (pointer test)"
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
        Nav.page = "home"
        const sz = arg("size")
        if (sz) { const p = sz.split("x"); width = parseInt(p[0]); height = parseInt(p[1]) }
    }

    // ---------------- same content as Main ----------------
    Item {
    id: stage
    anchors.fill: parent
    Item {
        id: browse
        anchors.fill: parent
        Loader {
            id: pageLoader
            anchors.fill: parent
            sourceComponent: {
                switch (Nav.page) {
                case "home": return homeC
                case "tv": return tvC
                case "movies": return moviesC
                case "new": return newC
                case "mylist": return myListC
                case "search": return searchC
                case "settings": return settingsC
                default: return homeC
                }
            }
            onLoaded: item.forceActiveFocus()
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
            anchors { left: parent.left; right: parent.right; top: parent.top }
            scrollY: pageLoader.item && pageLoader.item.scrollY !== undefined ? pageLoader.item.scrollY : 0
            z: 50
        }
    }
    Loader {
        id: modalLoader
        anchors.fill: parent
        active: Nav.detailId !== "" && Nav.playerPath === ""
        z: 100
        sourceComponent: DetailModal {}
    }
    }
    Shortcut {
        sequence: "Escape"
        enabled: Nav.playerPath === "" && Nav.detailId === "" && Nav.page === "search"
        onActivated: { Library.searchQuery = ""; Nav.go(Nav.previousPage) }
    }

    // ---------------- test driver ----------------
    TestCase { id: tc; when: false; name: "DevPointer" }

    readonly property string shotDir: {
        const e = Qt.application.arguments.filter(a => a.indexOf("--shots=") === 0)
        return e.length ? e[0].substring(8) : "/tmp"
    }
    readonly property string query: arg("query") || "min"
    property int passes: 0
    property int fails: 0
    function check(name, cond, extra) {
        if (cond) { passes++; console.log("PASS " + name) }
        else { fails++; console.log("FAIL " + name + (extra !== undefined ? "  [" + extra + "]" : "")) }
    }
    function shot(name) {
        const path = win.shotDir + "/ptr_" + name + ".png"
        const img = tc.grabImage(stage)          // synchronous (forces a window grab)
        img.save(path)
    }
    function findAll(it, pred, out) {
        if (!it) return out
        if (pred(it)) out.push(it)
        const ch = it.children || []
        for (let i = 0; i < ch.length; ++i) findAll(ch[i], pred, out)
        if (it.contentItem && it.contentItem !== it && ch.indexOf(it.contentItem) < 0) findAll(it.contentItem, pred, out)
        return out
    }
    function find(it, pred) { const r = findAll(it, pred, []); return r.length ? r[0] : null }
    function byName(it, n) { return find(it, o => o.objectName === n) }
    function center(item) { return item.mapToItem(win.contentItem, item.width / 2, item.height / 2) }
    function moveTo(item, dx, dy) {
        const p = item.mapToItem(win.contentItem, dx === undefined ? item.width / 2 : dx, dy === undefined ? item.height / 2 : dy)
        tc.mouseMove(win.contentItem, p.x, p.y)
    }
    function moveAbs(x, y) { tc.mouseMove(win.contentItem, x, y) }
    function clickOn(item, dx, dy) {
        const p = item.mapToItem(win.contentItem, dx === undefined ? item.width / 2 : dx, dy === undefined ? item.height / 2 : dy)
        tc.mouseMove(win.contentItem, p.x, p.y)
        tc.mouseClick(win.contentItem, p.x, p.y)
    }
    function page() { return pageLoader.item }
    function rows() { return findAll(page(), o => o.goPage !== undefined && o.cardW !== undefined && o.visible && o.itemCount > 0, []) }
    function cardsOf(row) {
        const cs = findAll(row, o => o.globalRect !== undefined && o.hasItem && o.visible, [])
        cs.sort((a, b) => a.mapToItem(null, 0, 0).x - b.mapToItem(null, 0, 0).x)
        return cs.filter(c => { const x = c.mapToItem(null, 0, 0).x; return x >= row.pad - 2 && x + c.width <= row.width - row.pad + 2 })
    }
    function ensureVisible(r) {
        const f = find(page(), o => o.scrollTo !== undefined)
        const y = r.mapToItem(win.contentItem, 0, 0).y
        if (y + r.height > win.height - 20) f.scrollTo(f.scrollY + (y + r.height - win.height + 60), false)
    }
    function bigRow() { const rs = rows().filter(r => r.pageCount > 1); return rs.length ? rs[0] : rows()[0] }
    function preview() { return page() ? page().hoverPreview : null }

    // state shared between steps
    property var row0: null
    property var card: null
    property var box: null
    property bool stayedOpen: true

    property int stepIdx: 0
    property var steps: [
        // 0: wait for the library / images
        () => { moveAbs(win.width / 2, 5); return 4500 },
        () => { shot("00_home"); win.row0 = bigRow(); check("home has rows", !!win.row0, rows().length); ensureVisible(win.row0); return 600 },

        // ---- hover preview: 500 ms delay ----
        () => { const cs = cardsOf(win.row0); win.card = cs[Math.min(1, cs.length - 1)]; moveTo(win.card); return 300 },
        () => { check("preview not open before 500ms", !preview().shown); return 450 },
        () => { check("preview opened after hover (>=500ms)", preview().isOpen); return 300 },
        () => { shot("01_preview"); win.box = preview().boxItem
                check("preview box is 1.5x card width", Math.abs(win.box.width - Math.max(300, win.card.width * 1.5)) < 2, win.box.width + " vs " + win.card.width)
                return 50 },
        // move around inside the preview: must stay open, no flicker
        () => { win.stayedOpen = true; moveTo(win.box, win.box.width * 0.2, win.box.height * 0.2); return 80 },
        () => { win.stayedOpen = win.stayedOpen && preview().isOpen; moveTo(win.box, win.box.width * 0.8, win.box.height * 0.4); return 80 },
        () => { win.stayedOpen = win.stayedOpen && preview().isOpen; moveTo(win.box, win.box.width * 0.5, win.box.height * 0.7); return 80 },
        () => { win.stayedOpen = win.stayedOpen && preview().isOpen; moveTo(win.box, win.box.width * 0.9, win.box.height * 0.95); return 250 },
        () => { win.stayedOpen = win.stayedOpen && preview().isOpen; check("preview stays open while moving inside it", win.stayedOpen); return 50 },
        // tooltip on the "Add to My List" button (100 ms delay)
        () => { const b = byName(win.box, "previewMyList"); moveTo(b); return 60 },
        () => { const b = byName(win.box, "previewMyList"); check("tooltip hidden before 100ms", !b.tipShown); return 150 },
        () => { const b = byName(win.box, "previewMyList"); check("tooltip shown after 100ms", b.tipShown); shot("02_tooltip"); return 100 },
        // leave: closes ~150 ms later
        () => { moveAbs(win.width / 2, Theme.navH + 40); return 80 },
        () => { check("preview still open 80ms after leaving (grace)", preview().shown); return 450 },
        () => { check("preview closed after leaving", !preview().shown); return 100 },

        // ---- first / last card grow direction ----
        () => { const cs = cardsOf(win.row0); win.card = cs[0]; moveTo(win.card); return 900 },
        () => { const b = preview().boxItem; const cx = win.card.mapToItem(win.contentItem, 0, 0).x
                check("first card preview grows to the right (left-aligned to gutter)", preview().isOpen && Math.abs(b.x - cx) < 3, b.x + " vs " + cx)
                shot("03_preview_first"); moveAbs(win.width / 2, Theme.navH + 40); return 500 },
        () => { const cs = cardsOf(win.row0); win.card = cs[cs.length - 1]; moveTo(win.card); return 900 },
        () => { const b = preview().boxItem; const r = win.card.mapToItem(win.contentItem, win.card.width, 0).x
                check("last card preview grows to the left (right-aligned)", preview().isOpen && Math.abs(b.x + b.width - r) < 3, (b.x + b.width) + " vs " + r)
                shot("04_preview_last"); return 50 },
        // scroll closes immediately
        () => { tc.mouseWheel(win.contentItem, win.width / 2, win.height / 2, 0, -120); return 30 },
        () => { check("wheel scroll closes preview immediately", !preview().shown); return 400 },
        () => { const f = find(page(), o => o.scrollTo !== undefined); f.scrollTo(0, false); moveAbs(win.width / 2, Theme.navH + 40); return 400 },

        // ---- paddles ----
        () => { win.row0 = bigRow(); ensureVisible(win.row0); return 400 },
        () => { moveTo(win.row0, win.row0.width - win.row0.pad / 2, win.row0.height - 20); return 300 },
        () => { check("right paddle visible on row hover", win.row0.rightPaddle.shown && win.row0.rightPaddle.opacity > 0.5)
                check("left paddle hidden on first page", !win.row0.leftPaddle.shown)
                shot("05_paddles"); return 50 },
        () => { clickOn(win.row0.rightPaddle); return 100 },
        () => { check("right paddle click advanced the page", win.row0.page === 1, win.row0.page); check("paging animates", win.row0.sliding); return 900 },
        () => { check("left paddle visible after first page", win.row0.leftPaddle.shown); shot("06_paged"); return 50 },
        () => { clickOn(win.row0.leftPaddle); return 900 },
        () => { check("left paddle click went back", win.row0.page === 0, win.row0.page); moveAbs(win.width / 2, 5); return 300 },
        () => { check("paddles hidden when row not hovered", !win.row0.rightPaddle.shown); return 50 },

        // ---- More Info -> detail modal ----
        () => { const b = byName(page(), "billboardMoreInfo"); clickOn(b); return 800 },
        () => { check("More Info opened the detail modal", Nav.detailId !== "")
                check("billboard media unloaded while the modal is open", page().billboard.mediaLoaded === false)
                const g = byName(modalLoader.item, "moreLikeThisGrid")
                const kids = g ? g.children.filter(c => c.width > 0 && c.height > 0) : []
                const distinctY = {}
                for (const k of kids) distinctY[Math.round(k.y)] = true
                check("More Like This grid laid out (3 columns, several rows)", kids.length >= 3 && Object.keys(distinctY).length >= 2, kids.length + " cells")
                shot("07_detail"); return 50 },
        () => { const f = find(modalLoader.item, o => o.scrollTo !== undefined); f.scrollTo(f.maxY * 0.75, false); return 500 },
        () => { shot("08_detail_more"); const f = find(modalLoader.item, o => o.scrollTo !== undefined); f.scrollTo(0, false); return 300 },
        () => { tc.mouseClick(win.contentItem, 12, win.height / 2); return 500 },
        () => { check("click outside closed the detail modal", Nav.detailId === ""); return 200 },

        // ---- Explore All ----
        () => { win.row0 = bigRow(); ensureVisible(win.row0); return 400 },
        () => { moveTo(win.row0, win.row0.pad + 20, 8); return 900 },
        () => { shot("09_explore_hover"); tc.mouseClick(win.contentItem, win.row0.mapToItem(win.contentItem, win.row0.pad + 20, 8).x,
                                                       win.row0.mapToItem(win.contentItem, win.row0.pad + 20, 8).y); return 600 },
        () => { check("Explore All opened the grid in place", !!page().exploreRow && Nav.page === "home"); shot("10_explore"); return 50 },
        () => { clickOn(byName(page(), "exploreBack")); return 400 },
        () => { check("Explore back arrow returned to rows", !page().exploreRow); return 100 },

        // ---- bell / notifications ----
        () => { moveTo(byName(nav, "bell")); return 600 },
        () => { check("bell hover opens Notifications", nav.notificationsOpen); shot("11_notifications"); moveAbs(win.width / 2, win.height / 2); return 600 },
        () => { check("notifications close after leaving", !nav.notificationsOpen); return 50 },

        // ---- nav link -> page change ----
        () => { const l = byName(nav, "navLink_tv"); clickOn(l, l.width - 10, l.height / 2); return 1500 },
        () => { check("nav link changed the page", Nav.page === "tv", Nav.page); shot("12_tv"); return 50 },
        () => { const l = byName(nav, "navLink_new"); clickOn(l, l.width - 10, l.height / 2); return 2500 },
        () => { check("New & Popular page", Nav.page === "new", Nav.page)
                const rs = rows(); const t = rs.filter(r => r.kind === "top10")
                check("top10 rows render as landscape rows", t.length > 0 && Math.abs(t[0].cardH - Math.round(t[0].cardW * 9 / 16)) < 1)
                if (t.length) { const f = find(page(), o => o.scrollTo !== undefined); f.scrollTo(t[0].mapToItem(f.contentItem, 0, 0).y - f.originY - Theme.navH - 20, false) }
                return 700 },
        () => { shot("13_top10"); const t = rows().filter(r => r.kind === "top10"); const cs = cardsOf(t[0]); moveTo(cs[Math.min(2, cs.length - 1)]); return 900 },
        () => { check("hover preview works on a top10 row", preview().isOpen); shot("14_top10_preview"); moveAbs(win.width / 2, 5); return 500 },

        // ---- search ----
        () => { const s = byName(nav, "searchBox"); clickOn(s, 12, s.height / 2); return 400 },
        () => { for (const ch of win.query) tc.keyClick(ch); return 1200 },
        () => { check("typing in search opens the search page", Nav.page === "search", Nav.page)
                check("all typed keys reach the search box", Library.searchQuery === win.query, Library.searchQuery)
                check("search has results", Library.searchResults.count > 0, Library.searchResults.count)
                shot("15_search"); return 50 },
        () => { const g = byName(page(), "searchGrid"); const cs = findAll(g, o => o.globalRect !== undefined && o.hasItem, []); moveTo(cs[0], 20, 20); return 120 },
        () => { const g = byName(page(), "searchGrid"); const cs = findAll(g, o => o.globalRect !== undefined && o.hasItem, []); moveTo(cs[0]); return 900 },
        () => { check("hover preview works in search grid", preview() && preview().isOpen); shot("15b_search_preview"); moveAbs(win.width / 2, win.height - 5); return 400 },
        () => { tc.keyClick(Qt.Key_Escape); return 600 },

        // ---- settings toggles ----
        () => { Library.searchQuery = ""; Nav.go("settings"); return 1200 },
        () => { const t = byName(page(), "autoplayToggle"); const before = Theme.autoplayPreviews; clickOn(t); return 300 },
        () => { check("autoplay toggle switched off", Theme.autoplayPreviews === false); shot("16_settings"); clickOn(byName(page(), "autoplayToggle")); return 300 },
        () => { check("autoplay toggle back on", Theme.autoplayPreviews === true); return 100 },
        // Clear watch history
        () => { clickOn(byName(page(), "clearHistoryButton")); return 600 },
        () => { check("Clear watch history empties Continue Watching", Library.continueWatching.count === 0, Library.continueWatching.count); return 100 },
        // profile persistence (QSettings profiles/current)
        () => { Theme.selectProfile(Theme.profiles[1]); return 100 },
        () => { check("chosen profile persisted", Theme.profileSettings.current === Theme.profiles[1].name && Nav.profileName === Theme.profiles[1].name)
                Theme.selectProfile(Theme.profiles[0]); return 100 },
        // autoplay off is honored by the billboard
        () => { Theme.autoplayPreviews = false; Nav.go("home"); return 4500 },
        () => { check("billboard does not autoplay when previews are off", page().billboard.mediaLoaded === false && !page().billboard.videoShown)
                Theme.autoplayPreviews = true; return 100 },

        () => { console.log("RESULT " + win.passes + " passed, " + win.fails + " failed"); Qt.quit(); return 0 }
    ]
    Timer {
        id: runner
        interval: 200
        running: true
        onTriggered: {
            if (win.stepIdx >= win.steps.length) return
            let wait = 100
            try { wait = win.steps[win.stepIdx++]() }
            catch (e) { win.fails++; console.log("FAIL step " + (win.stepIdx - 1) + " threw: " + e) ; wait = 200 }
            if (win.stepIdx < win.steps.length) { runner.interval = Math.max(1, wait || 1); runner.restart() }
        }
    }
}
