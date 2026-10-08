import QtQuick
import QtFlix

// Transient center feedback (play / pause / ±10 s / volume) - a dark disc with an icon that
// grows and fades out over 500 ms, like Netflix does for keyboard actions.
Item {
    id: root
    width: 110; height: 110
    opacity: 0
    visible: opacity > 0

    property url icon
    property string overlayText: ""

    function flash(iconUrl, txt) {
        icon = iconUrl
        overlayText = txt || ""
        anim.restart()
    }

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: Qt.rgba(0, 0, 0, 0.55)
    }
    Image {
        anchors.centerIn: parent
        width: 52; height: 52
        source: root.icon
        sourceSize: Qt.size(104, 104)
        Text {
            visible: root.overlayText !== ""
            anchors.centerIn: parent
            anchors.verticalCenterOffset: 1
            text: root.overlayText
            color: "white"
            font.family: Theme.font
            font.pixelSize: 15
            font.weight: Font.Bold
        }
    }

    ParallelAnimation {
        id: anim
        NumberAnimation { target: root; property: "opacity"; from: 1; to: 0; duration: 500; easing.type: Easing.InQuad }
        NumberAnimation { target: root; property: "scale"; from: 0.85; to: 1.25; duration: 500; easing.type: Easing.OutCubic }
    }
}
