import QtQuick

// ScrollArea's browser-like scrolling (no drag, smooth animated wheel, direct touchpad, keyboard paging) for a
// virtualizing view (ScrollList / ScrollGrid). Positions are measured from the top of the content:
// scrollY = contentY - originY (a header puts originY above 0, and ListView moves it a little when delegates of an
// unexpected height are created above the viewport). The view forwards its wheel and key events here.
QtObject {
    id: s
    required property Flickable view
    property real wheelStep: 1.1          // px per angle-delta unit (120 per notch -> ~130px)
    readonly property real scrollY: view.contentY - view.originY
    readonly property real maxY: Math.max(0, view.contentHeight - view.height)
    property real targetY: 0              // scroll target, in scrollY units

    function scrollTo(y: real, animated: bool) {
        targetY = Math.max(0, Math.min(maxY, y))
        if (!animated) { anim.stop(); view.contentY = view.originY + targetY }
        else { anim.to = view.originY + targetY; anim.restart() }
    }
    onMaxYChanged: if (scrollY > maxY) scrollTo(maxY, false)
    // A header that grows after creation (the billboard sizes itself once the window has its size) moves originY
    // while contentY stays, which would leave an unscrolled page part-way down: stay pinned to the top.
    readonly property Connections pinTop: Connections {
        target: s.view
        function onOriginYChanged() { if (s.targetY === 0 && !s.anim.running) s.view.contentY = s.view.originY }
    }

    function wheel(ev: WheelEvent) {
        if (ev.pixelDelta.y !== 0) {
            scrollTo(scrollY - ev.pixelDelta.y, false)
        } else {
            const base = anim.running ? targetY : scrollY
            scrollTo(base - ev.angleDelta.y * wheelStep, true)
        }
    }
    function key(ev: KeyEvent) {
        const page = view.height * 0.85
        if (ev.key === Qt.Key_PageDown || (ev.key === Qt.Key_Space && !(ev.modifiers & Qt.ShiftModifier))) scrollTo(scrollY + page, true)
        else if (ev.key === Qt.Key_PageUp || ev.key === Qt.Key_Space) scrollTo(scrollY - page, true)
        else if (ev.key === Qt.Key_Down) scrollTo(scrollY + 80, true)
        else if (ev.key === Qt.Key_Up) scrollTo(scrollY - 80, true)
        else if (ev.key === Qt.Key_Home) scrollTo(0, true)
        else if (ev.key === Qt.Key_End) scrollTo(maxY, true)
        else return
        ev.accepted = true
    }

    readonly property NumberAnimation anim: NumberAnimation {
        target: s.view; property: "contentY"; duration: 260; easing.type: Easing.OutCubic
    }
}
