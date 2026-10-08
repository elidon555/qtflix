import QtQuick
import QtQuick.Controls
import QtFlix

// Dev harness for the Player (QTFLIX_ROOT=DevPlayer). Arguments:
//   --file=<path>        play this file;  --episode  play a Person of Interest episode instead of the movie
//   --size=<W>x<H>       window size (default 1600x900);  --fullscreen  start full screen
//   --controls           keep the controls visible (screenshots)
//   --panel=<name>       open a popup: volume | episodes | audio | speed;  --appearance  audio popup on its
//                        "Subtitle Appearance" page
//   --sub=<file.srt>     load a sidecar subtitle;  --substyle=<json>  e.g. {"size":"large","background":"box"}
//   --hover=<0..1>       show the timeline hover preview (trickplay) at that fraction
//   --flash              keyboard-style +10 s with center flash
//   --pause=<ms>         pause after <ms>;  --seek=<ms> seek after load;  --next  jump to 12 s before the end
//   --handoff=<ms>       call playNext() after <ms> (seamless next-episode swap test)
//   --quit=<ms>          close the player (like the back arrow) after <ms>
//   --dismiss=<ms>       press "Watch Credits" on the next-episode card after <ms>
//   --fps                log delivered video frames/s;  --renders  log rendered window frames/s (idle check)
ApplicationWindow {
    id: win
    width: sizeArg(0, 1600); height: sizeArg(1, 900)
    visible: true
    visibility: arg("fullscreen") !== "" ? Window.FullScreen : Window.Windowed
    color: "black"
    font.family: Theme.font
    title: "QtFlix Player (dev)"

    readonly property var args: Qt.application.arguments
    function arg(name) {
        for (var i = 0; i < args.length; ++i) {
            if (args[i] === "--" + name) return "1"
            if (args[i].indexOf("--" + name + "=") === 0) return args[i].substring(name.length + 3)
        }
        return ""
    }
    function sizeArg(i, def) {
        var s = arg("size").split("x")
        return s.length === 2 && parseInt(s[i]) > 0 ? parseInt(s[i]) : def
    }

    readonly property string moviePath: "/home/neziri/Downloads/Minority Report 2002 REMASTERED 1080p (Multi) BluRay HEVC x265 5.1 BONE.mkv"
    readonly property string episodePath: "/home/neziri/Videos/Person of Interest (2011) Season 5 S05 (1080p BluRay x265 HEVC 10bit AAC 5.1 RZeroX)/Person of Interest (2011) - S05E05 - ShotSeeker (1080p BluRay x265 RZeroX).mkv"

    Player {
        id: player
        path: win.arg("file") !== "" ? win.arg("file") : (win.arg("episode") !== "" ? win.episodePath : win.moviePath)
        forceControls: win.arg("controls") !== ""
        forceHoverFraction: win.arg("hover") !== "" ? parseFloat(win.arg("hover")) : -1
        onClosed: Qt.quit()
    }

    // --fps: print delivered video frames per second (smoothness check)
    property int frames: 0
    Connections {
        target: win.arg("fps") !== "" ? player.videoOutput.videoSink : null
        function onVideoFrameChanged() { win.frames++ }
    }
    // --renders: rendered window frames per second (should be 0 while paused with hidden controls)
    property int swaps: 0
    Connections {
        target: win.arg("renders") !== "" ? win : null
        function onFrameSwapped() { win.swaps++ }
    }
    Timer {
        interval: 1000; repeat: true; running: win.arg("fps") !== "" || win.arg("renders") !== ""
        onTriggered: {
            console.log("fps", win.frames, "renders", win.swaps, "pos", player.mediaPlayer.position,
                        "status", player.mediaPlayer.mediaStatus, "state", player.mediaPlayer.playbackState, "controls", player.controlsOpacity,
                        "trick", player.trickplay.ready)
            win.frames = 0; win.swaps = 0
        }
    }
    Timer {
        interval: 2500; running: true
        onTriggered: {
            if (win.arg("seek") !== "") player.seekTo(parseInt(win.arg("seek")))
            if (win.arg("substyle") !== "") player.setSubtitleStyleJson(win.arg("substyle"))
            if (win.arg("panel") !== "" || win.arg("appearance") !== "")
                player.openPanel = win.arg("panel") !== "" ? win.arg("panel") : "audio"
            if (win.arg("appearance") !== "") player.showSubtitleAppearance()
            if (win.arg("sub") !== "") {
                player.sidecars = [{ path: win.arg("sub"), label: "Test (sidecar)" }]
                player.chooseSubtitle(player.subtitleLabels.length - 1, false)
            }
            if (win.arg("flash") !== "") player.seekBy(10000, true)
            if (win.arg("next") !== "") player.seekTo(player.durationMs - 12000)
        }
    }
    Timer {
        interval: win.arg("pause") !== "" ? parseInt(win.arg("pause")) : 0
        running: win.arg("pause") !== ""
        onTriggered: player.togglePlay(false)
    }
    Timer {
        interval: win.arg("quit") !== "" ? parseInt(win.arg("quit")) : 0
        running: win.arg("quit") !== ""
        onTriggered: player.close()
    }
    Timer {
        interval: win.arg("dismiss") !== "" ? parseInt(win.arg("dismiss")) : 0
        running: win.arg("dismiss") !== ""
        onTriggered: player.nextDismissed = true
    }
    Timer {
        interval: win.arg("handoff") !== "" ? parseInt(win.arg("handoff")) : 0
        running: win.arg("handoff") !== ""
        onTriggered: player.playNext(true)
    }
}
