pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtFlix

// Netflix player episode selector: "Season N" header and the season's episodes. One row is expanded
// (initially the playing one) with its still, synopsis and progress; clicking a collapsed row expands
// it, clicking an expanded row (or its still) plays it. Watched episodes get a check mark, partially
// watched ones a red progress bar.
PlayerPanel {
    id: p
    property int season: 1
    property var episodes: []            // Library.episodes(id, season)
    property string currentPath: ""
    property real maxHeight: 560
    signal episodePicked(string path)

    readonly property int pad: 24
    width: 620
    height: Math.min(pad + header.height + 8 + list.contentHeight + pad, maxHeight)
    color: Qt.rgba(0.149, 0.149, 0.149, 0.97)

    property int expandedIndex: -1
    function resetExpanded() {
        expandedIndex = -1
        for (var i = 0; i < episodes.length; ++i)
            if (episodes[i].path === currentPath) { expandedIndex = i; break }
    }
    onEpisodesChanged: resetExpanded()
    onCurrentPathChanged: resetExpanded()
    onOpenChanged: if (open) { resetExpanded(); list.positionCurrent() }

    function fmtMin(ms) { return ms > 0 ? Math.max(1, Math.round(ms / 60000)) + "m" : "" }

    Text {
        id: header
        x: p.pad + 8; y: p.pad
        text: qsTr("Season %1").arg(p.season)
        color: "white"
        font.family: Theme.font
        font.pixelSize: 24
        font.weight: Font.Bold
    }

    ListView {
        id: list
        x: p.pad
        y: header.y + header.height + 8
        width: p.width - 2 * p.pad
        height: p.height - y - p.pad
        clip: true
        spacing: 2
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: false
        model: p.episodes
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded; focusPolicy: Qt.NoFocus }
        onCountChanged: positionCurrent()
        function positionCurrent() { Qt.callLater(list.doPosition) }
        function doPosition() {
            if (contentHeight <= height + 1 || p.expandedIndex < 0) positionViewAtBeginning()
            else positionViewAtIndex(p.expandedIndex, ListView.Contain)
        }

        delegate: Item {
            id: row
            required property int index
            required property var modelData
            readonly property bool current: modelData.path === p.currentPath
            readonly property bool expanded: index === p.expandedIndex
            readonly property real progress: Math.min(1, modelData.progress || 0)
            readonly property bool watched: progress >= 0.95
            readonly property string desc: modelData.description || ""
            width: ListView.view.width
            height: expanded ? Math.max(56 + 90 + 16, 56 + descText.implicitHeight + 16) : 56

            Rectangle {
                anchors.fill: parent
                radius: 4
                color: rowMa.containsMouse ? "#333333" : (row.expanded ? "#2a2a2a" : "transparent")
            }

            Text {
                id: num
                x: 8; width: 32
                y: 16
                text: row.modelData.episode
                color: "white"
                font.family: Theme.font
                font.pixelSize: 16
                font.weight: Font.DemiBold
                font.features: { "tnum": 1 }
            }
            Text {
                x: 44
                y: 16
                width: row.width - x - 100
                text: row.modelData.title || qsTr("Episode %1").arg(row.modelData.episode)
                elide: Text.ElideRight
                color: "white"
                font.family: Theme.font
                font.pixelSize: 16
                font.weight: row.expanded ? Font.Bold : Font.Normal
            }
            // right side: check mark when watched, red progress bar when partially watched
            Image {
                visible: row.watched && !row.expanded
                anchors.right: parent.right; anchors.rightMargin: 12
                y: 18
                width: 20; height: 20
                source: "qrc:/assets/icons/player-check.svg"
                sourceSize: Qt.size(40, 40)
            }
            Rectangle {
                visible: !row.expanded && !row.watched && row.progress > 0.01
                anchors.right: parent.right; anchors.rightMargin: 12
                y: 26
                width: 64; height: 4; radius: 1; color: "#5b5b5b"
                Rectangle { width: parent.width * row.progress; height: parent.height; radius: 1; color: Theme.red }
            }
            Row {
                visible: row.expanded
                anchors.right: parent.right; anchors.rightMargin: 12
                y: 16
                spacing: 8
                Image {
                    visible: row.watched
                    width: 20; height: 20
                    source: "qrc:/assets/icons/player-check.svg"
                    sourceSize: Qt.size(40, 40)
                }
                Text {
                    text: p.fmtMin(row.modelData.durationMs)
                    color: "#b3b3b3"
                    font.family: Theme.font
                    font.pixelSize: 16
                }
            }

            // expanded content: still (with play glyph) + synopsis
            Item {
                id: still
                visible: row.expanded
                x: 44; y: 52
                width: 160; height: 90
                Rectangle { anchors.fill: parent; radius: 4; color: "#111" }
                Image {
                    anchors.fill: parent
                    source: row.expanded ? (row.modelData.still || row.modelData.thumb || "") : ""
                    sourceSize: Qt.size(320, 180)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 40; height: 40; radius: 20
                    visible: !row.current
                    color: Qt.rgba(0, 0, 0, rowMa.containsMouse ? 0.6 : 0.4)
                    border.color: "white"; border.width: 1
                    Image {
                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset: 1
                        width: 20; height: 20
                        source: "qrc:/assets/icons/player-play.svg"
                        sourceSize: Qt.size(40, 40)
                    }
                }
                Rectangle {
                    visible: row.progress > 0.01
                    anchors.bottom: parent.bottom
                    width: parent.width; height: 4; color: "#5b5b5b"
                    Rectangle { width: parent.width * row.progress; height: parent.height; color: Theme.red }
                }
            }
            Text {
                id: descText
                visible: row.expanded
                x: still.x + still.width + 16; y: 50
                width: row.width - x - 12
                text: row.desc !== "" ? row.desc : (row.current ? qsTr("Now playing") : "")
                color: "#d2d2d2"
                font.family: Theme.font
                font.pixelSize: 14
                lineHeight: 1.2
                wrapMode: Text.WordWrap
                maximumLineCount: 5
                elide: Text.ElideRight
            }

            MouseArea {
                id: rowMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (!row.expanded) { p.expandedIndex = row.index; return }
                    if (!row.current) p.episodePicked(row.modelData.path)
                }
            }
        }
    }
}
