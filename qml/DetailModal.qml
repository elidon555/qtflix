pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import QtMultimedia
import QtFlix

// Netflix "More Info" modal. Works both always-instantiated (shows while Nav.detailId !== "") and inside a
// Loader (active: Nav.detailId !== ""). Escape / click outside / × close it.
Item {
    id: root
    objectName: "detailModal"
    anchors.fill: parent
    z: 300
    visible: Nav.detailId !== "" || closing
    focus: visible

    readonly property string titleId: Nav.detailId
    readonly property TitleInfo t: TitleInfo {}      // the shown title (Library.title snapshot)
    readonly property bool hasTitle: t.valid
    property var seasonList: []
    property int season: 0
    property var episodes: []
    property var fileInfo: ({})
    property var similar: []
    property bool muted: Theme.previewMuted
    property bool closing: false
    property real anim: 0                     // 0 closed .. 1 open

    // 850px (Netflix) up to ~1600 wide windows, then grows with the window (53vw); never wider than 90%
    readonly property real boxW: Math.min(Math.round(width * 0.9), Math.max(850, Math.round(width * 0.53)))
    readonly property real mediaH: Math.round(boxW * 9 / 16)
    readonly property real s: boxW / 850      // scale factor for type/padding
    readonly property bool hasLogo: hasTitle && t.hasLogo && modalLogo.status !== Image.Error
    function seasonName(n: int): string { return n === 0 ? "Specials" : "Season " + n }

    function load() {
        if (titleId === "") return
        t.assign(Library.title(titleId))
        if (!hasTitle) return
        fileInfo = Library.fileInfo(t.path) || ({})
        if (t.isSeries) {
            seasonList = Library.seasons(t.titleId) || []
            const want = fileInfo && fileInfo.season ? fileInfo.season : (seasonList.length ? seasonList[0] : 1)
            season = want
            loadEpisodes()
        } else {
            seasonList = []; episodes = []
        }
        // "More Like This": other titles, same kind first
        const out = [], others = []
        const all = Library.allTitles
        for (let i = 0; all && i < all.count && out.length < 9; ++i) {
            const o = all.get(i)
            if (!o || o.id === t.titleId) continue
            if (o.isSeries === t.isSeries) out.push(o); else others.push(o)
        }
        for (let j = 0; out.length < 9 && j < others.length; ++j) out.push(others[j])
        similar = out
    }
    function refreshTitle() { if (hasTitle) { const m = Library.title(t.titleId); if (m && m.id) t.assign(m) } }
    // the shown season's episodes; also asks the backend to generate that season's episode stills first
    function loadEpisodes() {
        episodes = Library.episodes(t.titleId, season) || []
        Library.warmSeason(t.titleId, season)
    }
    function close() {
        if (closing) return
        closing = true
        closeAnim.restart()
    }

    onTitleIdChanged: {
        if (titleId === "") { if (!closing) { anim = 0; stopMedia() } return }
        const wasOpen = anim > 0 && !closing
        closing = false
        closeAnim.stop()
        stopMedia()
        load()
        scroller.scrollTo(0, false)
        if (!wasOpen) openAnim.restart()
        videoTimer.restart()
        root.forceActiveFocus()
    }
    Component.onCompleted: if (titleId !== "") { load(); openAnim.restart(); videoTimer.restart(); root.forceActiveFocus() }
    onSeasonChanged: if (hasTitle && t.isSeries) loadEpisodes()

    Connections {
        target: Library
        function onMyListChanged(id: string) { if (root.hasTitle && id === root.t.titleId) root.refreshTitle() }
        // only this title's files matter (the player saves progress every few seconds)
        function onProgressChanged(path: string) {
            if (!root.hasTitle || Library.titleIdForPath(path) !== root.t.titleId) return
            root.refreshTitle()
            if (root.t.isSeries) root.episodes = Library.episodes(root.t.titleId, root.season) || []
        }
        function onLibraryChanged() { if (root.titleId !== "") root.load() }
    }

    Keys.onEscapePressed: root.close()

    NumberAnimation { id: openAnim; target: root; property: "anim"; from: 0; to: 1; duration: 200; easing.type: Easing.OutCubic }
    SequentialAnimation {
        id: closeAnim
        NumberAnimation { target: root; property: "anim"; to: 0; duration: 160; easing.type: Easing.InCubic }
        ScriptAction { script: { root.stopMedia(); root.closing = false; Nav.closeDetail() } }
    }

    // ---- video (MediaPlayer + VideoOutput live in a Loader inside the media header) ----
    property bool videoWanted: false
    property bool videoShown: false
    property real seekTarget: 0
    function stopMedia() { videoTimer.stop(); videoShown = false; videoWanted = false }
    Timer {
        id: videoTimer
        interval: 1000
        onTriggered: if (root.hasTitle && Theme.autoplayPreviews && root.t.hasSource) { root.seekTarget = 0; root.videoWanted = true }
    }

    // ---- dim overlay ----
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.7)
        opacity: root.anim
    }

    ScrollArea {
        id: scroller
        anchors.fill: parent
        contentHeight: box.height + 64
        focus: true
        Keys.onEscapePressed: root.close()

        // click outside closes
        MouseArea {
            width: scroller.width
            height: Math.max(scroller.height, scroller.contentHeight)
            onClicked: root.close()
        }

        Item {
            id: box
            x: Math.round((scroller.width - root.boxW) / 2)
            y: 32
            width: root.boxW
            height: boxCol.height
            opacity: root.anim
            scale: 0.9 + 0.1 * root.anim
            transformOrigin: Item.Top

            RectangularShadow {
                anchors.fill: parent
                radius: 6
                blur: 24
                offset.y: 4
                color: Qt.rgba(0, 0, 0, 0.75)
            }
            Rectangle { anchors.fill: parent; radius: 6; color: Theme.bgElevated }
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onClicked: (ev) => ev.accepted = true }

            Column {
                id: boxCol
                width: parent.width

                // ================= media header =================
                Item {
                    id: media
                    width: parent.width
                    height: root.mediaH

                    // the still is a RoundedImage (no offscreen pass); only while the video is loaded does the
                    // header go through a mask layer for its rounded top corners
                    readonly property bool masked: videoLoader.active
                    Item {
                        id: mediaContent
                        anchors.fill: parent
                        layer.enabled: media.masked
                        layer.effect: MultiEffect {
                            maskEnabled: true
                            maskSource: mediaMask
                            maskThresholdMin: 0.5
                            maskSpreadAtMin: 1.0
                        }
                        RoundedImage {
                            anchors.fill: parent
                            topLeftRadius: 6; topRightRadius: 6; bottomLeftRadius: 0; bottomRightRadius: 0
                            fadeDuration: 0
                            source: root.hasTitle ? root.t.backdropImage : ""
                        }
                        Loader {
                            id: videoLoader
                            anchors.fill: parent
                            active: root.videoWanted && root.hasTitle && !root.closing
                            sourceComponent: Item {
                                MediaPlayer {
                                    id: player
                                    source: root.t.sourceUrl
                                    videoOutput: vout
                                    audioOutput: AudioOutput { muted: root.muted; volume: 0.7 }
                                    onMediaStatusChanged: {
                                        if ((mediaStatus === MediaPlayer.LoadedMedia || mediaStatus === MediaPlayer.BufferedMedia)
                                                && root.seekTarget === 0 && duration > 0) {
                                            root.seekTarget = Math.max(1, Math.round(duration * 0.2))
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
                                    Behavior on opacity { NumberAnimation { duration: 700 } }
                                }
                            }
                        }
                        // linear-gradient(0deg, #181818, transparent 50%)
                        Rectangle {
                            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                            anchors.bottomMargin: -1
                            height: parent.height * 0.5 + 1
                            gradient: Gradient {
                                GradientStop { position: 0; color: "transparent" }
                                GradientStop { position: 1; color: Theme.bgElevated }
                            }
                        }
                        Rectangle {
                            width: parent.width * 0.6; height: parent.height
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0; color: Qt.rgba(0, 0, 0, 0.35) }
                                GradientStop { position: 1; color: "transparent" }
                            }
                        }
                    }
                    Item {
                        id: mediaMask
                        anchors.fill: parent
                        visible: false
                        layer.enabled: media.masked
                        clip: true
                        Rectangle { width: parent.width; height: parent.height + 10; radius: 6 }
                    }

                    // close ×
                    Item {
                        anchors.right: parent.right; anchors.top: parent.top
                        anchors.margins: 16
                        width: Math.round(36 * root.s); height: width
                        Rectangle { anchors.fill: parent; radius: width / 2; color: "#181818"; border.width: cma.containsMouse ? 1 : 0; border.color: "white" }
                        Icon { anchors.centerIn: parent; name: "close"; size: Math.round(18 * root.s); color: "white"; strokeWidth: 2.2 }
                        MouseArea { id: cma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.close() }
                    }

                    // title + actions
                    Column {
                        x: Math.round(48 * root.s)
                        width: parent.width * 0.62
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: Math.round(root.mediaH * 0.09)
                        spacing: Math.round(22 * root.s)
                        Column {
                            width: parent.width
                            spacing: 6
                            Row {
                                visible: root.hasTitle && root.t.hasMeta === true
                                spacing: 6
                                Image {
                                    height: Math.round(18 * root.s); width: height
                                    anchors.verticalCenter: parent.verticalCenter
                                    source: "qrc:/assets/mark.svg"
                                    sourceSize: Qt.size(width, height)
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: root.hasTitle && root.t.isSeries ? "S E R I E S" : "F I L M"
                                    color: "#E5E5E5"
                                    font.family: Theme.font; font.pixelSize: Math.round(12 * root.s); font.weight: Font.DemiBold
                                    font.letterSpacing: 1
                                }
                            }
                            Image {
                                id: modalLogo
                                visible: root.hasLogo
                                source: root.hasTitle && root.t.hasLogo ? root.t.logoImage : ""
                                asynchronous: true; cache: true
                                fillMode: Image.PreserveAspectFit
                                mipmap: true
                                readonly property real ar: status === Image.Ready && implicitHeight > 0 ? implicitWidth / implicitHeight : 3
                                height: visible ? Math.round(Math.min(root.mediaH * 0.3, root.boxW * 0.4 / ar)) : 0
                                width: Math.round(height * ar)
                            }
                            Text {
                                visible: !root.hasLogo
                                width: parent.width
                                text: root.hasTitle ? root.t.title : ""
                                color: "white"
                                font.family: Theme.font
                                font.pixelSize: Math.round((text.length > 22 ? 38 : 48) * root.s)
                                font.weight: Font.ExtraBold
                                font.letterSpacing: -1
                                lineHeight: 0.95
                                wrapMode: Text.WordWrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                layer.enabled: true
                                layer.effect: MultiEffect { shadowEnabled: true; shadowColor: Qt.rgba(0, 0, 0, 0.5); shadowBlur: 0.5; shadowHorizontalOffset: 2; shadowVerticalOffset: 2 }
                            }
                        }
                        Row {
                            spacing: 10
                            NfButton {
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.hasTitle && root.t.progress > 0.01 && root.t.progress < 0.95 ? "Resume" : "Play"
                                iconName: "play"; primary: true
                                fontSize: Math.round(18 * root.s)
                                width: Math.max(implicitWidth, Math.round(130 * root.s))
                                onClicked: if (root.hasTitle) Nav.play(root.t.path)
                            }
                            CircleButton {
                                anchors.verticalCenter: parent.verticalCenter
                                size: Math.round(42 * root.s)
                                iconName: root.hasTitle && root.t.inMyList ? "check" : "plus"
                                tooltip: root.hasTitle && root.t.inMyList ? "Remove from My List" : "Add to My List"
                                onClicked: if (root.hasTitle) { Library.toggleMyList(root.t.titleId); root.refreshTitle() }
                            }
                            CircleButton {
                                id: like
                                property bool liked: false
                                anchors.verticalCenter: parent.verticalCenter
                                size: Math.round(42 * root.s)
                                iconName: "thumbsUp"
                                tooltip: liked ? "Rated" : "I like this"
                                borderColor: liked ? "white" : Qt.rgba(1, 1, 1, 0.5)
                                onClicked: liked = !liked
                            }
                        }
                    }
                    CircleButton {
                        anchors.right: parent.right; anchors.rightMargin: Math.round(48 * root.s)
                        anchors.bottom: parent.bottom; anchors.bottomMargin: Math.round(root.mediaH * 0.09) + 3
                        size: Math.round(40 * root.s)
                        borderWidth: 1; borderColor: Qt.rgba(1, 1, 1, 0.7); glass: false
                        iconName: root.muted ? "volumeOff" : "volumeOn"
                        visible: root.videoShown
                        onClicked: root.muted = !root.muted
                    }
                }

                // ================= details =================
                Item {
                    id: details
                    width: parent.width
                    height: detailsRow.height + 8
                    readonly property real padX: Math.round(48 * root.s)
                    Row {
                        id: detailsRow
                        x: details.padX
                        width: parent.width - 2 * details.padX
                        spacing: Math.round(32 * root.s)
                        // left column (2fr)
                        Column {
                            width: (detailsRow.width - detailsRow.spacing) * 2 / 3
                            spacing: 10
                            Row {
                                spacing: 8
                                Text {
                                    text: root.hasTitle ? (root.t.match || 95) + "% Match" : ""
                                    color: Theme.green; font.family: Theme.font; font.pixelSize: Math.round(16 * root.s); font.weight: Font.Bold
                                }
                                Text {
                                    visible: root.hasTitle && root.t.year > 0
                                    text: root.hasTitle ? root.t.year : ""
                                    color: "#BCBCBC"; font.family: Theme.font; font.pixelSize: Math.round(16 * root.s)
                                }
                                Text {
                                    text: Theme.lengthLabel(root.t.isSeries, root.t.seasonCount, root.t.episodeCount, root.t.durationMs)
                                    color: "#BCBCBC"; font.family: Theme.font; font.pixelSize: Math.round(16 * root.s)
                                }
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: root.hasTitle && !!root.t.quality
                                    width: hdText.implicitWidth + 10; height: hdText.implicitHeight + 1
                                    radius: 3; color: "transparent"; border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.4)
                                    Text { id: hdText; anchors.centerIn: parent; text: root.hasTitle ? (root.t.quality || "") : ""; color: "white"; font.family: Theme.font; font.pixelSize: Math.round(11 * root.s); font.weight: Font.DemiBold }
                                }
                            }
                            Row {
                                spacing: 8
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: mrText.implicitWidth + 12; height: mrText.implicitHeight + 2
                                    color: "transparent"; border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.4)
                                    Text { id: mrText; anchors.centerIn: parent; text: root.hasTitle ? (root.t.rating || "") : ""; color: "white"; font.family: Theme.font; font.pixelSize: Math.round(13 * root.s) }
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: root.advisory(root.t.rating)
                                    color: "white"; font.family: Theme.font; font.pixelSize: Math.round(14 * root.s)
                                }
                            }
                            // resume progress
                            Row {
                                visible: root.hasTitle && root.t.progress > 0.005 && root.t.progress < 0.97
                                spacing: 12
                                topPadding: 6
                                Text {
                                    visible: !!root.fileInfo && root.fileInfo.isSeries === true
                                    text: root.fileInfo && root.fileInfo.isSeries ? "S" + root.fileInfo.season + ":E" + root.fileInfo.episode
                                                                                   + (root.fileInfo.episodeTitle ? " “" + root.fileInfo.episodeTitle + "”" : "") : ""
                                    color: "white"; font.family: Theme.font; font.pixelSize: Math.round(14 * root.s); font.weight: Font.Bold
                                }
                                Item {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.round(140 * root.s); height: 2
                                    Rectangle { anchors.fill: parent; color: "#4D4D4D" }
                                    Rectangle { height: 2; width: parent.width * (root.hasTitle ? root.t.progress : 0); color: Theme.red }
                                }
                                Text {
                                    text: root.hasTitle ? Math.round(root.t.positionMs / 60000) + " of " + Math.max(1, Math.round(root.t.durationMs / 60000)) + "m" : ""
                                    color: "#D2D2D2"; font.family: Theme.font; font.pixelSize: Math.round(14 * root.s)
                                }
                            }
                            Text {
                                width: parent.width
                                topPadding: 8
                                text: {
                                    if (!root.hasTitle) return ""
                                    // The first line is a metadata summary already shown in the match row above.
                                    if (root.t.hasMeta) return root.t.description || ""
                                    const lines = (root.t.description || "").split("\n")
                                    return lines.length > 1 ? lines.slice(1).join("\n") : lines[0]
                                }
                                color: "white"; font.family: Theme.font; font.pixelSize: Math.round(16 * root.s)
                                lineHeight: 1.4
                                wrapMode: Text.WordWrap
                            }
                        }
                        // right column (1fr)
                        Column {
                            width: (detailsRow.width - detailsRow.spacing) / 3
                            spacing: 14
                            topPadding: 2
                            TagLine { label: "Genres:"; value: root.t.genreText }
                            TagLine {
                                label: root.hasTitle && root.t.isSeries ? "This show is:" : "This movie is:"
                                value: root.t.mainGenres
                            }
                            TagLine { label: "File:"; value: root.hasTitle ? (root.t.category || "") : "" }
                        }
                    }
                }

                // ================= episodes =================
                Column {
                    id: epSection
                    visible: root.hasTitle && root.t.isSeries
                    x: Math.round(48 * root.s)
                    width: parent.width - 2 * x
                    topPadding: 32
                    spacing: 0
                    Item {
                        width: parent.width
                        height: Math.round(48 * root.s)
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Episodes"
                            color: "white"; font.family: Theme.font; font.pixelSize: Math.round(24 * root.s); font.weight: Font.Bold
                        }
                        // season selector
                        ComboBox {
                            id: seasonBox
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            visible: root.seasonList.length > 1
                            model: root.seasonList
                            currentIndex: Math.max(0, root.seasonList.indexOf(root.season))
                            onActivated: (i) => root.season = root.seasonList[i]
                            implicitWidth: Math.max(Math.round(140 * root.s), seasonLabel.implicitWidth + Math.round(60 * root.s))
                            implicitHeight: Math.round(44 * root.s)
                            font.family: Theme.font
                            background: Rectangle { color: "#242424"; radius: 4; border.width: 1; border.color: seasonBox.hovered ? "#8C8C8C" : "#4D4D4D" }
                            contentItem: Text {
                                id: seasonLabel
                                leftPadding: 16
                                verticalAlignment: Text.AlignVCenter
                                text: root.seasonName(root.season)
                                color: "white"; font.family: Theme.font; font.pixelSize: Math.round(18 * root.s); font.weight: Font.Bold
                            }
                            indicator: Icon {
                                x: seasonBox.width - width - 14
                                anchors.verticalCenter: parent.verticalCenter
                                name: "caretDown"; size: Math.round(16 * root.s); color: "white"
                            }
                            delegate: ItemDelegate {
                                id: sd
                                required property var modelData
                                required property int index
                                width: seasonBox.popup.width
                                height: 44
                                hoverEnabled: true
                                background: Rectangle { color: sd.hovered ? "#414141" : "transparent" }
                                contentItem: Row {
                                    spacing: 6
                                    leftPadding: 4
                                    Text { anchors.verticalCenter: parent.verticalCenter; text: root.seasonName(sd.modelData); color: "white"; font.family: Theme.font; font.pixelSize: Math.round(16 * root.s); font.weight: sd.modelData === root.season ? Font.Bold : Font.Normal }
                                    Text { anchors.verticalCenter: parent.verticalCenter; text: "(" + Library.episodeCount(root.t.titleId, sd.modelData) + " Episodes)"; color: "#B3B3B3"; font.family: Theme.font; font.pixelSize: Math.round(13 * root.s) }
                                }
                            }
                            popup: Popup {
                                y: seasonBox.height + 2
                                width: Math.max(seasonBox.width, 220)
                                x: seasonBox.width - width
                                padding: 0
                                implicitHeight: Math.min(contentItem.implicitHeight, 360)
                                background: Rectangle { color: "#242424"; border.width: 1; border.color: "#4D4D4D"; radius: 4 }
                                contentItem: ListView {
                                    clip: true
                                    implicitHeight: contentHeight
                                    model: seasonBox.delegateModel
                                    currentIndex: seasonBox.highlightedIndex
                                }
                            }
                        }
                        Text {
                            visible: root.seasonList.length <= 1
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.seasonList.length === 1 ? root.seasonName(root.season) : ""
                            color: "white"; font.family: Theme.font; font.pixelSize: Math.round(18 * root.s); font.weight: Font.Bold
                        }
                    }
                    Item { width: 1; height: 8 }
                    Row {
                        spacing: 8
                        height: 24
                        Text { anchors.verticalCenter: parent.verticalCenter; text: root.seasonName(root.season) + ":"; color: "#D2D2D2"; font.family: Theme.font; font.pixelSize: Math.round(14 * root.s) }
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: srText.implicitWidth + 10; height: srText.implicitHeight + 2
                            color: "transparent"; border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.4)
                            Text { id: srText; anchors.centerIn: parent; text: root.hasTitle ? (root.t.rating || "") : ""; color: "white"; font.family: Theme.font; font.pixelSize: Math.round(11 * root.s) }
                        }
                        Text { anchors.verticalCenter: parent.verticalCenter; text: root.advisory(root.t.rating); color: "#D2D2D2"; font.family: Theme.font; font.pixelSize: Math.round(14 * root.s) }
                    }
                    Item { width: 1; height: 20 }
                    Rectangle { width: parent.width; height: 1; color: "#404040" }
                    Repeater {
                        model: root.episodes
                        delegate: Rectangle {
                            id: ep
                            required property var modelData
                            required property int index
                            readonly property bool current: root.hasTitle && modelData.path === root.t.path
                            width: epSection.width
                            height: Math.max(epThumb.height, epMeta.implicitHeight) + 64
                            radius: 6
                            color: (ema.containsMouse || current) ? "#333333" : "transparent"
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#404040"; visible: !(ema.containsMouse || ep.current) }
                            Text {
                                id: epNum
                                x: 0
                                width: Math.round(epSection.width * 0.07)
                                anchors.verticalCenter: parent.verticalCenter
                                horizontalAlignment: Text.AlignHCenter
                                text: ep.modelData.episode
                                color: "#D2D2D2"; font.family: Theme.font; font.pixelSize: Math.round(24 * root.s)
                            }
                            Item {
                                id: epThumb
                                x: epNum.width + 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.round(Math.max(130, epSection.width * 0.18)); height: Math.round(width * 9 / 16)
                                RoundedImage {
                                    anchors.fill: parent
                                    radius: 4
                                    source: ep.modelData.still || ep.modelData.thumb || ""
                                }
                                Item {
                                    visible: (ep.modelData.progress || 0) > 0.005
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                                    height: 4
                                    Rectangle { anchors.fill: parent; color: "#4D4D4D"; bottomLeftRadius: 4; bottomRightRadius: 4; antialiasing: true }
                                    Rectangle {
                                        height: parent.height; width: parent.width * Math.min(1, ep.modelData.progress || 0); color: Theme.red
                                        bottomLeftRadius: 4; bottomRightRadius: width >= parent.width - 4 ? 4 : 0; antialiasing: true
                                    }
                                }
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: Math.round(40 * root.s); height: width; radius: width / 2
                                    color: Qt.rgba(30/255, 30/255, 20/255, 0.5)
                                    border.width: 1; border.color: "white"
                                    opacity: ema.containsMouse ? 1 : 0
                                    Behavior on opacity { NumberAnimation { duration: 150 } }
                                    Icon { anchors.centerIn: parent; anchors.horizontalCenterOffset: 1; name: "play"; size: 18; color: "white" }
                                }
                            }
                            Column {
                                id: epMeta
                                anchors.left: epThumb.right; anchors.leftMargin: 16
                                anchors.right: parent.right; anchors.rightMargin: 16
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 8
                                Item {
                                    width: parent.width
                                    height: epTitle.implicitHeight
                                    Text {
                                        id: epTitle
                                        width: parent.width - epDur.implicitWidth - 16
                                        text: ep.modelData.title || ("Episode " + ep.modelData.episode)
                                        color: "white"; font.family: Theme.font; font.pixelSize: Math.round(16 * root.s); font.weight: Font.Bold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        id: epDur
                                        anchors.right: parent.right
                                        text: Theme.duration(ep.modelData.durationMs)
                                        color: "white"; font.family: Theme.font; font.pixelSize: Math.round(16 * root.s)
                                    }
                                }
                                Text {
                                    width: parent.width
                                    text: (ep.modelData.description ? ep.modelData.description
                                                                     : (ep.modelData.season === 0 ? "Special " + ep.modelData.episode + "."
                                                                                                  : "Episode " + ep.modelData.episode + " of Season " + ep.modelData.season + "."))
                                          + ((ep.modelData.progress || 0) > 0.95 ? "  Watched." : ((ep.modelData.progress || 0) > 0.005 ? "  " + Theme.remaining(ep.modelData.positionMs, ep.modelData.durationMs) + "." : ""))
                                    color: "#D2D2D2"; font.family: Theme.font; font.pixelSize: Math.round(14 * root.s)
                                    lineHeight: 1.3
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 3
                                    elide: Text.ElideRight
                                }
                            }
                            MouseArea { id: ema; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: Nav.play(ep.modelData.path) }
                        }
                    }
                }

                // ================= more like this =================
                Column {
                    id: moreSection
                    visible: root.similar.length > 0
                    x: Math.round(48 * root.s)
                    width: parent.width - 2 * x
                    topPadding: 40
                    spacing: 20
                    Text { text: "More Like This"; color: "white"; font.family: Theme.font; font.pixelSize: Math.round(24 * root.s); font.weight: Font.Bold }
                    // A real grid. (The old hand-positioned version existed because a Grid "never laid out": its
                    // cells had no size of their own — height came from children positioned with y: — so the
                    // positioner saw 0x0 items. GridLayout + explicit preferred sizes and fillHeight fixes it and
                    // also gives Netflix's equal-height cards per row.)
                    GridLayout {
                        id: simGrid
                        objectName: "moreLikeThisGrid"
                        width: moreSection.width
                        columns: 3
                        columnSpacing: Math.round(16 * root.s)
                        rowSpacing: Math.round(16 * root.s)
                        readonly property real cw: Math.floor((width - 2 * columnSpacing) / 3)
                        Repeater {
                            model: root.similar
                            delegate: Rectangle {
                                id: sim
                                required property var modelData
                                required property int index
                                Layout.preferredWidth: simGrid.cw
                                Layout.preferredHeight: simImg.height + simInfo.implicitHeight
                                Layout.fillHeight: true
                                Layout.alignment: Qt.AlignTop
                                radius: 4
                                color: "#2F2F2F"
                                Item {
                                    id: simImg
                                    width: parent.width; height: Math.round(width * 9 / 16)
                                    RoundedImage {
                                        anchors.fill: parent
                                        topLeftRadius: 4; topRightRadius: 4; bottomLeftRadius: 0; bottomRightRadius: 0
                                        bg: "#3A3A3A"
                                        source: sim.modelData.backdropImage
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        gradient: Gradient {
                                            GradientStop { position: 0.6; color: "transparent" }
                                            GradientStop { position: 1; color: Qt.rgba(0, 0, 0, 0.6) }
                                        }
                                    }
                                    Text {
                                        anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 8
                                        text: Theme.lengthLabel(sim.modelData.isSeries, sim.modelData.seasonCount, sim.modelData.episodeCount, sim.modelData.durationMs)
                                        color: "white"; font.family: Theme.font; font.pixelSize: Math.round(14 * root.s); font.weight: Font.Medium
                                        style: Text.Raised; styleColor: Qt.rgba(0, 0, 0, 0.5)
                                    }
                                    Text {
                                        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 10
                                        text: sim.modelData.title
                                        color: "white"; font.family: Theme.font; font.pixelSize: Math.round(15 * root.s); font.weight: Font.ExtraBold
                                        elide: Text.ElideRight
                                        style: Text.Raised; styleColor: Qt.rgba(0, 0, 0, 0.4)
                                    }
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: Math.round(44 * root.s); height: width; radius: width / 2
                                        color: Qt.rgba(30/255, 30/255, 20/255, 0.5)
                                        border.width: 1; border.color: "white"
                                        opacity: sma.containsMouse ? 1 : 0
                                        Behavior on opacity { NumberAnimation { duration: 150 } }
                                        Icon { anchors.centerIn: parent; anchors.horizontalCenterOffset: 1; name: "play"; size: Math.round(20 * root.s); color: "white" }
                                    }
                                }
                                MouseArea { id: sma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: Nav.openDetail(sim.modelData.id) }
                                Column {
                                    id: simInfo
                                    y: simImg.height
                                    width: parent.width
                                    padding: Math.round(14 * root.s)
                                    spacing: 10
                                    Item {
                                        width: parent.width - 2 * simInfo.padding
                                        height: Math.max(simMeta.height, simAdd.height)
                                        Column {
                                            id: simMeta
                                            spacing: 6
                                            Text { text: (sim.modelData.match || 95) + "% Match"; color: Theme.green; font.family: Theme.font; font.pixelSize: Math.round(15 * root.s); font.weight: Font.Bold }
                                            Row {
                                                spacing: 8
                                                Rectangle {
                                                    width: smr.implicitWidth + 10; height: smr.implicitHeight + 2
                                                    color: "transparent"; border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.4)
                                                    Text { id: smr; anchors.centerIn: parent; text: sim.modelData.rating || ""; color: "white"; font.family: Theme.font; font.pixelSize: Math.round(12 * root.s) }
                                                }
                                                Text { visible: sim.modelData.year > 0; text: sim.modelData.year; color: "white"; font.family: Theme.font; font.pixelSize: Math.round(15 * root.s) }
                                            }
                                        }
                                        CircleButton {
                                            id: simAdd
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            size: Math.round(38 * root.s)
                                            iconName: Library.inMyList(sim.modelData.id) || sim.modelData.inMyList ? "check" : "plus"
                                            tooltip: iconName === "check" ? "Remove from My List" : "Add to My List"
                                            onClicked: { Library.toggleMyList(sim.modelData.id); iconName = Library.inMyList(sim.modelData.id) ? "check" : "plus" }
                                        }
                                    }
                                    Text {
                                        width: parent.width - 2 * simInfo.padding
                                        text: sim.modelData.description || ""
                                        color: "#D2D2D2"; font.family: Theme.font; font.pixelSize: Math.round(14 * root.s)
                                        lineHeight: 1.3
                                        wrapMode: Text.WordWrap
                                        maximumLineCount: 4
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }
                }

                // ================= about =================
                Column {
                    x: Math.round(48 * root.s)
                    width: parent.width - 2 * x
                    topPadding: 44
                    bottomPadding: 40
                    spacing: 12
                    Text {
                        textFormat: Text.StyledText
                        text: "About <b>" + (root.hasTitle ? root.esc(root.t.title) : "") + "</b>"
                        color: "white"; font.family: Theme.font; font.pixelSize: Math.round(24 * root.s)
                        bottomPadding: 8
                        width: parent.width
                        wrapMode: Text.WordWrap
                    }
                    TagLine { width: parent.width; label: "Genres:"; value: root.t.genreText }
                    TagLine {
                        width: parent.width
                        label: root.hasTitle && root.t.isSeries ? "This show is:" : "This movie is:"
                        value: root.t.mainGenres
                    }
                    TagLine { width: parent.width; label: "Location:"; value: root.hasTitle ? (root.t.category || "") : "" }
                    Row {
                        spacing: 6
                        Text { text: "Maturity rating:"; color: "#777777"; font.family: Theme.font; font.pixelSize: Math.round(14 * root.s) }
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: amr.implicitWidth + 10; height: amr.implicitHeight + 2
                            color: "transparent"; border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.4)
                            Text { id: amr; anchors.centerIn: parent; text: root.hasTitle ? (root.t.rating || "") : ""; color: "white"; font.family: Theme.font; font.pixelSize: Math.round(12 * root.s) }
                        }
                        Text { text: root.advisory(root.t.rating) + (root.ageText(root.t.rating) ? "  ·  " + root.ageText(root.t.rating) : ""); color: "white"; font.family: Theme.font; font.pixelSize: Math.round(14 * root.s) }
                    }
                }
            }
        }
    }

    function esc(s: string): string { return String(s || "").replace(/&/g, "&amp;").replace(/</g, "&lt;") }
    function advisory(r: string): string {
        switch (r) {
        case "TV-MA": return "violence, language"
        case "R": return "violence, language, substances"
        case "TV-14": return "language, violence"
        case "PG-13": return "violence, language"
        case "PG": return "mild language"
        default: return ""
        }
    }
    function ageText(r: string): string {
        switch (r) {
        case "TV-MA": case "R": return "Recommended for ages 17 and up"
        case "TV-14": return "Recommended for ages 14 and up"
        case "PG-13": return "Recommended for ages 13 and up"
        case "PG": case "TV-PG": return "Recommended for ages 10 and up"
        default: return ""
        }
    }

    component TagLine: Text {
        property string label
        property string value
        readonly property real ks: Math.min(Theme.windowWidth * 0.9, Math.max(850, Theme.windowWidth * 0.53)) / 850
        width: parent ? parent.width : 200
        textFormat: Text.StyledText
        text: "<font color=\"#777777\">" + label + "</font> " + value.replace(/&/g, "&amp;").replace(/</g, "&lt;")
        color: "white"
        font.family: Theme.font
        font.pixelSize: Math.round(14 * ks)
        lineHeight: 1.3
        wrapMode: Text.WordWrap
        visible: value !== ""
    }
}
