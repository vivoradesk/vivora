import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Main-window device row (devices.jsx DeviceRow).  Avatar + identity + a
// right-side action: Connect (online peer), Offline tag, or — for the current
// device — an inline rename pencil.  Offline rows get an indented footnote;
// key-changed rows get an amber warning note with Verify / Remove.
//
// Purely presentational: it emits intents, the parent block wires them (to a
// real device-mesh backend once VIV-52 lands, to a toast / local ListModel
// mutation for now).
Item {
    id: root

    property DevicePalette pal: DevicePalette {}

    // Device fields (fed from the mock ListModel role names).
    property string devId: ""
    property string devName: ""
    property string os: "win"
    property bool   online: false
    property bool   current: false
    property bool   warned: false
    property string seen: ""
    property string peerCode: ""

    signal connectRequested(string devId, string devName)
    signal renameCommitted(string devId, string newName)
    signal copyCodeRequested(string devId, string devName)
    signal removeRequested(string devId, string devName)
    signal verifyRequested(string devId, string devName)

    readonly property var osLabel: ({ "mac": "macOS 14.5", "win": "Windows 11", "linux": "Ubuntu 24.04" })

    property bool editing: false

    implicitWidth: 320
    implicitHeight: col.implicitHeight
    height: implicitHeight

    function _startEdit() {
        nameField.text = root.devName
        root.editing = true
        nameField.forceActiveFocus()
        nameField.selectAll()
    }
    function _commit() {
        var v = nameField.text.trim()
        root.editing = false
        if (v.length > 0 && v !== root.devName)
            root.renameCommitted(root.devId, v)
    }

    ColumnLayout {
        id: col
        width: parent.width
        spacing: 0

        // ── The row itself ──────────────────────────────────────────────
        Rectangle {
            id: rowRect
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            radius: 9
            color: rowHover.hovered && !root.editing ? root.pal.paperSoft : "transparent"

            HoverHandler { id: rowHover }
            // One-click connect on the row body (online, non-current only).
            TapHandler {
                enabled: root.online && !root.current && !root.editing
                onTapped: root.connectRequested(root.devId, root.devName)
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 12

                DeviceAvatar {
                    pal: root.pal
                    os: root.os
                    online: root.online
                    warned: root.warned
                    Layout.alignment: Qt.AlignVCenter
                }

                // Identity column
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    // Name row (or inline rename field)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        visible: !root.editing

                        Label {
                            text: root.devName
                            color: root.online ? root.pal.ink : root.pal.inkMid
                            font.family: root.pal.sans
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                            Layout.maximumWidth: 180
                        }
                        // "This device" tag
                        Rectangle {
                            visible: root.current
                            radius: 4
                            color: root.pal.paperDeep
                            border.width: 1
                            border.color: root.pal.hair
                            Layout.preferredHeight: 16
                            Layout.preferredWidth: thisLbl.implicitWidth + 12
                            Label {
                                id: thisLbl
                                anchors.centerIn: parent
                                text: "THIS DEVICE"
                                color: root.pal.inkMid
                                font.family: root.pal.mono
                                font.pixelSize: 8
                                font.letterSpacing: 0.6
                            }
                        }
                        // Key-changed pill
                        Rectangle {
                            visible: root.warned
                            radius: 4
                            color: root.pal.amberSoft
                            border.width: 1
                            border.color: root.pal.amberBorder
                            Layout.preferredHeight: 16
                            Layout.preferredWidth: warnRow.implicitWidth + 10
                            Row {
                                id: warnRow
                                anchors.centerIn: parent
                                spacing: 4
                                Glyph { name: "warn"; size: 10; color: root.pal.amber; anchors.verticalCenter: parent.verticalCenter }
                                Label {
                                    text: "key changed"
                                    color: root.pal.amber
                                    font.family: root.pal.mono
                                    font.pixelSize: 9
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }

                    // Inline rename field
                    Rectangle {
                        visible: root.editing
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: 170
                        radius: 6
                        color: root.pal.paper
                        border.width: 1
                        border.color: root.pal.ink
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 3
                            spacing: 6
                            TextField {
                                id: nameField
                                Layout.fillWidth: true
                                background: null
                                padding: 0
                                color: root.pal.ink
                                font.family: root.pal.sans
                                font.pixelSize: 13
                                font.weight: Font.Medium
                                selectByMouse: true
                                onAccepted: root._commit()
                                Keys.onEscapePressed: root.editing = false
                            }
                            Rectangle {
                                Layout.preferredWidth: 20
                                Layout.preferredHeight: 20
                                radius: 4
                                color: root.pal.ink
                                Glyph { anchors.centerIn: parent; name: "check"; size: 12; color: root.pal.paper }
                                TapHandler { onTapped: root._commit() }
                            }
                        }
                    }

                    // Meta line
                    Label {
                        Layout.fillWidth: true
                        text: root.osLabel[root.os] + "  ·  "
                              + (root.current ? "This device"
                                 : root.online ? "Online now"
                                 : "Last seen " + root.seen)
                        color: root.pal.inkMid
                        font.family: root.pal.mono
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }

                // ── Right-side actions ─────────────────────────────────
                RowLayout {
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 4

                    // Connect button (online, non-current)
                    Rectangle {
                        visible: root.online && !root.current && !root.editing
                        Layout.preferredHeight: 28
                        Layout.preferredWidth: connRow.implicitWidth + 22
                        radius: 7
                        color: connHover.containsMouse ? root.pal.ink : root.pal.paper
                        border.width: 1
                        border.color: connHover.containsMouse ? root.pal.ink : root.pal.hairStrong
                        Row {
                            id: connRow
                            anchors.centerIn: parent
                            spacing: 6
                            Glyph { name: "connect"; size: 13
                                    color: connHover.containsMouse ? root.pal.paper : root.pal.ink
                                    anchors.verticalCenter: parent.verticalCenter }
                            Label {
                                text: "Connect"
                                color: connHover.containsMouse ? root.pal.paper : root.pal.ink
                                font.family: root.pal.sans
                                font.pixelSize: 13
                                font.weight: Font.Medium
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        MouseArea {
                            id: connHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.connectRequested(root.devId, root.devName)
                        }
                    }

                    // Offline tag
                    Label {
                        visible: !root.online && !root.current
                        text: "OFFLINE"
                        color: root.pal.inkFaint
                        font.family: root.pal.mono
                        font.pixelSize: 10
                        font.letterSpacing: 0.4
                        rightPadding: 6
                    }

                    // Pencil (current device inline rename)
                    Rectangle {
                        visible: root.current && !root.editing
                        Layout.preferredHeight: 28
                        Layout.preferredWidth: 28
                        radius: 7
                        color: pencilHover.containsMouse ? root.pal.paperDeep : "transparent"
                        opacity: rowHover.hovered ? 1.0 : 0.55
                        Glyph { anchors.centerIn: parent; name: "pencil"; size: 13
                                color: pencilHover.containsMouse ? root.pal.ink : root.pal.inkMid }
                        MouseArea {
                            id: pencilHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root._startEdit()
                        }
                    }

                    // ⋯ menu (non-current)
                    Rectangle {
                        id: kebab
                        visible: !root.current && !root.editing
                        Layout.preferredHeight: 28
                        Layout.preferredWidth: 28
                        radius: 7
                        color: kebabHover.containsMouse ? root.pal.paperDeep : "transparent"
                        opacity: (rowHover.hovered || menu.opened) ? 1.0 : 0.0
                        Behavior on opacity { NumberAnimation { duration: 100 } }
                        Row {
                            anchors.centerIn: parent
                            spacing: 2.5
                            Repeater {
                                model: 3
                                Rectangle { width: 3; height: 3; radius: 1.5
                                            color: kebabHover.containsMouse ? root.pal.ink : root.pal.inkMid
                                            anchors.verticalCenter: parent.verticalCenter }
                            }
                        }
                        MouseArea {
                            id: kebabHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: menu.open()
                        }

                        DeviceRowMenu {
                            id: menu
                            pal: root.pal
                            y: kebab.height + 2
                            x: kebab.width - width
                            onRename:     root._startEdit()
                            onCopyCode:   root.copyCodeRequested(root.devId, root.devName)
                            onRemove:     root.removeRequested(root.devId, root.devName)
                        }
                    }
                }
            }
        }

        // ── Offline footnote ────────────────────────────────────────────
        Label {
            visible: !root.online && !root.current
            Layout.fillWidth: true
            Layout.leftMargin: 46
            Layout.bottomMargin: 4
            text: "Device is offline. It will appear available next time it connects."
            color: root.pal.inkFaint
            font.family: root.pal.mono
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }

        // ── Key-changed warning note ────────────────────────────────────
        RowLayout {
            visible: root.warned
            Layout.fillWidth: true
            Layout.leftMargin: 46
            Layout.bottomMargin: 6
            Layout.topMargin: 2
            spacing: 8

            Glyph { name: "shield"; size: 13; color: root.pal.amber; Layout.alignment: Qt.AlignTop; Layout.topMargin: 2 }
            Label {
                Layout.fillWidth: true
                text: "Identity key changed on this device. If you didn't reinstall Vivora here, remove it."
                color: root.pal.amber
                font.family: root.pal.mono
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
            Row {
                spacing: 10
                Layout.alignment: Qt.AlignTop
                Label {
                    text: "Verify"
                    color: root.pal.amber
                    font.family: root.pal.sans
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    font.underline: true
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: root.verifyRequested(root.devId, root.devName) }
                }
                Label {
                    text: "Remove"
                    color: root.pal.amber
                    font.family: root.pal.sans
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    font.underline: true
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: root.removeRequested(root.devId, root.devName) }
                }
            }
        }
    }
}
