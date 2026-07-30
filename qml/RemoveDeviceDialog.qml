import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// VIV-52: confirm before removing a device from the account mesh.  Removal is
// consequential — it signs that device out and immediately kicks any live
// session it holds — so it gets an explicit cream-themed prompt matching
// TrustPromptDialog / ConnectionApprovalDialog.
//
// The caller sets deviceName (+ deviceId, carried for convenience) and open()s
// the dialog; confirmed() fires only on the danger button, dismissed() on
// Cancel / Escape / click-outside.
Dialog {
    id: dialog
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 20
    width: 380
    anchors.centerIn: Overlay.overlay

    property string deviceId: ""
    property string deviceName: ""

    signal confirmed()
    signal dismissed()

    // Escape / click-outside resolve as a cancel.
    onRejected: dialog.dismissed()

    QtObject {
        id: t
        readonly property color bg:           "#fbfaf7"
        readonly property color text:         "#1a1a1f"
        readonly property color textMuted:    "#6f6b60"
        readonly property color border:       "#d8d2c2"
        readonly property color danger:       "#c2372e"
        readonly property color dangerBg:     "#fae6e3"
        readonly property color dangerBorder: "#eec4be"
        readonly property string mono:        "JetBrains Mono, Cascadia Mono, Consolas, monospace"
    }

    background: Rectangle {
        color: t.bg
        radius: 14
        border.color: t.border
        border.width: 1
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
                color: t.dangerBg
                Label {
                    anchors.centerIn: parent
                    text: "🗑"
                    color: t.danger
                    font.pixelSize: 17
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    text: "REMOVE DEVICE"
                    color: t.danger
                    font.pixelSize: 10
                    font.bold: true
                    font.letterSpacing: 1.2
                }
                Label {
                    text: dialog.deviceName.length > 0
                          ? "Remove " + dialog.deviceName + "?"
                          : "Remove this device?"
                    color: t.text
                    font.pixelSize: 14
                    font.bold: true
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        // ── Consequence card ──────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: whyCol.implicitHeight + 22
            color: t.dangerBg
            border.color: t.dangerBorder
            border.width: 1
            radius: 9
            ColumnLayout {
                id: whyCol
                anchors.fill: parent
                anchors.margins: 11
                spacing: 6
                Label {
                    text: "It will be signed out of your account and any active "
                          + "session it holds will end immediately."
                    color: t.danger
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Label {
                    text: "You can add it back later by signing in on it again."
                    color: t.textMuted
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        // ── Footer buttons ────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 2
            spacing: 9
            RemoveDlgButton {
                label: "Cancel"
                Layout.preferredWidth: 110
                onClicked: { dialog.dismissed(); dialog.close() }
            }
            RemoveDlgButton {
                label: "Remove device"
                danger: true
                Layout.fillWidth: true
                onClicked: { dialog.confirmed(); dialog.close() }
            }
        }
    }

    // Flat themed button (same pattern as TrustPromptDialog's TrustDlgButton).
    component RemoveDlgButton: Rectangle {
        property string label: ""
        property bool   danger: false
        signal clicked
        Layout.preferredHeight: 40
        radius: 8
        color: danger
               ? (hover.containsMouse ? "#a92e26" : "#c2372e")
               : (hover.containsMouse ? "#efe9dc" : t.bg)
        border.color: danger ? "transparent" : t.border
        border.width: 1
        Label {
            anchors.centerIn: parent
            text: label
            color: danger ? "#ffffff" : t.text
            font.pixelSize: 13
            font.bold: danger
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
