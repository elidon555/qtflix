import QtQuick
import QtQuick.Shapes
import QtFlix

// Busy indicator: a grey ring with a red quarter arc spinning on it. Curve-rendered (smooth without MSAA, no
// clip/stencil while rotating).
Item {
    id: sp
    property real size: 24
    property real lineWidth: 2.5
    property color trackColor: "#333333"
    width: size; height: size
    Rectangle {
        anchors.fill: parent; radius: width / 2
        color: "transparent"; border.width: sp.lineWidth; border.color: sp.trackColor
    }
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: "transparent"
            strokeColor: Theme.red
            strokeWidth: sp.lineWidth
            capStyle: ShapePath.FlatCap
            PathAngleArc {
                centerX: sp.size / 2; centerY: sp.size / 2
                radiusX: (sp.size - sp.lineWidth) / 2; radiusY: radiusX
                startAngle: 180; sweepAngle: 90
            }
        }
    }
    RotationAnimation on rotation { from: 0; to: 360; duration: 900; loops: Animation.Infinite; running: sp.visible }
}
