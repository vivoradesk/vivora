import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: dlg
    width: 600
    height: 540
    minimumWidth: 480
    minimumHeight: 420
    title: "DeskBeam — Settings"

    signal closed()

    onVisibleChanged: if (!visible) closed()

    TabBar {
        id: bar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        TabButton { text: "General" }
        TabButton { text: "Network" }
        TabButton { text: "Capture" }
        TabButton { text: "Idle"    }
    }

    StackLayout {
        anchors.top: bar.bottom
        anchors.bottom: footer.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 16
        currentIndex: bar.currentIndex

        // ── General ────────────────────────────────────────────────
        ColumnLayout {
            spacing: 12
            CheckBox {
                text: "Start sharing automatically on launch"
                checked: App.settings.startSharingOnLaunch
                onToggled: App.settings.startSharingOnLaunch = checked
            }
            CheckBox {
                text: "Minimize to tray on window close"
                checked: App.settings.minimizeToTray
                onToggled: App.settings.minimizeToTray = checked
            }
            CheckBox {
                text: "Start at login (TODO: wire to OS auto-start)"
                enabled: false
                checked: App.settings.startAtLogin
                onToggled: App.settings.startAtLogin = checked
            }
            Item { Layout.fillHeight: true }
        }

        // ── Network ────────────────────────────────────────────────
        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 8

            Label { text: "Rendezvous (host:port):" }
            TextField {
                Layout.fillWidth: true
                placeholderText: "rdv.deskbeam.dev:7000"
                text: App.settings.rendezvous
                onEditingFinished: App.settings.rendezvous = text
            }

            Label { text: "Relay (host:port):" }
            TextField {
                Layout.fillWidth: true
                placeholderText: "relay.deskbeam.dev:7100"
                text: App.settings.relay
                onEditingFinished: App.settings.relay = text
            }

            Label { text: "License token file:" }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    Layout.fillWidth: true
                    text: App.settings.licenseFile
                    onEditingFinished: App.settings.licenseFile = text
                }
                Button {
                    text: "Browse…"
                    onClicked: licenseDlg.open()
                }
            }
            FileDialog {
                id: licenseDlg
                title: "Select license token"
                onAccepted: App.settings.licenseFile = selectedFile.toString().replace("file:///", "")
            }

            Label { text: "STUN server:" }
            TextField {
                Layout.fillWidth: true
                placeholderText: "stun.l.google.com:19302"
                text: App.settings.stunServer
                onEditingFinished: App.settings.stunServer = text
            }

            Label { text: "Host UDP port:" }
            RowLayout {
                Layout.fillWidth: true
                SpinBox {
                    Layout.fillWidth: true
                    from: 1024; to: 65535; value: App.settings.hostPort
                    editable: true
                    onValueModified: App.settings.hostPort = value
                }
                Label {
                    text: "(viewer must use --port " + App.settings.hostPort + ")"
                    opacity: 0.6
                    font.italic: true
                }
            }
            Item { Layout.columnSpan: 2; Layout.fillHeight: true }
        }

        // ── Capture ────────────────────────────────────────────────
        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 8

            Label { text: "Codec:" }
            ComboBox {
                Layout.fillWidth: true
                model: ["H.264", "HEVC"]
                currentIndex: App.settings.codecIndex
                onActivated: App.settings.codecIndex = currentIndex
            }

            Label { text: "Encoder:" }
            ComboBox {
                Layout.fillWidth: true
                model: ["Auto", "AMD (AMF)", "NVIDIA (NVENC)", "Intel (QSV)"]
                currentIndex: App.settings.encoderIndex
                onActivated: App.settings.encoderIndex = currentIndex
            }

            Label { text: "Bitrate (Mbps, 0 = auto):" }
            SpinBox {
                Layout.fillWidth: true
                from: 0; to: 200; value: App.settings.bitrateMbps
                onValueModified: App.settings.bitrateMbps = value
            }

            Label { text: "Display index:" }
            SpinBox {
                Layout.fillWidth: true
                from: 0; to: 8; value: App.settings.displayIndex
                onValueModified: App.settings.displayIndex = value
            }
            Item { Layout.columnSpan: 2; Layout.fillHeight: true }
        }

        // ── Idle ───────────────────────────────────────────────────
        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 8

            Label { text: "Disconnect after idle (minutes):" }
            SpinBox {
                Layout.fillWidth: true
                from: 1; to: 120; value: App.settings.idleTimeoutMin
                onValueModified: App.settings.idleTimeoutMin = value
            }

            Label { text: "Warning before disconnect (seconds):" }
            SpinBox {
                Layout.fillWidth: true
                from: 5; to: 120; value: App.settings.idleWarningSec
                onValueModified: App.settings.idleWarningSec = value
            }
            Item { Layout.columnSpan: 2; Layout.fillHeight: true }
        }
    }

    RowLayout {
        id: footer
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: 12
        Button { text: "Close"; onClicked: dlg.close() }
    }
}
