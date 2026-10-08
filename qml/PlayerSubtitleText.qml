import QtQuick
import QtQuick.Effects
import QtFlix

// One subtitle cue drawn with the user's "Subtitle Appearance" (Netflix options):
//   spec = { size: "small"|"medium"|"large", edge: "shadow"|"raised"|"depressed"|"outline"|"none",
//            background: "none"|"box", color: "white"|"yellow" }
Item {
    id: st
    property string text: ""
    property var spec: ({})
    property real basePixelSize: 32
    property real maxWidth: 800

    readonly property real sizeFactor: spec.size === "small" ? 0.75 : (spec.size === "large" ? 1.35 : 1.0)
    readonly property string edge: spec.edge || "shadow"
    readonly property bool boxed: spec.background === "box"
    readonly property real padX: boxed ? Math.round(txt.font.pixelSize * 0.35) : 0
    readonly property real padY: boxed ? Math.round(txt.font.pixelSize * 0.12) : 0

    implicitWidth: txt.width + 2 * padX
    implicitHeight: txt.height + 2 * padY
    width: implicitWidth
    height: implicitHeight

    Rectangle {
        anchors.fill: parent
        visible: st.boxed
        radius: 2
        color: Qt.rgba(0, 0, 0, 0.6)
    }

    Text {
        id: txt
        x: st.padX; y: st.padY
        width: Math.min(implicitWidth, st.maxWidth - 2 * st.padX)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        textFormat: Text.StyledText
        text: st.text
        color: st.spec.color === "yellow" ? "#FFE81A" : "#FFFFFF"
        style: st.edge === "outline" ? Text.Outline
             : st.edge === "raised" ? Text.Raised
             : st.edge === "depressed" ? Text.Sunken : Text.Normal
        styleColor: st.edge === "raised" || st.edge === "depressed" ? "#000000" : Qt.rgba(0, 0, 0, 0.95)
        font.family: Theme.font
        font.pixelSize: Math.round(st.basePixelSize * st.sizeFactor)
        font.weight: Font.DemiBold
        lineHeight: 1.1
        layer.enabled: st.visible && st.text !== "" && st.edge === "shadow"
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: "black"
            shadowOpacity: 0.9
            shadowBlur: 0.3
            shadowHorizontalOffset: 1
            shadowVerticalOffset: 2
        }
    }
}
