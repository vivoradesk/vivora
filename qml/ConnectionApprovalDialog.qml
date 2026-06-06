import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// VIV-55/VIV-61: connection approval popup, cream-themed (see the mockup
// attached to the issue).
//
// The IK handshake (VIV-61) authenticates the connecting viewer, so the
// trust card, fingerprint and "Trust this device" pin are now live: green
// "Recognized key · seen N times" when the viewer's key is in the address
// book, amber "New key — verify out of band" otherwise.  The GRANT ON
// ACCEPT toggles remain disabled placeholders pending VIV-60.
Dialog {
    id: dialog
    modal: true
    closePolicy: Popup.NoAutoClose
    padding: 20
    width: 380
    anchors.centerIn: Overlay.overlay

    property string approvalKey: ""
    property string peerCode:    ""
    property string pubkeyHex:   ""
    property string ipPort:      ""
    // Number of additional peers waiting behind this one in the queue.
    // Drives the "N more waiting" badge.  The parent Loader advances the
    // queue on approve/reject by rebinding the properties above.
    property int    morePending: 0

    // VIV-61: viewer identity.  recognized = this pubkey is already in the
    // address book; seenCount = how many prior contacts.  Drive the trust
    // card (green recognised / amber new key).
    property bool   recognized: false
    property int    seenCount:  0
    // Self-reported viewer device name (VIV-61); falls back to a network
    // scope label when the peer didn't send one.
    property string deviceName: ""
    // Bound to the "Trust this device — don't ask again" checkbox; passed
    // back on approve so the host pins the viewer as trusted.
    property bool   trustChecked: false

    readonly property int totalSeconds: 30
    property int    secondsRemaining: 30

    signal approved(string key, bool remember)
    signal rejected(string key)

    // Each time the parent rebinds us to a new pending peer, restart the
    // auto-reject countdown and clear the per-peer trust checkbox.
    onApprovalKeyChanged: {
        dialog.secondsRemaining = dialog.totalSeconds
        dialog.trustChecked = false
    }

    // Transport hint derived from the source IP — LAN for RFC1918 /
    // link-local ranges, WAN otherwise.  Best-effort display label only.
    readonly property string ipOnly:
        ipPort.lastIndexOf(":") > 0 ? ipPort.substring(0, ipPort.lastIndexOf(":"))
                                    : ipPort
    readonly property bool isLan: {
        var ip = dialog.ipOnly
        if (ip.indexOf("192.168.") === 0) return true
        if (ip.indexOf("10.") === 0)      return true
        if (ip.indexOf("127.") === 0)     return true
        if (ip.indexOf("169.254.") === 0) return true
        return /^172\.(1[6-9]|2[0-9]|3[01])\./.test(ip)
    }
    readonly property string transport: dialog.isLan ? "LAN" : "WAN"

    // 30-second auto-reject — Parsec/AnyDesk convention so a walked-away
    // user doesn't leave the dialog hanging forever.  The parent advances
    // the queue when we emit rejected().
    Timer {
        id: countdown
        interval: 1000
        repeat: true
        running: dialog.visible
        onTriggered: {
            dialog.secondsRemaining -= 1
            if (dialog.secondsRemaining <= 0)
                dialog.rejected(dialog.approvalKey)
        }
    }

    // ── Palette (mirrors main.qml's theme; the dialog card sits a touch
    //    lighter than the cream window so it reads as a raised surface) ──
    QtObject {
        id: t
        readonly property color bg:        "#fbfaf7"
        readonly property color text:      "#1a1a1f"
        readonly property color textMuted: "#6f6b60"
        readonly property color border:    "#d8d2c2"
        readonly property color accent:    "#3D6BFA"
        readonly property color green:     "#22a85c"
        readonly property color warn:      "#b6801b"
        readonly property color warnBg:    "#faf2dd"
        readonly property color warnBorder:"#ead9ab"
        readonly property color pill:      "#1a1a1f"
        readonly property string mono:     "Geist Mono, JetBrains Mono, Cascadia Mono, Consolas, monospace"
    }

    background: Rectangle {
        color: t.bg
        radius: 14
        border.color: t.border
        border.width: 1
    }
    // Hide the stock Dialog title bar — we draw our own header below.
    header: Item { implicitHeight: 0 }

    contentItem: ColumnLayout {
        spacing: 14

        // ── Header: countdown ring + "INCOMING CONNECTION" ───────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 11

            Canvas {
                id: ring
                Layout.preferredWidth: 38
                Layout.preferredHeight: 38
                property real frac: dialog.secondsRemaining / dialog.totalSeconds
                property bool urgent: dialog.secondsRemaining <= 10
                onFracChanged: requestPaint()
                onUrgentChanged: requestPaint()
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    var cx = width / 2, cy = height / 2, r = width / 2 - 3
                    ctx.lineWidth = 3
                    ctx.strokeStyle = t.border
                    ctx.beginPath(); ctx.arc(cx, cy, r, 0, 2 * Math.PI); ctx.stroke()
                    ctx.strokeStyle = urgent ? t.warn : t.accent
                    ctx.lineCap = "round"
                    ctx.beginPath()
                    ctx.arc(cx, cy, r, -Math.PI / 2,
                            -Math.PI / 2 + 2 * Math.PI * frac)
                    ctx.stroke()
                }
                Label {
                    anchors.centerIn: parent
                    text: dialog.secondsRemaining
                    color: ring.urgent ? t.warn : t.text
                    font.pixelSize: 12
                    font.bold: true
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    text: "INCOMING CONNECTION"
                    color: t.accent
                    font.pixelSize: 10
                    font.bold: true
                    font.letterSpacing: 1.2
                }
                Label {
                    text: "A peer wants to view your desktop"
                    color: t.text
                    font.pixelSize: 14
                    font.bold: true
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        // ── Identity: who's connecting (IP + transport) ──────────────
        // No device name yet — the viewer is anonymous under Noise_NK
        // (VIV-61).  Show a network-scoped title + the source address.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                text: dialog.deviceName.length > 0
                      ? dialog.deviceName
                      : (dialog.isLan ? "Peer on your local network"
                                      : "Peer on the internet")
                color: t.text
                font.pixelSize: 16
                font.bold: true
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: dialog.ipPort + " · " + dialog.transport
                color: t.textMuted
                font.pixelSize: 12
                font.family: t.mono
            }
        }

        // ── Trust card (VIV-61) ──────────────────────────────────────
        // Green when the viewer's static key is already in the address
        // book ("Recognized key · seen N times"); amber for a first-seen
        // key ("New key — verify out of band").
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: trustRow.implicitHeight + 20
            color: dialog.recognized ? "#e9f6ee" : t.warnBg
            border.color: dialog.recognized ? "#bfe3cd" : t.warnBorder
            border.width: 1
            radius: 9
            RowLayout {
                id: trustRow
                anchors.fill: parent
                anchors.margins: 10
                spacing: 9
                Label {
                    text: dialog.recognized ? "✓" : "⚠"
                    color: dialog.recognized ? t.green : t.warn
                    font.pixelSize: 15
                    Layout.alignment: Qt.AlignTop
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: dialog.recognized
                              ? ("Recognized key · seen " + dialog.seenCount
                                 + (dialog.seenCount === 1 ? " time" : " times"))
                              : "New key — verify out of band"
                        color: t.text
                        font.pixelSize: 12
                        font.bold: true
                    }
                    Label {
                        visible: !dialog.recognized
                        text: "First time this device connects. Confirm out of "
                              + "band who this is before accepting."
                        color: t.textMuted
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    // Viewer peer code — the human-readable identity derived
                    // from the same key as the fingerprint below.  Primary.
                    Label {
                        visible: dialog.peerCode.length > 0
                        text: dialog.peerCode
                        color: t.text
                        font.pixelSize: 13
                        font.family: t.mono
                        topPadding: 1
                    }
                    // Canonical hex fingerprint — small, for out-of-band
                    // verification.  Populated by the IK handshake.
                    Label {
                        visible: dialog.pubkeyHex.length >= 12
                        text: "ED25519 · " + dialog.pubkeyHex.substring(0, 6)
                              + " … " + dialog.pubkeyHex.substring(dialog.pubkeyHex.length - 4)
                        color: t.textMuted
                        font.pixelSize: 10
                        font.family: t.mono
                    }
                }
            }
        }

        // ── GRANT ON ACCEPT (disabled placeholder — VIV-60) ──────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            opacity: 0.45      // greyed-out: drawn but inert until VIV-60

            Label {
                text: "GRANT ON ACCEPT"
                color: t.textMuted
                font.pixelSize: 10
                font.bold: true
                font.letterSpacing: 1.2
            }
            GrantRow { glyph: "⌨"; label: "Keyboard & mouse control"; on: true }
            GrantRow { glyph: "⧉"; label: "Clipboard sync";           on: true }
            GrantRow { glyph: "🗀"; label: "File transfer";            on: false }
        }

        // ── Trust this device — don't ask again (VIV-61) ─────────────
        // Wrapped in a plain Item so the click MouseArea can anchor-fill
        // it (anchoring inside the layout-managed row is undefined and
        // collapses the dialog).
        Item {
            Layout.fillWidth: true
            implicitHeight: trustRowInner.implicitHeight
            RowLayout {
                id: trustRowInner
                anchors.fill: parent
                spacing: 8
                Rectangle {
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                    radius: 4
                    color: dialog.trustChecked ? t.accent : "transparent"
                    border.color: dialog.trustChecked ? t.accent : t.border
                    border.width: 1.5
                    Label {
                        anchors.centerIn: parent
                        visible: dialog.trustChecked
                        text: "✓"
                        color: "#ffffff"
                        font.pixelSize: 11
                        font.bold: true
                    }
                }
                Label {
                    text: "Trust this device — don't ask again"
                    color: t.text
                    font.pixelSize: 12
                }
                Item { Layout.fillWidth: true }
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: dialog.trustChecked = !dialog.trustChecked
            }
        }

        // ── "N more waiting" queue badge ─────────────────────────────
        Rectangle {
            visible: dialog.morePending > 0
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: "transparent"
            border.color: t.border
            border.width: 1
            radius: 7
            RowLayout {
                anchors.centerIn: parent
                spacing: 6
                Rectangle { width: 6; height: 6; radius: 3; color: t.accent }
                Label {
                    text: dialog.morePending === 1
                          ? "1 more peer waiting"
                          : dialog.morePending + " more peers waiting"
                    color: t.textMuted
                    font.pixelSize: 11
                    font.bold: true
                }
            }
        }

        // ── Footer: Reject / Accept & connect ────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 2
            spacing: 9
            DlgButton {
                label: "Reject"
                Layout.preferredWidth: 110
                onClicked: dialog.rejected(dialog.approvalKey)
            }
            DlgButton {
                label: "Accept & connect"
                glyph: "✓"
                primary: true
                Layout.fillWidth: true
                onClicked: dialog.approved(dialog.approvalKey, dialog.trustChecked)
            }
        }
    }

    // ── Reusable inline components ───────────────────────────────────

    // A capability row: glyph + label on the left, pill switch on the
    // right.  Inert (enabled:false) — the parent ColumnLayout greys the
    // whole group; this just renders the on/off visual state.
    component GrantRow: RowLayout {
        id: grantRow
        property string glyph: ""
        property string label: ""
        property bool   on: false
        enabled: false
        Layout.fillWidth: true
        spacing: 9
        Label {
            text: grantRow.glyph
            color: t.text
            font.pixelSize: 14
            Layout.preferredWidth: 18
        }
        Label {
            text: grantRow.label
            color: t.text
            font.pixelSize: 12
        }
        Item { Layout.fillWidth: true }
        Rectangle {            // pill switch, visual only
            id: pill
            Layout.preferredWidth: 36
            Layout.preferredHeight: 21
            radius: height / 2
            color: grantRow.on ? t.text : t.border
            Rectangle {
                width: 17; height: 17; radius: height / 2
                color: "#ffffff"
                anchors.verticalCenter: parent.verticalCenter
                x: grantRow.on ? pill.width - width - 2 : 2
            }
        }
    }

    // Flat themed button with a custom Rectangle background + internal
    // MouseArea (matches main.qml's AppButton, which lives in a different
    // file scope and so can't be reused here directly).
    component DlgButton: Rectangle {
        property string label: ""
        property string glyph: ""
        property bool   primary: false
        signal clicked
        Layout.preferredHeight: 40
        radius: 8
        color: primary
               ? (hover.containsMouse ? "#2a2a32" : t.pill)
               : (hover.containsMouse ? "#efe9dc" : t.bg)
        border.color: primary ? "transparent" : t.border
        border.width: 1
        RowLayout {
            anchors.centerIn: parent
            spacing: 6
            Label {
                text: glyph
                visible: glyph.length > 0
                color: primary ? "#ffffff" : t.text
                font.pixelSize: 13
            }
            Label {
                text: label
                color: primary ? "#ffffff" : t.text
                font.pixelSize: 13
                font.bold: primary
            }
        }
        MouseArea {
            id: hover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: parent.clicked()
        }
    }
}
