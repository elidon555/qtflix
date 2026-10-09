pragma ComponentBehavior: Bound
import QtQuick
import QtFlix

// The scrolling body of Home / TV Shows / Movies: billboard (+ optional genre heading) as the list header, one
// TitleRow per row, Footer as the list footer. Rows are ListView delegates: only the rows within about a screen
// of the viewport exist (recycled with reuseItems), each remembering the page it was paged to.
//  - rows: a RowsModel (roles name/kind/model) or a JS array of {name, kind, model}
//  - featured: the billboard's title map ({} or showBillboard false = no billboard)
ScrollList {
    id: root
    property var rows: null
    property var featured: ({})
    property bool showBillboard: true
    property string heading: ""               // genre-page heading under the nav ("TV Shows")
    property bool pageActive: true            // page visible and nothing covering it (billboard playback)
    property bool previewShown: false         // a hover preview is open (pauses the billboard)
    property real footerGap: 40
    signal preview(string id, rect globalRect)
    signal exploreAll(var row)                // {name, kind, model}

    readonly property real rowGap: Math.round(Theme.vw(2.8))
    readonly property Billboard billboard: (headerItem as Header)?.billboard ?? null
    property var rowPages: ({})               // row name -> page, survives delegate recycling

    model: rows
    reuseItems: true
    cacheBuffer: Math.max(0, Math.round(height))
    delegate: Array.isArray(rows) ? arrayRow : modelRow

    header: Header {}
    footer: Item {
        width: root.width
        height: footerItem.y + footerItem.height + 20
        Footer { id: footerItem; y: root.footerGap - root.rowGap; width: parent.width }
    }

    // ---- header: billboard + heading; the first row overlaps the billboard's faded bottom ----
    component Header: Item {
        id: hdr
        readonly property alias billboard: bb
        width: root.width
        height: root.showBillboard && bb.hasTitle ? bb.height - Math.round(bb.height * 0.24)
              : (headingText.visible ? headingText.y + headingText.height + Math.round(Theme.vw(1.8)) : Theme.navH + 30)
        Billboard {
            id: bb
            visible: root.showBillboard
            width: parent.width
            height: root.showBillboard ? Math.round(width * 0.5625) : 0
            title: root.featured
            active: root.pageActive && visible && !root.previewShown && root.scrollY < bb.height * 0.55
            loaded: root.pageActive && visible && root.scrollY < bb.height
        }
        Text {
            id: headingText
            visible: text !== ""
            x: Theme.gutter
            y: Theme.navH + (root.showBillboard ? Math.round(Theme.vw(0.4)) : Math.round(Theme.vw(1.8)))
            text: root.heading
            color: "white"
            font.family: Theme.font
            font.pixelSize: Math.round(Theme.clamp(Theme.vw(2.4), 26, 64))
            font.weight: root.showBillboard ? Font.Bold : Font.Medium
            style: root.showBillboard ? Text.Raised : Text.Normal
            styleColor: Qt.rgba(0, 0, 0, 0.3)
        }
    }

    // ---- rows ----
    component RowItem: Item {
        id: wrap
        property string rowName
        property string rowKind
        property var titles: null
        z: 3                                      // above the billboard header (ListView puts header/footer at z 1-2)
        width: root.width
        height: row.itemCount > 0 ? row.height + root.rowGap : 0
        visible: row.itemCount > 0
        function restorePage() { row.setPage(root.rowPages[wrap.rowName] ?? 0) }
        Component.onCompleted: restorePage()
        ListView.onReused: restorePage()
        TitleRow {
            id: row
            width: wrap.width
            name: wrap.rowName
            kind: wrap.rowKind
            model: wrap.titles
            onPageChanged: root.rowPages[wrap.rowName] = page
            onPreview: (id, r) => root.preview(id, r)
            onExploreAll: root.exploreAll({ name: wrap.rowName, kind: wrap.rowKind, model: wrap.titles })
        }
    }
    // RowsModel roles. NB: the "model" role shadows the usual model object, so it IS the TitleModel.
    Component {
        id: modelRow
        RowItem {
            required property string name
            required property string kind
            required property var model
            rowName: name; rowKind: kind; titles: model
        }
    }
    Component {
        id: arrayRow
        RowItem {
            required property var modelData
            rowName: modelData.name; rowKind: modelData.kind; titles: modelData.model
        }
    }
}
