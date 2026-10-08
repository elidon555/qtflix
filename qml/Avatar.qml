pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes

// Profile avatar: a coloured tile with a simple white emblem. `variant` picks the emblem
// (0 play, 1 equalizer, 2 reel, 3 spark), so profiles of the same colour still look different.
Rectangle {
    id: root
    property color avatarColor: "#0071EB"
    property int variant: 0
    property color featureColor: "white"
    radius: Math.max(4, width * 0.12)
    implicitWidth: 32; implicitHeight: 32
    clip: true
    gradient: Gradient {
        orientation: Gradient.Vertical
        GradientStop { position: 0; color: Qt.lighter(root.avatarColor, 1.25) }
        GradientStop { position: 1; color: Qt.darker(root.avatarColor, 1.25) }
    }

    readonly property real u: width / 100
    readonly property int kind: ((variant % 4) + 4) % 4

    // 0: play button in a ring
    Item {
        anchors.fill: parent
        visible: root.kind === 0
        Rectangle {
            anchors.centerIn: parent
            width: root.u * 58; height: width; radius: width / 2
            color: "transparent"; border.color: root.featureColor; border.width: root.u * 7
        }
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                fillColor: root.featureColor; strokeColor: "transparent"
                startX: root.u * 43; startY: root.u * 36
                PathLine { x: root.u * 66; y: root.u * 50 }
                PathLine { x: root.u * 43; y: root.u * 64 }
                PathLine { x: root.u * 43; y: root.u * 36 }
            }
        }
    }

    // 1: equalizer bars
    Row {
        anchors.centerIn: parent
        visible: root.kind === 1
        spacing: root.u * 7
        height: root.u * 52
        Repeater {
            model: [0.55, 1.0, 0.75, 0.4]
            Rectangle {
                id: bar
                required property var modelData
                anchors.bottom: parent.bottom
                width: root.u * 10; height: root.u * 52 * bar.modelData
                radius: width / 2; color: root.featureColor
            }
        }
    }

    // 2: film reel
    Item {
        anchors.fill: parent
        visible: root.kind === 2
        Rectangle {
            anchors.centerIn: parent
            width: root.u * 60; height: width; radius: width / 2
            color: root.featureColor
        }
        Repeater {
            model: 5
            Rectangle {
                id: hole
                required property int index
                readonly property real a: hole.index * 2 * Math.PI / 5 - Math.PI / 2
                width: root.u * 13; height: width; radius: width / 2
                x: root.width / 2 + Math.cos(hole.a) * root.u * 17 - width / 2
                y: root.height / 2 + Math.sin(hole.a) * root.u * 17 - height / 2
                color: root.avatarColor
            }
        }
        Rectangle {
            anchors.centerIn: parent
            width: root.u * 8; height: width; radius: width / 2
            color: root.avatarColor
        }
    }

    // 3: four-point spark
    Shape {
        anchors.fill: parent
        visible: root.kind === 3
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: root.featureColor; strokeColor: "transparent"
            startX: root.u * 50; startY: root.u * 20
            PathQuad { x: root.u * 80; y: root.u * 50; controlX: root.u * 55; controlY: root.u * 45 }
            PathQuad { x: root.u * 50; y: root.u * 80; controlX: root.u * 55; controlY: root.u * 55 }
            PathQuad { x: root.u * 20; y: root.u * 50; controlX: root.u * 45; controlY: root.u * 55 }
            PathQuad { x: root.u * 50; y: root.u * 20; controlX: root.u * 45; controlY: root.u * 45 }
        }
    }
}
