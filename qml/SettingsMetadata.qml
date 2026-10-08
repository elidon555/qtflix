pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtFlix

// "Metadata (TMDB)" settings section: API key, sync status, refresh/clear, and a manual "Fix a match" tool.
// Self-contained (only depends on Theme and the Tmdb / Library context properties). Public API: width.
Item {
    id: root
    implicitHeight: col.implicitHeight
    height: implicitHeight

    readonly property real fs: Math.round(Theme.clamp(Theme.vw(1.0), 15, 22))   // 16px at the default window size
    readonly property color warn: "#E87C03"                                    // Netflix form error orange

    // ---- "Fix a match" state ----
    property var titleItems: []          // [{id, label, state, query, year, series}]
    property string selectedId: ""
    property var results: []
    property int selectedResult: -1
    property bool searching: false
    property bool searchSeries: false
    property string fixNote: ""
    property bool userPicked: false      // until the user picks a title, follow the most relevant one

    function stateLabel(s) {
        switch (s) {
        case "matched": return "matched"
        case "notFound": return "not found"
        case "skipped": return "skipped (recording)"
        case "ignored": return "ignored"
        case "pending": return "fetching…"
        default: return "not fetched"
        }
    }
    function rebuildTitles() {
        const all = Library.allTitles
        const out = []
        const rank = { notFound: 0, new: 1, pending: 2, matched: 3, ignored: 4, skipped: 5 }
        for (let i = 0; all && i < all.count; ++i) {
            const t = all.get(i)
            const st = Tmdb.titleState(t.id)
            out.push({ id: t.id, state: st.state, query: st.query, year: st.year, series: st.series,
                       label: (st.query || t.title) + (st.year > 0 ? " (" + st.year + ")" : "") + "  ·  " + stateLabel(st.state) })
        }
        out.sort((a, b) => (rank[a.state] ?? 9) - (rank[b.state] ?? 9) || a.label.localeCompare(b.label))
        titleItems = out
        let idx = out.findIndex(x => x.id === selectedId)
        if ((idx < 0 || !userPicked) && out.length > 0 && idx !== 0) { idx = 0; selectTitle(out[0]) }
        titleCombo.currentIndex = idx
    }
    function selectTitle(item) {
        if (!item) return
        if (item.id !== selectedId) {
            results = []; selectedResult = -1; fixNote = ""
            queryField.text = item.query + (item.year > 0 ? " " + item.year : "")
            queryField.cursorPosition = 0
            searchSeries = item.series
        }
        selectedId = item.id
        const i = titleItems.findIndex(x => x.id === item.id)
        if (i >= 0 && titleCombo.currentIndex !== i) titleCombo.currentIndex = i
    }
    function runSearch() {
        if (queryField.text.trim() === "") return
        searching = true; results = []; selectedResult = -1; fixNote = ""
        Tmdb.search(queryField.text, searchSeries)
    }

    Component.onCompleted: rebuildTitles()
    Connections { target: Library; function onLibraryChanged() { root.rebuildTitles() } }
    Connections { target: Tmdb; function onStatsChanged() { root.rebuildTitles() } }
    Connections {
        target: Tmdb
        function onSearchResults(list) {
            root.searching = false
            root.results = list
            root.selectedResult = list.length > 0 ? 0 : -1
            if (list.length === 0) root.fixNote = Tmdb.enabled ? "No results. Try a shorter title or switch Movie / Series."
                                                              : "Add an API key first."
        }
    }

    Column {
        id: col
        width: parent.width
        spacing: Math.round(root.fs * 0.9)
        topPadding: Math.round(root.fs * 1.2)

        // ---- header ----
        Text {
            text: "Metadata (TMDB)"
            color: "white"; font.family: Theme.font; font.pixelSize: root.fs; font.weight: Font.DemiBold
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            textFormat: Text.StyledText
            color: "#A3A3A3"; font.family: Theme.font; font.pixelSize: Math.round(root.fs * 0.88)
            linkColor: "white"
            text: "Real posters, backdrops, title logos, synopses and episode stills from The Movie Database. "
                  + "Get a free API key (v3 key or v4 read access token) at "
                  + "<a href=\"https://www.themoviedb.org/settings/api\">themoviedb.org/settings/api</a>. "
                  + "<br><br>This product uses the TMDB API but is not endorsed or certified by TMDB."
            onLinkActivated: link => Qt.openUrlExternally(link)
            HoverHandler { cursorShape: parent.hoveredLink ? Qt.PointingHandCursor : Qt.ArrowCursor }
        }

        // ---- API key ----
        Row {
            spacing: Math.round(root.fs * 0.75)
            TextField {
                id: keyField
                property bool reveal: false
                width: Math.min(root.width, Math.round(root.fs * 30))
                height: Math.round(root.fs * 3.1)
                text: Tmdb.apiKey
                placeholderText: "TMDB API key"
                placeholderTextColor: "#8C8C8C"
                echoMode: reveal ? TextInput.Normal : TextInput.Password
                color: "white"; selectionColor: Theme.red; selectedTextColor: "white"
                font.family: Theme.font; font.pixelSize: root.fs
                leftPadding: Math.round(root.fs * 0.9); rightPadding: showBtn.width + Math.round(root.fs * 1.2)
                verticalAlignment: TextInput.AlignVCenter
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                onEditingFinished: Tmdb.apiKey = text
                background: Rectangle {
                    radius: 4
                    color: "#333333"
                    border.width: keyField.activeFocus ? 2 : 1
                    border.color: Tmdb.status === "Invalid API key" ? root.warn
                                 : keyField.activeFocus ? "white" : "#5A5A5A"
                }
                Text {
                    id: showBtn
                    anchors.right: parent.right; anchors.rightMargin: Math.round(root.fs * 0.9)
                    anchors.verticalCenter: parent.verticalCenter
                    text: keyField.reveal ? "HIDE" : "SHOW"
                    color: showMa.containsMouse ? "white" : "#B3B3B3"
                    font.family: Theme.font; font.pixelSize: Math.round(root.fs * 0.8); font.weight: Font.DemiBold
                    font.letterSpacing: 0.5
                    MouseArea {
                        id: showMa; anchors.fill: parent; anchors.margins: -6
                        hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: keyField.reveal = !keyField.reveal
                    }
                }
            }
            FlatButton {
                anchors.verticalCenter: parent.verticalCenter
                label: "Save"
                visible: keyField.text.trim() !== Tmdb.apiKey
                onClicked: Tmdb.apiKey = keyField.text
            }
        }

        // ---- status ----
        Row {
            spacing: Math.round(root.fs * 0.6)
            height: Math.round(root.fs * 1.5)
            Spinner {
                anchors.verticalCenter: parent.verticalCenter
                visible: Tmdb.busy
                size: Math.round(root.fs * 1.1)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: Tmdb.status
                color: (Tmdb.status === "Invalid API key" || Tmdb.status.indexOf("No network") === 0) ? root.warn : "white"
                font.family: Theme.font; font.pixelSize: root.fs; font.weight: Font.Medium
            }
        }

        Row {
            spacing: Math.round(root.fs * 0.75)
            FlatButton {
                label: "Refresh all"; red: true
                enabled: Tmdb.enabled
                onClicked: Tmdb.refreshAll()
            }
            FlatButton {
                label: "Clear cache"
                onClicked: Tmdb.clearCache()
            }
        }

        // ---- Fix a match ----
        Rectangle { width: parent.width; height: 1; color: "#333333" }
        Text {
            text: "Fix a match"
            color: "white"; font.family: Theme.font; font.pixelSize: root.fs; font.weight: Font.DemiBold
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            color: "#A3A3A3"; font.family: Theme.font; font.pixelSize: Math.round(root.fs * 0.88)
            text: "Pick a title, search TMDB, and choose the right entry. Your choice is kept across refreshes."
        }

        ComboBox {
            id: titleCombo
            width: Math.min(root.width, Math.round(root.fs * 30))
            height: Math.round(root.fs * 3.1)
            model: root.titleItems
            textRole: "label"
            font.family: Theme.font; font.pixelSize: root.fs
            onActivated: index => { root.userPicked = true; root.selectTitle(root.titleItems[index]) }
            contentItem: Text {
                leftPadding: Math.round(root.fs * 0.9)
                rightPadding: titleCombo.indicator.width + Math.round(root.fs)
                text: titleCombo.displayText
                color: "white"; font: titleCombo.font
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
            indicator: Canvas {
                x: titleCombo.width - width - Math.round(root.fs * 0.9)
                y: (titleCombo.height - height) / 2
                width: Math.round(root.fs * 0.75); height: Math.round(width * 0.55)
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.fillStyle = "white"
                    ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width / 2, height); ctx.closePath()
                    ctx.fill()
                }
            }
            background: Rectangle {
                radius: 4
                color: "#333333"
                border.width: titleCombo.visualFocus || titleCombo.popup.visible ? 2 : 1
                border.color: titleCombo.visualFocus || titleCombo.popup.visible ? "white" : "#5A5A5A"
            }
            delegate: ItemDelegate {
                id: dlg
                required property var modelData
                required property int index
                width: titleCombo.width
                height: Math.round(root.fs * 2.6)
                highlighted: titleCombo.highlightedIndex === index
                contentItem: Row {
                    spacing: Math.round(root.fs * 0.5)
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 8; height: 8; radius: 4
                        color: dlg.modelData.state === "matched" ? Theme.green
                             : dlg.modelData.state === "notFound" ? root.warn : "#5A5A5A"
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: titleCombo.width - Math.round(root.fs * 3)
                        text: dlg.modelData.label
                        color: "white"; font.family: Theme.font; font.pixelSize: Math.round(root.fs * 0.94)
                        elide: Text.ElideRight
                    }
                }
                background: Rectangle { color: dlg.highlighted ? "#404040" : "transparent" }
            }
            popup: Popup {
                y: titleCombo.height + 2
                width: titleCombo.width
                height: Math.min(contentItem.implicitHeight + 2, Math.round(root.fs * 2.6) * 10 + 2)
                padding: 1
                contentItem: ListView {
                    clip: true
                    implicitHeight: contentHeight
                    model: titleCombo.popup.visible ? titleCombo.delegateModel : null
                    currentIndex: titleCombo.highlightedIndex
                    ScrollIndicator.vertical: ScrollIndicator {}
                }
                background: Rectangle { color: "#141414"; border.width: 1; border.color: "#5A5A5A"; radius: 4 }
            }
        }

        Row {
            spacing: Math.round(root.fs * 0.75)
            TextField {
                id: queryField
                width: Math.min(root.width - segment.width - searchBtn.width - 2 * parent.spacing, Math.round(root.fs * 22))
                height: Math.round(root.fs * 3.1)
                placeholderText: "Search TMDB (title and optional year)"
                placeholderTextColor: "#8C8C8C"
                color: "white"; selectionColor: Theme.red; selectedTextColor: "white"
                font.family: Theme.font; font.pixelSize: root.fs
                leftPadding: Math.round(root.fs * 0.9)
                verticalAlignment: TextInput.AlignVCenter
                onAccepted: root.runSearch()
                background: Rectangle {
                    radius: 4; color: "#333333"
                    border.width: queryField.activeFocus ? 2 : 1
                    border.color: queryField.activeFocus ? "white" : "#5A5A5A"
                }
            }
            // Movie | Series segmented switch
            Rectangle {
                id: segment
                anchors.verticalCenter: parent.verticalCenter
                width: segRow.width + 4; height: Math.round(root.fs * 2.5)
                radius: 4; color: "#333333"
                Row {
                    id: segRow
                    x: 2; y: 2
                    Repeater {
                        model: ["Movie", "Series"]
                        delegate: Rectangle {
                            required property string modelData
                            required property int index
                            readonly property bool on: (index === 1) === root.searchSeries
                            width: segText.implicitWidth + Math.round(root.fs * 1.4)
                            height: segment.height - 4
                            radius: 3
                            color: on ? "white" : (segMa.containsMouse ? "#404040" : "transparent")
                            Text {
                                id: segText
                                anchors.centerIn: parent
                                text: parent.modelData
                                color: parent.on ? "black" : "white"
                                font.family: Theme.font; font.pixelSize: Math.round(root.fs * 0.9); font.weight: Font.DemiBold
                            }
                            MouseArea {
                                id: segMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: { root.searchSeries = parent.index === 1; root.runSearch() }
                            }
                        }
                    }
                }
            }
            FlatButton {
                id: searchBtn
                anchors.verticalCenter: parent.verticalCenter
                label: root.searching ? "Searching…" : "Search"
                enabled: Tmdb.enabled && !root.searching && root.selectedId !== ""
                onClicked: root.runSearch()
            }
        }

        // results
        ListView {
            id: resultList
            visible: root.results.length > 0
            width: parent.width
            height: visible ? Math.round(root.fs * 6.2 * 1.5) + Math.round(root.fs * 3.4) : 0
            orientation: ListView.Horizontal
            spacing: Math.round(root.fs * 0.6)
            clip: true
            model: root.results
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded; height: 6 }
            delegate: Item {
                id: rd
                required property var modelData
                required property int index
                readonly property bool sel: root.selectedResult === index
                width: Math.round(root.fs * 6.2)
                height: resultList.height
                Rectangle {
                    id: posterBox
                    width: parent.width; height: Math.round(width * 1.5)
                    radius: 4
                    color: "#2F2F2F"
                    clip: true
                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 12
                        visible: poster.status !== Image.Ready
                        text: rd.modelData.title
                        color: "#808080"; font.family: Theme.font; font.pixelSize: Math.round(root.fs * 0.75)
                        horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                    }
                    Image {
                        id: poster
                        anchors.fill: parent
                        source: rd.modelData.posterUrl || ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize.width: 200
                    }
                    Rectangle {
                        anchors.fill: parent; radius: 4; color: "transparent"
                        border.width: rd.sel ? 3 : (rma.containsMouse ? 1 : 0)
                        border.color: rd.sel ? "white" : "#B3B3B3"
                    }
                }
                Text {
                    anchors.top: posterBox.bottom; anchors.topMargin: 6
                    width: parent.width
                    text: rd.modelData.title + (rd.modelData.year > 0 ? "\n" + rd.modelData.year : "")
                    color: rd.sel ? "white" : "#B3B3B3"
                    font.family: Theme.font; font.pixelSize: Math.round(root.fs * 0.8)
                    wrapMode: Text.WordWrap; maximumLineCount: 3; elide: Text.ElideRight
                }
                MouseArea {
                    id: rma
                    anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: root.selectedResult = rd.index
                    onDoubleClicked: useBtn.clicked()
                }
            }
        }

        Row {
            spacing: Math.round(root.fs * 0.75)
            FlatButton {
                id: useBtn
                label: "Use this"; red: true
                enabled: root.selectedResult >= 0 && root.selectedResult < root.results.length && root.selectedId !== ""
                onClicked: {
                    const r = root.results[root.selectedResult]
                    Tmdb.assign(root.selectedId, r.tmdbId, r.isSeries)
                    root.fixNote = "Using “" + r.title + (r.year > 0 ? " (" + r.year + ")" : "") + "”. Artwork appears when the download finishes."
                }
            }
            FlatButton {
                label: "Not a movie or show"
                enabled: root.selectedId !== ""
                onClicked: {
                    Tmdb.assign(root.selectedId, 0, false)
                    root.results = []; root.selectedResult = -1
                    root.fixNote = "This title will no longer be looked up."
                }
            }
            FlatButton {
                label: "Refetch"
                enabled: Tmdb.enabled && root.selectedId !== ""
                onClicked: { Tmdb.refreshTitle(root.selectedId); root.fixNote = "Refetching…" }
            }
        }
        Text {
            visible: root.fixNote !== ""
            width: parent.width
            wrapMode: Text.WordWrap
            text: root.fixNote
            color: "#A3A3A3"; font.family: Theme.font; font.pixelSize: Math.round(root.fs * 0.88)
        }
        Item { width: 1; height: Math.round(root.fs * 0.3) }
    }

    // ---- building blocks (local, so this file stays self-contained) ----
    component FlatButton: Rectangle {
        id: b
        property string label
        property bool red: false
        signal clicked()
        readonly property bool hov: bma.containsMouse
        width: bt.implicitWidth + Math.round(root.fs * 2)
        height: Math.round(root.fs * 2.5)
        radius: 4
        opacity: enabled ? 1 : 0.5
        color: red ? (hov ? "#C11119" : Theme.red)
                   : (hov ? Qt.rgba(109/255, 109/255, 110/255, 0.4) : Qt.rgba(109/255, 109/255, 110/255, 0.7))
        Text { id: bt; anchors.centerIn: parent; text: b.label; color: "white"; font.family: Theme.font; font.pixelSize: root.fs; font.weight: Font.DemiBold }
        MouseArea { id: bma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; enabled: b.enabled; onClicked: b.clicked() }
    }
    component Spinner: Item {
        id: sp
        property real size: 18
        width: size; height: size
        Rectangle { anchors.fill: parent; radius: width / 2; color: "transparent"; border.width: 2; border.color: "#404040" }
        Item {
            width: sp.size / 2; height: sp.size / 2
            clip: true
            Rectangle { width: sp.size; height: sp.size; radius: sp.size / 2; color: "transparent"; border.width: 2; border.color: Theme.red }
        }
        RotationAnimation on rotation { from: 0; to: 360; duration: 900; loops: Animation.Infinite; running: sp.visible }
    }
}
