import QtQuick
import QtQuick.Controls

// Context menu for a non-current device row (devices.jsx `.ctx-menu`):
// Rename · Copy peer code · (sep) · Remove device.  A flat cream Popup rather
// than a native Menu so it matches the address-book styling.
Popup {
    id: menu
    property DevicePalette pal: DevicePalette {}
    width: 190
    padding: 5
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside | Popup.CloseOnReleaseOutsideParent

    signal rename()
    signal copyCode()
    signal remove()

    background: Rectangle {
        color: menu.pal.paper
        radius: 9
        border.width: 1
        border.color: menu.pal.hairStrong
    }

    // Reusable menu row.
    component Row_: Rectangle {
        id: mrow
        property string glyph: ""
        property string label: ""
        property bool danger: false
        signal triggered()
        width: menu.availableWidth
        height: 30
        radius: 6
        color: rowHover.containsMouse ? menu.pal.paperSoft : "transparent"
        Row {
            anchors.left: parent.left
            anchors.leftMargin: 9
            anchors.verticalCenter: parent.verticalCenter
            spacing: 9
            Glyph { name: mrow.glyph; size: 13
                    color: mrow.danger ? menu.pal.red : menu.pal.inkMid
                    anchors.verticalCenter: parent.verticalCenter }
            Label {
                text: mrow.label
                color: mrow.danger ? menu.pal.red : menu.pal.inkSoft
                font.family: menu.pal.sans
                font.pixelSize: 13
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        MouseArea {
            id: rowHover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: mrow.triggered()
        }
    }

    contentItem: Column {
        spacing: 1
        Row_ {
            glyph: "pencil"; label: "Rename"
            onTriggered: { menu.rename(); menu.close() }
        }
        Row_ {
            glyph: "copy"; label: "Copy peer code"
            onTriggered: { menu.copyCode(); menu.close() }
        }
        Rectangle { width: menu.availableWidth; height: 1; color: menu.pal.hair }
        Row_ {
            glyph: "x"; label: "Remove device"; danger: true
            onTriggered: { menu.remove(); menu.close() }
        }
    }
}
