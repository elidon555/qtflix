import QtQuick
import QtFlix

// Netflix billboard / modal button: "▶ Play" (primary, white) or "ⓘ More Info" (secondary, gray glass).
Rectangle {
    id: root
    property string text: "Play"
    property string iconName: "play"
    property bool primary: true
    property real fontSize: 18
    signal clicked()

    readonly property bool hovered: ma.containsMouse
    implicitHeight: Math.round(fontSize * 2.5)
    implicitWidth: row.implicitWidth + Math.round(fontSize * 1.25) + Math.round(fontSize * 1.5)
    radius: 4
    color: primary ? (hovered ? Qt.rgba(1, 1, 1, 0.75) : "white")
                   : (hovered ? Qt.rgba(109/255, 109/255, 110/255, 0.4) : Qt.rgba(109/255, 109/255, 110/255, 0.7))
    scale: ma.pressed ? 0.97 : 1
    Behavior on scale { NumberAnimation { duration: 80 } }

    Row {
        id: row
        x: Math.round(root.fontSize * 1.2)
        anchors.verticalCenter: parent.verticalCenter
        spacing: Math.round(root.fontSize * 0.6)
        Icon {
            anchors.verticalCenter: parent.verticalCenter
            name: root.iconName
            size: Math.round(root.fontSize * (root.iconName === "play" ? 1.45 : 1.4))
            color: root.primary ? "black" : "white"
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            font.family: Theme.font
            font.pixelSize: root.fontSize
            font.weight: Font.Bold
            color: root.primary ? "black" : "white"
        }
    }
    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
