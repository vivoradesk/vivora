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
        // VIV-23: outgoing connect paused on a TOFU trust question — show
        // the trust dialog (first-connect or red key-changed variant).
        function onTrustPromptRequested(peerCode, newFingerprint, oldFingerprint,
                                        mismatch) {
            trustLoader.peerCode       = peerCode
            trustLoader.newFingerprint = newFingerprint
            trustLoader.oldFingerprint = oldFingerprint
            trustLoader.mismatch       = mismatch
            trustLoader.active = true
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
    // VIV-23 outgoing TOFU trust prompt.  One at a time is fine — connects
    // are user-initiated, so there is no queue like the approval dialog's.
    Loader {
        id: trustLoader
        active: false
        property string peerCode: ""
        property string newFingerprint: ""
        property string oldFingerprint: ""
        property bool   mismatch: false
        sourceComponent: TrustPromptDialog {
            visible: true
            peerCode:       trustLoader.peerCode
            newFingerprint: trustLoader.newFingerprint
            oldFingerprint: trustLoader.oldFingerprint
            mismatch:       trustLoader.mismatch
            onTrusted:   { App.resolveTrustPrompt(true);  trustLoader.active = false }
            onDismissed: { App.resolveTrustPrompt(false); trustLoader.active = false }
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
            // Seed the Audio toggle from the global default (VIV-65).
            audioDefaultOn: App.settings.audioGrantDefault
            onApproved: (key, remember, gInput, gClip, gAudio, gFile) => {
                App.approveConnection(key, remember, gInput, gClip, gAudio, gFile)
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

        // ── Update banner (VIV-69) — shown when a newer build is published ──
        Rectangle {
            id: updateBanner
            property bool dismissed: false
            visible: App.updateAvailable && !dismissed
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            radius: 8
            color: Qt.rgba(0.24, 0.42, 0.98, 0.10)   // soft accent tint
            border.color: theme.accent
            border.width: 1
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 10
                spacing: 10
                Label {
                    text: "Update available — Vivora " + App.updateVersion
                    color: theme.text
                    font.pixelSize: 13
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                Rectangle {
                    Layout.preferredHeight: 26
                    Layout.preferredWidth: dlLbl.implicitWidth + 24
                    radius: 6
                    color: theme.accent
                    Label { id: dlLbl; anchors.centerIn: parent; text: "Download"
                            color: "#ffffff"; font.pixelSize: 12; font.bold: true }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: App.openDownloadPage() }
                }
                Label {
                    text: "✕"
                    color: theme.textMuted
                    font.pixelSize: 14
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: updateBanner.dismissed = true }
                }
            }
        }

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

        // ── My Devices (personal mesh) ───────────────────────────────
        // Front-end-ahead-of-backend: driven by the isolated DeviceMockModel
        // (inside MyDevicesBlock).  Renders the device list / empty state /
        // Pro gate per the mock's variant.  Preview any state with the
        // VIVORA_DEVICES_VARIANT env var (pro | trial | free | empty | cached).
        MyDevicesBlock {
            Layout.fillWidth: true
            onConnectRequested: (devName) => window.showToast("Connecting to " + devName + "…")
            onCopyPeerCode: (code, devName) => {
                clipboardHelper.copy(code)
                window.showToast("Peer code copied")
            }
            onNotify: (message) => window.showToast(message)
        }

        // Subtle divider below My Devices
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

    // ── Announcement modal (VIV-70) ──────────────────────────────────────
    Rectangle {
        anchors.fill: parent
        visible: App.announcementVisible
        color: "#88000000"            // scrim
        z: 2000
        MouseArea { anchors.fill: parent }   // swallow clicks behind the card

        Rectangle {
            id: annCard
            anchors.centerIn: parent
            width: Math.min(440, parent.width - 40)
            implicitHeight: annCol.implicitHeight + 36
            radius: 14
            color: theme.bg
            border.color: theme.border
            border.width: 1
            opacity: 0
            scale: 0.95
            transformOrigin: Item.Center

            // Pop-in on every content change (first show + each next in the queue).
            ParallelAnimation {
                id: annPop
                NumberAnimation { target: annCard; property: "opacity"; to: 1; duration: 150; easing.type: Easing.OutQuad }
                NumberAnimation { target: annCard; property: "scale"; to: 1; duration: 200; easing.type: Easing.OutBack; easing.overshoot: 1.5 }
            }
            Connections {
                target: App
                function onAnnouncementChanged() {
                    if (App.announcementVisible) { annCard.opacity = 0; annCard.scale = 0.95; annPop.restart() }
                }
            }

            ColumnLayout {
                id: annCol
                anchors.left: parent.left; anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 24
                spacing: 14

                // Rounded, clipped image with a little breathing room (top margin
                // leaves space for the close button in the corner).
                Rectangle {
                    visible: App.announcementImageUrl !== ""
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? 150 : 0
                    Layout.topMargin: visible ? 18 : 0   // clears the close button
                    radius: 10
                    clip: true
                    color: theme.hoverBg
                    Image {
                        anchors.fill: parent
                        source: App.announcementImageUrl
                        fillMode: Image.PreserveAspectCrop
                    }
                }
                Label {
                    text: App.announcementTitle
                    font.pixelSize: 18; font.bold: true; color: theme.text
                    wrapMode: Text.WordWrap; Layout.fillWidth: true; Layout.rightMargin: 16
                }
                Label {
                    text: App.announcementBody
                    visible: text !== ""
                    font.pixelSize: 13; color: theme.textMuted
                    wrapMode: Text.WordWrap; Layout.fillWidth: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    spacing: 8
                    Item { Layout.fillWidth: true }
                    Repeater {
                        model: App.announcementButtons
                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            Layout.preferredHeight: 34
                            Layout.preferredWidth: bLbl.implicitWidth + 28
                            radius: 8
                            color: index === 0
                                   ? (bMa.containsMouse ? Qt.darker(theme.accent, 1.15) : theme.accent)
                                   : (bMa.containsMouse ? Qt.darker(theme.hoverBg, 1.08) : theme.hoverBg)
                            border.color: index === 0 ? "transparent" : theme.border
                            border.width: 1
                            Label {
                                id: bLbl; anchors.centerIn: parent
                                text: modelData.label !== undefined ? modelData.label : "OK"
                                color: index === 0 ? "#ffffff" : theme.text
                                font.pixelSize: 13; font.bold: index === 0
                            }
                            MouseArea {
                                id: bMa
                                anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (modelData.url !== undefined && modelData.url !== "")
                                        App.openAnnouncementUrl(modelData.url)
                                    App.dismissAnnouncement()
                                }
                            }
                        }
                    }
                    Rectangle {                       // fallback when no buttons
                        visible: App.announcementButtons.length === 0
                        Layout.preferredHeight: 34
                        Layout.preferredWidth: gotLbl.implicitWidth + 28
                        radius: 8
                        color: gotMa.containsMouse ? Qt.darker(theme.accent, 1.15) : theme.accent
                        Label { id: gotLbl; anchors.centerIn: parent; text: "Got it"
                                color: "#ffffff"; font.pixelSize: 13; font.bold: true }
                        MouseArea { id: gotMa; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: App.dismissAnnouncement() }
                    }
                }
            }

            // Close ✕ — inside the card's top-right corner, sitting in the
            // padding above the content (the 24px margin + image top margin keep
            // it clear of the image / title).
            Rectangle {
                anchors.top: parent.top; anchors.right: parent.right
                anchors.topMargin: 12; anchors.rightMargin: 12
                width: 26; height: 26; radius: 7
                color: xMa.containsMouse ? theme.hoverBg : "transparent"
                Label {
                    anchors.centerIn: parent; text: "✕"; font.pixelSize: 13
                    color: xMa.containsMouse ? theme.text : theme.textMuted
                }
                MouseArea { id: xMa; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor; onClicked: App.dismissAnnouncement() }
            }
        }
    }

    // ── Poll modal (VIV-71) ──────────────────────────────────────────────
    Rectangle {
        id: pollRoot
        anchors.fill: parent
        visible: App.pollVisible
        color: "#88000000"
        z: 2000
        MouseArea { anchors.fill: parent }

        // local answer state
        property var selected: ({})        // option-id -> true
        property int rating: 0
        property bool submitted: false
        onVisibleChanged: if (visible) { selected = ({}); rating = 0; submitted = false; commentField.text = "" }

        Rectangle {
            id: pollCard
            anchors.centerIn: parent
            width: Math.min(440, parent.width - 40)
            implicitHeight: pollCol.implicitHeight + 48
            radius: 14
            color: theme.bg
            border.color: theme.border; border.width: 1
            opacity: 0; scale: 0.95; transformOrigin: Item.Center

            ParallelAnimation {
                id: pollPop
                NumberAnimation { target: pollCard; property: "opacity"; to: 1; duration: 150; easing.type: Easing.OutQuad }
                NumberAnimation { target: pollCard; property: "scale"; to: 1; duration: 200; easing.type: Easing.OutBack; easing.overshoot: 1.5 }
            }
            Connections {
                target: App
                function onPollChanged() { if (App.pollVisible) { pollCard.opacity = 0; pollCard.scale = 0.95; pollPop.restart() } }
            }

            ColumnLayout {
                id: pollCol
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                anchors.margins: 24
                spacing: 14

                Label {
                    text: App.pollQuestion
                    font.pixelSize: 18; font.bold: true; color: theme.text
                    wrapMode: Text.WordWrap; Layout.fillWidth: true; Layout.rightMargin: 16
                }
                Label {
                    text: App.pollBody; visible: text !== ""
                    font.pixelSize: 13; color: theme.textMuted
                    wrapMode: Text.WordWrap; Layout.fillWidth: true
                }

                // ── input area (hidden once submitted + results shown) ──
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 8
                    visible: !pollRoot.submitted

                    // single / multi → option rows
                    Repeater {
                        model: (App.pollResponseType === "single" || App.pollResponseType === "multi")
                               ? App.pollOptions : []
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true; Layout.preferredHeight: 38
                            radius: 8
                            property bool on: pollRoot.selected[modelData.id] === true
                            color: on ? Qt.rgba(0.24,0.42,0.98,0.10) : theme.ctrlBg
                            border.color: on ? theme.accent : theme.border; border.width: 1
                            RowLayout {
                                anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 10
                                Rectangle {   // radio / checkbox marker
                                    Layout.preferredWidth: 18; Layout.preferredHeight: 18
                                    radius: App.pollResponseType === "multi" ? 4 : 9
                                    border.color: parent.parent.on ? theme.accent : theme.border; border.width: 2
                                    color: "transparent"
                                    Rectangle { anchors.centerIn: parent; visible: parent.parent.parent.on
                                        width: 10; height: 10; radius: App.pollResponseType === "multi" ? 2 : 5; color: theme.accent }
                                }
                                Label { text: modelData.label; color: theme.text; font.pixelSize: 14
                                        Layout.fillWidth: true; elide: Text.ElideRight }
                            }
                            MouseArea {
                                anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    var s = pollRoot.selected
                                    if (App.pollResponseType === "single") s = ({})
                                    s[modelData.id] = !(pollRoot.selected[modelData.id] === true)
                                    pollRoot.selected = s
                                }
                            }
                        }
                    }

                    // rating → 1..5
                    RowLayout {
                        visible: App.pollResponseType === "rating"; spacing: 8
                        Repeater {
                            model: 5
                            delegate: Rectangle {
                                required property int index
                                Layout.preferredWidth: 44; Layout.preferredHeight: 40; radius: 8
                                property bool on: pollRoot.rating >= index + 1
                                color: on ? theme.accent : theme.ctrlBg
                                border.color: on ? "transparent" : theme.border; border.width: 1
                                Label { anchors.centerIn: parent; text: (index + 1)
                                        color: parent.on ? "#ffffff" : theme.text; font.pixelSize: 15; font.bold: true }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: pollRoot.rating = index + 1 }
                            }
                        }
                    }

                    // free_text OR optional comment
                    TextArea {
                        id: commentField
                        visible: App.pollResponseType === "free_text"
                        Layout.fillWidth: true
                        placeholderText: "Your answer…"
                        wrapMode: TextArea.Wrap
                        color: theme.text
                        background: Rectangle { color: theme.ctrlBg; radius: 8
                            border.color: commentField.activeFocus ? theme.accent : theme.border; border.width: 1 }
                    }
                }

                // ── results (after submit, if enabled) ──
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 6
                    visible: pollRoot.submitted && App.pollShowResults
                    Label { text: "Results · " + (App.pollResults.total !== undefined ? App.pollResults.total : 0) + " votes"
                            color: theme.textMuted; font.pixelSize: 12; font.bold: true }
                    Repeater {
                        model: App.pollOptions
                        delegate: ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true; spacing: 2
                            property int cnt: (App.pollResults.counts && App.pollResults.counts[modelData.id]) ? App.pollResults.counts[modelData.id] : 0
                            property int tot: App.pollResults.total ? App.pollResults.total : 0
                            property real frac: tot > 0 ? cnt / tot : 0
                            RowLayout { Layout.fillWidth: true
                                Label { text: modelData.label; color: theme.text; font.pixelSize: 13; Layout.fillWidth: true; elide: Text.ElideRight }
                                Label { text: Math.round(frac * 100) + "%"; color: theme.textMuted; font.pixelSize: 12 }
                            }
                            Rectangle { Layout.fillWidth: true; height: 7; radius: 4; color: theme.hoverBg
                                Rectangle { width: parent.width * parent.frac; height: parent.height; radius: 4; color: theme.accent } }
                        }
                    }
                    Label { visible: App.pollResults.avg_rating !== undefined
                            text: "Average: " + (App.pollResults.avg_rating !== undefined ? App.pollResults.avg_rating.toFixed(1) : "")
                            color: theme.text; font.pixelSize: 14; font.bold: true }
                }

                // ── action row ──
                RowLayout {
                    Layout.fillWidth: true; Layout.topMargin: 4; spacing: 8
                    Item { Layout.fillWidth: true }
                    // Submit (input phase)
                    Rectangle {
                        visible: !pollRoot.submitted
                        property bool ready: {
                            if (App.pollResponseType === "rating") return pollRoot.rating > 0
                            if (App.pollResponseType === "free_text") return commentField.text.trim().length > 0
                            return Object.keys(pollRoot.selected).some(function(k){ return pollRoot.selected[k] })
                        }
                        Layout.preferredHeight: 36; Layout.preferredWidth: subLbl.implicitWidth + 32; radius: 8
                        opacity: ready ? 1 : 0.5
                        color: subMa.containsMouse && ready ? Qt.darker(theme.accent, 1.15) : theme.accent
                        Label { id: subLbl; anchors.centerIn: parent; text: "Submit"; color: "#ffffff"; font.pixelSize: 13; font.bold: true }
                        MouseArea { id: subMa; anchors.fill: parent; hoverEnabled: true; enabled: parent.ready
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                var choice = []
                                for (var k in pollRoot.selected) if (pollRoot.selected[k]) choice.push(k)
                                App.submitPollResponse(choice, pollRoot.rating,
                                    App.pollResponseType === "free_text" ? commentField.text.trim() : "")
                                pollRoot.submitted = true
                            }
                        }
                    }
                    // Done (results phase)
                    Rectangle {
                        visible: pollRoot.submitted
                        Layout.preferredHeight: 36; Layout.preferredWidth: doneLbl.implicitWidth + 32; radius: 8
                        color: doneMa.containsMouse ? Qt.darker(theme.accent, 1.15) : theme.accent
                        Label { id: doneLbl; anchors.centerIn: parent; text: "Done"; color: "#ffffff"; font.pixelSize: 13; font.bold: true }
                        MouseArea { id: doneMa; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor; onClicked: App.dismissPoll() }
                    }
                }
            }

            // close ✕
            Rectangle {
                anchors.top: parent.top; anchors.right: parent.right
                anchors.topMargin: 12; anchors.rightMargin: 12
                width: 26; height: 26; radius: 7
                color: pxMa.containsMouse ? theme.hoverBg : "transparent"
                Label { anchors.centerIn: parent; text: "✕"; font.pixelSize: 13
                        color: pxMa.containsMouse ? theme.text : theme.textMuted }
                MouseArea { id: pxMa; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor; onClicked: App.dismissPoll() }
            }
        }
    }
}
