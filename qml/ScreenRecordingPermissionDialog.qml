import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// VIV-111 (macOS): shown when a share is requested without the Screen
// Recording (TCC) grant.  Cream-themed to match the trust / approval dialogs.
// Explains the missing permission and offers a one-click jump to the right
// System Settings pane.  Sharing stays off until the user grants + restarts.
Dialog {
    id: dialog
    modal: true
    closePolicy: Popup.NoAutoClose
    padding: 20
    width: 400
    anchors.centerIn: Overlay.overlay

    property string message: ""

    // openSettings() → deep-link to the Screen Recording pane; dismissed() →
    // close without granting.  The parent Loader wires both.
    signal openSettings()
    signal dismissed()

    QtObject {
        id: t
        readonly property color bg:        "#fbfaf7"
        readonly property color text:      "#1a1a1f"
        readonly property color textMuted: "#6f6b60"
        readonly property color border:    "#d8d2c2"
        readonly property color accent:    "#3D6BFA"
        readonly property color infoBg:    "#e7edfe"
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
                color: t.infoBg
                Label {
                    anchors.centerIn: parent
                    text: "🖥"
                    font.pixelSize: 18
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    text: "SCREEN RECORDING"
                    color: t.accent
                    font.pixelSize: 10
                    font.bold: true
                    font.letterSpacing: 1.2
                }
                Label {
                    text: "Permission needed to share your screen"
                    color: t.text
                    font.pixelSize: 14
                    font.bold: true
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        // ── Explanation ───────────────────────────────────────────────
        Label {
            text: dialog.message
            color: t.textMuted
            font.pixelSize: 12
            lineHeight: 1.25
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // ── Footer buttons ────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 2
            spacing: 9
            PermDlgButton {
                label: "Not now"
                Layout.preferredWidth: 110
                onClicked: dialog.dismissed()
            }
            PermDlgButton {
                label: "Open System Settings"
                glyph: "→"
                primary: true
                Layout.fillWidth: true
                onClicked: dialog.openSettings()
            }
        }
    }

    // Flat themed button (mirrors TrustPromptDialog's TrustDlgButton — separate
    // file scope, so it can't be reused directly).
    component PermDlgButton: Rectangle {
        property string label: ""
        property string glyph: ""
        property bool   primary: false
        signal clicked
        Layout.preferredHeight: 40
        radius: 8
        color: primary
               ? (hover.containsMouse ? "#2a2a32" : "#1a1a1f")
               : (hover.containsMouse ? "#efe9dc" : t.bg)
        border.color: primary ? "transparent" : t.border
        border.width: 1
        RowLayout {
            anchors.centerIn: parent
            spacing: 6
            Label {
                text: label
                color: primary ? "#ffffff" : t.text
                font.pixelSize: 13
                font.bold: primary
            }
            Label {
                text: glyph
                visible: glyph.length > 0
                color: primary ? "#ffffff" : t.text
                font.pixelSize: 13
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
