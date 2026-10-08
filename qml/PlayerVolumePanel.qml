import QtQuick
import QtFlix

// Vertical volume slider that pops above the volume button (red fill, red knob).
PlayerPanel {
    id: p
    property real level: 1.0            // 0..1
    property bool muted: false
    signal levelPicked(real level)

    width: 52
    height: 190
    color: Qt.rgba(0.149, 0.149, 0.149, 0.96)

    readonly property real shown: muted ? 0 : level

    Item {
        id: track
        anchors.horizontalCenter: parent.horizontalCenter
        y: 24
        width: 8
        height: p.height - 48
        Rectangle { anchors.fill: parent; radius: 1; color: "#5b5b5b" }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: parent.height * p.shown
            color: Theme.red
        }
        Rectangle {
            width: 20; height: 20; radius: 10
            color: Theme.red
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height * (1 - p.shown) - height / 2
            scale: volMa.pressed ? 1.2 : 1
            Behavior on scale { NumberAnimation { duration: 100 } }
        }
    }
    MouseArea {
        id: volMa
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        preventStealing: true
        function pick(y) {
            var v = 1 - (y - track.y) / track.height
            p.levelPicked(Math.max(0, Math.min(1, v)))
        }
        onPressed: function(m) { pick(m.y) }
        onPositionChanged: function(m) { if (pressed) pick(m.y) }
        onWheel: function(w) { p.levelPicked(Math.max(0, Math.min(1, p.level + (w.angleDelta.y > 0 ? 0.05 : -0.05)))) }
    }
}
