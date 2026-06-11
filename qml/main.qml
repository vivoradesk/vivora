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

    color: "#efece3"

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
        function onConnectionApprovalRequested(key, peerCode, pubkeyHex, ipPort,
                                               recognized, seenCount, deviceName) {
            // Queue the request and surface the window.  Concurrent
            // pending peers are handled one at a time via the FIFO in
            // approvalLoader — the dialog shows a "N more waiting" badge.
            approvalLoader.enqueue({ key: key, peerCode: peerCode,
                                     pubkeyHex: pubkeyHex, ipPort: ipPort,
                                     recognized: recognized, seenCount: seenCount,
                                     deviceName: deviceName })
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
        // FIFO queue of peers awaiting approval.  The dialog renders the
        // head (`current`); approve/reject advances to the next without
        // tearing the popup down, so a burst of incoming peers is handled
        // one at a time rather than the latest clobbering the rest.
        property var queue: []
        property var current: ({ key: "", peerCode: "", pubkeyHex: "", ipPort: "",
                                 recognized: false, seenCount: 0, deviceName: "" })

        function enqueue(item) {
            var q = queue.slice()
            q.push(item)
            queue = q
            if (!active)
                advance()
        }
        // Pop the head into `current`.  Deactivating the Loader (queue
        // drained) closes the dialog.
        function advance() {
            if (queue.length === 0) {
                active = false
                return
            }
            var q = queue.slice()
            current = q.shift()
            queue = q
            active = true
        }

        sourceComponent: ConnectionApprovalDialog {
            visible: true
            approvalKey: approvalLoader.current.key
            peerCode:    approvalLoader.current.peerCode
            pubkeyHex:   approvalLoader.current.pubkeyHex
            ipPort:      approvalLoader.current.ipPort
            recognized:  approvalLoader.current.recognized
            seenCount:   approvalLoader.current.seenCount
            deviceName:  approvalLoader.current.deviceName
            morePending: approvalLoader.queue.length
            onApproved: (key, remember, gInput, gClip, gFile) => {
                App.approveConnection(key, remember, gInput, gClip, gFile)
                approvalLoader.advance()
            }
            onDeclined: (key) => { App.rejectConnection(key); approvalLoader.advance() }
        }
    }

    // ── Brand colours + helpers ──────────────────────────────────────
    QtObject {
        id: theme
        readonly property color bg:        "#efece3"   // unified with Settings palette
        readonly property color hostBg:    "#e8e3d6"   // sharing card — slightly darker
        readonly property color hoverBg:   "#ded8c8"   // subtle highlight on hover
        readonly property color rowHover:  "#ded8c8"   // recent-list row hover
        readonly property color text:      "#1a1a1f"
        readonly property color textMuted: "#6f6b60"
        readonly property color border:    "#d4cdba"
        readonly property color pillBg:    "#1a1a1f"
        readonly property color pillFg:    "#ffffff"
        readonly property color accent:    "#3D6BFA"   // brand blue
        readonly property color sharing:   "#22a85c"   // green for active session
        readonly property color error:     "#dc3545"   // red
        readonly property color selected:  "#ded8c8"   // subtle active/selected fill
        readonly property string monoFont: "JetBrains Mono, Cascadia Mono, Consolas, monospace"
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
            // Pro badge (VIV-29) — shown when a valid Pro license is loaded.
            Rectangle {
                visible: App.licensePro
                radius: height / 2
                color: theme.accent
                Layout.preferredHeight: 22
                Layout.preferredWidth: proBadge.implicitWidth + 18
                Label {
                    id: proBadge
                    anchors.centerIn: parent
                    text: "PRO"
                    font.pixelSize: 10
                    font.bold: true
                    font.letterSpacing: 1
                    color: "#ffffff"
                }
            }
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
                // "WAITING" badge — no countdown until ephemeral code
                // rotation lands server-side (VIV-XX).  For now just
                // indicates that we're listening with no client.
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
                            onClicked: {
                                clipboardHelper.copy(App.myPeerCode)
                                window.showToast("Code copied")
                            }
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
                        onClicked: {
                            clipboardHelper.copy(App.myPubkeyHex)
                            window.showToast("Pubkey copied")
                        }
                    }
                }
            }

            // Pause button.  Link (needs web + vivora:// deeplink) and QR
            // (needs the mobile client + feature/qr-overlay merge) are
            // deferred — shipping dead buttons confuses users, so the
            // action row is just Pause until those backends land.
            AppButton {
                Layout.fillWidth: true
                visible: App.sharing
                glyph: "⏸"
                label: "Pause sharing"
                onClicked: App.stopSharing()
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

        // Divider — sets the Recent list (which mixes outgoing ↑ and
        // incoming ↓ peers) apart from the "connect to a peer" input above.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            Layout.topMargin: 2
            color: theme.border
            visible: App.peers.rowCount() > 0
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

    // Transient toast — bottom-centre confirmation for actions that
    // otherwise give no visual feedback (copy, refresh).  showToast(text)
    // pops it for ~1.6s then fades.
    function showToast(text) {
        toast.text = text
        toast.opacity = 1.0
        toastTimer.restart()
    }
    Rectangle {
        id: toast
        property alias text: toastLabel.text
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 56
        width: toastLabel.implicitWidth + 28
        height: toastLabel.implicitHeight + 16
        radius: height / 2
        color: "#15151a"
        opacity: 0.0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: 250 } }
        Label {
            id: toastLabel
            anchors.centerIn: parent
            color: "#ffffff"
            font.pixelSize: 12
        }
        Timer {
            id: toastTimer
            interval: 1600
            onTriggered: toast.opacity = 0.0
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
