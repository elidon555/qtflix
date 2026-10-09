import QtQuick

// A browse page as a virtualized GridView (cards are created only near the viewport) with ScrollArea's
// scrolling; see PageScroller. scrollY / scrollTo() / maxY are measured from the top of the content.
GridView {
    id: f
    interactive: false
    clip: false
    boundsBehavior: Flickable.StopAtBounds
    currentIndex: -1
    readonly property alias scrollY: ps.scrollY
    readonly property alias maxY: ps.maxY
    function scrollTo(y: real, animated: bool) { ps.scrollTo(y, animated) }

    PageScroller { id: ps; view: f }
    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        orientation: Qt.Vertical
        target: null
        onWheel: (ev) => ps.wheel(ev)
    }
    Keys.onPressed: (ev) => ps.key(ev)
}
