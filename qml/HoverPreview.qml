pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects
import QtMultimedia
import QtFlix

// Netflix "mini modal": after a 500 ms hover the card grows 1.5x out of its slot (250 ms, ease-out). The
// box is clamped to the gutters, so the first card of a row grows to the right and the last one to the
// left (Netflix's transform-origin trick). A muted preview plays after ~1 s (destroyed on close). Closes
// 150 ms after the pointer leaves the box, immediately on scroll / page change / play / detail.
// Put ONE of these at the page root (anchors.fill: parent) and call open(item, globalRect) from a card's
// preview signal.
Item {
    id: root
    objectName: "hoverPreview"
    anchors.fill: parent
    z: 50

    property var item: null                  // snapshot map (Library.title)
    property rect src: Qt.rect(0, 0, 0, 0)   // card rect in root coords
    property real p: 0                       // 0 = card size, 1 = fully expanded
    property bool shown: false
    property bool muted: Theme.previewMuted
    property real pad: Theme.gutter
    readonly property bool hasItem: !!item && !!item.id
    readonly property bool isOpen: shown && !closeAnim.running
    readonly property alias boxItem: box

    // ---- geometry ----
    readonly property real boxW: Math.round(Math.max(300, src.width * 1.5))
    readonly property real imgH: Math.round(boxW * 9 / 16)
    readonly property real boxH: imgH + info.implicitHeight
    readonly property real finalX: Math.max(pad, Math.min(width - pad - boxW, src.x + src.width / 2 - boxW / 2))
    readonly property real finalY: Math.max(Theme.navH * 0.4, Math.min(height - boxH - 12, src.y + src.height / 2 - imgH / 2 - imgH * 0.08))
    readonly property real startY: src.y + src.height / 2 - (src.width * 9 / 16) / 2

    function open(it, globalRect) {
        if (!it || !it.id) return
        const pt = root.mapFromItem(null, globalRect.x, globalRect.y)
        const r = Qt.rect(pt.x, pt.y, globalRect.width, globalRect.height)
        if (shown && item && item.id === it.id && Math.abs(r.x - src.x) < 2 && Math.abs(r.y - src.y) < 2) {
            graceTimer.stop()
            if (closeAnim.running) { closeAnim.stop(); openAnim.restart() }
            return
        }
        stopMedia()
        closeAnim.stop()
        const m = Library.title(it.id)
        item = (m && m.id) ? m : copyItem(it)
        src = r
        p = 0
        shown = true
        graceTimer.stop()
        openAnim.restart()
        videoTimer.restart()
    }
    function close(immediate) {
        videoTimer.stop()
        graceTimer.stop()
        if (!shown) return
        if (immediate) {
            openAnim.stop(); closeAnim.stop()
            p = 0; shown = false; stopMedia()
        } else if (!closeAnim.running) {
            openAnim.stop()
            closeAnim.restart()
        }
    }
    function copyItem(it) {
        const keys = ["id", "title", "year", "path", "sourceUrl", "isSeries", "seasonCount", "episodeCount", "durationMs",
                      "quality", "cardImage", "backdropImage", "positionMs", "progress", "inMyList", "rating", "genres",
                      "match", "description", "logoImage", "hasMeta"]
        const o = {}
        for (const k of keys) o[k] = it[k]
        return o
    }
    function refresh() {
        if (!hasItem) return
        const m = Library.title(item.id)
        if (m && m.id) item = m
    }
    function stopMedia() {
        videoTimer.stop()
        videoShown = false
        videoWanted = false
    }

    Connections {
        target: Nav
        function onPageChanged() { root.close(true) }
        function onDetailIdChanged() { root.close(true) }
        function onPlayerPathChanged() { root.close(true) }
    }
    Connections {
        target: Library
        function onMyListChanged(id) { if (root.hasItem && id === root.item.id) root.refresh() }
        function onLibraryChanged() { root.close(true) }
    }
    onVisibleChanged: if (!visible) close(true)

    NumberAnimation { id: openAnim; target: root; property: "p"; to: 1; duration: 250; easing.type: Easing.OutCubic }
    SequentialAnimation {
        id: closeAnim
        NumberAnimation { target: root; property: "p"; to: 0; duration: 180; easing.type: Easing.InOutQuad }
        ScriptAction { script: { root.shown = false; root.stopMedia() } }
    }

    // ---- hover tracking: close 150 ms after the pointer leaves the box ----
    Timer { id: graceTimer; interval: 150; onTriggered: root.close(false) }

    // ---- video preview (MediaPlayer + VideoOutput live in a Loader: destroyed when closed) ----
    property bool videoWanted: false
    property bool videoShown: false
    property real seekTarget: 0
    Timer {
        id: videoTimer
        interval: 1000
        onTriggered: if (root.shown && root.hasItem && Theme.autoplayPreviews && root.item.sourceUrl
                         && root.item.sourceUrl.toString() !== "") { root.seekTarget = 0; root.videoWanted = true }
    }

    // ---------------- the box ----------------
    Item {
        id: box
        objectName: "hoverPreviewBox"
        visible: root.shown
        width: root.boxW
        height: root.boxH
        x: root.src.x + (root.finalX - root.src.x) * root.p
        y: root.startY + (root.finalY - root.startY) * root.p
        transformOrigin: Item.TopLeft
        scale: (root.src.width / root.boxW) + (1 - root.src.width / root.boxW) * root.p

        HoverHandler {
            id: boxHover
            onHoveredChanged: {
                if (hovered) graceTimer.stop()
                else if (root.shown) graceTimer.restart()
            }
        }
        // swallow clicks/hover so cards below don't react; clicking the box opens the detail modal
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: if (root.hasItem) Nav.openDetail(root.item.id)
        }

        // box-shadow: 0 3px 10px rgba(0,0,0,.75)
        RectangularShadow {
            anchors.fill: parent
            radius: 6
            blur: 10
            offset.y: 3
            spread: 0
            color: Qt.rgba(0, 0, 0, 0.75 * root.p)
        }

        Rectangle { anchors.fill: parent; radius: 6; color: Theme.bgElevated }

        // ---- media (rounded top corners via mask) ----
        Item {
            id: media
            width: parent.width
            height: root.imgH
            Item {
                id: mediaContent
                anchors.fill: parent
                layer.enabled: true
                layer.smooth: true
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: mediaMask
                    maskThresholdMin: 0.5
                    maskSpreadAtMin: 1.0
                }
                Rectangle { anchors.fill: parent; color: "#2F2F2F" }
                Image {
                    anchors.fill: parent
                    source: root.hasItem ? root.item.backdropImage : ""
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                    sourceSize: Qt.size(root.boxW, root.imgH)
                }
                Loader {
                    id: videoLoader
                    anchors.fill: parent
                    active: root.videoWanted && root.shown
                    sourceComponent: Item {
                        MediaPlayer {
                            id: player
                            source: root.item ? root.item.sourceUrl : ""
                            videoOutput: vout
                            audioOutput: AudioOutput { muted: root.muted; volume: 0.7 }
                            onMediaStatusChanged: {
                                if ((mediaStatus === MediaPlayer.LoadedMedia || mediaStatus === MediaPlayer.BufferedMedia)
                                        && root.seekTarget === 0 && duration > 0) {
                                    const start = root.item && root.item.positionMs > 0 && root.item.progress < 0.95 ? root.item.positionMs : duration * 0.2
                                    root.seekTarget = Math.max(1, Math.round(start))
                                    position = root.seekTarget
                                    play()
                                } else if (mediaStatus === MediaPlayer.EndOfMedia) {
                                    root.videoShown = false
                                }
                            }
                            onPositionChanged: (pos) => { if (!root.videoShown && root.seekTarget > 0 && playbackState === MediaPlayer.PlayingState
                                                   && pos > root.seekTarget + 250) root.videoShown = true }
                        }
                        VideoOutput {
                            id: vout
                            anchors.fill: parent
                            fillMode: VideoOutput.PreserveAspectCrop
                            opacity: root.videoShown ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 500 } }
                        }
                    }
                }
                Rectangle {
                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                    height: parent.height * 0.55
                    gradient: Gradient {
                        GradientStop { position: 0; color: "transparent" }
                        GradientStop { position: 1; color: Qt.rgba(0, 0, 0, 0.7) }
                    }
                }
            }
            Item {
                id: mediaMask
                anchors.fill: parent
                visible: false
                layer.enabled: true
                clip: true
                Rectangle { width: parent.width; height: parent.height + 12; radius: 6 }
            }
            Image {
                visible: !!(root.item && root.item.hasMeta)
                x: 12; y: 12
                height: Math.round(root.boxW * 0.055); width: height
                source: "qrc:/assets/mark.svg"
                sourceSize: Qt.size(width, height)
            }
            // title logo when metadata provides one, else the title text
            Image {
                id: miniLogo
                readonly property bool has: root.hasItem && !!root.item.logoImage && root.item.logoImage.toString() !== ""
                readonly property bool use: has && status === Image.Ready   // falls back to the title text otherwise
                visible: use
                anchors.left: parent.left; anchors.leftMargin: 16
                anchors.bottom: parent.bottom; anchors.bottomMargin: 14
                source: has ? root.item.logoImage : ""
                asynchronous: true; cache: true
                fillMode: Image.PreserveAspectFit
                readonly property real ar: status === Image.Ready && implicitHeight > 0 ? implicitWidth / implicitHeight : 3
                height: Math.round(Math.min(root.imgH * 0.32, root.boxW * 0.5 / ar))
                width: Math.round(height * ar)
                mipmap: true
            }
            Text {
                visible: !miniLogo.use
                anchors.left: parent.left; anchors.leftMargin: 16
                anchors.right: muteBtn.left; anchors.rightMargin: 12
                anchors.bottom: parent.bottom; anchors.bottomMargin: 14
                text: root.hasItem ? root.item.title : ""
                color: "white"
                font.family: Theme.font
                font.pixelSize: Math.round(Math.max(16, root.boxW * 0.06))
                font.weight: Font.ExtraBold
                font.letterSpacing: -0.4
                lineHeight: 0.95
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                style: Text.Raised
                styleColor: Qt.rgba(0, 0, 0, 0.4)
            }
            CircleButton {
                id: muteBtn
                anchors.right: parent.right; anchors.rightMargin: 14
                anchors.bottom: parent.bottom; anchors.bottomMargin: 14
                size: info.btn
                borderWidth: 2
                glass: false
                iconName: root.muted ? "volumeOff" : "volumeOn"
                iconScale: 0.5
                visible: opacity > 0
                opacity: root.videoShown ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 300 } }
                onClicked: root.muted = !root.muted
            }
        }

        // ---- info (padding 16) ----
        Column {
            id: info
            y: root.imgH
            width: parent.width
            padding: 16
            spacing: Math.round(info.btn * 0.4)
            opacity: Math.max(0, (root.p - 0.3) / 0.7)
            readonly property real btn: Math.round(Math.max(32, Math.min(46, root.boxW * 0.089)))
            readonly property real fs: Math.round(Math.max(12, Math.min(18, root.boxW * 0.04)))

            // action buttons (32px, 2px rgba(255,255,255,.5) borders -> white on hover)
            Item {
                width: parent.width - 32
                height: info.btn
                Row {
                    spacing: Math.round(info.btn * 0.25)
                    CircleButton {
                        objectName: "previewPlay"
                        size: info.btn; filled: true; iconName: "play"; iconScale: 0.5
                        tooltip: "Play"
                        onClicked: if (root.hasItem) Nav.play(root.item.path)
                    }
                    CircleButton {
                        objectName: "previewMyList"
                        size: info.btn; iconName: root.hasItem && root.item.inMyList ? "check" : "plus"; iconScale: 0.5
                        tooltip: root.hasItem && root.item.inMyList ? "Remove from My List" : "Add to My List"
                        onClicked: if (root.hasItem) { Library.toggleMyList(root.item.id); root.refresh() }
                    }
                    CircleButton {
                        id: likeBtn
                        property bool liked: false
                        size: info.btn; iconName: "thumbsUp"; iconScale: 0.5
                        tooltip: liked ? "Rated" : "I like this"
                        borderColor: liked ? "white" : Qt.rgba(1, 1, 1, 0.5)
                        onClicked: liked = !liked
                    }
                }
                CircleButton {
                    objectName: "previewMore"
                    anchors.right: parent.right
                    size: info.btn; iconName: "chevronDown"; iconScale: 0.5
                    tooltip: root.hasItem && root.item.isSeries ? "Episodes & info" : "More info"
                    onClicked: if (root.hasItem) Nav.openDetail(root.item.id)
                }
            }

            // continue-watching progress
            Row {
                visible: root.hasItem && root.item.progress > 0.005 && root.item.progress < 0.97
                spacing: 10
                Item {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.round(root.boxW * 0.42); height: 2
                    Rectangle { anchors.fill: parent; color: "#4D4D4D" }
                    Rectangle { height: 2; width: parent.width * (root.hasItem ? root.item.progress : 0); color: Theme.red }
                }
                Text {
                    text: root.hasItem ? Math.round(root.item.positionMs / 60000) + " of " + Math.max(1, Math.round(root.item.durationMs / 60000)) + "m" : ""
                    color: "#D2D2D2"; font.family: Theme.font; font.pixelSize: info.fs - 1
                }
            }

            // match · rating · length · HD
            Row {
                spacing: 8
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.hasItem ? (root.item.match || 95) + "% Match" : ""
                    color: Theme.green
                    font.family: Theme.font; font.pixelSize: info.fs; font.weight: Font.Bold
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.hasItem && !!root.item.rating
                    width: rText.implicitWidth + 10; height: rText.implicitHeight + 2
                    color: "transparent"; border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.4)
                    Text { id: rText; anchors.centerIn: parent; text: root.hasItem ? (root.item.rating || "") : ""; color: "white"; font.family: Theme.font; font.pixelSize: info.fs - 2 }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Theme.lengthLabel(root.item)
                    color: "white"
                    font.family: Theme.font; font.pixelSize: info.fs
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.hasItem && !!root.item.quality
                    width: qText.implicitWidth + 10; height: qText.implicitHeight + 1
                    radius: 3
                    color: "transparent"; border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.4)
                    Text { id: qText; anchors.centerIn: parent; text: root.hasItem ? (root.item.quality || "") : ""; color: "white"; font.family: Theme.font; font.pixelSize: info.fs - 4; font.weight: Font.DemiBold }
                }
            }

            // genres separated by 4px #646464 dots
            Row {
                width: parent.width - 32
                height: genreFirst.implicitHeight
                spacing: 0
                clip: true
                Repeater {
                    model: root.hasItem && root.item.genres ? root.item.genres : []
                    delegate: Row {
                        id: gr
                        required property string modelData
                        required property int index
                        spacing: 0
                        Item {
                            visible: gr.index > 0
                            width: 14; height: genreFirst.implicitHeight
                            Rectangle { anchors.centerIn: parent; width: 4; height: 4; radius: 2; color: "#646464" }
                        }
                        Text {
                            text: gr.modelData
                            color: "white"
                            font.family: Theme.font; font.pixelSize: info.fs
                        }
                    }
                }
                Text { id: genreFirst; visible: false; text: "Ag"; font.family: Theme.font; font.pixelSize: info.fs }
            }
        }
    }
}
