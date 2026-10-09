pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtFlix

// Netflix top navigation: gradient when at top, solid #141414 once scrolled.
Item {
    id: root
    property real scrollY: 0
    readonly property bool solid: scrollY > 10
    readonly property real pad: Theme.gutter
    readonly property real fs: Theme.navFont
    readonly property real iconSize: Math.round(fs * 1.7)
    property alias menuOpen: profile.menuOpen      // account dropdown state (hover-driven)
    property alias notificationsOpen: bell.menuOpen
    height: Theme.navH

    // NavBar is always present and spans the full window width: it feeds Theme.vw().
    Binding { target: Theme; property: "windowWidth"; value: root.width; when: root.width > 0 }
    anchors.left: parent ? parent.left : undefined
    anchors.right: parent ? parent.right : undefined
    anchors.top: parent ? parent.top : undefined
    z: 100

    // ---- background ----
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.1; color: Qt.rgba(0, 0, 0, 0.7) }
            GradientStop { position: 1.0; color: "transparent" }
        }
    }
    Rectangle {
        anchors.fill: parent
        color: Theme.navBg
        opacity: root.solid ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 400 } }
    }

    // ---- left: logo + primary navigation ----
    Row {
        id: left
        x: root.pad
        anchors.verticalCenter: parent.verticalCenter
        spacing: 0
        BrandLogo {
            id: logo
            anchors.verticalCenter: parent.verticalCenter
            logoHeight: iconOnly ? Math.round(Theme.logoH * 1.2) : Theme.logoH
            iconOnly: root.width < 950
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { Library.searchQuery = ""; Nav.go("home") } }
        }
        Item { width: Math.round(root.fs * 3.2); height: 1 }
        Repeater {
            model: [ { label: "Home", page: "home" }, { label: "TV Shows", page: "tv" }, { label: "Movies", page: "movies" },
                     { label: "New & Popular", page: "new" }, { label: "My List", page: "mylist" } ]
            delegate: Text {
                id: link
                objectName: "navLink_" + modelData.page
                required property var modelData
                required property int index
                readonly property bool active: Nav.page === modelData.page
                anchors.verticalCenter: parent.verticalCenter
                leftPadding: index === 0 ? 0 : Math.round(root.fs * 1.43)
                text: modelData.label
                font.family: Theme.font
                font.pixelSize: root.fs
                font.weight: active ? Font.Bold : Font.Normal
                color: active ? "white" : (lma.containsMouse ? "#B3B3B3" : "#E5E5E5")
                Behavior on color { ColorAnimation { duration: 400 } }
                MouseArea {
                    id: lma
                    anchors.fill: parent
                    anchors.leftMargin: link.leftPadding
                    hoverEnabled: true
                    cursorShape: link.active ? Qt.ArrowCursor : Qt.PointingHandCursor
                    onClicked: { Library.searchQuery = ""; Nav.go(link.modelData.page) }
                }
            }
        }
    }

    // ---- right: search, bell, profile ----
    Row {
        id: right
        anchors.right: parent.right
        anchors.rightMargin: root.pad
        anchors.verticalCenter: parent.verticalCenter
        spacing: Math.round(root.fs * 1.55)
        layoutDirection: Qt.LeftToRight

        // search box
        Item {
            id: search
            objectName: "searchBox"
            anchors.verticalCenter: parent.verticalCenter
            property bool open: false
            readonly property real openWidth: Math.round(root.fs * 20)
            width: open ? openWidth : root.iconSize + 2
            height: Math.round(root.fs * 2.55)
            Behavior on width { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
            function openBox() { open = true; input.forceActiveFocus() }
            function closeBox() {
                input.text = ""
                open = false
                input.focus = false
            }
            Connections {
                target: Library
                function onSearchQueryChanged() {
                    if (input.text !== Library.searchQuery) {
                        input.text = Library.searchQuery
                        if (Library.searchQuery !== "") search.open = true
                    }
                }
            }
            Connections {
                target: Nav
                function onPageChanged() {
                    if (Nav.page !== "search" && search.open && input.text !== "") { input.text = ""; search.open = false }
                    // the page Loader focuses the freshly loaded page; keep typing in the search box
                    if (Nav.page === "search" && search.open) refocus.restart()
                }
            }
            Timer { id: refocus; interval: 0; onTriggered: if (search.open) input.forceActiveFocus() }
            Timer { id: searchDebounce; interval: 150; onTriggered: search.applyQuery() }
            function applyQuery() {
                searchDebounce.stop()
                const text = input.text
                Library.searchQuery = text
                if (text.trim() !== "") { if (Nav.page !== "search") Nav.go("search") }
                else if (Nav.page === "search") Nav.go(Nav.previousPage === "search" ? "home" : Nav.previousPage)
            }
            Rectangle {
                anchors.fill: parent
                color: Qt.rgba(0, 0, 0, 0.75)
                border.color: "white"
                border.width: 1
                opacity: search.open ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 150 } }
            }
            Icon {
                id: searchIcon
                x: search.open ? 9 : 0
                Behavior on x { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
                anchors.verticalCenter: parent.verticalCenter
                name: "search"; size: root.iconSize; color: "white"; strokeWidth: 2.1
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: search.open ? input.forceActiveFocus() : search.openBox()
                }
            }
            TextInput {
                id: input
                anchors.left: searchIcon.right
                anchors.leftMargin: 8
                anchors.right: clearBtn.left
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                visible: search.width > 80
                clip: true
                color: "white"
                selectionColor: "#4A4A4A"
                font.family: Theme.font
                font.pixelSize: root.fs
                selectByMouse: true
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Titles, people, genres"
                    color: "#8C8C8C"
                    font: input.font
                    visible: input.text === ""
                }
                // typing is debounced (one search + results relayout per pause, not per key); clearing is immediate
                onTextChanged: { if (text === "") search.applyQuery(); else searchDebounce.restart() }
                onActiveFocusChanged: if (!activeFocus && text === "") search.open = false
                Keys.onEscapePressed: { search.closeBox() }
            }
            Item {
                id: clearBtn
                width: input.text !== "" ? 24 : 0
                height: 24
                anchors.right: parent.right
                anchors.rightMargin: input.text !== "" ? 8 : 0
                anchors.verticalCenter: parent.verticalCenter
                visible: input.text !== "" && search.open
                Icon { anchors.centerIn: parent; name: "close"; size: 16; color: "white" }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { input.text = ""; input.forceActiveFocus() } }
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Kids"
            font.family: Theme.font; font.pixelSize: root.fs
            color: kma.containsMouse ? "#B3B3B3" : "white"
            Behavior on color { ColorAnimation { duration: 400 } }
            MouseArea { id: kma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: Nav.go("profiles") }
        }

        // bell with notification badge (hover opens the Notifications panel)
        Item {
            id: bell
            objectName: "bell"
            anchors.verticalCenter: parent.verticalCenter
            width: root.iconSize; height: root.iconSize
            readonly property bool hovered: bma.containsMouse || notifHover.hovered
            property bool menuOpen: false
            onHoveredChanged: {
                if (hovered) { bellClose.stop(); bellOpen.restart() }
                else { bellOpen.stop(); bellClose.restart() }
            }
            Timer { id: bellOpen; interval: 120; onTriggered: { notif.refresh(); bell.menuOpen = true } }
            Timer { id: bellClose; interval: 300; onTriggered: bell.menuOpen = false }
            Icon { anchors.centerIn: parent; name: "bell"; size: root.iconSize; color: "white"; strokeWidth: 1.9 }
            Rectangle {
                x: Math.round(parent.width * 0.55); y: -Math.round(parent.height * 0.12)
                width: Math.max(Math.round(root.fs * 1.15), badgeText.implicitWidth + 8); height: Math.round(root.fs * 1.08); radius: height / 2
                color: Theme.red
                visible: notif.newCount > 0
                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: notif.newCount > 9 ? "9+" : notif.newCount
                    color: "white"; font.family: Theme.font; font.pixelSize: Math.round(root.fs * 0.72); font.weight: Font.Bold
                }
            }
            MouseArea { id: bma; anchors.fill: parent; anchors.margins: -8; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: { notif.refresh(); bell.menuOpen = !bell.menuOpen } }
        }

        // profile + caret
        Item {
            id: profile
            anchors.verticalCenter: parent.verticalCenter
            readonly property real av: Math.round(root.fs * 2.3)
            width: av + Math.round(av * 0.62)
            height: av
            readonly property bool hovered: pma.containsMouse || menuHover.hovered
            property bool menuOpen: false
            onHoveredChanged: {
                if (hovered) { closeTimer.stop(); openTimer.restart() }
                else { openTimer.stop(); closeTimer.restart() }
            }
            Timer { id: openTimer; interval: 120; onTriggered: profile.menuOpen = true }
            Timer { id: closeTimer; interval: 300; onTriggered: profile.menuOpen = false }

            Avatar {
                width: profile.av; height: profile.av
                avatarColor: Nav.profileColor
                variant: Nav.profileAvatar
            }
            Icon {
                x: profile.av + Math.round(profile.av * 0.3); anchors.verticalCenter: parent.verticalCenter
                name: "caretDown"; size: Math.round(root.fs * 0.86); color: "white"
                rotation: profile.menuOpen ? 180 : 0
                Behavior on rotation { NumberAnimation { duration: 367; easing.type: Easing.Bezier; easing.bezierCurve: [0.21, 0, 0.07, 1, 1, 1] } }
            }
            MouseArea { id: pma; anchors.fill: parent; anchors.margins: -10; hoverEnabled: true; cursorShape: Qt.PointingHandCursor }
        }
    }

    // ---- account dropdown ----
    Item {
        id: menu
        visible: opacity > 0
        opacity: profile.menuOpen ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        width: 220
        height: menuCol.height + 2
        anchors.right: parent.right
        anchors.rightMargin: root.pad - 4
        y: root.height - 2
        z: 200

        HoverHandler { id: menuHover; enabled: menu.visible }

        // callout arrow above the menu, pointing at the caret
        Icon {
            name: "caretDown"; size: 18; color: "#E5E5E5"; rotation: 180
            x: menu.width - 40; y: -11
        }
        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.9)
            border.width: 1
            border.color: Qt.rgba(1, 1, 1, 0.15)
        }
        Rectangle { width: parent.width; height: 2; color: "#E5E5E5" }

        Column {
            id: menuCol
            y: 2
            width: parent.width
            topPadding: 10
            Repeater {
                model: Theme.profiles
                delegate: MenuItemRow {
                    required property var modelData
                    label: modelData.name
                    avatarColor: modelData.color
                    avatarVariant: modelData.avatar
                    onActivated: { Theme.selectProfile(modelData); profile.menuOpen = false }
                }
            }
            MenuItemRow { label: "Manage Profiles"; iconName: "pencil"; onActivated: { profile.menuOpen = false; Nav.go("profiles") } }
            MenuItemRow { label: "Settings"; iconName: "settings"; onActivated: { profile.menuOpen = false; Nav.go("settings") } }
            MenuItemRow { label: Library.scanning ? "Scanning…" : "Rescan library"; iconName: "replay"; onActivated: { profile.menuOpen = false; Library.rescan() } }
            Item { width: 1; height: 10 }
            Rectangle { width: parent.width; height: 1; color: Qt.rgba(1, 1, 1, 0.25) }
            Item {
                width: parent.width; height: 44
                Text {
                    anchors.centerIn: parent
                    text: "Switch profiles"
                    color: "white"
                    font.family: Theme.font; font.pixelSize: 13
                    font.underline: swma.containsMouse
                }
                MouseArea { id: swma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { profile.menuOpen = false; Nav.go("profiles") } }
            }
        }
    }

    // ---- notifications panel: the 5 most recently added titles ----
    Item {
        id: notif
        visible: opacity > 0
        opacity: bell.menuOpen ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        readonly property real w: Math.round(Math.max(380, root.fs * 28))
        width: w
        height: notifCol.height + 2
        z: 200
        y: root.height - 2
        property real bellX: 0                  // bell centre in NavBar coords (refreshed on open)
        x: Math.min(root.width - root.pad - w + 10, bellX - w + 60)
        property var items: []
        property int newCount: 0
        function refresh() {
            bellX = bell.mapToItem(root, bell.width / 2, 0).x
            items = Library.recentTitles(5)          // newest first
            newCount = Library.recentCount()         // added within the last 7 days
        }
        Connections { target: Library; function onLibraryChanged() { notif.refresh() } }
        Component.onCompleted: refresh()

        HoverHandler { id: notifHover; enabled: notif.visible }
        Icon {
            name: "caretDown"; size: 18; color: "#E5E5E5"; rotation: 180
            x: notif.bellX - notif.x - 9; y: -11
        }
        Rectangle { anchors.fill: parent; color: Qt.rgba(0, 0, 0, 0.9); border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.15) }
        Rectangle { width: parent.width; height: 2; color: "#E5E5E5" }
        Column {
            id: notifCol
            y: 2
            width: parent.width
            Text {
                visible: notif.items.length === 0
                width: parent.width; padding: 20
                text: "No recent notifications"
                color: "#B3B3B3"; font.family: Theme.font; font.pixelSize: root.fs
            }
            Repeater {
                model: notif.items
                delegate: Item {
                    id: nr
                    required property var modelData
                    required property int index
                    width: notifCol.width
                    height: Math.round(thumb.height + 32)
                    Rectangle { anchors.fill: parent; color: nma.containsMouse ? Qt.rgba(1, 1, 1, 0.06) : "transparent" }
                    Rectangle { visible: nr.index > 0; width: parent.width; height: 1; color: "#4D4D4D" }
                    Image {
                        id: thumb
                        x: 16; anchors.verticalCenter: parent.verticalCenter
                        width: Math.round(root.fs * 8); height: Math.round(width * 9 / 16)
                        source: nr.modelData.backdropImage
                        sourceSize: Theme.thumbSize(width * 2)
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true; cache: true
                        Rectangle { anchors.fill: parent; color: "#2F2F2F"; visible: parent.status !== Image.Ready; z: -1 }
                    }
                    Column {
                        anchors.left: thumb.right; anchors.leftMargin: 16
                        anchors.right: parent.right; anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Text { text: "New Arrival"; color: "white"; font.family: Theme.font; font.pixelSize: root.fs; font.weight: Font.Medium }
                        Text { width: parent.width; text: nr.modelData.title; color: "#B3B3B3"; font.family: Theme.font; font.pixelSize: root.fs; elide: Text.ElideRight }
                        Text { text: Theme.relativeDate(nr.modelData.added); color: "#808080"; font.family: Theme.font; font.pixelSize: Math.round(root.fs * 0.86) }
                    }
                    MouseArea { id: nma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: { bell.menuOpen = false; Nav.openDetail(nr.modelData.id) } }
                }
            }
        }
    }

    component MenuItemRow: Item {
        id: mi
        property string label
        property string iconName: ""
        property color avatarColor: "transparent"
        property int avatarVariant: 0
        signal activated()
        width: 220; height: 42
        Row {
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10
            Item {
                width: 32; height: 32
                Avatar { anchors.fill: parent; visible: mi.iconName === ""; avatarColor: mi.avatarColor; variant: mi.avatarVariant }
                Icon {
                    anchors.centerIn: parent
                    visible: mi.iconName !== "" && mi.iconName !== "settings"
                    name: mi.iconName; size: 22; color: "#B3B3B3"; strokeWidth: 1.8
                }
                // gear for settings
                Text {
                    anchors.centerIn: parent
                    visible: mi.iconName === "settings"
                    text: "⚙"; color: "#B3B3B3"; font.pixelSize: 22
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: mi.label
                color: "white"
                font.family: Theme.font; font.pixelSize: 13
                font.underline: mma.containsMouse
            }
        }
        MouseArea { id: mma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: mi.activated() }
    }
}
