// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Asks for a host's unattended-access password before dialling it.
//
// Opened from the device menu ("Connect with password…") and again when the
// host refused a password, with the reason in `message`.  The password is
// handed to App.connectWithPassword and not kept: nothing here or in the C++
// side stores it.
Dialog {
    id: dialog
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 20
    width: 380
    anchors.centerIn: Overlay.overlay

    property string peer: ""
    // Why we are asking again ("Wrong password", locked out).  Empty on a
    // first ask.
    property string message: ""

    signal submitted(string password)

    QtObject {
        id: t
        readonly property color bg:         "#fbfaf7"
        readonly property color text:       "#1a1a1f"
        readonly property color textMuted:  "#6f6b60"
        readonly property color border:     "#d8d2c2"
        readonly property color accent:     "#3D6BFA"
        readonly property color accentSoft: "#e8edfe"
        readonly property color danger:     "#c2412d"
        readonly property color dangerSoft: "#fbe9e5"
        readonly property string mono:      App.monoFont
    }

    background: Rectangle {
        color: t.bg
        radius: 14
        border.color: t.border
        border.width: 1
    }
    header: Item { implicitHeight: 0 }

    onOpened: pwField.forceActiveFocus()

    function submit() {
        if (pwField.text.length === 0) return
        const pw = pwField.text
        pwField.text = ""
        dialog.submitted(pw)
        dialog.close()
    }

    // Same shape as RelayProDialog's button.
    component DlgButton: Rectangle {
        property string label: ""
        property bool   primary: false
        property bool   enabledState: true
        signal clicked
        Layout.preferredHeight: 40
        radius: 8
        opacity: enabledState ? 1.0 : 0.45
        color: primary
               ? (hover.containsMouse && enabledState ? Qt.darker(t.accent, 1.12) : t.accent)
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
            onClicked: if (parent.enabledState) parent.clicked()
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
                    text: "🔑"
                    font.pixelSize: 16
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    text: "ACCESS PASSWORD"
                    color: t.accent
                    font.pixelSize: 10
                    font.bold: true
                    font.letterSpacing: 1.2
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.peer.length > 0 ? "Connect to " + dialog.peer
                                                 : "Connect with a password"
                    color: t.text
                    font.pixelSize: 14
                    font.bold: true
                    elide: Text.ElideMiddle
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: "If the other computer has unattended access turned on, its "
                  + "password lets you in without anyone there to accept."
            color: t.textMuted
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            lineHeight: 1.3
        }

        Rectangle {
            visible: dialog.message.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: errLbl.implicitHeight + 16
            radius: 8
            color: t.dangerSoft
            Label {
                id: errLbl
                anchors.fill: parent
                anchors.margins: 8
                text: dialog.message
                color: t.danger
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }

        TextField {
            id: pwField
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            echoMode: TextInput.Password
            placeholderText: "Password"
            color: t.text
            font.pixelSize: 13
            leftPadding: 10
            background: Rectangle {
                radius: 8
                color: "#ffffff"
                border.color: pwField.activeFocus ? t.accent : t.border
                border.width: 1
            }
            Keys.onReturnPressed: dialog.submit()
            Keys.onEnterPressed: dialog.submit()
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 2
            spacing: 9
            DlgButton {
                label: "Cancel"
                Layout.preferredWidth: 110
                onClicked: { pwField.text = ""; dialog.close() }
            }
            DlgButton {
                label: "Connect"
                primary: true
                enabledState: pwField.text.length > 0
                Layout.fillWidth: true
                onClicked: dialog.submit()
            }
        }
    }
}
