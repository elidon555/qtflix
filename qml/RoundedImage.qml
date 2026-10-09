import QtQuick

// An image with rounded corners drawn in ONE pass: a ShaderEffect samples the (hidden) Image's texture directly,
// cover-crops it like Image.PreserveAspectCrop and cuts the corners with an antialiased SDF (no offscreen layer, no
// mask item, crisp without MSAA). `bg` is the placeholder underneath; the image fades in only on its first load
// (re-decodes at another size keep the old texture until the new one is ready). Requested sizes are quantized
// (Theme.thumbSize) so layout jitter never triggers new decodes. On the software scene graph (no shaders) it
// falls back to a plain square Image over a (rounded) bg Rectangle.
Rectangle {
    id: root
    property url source
    // corners: Rectangle's radius / topLeftRadius / topRightRadius / bottomRightRadius / bottomLeftRadius
    property color bg: "#2F2F2F"
    color: software ? bg : "transparent"           // the shader draws bg itself (a transparent Rectangle draws nothing)
    property int fadeDuration: 300
    property real requestWidth: width              // decode width (before quantizing)
    readonly property alias status: img.status

    // only request once laid out (a 0 x 0 item would decode a throwaway thumbnail first)
    readonly property bool sized: width >= 1 && height >= 1
    readonly property bool software: GraphicsInfo.api === GraphicsInfo.Software
    property url shownSource                     // source of the texture currently on screen (fade bookkeeping)
    property real imageOpacity: 0

    function stripQuery(u: string): string { const i = u.indexOf("?"); return i < 0 ? u : u.substring(0, i) }
    onSourceChanged: {
        // a different picture (delegate reuse, other title) fades in again; ?v= / size refreshes don't
        if (stripQuery(source.toString()) !== stripQuery(shownSource.toString())) { fade.stop(); imageOpacity = 0 }
    }

    Image {
        id: img
        anchors.fill: parent
        visible: root.software
        opacity: root.imageOpacity
        source: root.sized ? root.source : ""
        sourceSize: Theme.thumbSize(root.requestWidth)
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
        retainWhileLoading: true
        onStatusChanged: {
            if (status !== Image.Ready) return
            root.shownSource = root.source
            if (root.imageOpacity < 1 && !fade.running) fade.restart()
        }
    }
    NumberAnimation { id: fade; target: root; property: "imageOpacity"; to: 1; duration: root.fadeDuration }

    ShaderEffect {
        anchors.fill: parent
        visible: !root.software
        readonly property Image source: img
        readonly property size itemSize: Qt.size(root.width, root.height)
        readonly property vector4d radii: Qt.vector4d(root.topLeftRadius, root.topRightRadius,
                                                      root.bottomRightRadius, root.bottomLeftRadius)
        // PreserveAspectCrop: show the centred part of the texture that has the item's aspect ratio
        readonly property vector4d uvRect: {
            const iw = img.implicitWidth, ih = img.implicitHeight
            if (iw <= 0 || ih <= 0 || root.height <= 0) return Qt.vector4d(0, 0, 1, 1)
            const k = (root.width / root.height) / (iw / ih)
            return k < 1 ? Qt.vector4d((1 - k) / 2, 0, k, 1) : Qt.vector4d(0, (1 - 1 / k) / 2, 1, 1 / k)
        }
        readonly property color bg: root.bg
        readonly property real imageOpacity: img.status === Image.Ready || img.status === Image.Loading ? root.imageOpacity : 0
        fragmentShader: "qrc:/shaders/roundedimage.frag.qsb"
    }
}
