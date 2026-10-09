pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// Netflix gallery grid of 16:9 cards (My List, New & Popular, search results, "Explore All") as a whole scrolling
// page: a virtualized GridView (only cards near the viewport exist, recycled with reuseItems) spanning the page
// width, cards between the gutters, columns following Theme.cardsPerPage(page width). `top` / `bottom` are the
// page content above / below the grid (full page width, x = 0 at the page's left edge).
ScrollGrid {
    id: root
    property var titles: null                    // TitleModel
    property string kind: "grid"
    property int limit: -1                       // e.g. 10 for a Top 10 row
    property Component topContent: null
    property Component bottomContent: null
    property bool pinBottom: false               // short pages: `bottomContent` sits at the bottom of the viewport
    signal preview(string id, rect globalRect)

    readonly property real pad: Theme.gutter
    readonly property real gap: Theme.cardGap
    readonly property int perRow: Theme.cardsPerPage(width)
    readonly property real cardW: Math.floor((width - 2 * pad - (perRow - 1) * gap) / perRow)
    readonly property real rowGap: Math.round(Theme.vw(2.6))
    readonly property int shown: limit >= 0 ? Math.min(limit, titles ? titles.count : 0) : (titles ? titles.count : 0)

    leftMargin: pad
    rightMargin: pad - gap                       // room for the last column's trailing gap
    cellWidth: cardW + gap
    cellHeight: Math.round(cardW * 9 / 16) + gap + rowGap
    reuseItems: true
    cacheBuffer: Math.max(0, Math.round(height))
    model: limit >= 0 ? shown : titles
    delegate: limit >= 0 ? limitedCard : liveCard

    function activate(id: string, path: string) {
        if (id === "") return
        if (root.kind === "continue") Nav.play(path)
        else Nav.openDetail(id)
    }

    // header/footer are laid out from the left margin: shift them back to the page edge
    header: Item {
        width: root.width
        height: topLoader.height
        Loader { id: topLoader; x: -root.pad; width: root.width; sourceComponent: root.topContent }
    }
    footer: Item {
        readonly property real gridH: (root.headerItem ? root.headerItem.height : 0) + Math.ceil(root.shown / root.perRow) * root.cellHeight
        width: root.width
        // pinned: the page is 20 px taller than the viewport, as when the footer was positioned by hand
        height: root.pinBottom ? Math.max(bottomLoader.height, root.height + 20 - gridH) : bottomLoader.height
        Loader { id: bottomLoader; x: -root.pad; anchors.bottom: parent.bottom; width: root.width; sourceComponent: root.bottomContent }
    }

    Component {
        id: liveCard
        TitleCard {
            // TitleModel roles ("id" can't be a property name, so that one is read through `model`)
            required property var model
            required title
            required backdropImage
            required hasMeta
            required isRecent
            required progress
            required path
            titleId: model.id
            width: root.cardW
            kind: root.kind
            onClicked: root.activate(titleId, path)
            onPreview: (id, r) => root.preview(id, r)
        }
    }
    // the first `limit` titles (model is a count): role maps from TitleModel.get(), refreshed on data changes
    property int revision: 0
    Connections {
        target: root.titles
        enabled: root.limit >= 0
        function onDataChanged() { root.revision++ }
    }
    Component {
        id: limitedCard
        TitleCard {
            required property int index
            readonly property var m: { root.revision; return root.titles ? root.titles.get(index) : ({}) }
            titleId: m.id ?? ""
            title: m.title ?? ""
            backdropImage: m.backdropImage ?? ""
            hasMeta: m.hasMeta === true
            isRecent: m.isRecent === true
            progress: m.progress ?? 0
            path: m.path ?? ""
            width: root.cardW
            kind: root.kind
            onClicked: root.activate(titleId, path)
            onPreview: (id, r) => root.preview(id, r)
        }
    }
}
