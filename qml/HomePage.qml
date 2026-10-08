pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// Home: billboard + lolomo rows from Library.homeRows. The first row overlaps the billboard's faded bottom.
// "Explore All" on a row opens that row as a grid in place (exploreRow), Escape / back arrow returns.
FocusScope {
    id: pageRoot
    objectName: "homePage"
    property alias scrollY: scroller.contentY
    property var rowsModel: Library.homeRows
    property var featured: Library.featured
    property var exploreRow: null                 // {name, kind, model} while the Explore All grid is shown
    readonly property bool active: visible && Nav.detailId === "" && Nav.playerPath === ""
    readonly property alias hoverPreview: hoverPreview
    readonly property alias billboard: billboard

    function explore(r) { hoverPreview.close(true); exploreRow = r; scroller.scrollTo(0, false) }
    function closeExplore() { hoverPreview.close(true); exploreRow = null; scroller.scrollTo(0, false) }
    Keys.onEscapePressed: (ev) => { if (exploreRow) { closeExplore(); ev.accepted = true } else ev.accepted = false }

    ScrollArea {
        id: scroller
        anchors.fill: parent
        focus: true
        contentHeight: content.height
        onContentYChanged: hoverPreview.close(true)

        Item {
            id: content
            width: scroller.width
            height: Math.max(scroller.height, footer.y + footer.height + 20)

            Billboard {
                id: billboard
                visible: !pageRoot.exploreRow
                width: parent.width
                title: pageRoot.featured
                active: pageRoot.active && visible && !hoverPreview.shown && scroller.contentY < billboard.height * 0.55
                loaded: pageRoot.active && visible && scroller.contentY < billboard.height
            }

            Column {
                id: rows
                visible: !pageRoot.exploreRow
                width: parent.width
                y: billboard.hasTitle ? billboard.height - Math.round(billboard.height * 0.24) : Theme.navH + 30
                spacing: Math.round(Theme.vw(2.8))
                Repeater {
                    model: pageRoot.rowsModel
                    delegate: Item {
                        id: wrap
                        // RowsModel roles. NB: the "model" role shadows the usual model object, so it IS the TitleModel.
                        required property string name
                        required property string kind
                        required property var model
                        width: rows.width
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
            }

            ExploreView {
                id: exploreView
                visible: !!pageRoot.exploreRow
                x: Theme.gutter
                y: Theme.navH + Math.round(Theme.vw(1.5))
                width: parent.width - 2 * Theme.gutter
                row: pageRoot.exploreRow
                onBack: pageRoot.closeExplore()
                onPreview: (it, r) => hoverPreview.open(it, r)
            }

            Footer {
                id: footer
                y: (pageRoot.exploreRow ? exploreView.y + exploreView.height : rows.y + rows.height) + 40
                width: parent.width
            }
        }
    }

    HoverPreview { id: hoverPreview }
}
