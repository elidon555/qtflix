# qtflix

A streaming-service style front end for the videos already on your disk. Qt 6 / QML, Linux.

No accounts, no DRM, no network: it scans folders, groups episodes into series,
grabs thumbnails with ffmpeg, remembers where you stopped, and plays everything
through Qt Multimedia's FFmpeg backend.

## Build & run

```
sudo apt install cmake ninja-build qt6-base-dev qt6-declarative-dev qt6-multimedia-dev qt6-svg-dev qt6-shadertools-dev \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts qml6-module-qtquick-window \
  qml6-module-qtquick-templates qml6-module-qtquick-effects qml6-module-qtquick-shapes qml6-module-qtquick-dialogs \
  qml6-module-qtqml-workerscript qml6-module-qtmultimedia qml6-module-qtcore libqt6svg6 fonts-inter ffmpeg
./run.sh            # builds into ./build and launches
```

```
./run.sh --play=/path/to/file.mkv   # open straight into the player
./lint.sh                            # qmllint (+ clang-tidy / clazy if installed)
```

## Install (app menu shortcut)

```
./install.sh              # builds and installs to ~/.local, no sudo; QtFlix shows up in your app menu
./install.sh --deb        # builds build/qtflix_<ver>_amd64.deb and installs it with apt
./install.sh --system     # installs to /usr/local for all users
./install.sh --uninstall  # removes the ~/.local install (add --system for /usr/local)
```

Default library folders are `~/Videos` and `~/Downloads`. Change them in the avatar menu under Settings.
Folders are watched, so new files show up without a rescan.

## Real artwork and synopses (optional, TMDB)

Out of the box every card and billboard uses a frame grabbed from the file and a description built from
file facts. For real posters, backdrops, title logos, synopses, ratings and episode stills:

1. Create a free account at https://www.themoviedb.org and open https://www.themoviedb.org/settings/api.
2. Copy either the "API Key" (v3) or the "API Read Access Token" (v4).
3. In the app: avatar menu, Settings, Metadata. Paste the key and press Enter.

Status goes "Checking API key" then "Fetching n of N" then "Up to date". Everything is cached under
`~/.cache/qtflix`, so later launches work offline. Wrong match? Use "Fix a match" in the same section.
Screen recordings and ticket-style file names are never sent to TMDB.

This product uses the TMDB API but is not endorsed or certified by TMDB.

## Player

Space / K play-pause, Left / Right or J / L seek 10 s, Up / Down volume, M mute, F fullscreen, Escape back.
Blu-ray image subtitles (PGS) embedded in MKV files are decoded and drawn by the app itself, since Qt
Multimedia can't render them. Hover the timeline for frame previews (generated once per file in the background). Audio and subtitle tracks,
subtitle appearance, playback speed and volume are remembered between sessions.

Artwork: drop `poster.jpg` / `fanart.jpg` (or `<file>.jpg`, `<file>-fanart.jpg`) next to a video or in a
series folder and it is used instead of a grabbed frame. Subtitles: `<file>.srt`, `<file>.en.srt`, `.vtt`,
`.ass`, or a `Subs/` folder next to the file. Embedded audio and subtitle tracks are listed in the player.

Data lives in `~/.local/share/qtflix` (watch state, TMDB match overrides) and `~/.cache/qtflix`
(thumbnails, scrub previews, scan cache, TMDB cache). Settings are in `~/.config/qtflix/qtflix.conf`.

Debug switches: `QTFLIX_TIMING=1` (startup timings), `QTFLIX_HWACCEL=0` (software frame grabs),
`QTFLIX_NO_WARMUP=1` (no thumbnail pre-generation), `QTFLIX_SCREENSHOT=/path.png` (grab window and quit),
`QTFLIX_ROOT=DevBrowse|DevPlayer|DevPointer` (dev harnesses; DevPointer runs the pointer-event test suite).

## License

MIT, see [LICENSE](LICENSE).

qtflix is an independent project. It is not affiliated with, endorsed by or connected to Netflix, Inc.
