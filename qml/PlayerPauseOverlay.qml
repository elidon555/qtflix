import QtQuick
import QtFlix

// Netflix "You're watching" screen shown when playback has been paused and the mouse is idle.
Item {
    id: ov
    property bool shown: false
    property string title: ""
    property string episodeLine: ""
    property string description: ""

    opacity: shown ? 1 : 0
    visible: opacity > 0.01
    Behavior on opacity { NumberAnimation { duration: 500; easing.type: Easing.InOutQuad } }

    Rectangle { anchors.fill: parent; color: Qt.rgba(0, 0, 0, 0.5) }
    // extra darkening towards the left where the text sits
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.55) }
            GradientStop { position: 0.6; color: Qt.rgba(0, 0, 0, 0.0) }
        }
    }

    Column {
        id: block
        x: Math.round(ov.width * 0.1)
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(ov.width * 0.45, 760)
        spacing: 14
        transform: Translate { x: ov.shown ? 0 : -24; Behavior on x { NumberAnimation { duration: 500; easing.type: Easing.OutCubic } } }

        Text {
            text: qsTr("You're watching")
            color: "#cccccc"
            font.family: Theme.font
            font.pixelSize: 18
        }
        Text {
            width: parent.width
            text: ov.title
            color: "white"
            font.family: Theme.font
            font.pixelSize: 52
            font.weight: Font.Bold
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
            lineHeight: 0.95
        }
        Text {
            visible: ov.episodeLine !== ""
            width: parent.width
            text: ov.episodeLine
            color: "white"
            font.family: Theme.font
            font.pixelSize: 22
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Text {
            visible: ov.description !== ""
            width: parent.width
            topPadding: 4
            text: ov.description
            color: "#dddddd"
            font.family: Theme.font
            font.pixelSize: 18
            lineHeight: 1.25
            wrapMode: Text.WordWrap
            maximumLineCount: 4
            elide: Text.ElideRight
        }
    }

    Text {
        anchors.right: parent.right; anchors.rightMargin: Math.round(ov.width * 0.05)
        anchors.bottom: parent.bottom; anchors.bottomMargin: Math.round(ov.height * 0.1)
        text: qsTr("Paused")
        color: "#cccccc"
        font.family: Theme.font
        font.pixelSize: 20
    }
}
