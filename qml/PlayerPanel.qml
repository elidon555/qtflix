import QtQuick
import QtFlix

// Base of the Netflix player popups (audio/subtitles, speed, episodes, volume):
// near-black translucent box, fades in/out, swallows clicks so they don't toggle playback.
Rectangle {
    id: panel
    property bool open: false
    readonly property bool hovered: hh.hovered
    default property alias content: holder.data
    property real padding: 0

    color: Qt.rgba(0.149, 0.149, 0.149, 0.96)   // Netflix popup grey #262626
    radius: 4
    opacity: open ? 1 : 0
    visible: opacity > 0.01
    Behavior on opacity { NumberAnimation { duration: 150 } }

    HoverHandler { id: hh }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: false
        onWheel: function(wheel) { wheel.accepted = true }
    }
    Item {
        id: holder
        anchors.fill: parent
        anchors.margins: panel.padding
    }
}
