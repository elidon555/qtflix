pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// "Explore All" gallery for one row, opened in place by HomePage/BrowsePage (Nav.page is unchanged):
// back arrow + row name heading, then the row's titles as a 16:9 grid, then the footer. A whole scrolling page.
TitleGrid {
    id: root
    property var row: null                     // {name, kind, model}
    property real footerGap: 40
    signal back()

    titles: row ? row.model : null
    kind: row && row.kind === "top10" ? "top10" : (row && row.kind === "continue" ? "continue" : "grid")
    limit: row && row.kind === "top10" ? 10 : -1

    topContent: Item {
        height: titleRow.y + titleRow.height + Math.round(Theme.vw(1.6))
        Row {
            id: titleRow
            x: Theme.gutter
            y: Theme.navH + Math.round(Theme.vw(1.5))
            spacing: Math.round(Theme.vw(1))
            Item {
                id: backBtn
                objectName: "exploreBack"
                anchors.verticalCenter: parent.verticalCenter
                width: heading.font.pixelSize * 1.1; height: width
                Icon { anchors.centerIn: parent; name: "back"; size: parent.width * 0.85; strokeWidth: 2.2
                       color: bma.containsMouse ? "#B3B3B3" : "white" }
                MouseArea { id: bma; anchors.fill: parent; anchors.margins: -6; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.back() }
            }
            Text {
                id: heading
                text: root.row ? root.row.name : ""
                color: "white"
                font.family: Theme.font
                font.pixelSize: Math.round(Theme.clamp(Theme.vw(2.4), 26, 64))
                font.weight: Font.Bold
            }
        }
    }
    bottomContent: Item {
        height: footerItem.y + footerItem.height + 20
        Footer { id: footerItem; y: root.footerGap - root.gap; width: parent.width }   // (the last grid row ends with its gap)
    }
}
