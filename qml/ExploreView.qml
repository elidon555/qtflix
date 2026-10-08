import QtQuick
import QtFlix

// "Explore All" gallery for one row, opened in place by HomePage/BrowsePage (Nav.page is unchanged):
// back arrow + row name heading, then the row's titles as a 16:9 grid.
Column {
    id: root
    property var row: null                     // {name, kind, model}
    signal back()
    signal preview(var item, rect globalRect)
    spacing: Math.round(Theme.vw(1.6))

    Row {
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
    TitleGrid {
        width: root.width
        model: root.row ? root.row.model : null
        kind: root.row && root.row.kind === "top10" ? "top10" : (root.row && root.row.kind === "continue" ? "continue" : "grid")
        limit: root.row && root.row.kind === "top10" ? 10 : -1
        onPreview: (it, r) => root.preview(it, r)
    }
}
