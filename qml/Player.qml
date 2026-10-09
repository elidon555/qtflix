import QtQuick
import QtMultimedia
import QtCore
import QtFlix

// Full-window video player modelled on the Netflix web player.
//   Player { path: "/abs/file.mkv"; onClosed: ... }
// Changing `path` while open swaps the source in place (next episode / episode picker) without
// recreating the player.
Rectangle {
    id: root

    // ---- public API ------------------------------------------------------------------------
    property string path: ""
    signal closed()
    property bool forceControls: false        // dev/screenshot aid: keep the controls visible
    property real forceHoverFraction: -1      // dev/screenshot aid: show the timeline preview there
    readonly property alias videoOutput: video
    readonly property alias mediaPlayer: player
    readonly property alias trickplay: trick

    // ---- state -----------------------------------------------------------------------------
    anchors.fill: parent
    color: "black"
    focus: true

    property string currentPath: ""           // file that is loaded in the MediaPlayer
    property var info: ({})
    property var sidecars: []                 // [{path,label}]
    property var episodeList: []
    property real resumeMs: 0
    property bool resumeDone: false           // initial seek handled: player.position/duration belong to currentPath
    property bool videoReady: false           // first frame of currentPath at the resume point is on screen
    property bool ended: false
    property bool nextDismissed: false        // "Watch Credits": stays dismissed until the end of the file
    property bool closing: false
    property bool finalized: false            // the final progress of currentPath has been written
    property bool watchedMarked: false        // Library.markWatched() done for currentPath
    property bool tracksApplied: false        // remembered audio/subtitle choice applied for currentPath
    property bool extrasReady: false          // sidecars / episodeList of currentPath are loaded

    // persisted player preferences (same QSettings file as the C++ side: player/<key>)
    Settings {
        id: settings
        category: "player"
        property real volume: 1.0
        property bool muted: false
        property real speed: 1.0
        property string subtitleStyle: ""
        property string lastSubtitle: ""      // JSON {off:true} | {lang, label, kind}
        property string lastAudio: ""         // JSON {lang, label}
    }
    property alias volumeLevel: settings.volume   // slider value 0..1 (perceptual)
    property alias muted: settings.muted

    property bool userActive: true            // the mouse moved / a key was pressed recently
    property string openPanel: ""             // "" | "volume" | "episodes" | "audio" | "speed"

    property int subtitleChoice: 0            // index into subtitleLabels (0 = Off)
    property bool enteredFullscreen: false
    property int visibilityBeforeFullscreen: Window.Windowed

    readonly property bool isPlaying: player.playbackState === MediaPlayer.PlayingState
    readonly property bool isPaused: player.playbackState === MediaPlayer.PausedState
    readonly property bool hasError: player.error !== MediaPlayer.NoError
    readonly property bool isFullScreen: Window.visibility === Window.FullScreen
    readonly property real durationMs: (resumeDone && player.duration > 0) ? player.duration : (info.durationMs || 0)
    // position of currentPath (the MediaPlayer still reports the old file's clock during a swap)
    readonly property real positionMs: resumeDone ? player.position : resumeMs
    readonly property bool hasNext: !!info.nextPath
    readonly property bool isSeries: !!info.isSeries

    // Spinner: while loading, or while "playing" but the clock is not advancing (the FFmpeg
    // backend's Buffering/Stalled statuses are not reliable for local files). A single-shot
    // watchdog restarted by every position update: it never fires during normal playback.
    property bool stalled: false
    Timer {
        id: stallWatch
        interval: 700
        onTriggered: root.stalled = root.isPlaying
    }
    onIsPlayingChanged: {
        stalled = false
        if (isPlaying) stallWatch.restart(); else stallWatch.stop()
    }
    readonly property bool buffering: !hasError && currentPath !== "" && !isPaused && !ended
                                      && (player.mediaStatus === MediaPlayer.LoadingMedia
                                          || player.mediaStatus === MediaPlayer.StalledMedia
                                          || stalled || !videoReady)

    readonly property string titleText: info.title || baseName(currentPath || path)
    // Netflix shows "E4 Episode title" next to the bold show title
    readonly property string episodeText: isSeries
        ? ((info.season === 0 ? qsTr("Special") + " " : "E") + (info.episode || 0)
           + (info.episodeTitle ? " " + info.episodeTitle : ""))
        : ""
    readonly property string pauseEpisodeText: isSeries
        ? ("Season " + (info.season || 0) + ": Ep. " + (info.episode || 0)
           + (info.episodeTitle ? " “" + info.episodeTitle + "”" : ""))
        : ""
    readonly property string description: {
        if (!info.id) return ""
        if (isSeries) {
            for (var i = 0; i < episodeList.length; ++i)
                if (episodeList[i].path === currentPath && episodeList[i].description)
                    return episodeList[i].description
        }
        var t = Library.title(info.id)
        return (t && t.description) ? t.description : ""
    }

    readonly property bool pauseOverlayShown: isPaused && !userActive && !forceControls && !hasError
                                               && !ended && openPanel === "" && !nextCard.shown
    readonly property bool controlsShown: forceControls || userActive || timeline.dragging
                                          || openPanel !== "" || hasError
                                          || (!isPlaying && !pauseOverlayShown)
    property real controlsOpacity: controlsShown ? 1 : 0
    Behavior on controlsOpacity { NumberAnimation { duration: 250; easing.type: Easing.InOutQuad } }
    readonly property bool cursorHidden: controlsOpacity < 0.05 && !hasError

    // ---- responsive metrics (layout follows the width: 1366 .. 2560+) -----------------------
    readonly property real sidePad: Math.round(Math.max(20, Math.min(48, width * 0.018)))
    readonly property real iconGap: width >= 1600 ? 32 : 24     // visual gap between 32 px icons
    readonly property int iconSize: 32

    // ---- helpers ---------------------------------------------------------------------------
    function fmt(ms: real): string {
        var s = Math.max(0, Math.floor(ms / 1000))
        var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60
        var ss = (sec < 10 ? "0" : "") + sec
        return h > 0 ? h + ":" + (m < 10 ? "0" : "") + m + ":" + ss : m + ":" + ss
    }
    function baseName(p) {
        if (!p) return ""
        var b = p.substring(p.lastIndexOf("/") + 1)
        var dot = b.lastIndexOf(".")
        return dot > 0 ? b.substring(0, dot) : b
    }
    function fileUrl(p) {
        return "file://" + p.split("/").map(encodeURIComponent).join("/")
    }
    function esc(s) {
        return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }
    function icon(name) { return "qrc:/assets/icons/player-" + name + ".svg" }
    function parseJson(s, fallback) {
        if (!s) return fallback
        try { var v = JSON.parse(s); return (v && typeof v === "object") ? v : fallback } catch (e) { return fallback }
    }

    function wake() {
        userActive = true
        idleTimer.restart()
    }

    function refreshInfo() {
        if (!currentPath) return
        var i = Library.fileInfo(currentPath)
        info = i ? i : ({})
        refreshExtras(currentPath)
    }
    // Sidecar subtitles (directory listings) and the season's episode list: not needed for the first
    // frame, so load() defers them until the media source is set.
    function refreshExtras(p) {
        if (p !== currentPath || !currentPath) return     // a newer file took over
        sidecars = Library.sidecarSubtitles(currentPath)
        refreshEpisodes()
        extrasReady = true
        if (resumeDone && !tracksApplied) applyRememberedTracks()
    }
    function refreshEpisodes() {
        episodeList = (info.isSeries && info.id) ? (Library.episodes(info.id, info.season) || []) : []
    }

    // ---- loading / handoff -----------------------------------------------------------------
    function load() {
        if (path === currentPath) return
        finishCurrent()                       // one final save for the outgoing file
        currentPath = path
        ended = false
        nextDismissed = false
        resumeDone = false
        videoReady = false
        finalized = false
        watchedMarked = false
        tracksApplied = false
        extrasReady = false
        sidecars = []
        episodeList = []
        openPanel = ""
        subtitleChoice = 0
        subs.source = ""
        seekThrottle.stop(); seekThrottle.target = -1
        endCloseTimer.stop()
        revealTimer.stop()
        if (path === "") {
            player.stop()
            player.source = ""
            info = ({})
            return
        }
        var i = Library.fileInfo(path)        // cheap lookup: url, resume point, next episode
        info = i ? i : ({})
        resumeMs = info.positionMs || Library.position(path) || 0
        if (info.durationMs > 0 && resumeMs >= info.durationMs * 0.95) resumeMs = 0   // finished before: start over
        player.source = info.url ? info.url : fileUrl(path)
        player.playbackRate = settings.speed
        player.play()
        Qt.callLater(refreshExtras, path)
        wake()                                // controls show the new title for a few seconds
        root.forceActiveFocus()
    }

    function tryResume() {
        if (resumeDone || player.duration <= 0) return
        var st = player.mediaStatus
        if (st !== MediaPlayer.LoadedMedia && st !== MediaPlayer.BufferingMedia && st !== MediaPlayer.BufferedMedia)
            return      // the FFmpeg backend ignores seeks issued while still loading
        resumeDone = true
        revealTimer.restart()
        if (resumeMs > 0 && resumeMs < player.duration * 0.95)
            player.position = resumeMs
        else
            resumeMs = 0
        applyRememberedTracks()
    }

    // periodic / pause save; never for a file whose media isn't loaded yet
    function saveProgress() {
        if (currentPath === "" || finalized || !resumeDone || player.duration <= 0 || player.position <= 0) return
        if (watchedMarked && player.position >= player.duration * 0.95) return   // keep the 100 %
        Library.setProgress(currentPath, player.position, player.duration)
    }
    function finishCurrent() {
        if (currentPath === "" || finalized) return
        saveProgress()
        finalized = true
    }
    function markWatchedOnce() {
        if (watchedMarked || currentPath === "") return
        watchedMarked = true
        Library.markWatched(currentPath)
    }

    function close() {
        if (closing) return
        closing = true
        finishCurrent()
        Library.flushProgress() // setProgress() persists lazily
        player.stop()
        restoreWindow()
        closed()
        closing = false
    }

    function playPath(p) {
        if (!p || p === currentPath) return
        if (Nav.playerPath !== "" && Nav.playerPath === path)
            Nav.playerPath = p           // keeps Main.qml's `path: Nav.playerPath` binding intact
        if (path !== p)
            path = p
    }

    // fromCard: the post-play card / end of file (counts as watched); else the "Next Episode" button
    function playNext(fromCard) {
        if (!hasNext) return
        if (fromCard) { markWatchedOnce(); finalized = true }
        playPath(info.nextPath)
    }

    function togglePlay(showFlash) {
        if (hasError) return
        if (ended) {
            ended = false; finalized = false; watchedMarked = false
            player.position = 0; player.play(); return
        }
        if (isPlaying) { player.pause(); if (showFlash) flash.flash(icon("pause")) }
        else { player.play(); if (showFlash) flash.flash(icon("play")) }
    }

    function seekTo(ms) {
        if (durationMs <= 0) return
        if (ended) { ended = false; finalized = false }
        player.position = Math.max(0, Math.min(durationMs - 500, ms))
    }
    function seekBy(delta, showFlash) {
        seekTo(player.position + delta)
        if (showFlash) flash.flash(icon(delta < 0 ? "rewind" : "forward"), "10")
    }

    function setVolume(v) {
        volumeLevel = Math.max(0, Math.min(1, v))
        muted = volumeLevel <= 0
    }
    function toggleMute() {
        muted = !muted
        if (!muted && volumeLevel <= 0) volumeLevel = 0.5
    }
    function volumeIcon() {
        if (muted || volumeLevel <= 0) return icon("volume-mute")
        return volumeLevel < 0.5 ? icon("volume-low") : icon("volume")
    }
    function setSpeed(r) {
        settings.speed = r
        player.playbackRate = r
    }

    function toggleFullscreen() {
        var w = Window.window
        if (!w) return
        if (w.visibility === Window.FullScreen) {
            w.visibility = visibilityBeforeFullscreen === Window.Maximized ? Window.Maximized : Window.Windowed
            enteredFullscreen = false
        } else {
            visibilityBeforeFullscreen = w.visibility
            enteredFullscreen = true
            w.visibility = Window.FullScreen      // frameless, covers the whole screen
        }
    }
    function restoreWindow() {
        var w = Window.window
        if (w && enteredFullscreen && w.visibility === Window.FullScreen)
            w.visibility = visibilityBeforeFullscreen === Window.Maximized ? Window.Maximized : Window.Windowed
        enteredFullscreen = false
    }

    // ---- tracks ----------------------------------------------------------------------------
    function trackLanguage(md) {
        if (!md) return ""
        var l = md.stringValue(MediaMetaData.Language)
        if (!l || l === "C" || l === "Default" || l === "Unknown" || l === "AnyLanguage") return ""
        return l
    }
    function trackTitle(md) { return md ? (md.stringValue(MediaMetaData.Title) || "") : "" }
    function trackLabels(tracks, fallback, isAudio) {
        var out = [], count = {}
        for (var i = 0; i < tracks.length; ++i) {
            var lang = trackLanguage(tracks[i])
            var key = lang || "?"
            count[key] = (count[key] || 0) + 1
        }
        var seen = {}
        for (var j = 0; j < tracks.length; ++j) {
            var lg = trackLanguage(tracks[j]), tt = trackTitle(tracks[j])
            var label = lg || tt || (fallback + " " + (j + 1))
            var k = lg || "?"
            if (lg && count[k] > 1) {
                seen[k] = (seen[k] || 0) + 1
                label += tt && !isAudio ? " (" + tt + ")" : " " + seen[k]
            }
            if (isAudio && j === 0 && tracks.length > 1) label += " [Original]"
            out.push(label)
        }
        return out
    }
    readonly property var audioLabels: trackLabels(player.audioTracks, qsTr("Track"), true)
    readonly property var embeddedSubLabels: trackLabels(player.subtitleTracks, qsTr("Subtitles"), false)
    readonly property var subtitleLabels: {
        var l = [qsTr("Off")].concat(embeddedSubLabels)
        for (var i = 0; i < sidecars.length; ++i) l.push(sidecars[i].label || baseName(sidecars[i].path))
        return l
    }
    readonly property bool embeddedSubtitleActive: subtitleChoice > 0 && subtitleChoice <= player.subtitleTracks.length

    function chooseSubtitle(i, remember) {
        subtitleChoice = i
        var nEmb = player.subtitleTracks.length
        if (i <= 0) {
            player.activeSubtitleTrack = -1
            subs.source = ""
        } else if (i <= nEmb) {
            subs.source = ""
            applyEmbeddedSubtitle(i - 1)
        } else {
            player.activeSubtitleTrack = -1
            subs.source = fileUrl(sidecars[i - 1 - nEmb].path)
        }
        if (i <= 0 || i > nEmb) { pendingEmbedded = -1; pgs.stream = -1 }
        if (remember) {
            settings.lastSubtitle = i <= 0 ? JSON.stringify({ off: true })
                : JSON.stringify({ lang: subtitleLang(i), label: cleanLabel(subtitleLabels[i]),
                                   kind: i <= nEmb ? "embedded" : "sidecar" })
        }
    }
    // Image-based Blu-ray subtitles (PGS) can't be drawn by Qt Multimedia (it only logs "Invalid subtitle
    // time"), so those tracks are rendered by PgsSubtitles instead and never handed to the MediaPlayer.
    property int pendingEmbedded: -1          // embedded choice waiting for the stream probe
    function applyEmbeddedSubtitle(ordinal) {
        if (!pgs.streamsKnown) {              // probe still running: decide when it lands
            pendingEmbedded = ordinal
            player.activeSubtitleTrack = -1
            pgs.stream = -1
            return
        }
        pendingEmbedded = -1
        if (pgs.isPgs(ordinal)) {
            player.activeSubtitleTrack = -1
            pgs.stream = ordinal
        } else {
            pgs.stream = -1
            player.activeSubtitleTrack = ordinal
        }
    }
    function chooseAudio(i, remember) {
        player.activeAudioTrack = i
        if (remember)
            settings.lastAudio = JSON.stringify({ lang: audioLang(i), label: cleanLabel(audioLabels[i]) })
    }

    // language matching for the remembered tracks: names ("English") or ISO codes ("en", "eng")
    readonly property var langCodes: ({
        en: "english", eng: "english", es: "spanish", spa: "spanish", fr: "french", fre: "french", fra: "french",
        de: "german", ger: "german", deu: "german", it: "italian", ita: "italian", pt: "portuguese", por: "portuguese",
        nl: "dutch", dut: "dutch", nld: "dutch", sq: "albanian", alb: "albanian", sqi: "albanian",
        ru: "russian", rus: "russian", ja: "japanese", jpn: "japanese", ko: "korean", kor: "korean",
        zh: "chinese", chi: "chinese", zho: "chinese", ar: "arabic", ara: "arabic", tr: "turkish", tur: "turkish",
        pl: "polish", pol: "polish", sv: "swedish", swe: "swedish", da: "danish", dan: "danish",
        no: "norwegian", nor: "norwegian", fi: "finnish", fin: "finnish", el: "greek", gre: "greek", ell: "greek",
        hr: "croatian", hrv: "croatian", sr: "serbian", srp: "serbian", ro: "romanian", rum: "romanian", ron: "romanian",
        hu: "hungarian", hun: "hungarian", cs: "czech", cze: "czech", ces: "czech", he: "hebrew", heb: "hebrew"
    })
    function cleanLabel(l) { return String(l || "").replace(/\s*\[Original\]$/, "").trim() }
    function normLang(s) {
        var t = String(s || "").toLowerCase().replace(/\[.*?\]|\(.*?\)/g, "").replace(/[._-]/g, " ")
                    .replace(/\s+\d+$/, "").trim()
        var first = t.split(/\s+/)[0] || ""
        return langCodes[first] || first
    }
    function subtitleLang(i) {
        var nEmb = player.subtitleTracks.length
        if (i >= 1 && i <= nEmb) {
            var l = trackLanguage(player.subtitleTracks[i - 1])
            if (l) return normLang(l)
        }
        return normLang(subtitleLabels[i])
    }
    function audioLang(i) {
        var l = trackLanguage(player.audioTracks[i])
        return normLang(l || audioLabels[i])
    }
    function applyRememberedTracks() {
        if (tracksApplied || !extrasReady) return     // refreshExtras() calls again once sidecars are known
        tracksApplied = true
        // audio
        var a = parseJson(settings.lastAudio, null)
        if (a && audioLabels.length > 1) {
            var best = -1
            for (var i = 0; i < audioLabels.length && best < 0; ++i)
                if (cleanLabel(audioLabels[i]) === a.label) best = i
            for (var j = 0; j < audioLabels.length && best < 0; ++j)
                if (a.lang && audioLang(j) === a.lang) best = j
            if (best >= 0 && best !== player.activeAudioTrack) chooseAudio(best, false)
        }
        // subtitles
        var s = parseJson(settings.lastSubtitle, null)
        if (!s || s.off) return
        var nEmb = player.subtitleTracks.length
        var pick = -1
        for (var k = 1; k < subtitleLabels.length && pick < 0; ++k)
            if (cleanLabel(subtitleLabels[k]) === s.label && (k <= nEmb) === (s.kind === "embedded")) pick = k
        for (var m = 1; m < subtitleLabels.length && pick < 0; ++m)
            if (s.lang && subtitleLang(m) === s.lang && (m <= nEmb) === (s.kind === "embedded")) pick = m
        for (var n = 1; n < subtitleLabels.length && pick < 0; ++n)
            if (s.lang && subtitleLang(n) === s.lang) pick = n
        if (pick > 0) chooseSubtitle(pick, false)
    }

    // ---- subtitle appearance ---------------------------------------------------------------
    property string styleOverride: ""          // dev harness: non-persistent style JSON
    function setSubtitleStyleJson(json) { styleOverride = json }
    function showSubtitleAppearance() { openPanel = "audio"; audioPanel.page = "appearance" }
    readonly property var subtitleStyle: {
        var d = { size: "medium", edge: "shadow", background: "none", color: "white" }
        var v = parseJson(styleOverride || settings.subtitleStyle, {})
        for (var k in d) if (typeof v[k] === "string") d[k] = v[k]
        return d
    }
    function setSubtitleStyle(key, value) {
        var s = Object.assign({}, subtitleStyle)
        s[key] = value
        styleOverride = ""
        settings.subtitleStyle = JSON.stringify(s)
    }

    // ---- lifecycle -------------------------------------------------------------------------
    // the first load waits for completion so `settings` (speed, tracks) is already read from disk
    property bool completed: false
    onPathChanged: if (completed) load()
    Component.onCompleted: {
        completed = true
        if (path !== "" && currentPath !== path) load()
        forceActiveFocus()
    }
    Component.onDestruction: { finishCurrent(); restoreWindow() }
    onVisibleChanged: if (visible) forceActiveFocus()
    onActiveFocusChanged: if (!activeFocus && visible && path !== "") Qt.callLater(function() {
        if (root.visible && !root.activeFocus) root.forceActiveFocus()
    })
    onOpenPanelChanged: {
        if (openPanel === "episodes") refreshEpisodes()     // fresh progress / watched marks
        if (openPanel === "") forceActiveFocus()
    }

    Connections {
        target: Library
        function onLibraryChanged() { root.refreshInfo() }
    }

    // ---- media -----------------------------------------------------------------------------
    MediaPlayer {
        id: player
        videoOutput: video
        audioOutput: audio
        onMediaStatusChanged: {
            if (mediaStatus === MediaPlayer.LoadedMedia || mediaStatus === MediaPlayer.BufferedMedia)
                root.tryResume()
            if (mediaStatus === MediaPlayer.EndOfMedia && root.resumeDone) {
                root.ended = true
                root.videoReady = true
                root.markWatchedOnce()
                root.finalized = true
                if (!root.hasNext) endCloseTimer.restart()
            }
        }
        onDurationChanged: root.tryResume()
        onTracksChanged: if (root.resumeDone) { root.tracksApplied = false; root.applyRememberedTracks() }
        onPositionChanged: {
            root.stalled = false
            if (root.isPlaying) stallWatch.restart()
            if (!root.resumeDone) return
            if (!root.videoReady && (root.resumeMs <= 0 || position >= root.resumeMs - 3000))
                root.videoReady = true
            if (duration > 0) {
                if (!root.watchedMarked && position >= duration * 0.95) root.markWatchedOnce()
                else if (root.watchedMarked && position < duration * 0.9 && !root.ended) root.watchedMarked = false
            }
        }
        onPlaybackStateChanged: {
            if (playbackState === MediaPlayer.PausedState) { root.saveProgress(); Library.flushProgress() }
            root.wake()
        }
        onErrorOccurred: function(error, errorString) {
            console.warn("Player error", error, errorString)
        }
    }
    AudioOutput {
        id: audio
        volume: root.volumeLevel * root.volumeLevel     // perceptual curve
        muted: root.muted
    }
    VideoOutput {
        id: video
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectFit
        // hide the frame-0 flash before the resume seek of a new file lands
        opacity: root.videoReady ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
    }

    SubtitleTrack {
        id: subs
        positionMs: player.position
    }

    Trickplay {
        id: trick
        file: root.currentPath
        durationMs: root.durationMs
        enabled: root.visible && root.currentPath !== ""
    }

    Timer { id: saveTimer; interval: 5000; repeat: true; running: root.isPlaying; onTriggered: root.saveProgress() }
    Timer { id: idleTimer; interval: 3000; onTriggered: root.userActive = false }
    Timer { id: revealTimer; interval: 1500; onTriggered: root.videoReady = true }   // seek never "landed"
    Timer { id: endCloseTimer; interval: 1500; onTriggered: root.close() }
    Timer { id: seekThrottle; interval: 120; property real target: -1
        onTriggered: if (target >= 0) { root.seekTo(target); target = -1 } }

    // ---- mouse on the video: click = play/pause, double click = fullscreen, move = wake ----
    property real lastMouseX: -1
    property real lastMouseY: -1
    function mouseMoved(x, y) {
        if (Math.abs(x - lastMouseX) + Math.abs(y - lastMouseY) > 2) {
            lastMouseX = x; lastMouseY = y
            wake()
        }
    }
    HoverHandler {
        id: rootHover
        cursorShape: root.cursorHidden ? Qt.BlankCursor : Qt.ArrowCursor
        onPointChanged: root.mouseMoved(point.position.x, point.position.y)
    }
    MouseArea {
        id: videoMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: root.cursorHidden ? Qt.BlankCursor : Qt.ArrowCursor
        onPositionChanged: function(m) { root.mouseMoved(m.x, m.y) }
        onClicked: {
            root.forceActiveFocus()
            if (root.openPanel !== "") { root.openPanel = ""; return }
            root.togglePlay(true)
        }
        onDoubleClicked: {
            root.togglePlay(false)   // undo the first click's toggle, like Netflix
            root.toggleFullscreen()
        }
        onWheel: function(w) { root.setVolume(root.volumeLevel + (w.angleDelta.y > 0 ? 0.05 : -0.05)); root.wake() }
    }

    // ---- sidecar subtitles (embedded text tracks are drawn by VideoOutput itself) ----------
    PlayerSubtitleText {
        id: subtitleText
        visible: text !== "" && !root.hasError
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.controlsOpacity > 0.5 ? bottomBar.height + 16 : Math.round(root.height * 0.08)
        Behavior on anchors.bottomMargin { NumberAnimation { duration: 250; easing.type: Easing.InOutQuad } }
        maxWidth: root.width * 0.8
        text: subs.currentText
        active: subs.valid && !root.hasError    // a sidecar track is on: keep its shadow layer alive
        spec: root.subtitleStyle
        basePixelSize: Math.max(20, Math.round(root.height * 0.034))
    }

    // ---- Blu-ray image subtitles (PGS) ------------------------------------------------------
    PgsSubtitles {
        id: pgs
        anchors.fill: parent
        visible: !root.hasError && stream >= 0
        file: root.currentPath
        positionMs: player.position
        videoRect: video.contentRect
        lift: root.controlsOpacity > 0.5 ? bottomBar.height + 16 : 0
        onStreamsChanged: if (streamsKnown && root.pendingEmbedded >= 0) root.applyEmbeddedSubtitle(root.pendingEmbedded)
    }

    // ---- pause screen ----------------------------------------------------------------------
    PlayerPauseOverlay {
        anchors.fill: parent
        shown: root.pauseOverlayShown
        title: root.titleText
        episodeLine: root.pauseEpisodeText
        description: root.description
    }

    // ---- buffering / feedback --------------------------------------------------------------
    PlayerSpinner {
        anchors.centerIn: parent
        visible: root.buffering
    }
    PlayerFlash {
        id: flash
        anchors.centerIn: parent
    }

    // ---- controls overlay ------------------------------------------------------------------
    Item {
        id: controls
        anchors.fill: parent
        opacity: root.controlsOpacity
        visible: opacity > 0.01

        // top shade + back arrow
        Rectangle {
            anchors.top: parent.top
            width: parent.width; height: Math.round(parent.height * 0.15)
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.6) }
                GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.0) }
            }
        }
        PlayerIconButton {
            id: backBtn
            x: root.sidePad - 8; y: root.sidePad - 8
            iconSize: root.iconSize                     // 48 px hit area
            icon: root.icon("back")
            onClicked: root.close()
        }

        // bottom shade: transparent -> 80 % black over the last quarter of the height
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width; height: Math.round(parent.height * 0.25)
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.0) }
                GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.8) }
            }
        }

        Item {
            id: bottomBar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: root.sidePad
            anchors.rightMargin: root.sidePad
            height: 132

            // timeline row
            PlayerTimeline {
                id: timeline
                anchors.left: parent.left
                anchors.right: remaining.left
                anchors.rightMargin: 18
                y: 22
                height: 24
                duration: root.durationMs
                trickplay: trick
                forceHoverFraction: root.forceHoverFraction
                onSeekRequested: function(ms, done) {
                    if (done) { seekThrottle.stop(); seekThrottle.target = -1; root.seekTo(ms) }
                    else { seekThrottle.target = ms; if (!seekThrottle.running) seekThrottle.start() }
                }
            }
            Text {
                id: remaining
                anchors.right: parent.right
                anchors.verticalCenter: timeline.verticalCenter
                color: "white"
                font.family: Theme.font
                font.pixelSize: 16
                font.weight: Font.Medium
                font.features: { "tnum": 1 }
            }

            // the clock only drives these while the controls are visible (the values are gated too: a
            // Binding evaluates `value` even while `when` is false)
            Binding {
                target: timeline; property: "position"
                when: controls.visible; value: controls.visible ? root.positionMs : 0
                restoreMode: Binding.RestoreNone
            }
            Binding {
                target: remaining; property: "text"
                when: controls.visible
                value: controls.visible
                       ? root.fmt(Math.max(0, root.durationMs - (timeline.dragging ? timeline.dragPos : root.positionMs)))
                       : ""
                restoreMode: Binding.RestoreNone
            }

            // button row
            Item {
                id: buttonRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 14
                height: 56

                Row {
                    id: leftGroup
                    anchors.left: parent.left
                    anchors.leftMargin: -8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: root.iconGap - 16
                    PlayerIconButton {
                        iconSize: root.iconSize
                        icon: root.isPlaying ? root.icon("pause") : root.icon("play")
                        onClicked: root.togglePlay(false)
                    }
                    PlayerIconButton {
                        iconSize: root.iconSize
                        icon: root.icon("rewind"); overlayText: "10"
                        onClicked: root.seekBy(-10000, false)
                    }
                    PlayerIconButton {
                        iconSize: root.iconSize
                        icon: root.icon("forward"); overlayText: "10"
                        onClicked: root.seekBy(10000, false)
                    }
                    PlayerIconButton {
                        id: volumeBtn
                        iconSize: root.iconSize
                        icon: root.volumeIcon()
                        active: root.openPanel === "volume"
                        onHoveredChanged: if (hovered) root.openPanel = "volume"
                        onClicked: root.toggleMute()
                    }
                }

                Text {
                    id: titleLabel
                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, parent.width - 2 * Math.max(leftGroup.width, rightGroup.width) - 60)
                    elide: Text.ElideRight
                    textFormat: Text.StyledText
                    text: "<b>" + root.esc(root.titleText) + "</b>"
                          + (root.episodeText ? "&nbsp;&nbsp;" + root.esc(root.episodeText) : "")
                    color: "white"
                    font.family: Theme.font
                    font.pixelSize: 20
                    font.weight: Font.Normal
                }

                Row {
                    id: rightGroup
                    anchors.right: parent.right
                    anchors.rightMargin: -8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: root.iconGap - 16
                    PlayerIconButton {
                        id: nextBtn
                        visible: root.hasNext
                        iconSize: root.iconSize
                        icon: root.icon("next")
                        onHoveredChanged: if (hovered) root.openPanel = ""
                        onClicked: root.playNext(false)
                    }
                    PlayerIconButton {
                        id: episodesBtn
                        visible: root.isSeries && root.episodeList.length > 0
                        iconSize: root.iconSize
                        icon: root.icon("episodes")
                        active: root.openPanel === "episodes"
                        onHoveredChanged: if (hovered) root.openPanel = "episodes"
                        onClicked: root.openPanel = "episodes"
                    }
                    PlayerIconButton {
                        id: audioBtn
                        iconSize: root.iconSize
                        icon: root.icon("subtitles")
                        active: root.openPanel === "audio"
                        onHoveredChanged: if (hovered) root.openPanel = "audio"
                        onClicked: root.openPanel = "audio"
                    }
                    PlayerIconButton {
                        id: speedBtn
                        iconSize: root.iconSize
                        icon: root.icon("speed")
                        active: root.openPanel === "speed"
                        onHoveredChanged: if (hovered) root.openPanel = "speed"
                        onClicked: root.openPanel = "speed"
                    }
                    PlayerIconButton {
                        id: fsBtn
                        iconSize: root.iconSize
                        icon: root.isFullScreen ? root.icon("fullscreen-exit") : root.icon("fullscreen")
                        onHoveredChanged: if (hovered) root.openPanel = ""
                        onClicked: root.toggleFullscreen()
                    }
                }
            }
        }

        // ---- popups (anchored above their buttons) ----------------------------------------
        function panelX(btn, w) {
            // depend on geometry so the binding re-evaluates on resize / relayout
            var dummy = controls.width + rightGroup.x + leftGroup.x + btn.x + btn.visible
            var p = btn.mapToItem(controls, btn.width / 2, 0)
            return Math.max(16, Math.min(controls.width - w - 16, p.x - w / 2))
        }
        readonly property real panelBottom: bottomBar.y + timeline.y - 10

        PlayerVolumePanel {
            id: volumePanel
            open: root.openPanel === "volume"
            x: controls.panelX(volumeBtn, width)
            y: controls.panelBottom - height
            level: root.volumeLevel
            muted: root.muted
            onLevelPicked: function(v) { root.setVolume(v) }
        }
        PlayerEpisodesPanel {
            id: episodesPanel
            open: root.openPanel === "episodes"
            width: Math.min(620, controls.width - 32)
            x: Math.max(16, Math.min(controls.width - width - 16, controls.panelX(episodesBtn, width) + width / 2 - 80))
            y: controls.panelBottom - height
            maxHeight: controls.panelBottom - 90
            season: root.info.season || 1
            episodes: root.episodeList
            currentPath: root.currentPath
            onEpisodePicked: function(p) { root.playPath(p) }
        }
        PlayerAudioSubsPanel {
            id: audioPanel
            open: root.openPanel === "audio"
            x: Math.max(16, Math.min(controls.width - width - 16, controls.panelX(audioBtn, width) + width / 2 - 120))
            y: controls.panelBottom - height
            maxHeight: controls.panelBottom - 90
            audioLabels: root.audioLabels
            audioIndex: Math.max(0, player.activeAudioTrack)
            subtitleLabels: root.subtitleLabels
            subtitleIndex: root.subtitleChoice
            embeddedSubtitleActive: root.embeddedSubtitleActive
            subtitleStyle: root.subtitleStyle
            onAudioSelected: function(i) { root.chooseAudio(i, true) }
            onSubtitleSelected: function(i) { root.chooseSubtitle(i, true) }
            onStyleOptionPicked: function(k, v) { root.setSubtitleStyle(k, v) }
        }
        PlayerSpeedPanel {
            id: speedPanel
            open: root.openPanel === "speed"
            x: Math.max(16, Math.min(controls.width - width - 16, controls.panelX(speedBtn, width) + width / 2 - 140))
            y: controls.panelBottom - height
            rate: player.playbackRate
            onRateSelected: function(r) { root.setSpeed(r) }
        }
    }

    // keep a popup open while the pointer is on its button or on the popup itself
    readonly property bool panelHeld: {
        switch (openPanel) {
        case "volume": return volumeBtn.hovered || volumePanel.hovered
        case "episodes": return episodesBtn.hovered || episodesPanel.hovered
        case "audio": return audioBtn.hovered || audioPanel.hovered
        case "speed": return speedBtn.hovered || speedPanel.hovered
        }
        return true
    }
    Timer {
        interval: 350
        running: root.openPanel !== "" && !root.panelHeld && !root.forceControls
        onTriggered: root.openPanel = ""
    }

    // ---- next episode card -----------------------------------------------------------------
    // Shows 15 s before the end; "Watch Credits" hides it until the end of the file, where it
    // comes back (with the auto-play countdown).
    PlayerNextCard {
        id: nextCard
        anchors.right: parent.right
        anchors.rightMargin: root.sidePad + 12
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.controlsOpacity > 0.5 ? bottomBar.height + 20 : 48
        Behavior on anchors.bottomMargin { NumberAnimation { duration: 250; easing.type: Easing.InOutQuad } }
        shown: root.hasNext && !root.hasError && root.resumeDone && root.durationMs > 0
               && (root.ended || (!root.nextDismissed && player.position > root.durationMs - 15000))
        paused: root.isPaused && !root.ended
        label: root.info.nextLabel || ""
        thumb: {
            for (var i = 0; i < root.episodeList.length; ++i)
                if (root.episodeList[i].path === root.info.nextPath)
                    return root.episodeList[i].still || root.episodeList[i].thumb
            return root.info.backdrop || ""
        }
        onPlayNext: root.playNext(true)
        onDismissed: root.nextDismissed = true
    }

    // ---- error state -----------------------------------------------------------------------
    Rectangle {
        anchors.fill: parent
        visible: root.hasError
        color: "black"
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }
        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 80, 720)
            spacing: 16
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Whoops, something went wrong…")
                color: "white"
                font.family: Theme.font
                font.pixelSize: 40
                font.weight: Font.Bold
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Error Code: QF-%1").arg(1000 + player.error)
                color: "#b3b3b3"
                font.family: Theme.font
                font.pixelSize: 18
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: player.errorString
                color: "#808080"
                font.family: Theme.font
                font.pixelSize: 16
                wrapMode: Text.WordWrap
            }
            Item { width: 1; height: 12 }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 160; height: 48; radius: 4
                color: errBackMa.containsMouse ? "#e6e6e6" : "white"
                Text {
                    anchors.centerIn: parent
                    text: qsTr("Back")
                    color: "black"
                    font.family: Theme.font
                    font.pixelSize: 18
                    font.weight: Font.Bold
                }
                MouseArea {
                    id: errBackMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.close()
                }
            }
        }
    }

    // ---- keyboard --------------------------------------------------------------------------
    // Every handled key is accepted so nothing underneath (browse Flickables) scrolls.
    Keys.onPressed: function(e) {
        root.wake()
        switch (e.key) {
        case Qt.Key_Space:
        case Qt.Key_K:
        case Qt.Key_MediaPlay:
        case Qt.Key_MediaPause:
        case Qt.Key_MediaTogglePlayPause:
            root.togglePlay(true); break
        case Qt.Key_Left:
        case Qt.Key_J:
            root.seekBy(-10000, true); break
        case Qt.Key_Right:
        case Qt.Key_L:
            root.seekBy(10000, true); break
        case Qt.Key_Up:
            root.setVolume(root.volumeLevel + 0.1); flash.flash(root.volumeIcon()); break
        case Qt.Key_Down:
            root.setVolume(root.volumeLevel - 0.1); flash.flash(root.volumeIcon()); break
        case Qt.Key_M:
            root.toggleMute(); flash.flash(root.volumeIcon()); break
        case Qt.Key_F:
            root.toggleFullscreen(); break
        case Qt.Key_Escape:
            if (root.openPanel !== "") root.openPanel = ""
            else if (root.isFullScreen) root.toggleFullscreen()
            else root.close()
            break
        case Qt.Key_Home:
            root.seekTo(0); break
        case Qt.Key_End:
            root.seekTo(root.durationMs - 1000); break
        case Qt.Key_N:
            if (root.hasNext && (e.modifiers & Qt.ShiftModifier)) root.playNext(false)
            break
        case Qt.Key_PageUp:
        case Qt.Key_PageDown:
        case Qt.Key_Tab:
        case Qt.Key_Backtab:
        case Qt.Key_Return:
        case Qt.Key_Enter:
            break
        default:
            return
        }
        e.accepted = true
    }
}
