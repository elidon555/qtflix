pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects
import QtMultimedia
import QtFlix

// Netflix hero billboard (56.25vw tall). Backdrop still, then after 2 s a muted video preview that plays
// ~25 s and fades back to the still. 3.5 s after the video starts the title logo/text shrinks to 60% (origin
// bottom-left) while the synopsis fades out (1.3 s, cubic), and the mute button + maturity box slide in from
// the right edge, exactly like Netflix.
//  - active:   playback allowed (page visible, nothing covering it, billboard mostly on screen)
//  - loaded:   keep the media pipeline alive; false (scrolled > 1 billboard height away, modal/player/other
//              page shown) destroys the MediaPlayer + VideoOutput (Loader) and unloads the source
Item {
    id: root
    property var title: ({})                 // title map (Library.featured / TitleModel.get())
    readonly property TitleInfo titleInfo: TitleInfo {}   // typed copy of `title`
    property bool active: true
    property bool loaded: active
    property bool muted: Theme.previewMuted
    property real pad: Theme.gutter
    signal playClicked()
    signal moreInfoClicked()

    readonly property bool hasTitle: titleInfo.valid
    readonly property bool hasLogo: hasTitle && titleInfo.hasLogo && logo.status !== Image.Error
    width: parent ? parent.width : 1600
    height: Math.round(width * 0.5625)
    readonly property real vw: width / 100

    // ---------------- media state ----------------
    property bool mediaLoaded: false        // Loader active
    property bool videoShown: false         // first frames are on screen
    property bool videoEnded: false         // preview finished (shows replay)
    property bool collapsed: false          // logo shrunk + synopsis hidden
    property real seekTarget: 0
    readonly property bool canLoad: loaded && visible && Theme.autoplayPreviews && hasTitle
                                    && titleInfo.hasSource && !videoEnded
    readonly property MediaPlayer player: mediaLoader.item ? loadedPlayer : null
    property MediaPlayer loadedPlayer: null  // set by the loaded MediaPlayer (Loader.item is an anonymous type)

    onTitleChanged: { titleInfo.assign(title); videoEnded = false; unload() }
    Component.onCompleted: titleInfo.assign(title)
    onCanLoadChanged: if (!canLoad) unload()
    onActiveChanged: {
        if (!player) return
        if (active && player.playbackState === MediaPlayer.PausedState) player.play()
        else if (!active && player.playbackState === MediaPlayer.PlayingState) player.pause()
    }

    function unload() {
        startTimer.stop(); stopTimer.stop(); collapseTimer.stop(); fadeStop.stop()
        mediaLoaded = false
        videoShown = false
        collapsed = false
    }
    function endVideo() {
        videoShown = false
        collapsed = false
        videoEnded = true
        collapseTimer.stop(); stopTimer.stop()
        fadeStop.restart()
    }
    function replay() {
        fadeStop.stop()
        mediaLoaded = false
        videoEnded = false
        seekTarget = 0
        mediaLoaded = true
    }

    Timer { id: startTimer; interval: 2000; running: root.canLoad && root.active && !root.mediaLoaded
            onTriggered: { root.seekTarget = 0; root.mediaLoaded = true } }
    Timer { id: stopTimer; interval: 25000; onTriggered: root.endVideo() }
    Timer { id: collapseTimer; interval: 3500; onTriggered: root.collapsed = true }
    Timer { id: fadeStop; interval: 900; onTriggered: root.mediaLoaded = false }

    // ---------------- visuals ----------------
    Rectangle { anchors.fill: parent; color: Theme.bg }

    RoundedImage {
        id: still
        anchors.fill: parent
        radius: 0
        bg: "transparent"
        fadeDuration: 600
        source: root.hasTitle ? root.titleInfo.backdropImage : ""
    }

    Loader {
        id: mediaLoader
        anchors.fill: parent
        active: root.mediaLoaded && (root.canLoad || fadeStop.running)
        sourceComponent: Item {
            MediaPlayer {
                id: mp
                Component.onCompleted: root.loadedPlayer = mp
                source: root.titleInfo.sourceUrl
                videoOutput: vout
                audioOutput: AudioOutput { muted: root.muted; volume: 0.7 }
                onMediaStatusChanged: {
                    if (mediaStatus === MediaPlayer.LoadedMedia || mediaStatus === MediaPlayer.BufferedMedia) {
                        if (root.seekTarget === 0 && duration > 0) {
                            root.seekTarget = Math.max(1, Math.round(duration * 0.2))
                            position = root.seekTarget
                            if (root.active) play(); else pause()
                        }
                    } else if (mediaStatus === MediaPlayer.EndOfMedia) {
                        root.endVideo()
                    } else if (mediaStatus === MediaPlayer.InvalidMedia) {
                        root.videoEnded = true
                    }
                }
                onPositionChanged: (pos) => {
                    if (!root.videoShown && !root.videoEnded && root.seekTarget > 0 && playbackState === MediaPlayer.PlayingState
                            && pos > root.seekTarget + 250) {
                        root.videoShown = true
                        stopTimer.restart()
                        collapseTimer.restart()
                    }
                }
                onErrorOccurred: (error, errorString) => { root.videoEnded = true }
            }
            VideoOutput {
                id: vout
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectCrop
                opacity: root.videoShown ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 900; easing.type: Easing.InOutQuad } }
            }
        }
    }

    // left "trailer vignette": linear-gradient(77deg, rgba(0,0,0,.6), transparent 85%)
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.6) }
            GradientStop { position: 0.3; color: Qt.rgba(0, 0, 0, 0.35) }
            GradientStop { position: 0.6; color: Qt.rgba(0, 0, 0, 0.08) }
            GradientStop { position: 0.85; color: "transparent" }
        }
    }
    // top shade under the nav
    Rectangle {
        width: parent.width; height: Theme.navH * 2
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.rgba(0, 0, 0, 0.25) }
            GradientStop { position: 1; color: "transparent" }
        }
    }
    // bottom "hero vignette" (Netflix stops) fading into #141414. Taller than Netflix's 14.7vw so the first
    // row's cards (which start ~20% above the bottom) sit on an almost solid dark area with no visible edge.
    Rectangle {
        anchors.left: parent.left; anchors.right: parent.right
        anchors.bottom: parent.bottom; anchors.bottomMargin: -1
        height: Math.round(parent.height * 0.42) + 1
        gradient: Gradient {
            GradientStop { position: 0.00; color: Qt.rgba(20/255, 20/255, 20/255, 0) }
            GradientStop { position: 0.15; color: Qt.rgba(20/255, 20/255, 20/255, 0.15) }
            GradientStop { position: 0.29; color: Qt.rgba(20/255, 20/255, 20/255, 0.35) }
            GradientStop { position: 0.44; color: Qt.rgba(20/255, 20/255, 20/255, 0.58) }
            GradientStop { position: 0.60; color: Qt.rgba(20/255, 20/255, 20/255, 0.85) }
            GradientStop { position: 0.70; color: Theme.bg }
            GradientStop { position: 1.00; color: Theme.bg }
        }
    }

    // ---------------- info block ----------------
    // cp: 0 = expanded, 1 = collapsed (Netflix: transform scale(.6) from bottom-left + synopsis fade, 1.3 s)
    property real cp: collapsed ? 1 : 0
    Behavior on cp { NumberAnimation { duration: 1300; easing.type: Easing.InOutCubic } }

    Column {
        id: info
        x: root.pad
        width: Math.round(root.width * 0.36)
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.round(root.height * 0.35)
        spacing: 0
        visible: root.hasTitle

        // tag + logo/title: shrinks to 60% around its bottom-left corner and drops into the synopsis' space
        Item {
            id: titleGroup
            width: parent.width
            height: titleCol.height
            transform: [
                Scale { origin.x: 0; origin.y: titleGroup.height; xScale: 1 - 0.4 * root.cp; yScale: 1 - 0.4 * root.cp },
                Translate { y: synopsisBlock.height * root.cp }
            ]
            Column {
                id: titleCol
                width: parent.width
                spacing: Math.round(root.vw * 0.5)

                // "N SERIES" / "N FILM" — only with real metadata
                Row {
                    visible: root.hasTitle && root.titleInfo.hasMeta
                    spacing: Math.round(root.vw * 0.35)
                    height: Math.round(Math.max(20, root.vw * 1.7))
                    Image {
                        height: Math.round(parent.height * 0.85); width: height
                        anchors.verticalCenter: parent.verticalCenter
                        source: "qrc:/assets/mark.svg"
                        sourceSize: Qt.size(width, height)
                        fillMode: Image.PreserveAspectFit
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: 1
                        text: root.titleInfo.isSeries ? "S E R I E S" : "F I L M"
                        color: "#E5E5E5"
                        font.family: Theme.font
                        font.pixelSize: Math.round(Math.max(11, root.vw * 0.95))
                        font.weight: Font.DemiBold
                        font.letterSpacing: Math.max(1, root.vw * 0.12)
                        style: Text.Raised; styleColor: Qt.rgba(0, 0, 0, 0.4)
                    }
                }

                // title logo (when metadata provides one)
                Image {
                    id: logo
                    visible: root.hasLogo
                    source: root.hasTitle && root.titleInfo.hasLogo ? root.titleInfo.logoImage : ""
                    asynchronous: true
                    cache: true
                    fillMode: Image.PreserveAspectFit
                    readonly property real maxH: Math.round(Math.min(root.vw * 11.25, 300))   // 180px at 1600
                    readonly property real maxW: Math.round(root.width * 0.4)
                    readonly property real ar: status === Image.Ready && implicitHeight > 0 ? implicitWidth / implicitHeight : 3
                    height: visible ? Math.round(Math.min(maxH, maxW / ar)) : 0
                    width: Math.round(height * ar)
                    smooth: true
                    mipmap: true
                }

                // title text (fallback)
                Text {
                    id: titleText
                    visible: !root.hasLogo
                    width: parent.width
                    text: root.titleInfo.title
                    font.pixelSize: Math.round(text.length > 24 ? root.vw * 2.6 : root.vw * 3.5)
                    font.family: Theme.font
                    font.weight: Font.ExtraBold
                    font.letterSpacing: -1
                    lineHeight: 0.95
                    color: "white"
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    layer.enabled: true
                    layer.effect: MultiEffect { shadowEnabled: true; shadowColor: Qt.rgba(0, 0, 0, 0.45); shadowBlur: 0.5; shadowHorizontalOffset: 2; shadowVerticalOffset: 2 }
                }
            }
        }

        // synopsis: stays in layout (Netflix keeps its box), fades out while the title drops into it
        Item {
            id: synopsisBlock
            width: parent.width
            height: synopsis.text !== "" ? synopsis.implicitHeight + Math.round(root.vw * 1.2) : 0
            opacity: 1 - root.cp
            Text {
                id: synopsis
                y: Math.round(root.vw * 1.2)
                width: parent.width
                text: root.titleInfo.description
                color: "white"
                font.family: Theme.font
                font.pixelSize: Math.round(Math.max(13, root.vw * 1.2))
                lineHeight: 1.12
                wrapMode: Text.WordWrap
                maximumLineCount: 3
                elide: Text.ElideRight
                style: Text.Raised; styleColor: Qt.rgba(0, 0, 0, 0.35)
            }
        }

        Item { width: 1; height: Math.round(root.vw * 1.5) }
        Row {
            spacing: Math.round(Math.max(8, root.vw * 0.7))
            NfButton {
                objectName: "billboardPlay"
                text: "Play"; iconName: "play"; primary: true
                fontSize: Math.round(Math.max(14, root.vw * 1.2))
                onClicked: { root.playClicked(); Nav.play(root.titleInfo.path) }
            }
            NfButton {
                objectName: "billboardMoreInfo"
                text: "More Info"; iconName: "info"; primary: false
                fontSize: Math.round(Math.max(14, root.vw * 1.2))
                onClicked: { root.moreInfoClicked(); Nav.openDetail(root.titleInfo.titleId) }
            }
        }
    }

    // ---------------- bottom-right: mute / replay + maturity rating (slide in from the right) ----------------
    readonly property bool sideShown: hasTitle && (videoShown || videoEnded)
    Row {
        id: side
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.round(root.height * 0.35)
        spacing: Math.round(root.vw * 0.8)
        visible: root.hasTitle && slide.x < width
        transform: Translate {
            id: slide
            x: root.sideShown ? 0 : side.width + 2
            Behavior on x { NumberAnimation { duration: 600; easing.type: Easing.OutCubic } }
        }
        Behavior on opacity { NumberAnimation { duration: 400 } }
        opacity: root.sideShown ? 1 : 0
        CircleButton {
            anchors.verticalCenter: parent.verticalCenter
            size: Math.round(Math.max(32, root.vw * 2.6))
            iconName: root.videoEnded ? "replay" : (root.muted ? "volumeOff" : "volumeOn")
            iconScale: 0.5
            borderWidth: 1
            borderColor: Qt.rgba(1, 1, 1, 0.7)
            glass: false
            onClicked: root.videoEnded ? root.replay() : (root.muted = !root.muted)
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            readonly property real fs: Math.round(Math.max(12, root.vw * 1.1))
            height: Math.round(fs * 2.2)
            width: ratingText.implicitWidth + Math.round(root.vw * 0.8) + Math.round(root.vw * 3.5) + 3
            color: Qt.rgba(51/255, 51/255, 51/255, 0.6)
            Rectangle { width: 3; height: parent.height; color: "#DCDCDC" }
            Text {
                id: ratingText
                x: 3 + Math.round(root.vw * 0.8)
                anchors.verticalCenter: parent.verticalCenter
                text: root.titleInfo.rating
                color: "white"
                font.family: Theme.font
                font.pixelSize: parent.fs
            }
        }
    }
}
