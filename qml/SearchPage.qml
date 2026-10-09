pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// Search results grid (Library.searchResults, a virtualized TitleGrid page) with "More to explore" chips and
// Netflix's empty state. (NavBar debounces typing into Library.searchQuery.)
FocusScope {
    id: page
    objectName: "searchPage"
    readonly property alias hoverPreview: hoverPreview
    readonly property alias scrollY: grid.scrollY
    property var results: Library.searchResults
    readonly property string query: Library.searchQuery
    readonly property real pad: Theme.gutter
    readonly property real fs: Math.round(Theme.clamp(Theme.vw(1.0), 14, 24))

    // related terms collected from the results' genres / categories
    readonly property var related: {
        const seen = {}, out = []
        const n = results ? Math.min(results.count, 30) : 0
        for (let i = 0; i < n && out.length < 8; ++i) {
            const t = results.get(i)
            const cands = (t.genres || []).concat([t.category])
            for (let j = 0; j < cands.length && out.length < 8; ++j) {
                const g = cands[j]
                if (g && !seen[g] && g.toLowerCase() !== page.query.toLowerCase()) { seen[g] = true; out.push(g) }
            }
        }
        return out
    }

    TitleGrid {
        id: grid
        objectName: "searchGrid"
        anchors.fill: parent
        focus: true
        titles: page.results
        onContentYChanged: hoverPreview.close(true)
        onPreview: (id, r) => hoverPreview.open(id, r)

        topContent: Item {
            height: chips.y + (chips.visible ? chips.height : 0)
            // More to explore: a | b | c
            Flow {
                id: chips
                x: page.pad
                y: Theme.navH + Math.round(Theme.vw(3.4))
                width: parent.width - 2 * page.pad
                spacing: 0
                visible: page.results && page.results.count > 0 && page.related.length > 0
                bottomPadding: 34
                Text {
                    text: "More to explore:"
                    color: "#808080"
                    font.family: Theme.font; font.pixelSize: page.fs
                    rightPadding: 12
                }
                Repeater {
                    model: page.related
                    Text {
                        id: chip
                        required property string modelData
                        required property int index
                        readonly property bool last: index === page.related.length - 1
                        text: modelData
                        color: tma.containsMouse ? "#B3B3B3" : "white"
                        font.family: Theme.font; font.pixelSize: page.fs
                        Text {
                            visible: !chip.last
                            x: chip.implicitWidth + 8
                            text: "|"; color: "#808080"; font.family: Theme.font; font.pixelSize: page.fs
                        }
                        width: implicitWidth + (last ? 0 : 22)
                        MouseArea { id: tma; anchors.fill: parent; anchors.rightMargin: chip.last ? 0 : 22; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: Library.searchQuery = chip.modelData }
                    }
                }
            }
        }
        bottomContent: Item { height: 80 - grid.gap }
    }

    // Empty state (Netflix wording)
    Column {
        visible: page.query.trim() !== "" && (!page.results || page.results.count === 0)
        anchors.horizontalCenter: parent.horizontalCenter
        y: Theme.navH + Math.round(page.height * 0.12)
        width: Math.min(page.fs * 35, parent.width - 2 * page.pad)
        spacing: 18
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: "Your search for \"" + page.query + "\" did not have any matches."
            color: "white"
            font.family: Theme.font; font.pixelSize: page.fs
        }
        Column {
            spacing: 4
            Text { text: "Suggestions:"; color: "white"; font.family: Theme.font; font.pixelSize: page.fs }
            Repeater {
                model: ["Try different keywords", "Looking for a movie or TV show?",
                        "Try using a movie, TV show title, an actor or director",
                        "Try a genre, like comedy, romance, sports, or drama"]
                Row {
                    id: suggestion
                    required property string modelData
                    leftPadding: 22
                    spacing: 10
                    Text { text: "•"; color: "white"; font.family: Theme.font; font.pixelSize: page.fs }
                    Text { text: suggestion.modelData; color: "white"; font.family: Theme.font; font.pixelSize: page.fs }
                }
            }
        }
    }

    HoverPreview { id: hoverPreview }
}
