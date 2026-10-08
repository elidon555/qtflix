pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes
import QtQuick.Effects
import QtFlix

// Intro: the ring of the "q" mark draws itself, the tail and play button snap in, the wordmark slides
// out of the mark, a warm glow pulses, then everything fades. ~2.2 s, skippable with a click or any key.
Rectangle {
    id: root
    color: "black"
    focus: true
    signal finished()

    property bool done: false
    function finish() {
        if (done) return
        done = true
        timeline.stop()
        root.finished()
    }

    readonly property real mh: Math.round(Math.min(height * 0.2, 160))   // mark height
    readonly property real k: mh / 100                                    // mark units -> px

    Keys.onPressed: (ev) => { ev.accepted = true; root.finish() }
    MouseArea { anchors.fill: parent; cursorShape: Qt.BlankCursor; onClicked: root.finish() }

    Item {
        id: stage
        anchors.centerIn: parent
        width: markItem.width + word.width * word.reveal + gap.width * word.reveal
        height: root.mh
        property real glow: 0
        property real lift: 1
        scale: lift

        Item {
            id: markItem
            width: root.mh; height: root.mh
            layer.enabled: true
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: "#FF3B1F"
                shadowBlur: 1.0
                blurMax: 64
                shadowOpacity: stage.glow
                shadowHorizontalOffset: 0
                shadowVerticalOffset: 0
            }

            // ring, drawn as a sweeping arc
            Shape {
                id: ring
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                property real sweep: 0
                ShapePath {
                    strokeWidth: 14 * root.k
                    capStyle: ShapePath.RoundCap
                    fillColor: "transparent"
                    strokeColor: "#F2261A"
                    PathAngleArc {
                        centerX: 46 * root.k; centerY: 46 * root.k
                        radiusX: 34 * root.k; radiusY: 34 * root.k
                        startAngle: 45
                        sweepAngle: ring.sweep
                    }
                }
            }
            // tail
            Shape {
                id: tail
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                property real p: 0
                opacity: p
                transform: Translate { x: -10 * root.k * (1 - tail.p); y: -10 * root.k * (1 - tail.p) }
                ShapePath {
                    strokeColor: "transparent"
                    fillColor: "#E50914"
                    scale: Qt.size(root.k, root.k)
                    PathSvg { path: "M63 76l13-13l20 20a4 4 0 0 1 0 5.7l-7.3 7.3a4 4 0 0 1-5.7 0z" }
                }
            }
            // play button
            Shape {
                id: play
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                property real p: 0
                opacity: Math.min(1, p * 1.5)
                transform: Scale { origin.x: 48 * root.k; origin.y: 46 * root.k; xScale: play.p; yScale: play.p }
                ShapePath {
                    strokeColor: "transparent"
                    fillColor: "white"
                    scale: Qt.size(root.k, root.k)
                    PathSvg { path: "M38 31.5v29a2 2 0 0 0 3 1.7l23-14.5a2 2 0 0 0 0-3.4l-23-14.5a2 2 0 0 0-3 1.7z" }
                }
            }
        }

        Item { id: gap; anchors.left: markItem.right; width: root.mh * 0.18; height: 1 }

        // wordmark, revealed left to right out of the mark
        Item {
            id: wordClip
            anchors.left: gap.right
            anchors.verticalCenter: markItem.verticalCenter
            width: word.width * word.reveal
            height: word.height
            clip: true
            Text {
                id: word
                property real reveal: 0
                text: "qtflix"
                font.family: Theme.font
                font.pixelSize: Math.round(root.mh * 1.1)
                font.weight: Font.Black
                font.letterSpacing: -Math.round(root.mh * 0.04)
                color: "white"
                x: -width * 0.25 * (1 - reveal)
                opacity: reveal
            }
        }
    }

    Rectangle { id: fadeOut; anchors.fill: parent; color: "black"; opacity: 0 }

    SequentialAnimation {
        id: timeline
        running: true
        PauseAnimation { duration: 150 }
        NumberAnimation { target: ring; property: "sweep"; from: 0; to: 360; duration: 520; easing.type: Easing.InOutCubic }
        ParallelAnimation {
            NumberAnimation { target: tail; property: "p"; to: 1; duration: 200; easing.type: Easing.OutCubic }
            NumberAnimation { target: play; property: "p"; to: 1; duration: 320; easing.type: Easing.OutBack; easing.overshoot: 2.2 }
        }
        ParallelAnimation {
            NumberAnimation { target: word; property: "reveal"; to: 1; duration: 420; easing.type: Easing.OutCubic }
            SequentialAnimation {
                NumberAnimation { target: stage; property: "glow"; to: 0.85; duration: 260; easing.type: Easing.OutQuad }
                NumberAnimation { target: stage; property: "glow"; to: 0.3; duration: 420; easing.type: Easing.InOutQuad }
            }
        }
        PauseAnimation { duration: 260 }
        ParallelAnimation {
            NumberAnimation { target: stage; property: "lift"; to: 1.06; duration: 360; easing.type: Easing.InQuad }
            NumberAnimation { target: fadeOut; property: "opacity"; to: 1; duration: 360; easing.type: Easing.InQuad }
        }
        ScriptAction { script: root.finish() }
    }
}
