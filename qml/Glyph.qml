// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

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
            // Four panes as independent single-subpath quads (`subpaths`)
            // instead of one multi-subpath fill: the latter triangulates
            // incorrectly on the D3D11 RHI backend and renders garbled
            // (VIV-117).  Geometry is a clean 2x2 grid with a 2-unit centre
            // gap so the four panes stay legible at icon size (~15px) — the
            // original 1-unit slanted gaps collapsed sub-pixel and read as two
            // solid bars once the triangulation garble was gone.
            return { vb: 24, filled: true, sw: 0, d: "",
                subpaths: [
                    "M3 3 H11 V11 H3 Z",
                    "M13 3 H21 V11 H13 Z",
                    "M3 13 H11 V21 H3 Z",
                    "M13 13 H21 V21 H13 Z"
                ] }
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
        case "upload":
            return { vb: 16, filled: false, sw: 1.5,
                d: "M8 11 V3 M4 7 L8 3 L12 7 M2 13 H14" }
        case "download":
            return { vb: 16, filled: false, sw: 1.5,
                d: "M8 3 V11 M4 7 L8 11 L12 7 M2 13 H14" }
        case "link":
            return { vb: 16, filled: false, sw: 1.5,
                d: "M6 10 a2.83 2.83 0 0 0 4 0 L13 7 a2.83 2.83 0 0 0 -4 -4 L8.5 3.5 M10 6 a2.83 2.83 0 0 0 -4 0 L3 9 a2.83 2.83 0 0 0 4 4 L7.5 12.5" }
        case "qr":
            return { vb: 16, filled: true, sw: 0,
                d: "M2 2h4v4H2V2zm1 1v2h2V3H3zm7-1h4v4h-4V2zm1 1v2h2V3h-2zM2 10h4v4H2v-4zm1 1v2h2v-2H3zm7-1h2v1h-1v1h-1v-2zm3 0h1v2h-1v-2zm-3 2h1v1h-1v-1zm2 1h2v1h-2v-1zm-2 1h1v1h-1v-1z" }
        case "pause":
            return { vb: 16, filled: true, sw: 0,
                d: "M4 3 H6.5 V13 H4 Z M9.5 3 H12 V13 H9.5 Z" }
        case "pin":
            return { vb: 16, filled: false, sw: 1.4,
                d: "M8 11 V14 M5 7 H11 L9 3 H7 L5 7 Z M4 7 H12" }
        case "gear":
            return { vb: 16, filled: false, sw: 1.4,
                d: "M8 6 a2 2 0 1 0 0.001 0 Z M8 1.6 V3.4 M8 12.6 V14.4 M14.4 8 H12.6 M3.4 8 H1.6 M12.1 3.9 L10.9 5.1 M5.1 10.9 L3.9 12.1 M12.1 12.1 L10.9 10.9 M5.1 5.1 L3.9 3.9" }

        // ---- devices (stroke, 24 viewBox) ----
        case "devices":
            return { vb: 24, filled: false, sw: 1.5,
                d: "M3.8 4 H13.2 A1.3 1.3 0 0 1 14.5 5.3 V11.7 A1.3 1.3 0 0 1 13.2 13 H3.8 A1.3 1.3 0 0 1 2.5 11.7 V5.3 A1.3 1.3 0 0 1 3.8 4 Z M5.5 16 H11.5 M8.5 13 V16 M17.7 8.5 H20.3 A1.2 1.2 0 0 1 21.5 9.7 V18.3 A1.2 1.2 0 0 1 20.3 19.5 H17.7 A1.2 1.2 0 0 1 16.5 18.3 V9.7 A1.2 1.2 0 0 1 17.7 8.5 Z" }
        }
        return { vb: 16, filled: false, sw: 1.5, d: "" }
    }

    readonly property var _icon: _def(name)
    // Icons that supply `subpaths` render one ShapePath per pane (see "win").
    readonly property var _subs: _icon.subpaths ? _icon.subpaths : []

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
        // Single-path icons: every glyph except those that supply `subpaths`.
        ShapePath {
            strokeColor: g._icon.filled ? "transparent" : g.color
            strokeWidth: g._icon.sw
            fillColor: g._icon.filled ? g.color : "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: g._subs.length > 0 ? "" : g._icon.d }
        }
        // Multi-quad icons (the Windows logo): one independent filled ShapePath
        // per pane so no single path holds multiple disjoint subpaths, which
        // triangulates incorrectly on the D3D11 RHI backend (VIV-117).  Four
        // fixed slots cover the glyph; unused slots render an empty path.
        ShapePath {
            strokeColor: "transparent"; strokeWidth: 0
            fillColor: g._icon.filled ? g.color : "transparent"
            PathSvg { path: g._subs.length > 0 ? g._subs[0] : "" }
        }
        ShapePath {
            strokeColor: "transparent"; strokeWidth: 0
            fillColor: g._icon.filled ? g.color : "transparent"
            PathSvg { path: g._subs.length > 1 ? g._subs[1] : "" }
        }
        ShapePath {
            strokeColor: "transparent"; strokeWidth: 0
            fillColor: g._icon.filled ? g.color : "transparent"
            PathSvg { path: g._subs.length > 2 ? g._subs[2] : "" }
        }
        ShapePath {
            strokeColor: "transparent"; strokeWidth: 0
            fillColor: g._icon.filled ? g.color : "transparent"
            PathSvg { path: g._subs.length > 3 ? g._subs[3] : "" }
        }
    }
}
