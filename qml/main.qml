import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: window
    width: 495
    height: 700
    minimumWidth: 420
    minimumHeight: 560
    visible: true
    title: "Vivora"

    color: "#ede6d4"

    // The app stays running in the tray; closing the window only hides it.
    onClosing: (close) => {
        if (App.settings.minimizeToTray) {
            close.accepted = false
            window.hide()
        }
    }

    Connections {
        target: App
        function onShowWindowRequested() {
            window.show()
            window.raise()
            window.requestActivate()
        }
        function onSettingsRequested() {
            settingsLoader.active = true
        }
        function onConnectionApprovalRequested(key, peerCode, pubkeyHex, ipPort) {
            // Raise the dialog on top.  Multiple concurrent pending
            // clients would race to share the single Loader — for now
            // the latest request wins (rare in practice; multi-pending
            // is Phase D territory along with the queue UI).
            approvalLoader.approvalKey = key
            approvalLoader.peerCode    = peerCode
            approvalLoader.pubkeyHex   = pubkeyHex
            approvalLoader.ipPort      = ipPort
            approvalLoader.active      = true
            window.show()
            window.raise()
            window.requestActivate()
        }
    }

    Loader {
        id: settingsLoader
        active: false
        sourceComponent: SettingsDialog {
            visible: true
            onClosed: settingsLoader.active = false
        }
    }
    Loader {
        id: addPeerLoader
        active: false
        property string pendingCode: ""
        property string pendingPubkey: ""
        sourceComponent: AddPeerDialog {
            visible: true
            initialCode: addPeerLoader.pendingCode
            initialPubkey: addPeerLoader.pendingPubkey
            onClosed: addPeerLoader.active = false
        }
    }
    Loader {
        id: approvalLoader
        active: false
        property string approvalKey: ""
        property string peerCode:    ""
        property string pubkeyHex:   ""
        property string ipPort:      ""
        sourceComponent: ConnectionApprovalDialog {
            visible: true
            approvalKey: approvalLoader.approvalKey
            peerCode:    approvalLoader.peerCode
            pubkeyHex:   approvalLoader.pubkeyHex
            ipPort:      approvalLoader.ipPort
            onApproved: (key) => App.approveConnection(key)
            onRejected: (key) => App.rejectConnection(key)
            onClosed:   approvalLoader.active = false
        }
    }

    // ── Brand colours + helpers ──────────────────────────────────────
    QtObject {
        id: theme
        readonly property color bg:        "#ede6d4"
        readonly property color hostBg:    "#e6dec8"   // sharing card — slightly darker, visually separates host area
        readonly property color hoverBg:   "#dcd2b4"   // subtle highlight on hover (matches bg family)
        readonly property color rowHover:  "#dcd2b4"   // recent-list row hover
        readonly property color text:      "#15151a"
        readonly property color textMuted: "#6b6b75"
        readonly property color border:    "#cfc6a9"
        readonly property color pillBg:    "#15151a"
        readonly property color pillFg:    "#ffffff"
        readonly property color accent:    "#3D6BFA"   // brand blue
        readonly property color sharing:   "#22a85c"   // green for active session
        readonly property color error:     "#dc3545"   // red
        readonly property string monoFont: "Geist Mono, JetBrains Mono, Cascadia Mono, Consolas, monospace"
    }

    // Reusable component: a flat button with custom Rectangle background
    // so it matches the design (defaults Qt buttons are white pills with
    // their own shadow which clash with the cream bg).  Uses internal
    // MouseArea for hover + click.
    component AppButton: Rectangle {
        property string glyph: ""
        property string label: ""
        property bool primary: false
        property color baseColor: theme.bg   // matches main bg by default
        signal clicked
        Layout.preferredHeight: 36
        radius: 7
        color: hoverArea.containsMouse
               ? (primary ? "#2456cf" : theme.hoverBg)
               : (primary ? theme.accent : baseColor)
        border.color: primary ? "transparent" : theme.border
        border.width: 1
        RowLayout {
            anchors.centerIn: parent
            spacing: 6
            Label {
                text: glyph
                visible: glyph.length > 0
                color: primary ? "#ffffff" : theme.text
                font.pixelSize: 13
            }
            Label {
                text: label
                visible: label.length > 0
                color: primary ? "#ffffff" : theme.text
                font.pixelSize: 13
            }
        }
        MouseArea {
            id: hoverArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: parent.clicked()
        }
    }

    function formatFingerprint(hex) {
        if (!hex || hex.length < 12) return hex || "—"
        return "ED25:" + hex.substring(0, 6) + "…" + hex.substring(hex.length - 4)
    }

    // ── Layout ───────────────────────────────────────────────────────
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 14

        // ── Header: logo + name + status badge ───────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Image {
                source: "qrc:/icons/app/32.png"
                sourceSize.width: 22; sourceSize.height: 22
                Layout.preferredWidth: 22; Layout.preferredHeight: 22
            }
            Label {
                text: "Vivora"
                font.pixelSize: 18
                font.bold: true
                color: theme.text
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                visible: App.sharing
                radius: height / 2
                color: theme.selected
                border.color: theme.border
                Layout.preferredHeight: 22
                Layout.preferredWidth: badge.implicitWidth + 18
                RowLayout {
                    id: badge
                    anchors.centerIn: parent
                    spacing: 5
                    Rectangle {
                        width: 7; height: 7; radius: 4
                        color: theme.accent
                    }
                    Label {
                        text: "SHARING"
                        font.pixelSize: 10
                        font.bold: true
                        font.letterSpacing: 1
                        color: theme.accent
                    }
                }
            }
        }

        // ── Sharing section ──────────────────────────────────────────
        // Wrapper Rectangle gives the "host card" its own subtly darker
        // background so it reads as a distinct zone from the "connect
        // to a peer" half below.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: hostCard.implicitHeight + 24
            visible: App.sharing
            color: theme.hostBg
            radius: 10

            ColumnLayout {
                id: hostCard
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Label {
                    text: "⇡"
                    color: theme.textMuted
                    font.pixelSize: 12
                }
                Label {
                    text: "SHARING THIS DESKTOP"
                    color: theme.textMuted
                    font.pixelSize: 11
                    font.letterSpacing: 1
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                // WAITING countdown — Phase D will tie this to ephemeral
                // code rotation TTL.  Placeholder for now.
                RowLayout {
                    visible: App.sharing && App.clientCount === 0
                    spacing: 5
                    Rectangle {
                        width: 6; height: 6; radius: 3
                        color: theme.accent
                    }
                    Label {
                        text: "WAITING"
                        color: theme.text
                        font.pixelSize: 10
                        font.letterSpacing: 1
                        font.bold: true
                    }
                    Label {
                        text: "·"
                        color: theme.textMuted
                    }
                    Label {
                        text: "—"  // placeholder until Phase D
                        color: theme.textMuted
                        font.pixelSize: 11
                        font.family: theme.monoFont
                    }
                }
                RowLayout {
                    visible: App.sharing && App.clientCount > 0
                    spacing: 5
                    Rectangle {
                        width: 6; height: 6; radius: 3
                        color: theme.sharing
                    }
                    Label {
                        text: App.clientCount === 1 ? "1 CLIENT" : App.clientCount + " CLIENTS"
                        color: theme.sharing
                        font.pixelSize: 10
                        font.letterSpacing: 1
                        font.bold: true
                    }
                }
            }

            // Peer code pill
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: codeRow.implicitHeight + 22
                visible: App.sharing
                color: theme.pillBg
                radius: 10
                RowLayout {
                    id: codeRow
                    anchors.fill: parent
                    anchors.margins: 11
                    spacing: 8
                    Label {
                        text: App.myPeerCode || "…"
                        color: theme.pillFg
                        font.family: theme.monoFont
                        font.pixelSize: 17
                        font.bold: true
                        wrapMode: Text.WrapAnywhere
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.preferredWidth: 30; Layout.preferredHeight: 30
                        radius: 6
                        color: copyCodeArea.containsMouse ? "#2a2a32" : "transparent"
                        Label {
                            anchors.centerIn: parent
                            text: "⧉"
                            color: theme.pillFg
                            font.pixelSize: 14
                        }
                        MouseArea {
                            id: copyCodeArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: clipboardHelper.copy(App.myPeerCode)
                        }
                    }
                }
            }

            // Pubkey fingerprint row
            RowLayout {
                Layout.fillWidth: true
                visible: App.sharing
                Label {
                    text: "PUBKEY"
                    color: theme.textMuted
                    font.pixelSize: 10
                    font.letterSpacing: 1
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: formatFingerprint(App.myPubkeyHex)
                    color: theme.text
                    font.family: theme.monoFont
                    font.pixelSize: 12
                }
                Rectangle {
                    Layout.preferredWidth: 24; Layout.preferredHeight: 22
                    radius: 5
                    color: copyKeyArea.containsMouse ? theme.hoverBg : "transparent"
                    Label { anchors.centerIn: parent; text: "⧉"; color: theme.textMuted; font.pixelSize: 12 }
                    MouseArea {
                        id: copyKeyArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: clipboardHelper.copy(App.myPubkeyHex)
                    }
                }
            }

            // 4-button action row: Link / QR / Refresh / Pause
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                visible: App.sharing
                Repeater {
                    model: [
                        { glyph: "🔗", label: "Link",    width: 0 },
                        { glyph: "▦",  label: "QR",      width: 0 },
                        { glyph: "↻",  label: "",        width: 44 },
                        { glyph: "⏸",  label: "",        width: 44 }
                    ]
                    delegate: AppButton {
                        Layout.fillWidth: modelData.width === 0
                        Layout.preferredWidth: modelData.width || -1
                        glyph: modelData.glyph
                        label: modelData.label
                        onClicked: {
                            if (modelData.label === "Link") {
                                // Phase E will land a real short URL.  For
                                // now just copy a vivora.dev/c/<code>
                                // placeholder so users get something useful.
                                clipboardHelper.copy("https://vivora.dev/c/" + App.myPeerCode)
                            } else if (modelData.glyph === "⏸") {
                                // Pause = deregister from rendezvous + tear
                                // down host loop.  Resume via the CTA that
                                // replaces this card when App.sharing is
                                // false.
                                App.stopSharing()
                            }
                            // QR (Phase E) and Refresh (Phase D) stay
                            // no-op until those phases land.
                        }
                    }
                }
            }

            }  // hostCard ColumnLayout
        }      // Sharing card Rectangle

        // Paused-state CTA — host starts at launch (always-available
        // model), so App.sharing=false only happens when the user hit
        // the Pause button in the sharing card or stopped via tray.
        // Resume re-registers with rendezvous + spins the host loop
        // back up using the same identity.
        Rectangle {
            visible: !App.sharing
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            color: theme.hostBg
            radius: 10
            ColumnLayout {
                anchors.centerIn: parent
                spacing: 4
                Label {
                    text: "Sharing paused"
                    color: theme.text
                    font.bold: true
                    Layout.alignment: Qt.AlignHCenter
                }
                Label {
                    text: "Click below to start receiving connections again"
                    color: theme.textMuted
                    font.pixelSize: 11
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }
        AppButton {
            visible: !App.sharing
            Layout.fillWidth: true
            label: "▶  Resume sharing"
            primary: true
            onClicked: App.startSharing()
        }

        // Subtle divider
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: theme.border
        }

        // ── Connect section ──────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label { text: "⇣"; color: theme.textMuted; font.pixelSize: 12 }
            Label {
                text: "CONNECT TO A PEER"
                color: theme.textMuted
                font.pixelSize: 11
                font.letterSpacing: 1
                font.bold: true
            }
        }

        TextField {
            id: peerInput
            Layout.fillWidth: true
            placeholderText: "peer code (e.g. swift-tiger-4271)"
            placeholderTextColor: theme.textMuted
            font.family: theme.monoFont
            font.pixelSize: 13
            color: theme.text
            // selectByMouse + selectionColor → keep selection legible on
            // the warm-bg theme (default Qt palette picks blue that
            // clashes).
            selectByMouse: true
            selectionColor: theme.accent
            selectedTextColor: "#ffffff"
            background: Rectangle {
                color: theme.bg
                border.color: peerInput.activeFocus ? theme.accent : theme.border
                border.width: 1
                radius: 7
            }
            onAccepted: {
                if (text.length > 0) {
                    App.connectToPeer(text)
                    text = ""
                }
            }
        }

        // Recent label + count
        RowLayout {
            Layout.fillWidth: true
            visible: App.peers.rowCount() > 0
            Label {
                text: "RECENT"
                color: theme.textMuted
                font.pixelSize: 11
                font.letterSpacing: 1
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            Label {
                text: App.peers.rowCount()
                color: theme.textMuted
                font.pixelSize: 11
            }
        }

        AddressBookView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            onPeerActivated: (alias, pubkey, code) => {
                App.connectToPeer(pubkey.length > 0 ? pubkey : code)
            }
        }

        // ── Footer ───────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            AppButton {
                glyph: "⚙"
                label: "Settings"
                Layout.preferredWidth: 100
                onClicked: App.openSettings()
            }
            Item { Layout.fillWidth: true }
            AppButton {
                label: "Hide"
                Layout.preferredWidth: 80
                onClicked: window.hide()
            }
        }
    }

    // Clipboard shim (Qt 6 lacks a direct QML clipboard primitive)
    Item {
        id: clipboardHelper
        function copy(text) {
            clipText.text = text
            clipText.selectAll()
            clipText.copy()
        }
        TextEdit { id: clipText; visible: false }
    }
}
