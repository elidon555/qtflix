pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// TV Shows / Movies / New & Popular / My List.
//  - rows: a RowsModel (Library.seriesRows ...) or a JS array of {name, kind, model}
//  - grid: a TitleModel shown as a 16:9 grid (My List), optionally after the rows
//  - showBillboard: hero on top (defaults to true when rows are given)
//  - featuredFrom: TitleModel to pick the hero from (else Library.featured)
// "Explore All" on a row opens that row as a grid in place (exploreRow); Escape / back arrow returns.
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
    property alias scrollY: scroller.contentY
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

    function explore(r) { hoverPreview.close(true); exploreRow = r; scroller.scrollTo(0, false) }
    function closeExplore() { hoverPreview.close(true); exploreRow = null; scroller.scrollTo(0, false) }
    Keys.onEscapePressed: (ev) => { if (exploreRow) { closeExplore(); ev.accepted = true } else ev.accepted = false }

    readonly property real pad: Theme.gutter
    readonly property real rowGap: Math.round(Theme.vw(2.8))
    readonly property bool exploring: !!exploreRow

    ScrollArea {
        id: scroller
        anchors.fill: parent
        focus: true
        contentHeight: content.height
        onContentYChanged: hoverPreview.close(true)

        Item {
            id: content
            width: scroller.width
            height: footer.y + footer.height + 20

            Billboard {
                id: billboard
                visible: pageRoot.showBillboard && !pageRoot.exploring
                width: parent.width
                height: pageRoot.showBillboard ? Math.round(width * 0.5625) : 0
                title: pageRoot.featured
                active: pageRoot.active && visible && !hoverPreview.shown && scroller.contentY < billboard.height * 0.55
                loaded: pageRoot.active && visible && scroller.contentY < billboard.height
            }

            // genre-page style heading ("TV Shows") under the nav
            Text {
                id: headingText
                visible: text !== "" && !pageRoot.exploring
                x: pageRoot.pad
                y: Theme.navH + (pageRoot.showBillboard ? Math.round(Theme.vw(0.4)) : Math.round(Theme.vw(1.8)))
                text: pageRoot.heading
                color: "white"
                font.family: Theme.font
                font.pixelSize: Math.round(Theme.clamp(Theme.vw(2.4), 26, 64))
                font.weight: pageRoot.showBillboard ? Font.Bold : Font.Medium
                style: pageRoot.showBillboard ? Text.Raised : Text.Normal
                styleColor: Qt.rgba(0, 0, 0, 0.3)
            }

            Column {
                id: rowsCol
                visible: !pageRoot.exploring
                width: parent.width
                y: pageRoot.showBillboard && billboard.hasTitle ? billboard.height - Math.round(billboard.height * 0.24)
                                                            : headingText.y + (headingText.text !== "" ? headingText.height + Math.round(Theme.vw(1.8)) : 0)
                spacing: pageRoot.rowGap
                // RowsModel (roles name/kind/model; the "model" role shadows the model object)
                Repeater {
                    model: Array.isArray(pageRoot.rows) ? null : pageRoot.rows
                    delegate: Item {
                        id: wrap
                        required property string name
                        required property string kind
                        required property var model
                        width: rowsCol.width
                        height: row.itemCount > 0 ? row.height : 0
                        visible: row.itemCount > 0
                        TitleRow {
                            id: row
                            width: wrap.width
                            name: wrap.name
                            kind: wrap.kind
                            model: wrap.model
                            onPreview: (it, r) => hoverPreview.open(it, r)
                            onExploreAll: pageRoot.explore({ name: wrap.name, kind: wrap.kind, model: wrap.model })
                        }
                    }
                }
                // JS array of {name, kind, model}
                Repeater {
                    model: Array.isArray(pageRoot.rows) ? pageRoot.rows : null
                    delegate: Item {
                        id: wrap2
                        required property var modelData
                        width: rowsCol.width
                        height: row2.itemCount > 0 ? row2.height : 0
                        visible: row2.itemCount > 0
                        TitleRow {
                            id: row2
                            width: wrap2.width
                            name: wrap2.modelData.name
                            kind: wrap2.modelData.kind
                            model: wrap2.modelData.model
                            onPreview: (it, r) => hoverPreview.open(it, r)
                            onExploreAll: pageRoot.explore(wrap2.modelData)
                        }
                    }
                }
            }

            // grid (My List / everything)
            Column {
                id: gridCol
                visible: !!pageRoot.grid && !pageRoot.exploring
                x: pageRoot.pad
                y: rowsCol.y + rowsCol.height + (pageRoot.rows && rowsCol.height > 0 ? pageRoot.rowGap : 0)
                width: parent.width - 2 * pageRoot.pad
                spacing: Math.round(Theme.rowHeaderFont * 0.5)
                Text {
                    visible: pageRoot.gridHeading !== ""
                    text: pageRoot.gridHeading
                    color: "#E5E5E5"
                    font.family: Theme.font
                    font.pixelSize: Theme.rowHeaderFont
                    font.weight: Font.DemiBold
                }
                TitleGrid {
                    width: parent.width
                    model: pageRoot.grid
                    onPreview: (it, r) => hoverPreview.open(it, r)
                }
                Text {
                    visible: !!pageRoot.grid && pageRoot.grid.count === 0
                    topPadding: 40
                    text: pageRoot.emptyText
                    color: "#808080"
                    font.family: Theme.font
                    font.pixelSize: Math.round(Theme.clamp(Theme.vw(1.15), 16, 28))
                }
            }

            ExploreView {
                id: exploreView
                visible: pageRoot.exploring
                x: pageRoot.pad
                y: Theme.navH + Math.round(Theme.vw(1.5))
                width: parent.width - 2 * pageRoot.pad
                row: pageRoot.exploreRow
                onBack: pageRoot.closeExplore()
                onPreview: (it, r) => hoverPreview.open(it, r)
            }

            Footer {
                id: footer
                y: Math.max(scroller.height - height,
                            (pageRoot.exploring ? exploreView.y + exploreView.height
                                            : (gridCol.visible ? gridCol.y + gridCol.height : rowsCol.y + rowsCol.height)) + 60)
                width: parent.width
            }
        }
    }

    HoverPreview { id: hoverPreview }
}
