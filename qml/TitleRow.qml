pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// One Netflix "lolomo" row: header (1.4vw, "Explore All ›" slides in on hover) + a paginated strip of
// 16:9 cards with gutter-wide paddles, 12x2 page indicators and a peeking next card in the right gutter.
// kind "top10" rows use the same landscape cards (max 10 items) with a small TOP 10 flag.
Item {
    id: root
    property alias model: list.model
    property string name
    property string kind: "normal"           // "continue" | "mylist" | "top10" | "normal"
    property real pad: Theme.gutter
    signal preview(var item, rect globalRect)
    signal exploreAll()

    readonly property int perPage: Theme.cardsPerPage(width)
    readonly property real avail: width - 2 * pad
    readonly property real spacing: Theme.cardGap
    readonly property real cardW: Math.floor((avail - (perPage - 1) * spacing) / perPage)
    readonly property real stride: cardW + spacing
    readonly property real cardH: Math.round(cardW * 9 / 16)
    readonly property int itemCount: kind === "top10" ? Math.min(10, list.count) : list.count
    readonly property int pageCount: Math.max(1, Math.ceil(itemCount / perPage))
    property int page: 0
    readonly property bool rowHovered: rowHover.hovered
    readonly property real headerH: header.height
    readonly property alias sliding: slide.running

    width: parent ? parent.width : 1600
    height: header.height + headerGap + cardH
    readonly property real headerGap: Math.round(titleText.font.pixelSize * 0.5)

    // The ListView spans the full row width (so peeking cards in the gutters aren't culled) with
    // left/right margins = gutter; contentX == -pad shows the first card at the gutter.
    function maxX() { return Math.max(0, itemCount * stride - spacing - avail) }
    function xForPage(p) { return -pad + Math.min(p * perPage * stride, maxX()) }
    function goPage(p) {
        p = Math.max(0, Math.min(pageCount - 1, p))
        if (p === page && !slide.running) return
        page = p
        slide.to = xForPage(p)
        slide.restart()
    }
    // deferred relayout (a Timer dies with the row, unlike Qt.callLater closures)
    Timer { id: relayout; interval: 0; onTriggered: { root.clampPage(); if (!slide.running) list.contentX = root.xForPage(root.page) } }
    onPerPageChanged: relayout.restart()
    onCardWChanged: { slide.stop(); list.contentX = xForPage(page) }
    function clampPage() { if (page > pageCount - 1) { page = Math.max(0, pageCount - 1); slide.stop(); list.contentX = xForPage(page) } }
    onPageCountChanged: relayout.restart()

    HoverHandler { id: rowHover }

    // ---------------- header ----------------
    Item {
        id: header
        x: root.pad
        width: headerRow.width
        height: titleText.implicitHeight
        HoverHandler { id: headerHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.exploreAll() }
        Row {
            id: headerRow
            spacing: 0
            Text {
                id: titleText
                text: root.name
                color: "#E5E5E5"
                font.family: Theme.font
                font.pixelSize: Theme.rowHeaderFont
                font.weight: Font.DemiBold
            }
            // "Explore All" slides out from behind the chevron
            Item {
                id: explore
                height: titleText.height
                width: headerHover.hovered ? exploreText.implicitWidth + Math.round(titleText.font.pixelSize * 0.6) : Math.round(titleText.font.pixelSize * 0.35)
                clip: true
                Behavior on width { NumberAnimation { duration: 750; easing.type: Easing.OutCubic } }
                Text {
                    id: exploreText
                    x: Math.round(titleText.font.pixelSize * 0.5) - (headerHover.hovered ? 0 : implicitWidth)
                    Behavior on x { NumberAnimation { duration: 750; easing.type: Easing.OutCubic } }
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Math.round(titleText.height * 0.14)
                    text: "Explore All"
                    color: Theme.cyan
                    font.family: Theme.font
                    font.pixelSize: Math.round(titleText.font.pixelSize * 0.6)
                    font.weight: Font.DemiBold
                    opacity: headerHover.hovered ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 400 } }
                }
            }
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: Math.round(titleText.height * 0.06)
                name: "chevronRight"
                size: Math.round(titleText.font.pixelSize * (headerHover.hovered ? 0.55 : 0.75))
                strokeWidth: 3
                color: Theme.cyan
                opacity: root.rowHovered ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 300 } }
                Behavior on size { NumberAnimation { duration: 750; easing.type: Easing.OutCubic } }
            }
        }
    }

    // page indicator (12x2 bars), top-right, aligned to the right gutter
    Row {
        id: indicator
        anchors.right: parent.right
        anchors.rightMargin: root.pad
        y: Math.round(header.height - height - root.headerGap * 0.2)
        spacing: 2
        visible: root.pageCount > 1
        opacity: root.rowHovered ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        Repeater {
            model: root.pageCount
            Rectangle {
                required property int index
                width: 12; height: 2
                color: index === root.page ? "#AAAAAA" : "#4D4D4D"
            }
        }
    }

    // ---------------- strip ----------------
    ListView {
        id: list
        x: 0
        y: header.height + root.headerGap
        width: root.width
        leftMargin: root.pad
        rightMargin: root.pad
        height: root.cardH
        orientation: ListView.Horizontal
        interactive: false
        spacing: root.spacing
        clip: false
        cacheBuffer: Math.max(0, Math.round(root.stride * 2))
        Component.onCompleted: contentX = root.xForPage(root.page)
        onCountChanged: relayout.restart()
        boundsBehavior: Flickable.StopAtBounds
        reuseItems: false
        delegate: TitleCard {
            id: card
            required property var model
            required property int index
            width: root.cardW
            item: model
            kind: root.kind
            visible: root.kind !== "top10" || index < 10
            previewEnabled: visible && root.inView(x, width)
            onClicked: root.activate(card.item)
            onPreview: (it, r) => root.preview(it, r)
        }

        NumberAnimation {
            id: slide
            target: list
            property: "contentX"
            duration: 750
            easing.type: Easing.Bezier
            easing.bezierCurve: Theme.easePage
        }
    }

    function inView(x, w) {
        const rel = x - (list.contentX + root.pad)
        return !slide.running && rel >= -2 && rel + w <= root.avail + 2
    }
    function activate(item) {
        if (!item) return
        if (root.kind === "continue") Nav.play(item.path)
        else Nav.openDetail(item.id)
    }

    // ---------------- paddles (gutter-wide, only while the row is hovered) ----------------
    Paddle {
        id: leftPaddle
        objectName: "leftPaddle"
        x: 0
        y: list.y
        width: root.pad - 2
        height: root.cardH
        isLeft: true
        shown: root.rowHovered && (root.page > 0 || (slide.running && list.contentX > -root.pad + 1))
        onClicked: root.goPage(root.page - 1)
    }
    Paddle {
        id: rightPaddle
        objectName: "rightPaddle"
        x: root.width - root.pad + 2
        y: list.y
        width: root.pad - 2
        height: root.cardH
        isLeft: false
        shown: root.rowHovered && root.page < root.pageCount - 1
        onClicked: root.goPage(root.page + 1)
    }
    readonly property alias rightPaddle: rightPaddle
    readonly property alias leftPaddle: leftPaddle

    component Paddle: Rectangle {
        id: pd
        property bool isLeft: true
        property bool shown: false
        signal clicked()
        readonly property bool hov: pma.containsMouse
        visible: shown || opacity > 0
        opacity: shown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        color: hov ? Qt.rgba(20/255, 20/255, 20/255, 0.7) : Qt.rgba(20/255, 20/255, 20/255, 0.5)
        radius: 0
        Icon {
            anchors.centerIn: parent
            name: pd.isLeft ? "chevronLeft" : "chevronRight"
            size: Math.round(Math.min(Theme.vw(2.5), pd.width * 0.9))
            strokeWidth: 2.6
            color: "white"
            scale: pd.hov ? 1.25 : 1
            Behavior on scale { NumberAnimation { duration: 100 } }
        }
        MouseArea { id: pma; anchors.fill: parent; enabled: pd.shown; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: pd.clicked() }
    }
}
