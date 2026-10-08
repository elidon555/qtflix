pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects
import QtFlix

// 16:9 Netflix boxart card (4px radius). Emits preview() after hovering 500 ms so the page can show the
// HoverPreview; the card itself never scales (the preview does). kind "top10" adds the red TOP 10 flag.
Item {
    id: root
    property var item: ({})                 // role map / delegate model object (id, title, backdropImage, progress, ...)
    property string kind: "normal"          // "continue" | "mylist" | "normal" | "grid" | "top10"
    property bool previewEnabled: true
    property int hoverDelay: 500
    signal clicked()
    signal preview(var item, rect globalRect)

    implicitWidth: 240
    width: implicitWidth
    height: Math.round(width * 9 / 16)

    readonly property bool hovered: ma.containsMouse
    readonly property bool hasItem: !!item && !!item.id
    readonly property real fs: Math.max(12, Math.min(19, width * 0.066))

    function globalRect() {
        const p = root.mapToItem(null, 0, 0)
        return Qt.rect(p.x, p.y, root.width, root.height)
    }
    Timer {
        id: hoverTimer
        interval: root.hoverDelay
        onTriggered: if (root.hovered && root.previewEnabled && root.hasItem) root.preview(root.item, root.globalRect())
    }
    onHoveredChanged: hovered ? hoverTimer.restart() : hoverTimer.stop()
    onPreviewEnabledChanged: if (!previewEnabled) hoverTimer.stop()

    Item {
        id: content
        anchors.fill: parent
        layer.enabled: true
        layer.smooth: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: mask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }

        Rectangle { anchors.fill: parent; color: "#2F2F2F" }
        Image {
            id: img
            anchors.fill: parent
            source: root.hasItem ? root.item.backdropImage : ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            cache: true
            sourceSize: Qt.size(Math.ceil(root.width), Math.ceil(root.height))
            opacity: status === Image.Ready ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 300 } }
        }
        // bottom scrim so the title reads on any frame
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            height: parent.height * 0.62
            gradient: Gradient {
                GradientStop { position: 0; color: "transparent" }
                GradientStop { position: 0.55; color: Qt.rgba(0, 0, 0, 0.35) }
                GradientStop { position: 1; color: Qt.rgba(0, 0, 0, 0.78) }
            }
        }
        // brand mark, top-left (only for titles with real metadata)
        Image {
            visible: !!(root.item && root.item.hasMeta)
            x: Math.round(root.width * 0.035); y: Math.round(root.width * 0.035)
            height: Math.round(root.width * 0.075); width: height
            source: "qrc:/assets/mark.svg"
            sourceSize: Qt.size(width, height)
        }
        // Netflix "TOP 10" corner flag (top-right)
        Rectangle {
            visible: root.kind === "top10"
            anchors.right: parent.right
            width: Math.round(root.width * 0.1); height: Math.round(width * 1.3)
            color: Theme.red
            Column {
                anchors.centerIn: parent
                spacing: -Math.round(root.width * 0.008)
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "TOP"; color: "white"; font.family: Theme.font; font.pixelSize: Math.max(6, Math.round(root.width * 0.026)); font.weight: Font.Bold }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "10"; color: "white"; font.family: Theme.font; font.pixelSize: Math.max(8, Math.round(root.width * 0.05)); font.weight: Font.Black; font.letterSpacing: -0.5 }
            }
        }
        Text {
            id: titleLabel
            anchors.left: parent.left; anchors.leftMargin: Math.round(root.width * 0.05)
            anchors.right: parent.right; anchors.rightMargin: Math.round(root.width * 0.05)
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Math.round(root.width * 0.045) + (root.kind === "continue" ? 6 : 0) + (recent.visible ? recent.height : 0)
            text: root.hasItem ? root.item.title : ""
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
        Rectangle {
            id: recent
            visible: root.kind !== "continue" && root.hasItem && Theme.isRecent(root.item.added)
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
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
        // continue watching: progress bar
        Item {
            visible: root.kind === "continue"
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: Math.round(root.width * 0.05); anchors.rightMargin: Math.round(root.width * 0.05)
            anchors.bottomMargin: Math.round(root.width * 0.035)
            height: 3
            Rectangle { anchors.fill: parent; color: Qt.rgba(1, 1, 1, 0.3) }
            Rectangle {
                height: parent.height
                width: parent.width * Math.max(0, Math.min(1, root.hasItem ? (root.item.progress || 0) : 0))
                color: Theme.red
            }
        }
        // continue watching: play circle on hover
        Rectangle {
            visible: root.kind === "continue"
            anchors.centerIn: parent
            width: Math.round(root.height * 0.36); height: width; radius: width / 2
            color: Qt.rgba(0, 0, 0, 0.5)
            border.width: 2; border.color: "white"
            opacity: root.hovered ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 200 } }
            Icon { anchors.centerIn: parent; anchors.horizontalCenterOffset: 2; name: "play"; size: parent.width * 0.45; color: "white" }
        }
    }
    Rectangle {
        id: mask
        anchors.fill: parent
        radius: 4
        visible: false
        layer.enabled: true
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
