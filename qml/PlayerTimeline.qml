pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects
import QtFlix

// Netflix seek bar: thin grey track, red elapsed part, red round knob (16 px, 20 px while scrubbing);
// the track thickens on hover; click/drag to seek. Hovering shows a preview frame from the trickplay
// sprite sheets (when generated) with the time under it.
Item {
    id: tl
    property real duration: 0
    property real position: 0
    final property Trickplay trickplay: null
    property real forceHoverFraction: -1         // dev/screenshot aid
    readonly property bool hovered: ma.containsMouse || ma.pressed
    readonly property bool dragging: ma.pressed
    property real dragPos: 0
    signal seekRequested(real ms, bool done)

    implicitHeight: 24
    readonly property real shownPos: ma.pressed ? dragPos : position
    readonly property real frac: duration > 0 ? Math.max(0, Math.min(1, shownPos / duration)) : 0

    function fmt(ms: real): string {
        var s = Math.max(0, Math.floor(ms / 1000))
        var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60
        var ss = (sec < 10 ? "0" : "") + sec
        return h > 0 ? h + ":" + (m < 10 ? "0" : "") + m + ":" + ss : m + ":" + ss
    }
    function posAt(x: real): real {
        return duration * Math.max(0, Math.min(1, x / Math.max(1, track.width)))
    }

    Rectangle {
        id: track
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: tl.hovered ? 6 : 4
        color: Qt.rgba(1, 1, 1, 0.3)
        Behavior on height { NumberAnimation { duration: 100 } }

        // hover "ghost" segment (lighter) between the playhead and the cursor
        Rectangle {
            visible: ma.containsMouse && !ma.pressed
            readonly property real hx: Math.max(0, Math.min(track.width, ma.mouseX))
            readonly property real px: track.width * tl.frac
            x: Math.min(hx, px); width: Math.abs(hx - px)
            height: parent.height
            color: Qt.rgba(1, 1, 1, 0.25)
        }
        Rectangle {
            width: track.width * tl.frac
            height: parent.height
            color: Theme.red
        }
    }

    Rectangle {
        id: knob
        property real size: ma.pressed ? 20 : 16
        width: size; height: size; radius: size / 2
        color: Theme.red
        x: track.width * tl.frac - width / 2
        anchors.verticalCenter: parent.verticalCenter
        Behavior on size { NumberAnimation { duration: 100 } }
    }

    // ---- hover preview: trickplay frame (when available) + time --------------------------------
    Item {
        id: preview
        readonly property bool forced: tl.forceHoverFraction >= 0
        visible: (ma.containsMouse || ma.pressed || forced) && tl.duration > 0
        readonly property real cx: ma.pressed ? track.width * tl.frac
                                 : forced ? track.width * tl.forceHoverFraction
                                 : Math.max(0, Math.min(track.width, ma.mouseX))
        readonly property real ms: ma.pressed ? tl.dragPos : tl.posAt(cx)
        readonly property bool trickReady: !!tl.trickplay && tl.trickplay.ready && tl.trickplay.frameWidth > 0
        // empty map while the sheet for `ms` is still being generated
        readonly property var frame: (visible && trickReady && tl.trickplay.frameCount > 0) ? tl.trickplay.frameFor(ms) : null
        readonly property bool hasThumb: !!frame && frame.index !== undefined
        // sheet the pointer is on, else the playhead's (so the first hover finds it decoded); the
        // playhead part follows the clock, so it is kept free of `var` lookups (AOT-compiled)
        readonly property int hoverSheet: hasThumb ? frame.sheet : -1
        readonly property int playheadSheet: trickReady
            ? Math.floor(tl.position / tl.trickplay.intervalMs / (tl.trickplay.columns * tl.trickplay.rows)) : -1
        readonly property int sheet: hoverSheet >= 0 ? hoverSheet : playheadSheet
        onFrameChanged: syncShown()
        // The tile offsets follow the frame only once its sheet is decoded (sheets load asynchronously;
        // until then the previous sheet stays on screen with its own offsets).
        function syncShown() {
            if (hasThumb && sheetImage.status === Image.Ready && String(sheetImage.source) === String(frame.source)) {
                sheetImage.shownX = frame.x
                sheetImage.shownY = frame.y
                sheetImage.shown = true
            }
        }
        readonly property real thumbW: 240
        readonly property real thumbH: Math.round(thumbW * 9 / 16)
        width: hasThumb ? thumbW : timeText.implicitWidth + 20
        height: (hasThumb ? thumbH + 8 : 0) + timeText.implicitHeight
        x: Math.max(0, Math.min(tl.width - width, cx - width / 2))
        y: -height - 18

        // 16:9 frame; scope films are letterboxed inside it like the video itself
        Rectangle {
            id: thumbBox
            visible: preview.hasThumb
            width: preview.thumbW; height: preview.thumbH
            radius: 4
            color: "black"
            clip: true
            // round the corners of the frame (only rendered while the preview is visible)
            layer.enabled: preview.visible
            layer.effect: MultiEffect {
                maskEnabled: true
                maskSource: thumbMask
                maskThresholdMin: 0.5
                maskSpreadAtMin: 1.0
            }
            Item {
                id: tile
                readonly property real s: preview.thumbW / Math.max(1, tl.trickplay ? tl.trickplay.frameWidth : 1)
                width: (tl.trickplay ? tl.trickplay.frameWidth : 0) * s
                height: (tl.trickplay ? tl.trickplay.frameHeight : 0) * s
                anchors.centerIn: parent
                clip: true
                Image {
                    id: sheetImage
                    property bool shown: false  // a tile of the loaded sheet is placed (shownX/Y)
                    property real shownX: 0
                    property real shownY: 0
                    source: preview.hasThumb ? preview.frame.source : ""
                    x: -shownX * tile.s
                    y: -shownY * tile.s
                    visible: shown
                    width: sourceSize.width * tile.s
                    height: sourceSize.height * tile.s
                    smooth: true
                    asynchronous: true     // a sheet decodes off the GUI thread; neighbours are preloaded below
                    cache: true
                    retainWhileLoading: true
                    onStatusChanged: preview.syncShown()
                }
            }
        }
        // Keep the current sheet and both neighbours decoded (and referenced, so the pixmap cache can't
        // drop them): crossing a sheet boundary then swaps instantly. Slot k holds whichever of
        // sheet-1 .. sheet+1 is k modulo 3, so moving one sheet on reloads only one slot.
        Repeater {
            model: 3
            Image {
                required property int index
                readonly property int sheetNo: preview.sheet < 0 ? -1
                    : preview.sheet - 1 + ((index - (preview.sheet - 1)) % 3 + 3) % 3
                visible: false
                asynchronous: true
                cache: true
                source: tl.trickplay && sheetNo >= 0 && sheetNo < tl.trickplay.sheetCount
                        ? tl.trickplay.sheetSource(sheetNo) : ""
            }
        }
        Rectangle {
            id: thumbMask
            width: preview.thumbW; height: preview.thumbH
            radius: 4
            visible: false
            layer.enabled: true
        }
        Text {
            id: timeText
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            text: tl.fmt(preview.ms)
            color: "white"
            style: Text.Outline
            styleColor: Qt.rgba(0, 0, 0, 0.35)
            font.family: Theme.font
            font.pixelSize: 16
            font.weight: Font.Medium
            font.features: { "tnum": 1 }
        }
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        anchors.topMargin: -10
        anchors.bottomMargin: -10
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        preventStealing: true
        onPressed: function(mouse) { tl.dragPos = tl.posAt(mouse.x); tl.seekRequested(tl.dragPos, false) }
        onPositionChanged: function(mouse) {
            if (pressed) { tl.dragPos = tl.posAt(mouse.x); tl.seekRequested(tl.dragPos, false) }
        }
        onReleased: tl.seekRequested(tl.dragPos, true)
    }
}
