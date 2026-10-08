pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// Netflix gallery grid of 16:9 cards (My List, search results, "Explore All").
// Width = the page width minus gutters; columns follow Theme.cardsPerPage(page width).
Flow {
    id: root
    property var model: null
    property real pageWidth: Theme.windowWidth
    property string kind: "grid"
    property int limit: -1                       // e.g. 10 for a Top 10 row
    signal preview(var item, rect globalRect)

    readonly property int perRow: Theme.cardsPerPage(pageWidth)
    readonly property real cardW: Math.floor((width - (perRow - 1) * spacing) / perRow)
    readonly property real rowGap: Math.round(Theme.vw(2.6))
    spacing: Theme.cardGap

    Repeater {
        model: root.model
        delegate: Item {
            id: cell
            required property var model
            required property int index
            visible: root.limit < 0 || index < root.limit
            width: visible ? root.cardW : 0
            height: visible ? card.height + root.rowGap : 0
            TitleCard {
                id: card
                width: root.cardW
                item: cell.model
                kind: root.kind
                onClicked: root.kind === "continue" ? Nav.play(card.item.path) : Nav.openDetail(card.item.id)
                onPreview: (it, r) => root.preview(it, r)
            }
        }
    }
}
