import QtQuick
import QtFlix

// The qtflix logo: the ring "q" mark (assets/mark.svg) followed by the lowercase wordmark.
// Set `logoHeight`; the width follows. `iconOnly: true` shows just the mark.
Item {
    id: root
    property real logoHeight: 25
    property bool iconOnly: false
    height: logoHeight
    width: iconOnly ? mark.width : Math.round(row.implicitWidth)
    implicitHeight: logoHeight
    implicitWidth: width

    Row {
        id: row
        height: root.logoHeight
        spacing: Math.round(root.logoHeight * 0.22)
        Image {
            id: mark
            width: root.logoHeight; height: root.logoHeight
            source: "qrc:/assets/mark.svg"
            sourceSize: Qt.size(Math.ceil(width * 2), Math.ceil(height * 2))
            smooth: true
        }
        Text {
            visible: !root.iconOnly
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: -Math.round(root.logoHeight * 0.04)
            text: "qtflix"
            font.family: Theme.font
            font.pixelSize: Math.round(root.logoHeight * 1.12)
            font.weight: Font.Black
            font.letterSpacing: -Math.round(root.logoHeight * 0.04)
            color: Theme.text
        }
    }
}
