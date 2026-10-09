pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// 16:9 Netflix boxart card (4px radius). Emits preview() after hovering 500 ms so the page can show the
// HoverPreview; the card itself never scales (the preview does). kind "top10" adds the red TOP 10 flag.
// The title data comes in as typed properties named after the TitleModel roles, so view delegates can bind
// them straight from the model (`required title` ...) and reuse cards (reuseItems) without any map lookups.
// Rarely shown parts (flag, ribbon, progress, play circle, brand mark) are Loaders: off cards don't build them.
Item {
    id: root
    property string titleId
    property string title
    property url backdropImage
    property bool hasMeta: false
    property bool isRecent: false
    property real progress: 0
    property string path
    property string kind: "normal"          // "continue" | "mylist" | "normal" | "grid" | "top10"
    property bool previewEnabled: true
    property int hoverDelay: 500
    signal clicked()
    signal preview(string id, rect globalRect)

    implicitWidth: 240
    width: implicitWidth
    height: Math.round(width * 9 / 16)

    readonly property bool hovered: ma.containsMouse
    readonly property bool hasItem: titleId !== ""
    readonly property real fs: Math.max(12, Math.min(19, width * 0.066))
    readonly property bool isContinue: kind === "continue"

    function globalRect(): rect {
        const p = root.mapToItem(null, 0, 0)
        return Qt.rect(p.x, p.y, root.width, root.height)
    }
    Timer {
        id: hoverTimer
        interval: root.hoverDelay
        onTriggered: if (root.hovered && root.previewEnabled && root.hasItem) root.preview(root.titleId, root.globalRect())
    }
    onHoveredChanged: hovered ? hoverTimer.restart() : hoverTimer.stop()
    onPreviewEnabledChanged: if (!previewEnabled) hoverTimer.stop()
    // view delegate recycling (reuseItems): a parked card must not fire a stale preview
    ListView.onPooled: hoverTimer.stop()
    GridView.onPooled: hoverTimer.stop()

    // While the hover delay runs, decode the backdrop at the HoverPreview's size so the preview opens sharp
    // (same url + sourceSize as HoverPreview's request -> pixmap cache hit).
    Loader {
        active: root.hovered && root.previewEnabled && root.hasItem
        sourceComponent: Image {
            visible: false
            asynchronous: true
            source: root.backdropImage
            sourceSize: Theme.thumbSize(Math.round(Math.max(300, root.width * 1.5)))
        }
    }

    RoundedImage {
        id: art
        anchors.fill: parent
        radius: 4
        source: root.hasItem ? root.backdropImage : ""
    }
    // bottom scrim so the title reads on any frame
    Rectangle {
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        height: parent.height * 0.62
        bottomLeftRadius: 4; bottomRightRadius: 4
        antialiasing: true
        gradient: Gradient {
            GradientStop { position: 0; color: "transparent" }
            GradientStop { position: 0.55; color: Qt.rgba(0, 0, 0, 0.35) }
            GradientStop { position: 1; color: Qt.rgba(0, 0, 0, 0.78) }
        }
    }
    // brand mark, top-left (only for titles with real metadata)
    Loader {
        active: root.hasMeta
        x: Math.round(root.width * 0.035); y: Math.round(root.width * 0.035)
        sourceComponent: Image {
            height: Math.round(root.width * 0.075); width: height
            source: "qrc:/assets/mark.svg"
            sourceSize: Qt.size(width, height)
        }
    }
    // Netflix "TOP 10" corner flag (top-right)
    Loader {
        active: root.kind === "top10"
        anchors.right: parent.right
        sourceComponent: Rectangle {
            width: Math.round(root.width * 0.1); height: Math.round(width * 1.3)
            topRightRadius: 4
            antialiasing: true
            color: Theme.red
            Column {
                anchors.centerIn: parent
                spacing: -Math.round(root.width * 0.008)
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "TOP"; color: "white"; font.family: Theme.font; font.pixelSize: Math.max(6, Math.round(root.width * 0.026)); font.weight: Font.Bold }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "10"; color: "white"; font.family: Theme.font; font.pixelSize: Math.max(8, Math.round(root.width * 0.05)); font.weight: Font.Black; font.letterSpacing: -0.5 }
            }
        }
    }
    Text {
        id: titleLabel
        anchors.left: parent.left; anchors.leftMargin: Math.round(root.width * 0.05)
        anchors.right: parent.right; anchors.rightMargin: Math.round(root.width * 0.05)
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.round(root.width * 0.045) + (root.isContinue ? 6 : 0) + (recent.active ? recent.height : 0)
        text: root.title
        color: "white"
        font.family: Theme.font
        font.pixelSize: Math.round(root.fs)
        font.weight: Font.Bold
        font.letterSpacing: -0.2
        lineHeight: 0.95
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
        style: Text.Raised
        styleColor: Qt.rgba(0, 0, 0, 0.35)
    }
    // "Recently added" ribbon, bottom center
    Loader {
        id: recent
        active: !root.isContinue && root.hasItem && root.isRecent
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        sourceComponent: Rectangle {
            width: recentText.implicitWidth + 12; height: recentText.implicitHeight + 4
            radius: 2
            color: Theme.red
            Text {
                id: recentText
                anchors.centerIn: parent
                text: "Recently added"
                color: "white"
                font.family: Theme.font; font.pixelSize: Math.max(10, Math.round(root.width * 0.045)); font.weight: Font.DemiBold
            }
        }
    }
    // continue watching: progress bar
    Loader {
        active: root.isContinue
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.leftMargin: Math.round(root.width * 0.05); anchors.rightMargin: Math.round(root.width * 0.05)
        anchors.bottomMargin: Math.round(root.width * 0.035)
        height: 3
        sourceComponent: Item {
            Rectangle { anchors.fill: parent; color: Qt.rgba(1, 1, 1, 0.3) }
            Rectangle {
                height: parent.height
                width: parent.width * Math.max(0, Math.min(1, root.progress))
                color: Theme.red
            }
        }
    }
    // continue watching: play circle on hover
    Loader {
        active: root.isContinue
        anchors.centerIn: parent
        sourceComponent: Rectangle {
            width: Math.round(root.height * 0.36); height: width; radius: width / 2
            color: Qt.rgba(0, 0, 0, 0.5)
            border.width: 2; border.color: "white"
            opacity: root.hovered ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 200 } }
            Icon { anchors.centerIn: parent; anchors.horizontalCenterOffset: 2; name: "play"; size: parent.width * 0.45; color: "white" }
        }
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
