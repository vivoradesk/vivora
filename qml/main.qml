import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: window
    width: 520
    height: 620
    minimumWidth: 420
    minimumHeight: 480
    visible: true
    title: "Vivora"

    // The app stays running in the tray; closing the window only hides it.
    // The Quit menu entry (in the tray) is the explicit way out.
    onClosing: (close) => {
        if (App.settings.minimizeToTray) {
            close.accepted = false
            window.hide()
        }
    }

    // Bring the window back from the tray when AppController asks.
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

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        // ── My host section ──────────────────────────────────────────
        GroupBox {
            title: "My host"
            Layout.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                RowLayout {
                    Label {
                        text: App.sharing ? "● Sharing" : "○ Idle"
                        color: App.sharing ? "#4caf50" : "#9e9e9e"
                        font.bold: true
                    }
                    Label {
                        visible: App.sharing
                        text: App.clientCount === 1
                              ? "(1 client)"
                              : `(${App.clientCount} clients)`
                        opacity: 0.7
                    }
                    Item { Layout.fillWidth: true }
                    Button {
                        text: App.sharing ? "Stop sharing" : "Start sharing"
                        onClicked: App.sharing ? App.stopSharing() : App.startSharing()
                    }
                }

                RowLayout {
                    visible: App.sharing
                    Label { text: "Code:"; opacity: 0.7 }
                    Label {
                        text: App.myPeerCode
                        font.family: "monospace"
                        Layout.fillWidth: true
                    }
                    Button {
                        text: "Copy"
                        onClicked: clipboardHelper.copy(App.myPeerCode)
                    }
                }

                RowLayout {
                    visible: App.sharing
                    Label { text: "Pubkey:"; opacity: 0.7 }
                    Label {
                        text: App.myPubkeyHex.substring(0, 16) + "…"
                        font.family: "monospace"
                        opacity: 0.7
                        Layout.fillWidth: true
                    }
                    Button {
                        text: "Copy hex"
                        onClicked: clipboardHelper.copy(App.myPubkeyHex)
                    }
                }
            }
        }

        // ── Connect section ──────────────────────────────────────────
        GroupBox {
            title: "Connect to a peer"
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                RowLayout {
                    TextField {
                        id: peerInput
                        Layout.fillWidth: true
                        placeholderText: "peer code (e.g. swift-tiger-4271)"
                        onAccepted: connectBtn.clicked()
                    }
                    Button {
                        id: connectBtn
                        text: "Connect"
                        enabled: peerInput.text.length > 0
                        onClicked: {
                            App.connectToPeer(peerInput.text)
                            peerInput.text = ""
                        }
                    }
                }

                Label {
                    text: "Recent"
                    opacity: 0.7
                    visible: App.peers.rowCount() > 0
                }
                AddressBookView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    onPeerActivated: (alias, pubkey, code) => {
                        // Connect by the most stable identifier we have.
                        App.connectToPeer(pubkey.length > 0 ? pubkey : code)
                    }
                    // Rename / Forget now live inside the view's right-click
                    // context menu (Phase C).  No host-side wiring needed
                    // because AddressBook backend mutators are Q_INVOKABLE.
                }
            }
        }

        // ── Footer ────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Button {
                text: "Settings…"
                onClicked: App.openSettings()
            }
            Item { Layout.fillWidth: true }
            Button {
                text: "Hide"
                onClicked: window.hide()
            }
        }
    }

    // Tiny QtObject helper for clipboard access from QML — Qt 6 doesn't
    // expose a clipboard primitive on the QML side without a tiny C++
    // shim.  Put the shim inline as a plain Item using TextEdit's
    // selectAll/copy trick.
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
