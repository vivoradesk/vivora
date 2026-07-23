import QtQuick
import QtQuick.Shapes

// Monochrome vector icon renderer for the "My Devices" UI.  Reproduces the
// designer handoff's inline SVGs (OS glyphs from devices.jsx `OS`, small
// icons from chrome.jsx `I` / devices.jsx `DevI`) via QtQuick.Shapes so the
// exact path data can be reused — no colour emoji, no bitmap assets.
//
// Each icon is authored in its original viewBox (16 or 24) and scaled to the
// requested `size`.  Stroke widths stay in viewBox units so they scale with
// the glyph, matching the design.
Item {
    id: g
    property string name: ""
    property real size: 14
    property color color: "#111114"

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    // name -> { vb: viewBox, sw: strokeWidth, filled: bool, d: pathData }
    function _def(n) {
        switch (n) {
        // ---- OS glyphs (fill, 24 viewBox) ----
        case "win":
            return { vb: 24, filled: true, sw: 0,
                d: "M3 4.6 10.6 3.5v7.7H3zM11.6 3.35 21 2v9.2h-9.4zM3 12.8h7.6v7.7L3 19.4zM11.6 12.8H21V22l-9.4-1.3z" }
        case "mac":
            return { vb: 24, filled: true, sw: 0,
                d: "M17.05 12.04c-.03-2.7 2.21-3.99 2.31-4.06-1.26-1.85-3.23-2.1-3.92-2.13-1.67-.17-3.26.98-4.11.98-.86 0-2.16-.96-3.55-.93-1.83.03-3.51 1.06-4.45 2.7-1.9 3.3-.49 8.18 1.36 10.86.9 1.31 1.97 2.78 3.37 2.73 1.36-.05 1.87-.88 3.51-.88 1.64 0 2.1.88 3.53.85 1.46-.03 2.39-1.33 3.28-2.65 1.04-1.52 1.47-2.99 1.49-3.07-.03-.01-2.85-1.1-2.87-4.34zM14.43 4.3c.73-.9 1.23-2.14 1.09-3.39-1.06.04-2.36.71-3.12 1.6-.68.78-1.28 2.04-1.12 3.26 1.19.09 2.41-.6 3.15-1.47z" }
        case "linux":
            return { vb: 24, filled: true, sw: 0,
                d: "M12 1.8c-2.05 0-3.35 1.62-3.35 3.98 0 1.06.06 1.66.06 2.5 0 .74-1.06 1.9-1.86 3.2C5.9 13.02 5.4 14.4 5.4 15.6c0 .35.06.66.2.92-.5.55-.9 1.2-.9 1.86 0 .5.28.82.78.98-.1.28-.16.56-.16.84 0 .82.66 1.24 1.78 1.24.78 0 1.44-.24 1.92-.66.6.24 1.5.38 2.98.38s2.38-.14 2.98-.38c.48.42 1.14.66 1.92.66 1.12 0 1.78-.42 1.78-1.24 0-.28-.06-.56-.16-.84.5-.16.78-.48.78-.98 0-.66-.4-1.31-.9-1.86.14-.26.2-.57.2-.92 0-1.2-.5-2.58-1.45-4.12-.8-1.3-1.86-2.46-1.86-3.2 0-.84.06-1.44.06-2.5C15.35 3.42 14.05 1.8 12 1.8zm-1.55 4.03c.5 0 .9.52.9 1.16s-.4 1.16-.9 1.16-.9-.52-.9-1.16.4-1.16.9-1.16zm3.12 0c.5 0 .9.52.9 1.16s-.4 1.16-.9 1.16-.9-.52-.9-1.16.4-1.16.9-1.16zm-1.6 2.94c.66 0 1.6.42 1.6.86 0 .26-.34.42-.72.58-.36.16-.76.32-.88.32s-.52-.16-.88-.32c-.38-.16-.72-.32-.72-.58 0-.44.94-.86 1.6-.86z" }

        // ---- small icons (stroke unless noted, 16 viewBox) ----
        case "check":
            return { vb: 16, filled: false, sw: 2, d: "M3 8 L7 12 L13 4" }
        case "lock":
            return { vb: 16, filled: false, sw: 1.5,
                d: "M4.5 7 H11.5 A1.5 1.5 0 0 1 13 8.5 V12.5 A1.5 1.5 0 0 1 11.5 14 H4.5 A1.5 1.5 0 0 1 3 12.5 V8.5 A1.5 1.5 0 0 1 4.5 7 Z M5.5 7 V5 a2.5 2.5 0 0 1 5 0 V7" }
        case "spark":
            return { vb: 16, filled: true, sw: 0,
                d: "M8 1.5 9.4 5.8 13.7 7 9.4 8.2 8 12.5 6.6 8.2 2.3 7 6.6 5.8z" }
        case "shield":
            return { vb: 16, filled: false, sw: 1.5,
                d: "M8 1.8 13 3.6v4.1c0 3.3-2.1 5.6-5 6.5-2.9-.9-5-3.2-5-6.5V3.6z" }
        case "warn":
            return { vb: 16, filled: false, sw: 1.6,
                d: "M8 2 L1.6 13.4 L14.4 13.4 Z M8 6.4 L8 9.4 M8 11.3 L8 11.5" }
        case "pencil":
            return { vb: 16, filled: false, sw: 1.4,
                d: "M11.5 2.5 L13.5 4.5 L5.5 12.5 L3 13 L3.5 10.5 Z" }
        case "connect":
            return { vb: 16, filled: false, sw: 1.6,
                d: "M3 8 H12 M8.5 4.5 L12 8 L8.5 11.5" }
        case "copy":
            return { vb: 16, filled: false, sw: 1.5,
                d: "M6.5 5 H12.5 A1.5 1.5 0 0 1 14 6.5 V12.5 A1.5 1.5 0 0 1 12.5 14 H6.5 A1.5 1.5 0 0 1 5 12.5 V6.5 A1.5 1.5 0 0 1 6.5 5 Z M11 5 V3.5 A1.5 1.5 0 0 0 9.5 2 H3.5 A1.5 1.5 0 0 0 2 3.5 V9.5 A1.5 1.5 0 0 0 3.5 11 H5" }
        case "x":
            return { vb: 16, filled: false, sw: 1.6, d: "M4 4 L12 12 M12 4 L4 12" }
        case "refresh":
            return { vb: 16, filled: false, sw: 1.4,
                d: "M14 8 a6 6 0 1 0 -1.76 4.24 M14 4 L14 8 L10 8" }

        // ---- devices (stroke, 24 viewBox) ----
        case "devices":
            return { vb: 24, filled: false, sw: 1.5,
                d: "M3.8 4 H13.2 A1.3 1.3 0 0 1 14.5 5.3 V11.7 A1.3 1.3 0 0 1 13.2 13 H3.8 A1.3 1.3 0 0 1 2.5 11.7 V5.3 A1.3 1.3 0 0 1 3.8 4 Z M5.5 16 H11.5 M8.5 13 V16 M17.7 8.5 H20.3 A1.2 1.2 0 0 1 21.5 9.7 V18.3 A1.2 1.2 0 0 1 20.3 19.5 H17.7 A1.2 1.2 0 0 1 16.5 18.3 V9.7 A1.2 1.2 0 0 1 17.7 8.5 Z" }
        }
        return { vb: 16, filled: false, sw: 1.5, d: "" }
    }

    readonly property var _icon: _def(name)

    Shape {
        id: shp
        width: g._icon.vb
        height: g._icon.vb
        antialiasing: true
        transform: Scale {
            origin.x: 0; origin.y: 0
            xScale: g.size / g._icon.vb
            yScale: g.size / g._icon.vb
        }
        ShapePath {
            strokeColor: g._icon.filled ? "transparent" : g.color
            strokeWidth: g._icon.sw
            fillColor: g._icon.filled ? g.color : "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: g._icon.d }
        }
    }
}
