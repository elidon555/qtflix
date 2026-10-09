pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// TV Shows / Movies / New & Popular / My List.
//  - rows: a RowsModel (Library.seriesRows ...) or a JS array of {name, kind, model} (a JS array above a grid)
//  - grid: a TitleModel shown as a 16:9 grid (My List), optionally after the rows
//  - showBillboard: hero on top (defaults to true when rows are given)
//  - featuredFrom: TitleModel to pick the hero from (else Library.featured)
// Pages without a grid are a RowsView (virtualized rows); pages with a grid are a TitleGrid (virtualized cards)
// whose header holds the heading and the (few) rows. "Explore All" on a row opens that row as a grid in place
// (exploreRow); Escape / back arrow returns.
FocusScope {
    id: pageRoot
    objectName: "browsePage"
    property var rows: null
    property string heading: ""
    property var grid: null
    property string gridHeading: ""
    property bool showBillboard: !!rows
    property var featuredFrom: null
    property string emptyText: heading === "My List" ? "You haven't added any titles to your list yet." : "Nothing here yet."
    readonly property real scrollY: exploring ? ((explore.item as ExploreView)?.scrollY ?? 0)
                                              : ((view.item as RowsView)?.scrollY ?? (view.item as TitleGrid)?.scrollY ?? 0)
    property var exploreRow: null
    readonly property bool active: visible && Nav.detailId === "" && Nav.playerPath === ""
    readonly property alias hoverPreview: hoverPreview

    property var featured: Library.featured
    function pickFeatured() {
        if (featuredFrom && featuredFrom.count > 0) {
            const m = featuredFrom.get(Math.floor(Math.random() * featuredFrom.count))
            if (m && m.id) { featured = m; return }
        }
        featured = Library.featured
    }
    Component.onCompleted: pickFeatured()
    onFeaturedFromChanged: pickFeatured()
    Connections { target: Library; function onLibraryChanged() { pageRoot.pickFeatured() } function onFeaturedChanged() { if (!pageRoot.featuredFrom) pageRoot.featured = Library.featured } }

    function explore(r: var) { hoverPreview.close(true); exploreRow = r }
    function closeExplore() {
        hoverPreview.close(true); exploreRow = null
        const rowsView = view.item as RowsView, gridView = view.item as TitleGrid
        if (rowsView) rowsView.scrollTo(0, false)
        if (gridView) gridView.scrollTo(0, false)
    }
    Keys.onEscapePressed: (ev) => { if (exploreRow) { closeExplore(); ev.accepted = true } else ev.accepted = false }

    readonly property real pad: Theme.gutter
    readonly property real rowGap: Math.round(Theme.vw(2.8))
    readonly property bool exploring: !!exploreRow

    Loader {
        id: view
        anchors.fill: parent
        visible: !pageRoot.exploring
        focus: !pageRoot.exploring
        sourceComponent: pageRoot.grid ? gridPage : rowsPage
    }

    Component {
        id: rowsPage
        RowsView {
            focus: true
            rows: pageRoot.rows
            featured: pageRoot.featured
            showBillboard: pageRoot.showBillboard
            heading: pageRoot.heading
            footerGap: 60
            pageActive: pageRoot.active
            previewShown: hoverPreview.shown
            onContentYChanged: hoverPreview.close(true)
            onPreview: (id, r) => hoverPreview.open(id, r)
            onExploreAll: (r) => pageRoot.explore(r)
        }
    }

    // heading, rows (New & Popular has three) and the grid heading above the grid; empty text + footer below
    Component {
        id: gridPage
        TitleGrid {
            id: gridView
            focus: true
            titles: pageRoot.grid
            pinBottom: true
            onContentYChanged: hoverPreview.close(true)
            onPreview: (id, r) => hoverPreview.open(id, r)
            topContent: Item {
                height: gridHead.y + (gridHead.visible ? gridHead.height + Math.round(Theme.rowHeaderFont * 0.5) : 0)
                Text {
                    id: headingText
                    visible: text !== ""
                    x: pageRoot.pad
                    y: Theme.navH + Math.round(Theme.vw(1.8))
                    text: pageRoot.heading
                    color: "white"
                    font.family: Theme.font
                    font.pixelSize: Math.round(Theme.clamp(Theme.vw(2.4), 26, 64))
                    font.weight: Font.Medium
                }
                Column {
                    id: rowsCol
                    width: parent.width
                    y: headingText.y + (headingText.visible ? headingText.height + Math.round(Theme.vw(1.8)) : 0)
                    spacing: pageRoot.rowGap
                    Repeater {
                        model: pageRoot.rows
                        delegate: Item {
                            id: wrap
                            required property var modelData
                            width: rowsCol.width
                            height: row.itemCount > 0 ? row.height : 0
                            visible: row.itemCount > 0
                            TitleRow {
                                id: row
                                width: wrap.width
                                name: wrap.modelData.name
                                kind: wrap.modelData.kind
                                model: wrap.modelData.model
                                onPreview: (id, r) => hoverPreview.open(id, r)
                                onExploreAll: pageRoot.explore(wrap.modelData)
                            }
                        }
                    }
                }
                Text {
                    id: gridHead
                    visible: pageRoot.gridHeading !== ""
                    x: pageRoot.pad
                    y: rowsCol.y + rowsCol.height + (rowsCol.height > 0 ? pageRoot.rowGap : 0)
                    text: pageRoot.gridHeading
                    color: "#E5E5E5"
                    font.family: Theme.font
                    font.pixelSize: Theme.rowHeaderFont
                    font.weight: Font.DemiBold
                }
            }
            bottomContent: Column {
                Text {
                    visible: !!pageRoot.grid && pageRoot.grid.count === 0
                    x: pageRoot.pad
                    topPadding: 40
                    text: pageRoot.emptyText
                    color: "#808080"
                    font.family: Theme.font
                    font.pixelSize: Math.round(Theme.clamp(Theme.vw(1.15), 16, 28))
                }
                Item { width: 1; height: 60 - gridView.gap }     // (the last grid row ends with its gap)
                Footer { width: parent.width }
                Item { width: 1; height: 20 }
            }
        }
    }

    Loader {
        id: explore
        anchors.fill: parent
        active: pageRoot.exploring
        focus: active
        sourceComponent: ExploreView {
            focus: true
            row: pageRoot.exploreRow
            footerGap: 60
            onBack: pageRoot.closeExplore()
            onContentYChanged: hoverPreview.close(true)
            onPreview: (id, r) => hoverPreview.open(id, r)
        }
    }

    HoverPreview { id: hoverPreview }
}
