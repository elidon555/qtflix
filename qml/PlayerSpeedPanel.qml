pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// "Playback Speed" popup with a 5-stop horizontal slider (0.5x .. 1.5x), Netflix style.
PlayerPanel {
    id: p
    property real rate: 1.0
    signal rateSelected(real rate)
    readonly property var rates: [0.5, 0.75, 1.0, 1.25, 1.5]
    readonly property var labels: ["0.5x", "0.75x", "1x (Normal)", "1.25x", "1.5x"]
    readonly property int currentIndex: {
        for (var i = 0; i < rates.length; ++i) if (Math.abs(rates[i] - rate) < 0.01) return i
        return 2
    }

    width: 640
    height: 200
    color: Qt.rgba(0.149, 0.149, 0.149, 0.97)

    Text {
        x: 24; y: 24
        text: qsTr("Playback Speed")
        color: "white"
        font.family: Theme.font
        font.pixelSize: 24
        font.weight: Font.Bold
    }

    Item {
        id: slider
        x: 70; width: p.width - 140
        y: 108
        height: 2
        readonly property real step: width / (p.rates.length - 1)

        Rectangle { anchors.fill: parent; color: "#808080" }

        Repeater {
            model: p.rates.length
            delegate: Item {
                id: stop
                required property int index
                readonly property bool selected: index === p.currentIndex
                x: index * slider.step - width / 2
                y: -height / 2 + 1
                width: 110; height: 40
                // small dot for unselected stops; big white disc with grey ring for the selected one
                Rectangle {
                    anchors.centerIn: parent
                    width: stop.selected ? 28 : (stopMa.containsMouse ? 16 : 12)
                    height: width; radius: width / 2
                    color: stop.selected ? "white" : "#b3b3b3"
                    border.width: stop.selected ? 5 : 0
                    border.color: "#7a7a7a"
                    Behavior on width { NumberAnimation { duration: 100 } }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: parent.height + 6
                    text: p.labels[stop.index]
                    color: stop.selected ? "white" : "#b3b3b3"
                    font.family: Theme.font
                    font.pixelSize: 16
                    font.weight: stop.selected ? Font.DemiBold : Font.Normal
                }
                MouseArea {
                    id: stopMa
                    anchors.fill: parent
                    anchors.bottomMargin: -32
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: p.rateSelected(p.rates[stop.index])
                }
            }
        }
    }
}
