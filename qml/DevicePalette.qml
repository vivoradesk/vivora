import QtQuick

// Design tokens for the "My Devices" surfaces, lifted verbatim from the
// designer handoff (app.css `:root`).  Kept in one QtObject so the device
// components share a single source of truth instead of hard-coding hex in
// every file.  These match the values the rest of the app's `theme` objects
// already use (blue #3D6BFA, amber #b6801b, …); the extra device-only tokens
// (paper, online, hair, …) that aren't on the main.qml / SettingsDialog.qml
// themes are gathered here.
QtObject {
    readonly property color paper:      "#f6f4ef"
    readonly property color paperSoft:  "#efece4"
    readonly property color paperDeep:  "#e7e3d8"
    readonly property color ink:        "#111114"
    readonly property color inkSoft:    "#2a2a2e"
    readonly property color inkMid:     "#5e5e63"
    readonly property color inkFaint:   "#8a8a90"
    readonly property color hair:        Qt.rgba(17/255, 17/255, 20/255, 0.08)
    readonly property color hairStrong:  Qt.rgba(17/255, 17/255, 20/255, 0.14)
    readonly property color blue:       "#3D6BFA"
    readonly property color blueSoft:    Qt.rgba(61/255, 107/255, 250/255, 0.10)
    readonly property color blueBorder:  Qt.rgba(61/255, 107/255, 250/255, 0.22)
    readonly property color online:     "#3ddc84"
    readonly property color onlineGlow:  Qt.rgba(61/255, 220/255, 132/255, 0.28)
    readonly property color green:      "#1FA463"
    readonly property color red:        "#E5484D"
    readonly property color redSoft:     Qt.rgba(229/255, 72/255, 77/255, 0.10)
    readonly property color amber:      "#b6801b"
    readonly property color amberSoft:   Qt.rgba(182/255, 128/255, 27/255, 0.12)
    readonly property color amberBorder: Qt.rgba(182/255, 128/255, 27/255, 0.28)

    readonly property string sans:  App.sansFont
    readonly property string mono:  App.monoFont
}
