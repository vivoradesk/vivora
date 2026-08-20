import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shown when a connection failed in the one way the Vivora-operated relay
// would have fixed: the peer was found, and the direct path never came up.
//
// This is the moment the relay is worth explaining. Before it, "Vivora Pro
// includes a relay" is an abstraction; here it is the answer to the thing that
// just went wrong. AppController is deliberately strict about when it fires --
// a mistyped code or an offline host raises a plain toast instead, because
// selling a subscription as the fix for a typo is worse than saying nothing.
//
// Shown once per run. If someone retries and fails again they get the toast;
// they have already been told.
Dialog {
    id: dialog
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 20
    width: 400
    anchors.centerIn: Overlay.overlay

    property string peer: ""

    signal upgradeRequested()

    QtObject {
        id: t
        readonly property color bg:         "#fbfaf7"
        readonly property color text:       "#1a1a1f"
        readonly property color textMuted:  "#6f6b60"
        readonly property color border:     "#d8d2c2"
        readonly property color accent:     "#3D6BFA"
        readonly property color accentSoft: "#e8edfe"
        readonly property color hair:       "#e4ded0"
        readonly property string mono:      "JetBrains Mono, Cascadia Mono, Consolas, monospace"
    }

    background: Rectangle {
        color: t.bg
        radius: 14
        border.color: t.border
        border.width: 1
    }
    header: Item { implicitHeight: 0 }

    // Same shape as RemoveDeviceDialog's button, so the two prompts feel like
    // one product.
    component DlgButton: Rectangle {
        property string label: ""
        property bool   primary: false
        signal clicked
        Layout.preferredHeight: 40
        radius: 8
        color: primary
               ? (hover.containsMouse ? Qt.darker(t.accent, 1.12) : t.accent)
               : (hover.containsMouse ? "#efe9dc" : t.bg)
        border.color: primary ? "transparent" : t.border
        border.width: 1
        Label {
            anchors.centerIn: parent
            text: label
            color: primary ? "#ffffff" : t.text
            font.pixelSize: 13
            font.bold: primary
        }
        MouseArea {
            id: hover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: parent.clicked()
        }
    }

    contentItem: ColumnLayout {
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            spacing: 11
            Rectangle {
                Layout.preferredWidth: 38
                Layout.preferredHeight: 38
                radius: 19
                color: t.accentSoft
                Label {
                    anchors.centerIn: parent
                    text: "⇄"
                    color: t.accent
                    font.pixelSize: 18
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    text: "COULDN'T CONNECT DIRECTLY"
                    color: t.accent
                    font.pixelSize: 10
                    font.bold: true
                    font.letterSpacing: 1.2
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.peer.length > 0
                          ? "Found " + dialog.peer + ", but couldn't reach it"
                          : "Found the host, but couldn't reach it"
                    color: t.text
                    font.pixelSize: 14
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
            }
        }

        // What actually happened, in the user's terms.
        Label {
            Layout.fillWidth: true
            text: "The other machine is online — your networks just won't let the "
                  + "two of you talk to each other directly. That usually means a "
                  + "strict NAT, mobile internet, or a corporate firewall on one side."
            color: t.textMuted
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            lineHeight: 1.3
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: fixCol.implicitHeight + 24
            color: t.accentSoft
            border.color: Qt.rgba(0.24, 0.42, 0.98, 0.25)
            border.width: 1
            radius: 9
            ColumnLayout {
                id: fixCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6
                Label {
                    Layout.fillWidth: true
                    text: "<b>Vivora Pro</b> routes the stream through a relay when a "
                          + "direct connection can't be made, so this works anyway."
                    textFormat: Text.RichText
                    color: t.text
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                    lineHeight: 1.3
                }
                Label {
                    Layout.fillWidth: true
                    text: "$9.90/month · cancel anytime"
                    color: t.textMuted
                    font.family: t.mono
                    font.pixelSize: 11
                }
            }
        }

        // The relay is AGPL and self-hostable; not saying so here would make
        // the paid option look like the only option, which it is not.
        Label {
            Layout.fillWidth: true
            text: "You can also run your own relay — it's open source, and there's "
                  + "a setting for pointing Vivora at it."
            color: t.textMuted
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 2
            spacing: 9
            DlgButton {
                label: "Not now"
                Layout.preferredWidth: 110
                onClicked: dialog.close()
            }
            DlgButton {
                label: "See Vivora Pro"
                primary: true
                Layout.fillWidth: true
                onClicked: { dialog.upgradeRequested(); dialog.close() }
            }
        }
    }
}
