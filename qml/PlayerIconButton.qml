import QtQuick
import QtFlix

// Netflix player control: a white vector icon that grows on hover (scale 1.25, 100 ms).
Item {
    id: btn
    property url icon
    property real iconSize: 32
    property string overlayText: ""      // e.g. "10" drawn inside the rewind/forward arrows
    property bool active: false           // keep the "hovered" look (e.g. while its panel is open)
    readonly property bool hovered: ma.containsMouse
    signal clicked()

    implicitWidth: iconSize + 16
    implicitHeight: iconSize + 16

    Item {
        id: glyph
        anchors.centerIn: parent
        width: btn.iconSize; height: btn.iconSize
        scale: (btn.hovered || btn.active) ? 1.25 : 1.0
        Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }

        Image {
            anchors.fill: parent
            source: btn.icon
            sourceSize: Qt.size(btn.iconSize * 2, btn.iconSize * 2)   // stays crisp when scaled up
            smooth: true
            fillMode: Image.PreserveAspectFit
        }
        Text {
            visible: btn.overlayText !== ""
            anchors.centerIn: parent
            anchors.verticalCenterOffset: 1
            text: btn.overlayText
            color: "white"
            font.family: Theme.font
            font.pixelSize: Math.round(btn.iconSize * 0.30)
            font.weight: Font.Bold
            font.letterSpacing: -0.3
        }
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: btn.clicked()
    }
}
