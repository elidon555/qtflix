import QtQuick
import QtQuick.Shapes

// Vector icon drawn on a 24x24 grid, scaled to `size`. Netflix-style glyphs.
Item {
    id: root
    property string name: "play"
    property color color: "white"
    property real size: 24
    property real strokeWidth: 2
    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    readonly property string fillPath: IconPaths.fills[name] ?? ""
    readonly property string strokePath: IconPaths.strokes[name] ?? ""

    Shape {
        width: 24; height: 24
        scale: root.size / 24
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: root.fillPath !== "" ? root.color : "transparent"
            strokeColor: "transparent"
            strokeWidth: 0
            PathSvg { path: root.fillPath }
        }
        ShapePath {
            fillColor: "transparent"
            strokeColor: root.strokePath !== "" ? root.color : "transparent"
            strokeWidth: root.strokeWidth
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: root.strokePath }
        }
    }
}
