// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Main-window "My Devices" block (devices.jsx MyDevicesBlock).  Header (title +
// count + tier badge, or an UPDATING spinner in the cached state) over one of:
// device list (pro / trial / cached) · empty state · ProGate (free).
//
// Data + tier come from the isolated DeviceMockModel — see that file for how
// to swap in the real backend.  Intents bubble up as signals for main.qml to
// wire to toasts / the real connect path.
ColumnLayout {
    id: block
    property DevicePalette pal: DevicePalette {}

    // Instantiated here so this surface is fully self-contained; SettingsDialog
    // makes its own instance.  Both read the same env-driven variant.
    property DeviceMockModel mock: DeviceMockModel {}

    signal connectRequested(string peerCode, string pubkey, string devName)
    signal copyPeerCode(string code, string devName)
    signal notify(string message)
    signal upgradeRequested()

    spacing: 0

    readonly property int deviceCount: mock.model.count
    readonly property bool isCached: mock.variant === "cached"
    readonly property bool isFree: mock.variant === "free"
    readonly property bool isEmpty: mock.variant === "empty"
    readonly property bool scroll: deviceCount > 5
    // True when the device list (not the ProGate / empty state) is showing —
    // main.qml binds the block's fillHeight to this so the list can flex and
    // scroll internally without leaving a gap under the free/empty cards.
    readonly property bool showsList: !isFree && !isEmpty

    function _onlineCount() {
        var n = 0
        for (var i = 0; i < mock.model.count; ++i) {
            var d = mock.model.get(i)
            if (d.online && !d.current) n++
        }
        return n
    }

    // ── Header ──────────────────────────────────────────────────────────
    RowLayout {
        Layout.fillWidth: true
        Layout.bottomMargin: 6
        spacing: 8

        Glyph { name: "devices"; size: 13; color: block.pal.inkMid; Layout.alignment: Qt.AlignVCenter }
        Label {
            text: "MY DEVICES"
            color: block.pal.inkMid
            font.family: block.pal.mono
            font.pixelSize: 11
            font.letterSpacing: 0.6
        }
        // Count pill
        Rectangle {
            visible: !block.isFree && !block.isEmpty
            Layout.preferredHeight: 16
            Layout.preferredWidth: countLbl.implicitWidth + 12
            radius: 8
            color: block.pal.paperSoft
            border.width: 1
            border.color: block.pal.hair
            Label {
                id: countLbl
                anchors.centerIn: parent
                text: block.deviceCount
                color: block.pal.inkFaint
                font.family: block.pal.mono
                font.pixelSize: 10
            }
        }

        Item { Layout.fillWidth: true }

        // Refreshing indicator (cached)
        RowLayout {
            visible: block.isCached
            spacing: 6
            Item {
                Layout.preferredWidth: 11; Layout.preferredHeight: 11
                Layout.alignment: Qt.AlignVCenter
                Glyph {
                    id: spinGlyph
                    anchors.centerIn: parent
                    name: "refresh"; size: 11; color: block.pal.inkFaint
                    transformOrigin: Item.Center
                    RotationAnimation on rotation {
                        running: block.isCached
                        loops: Animation.Infinite
                        from: 0; to: 360; duration: 900
                    }
                }
            }
            Label {
                text: "UPDATING…"
                color: block.pal.inkFaint
                font.family: block.pal.mono
                font.pixelSize: 10
                font.letterSpacing: 0.4
            }
        }

        // Tier badge (pro / trial / free)  — hidden while cached.
        TierBadge {
            visible: !block.isCached && !block.isEmpty
            pal: block.pal
            tier: block.mock.variant === "trial" ? "trial"
                  : block.mock.variant === "free" ? "locked" : "pro"
            text: block.mock.variant === "trial" ? "Trial · 9 days left"
                  : block.mock.variant === "free" ? "Pro" : "Pro"
        }
    }

    // ── Body ────────────────────────────────────────────────────────────

    // Free → Pro gate
    ProGate {
        visible: block.isFree
        Layout.fillWidth: true
        pal: block.pal
        onUpgradeRequested: block.upgradeRequested()
    }

    // Empty → no-other-devices card
    Rectangle {
        visible: block.isEmpty
        Layout.fillWidth: true
        Layout.preferredHeight: emptyCol.implicitHeight + 44
        radius: 10
        color: block.pal.paperSoft
        border.width: 1
        border.color: block.pal.hairStrong
        // dashed-look approximation (QML Rectangle has no dashed border)
        ColumnLayout {
            id: emptyCol
            anchors.centerIn: parent
            width: parent.width - 36
            spacing: 4
            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                Layout.bottomMargin: 8
                Layout.preferredWidth: 40; Layout.preferredHeight: 40; radius: 10
                color: block.pal.paper
                border.width: 1; border.color: block.pal.hair
                Glyph { anchors.centerIn: parent; name: "devices"; size: 18; color: block.pal.inkMid }
            }
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "No other devices yet"
                color: block.pal.ink
                font.family: block.pal.sans
                font.pixelSize: 14
                font.weight: Font.Medium
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: "Sign in to Vivora on another computer with this account and it will "
                      + "appear here — ready to connect in one click."
                color: block.pal.inkMid
                font.family: block.pal.sans
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                lineHeight: 1.35
            }
        }
    }

    // List (pro / trial / cached)
    ListView {
        id: listView
        visible: !block.isFree && !block.isEmpty
        Layout.fillWidth: true
        // Sized to its rows, but capped at ~4 rows: beyond that the list scrolls
        // internally instead of pushing the rest of the window down.  Not
        // fillHeight, so a tall window leaves the slack to the bottom spacer
        // rather than stretching an empty gap into this block.
        readonly property int maxListHeight: 160
        Layout.preferredHeight: Math.min(contentHeight, maxListHeight)
        Layout.maximumHeight: maxListHeight
        interactive: contentHeight > maxListHeight
        clip: true
        spacing: 1
        opacity: block.isCached ? 0.55 : 1.0
        model: block.mock.model
        boundsBehavior: Flickable.StopAtBounds

        // One device row per wheel notch (the default flick skips ~2).
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (event) => {
                if (listView.count <= 0) return
                const rowH = listView.contentHeight / listView.count
                const notches = event.angleDelta.y / 120
                const maxY = Math.max(0, listView.contentHeight - listView.height)
                listView.contentY = Math.max(0, Math.min(maxY,
                                    listView.contentY - notches * rowH))
            }
        }

        ScrollBar.vertical: ScrollBar {
            id: devScroll
            policy: listView.contentHeight > listView.maxListHeight
                    ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            width: 10
            contentItem: Rectangle {
                implicitWidth: 6
                radius: 3
                color: block.pal.inkMid
                opacity: (devScroll.pressed || devScroll.hovered) ? 1.0 : 0.7
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }
        }

        delegate: DeviceRow {
            width: ListView.view.width
            pal: block.pal
            devId: model.devId
            devName: model.devName
            os: model.os
            online: model.online
            current: model.current
            warned: model.warned
            seen: model.seen
            peerCode: model.peerCode

            onConnectRequested: (id, name) => block.connectRequested(model.peerCode, model.pubkey, name)
            onRenameCommitted:  (id, name) => block.mock.renameMain(id, name)
            onCopyCodeRequested: (id, name) => block.copyPeerCode(model.peerCode, name)
            // Removal is consequential (signs the device out + kicks live
            // sessions) → confirm first, then remove on the dialog's OK.
            onRemoveRequested:  (id, name) => {
                removeConfirm.deviceId = id
                removeConfirm.deviceName = name
                removeConfirm.open()
            }
            onVerifyRequested:  (id, name) => block.notify("Verifying " + name + "…")
        }
    }

    // Confirm-before-remove (VIV-52).  Lives outside the ListView so it isn't
    // torn down with the delegate that triggered it.
    RemoveDeviceDialog {
        id: removeConfirm
        onConfirmed: {
            block.mock.removeMain(removeConfirm.deviceId)
            block.notify("Removed " + removeConfirm.deviceName)
        }
    }

    // Scroll footer
    Label {
        visible: block.scroll
        Layout.fillWidth: true
        Layout.topMargin: 8
        horizontalAlignment: Text.AlignRight
        text: (block._onlineCount() + 1) + " online · " + block.deviceCount + " total"
        color: block.pal.inkFaint
        font.family: block.pal.mono
        font.pixelSize: 10
    }
}
