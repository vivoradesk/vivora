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

    signal connectRequested(string devName)
    signal copyPeerCode(string code, string devName)
    signal notify(string message)

    spacing: 0

    readonly property int deviceCount: mock.model.count
    readonly property bool isCached: mock.variant === "cached"
    readonly property bool isFree: mock.variant === "free"
    readonly property bool isEmpty: mock.variant === "empty"
    readonly property bool scroll: deviceCount > 5

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
        Layout.bottomMargin: 10
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
        onStartTrialRequested: block.notify("Starting your 14-day Pro trial…")
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
        Layout.preferredHeight: block.scroll ? 268 : contentHeight
        interactive: block.scroll
        clip: true
        spacing: 1
        opacity: block.isCached ? 0.55 : 1.0
        model: block.mock.model
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: block.scroll ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }

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

            onConnectRequested: (id, name) => block.connectRequested(name)
            onRenameCommitted:  (id, name) => block.mock.renameMain(id, name)
            onCopyCodeRequested: (id, name) => block.copyPeerCode(model.peerCode, name)
            onRemoveRequested:  (id, name) => { block.mock.removeMain(id); block.notify("Removed " + name) }
            onVerifyRequested:  (id, name) => block.notify("Verifying " + name + "…")
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
