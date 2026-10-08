pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtFlix

// Netflix profile gate: "Who's watching?"
Rectangle {
    id: root
    color: Theme.bg
    signal chosen()

    readonly property real tile: Math.max(84, Math.min(200, width * 0.1))
    readonly property real gap: Math.round(width * 0.02)
    property bool managing: false
    property bool adding: false
    property int editIndex: -1               // >= 0: editing Theme.profiles[editIndex] (manage mode)
    focus: true
    opacity: 0
    scale: 1.1
    Component.onCompleted: { opacity = 1; scale = 1 }
    Behavior on opacity { NumberAnimation { duration: 450; easing.type: Easing.OutCubic } }
    Behavior on scale { NumberAnimation { duration: 450; easing.type: Easing.OutCubic } }

    function choose(p) {
        Theme.selectProfile(p)
        root.chosen()
    }

    // header with just the logo
    BrandLogo {
        x: Math.round(root.width * 0.04)
        y: 22
        logoHeight: Math.round(Math.max(24, Math.min(45, root.width * 0.02)))
    }

    Column {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -20
        spacing: 0
        visible: !root.adding

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.managing ? "Manage Profiles:" : "Who's watching?"
            color: "white"
            font.family: Theme.font
            font.pixelSize: Math.round(Math.max(32, root.width * 0.035))
            font.weight: Font.Normal
        }
        Item { width: 1; height: Math.round(root.tile * 0.2) + 12 }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: root.gap
            Repeater {
                model: Theme.profiles
                delegate: Item {
                    id: prof
                    required property var modelData
                    required property int index
                    width: root.tile
                    height: root.tile + nameText.height + root.tile * 0.08 + 6
                    readonly property bool hov: pma.containsMouse
                    Avatar {
                        id: av
                        width: root.tile; height: root.tile
                        avatarColor: prof.modelData.color
                        variant: prof.modelData.avatar
                        Rectangle {
                            anchors.fill: parent
                            color: "transparent"
                            radius: 4
                            border.width: prof.hov ? Math.max(3, Math.round(root.tile * 0.03)) : 0
                            border.color: "#E5E5E5"
                        }
                        // manage mode: dim + pencil
                        Rectangle {
                            anchors.fill: parent; radius: 4
                            color: Qt.rgba(0, 0, 0, 0.5)
                            visible: root.managing
                            Rectangle {
                                anchors.centerIn: parent
                                width: parent.width * 0.3; height: width; radius: width / 2
                                color: "transparent"; border.color: "white"; border.width: 2
                                Icon { anchors.centerIn: parent; name: "pencil"; size: parent.width * 0.55; color: "white" }
                            }
                        }
                    }
                    Text {
                        id: nameText
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: root.tile + Math.round(root.tile * 0.08)
                        text: prof.modelData.name
                        color: prof.hov ? "#E5E5E5" : "#808080"
                        font.family: Theme.font
                        font.pixelSize: Math.round(Math.max(14, root.width * 0.013))
                        width: root.tile + root.gap
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        id: pma
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (!root.managing) { root.choose(prof.modelData); return }
                            root.editIndex = prof.index
                            nameField.text = prof.modelData.name
                            root.adding = true
                            nameField.forceActiveFocus()
                        }
                    }
                }
            }
            // Add Profile
            Item {
                id: addTile
                width: root.tile
                height: root.tile + addName.height + root.tile * 0.08 + 6
                visible: Theme.profiles.length < 5
                readonly property bool hov: ama.containsMouse
                Rectangle {
                    width: root.tile; height: root.tile; radius: 4
                    color: addTile.hov ? "#E5E5E5" : "transparent"
                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.width * 0.5; height: width; radius: width / 2
                        color: addTile.hov ? "#808080" : "#808080"
                        Icon { anchors.centerIn: parent; name: "plus"; size: parent.width * 0.62; color: addTile.hov ? "#E5E5E5" : Theme.bg }
                    }
                }
                Text {
                    id: addName
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: root.tile + Math.round(root.tile * 0.08)
                    text: "Add Profile"
                    color: addTile.hov ? "#E5E5E5" : "#808080"
                    font.family: Theme.font
                    font.pixelSize: Math.round(Math.max(14, root.width * 0.013))
                }
                MouseArea {
                    id: ama
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { root.editIndex = -1; nameField.text = ""; root.adding = true; nameField.forceActiveFocus() }
                }
            }
        }
        Item { width: 1; height: Math.round(root.width * 0.035) }
        // MANAGE PROFILES / DONE
        Rectangle {
            id: manageBtn
            anchors.horizontalCenter: parent.horizontalCenter
            readonly property bool hov: mma.containsMouse
            readonly property real fs: Math.round(Math.max(13, root.width * 0.012))
            width: manageText.implicitWidth + fs * 3
            height: manageText.implicitHeight + fs
            color: root.managing ? (hov ? Theme.red : "white") : "transparent"
            border.width: root.managing ? 0 : 1
            border.color: hov ? "white" : "#808080"
            Text {
                id: manageText
                anchors.centerIn: parent
                text: root.managing ? "DONE" : "MANAGE PROFILES"
                color: root.managing ? (manageBtn.hov ? "white" : "black") : (manageBtn.hov ? "white" : "#808080")
                font.family: Theme.font
                font.pixelSize: manageBtn.fs
                font.weight: root.managing ? Font.Bold : Font.Normal
                font.letterSpacing: 2
            }
            MouseArea {
                id: mma
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.managing = !root.managing
            }
        }
    }

    // ---- Add Profile page ----
    Item {
        id: addPage
        anchors.fill: parent
        visible: root.adding
        opacity: root.adding ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        readonly property bool editing: root.editIndex >= 0 && root.editIndex < Theme.profiles.length
        readonly property var editing_p: editing ? Theme.profiles[root.editIndex] : null
        readonly property color newColor: editing ? editing_p.color : ["#0071EB", "#E50914", "#F5A623", "#1CE783", "#8C1AFF"][Theme.profiles.length % 5]
        readonly property int newAvatar: editing ? editing_p.avatar : Theme.profiles.length % 4
        function commit() {
            const n = nameField.text.trim()
            if (n === "") return
            const list = Theme.profiles.slice()
            if (editing) {
                const old = list[root.editIndex]
                list[root.editIndex] = { name: n, color: old.color, avatar: old.avatar }
                if (Nav.profileName === old.name) { Theme.setProfiles(list); Theme.selectProfile(list[root.editIndex]) }
            } else {
                list.push({ name: n, color: addPage.newColor, avatar: addPage.newAvatar })
            }
            Theme.setProfiles(list)
            root.adding = false
            root.forceActiveFocus()
        }
        function remove() {
            if (!editing || Theme.profiles.length <= 1) return
            const list = Theme.profiles.slice()
            const gone = list.splice(root.editIndex, 1)[0]
            Theme.setProfiles(list)
            if (Nav.profileName === gone.name) Theme.selectProfile(list[0])
            root.adding = false
            root.forceActiveFocus()
        }
        Column {
            anchors.centerIn: parent
            width: Math.min(root.width * 0.6, 760)
            spacing: 0
            Text {
                text: addPage.editing ? "Edit Profile" : "Add Profile"; color: "white"
                font.family: Theme.font; font.pixelSize: Math.round(Math.max(36, root.width * 0.04))
            }
            Text {
                text: addPage.editing ? "Change the name of this profile, or delete it." : "Add a profile for another person watching here."
                color: "#666666"
                font.family: Theme.font; font.pixelSize: Math.round(Math.max(14, root.width * 0.012))
                topPadding: 4; bottomPadding: 18
            }
            Rectangle { width: parent.width; height: 1; color: "#333333" }
            Row {
                topPadding: 24; bottomPadding: 24
                spacing: 24
                Avatar { width: Math.min(root.tile * 0.8, 140); height: width; avatarColor: addPage.newColor; variant: addPage.newAvatar }
                TextField {
                    id: nameField
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(root.width * 0.3, 420)
                    height: 44
                    placeholderText: "Name"
                    placeholderTextColor: "#999"
                    color: "white"
                    font.family: Theme.font; font.pixelSize: 18
                    leftPadding: 12
                    background: Rectangle { color: "#666666"; border.width: nameField.activeFocus ? 1 : 0; border.color: "white" }
                    Keys.onReturnPressed: addPage.commit()
                    Keys.onEscapePressed: { root.adding = false; root.forceActiveFocus() }
                }
            }
            Rectangle { width: parent.width; height: 1; color: "#333333" }
            Row {
                topPadding: 32
                spacing: 20
                Rectangle {
                    id: contBtn
                    readonly property bool hov: cma.containsMouse
                    width: contText.implicitWidth + 48; height: contText.implicitHeight + 18
                    color: hov ? Theme.red : "white"
                    Text {
                        id: contText; anchors.centerIn: parent; text: addPage.editing ? "SAVE" : "CONTINUE"
                        color: contBtn.hov ? "white" : "black"
                        font.family: Theme.font; font.pixelSize: 16; font.weight: Font.Bold; font.letterSpacing: 2
                    }
                    MouseArea { id: cma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: addPage.commit() }
                }
                Rectangle {
                    id: cancelBtn
                    readonly property bool hov: xma.containsMouse
                    width: cancelText.implicitWidth + 48; height: cancelText.implicitHeight + 18
                    color: "transparent"; border.width: 1; border.color: hov ? "white" : "#808080"
                    Text {
                        id: cancelText; anchors.centerIn: parent; text: "CANCEL"
                        color: cancelBtn.hov ? "white" : "#808080"
                        font.family: Theme.font; font.pixelSize: 16; font.letterSpacing: 2
                    }
                    MouseArea { id: xma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { root.adding = false; root.forceActiveFocus() } }
                }
                Rectangle {
                    id: delBtn
                    visible: addPage.editing && Theme.profiles.length > 1
                    readonly property bool hov: dma.containsMouse
                    width: delText.implicitWidth + 48; height: delText.implicitHeight + 18
                    color: "transparent"; border.width: 1; border.color: hov ? "white" : "#808080"
                    Text {
                        id: delText; anchors.centerIn: parent; text: "DELETE PROFILE"
                        color: delBtn.hov ? "white" : "#808080"
                        font.family: Theme.font; font.pixelSize: 16; font.letterSpacing: 2
                    }
                    MouseArea { id: dma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: addPage.remove() }
                }
            }
        }
    }
}
