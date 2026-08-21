// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// VIV-23: outgoing-connect TOFU trust prompt, cream-themed to match
// ConnectionApprovalDialog.
//
// Two variants driven by `mismatch`:
//   false — first connect to an unknown host: friendly blue prompt showing
//           the peer code + key fingerprint; Trust & connect / Cancel.
//   true  — the pinned key CHANGED (possible MITM): red warning showing the
//           old and new fingerprints; Disconnect (default, safe) /
//           Trust new key (replaces the pin).
// The connect attempt is already aborted when this shows; "trust" pins the
// key and AppController re-dials.
Dialog {
    id: dialog
    modal: true
    // Only the two buttons resolve the prompt — Escape / click-outside would
    // leave the paused connect without an answer (mirrors the approval
    // dialog's NoAutoClose).
    closePolicy: Popup.NoAutoClose
    padding: 20
    width: 390
    anchors.centerIn: Overlay.overlay

    property string peerCode: ""
    property string newFingerprint: ""
    property string oldFingerprint: ""
    property bool   mismatch: false

    // trusted() → pin the key (replacing on mismatch) and reconnect;
    // dismissed() → drop the attempt.  Escape counts as dismissed (the
    // parent Loader wires onRejected too).
    signal trusted()
    signal dismissed()

    QtObject {
        id: t
        readonly property color bg:        "#fbfaf7"
        readonly property color text:      "#1a1a1f"
        readonly property color textMuted: "#6f6b60"
        readonly property color border:    "#d8d2c2"
        readonly property color accent:    "#3D6BFA"
        readonly property color danger:    "#c2372e"
        readonly property color dangerBg:  "#fae6e3"
        readonly property color dangerBorder: "#eec4be"
        readonly property color warnBg:    "#faf2dd"
        readonly property color warnBorder:"#ead9ab"
        readonly property color warn:      "#b6801b"
        readonly property string mono:      App.monoFont
    }

    background: Rectangle {
        color: t.bg
        radius: 14
        border.color: dialog.mismatch ? t.dangerBorder : t.border
        border.width: dialog.mismatch ? 2 : 1
    }
    header: Item { implicitHeight: 0 }

    contentItem: ColumnLayout {
        spacing: 14

        // ── Header ────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 11
            Rectangle {
                Layout.preferredWidth: 38
                Layout.preferredHeight: 38
                radius: 19
                color: dialog.mismatch ? t.dangerBg : "#e7edfe"
                Label {
                    anchors.centerIn: parent
                    text: dialog.mismatch ? "⚠" : "🔑"
                    color: dialog.mismatch ? t.danger : t.accent
                    font.pixelSize: 17
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    text: dialog.mismatch ? "HOST KEY CHANGED" : "FIRST CONNECTION"
                    color: dialog.mismatch ? t.danger : t.accent
                    font.pixelSize: 10
                    font.bold: true
                    font.letterSpacing: 1.2
                }
                Label {
                    text: dialog.mismatch
                          ? "This host's identity key is different"
                          : "First time connecting to this host"
                    color: t.text
                    font.pixelSize: 14
                    font.bold: true
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        // ── Peer code ─────────────────────────────────────────────────
        Label {
            text: dialog.peerCode
            color: t.text
            font.pixelSize: 16
            font.bold: true
            font.family: t.mono
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        // ── Explanation + fingerprint card ────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: fpCol.implicitHeight + 22
            color: dialog.mismatch ? t.dangerBg : t.warnBg
            border.color: dialog.mismatch ? t.dangerBorder : t.warnBorder
            border.width: 1
            radius: 9
            ColumnLayout {
                id: fpCol
                anchors.fill: parent
                anchors.margins: 11
                spacing: 6
                Label {
                    text: dialog.mismatch
                          ? "The key no longer matches the one you trusted. "
                            + "This could be a man-in-the-middle attack. If the "
                            + "host reinstalled Vivora, this is expected."
                          : "Verify this fingerprint with the host out of band "
                            + "(call, message) before trusting."
                    color: dialog.mismatch ? t.danger : t.textMuted
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                GridLayout {
                    columns: 2
                    columnSpacing: 10
                    rowSpacing: 2
                    Layout.fillWidth: true
                    Label {
                        visible: dialog.mismatch
                        text: "Was"
                        color: t.textMuted
                        font.pixelSize: 11
                    }
                    Label {
                        visible: dialog.mismatch
                        text: dialog.oldFingerprint
                        color: t.textMuted
                        font.pixelSize: 13
                        font.family: t.mono
                        font.strikeout: true
                    }
                    Label {
                        text: dialog.mismatch ? "Now" : "Key"
                        color: t.textMuted
                        font.pixelSize: 11
                    }
                    Label {
                        text: dialog.newFingerprint
                        color: t.text
                        font.pixelSize: 13
                        font.bold: true
                        font.family: t.mono
                    }
                }
            }
        }

        // ── Footer buttons ────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 2
            spacing: 9
            // Mismatch: Disconnect is the primary/safe default; trusting the
            // new key is the flat (danger-tinted) action.  First connect:
            // Trust & connect is primary, Cancel is flat.
            TrustDlgButton {
                label: dialog.mismatch ? "Trust new key" : "Cancel"
                danger: dialog.mismatch
                Layout.preferredWidth: dialog.mismatch ? 150 : 110
                onClicked: dialog.mismatch ? dialog.trusted() : dialog.dismissed()
            }
            TrustDlgButton {
                label: dialog.mismatch ? "Disconnect" : "Trust & connect"
                glyph: dialog.mismatch ? "" : "✓"
                primary: true
                Layout.fillWidth: true
                onClicked: dialog.mismatch ? dialog.dismissed() : dialog.trusted()
            }
        }
    }

    // Flat themed button (same pattern as ConnectionApprovalDialog's
    // DlgButton — different file scope, so it can't be reused directly).
    component TrustDlgButton: Rectangle {
        property string label: ""
        property string glyph: ""
        property bool   primary: false
        property bool   danger: false
        signal clicked
        Layout.preferredHeight: 40
        radius: 8
        color: primary
               ? (hover.containsMouse ? "#2a2a32" : "#1a1a1f")
               : (hover.containsMouse ? "#efe9dc" : t.bg)
        border.color: primary ? "transparent" : (danger ? t.dangerBorder : t.border)
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
                color: primary ? "#ffffff" : (danger ? t.danger : t.text)
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
