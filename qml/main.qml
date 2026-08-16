import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Shapes

ApplicationWindow {
    id: window
    width: 495
    height: 735
    minimumWidth: 420
    // Tall enough that the sharing card, My Devices, the peer-code input and
    // Recent all stay on screen at once; the two lists scroll internally rather
    // than the whole window scrolling.
    minimumHeight: 705
    visible: true
    title: "Vivora"

    color: "#f6f4ef"   // paper — matches the design handoff surface

    // ── Derived launcher state (label + colour for the header state-pill
    // and brand mark).  Mapped straight onto the existing host-state
    // sources — no separate state machine.  Blue = waiting/idle, green =
    // live/connected, grey = paused.
    readonly property color statePillColor: !App.sharing ? theme.inkFaint
            : (App.clientCount > 0 ? theme.green : theme.blue)
    readonly property string statePillLabel: !App.sharing ? "PAUSED"
            : (App.clientCount > 0 ? "LIVE" : "WAITING")
    readonly property bool statePulsing: App.sharing

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
        // VIV-111 (macOS): a share was requested without the Screen Recording
        // grant — show the actionable permission dialog, keep sharing off.
        function onScreenRecordingPermissionRequired(message) {
            permissionLoader.message = message
            permissionLoader.active = true
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
    // VIV-111 macOS Screen Recording permission prompt.  One at a time is fine
    // (only fires on a share attempt).
    Loader {
        id: permissionLoader
        active: false
        property string message: ""
        sourceComponent: ScreenRecordingPermissionDialog {
            visible: true
            message: permissionLoader.message
            onOpenSettings: { App.openScreenRecordingSettings(); permissionLoader.active = false }
            onDismissed:    permissionLoader.active = false
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
        readonly property color ctrlBg:     "#fbfaf5"   // near-white control fill (poll modal)
        readonly property string monoFont: "JetBrains Mono, Cascadia Mono, Consolas, monospace"

        // ── Design-handoff tokens (used by the restyled launcher) ──────
        readonly property color paper:      "#f6f4ef"
        readonly property color paperSoft:  "#efece4"
        readonly property color paperDeep:  "#e7e3d8"
        readonly property color ink:        "#111114"
        readonly property color inkSoft:    "#2a2a2e"
        readonly property color inkMid:     "#5e5e63"
        readonly property color inkFaint:   "#8a8a90"
        readonly property color hair:       Qt.rgba(0.067, 0.067, 0.078, 0.10)
        readonly property color hairStrong: Qt.rgba(0.067, 0.067, 0.078, 0.16)
        readonly property color blue:       "#3D6BFA"
        readonly property color green:      "#1FA463"
        readonly property color red:        "#E5484D"
        readonly property color online:     "#3ddc84"
    }

    // ── Brand mark ─────────────────────────────────────────────────────
    // The two-curve + three-dot logo from the design handoff, rebuilt with
    // QtQuick.Shapes so it scales crisply.  The lower dot takes the current
    // state accent colour.
    component BrandMark: Item {
        id: bm
        property real size: 22
        property color markColor: theme.ink
        property color accentColor: theme.blue
        implicitWidth: size
        implicitHeight: size
        width: size
        height: size

        Shape {
            width: 32; height: 32
            antialiasing: true
            transform: Scale {
                origin.x: 0; origin.y: 0
                xScale: bm.size / 32
                yScale: bm.size / 32
            }
            ShapePath {
                strokeColor: bm.markColor
                strokeWidth: 3
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: "M 5 5 Q 2 10 8 14 Q 14 19 16 24" }
            }
            ShapePath {
                strokeColor: bm.markColor
                strokeWidth: 3
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: "M 27 5 Q 30 10 24 14 Q 18 19 16 24" }
            }
        }
        Rectangle {
            width: bm.size * (5.0 / 32); height: width; radius: width / 2
            color: bm.markColor
            x: bm.size * (5.0 / 32) - width / 2
            y: bm.size * (5.0 / 32) - height / 2
        }
        Rectangle {
            width: bm.size * (5.0 / 32); height: width; radius: width / 2
            color: bm.markColor
            x: bm.size * (27.0 / 32) - width / 2
            y: bm.size * (5.0 / 32) - height / 2
        }
        Rectangle {
            width: bm.size * (8.0 / 32); height: width; radius: width / 2
            color: bm.accentColor
            x: bm.size * (16.0 / 32) - width / 2
            y: bm.size * (25.5 / 32) - height / 2
        }
    }

    // A status dot with an optional expanding-ring pulse (matches the
    // design's `pulse` keyframe on the header / sharing status dots).
    component PulseDot: Item {
        id: pd
        property color dotColor: theme.blue
        property bool pulsing: true
        property real dotSize: 7
        implicitWidth: dotSize
        implicitHeight: dotSize

        Rectangle {
            id: pulseRing
            anchors.centerIn: parent
            width: pd.dotSize; height: pd.dotSize; radius: width / 2
            color: "transparent"
            border.color: pd.dotColor
            border.width: 1
            visible: pd.pulsing
            scale: 1.0
            opacity: 0.0
            // Fire one ripple every few seconds instead of looping animations
            // back-to-back.  A continuously-running animation forces a 60fps
            // scene-graph resync/repaint for the WHOLE window (~15% CPU on a
            // laptop whenever the window is visible).  A Timer-driven one-shot
            // leaves a real idle gap where NO animation runs, so the render loop
            // fully sleeps between pulses — same "listening" ripple, a fraction
            // of the cost.
            ParallelAnimation {
                id: pulseRipple
                NumberAnimation { target: pulseRing; property: "scale"
                                  from: 1.0; to: 2.6; duration: 1600; easing.type: Easing.OutQuad }
                NumberAnimation { target: pulseRing; property: "opacity"
                                  from: 0.38; to: 0.0; duration: 1600; easing.type: Easing.OutQuad }
            }
            // Only ripple while the window is focused.  Left open in the
            // background (the common case), the pulse stops entirely so the app
            // drops to ~idle CPU instead of repainting forever; it resumes the
            // moment the user clicks back in.
            Timer {
                interval: 3200
                running: pd.pulsing && pd.visible && window.active
                repeat: true
                triggeredOnStart: true
                onTriggered: pulseRipple.restart()
            }
        }
        Rectangle {
            anchors.centerIn: parent
            width: pd.dotSize; height: pd.dotSize; radius: width / 2
            color: pd.dotColor
        }
    }

    // Soft, low-emphasis action button (design `.btn.btn-soft.btn-sm`).
    // `active: false` renders it as a visibly-inert placeholder for
    // features whose backend hasn't landed yet.
    component SoftButton: Rectangle {
        id: sb
        property string iconName: ""
        property string label: ""
        property bool active: true
        signal clicked
        implicitHeight: 30
        radius: 6
        color: (active && sbArea.containsMouse) ? theme.paperDeep : theme.paperSoft
        border.color: theme.hair
        border.width: 1
        opacity: active ? 1.0 : 0.5
        RowLayout {
            anchors.centerIn: parent
            spacing: 6
            Glyph {
                name: sb.iconName
                visible: sb.iconName.length > 0
                size: 13
                color: theme.inkSoft
            }
            Label {
                text: sb.label
                visible: sb.label.length > 0
                color: theme.inkSoft
                font.pixelSize: 12
            }
        }
        MouseArea {
            id: sbArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: sb.active ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: sb.clicked()
        }
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
        anchors.margins: 12
        spacing: 10

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

        // ── Header: brand mark + wordmark + state pill ───────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            BrandMark {
                size: 22
                markColor: theme.ink
                accentColor: window.statePillColor
            }
            Label {
                text: "Vivora"
                font.pixelSize: 16
                font.bold: true
                font.letterSpacing: -0.2
                color: theme.ink
            }
            // State pill — subtle rounded chip; the coloured dot (+ pulse)
            // carries the state, the label is neutral ink.
            Rectangle {
                Layout.preferredHeight: 22
                Layout.preferredWidth: pillRow.implicitWidth + 18
                radius: height / 2
                color: theme.paperSoft
                border.color: theme.hair
                border.width: 1
                RowLayout {
                    id: pillRow
                    anchors.centerIn: parent
                    spacing: 6
                    PulseDot {
                        dotSize: 7
                        dotColor: window.statePillColor
                        pulsing: window.statePulsing
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Label {
                        text: window.statePillLabel
                        color: theme.inkMid
                        font.family: theme.monoFont
                        font.pixelSize: 10
                        font.letterSpacing: 0.5
                    }
                }
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
        }

        // ── Middle: the sharing card, My Devices, Connect and Recent are all
        // pinned (no window-wide scroll).  My Devices is capped (scrolls
        // internally past ~4 rows); Recent takes the leftover height and scrolls
        // internally; a trailing spacer soaks up any surplus so the sections
        // stay top-packed at any window height.
        ColumnLayout {
            id: middleCol
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10

                // ── Sharing section ──────────────────────────────────────────
                // Wrapper Rectangle gives the "host card" its own subtly darker
                // background so it reads as a distinct zone from the "connect
                // to a peer" half below.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: hostCard.implicitHeight + 20
                    visible: App.sharing
                    color: theme.hostBg
                    radius: 10

                    ColumnLayout {
                        id: hostCard
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 6

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
                        Layout.preferredHeight: codeRow.implicitHeight + 18
                        visible: App.sharing
                        color: theme.pillBg
                        radius: 10
                        RowLayout {
                            id: codeRow
                            anchors.fill: parent
                            anchors.margins: 9
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
                    id: myDevicesBlock
                    Layout.fillWidth: true
                    // Connect a mesh device through the same path as the manual
                    // peer-code input — dial its peer code via App.connectToPeer.
                    onConnectRequested: (peerCode, pubkey, devName) => {
                        if (peerCode.length > 0) {
                            // Own account device: pre-pin its mesh key so the
                            // viewer skips the first-connect TOFU dialog.  Falls
                            // back to a plain connect when the key is missing.
                            App.connectToAccountDevice(peerCode, pubkey)
                            window.showToast("Connecting to " + devName + "…")
                        } else {
                            window.showToast("No peer code for " + devName)
                        }
                    }
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
                    Glyph { name: "download"; size: 13; color: theme.inkMid }
                    Label {
                        text: "CONNECT TO A PEER"
                        color: theme.inkMid
                        font.family: theme.monoFont
                        font.pixelSize: 10
                        font.letterSpacing: 0.6
                    }
                }

                TextField {
                    id: peerInput
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    leftPadding: 12
                    rightPadding: 12
                    placeholderText: "peer code (e.g. swift-tiger-4271)"
                    placeholderTextColor: theme.inkFaint
                    font.family: theme.monoFont
                    font.pixelSize: 13
                    color: theme.ink
                    // selectByMouse + selectionColor → keep selection legible on
                    // the warm-bg theme (default Qt palette picks blue that
                    // clashes).
                    selectByMouse: true
                    selectionColor: theme.accent
                    selectedTextColor: "#ffffff"
                    background: Rectangle {
                        color: theme.paper
                        border.color: peerInput.activeFocus ? theme.ink : theme.hairStrong
                        border.width: 1
                        radius: 8
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

                // Recent label + count badge
                RowLayout {
                    Layout.fillWidth: true
                    visible: App.peers.rowCount() > 0
                    Label {
                        text: "RECENT"
                        color: theme.inkMid
                        font.family: theme.monoFont
                        font.pixelSize: 10
                        font.letterSpacing: 0.6
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: App.peers.rowCount()
                        color: theme.inkFaint
                        font.family: theme.monoFont
                        font.pixelSize: 10
                    }
                }

                // Recent list — the sole space-filler below the pinned sections:
                // it takes ALL the leftover height (showing as many rows as fit),
                // floors at ~3 rows, and scrolls INTERNALLY when the window is too
                // short for every row.  No max + no trailing spacer, so surplus
                // goes to Recent rather than an empty gap.
                AddressBookView {
                    id: recentList
                    visible: App.peers.rowCount() > 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: App.peers.rowCount() > 0 ? 114 : 0
                    interactive: height < contentHeight
                    onPeerActivated: (alias, pubkey, code) => {
                        App.connectToPeer(pubkey.length > 0 ? pubkey : code)
                    }
                    ScrollBar.vertical: ScrollBar {
                        id: recentScroll
                        policy: recentList.height < recentList.contentHeight
                                ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                        width: 10
                        contentItem: Rectangle {
                            implicitWidth: 6
                            radius: 3
                            color: theme.textMuted
                            opacity: (recentScroll.pressed || recentScroll.hovered)
                                     ? 1.0 : 0.7
                            Behavior on opacity { NumberAnimation { duration: 120 } }
                        }
                    }
                }

            }   // middleCol

        // ── Footer ───────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            SoftButton {
                iconName: "gear"
                label: "Settings"
                Layout.preferredWidth: 104
                Layout.preferredHeight: 32
                onClicked: App.openSettings()
            }
            Item { Layout.fillWidth: true }
            SoftButton {
                label: "Hide"
                Layout.preferredWidth: 80
                Layout.preferredHeight: 32
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
