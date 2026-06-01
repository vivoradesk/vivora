import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    modal: true
    title: "Incoming connection"
    standardButtons: Dialog.NoButton  // custom buttons below
    closePolicy: Popup.NoAutoClose

    property string approvalKey: ""
    property string peerCode:    ""
    property string pubkeyHex:   ""
    property string ipPort:      ""

    signal approved(string key)
    signal rejected(string key)

    // 30-second auto-reject timeout — Parsec/AnyDesk convention so a
    // user who walked away doesn't leave a dialog hanging forever.
    property int    secondsRemaining: 30

    Timer {
        id: countdown
        interval: 1000
        repeat: true
        running: dialog.visible
        onTriggered: {
            dialog.secondsRemaining -= 1
            if (dialog.secondsRemaining <= 0) {
                dialog.rejected(dialog.approvalKey)
                dialog.accept()
            }
        }
    }

    ColumnLayout {
        spacing: 12
        Layout.minimumWidth: 360

        Label {
            text: "A peer is asking to connect to your desktop"
            color: "#15151a"
            font.pixelSize: 14
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        ColumnLayout {
            spacing: 4
            Label {
                text: dialog.peerCode.length > 0 ? dialog.peerCode : "(code pending)"
                color: "#15151a"
                font.family: "Geist Mono, JetBrains Mono, Consolas, monospace"
                font.pixelSize: 14
                font.bold: true
            }
            Label {
                text: "From " + dialog.ipPort
                color: "#6b6b75"
                font.pixelSize: 11
                font.family: "Geist Mono, JetBrains Mono, Consolas, monospace"
            }
            Label {
                visible: dialog.pubkeyHex.length > 8
                text: "Pubkey ED25:" + dialog.pubkeyHex.substring(0, 6) + "…"
                       + dialog.pubkeyHex.substring(dialog.pubkeyHex.length - 4)
                color: "#6b6b75"
                font.pixelSize: 11
                font.family: "Geist Mono, JetBrains Mono, Consolas, monospace"
            }
        }

        Label {
            text: "Auto-rejecting in " + dialog.secondsRemaining + "s if you don't decide."
            color: "#6b6b75"
            font.pixelSize: 11
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Button {
                text: "Reject"
                Layout.fillWidth: true
                onClicked: { dialog.rejected(dialog.approvalKey); dialog.accept() }
            }
            Button {
                text: "Accept"
                Layout.fillWidth: true
                highlighted: true
                onClicked: { dialog.approved(dialog.approvalKey); dialog.accept() }
            }
        }
    }
}
