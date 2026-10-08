pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// Netflix "Audio | Subtitles" popup: two columns, the current item has a check mark. A footer link
// opens the "Subtitle Appearance" page (size / style / background / colour) with a live preview.
PlayerPanel {
    id: p
    property var audioLabels: []        // ["English [Original]", "Spanish", ...]
    property int audioIndex: 0
    property var subtitleLabels: []     // ["Off", "English", ...]
    property int subtitleIndex: 0
    property bool embeddedSubtitleActive: false
    property var subtitleStyle: ({})     // {size, edge, background, color}
    property real maxHeight: 600
    signal audioSelected(int index)
    signal subtitleSelected(int index)
    signal styleOptionPicked(string key, string value)

    property string page: "tracks"       // "tracks" | "appearance"
    onOpenChanged: if (!open) page = "tracks"

    readonly property int pad: 24
    readonly property real colWidth: 280
    width: (audioLabels.length > 0 ? colWidth * 2 : colWidth) + 2 * pad
    height: Math.min(maxHeight, (page === "tracks" ? tracksPage.implicitHeight : appearancePage.implicitHeight) + 2 * pad)
    color: Qt.rgba(0.149, 0.149, 0.149, 0.97)
    clip: true

    component Header: Text {
        color: "white"
        font.family: Theme.font
        font.pixelSize: 24
        font.weight: Font.Bold
    }

    component Column_: Column {
        id: col
        property string header
        property var labels: []
        property int current: 0
        property real listMax: 400
        signal picked(int index)
        spacing: 12
        Header { text: col.header; leftPadding: 8 }
        ListView {
            id: lv
            width: col.width
            height: Math.min(contentHeight, col.listMax)
            clip: true
            interactive: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds
            keyNavigationEnabled: false
            model: col.labels
            delegate: Item {
                id: row
                required property int index
                required property var modelData
                readonly property bool selected: index === col.current
                width: lv.width
                height: 40
                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: "#333333"
                    visible: rowMa.containsMouse
                }
                Image {
                    visible: row.selected
                    x: 8; anchors.verticalCenter: parent.verticalCenter
                    width: 20; height: 20
                    source: "qrc:/assets/icons/player-check.svg"
                    sourceSize: Qt.size(40, 40)
                }
                Text {
                    x: 40; width: parent.width - x - 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData
                    elide: Text.ElideRight
                    color: (row.selected || rowMa.containsMouse) ? "white" : "#b3b3b3"
                    font.family: Theme.font
                    font.pixelSize: 16
                    font.weight: row.selected ? Font.DemiBold : Font.Normal
                }
                MouseArea {
                    id: rowMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: col.picked(row.index)
                }
            }
        }
    }

    // ---------------- page 1: tracks --------------------------------------------------------------
    Column {
        id: tracksPage
        x: p.pad; y: p.pad
        width: p.width - 2 * p.pad
        visible: p.page === "tracks"
        spacing: 12
        readonly property real listMax: Math.max(120, p.maxHeight - 2 * p.pad - 36 - 12 - 56)
        Row {
            Column_ {
                id: audioCol
                visible: p.audioLabels.length > 0
                width: p.colWidth; listMax: tracksPage.listMax
                header: qsTr("Audio")
                labels: p.audioLabels
                current: p.audioIndex
                onPicked: function(i) { p.audioSelected(i) }
            }
            Column_ {
                id: subCol
                width: p.colWidth; listMax: tracksPage.listMax
                header: qsTr("Subtitles")
                labels: p.subtitleLabels
                current: p.subtitleIndex
                onPicked: function(i) { p.subtitleSelected(i) }
            }
        }
        // footer: link to the appearance page (+ hint when an embedded track is active)
        Rectangle { width: parent.width; height: 1; color: "#404040" }
        Item {
            width: parent.width; height: 40
            Rectangle { anchors.fill: parent; radius: 4; color: "#333333"; visible: appMa.containsMouse }
            Image {
                x: 8; anchors.verticalCenter: parent.verticalCenter
                width: 20; height: 20
                source: "qrc:/assets/icons/player-settings.svg"
                sourceSize: Qt.size(40, 40)
            }
            Text {
                x: 40; anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Subtitle Appearance")
                color: appMa.containsMouse ? "white" : "#b3b3b3"
                font.family: Theme.font
                font.pixelSize: 16
            }
            Text {
                anchors.right: parent.right; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: p.embeddedSubtitleActive
                width: Math.min(implicitWidth, parent.width - 260)
                elide: Text.ElideRight
                text: qsTr("Embedded tracks keep their own style")
                color: "#808080"
                font.family: Theme.font
                font.pixelSize: 13
            }
            MouseArea {
                id: appMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: p.page = "appearance"
            }
        }
    }

    // ---------------- page 2: subtitle appearance -------------------------------------------------
    component Chip: Rectangle {
        id: chip
        property string label
        property bool selected: false
        signal clicked()
        width: chipText.implicitWidth + 28
        height: 32
        radius: 16
        color: selected ? "white" : (chipMa.containsMouse ? "#4d4d4d" : "#333333")
        Text {
            id: chipText
            anchors.centerIn: parent
            text: chip.label
            color: chip.selected ? "black" : "white"
            font.family: Theme.font
            font.pixelSize: 14
            font.weight: chip.selected ? Font.DemiBold : Font.Normal
        }
        MouseArea {
            id: chipMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.clicked()
        }
    }
    component OptionRow: Item {
        id: orow
        property string title
        property string key
        property var options: []        // [[value, label], ...]
        property string current
        signal picked(string key, string value)
        width: parent ? parent.width : 0
        height: Math.max(36, flow.implicitHeight)
        Text {
            width: 104
            y: 6
            text: orow.title
            color: "#b3b3b3"
            font.family: Theme.font
            font.pixelSize: 16
        }
        Flow {
            id: flow
            x: 112
            width: orow.width - x
            spacing: 8
            Repeater {
                model: orow.options
                delegate: Chip {
                    required property var modelData
                    label: modelData[1]
                    selected: orow.current === modelData[0]
                    onClicked: orow.picked(orow.key, modelData[0])
                }
            }
        }
    }

    Column {
        id: appearancePage
        x: p.pad; y: p.pad
        width: p.width - 2 * p.pad
        visible: p.page === "appearance"
        spacing: 16

        Item {
            width: parent.width; height: 32
            Row {
                spacing: 8
                anchors.verticalCenter: parent.verticalCenter
                Image {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24; height: 24
                    source: "qrc:/assets/icons/player-back.svg"
                    sourceSize: Qt.size(48, 48)
                    opacity: backMa.containsMouse ? 1 : 0.8
                }
                Header { text: qsTr("Subtitle Appearance") }
            }
            MouseArea {
                id: backMa
                width: 40; height: parent.height
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: p.page = "tracks"
            }
        }

        // live preview over a neutral "video" backdrop
        Rectangle {
            width: parent.width; height: 96
            radius: 4
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "#3a4a5a" }
                GradientStop { position: 1.0; color: "#7a6a52" }
            }
            PlayerSubtitleText {
                anchors.centerIn: parent
                text: qsTr("This is how subtitles will look.")
                spec: p.subtitleStyle
                basePixelSize: 26
                maxWidth: parent.width - 24
            }
        }

        OptionRow {
            title: qsTr("Size"); key: "size"; onPicked: function(k, v) { p.styleOptionPicked(k, v) }
            current: p.subtitleStyle.size || "medium"
            options: [["small", qsTr("Small")], ["medium", qsTr("Medium")], ["large", qsTr("Large")]]
        }
        OptionRow {
            title: qsTr("Style"); key: "edge"; onPicked: function(k, v) { p.styleOptionPicked(k, v) }
            current: p.subtitleStyle.edge || "shadow"
            options: [["shadow", qsTr("Drop shadow")], ["raised", qsTr("Raised")], ["depressed", qsTr("Depressed")],
                      ["outline", qsTr("Uniform outline")], ["none", qsTr("None")]]
        }
        OptionRow {
            title: qsTr("Background"); key: "background"; onPicked: function(k, v) { p.styleOptionPicked(k, v) }
            current: p.subtitleStyle.background || "none"
            options: [["none", qsTr("None")], ["box", qsTr("Semi-transparent box")]]
        }
        OptionRow {
            title: qsTr("Font color"); key: "color"; onPicked: function(k, v) { p.styleOptionPicked(k, v) }
            current: p.subtitleStyle.color || "white"
            options: [["white", qsTr("White")], ["yellow", qsTr("Yellow")]]
        }
        Text {
            width: parent.width
            visible: p.embeddedSubtitleActive
            wrapMode: Text.WordWrap
            text: qsTr("The active subtitle track is embedded in the video file and is drawn with the file's own style; these settings apply to external subtitle files.")
            color: "#808080"
            font.family: Theme.font
            font.pixelSize: 13
        }
    }
}
