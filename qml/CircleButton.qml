import QtQuick
import QtFlix

// Round icon button used in the hover preview, detail modal and billboard (mute).
Item {
    id: root
    property string iconName: "plus"
    property real size: 32
    property real iconScale: 0.5
    property bool filled: false              // white disc with black glyph (Play)
    property bool glass: true                // dark translucent fill for outlined buttons
    property real borderWidth: 2
    property color borderColor: Qt.rgba(1, 1, 1, 0.5)
    property string tooltip: ""
    signal clicked()

    readonly property bool hovered: ma.containsMouse
    width: size; height: size
    implicitWidth: size; implicitHeight: size

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: root.filled ? (root.hovered ? Qt.rgba(1, 1, 1, 0.8) : "white")
                           : (root.glass ? (root.hovered ? Qt.rgba(1, 1, 1, 0.1) : Qt.rgba(42/255, 42/255, 42/255, 0.6)) : "transparent")
        border.width: root.filled ? 0 : root.borderWidth
        border.color: root.hovered ? "white" : root.borderColor
        Behavior on border.color { ColorAnimation { duration: 100 } }
    }
    Icon {
        anchors.centerIn: parent
        anchors.horizontalCenterOffset: root.iconName === "play" ? root.size * 0.03 : 0
        name: root.iconName
        size: Math.round(root.size * root.iconScale)
        color: root.filled ? "black" : "white"
        strokeWidth: 2.2
    }
    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }

    // Netflix tooltip: light box with bold dark text and a small caret, above the button (100 ms delay).
    property bool tipShown: false
    onHoveredChanged: if (hovered) tipTimer.restart(); else { tipTimer.stop(); tipShown = false }
    Timer { id: tipTimer; interval: 100; onTriggered: root.tipShown = root.hovered }
    Item {
        visible: root.tooltip !== "" && opacity > 0
        opacity: root.tipShown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
        z: 10
        width: tipBox.width; height: tipBox.height + 7
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.top
        anchors.bottomMargin: 6
        Rectangle {
            id: tipBox
            width: tipText.implicitWidth + Math.round(tipText.font.pixelSize * 1.7); height: tipText.implicitHeight + Math.round(tipText.font.pixelSize * 0.8)
            radius: 4
            color: "#E6E6E6"
            Text {
                id: tipText
                anchors.centerIn: parent
                text: root.tooltip
                font.family: Theme.font; font.pixelSize: Math.round(Theme.clamp(Theme.vw(0.95), 13, 22)); font.weight: Font.Bold
                color: "#181818"
            }
        }
        Rectangle {
            width: 12; height: 12; rotation: 45
            color: "#E6E6E6"
            anchors.horizontalCenter: parent.horizontalCenter
            y: tipBox.height - 7
        }
    }
}
