pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// Page footer: app links, credits.
Item {
    id: root
    width: parent ? parent.width : 1600
    height: col.height + 60
    readonly property real w: Math.min(980, width - 2 * Theme.gutter)
    readonly property string repo: "https://github.com/elidon555/qtflix"

    Column {
        id: col
        x: Math.round((root.width - root.w) / 2)
        width: root.w
        spacing: 0
        topPadding: 28
        BrandLogo { logoHeight: 22; opacity: 0.85 }
        Item { width: 1; height: 22 }
        Grid {
            columns: 4
            columnSpacing: 0
            rowSpacing: 14
            Repeater {
                model: [
                    { label: "Settings", act: function () { Nav.go("settings") } },
                    { label: "Rescan library", act: function () { Library.rescan() } },
                    { label: "My List", act: function () { Nav.go("mylist") } },
                    { label: "Switch profile", act: function () { Nav.go("profiles") } },
                    { label: "Source code", act: function () { Qt.openUrlExternally(root.repo) } },
                    { label: "Report an issue", act: function () { Qt.openUrlExternally(root.repo + "/issues") } },
                    { label: "License", act: function () { Qt.openUrlExternally(root.repo + "/blob/main/LICENSE") } },
                    { label: "TMDB", act: function () { Qt.openUrlExternally("https://www.themoviedb.org") } }
                ]
                Text {
                    id: link
                    required property var modelData
                    width: root.w / 4
                    text: link.modelData.label
                    color: "#808080"
                    font.family: Theme.font; font.pixelSize: 13
                    font.underline: fma.containsMouse
                    MouseArea { id: fma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: link.modelData.act() }
                }
            }
        }
        Item { width: 1; height: 24 }
        Text {
            width: root.w; wrapMode: Text.WordWrap
            text: "qtflix plays the video files on your own computer. It is not affiliated with Netflix. "
                  + "This product uses the TMDB API but is not endorsed or certified by TMDB."
            color: "#808080"; font.family: Theme.font; font.pixelSize: 11; lineHeight: 1.3
        }
    }
}
