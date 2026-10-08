import QtQuick
import QtFlix

// "Next Episode" post-play card (bottom-right): thumbnail, label, title, a white
// "Next Episode" button whose background fills over 10 s before auto-playing, and "Watch Credits".
Item {
    id: card
    property url thumb
    property string label: ""            // "S5:E2 SNAFU"
    property bool shown: false
    property bool paused: false          // countdown halts while the user has paused playback
    property int countdownMs: 10000
    signal playNext()
    signal dismissed()

    width: 460
    height: content.implicitHeight
    opacity: shown ? 1 : 0
    visible: opacity > 0.01
    Behavior on opacity { NumberAnimation { duration: 250 } }
    transform: Translate { x: card.shown ? 0 : 40; Behavior on x { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } } }

    property real progress: 0
    onShownChanged: {
        progress = 0
        if (shown) countdown.restart(); else countdown.stop()
    }
    onPausedChanged: if (shown) { if (paused) countdown.pause(); else countdown.resume() }
    NumberAnimation {
        id: countdown
        target: card; property: "progress"
        from: 0; to: 1
        duration: card.countdownMs
        onFinished: if (card.shown && card.progress >= 1) card.playNext()
    }

    Column {
        id: content
        width: parent.width
        spacing: 14

        Rectangle {
            width: parent.width
            height: 112
            radius: 4
            color: Qt.rgba(0.08, 0.08, 0.08, 0.92)
            border.color: Qt.rgba(1, 1, 1, 0.08)
            Item {
                id: thumbBox
                x: 12; y: 12
                width: 157; height: 88
                Rectangle { anchors.fill: parent; radius: 2; color: "#222" }
                Image {
                    anchors.fill: parent
                    source: card.thumb
                    sourceSize: Qt.size(314, 176)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
                Image {
                    anchors.centerIn: parent
                    width: 28; height: 28
                    source: "qrc:/assets/icons/player-play.svg"
                    sourceSize: Qt.size(56, 56)
                    opacity: 0.9
                }
            }
            Column {
                anchors.left: thumbBox.right; anchors.leftMargin: 16
                anchors.right: parent.right; anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                Text {
                    text: qsTr("Next Episode")
                    color: "#b3b3b3"
                    font.family: Theme.font
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.8
                }
                Text {
                    width: parent.width
                    text: card.label
                    color: "white"
                    font.family: Theme.font
                    font.pixelSize: 18
                    font.weight: Font.Bold
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
            }
        }

        Row {
            anchors.right: parent.right
            spacing: 12
            // Watch Credits: outlined / translucent
            Rectangle {
                width: creditsText.implicitWidth + 48; height: 52
                radius: 4
                color: creditsMa.containsMouse ? Qt.rgba(1, 1, 1, 0.25) : Qt.rgba(0.43, 0.43, 0.43, 0.4)
                border.color: Qt.rgba(1, 1, 1, 0.75)
                border.width: 1
                Text {
                    id: creditsText
                    anchors.centerIn: parent
                    text: qsTr("Watch Credits")
                    color: "white"
                    font.family: Theme.font
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                MouseArea {
                    id: creditsMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: card.dismissed()
                }
            }
            // Next Episode: white, with a darker fill sweeping left -> right during the countdown
            Rectangle {
                id: nextBtn
                width: nextRow.implicitWidth + 48; height: 52
                radius: 4
                color: nextMa.containsMouse ? "#e6e6e6" : "white"
                clip: true
                Rectangle {
                    width: parent.width * card.progress
                    height: parent.height
                    color: "#bdbdbd"
                }
                Row {
                    id: nextRow
                    anchors.centerIn: parent
                    spacing: 12
                    Image {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 22; height: 22
                        source: "qrc:/assets/icons/player-play-black.svg"
                        sourceSize: Qt.size(44, 44)
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Next Episode")
                        color: "black"
                        font.family: Theme.font
                        font.pixelSize: 18
                        font.weight: Font.Bold
                    }
                }
                MouseArea {
                    id: nextMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: card.playNext()
                }
            }
        }
    }
}
