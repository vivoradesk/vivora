import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Settings → Account → My Devices (devices.jsx MyDevicesSettings).  Account
// strip + tier badge, optional trial banner, then either the device table or
// the ProGate (free tier), and the remote-removal footnote.
//
// Data/tier from the isolated DeviceMockModel.  `accountName` / `accountEmail`
// default to the mock but can be fed the real signed-in identity by the
// parent (see SettingsDialog) — the device rows themselves stay mock until the
// VIV-52 backend lands.
ColumnLayout {
    id: pane
    property DevicePalette pal: DevicePalette {}
    property DeviceMockModel mock: DeviceMockModel {}

    property string accountName: "Maxim Kozlov"
    property string accountEmail: "maxim@vivora.dev"

    signal notify(string message)
    signal upgradeRequested()

    spacing: 0
    readonly property string tier: mock.tier

    function _initials(name, email) {
        var src = (name && name.length > 0) ? name : (email || "?")
        var parts = src.replace(/@.*$/, "").split(/[\s._-]+/).filter(function (p) { return p.length > 0 })
        if (parts.length === 0) return "?"
        if (parts.length === 1) return parts[0].substring(0, 2).toUpperCase()
        return (parts[0][0] + parts[1][0]).toUpperCase()
    }

    // ── Title ───────────────────────────────────────────────────────────
    Label {
        text: "My Devices"
        color: pane.pal.ink
        font.family: pane.pal.sans
        font.pixelSize: 16
        font.weight: Font.DemiBold
    }
    Label {
        Layout.fillWidth: true
        Layout.topMargin: 4
        Layout.bottomMargin: 16
        text: "Devices signed in to your Vivora account connect to each other instantly — "
              + "no codes, no approval."
        color: pane.pal.inkMid
        font.family: pane.pal.sans
        font.pixelSize: 13
        wrapMode: Text.WordWrap
    }

    // ── Account strip ───────────────────────────────────────────────────
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 72
        radius: 12
        color: pane.pal.paperSoft
        border.width: 1
        border.color: pane.pal.hair
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 14
            Rectangle {
                Layout.preferredWidth: 44; Layout.preferredHeight: 44
                radius: 12
                color: pane.pal.ink
                Label {
                    anchors.centerIn: parent
                    text: pane._initials(pane.accountName, pane.accountEmail)
                    color: pane.pal.paper
                    font.family: pane.pal.sans
                    font.pixelSize: 16
                    font.weight: Font.Medium
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: pane.accountName
                    color: pane.pal.ink
                    font.family: pane.pal.sans
                    font.pixelSize: 14
                    font.weight: Font.Medium
                }
                Label {
                    text: pane.accountEmail
                    color: pane.pal.inkMid
                    font.family: pane.pal.mono
                    font.pixelSize: 11
                }
            }
            TierBadge {
                pal: pane.pal
                tier: pane.tier === "trial" ? "trial" : pane.tier === "free" ? "locked" : "pro"
                text: pane.tier === "trial" ? "Trial" : pane.tier === "free" ? "Free" : "Pro"
            }
        }
    }

    // ── Trial banner ────────────────────────────────────────────────────
    Rectangle {
        visible: pane.tier === "trial"
        Layout.fillWidth: true
        Layout.topMargin: 16
        Layout.preferredHeight: 52
        radius: 10
        color: pane.pal.amberSoft
        border.width: 1
        border.color: pane.pal.amberBorder
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            spacing: 12
            Glyph { name: "spark"; size: 14; color: pane.pal.amber; Layout.alignment: Qt.AlignVCenter }
            Label {
                Layout.fillWidth: true
                text: "<b>Trial — 9 days left.</b> You're on the Pro trial. Keep one-click device access after it ends."
                textFormat: Text.RichText
                color: pane.pal.inkSoft
                font.family: pane.pal.sans
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            Rectangle {
                Layout.preferredHeight: 30
                Layout.preferredWidth: upLbl.implicitWidth + 24
                radius: 7
                color: upHover.containsMouse ? pane.pal.inkSoft : pane.pal.ink
                Label { id: upLbl; anchors.centerIn: parent; text: "Upgrade"
                        color: pane.pal.paper; font.family: pane.pal.sans
                        font.pixelSize: 13; font.weight: Font.Medium }
                MouseArea { id: upHover; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor; onClicked: pane.upgradeRequested() }
            }
        }
    }

    // ── Free → ProGate ──────────────────────────────────────────────────
    ProGate {
        visible: pane.tier === "free"
        Layout.fillWidth: true
        Layout.topMargin: 4
        pal: pane.pal
        onStartTrialRequested: pane.notify("Starting your 14-day Pro trial…")
    }

    // ── Table (pro / trial) ─────────────────────────────────────────────
    Rectangle {
        id: tableFrame
        visible: pane.tier !== "free"
        Layout.fillWidth: true
        Layout.topMargin: 16
        Layout.preferredHeight: tableCol.implicitHeight
        radius: 10
        color: "transparent"
        border.width: 1
        border.color: pane.pal.hair
        clip: true

        // Column widths sized to fit the Settings pane without horizontal
        // overflow (Device takes the flexible remainder).  Actions is wide
        // enough for the Rename + Remove buttons so they never spill past the
        // pane edge or collide with the Status column.
        readonly property real osCol: 120
        readonly property real statusCol: 84
        readonly property real actionsCol: 128

        ColumnLayout {
            id: tableCol
            width: parent.width
            spacing: 0

            // Header
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                color: pane.pal.paperSoft
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 12
                    Label { text: "DEVICE"; Layout.fillWidth: true; elide: Text.ElideRight
                            color: pane.pal.inkMid
                            font.family: pane.pal.mono; font.pixelSize: 10; font.letterSpacing: 0.6 }
                    Label { text: "OPERATING SYSTEM"; Layout.preferredWidth: tableFrame.osCol
                            elide: Text.ElideRight
                            color: pane.pal.inkMid; font.family: pane.pal.mono
                            font.pixelSize: 10; font.letterSpacing: 0.6 }
                    Label { text: "STATUS"; Layout.preferredWidth: tableFrame.statusCol
                            elide: Text.ElideRight
                            color: pane.pal.inkMid; font.family: pane.pal.mono
                            font.pixelSize: 10; font.letterSpacing: 0.6 }
                    Label { text: "ACTIONS"; Layout.preferredWidth: tableFrame.actionsCol
                            horizontalAlignment: Text.AlignRight
                            color: pane.pal.inkMid; font.family: pane.pal.mono
                            font.pixelSize: 10; font.letterSpacing: 0.6 }
                }
            }
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: pane.pal.hair }

            // Rows
            Repeater {
                model: pane.mock.settingsModel
                SettingsDeviceRow {
                    Layout.fillWidth: true
                    pal: pane.pal
                    osColWidth: tableFrame.osCol
                    statusColWidth: tableFrame.statusCol
                    actionsColWidth: tableFrame.actionsCol
                    devId: model.devId
                    devName: model.devName
                    os: model.os
                    online: model.online
                    current: model.current
                    warned: model.warned
                    seen: model.seen
                    host: model.host
                    onRenameCommitted: (id, name) => pane.mock.renameMain(id, name)
                    onRemoveRequested: (id, name) => { pane.mock.removeMain(id); pane.notify("Removed " + name) }
                }
            }
        }
    }

    // ── Footnote ────────────────────────────────────────────────────────
    Label {
        visible: pane.tier !== "free"
        Layout.fillWidth: true
        Layout.topMargin: 14
        text: "Removing a device signs it out of your account remotely and ends any active sessions."
        color: pane.pal.inkFaint
        font.family: pane.pal.mono
        font.pixelSize: 11
        wrapMode: Text.WordWrap
        lineHeight: 1.4
    }
}
