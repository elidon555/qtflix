pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtFlix

// Settings, styled like Netflix's account pages (dark variant): big title, section headers, rounded cards
// with rows separated by hairlines, red toggles.
FocusScope {
    id: page
    objectName: "settingsPage"
    property alias scrollY: scroller.contentY
    readonly property real colW: Math.min(Math.round(Theme.clamp(Theme.vw(52), 700, 1200)), width - 2 * Theme.gutter)
    readonly property real fs: Math.round(Theme.clamp(Theme.vw(1.0), 15, 22))
    property string historyNote: ""

    FolderDialog {
        id: folderDialog
        title: "Add a library folder"
        onAccepted: Library.addFolder(selectedFolder)
    }

    // No bulk API: clear every file with progress (movies + every episode of every series).
    function clearWatchHistory() {
        const paths = []
        const all = Library.allTitles
        for (let i = 0; all && i < all.count; ++i) {
            const t = all.get(i)
            if (!t.isSeries) {
                if ((t.positionMs || 0) > 0 || (t.progress || 0) > 0) paths.push(t.path)
                continue
            }
            const seasons = Library.seasons(t.id) || []
            for (let s = 0; s < seasons.length; ++s) {
                const eps = Library.episodes(t.id, seasons[s]) || []
                for (let e = 0; e < eps.length; ++e)
                    if ((eps[e].positionMs || 0) > 0 || (eps[e].progress || 0) > 0) paths.push(eps[e].path)
            }
        }
        // anything still listed in Continue Watching
        const cw = Library.continueWatching
        for (let k = 0; cw && k < cw.count; ++k) { const p = cw.get(k).path; if (paths.indexOf(p) < 0) paths.push(p) }
        for (let j = 0; j < paths.length; ++j) Library.clearProgress(paths[j])
        historyNote = paths.length === 0 ? "Your watch history is already empty."
                                         : "Cleared watch history for " + paths.length + (paths.length === 1 ? " video." : " videos.")
    }

    ScrollArea {
        id: scroller
        anchors.fill: parent
        focus: true
        contentHeight: col.y + col.height + 80

        Column {
            id: col
            x: Math.round((scroller.width - page.colW) / 2)
            y: Theme.navH + Math.round(page.fs * 2.5)
            width: page.colW
            spacing: 0

            Text {
                text: "Settings"
                color: "white"
                font.family: Theme.font; font.pixelSize: Math.round(page.fs * 2.4); font.weight: Font.Bold
                bottomPadding: 6
            }
            Row {
                spacing: 10
                bottomPadding: Math.round(page.fs * 1.8)
                Avatar { width: Math.round(page.fs * 1.5); height: width; avatarColor: Nav.profileColor; variant: Nav.profileAvatar; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Nav.profileName + " · " + Library.titleCount + (Library.titleCount === 1 ? " title" : " titles") + " in your library"
                    color: "#B3B3B3"; font.family: Theme.font; font.pixelSize: page.fs
                }
            }

            // ---------------- Playback ----------------
            SectionHeader { text: "Playback settings" }
            Card {
                SettingRow {
                    objectName: "autoplayRow"
                    title: "Autoplay previews while browsing on all devices"
                    subtitle: "Previews play on the billboard, when you hover a title and in the details page."
                    Toggle { objectName: "autoplayToggle"; checked: Theme.autoplayPreviews; onToggled: (v) => Theme.autoplayPreviews = v }
                }
                SettingRow {
                    title: "Previews muted by default"
                    subtitle: "Start previews without sound. You can always unmute one with its speaker button."
                    last: true
                    Toggle { checked: Theme.previewMuted; onToggled: (v) => Theme.previewMuted = v }
                }
            }

            // ---------------- Library ----------------
            SectionHeader { text: "Library" }
            Card {
                SettingRow {
                    title: Library.scanning ? "Scanning…" : (Library.titleCount + (Library.titleCount === 1 ? " title" : " titles"))
                    subtitle: (Library.movies ? Library.movies.count : 0) + " movies · " + (Library.series ? Library.series.count : 0) + " series · "
                              + (Library.myList ? Library.myList.count : 0) + " in My List"
                    Row {
                        spacing: 10
                        Spinner { anchors.verticalCenter: parent.verticalCenter; visible: Library.scanning; size: 20 }
                        NfFlatButton { label: Library.scanning ? "Scanning…" : "Rescan"; enabled: !Library.scanning; onClicked: Library.rescan() }
                        NfFlatButton { label: "Add folder"; red: true; onClicked: folderDialog.open() }
                    }
                }
                Text {
                    visible: Library.folders.length === 0
                    x: Math.round(page.fs * 1.5)
                    text: "No folders yet. Add a folder with movies or TV shows."
                    color: "#B3B3B3"; font.family: Theme.font; font.pixelSize: page.fs
                    topPadding: 16; bottomPadding: 16
                }
                Repeater {
                    model: Library.folders
                    delegate: Rectangle {
                        id: frow
                        required property string modelData
                        required property int index
                        width: parent.width
                        height: Math.round(page.fs * 3.4)
                        color: fhov.hovered ? Qt.rgba(1, 1, 1, 0.04) : "transparent"
                        HoverHandler { id: fhov }
                        Rectangle { width: parent.width; height: 1; color: "#333333" }
                        Text {
                            x: Math.round(page.fs * 1.5)
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - x - 80
                            text: frow.modelData
                            color: "white"; font.family: Theme.font; font.pixelSize: page.fs
                            elide: Text.ElideMiddle
                        }
                        Item {
                            id: rm
                            anchors.right: parent.right; anchors.rightMargin: Math.round(page.fs * 1.2)
                            anchors.verticalCenter: parent.verticalCenter
                            width: 32; height: 32
                            Rectangle {
                                anchors.fill: parent; radius: 16
                                color: rma.containsMouse ? "#333333" : "transparent"
                                border.width: 1; border.color: rma.containsMouse ? "white" : Qt.rgba(1, 1, 1, 0.4)
                            }
                            Icon { anchors.centerIn: parent; name: "close"; size: 14; color: "white"; strokeWidth: 2.4 }
                            MouseArea { id: rma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: Library.removeFolder(frow.modelData) }
                            ToolTip.visible: rma.containsMouse
                            ToolTip.text: "Remove folder"
                            ToolTip.delay: 500
                        }
                    }
                }
            }

            // ---------------- Metadata (SettingsMetadata.qml, owned by the metadata agent) ----------------
            SectionHeader { text: "Metadata"; visible: metaLoader.status === Loader.Ready }
            Card {
                visible: metaLoader.status === Loader.Ready
                Loader {
                    id: metaLoader
                    x: Math.round(page.fs * 1.5)
                    width: parent.width - 2 * x
                    source: "SettingsMetadata.qml"
                    onStatusChanged: if (status === Loader.Error) source = ""   // not written yet: ignore
                }
                Item { width: 1; height: Math.round(page.fs * 1.2) }
            }

            // ---------------- Viewing activity ----------------
            SectionHeader { text: "Viewing activity" }
            Card {
                SettingRow {
                    title: "Clear watch history"
                    subtitle: page.historyNote !== "" ? page.historyNote
                                                      : "Removes resume points and the Continue Watching row. My List is kept."
                    last: true
                    NfFlatButton { objectName: "clearHistoryButton"; label: "Clear watch history"; onClicked: page.clearWatchHistory() }
                }
            }

            // ---------------- Profile ----------------
            SectionHeader { text: "Profile" }
            Card {
                SettingRow {
                    title: Nav.profileName
                    subtitle: "Profiles are saved on this computer."
                    last: true
                    leading: Avatar { width: Math.round(page.fs * 2.8); height: width; avatarColor: Nav.profileColor; variant: Nav.profileAvatar }
                    NfFlatButton { label: "Manage profiles"; onClicked: Nav.go("profiles") }
                }
            }
        }
    }

    // ---------------- building blocks ----------------
    component SectionHeader: Text {
        color: "white"
        font.family: Theme.font; font.pixelSize: Math.round(page.fs * 1.35); font.weight: Font.Bold
        topPadding: Math.round(page.fs * 1.8); bottomPadding: Math.round(page.fs * 0.8)
    }
    component Card: Rectangle {
        default property alias content: cardCol.data
        width: page.colW
        height: cardCol.height
        radius: 8
        color: "#1F1F1F"
        border.width: 1; border.color: "#2E2E2E"
        clip: true
        Column { id: cardCol; width: parent.width }
    }
    component SettingRow: Item {
        id: sr
        property string title
        property string subtitle
        property bool last: false
        property Item leading: null
        default property alias trailing: trailingBox.data
        width: parent ? parent.width : page.colW
        height: Math.max(textCol.implicitHeight, trailingBox.childrenRect.height) + Math.round(page.fs * 2.2)
        function adoptLeading() { if (leading) { leading.parent = leadBox; leading.anchors.verticalCenter = leadBox.verticalCenter } }
        onLeadingChanged: adoptLeading()
        Component.onCompleted: adoptLeading()
        Item { id: leadBox; x: Math.round(page.fs * 1.5); width: sr.leading ? sr.leading.width + Math.round(page.fs) : 0; height: parent.height }
        Column {
            id: textCol
            anchors.left: leadBox.right
            anchors.right: trailingBox.left; anchors.rightMargin: Math.round(page.fs * 1.5)
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            Text { width: parent.width; text: sr.title; color: "white"; font.family: Theme.font; font.pixelSize: page.fs; font.weight: Font.DemiBold; wrapMode: Text.WordWrap }
            Text { width: parent.width; visible: text !== ""; text: sr.subtitle; color: "#A3A3A3"; font.family: Theme.font; font.pixelSize: Math.round(page.fs * 0.88); wrapMode: Text.WordWrap }
        }
        Item {
            id: trailingBox
            anchors.right: parent.right; anchors.rightMargin: Math.round(page.fs * 1.5)
            anchors.verticalCenter: parent.verticalCenter
            width: childrenRect.width; height: childrenRect.height
        }
        Rectangle { visible: !sr.last; anchors.bottom: parent.bottom; x: Math.round(page.fs * 1.5); width: parent.width - 2 * x; height: 1; color: "#333333" }
    }
    // Netflix-style switch: red track when on, gray when off, white knob
    component Toggle: Item {
        id: tg
        property bool checked: false
        signal toggled(bool value)
        width: Math.round(page.fs * 2.9); height: Math.round(page.fs * 1.6)
        Rectangle {
            anchors.fill: parent; radius: height / 2
            color: tg.checked ? Theme.red : "#5A5A5A"
            Behavior on color { ColorAnimation { duration: 150 } }
        }
        Rectangle {
            width: tg.height - 6; height: width; radius: width / 2
            y: 3; x: tg.checked ? tg.width - width - 3 : 3
            color: "white"
            Behavior on x { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
        }
        MouseArea { anchors.fill: parent; anchors.margins: -4; cursorShape: Qt.PointingHandCursor; onClicked: tg.toggled(!tg.checked) }
    }
    component NfFlatButton: Rectangle {
        id: b
        property string label
        property bool red: false
        signal clicked()
        readonly property bool hov: bma.containsMouse
        width: bt.implicitWidth + Math.round(page.fs * 2)
        height: Math.round(page.fs * 2.5)
        radius: 4
        opacity: enabled ? 1 : 0.6
        color: red ? (hov ? "#C11119" : "#E50914") : (hov ? Qt.rgba(109/255, 109/255, 110/255, 0.4) : Qt.rgba(109/255, 109/255, 110/255, 0.7))
        Text { id: bt; anchors.centerIn: parent; text: b.label; color: "white"; font.family: Theme.font; font.pixelSize: page.fs; font.weight: Font.DemiBold }
        MouseArea { id: bma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; enabled: b.enabled; onClicked: b.clicked() }
    }
    component Spinner: Item {
        id: sp
        property real size: 24
        width: size; height: size
        Rectangle {
            anchors.fill: parent; radius: width / 2
            color: "transparent"; border.width: 2.5; border.color: "#333333"
        }
        Rectangle {
            width: sp.size * 0.5; height: sp.size * 0.5
            color: "transparent"
            clip: true
            Rectangle { width: sp.size; height: sp.size; radius: sp.size / 2; color: "transparent"; border.width: 2.5; border.color: "#E50914" }
        }
        RotationAnimation on rotation { from: 0; to: 360; duration: 900; loops: Animation.Infinite; running: sp.visible }
    }
}
