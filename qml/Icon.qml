import QtQuick
import QtQuick.Shapes

// Vector icon drawn on a 24x24 grid, scaled to `size`. Netflix-style glyphs.
Item {
    id: root
    property string name: "play"
    property color color: "white"
    property real size: 24
    property real strokeWidth: 2
    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    readonly property var fills: ({
        "play": "M5 2.69127C5 1.93067 5.81547 1.44851 6.48192 1.81506L23.4069 11.1238C24.0977 11.5037 24.0977 12.4963 23.4069 12.8762L6.48192 22.1849C5.81546 22.5515 5 22.0693 5 21.3087V2.69127Z",
        "info": "M12 3C7.02944 3 3 7.02944 3 12C3 16.9706 7.02944 21 12 21C16.9706 21 21 16.9706 21 12C21 7.02944 16.9706 3 12 3ZM1 12C1 5.92487 5.92487 1 12 1C18.0751 1 23 5.92487 23 12C23 18.0751 18.0751 23 12 23C5.92487 23 1 18.0751 1 12ZM13 10V18H11V10H13ZM12 8.5C12.8284 8.5 13.5 7.82843 13.5 7C13.5 6.17157 12.8284 5.5 12 5.5C11.1716 5.5 10.5 6.17157 10.5 7C10.5 7.82843 11.1716 8.5 12 8.5Z",
        "plus": "M11 11V2H13V11H22V13H13V22H11V13H2V11H11Z",
        "check": "M8.68239 19.7312L23.6824 5.73115L22.3178 4.26904L8.02404 17.6098L2.70718 12.2929L1.29297 13.7071L7.29297 19.7071C7.67401 20.0882 8.28845 20.0988 8.68239 19.7312Z",
        "volumeOn": "M11 4.00003C11 3.59557 10.7564 3.23093 10.3827 3.07615C10.009 2.92137 9.57889 3.00692 9.29289 3.29292L4.58579 8.00003H1C0.447715 8.00003 0 8.44774 0 9.00003V15C0 15.5523 0.447715 16 1 16H4.58579L9.29289 20.7071C9.57889 20.9931 10.009 21.0787 10.3827 20.9239C10.7564 20.7691 11 20.4045 11 20V4.00003Z",
        "volumeOff": "M11 4.00003C11 3.59557 10.7564 3.23093 10.3827 3.07615C10.009 2.92137 9.57889 3.00692 9.29289 3.29292L4.58579 8.00003H1C0.447715 8.00003 0 8.44774 0 9.00003V15C0 15.5523 0.447715 16 1 16H4.58579L9.29289 20.7071C9.57889 20.9931 10.009 21.0787 10.3827 20.9239C10.7564 20.7691 11 20.4045 11 20V4.00003Z",
        "volume": "",
        "bell": "",
        "caretDown": "M5 9L12 16L19 9Z",
        "pencil": "M15.6 3.4a2 2 0 0 1 2.83 0l2.17 2.17a2 2 0 0 1 0 2.83L8.5 20.5H3.5v-5L15.6 3.4ZM14.2 7.6L5.5 16.3v2.2h2.2l8.7-8.7l-2.2-2.2Z",
        "n": ""
    })
    readonly property var strokes: ({
        "thumbsUp": "M7.5 10.5L11.2 3.6C11.4 3.2 11.8 3 12.2 3C13.5 3 14.5 4.1 14.3 5.4L13.8 9H19.2C20.5 9 21.4 10.2 21.1 11.4L19.4 18.6C19.2 19.4 18.4 20 17.5 20H7.5V10.5ZM3 10.5H7.5V20H3Z",
        "chevronDown": "M4.5 8.5L12 16L19.5 8.5",
        "chevronUp": "M4.5 15.5L12 8L19.5 15.5",
        "chevronRight": "M8.5 3.5L17 12L8.5 20.5",
        "chevronLeft": "M15.5 3.5L7 12L15.5 20.5",
        "search": "M10.5 3.5A7 7 0 1 0 10.5 17.5A7 7 0 1 0 10.5 3.5ZM15.6 15.6L21.5 21.5",
        "bell": "M12 2.8C8.7 2.8 6.4 5.4 6.4 8.6V13.2L4.2 16.6V17.7H19.8V16.6L17.6 13.2V8.6C17.6 5.4 15.3 2.8 12 2.8ZM9.6 20.2C10.1 21.2 11 21.8 12 21.8C13 21.8 13.9 21.2 14.4 20.2",
        "close": "M5 5L19 19M19 5L5 19",
        "volumeOn": "M14.5 8.5C15.5 9.4 16 10.6 16 12C16 13.4 15.5 14.6 14.5 15.5M17.5 5.5C19.3 7.2 20.3 9.5 20.3 12C20.3 14.5 19.3 16.8 17.5 18.5",
        "volumeOff": "M15.5 9L21.5 15M21.5 9L15.5 15",
        "replay": "M4.5 12A7.5 7.5 0 1 0 7 6.4M7.3 2.2L6.8 6.7L11.3 7.2",
        "plusThin": "M12 3V21M3 12H21",
        "info": "",
        "pencil": "",
        "back": "M20 12H4.5M10.5 5.5L4 12L10.5 18.5"
    })

    Shape {
        width: 24; height: 24
        scale: root.size / 24
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: (root.fills[root.name] || "") !== "" ? root.color : "transparent"
            strokeColor: "transparent"
            strokeWidth: 0
            PathSvg { path: root.fills[root.name] || "" }
        }
        ShapePath {
            fillColor: "transparent"
            strokeColor: (root.strokes[root.name] || "") !== "" ? root.color : "transparent"
            strokeWidth: root.strokeWidth
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: root.strokes[root.name] || "" }
        }
    }
}
