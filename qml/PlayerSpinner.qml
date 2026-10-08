import QtQuick
import QtQuick.Shapes
import QtFlix

// Netflix-style buffering spinner: a thin red ring sweeping around.
Item {
    id: root
    property real size: 72
    property real thickness: 5
    width: size; height: size

    Shape {
        id: ring
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        layer.enabled: true
        layer.samples: 4
        ShapePath {
            strokeColor: Theme.red
            strokeWidth: root.thickness
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: root.size / 2; centerY: root.size / 2
                radiusX: root.size / 2 - root.thickness; radiusY: radiusX
                startAngle: 0
                sweepAngle: 270
            }
        }
        RotationAnimator on rotation {
            from: 0; to: 360; duration: 900
            loops: Animation.Infinite
            running: root.visible
        }
    }
}
