import QtQuick

// OS-glyph square with a corner presence dot.  Spec (devices.jsx
// DeviceRowSpec): 34x34, radius 9, glyph 15, status dot 12px with a 2.5px
// paper-coloured ring; online = #3ddc84 + glow, offline = ink-faint.
Item {
    id: ava
    property DevicePalette pal: DevicePalette {}
    property string os: "win"
    property bool online: false
    property bool warned: false
    property real size: 34
    property real glyphSize: 15

    implicitWidth: size
    implicitHeight: size

    Rectangle {
        id: box
        anchors.fill: parent
        radius: 9
        color: ava.pal.paperSoft
        border.width: 1
        border.color: ava.warned ? ava.pal.amberBorder : ava.pal.hair

        Glyph {
            anchors.centerIn: parent
            name: ava.os
            size: ava.glyphSize
            color: ava.online ? ava.pal.inkSoft : ava.pal.inkFaint
            opacity: ava.online ? 1.0 : 0.7
        }
    }

    // Presence dot at the bottom-right corner, straddling the edge.
    Rectangle {
        width: 12
        height: 12
        radius: 6
        anchors.right: box.right
        anchors.bottom: box.bottom
        anchors.rightMargin: -3
        anchors.bottomMargin: -3
        color: ava.online ? ava.pal.online : ava.pal.inkFaint
        border.width: 2.5
        border.color: ava.pal.paper

        // Soft glow ring for the online state (approximates the CSS
        // box-shadow 0 0 0 2px rgba(online,0.28)).
        Rectangle {
            visible: ava.online
            anchors.centerIn: parent
            width: parent.width + 4
            height: parent.height + 4
            radius: width / 2
            color: "transparent"
            border.width: 2
            border.color: ava.pal.onlineGlow
            z: -1
        }
    }
}
