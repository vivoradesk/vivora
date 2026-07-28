import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One row of the Settings → Account → My Devices table (devices.jsx
// SettingsDeviceRow).  Columns: Device · Operating system · Status · Actions.
Rectangle {
    id: row
    property DevicePalette pal: DevicePalette {}

    property string devId: ""
    property string devName: ""
    property string os: "win"
    property bool   online: false
    property bool   current: false
    property bool   warned: false
    property string seen: ""
    property string host: ""

    // Column widths — fed by the parent table so header and rows line up.
    // Defaults match MyDevicesSettings and are sized to fit the Settings pane.
    property real osColWidth: 120
    property real statusColWidth: 84
    property real actionsColWidth: 128

    signal renameCommitted(string devId, string newName)
    signal removeRequested(string devId, string devName)

    property bool editing: false
    readonly property var osLabel: ({ "mac": "macOS 14.5", "win": "Windows 11", "linux": "Ubuntu 24.04" })

    implicitHeight: 54
    color: current ? pal.blueSoft : (hover.hovered ? pal.paperSoft : "transparent")

    HoverHandler { id: hover }

    function _commit() {
        var v = nameField.text.trim()
        editing = false
        if (v.length > 0 && v !== devName)
            renameCommitted(devId, v)
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 12

        // ── Device (avatar + name/host) ──
        RowLayout {
            Layout.fillWidth: true
            spacing: 11
            DeviceAvatar {
                pal: row.pal
                os: row.os
                online: row.online
                warned: row.warned
                size: 30
                glyphSize: 14
                Layout.alignment: Qt.AlignVCenter
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                // Name (or inline rename)
                RowLayout {
                    visible: !row.editing
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        text: row.devName
                        color: row.pal.ink
                        font.family: row.pal.sans
                        font.pixelSize: 13
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                        // Fill the (narrow) Device column and elide so long
                        // names never spill into the Operating-system column.
                        Layout.fillWidth: true
                        Layout.maximumWidth: 150
                    }
                    Rectangle {
                        visible: row.current
                        radius: 4
                        color: row.pal.paperDeep
                        border.width: 1; border.color: row.pal.hair
                        Layout.preferredHeight: 16
                        Layout.preferredWidth: tlbl.implicitWidth + 12
                        Label { id: tlbl; anchors.centerIn: parent; text: "THIS DEVICE"
                                color: row.pal.inkMid; font.family: row.pal.mono
                                font.pixelSize: 8; font.letterSpacing: 0.6 }
                    }
                    Rectangle {
                        visible: row.warned
                        radius: 4
                        color: row.pal.amberSoft
                        border.width: 1; border.color: row.pal.amberBorder
                        Layout.preferredHeight: 16
                        Layout.preferredWidth: wrow.implicitWidth + 10
                        Row {
                            id: wrow; anchors.centerIn: parent; spacing: 4
                            Glyph { name: "warn"; size: 10; color: row.pal.amber; anchors.verticalCenter: parent.verticalCenter }
                            Label { text: "key changed"; color: row.pal.amber
                                    font.family: row.pal.mono; font.pixelSize: 9
                                    anchors.verticalCenter: parent.verticalCenter }
                        }
                    }
                }
                Rectangle {
                    visible: row.editing
                    Layout.preferredHeight: 24
                    Layout.preferredWidth: 160
                    radius: 6
                    color: row.pal.paper
                    border.width: 1; border.color: row.pal.ink
                    TextField {
                        id: nameField
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        background: null
                        padding: 0
                        verticalAlignment: TextInput.AlignVCenter
                        color: row.pal.ink
                        font.family: row.pal.sans
                        font.pixelSize: 13
                        selectByMouse: true
                        onAccepted: row._commit()
                        Keys.onEscapePressed: row.editing = false
                    }
                }
                Label {
                    visible: !row.editing
                    text: row.host
                    color: row.pal.inkFaint
                    font.family: row.pal.mono
                    font.pixelSize: 10
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
        }

        // ── Operating system ──
        RowLayout {
            Layout.preferredWidth: row.osColWidth
            spacing: 8
            Glyph { name: row.os; size: 15; color: row.pal.inkMid; Layout.alignment: Qt.AlignVCenter }
            Label {
                text: row.osLabel[row.os]
                color: row.pal.inkSoft
                font.family: row.pal.sans
                font.pixelSize: 13
                // Clip within the column instead of overflowing into Status.
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
        }

        // ── Status ──
        RowLayout {
            Layout.preferredWidth: row.statusColWidth
            spacing: 7
            Rectangle {
                width: 7; height: 7; radius: 3.5
                color: row.online ? row.pal.online : row.pal.inkFaint
                Layout.alignment: Qt.AlignVCenter
            }
            Label {
                text: row.current ? "This device" : row.online ? "Online now" : row.seen
                color: row.pal.inkMid
                font.family: row.pal.mono
                font.pixelSize: 11
                // Clip within the column so it never overlaps the actions.
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
        }

        // ── Actions ──
        RowLayout {
            Layout.preferredWidth: row.actionsColWidth
            layoutDirection: Qt.RightToLeft
            spacing: 4
            // Remove (not current)
            Rectangle {
                visible: !row.current
                Layout.preferredHeight: 26
                Layout.preferredWidth: rmLbl.implicitWidth + 16
                radius: 6
                color: rmHover.containsMouse ? row.pal.redSoft : "transparent"
                Label { id: rmLbl; anchors.centerIn: parent; text: "Remove"
                        color: row.pal.red; font.family: row.pal.sans
                        font.pixelSize: 12; font.weight: Font.Medium }
                MouseArea { id: rmHover; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: row.removeRequested(row.devId, row.devName) }
            }
            // Rename
            Rectangle {
                visible: !row.editing
                Layout.preferredHeight: 26
                Layout.preferredWidth: rnLbl.implicitWidth + 16
                radius: 6
                color: rnHover.containsMouse ? row.pal.paperDeep : "transparent"
                Label { id: rnLbl; anchors.centerIn: parent; text: "Rename"
                        color: rnHover.containsMouse ? row.pal.ink : row.pal.inkSoft
                        font.family: row.pal.sans; font.pixelSize: 12; font.weight: Font.Medium }
                MouseArea { id: rnHover; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        nameField.text = row.devName
                        row.editing = true
                        nameField.forceActiveFocus()
                        nameField.selectAll()
                    }
                }
            }
        }
    }

    // Bottom hairline
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: row.pal.hair
    }
}
