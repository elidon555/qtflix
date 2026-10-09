pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// Home: billboard + lolomo rows from Library.homeRows (RowsView). The first row overlaps the billboard's faded
// bottom. "Explore All" on a row opens that row as a grid in place (exploreRow), Escape / back arrow returns.
FocusScope {
    id: pageRoot
    objectName: "homePage"
    readonly property real scrollY: exploreRow ? ((explore.item as ExploreView)?.scrollY ?? 0) : rowsView.scrollY
    property var rowsModel: Library.homeRows
    property var featured: Library.featured
    property var exploreRow: null                 // {name, kind, model} while the Explore All grid is shown
    readonly property bool active: visible && Nav.detailId === "" && Nav.playerPath === ""
    readonly property alias hoverPreview: hoverPreview
    readonly property Billboard billboard: rowsView.billboard

    function explore(r: var) { hoverPreview.close(true); exploreRow = r }
    function closeExplore() { hoverPreview.close(true); exploreRow = null; rowsView.scrollTo(0, false) }
    Keys.onEscapePressed: (ev) => { if (exploreRow) { closeExplore(); ev.accepted = true } else ev.accepted = false }

    RowsView {
        id: rowsView
        anchors.fill: parent
        focus: !pageRoot.exploreRow
        visible: !pageRoot.exploreRow
        rows: pageRoot.rowsModel
        featured: pageRoot.featured
        pageActive: pageRoot.active
        previewShown: hoverPreview.shown
        onContentYChanged: hoverPreview.close(true)
        onPreview: (id, r) => hoverPreview.open(id, r)
        onExploreAll: (r) => pageRoot.explore(r)
    }

    Loader {
        id: explore
        anchors.fill: parent
        active: !!pageRoot.exploreRow
        focus: active
        sourceComponent: ExploreView {
            focus: true
            row: pageRoot.exploreRow
            onBack: pageRoot.closeExplore()
            onContentYChanged: hoverPreview.close(true)
            onPreview: (id, r) => hoverPreview.open(id, r)
        }
    }

    HoverPreview { id: hoverPreview }
}
