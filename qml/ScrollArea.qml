import QtQuick

// Browser-like vertical scroller: no drag-to-scroll, smooth animated mouse wheel, direct touchpad.
// No visible scrollbar (Netflix hides it).
Flickable {
    id: f
    interactive: false
    clip: false
    boundsBehavior: Flickable.StopAtBounds
    flickableDirection: Flickable.VerticalFlick
    contentWidth: width
    property real wheelStep: 1.1          // px per angle-delta unit (120 per notch -> ~130px)
    property real maxY: Math.max(0, contentHeight - height)
    property real targetY: 0

    function scrollTo(y, animated) {
        targetY = Math.max(0, Math.min(maxY, y))
        if (animated === false) { anim.stop(); contentY = targetY }
        else { anim.to = targetY; anim.restart() }
    }
    onMaxYChanged: if (contentY > maxY) { anim.stop(); contentY = maxY; targetY = maxY }

    NumberAnimation { id: anim; target: f; property: "contentY"; duration: 260; easing.type: Easing.OutCubic }

    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        orientation: Qt.Vertical
        target: null
        onWheel: (ev) => {
            if (ev.pixelDelta.y !== 0) {
                anim.stop()
                f.targetY = Math.max(0, Math.min(f.maxY, f.contentY - ev.pixelDelta.y))
                f.contentY = f.targetY
            } else {
                const base = anim.running ? f.targetY : f.contentY
                f.scrollTo(base - ev.angleDelta.y * f.wheelStep, true)
            }
        }
    }
    Keys.onPressed: (ev) => {
        const page = f.height * 0.85
        if (ev.key === Qt.Key_PageDown || (ev.key === Qt.Key_Space && !(ev.modifiers & Qt.ShiftModifier))) { scrollTo(contentY + page); ev.accepted = true }
        else if (ev.key === Qt.Key_PageUp || ev.key === Qt.Key_Space) { scrollTo(contentY - page); ev.accepted = true }
        else if (ev.key === Qt.Key_Down) { scrollTo(contentY + 80); ev.accepted = true }
        else if (ev.key === Qt.Key_Up) { scrollTo(contentY - 80); ev.accepted = true }
        else if (ev.key === Qt.Key_Home) { scrollTo(0); ev.accepted = true }
        else if (ev.key === Qt.Key_End) { scrollTo(maxY); ev.accepted = true }
    }
}
