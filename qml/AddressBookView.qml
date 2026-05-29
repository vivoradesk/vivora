import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// Compact list of remembered peers.
//   left-click  / Enter / Return — select
//   double-click / Enter on selected — activate (connect)
//   right-click — context menu (Rename / Forget)
//   F2 on selected — Rename
//   Delete on selected — Forget
ListView {
    id: list
    model: App.peers
    clip: true
    spacing: 2
    focus: true
    // Default to no selection — the design is mouse-first, so a
    // persistent highlight on the first row reads as "selected" when
    // it's really just a focus cursor.  Keyboard nav still works
    // (arrow keys / Enter); selection appears the moment the list
    // gains keyboard focus via Tab or arrow press.
    currentIndex: -1

    signal peerActivated(string alias, string pubkey, string code)

    // ── Inline rename / forget dialogs ─────────────────────────────────
    // Kept inside the view so the parent (main.qml) doesn't have to
    // wire row-index marshalling for what is effectively row-local UI.

    property int    pendingRow:     -1
    property string pendingAlias:   ""
    property string pendingLabel:   ""

    Dialog {
        id: renameDlg
        modal: true
        anchors.centerIn: parent.parent ? parent.parent : undefined
        title: "Rename peer"
        standardButtons: Dialog.Save | Dialog.Cancel

        ColumnLayout {
            spacing: 8
            Label {
                text: "Alias for " + list.pendingLabel
                opacity: 0.7
            }
            TextField {
                id: renameField
                Layout.fillWidth: true
                Layout.minimumWidth: 280
                text: list.pendingAlias
                placeholderText: "e.g. Mom's Mac"
                onAccepted: renameDlg.accept()
            }
        }

        onAccepted: {
            if (list.pendingRow >= 0) {
                App.peers.setAlias(list.pendingRow, renameField.text.trim())
            }
        }
        onOpened: { renameField.selectAll(); renameField.forceActiveFocus() }
    }

    Dialog {
        id: forgetDlg
        modal: true
        anchors.centerIn: parent.parent ? parent.parent : undefined
        title: "Forget peer"
        standardButtons: Dialog.Yes | Dialog.No
        Label {
            text: "Forget " + list.pendingLabel + "?\n" +
                  "The peer can reconnect later; only the local record is removed."
        }
        onAccepted: {
            if (list.pendingRow >= 0) App.peers.remove(list.pendingRow)
        }
    }

    function openRename(row, alias, label) {
        list.pendingRow   = row
        list.pendingAlias = alias
        list.pendingLabel = label
        renameDlg.open()
    }

    function openForget(row, label) {
        list.pendingRow   = row
        list.pendingLabel = label
        forgetDlg.open()
    }

    // ── Per-row delegate ──────────────────────────────────────────────
    delegate: Rectangle {
        id: row
        width: list.width
        height: 36
        radius: 6
        // Hover-only highlight via HoverHandler — keyboard selection
        // visual is intentionally dropped to match the mouse-first
        // design.  Keyboard nav (arrows / Enter) still works, just
        // without a persistent selection bar.
        color: hoverHandler.hovered ? "#dcd2b4" : "transparent"

        HoverHandler {
            id: hoverHandler
        }

        property string roleAlias:  model.alias   || ""
        property string roleCode:   model.code    || ""
        property string rolePubkey: model.pubkey  || ""
        property string displayLabel: roleAlias.length > 0 ? roleAlias : roleCode

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 8

            // Explicit text colour — without it macOS Dark mode QPalette
            // picks white-on-cream and the labels become unreadable on
            // the warm-bg window.
            Label {
                text: row.displayLabel
                color: "#15151a"
                font.family: "Geist Mono, JetBrains Mono, Cascadia Mono, Consolas, monospace"
                font.bold: row.roleAlias.length > 0
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Label {
                text: row.roleAlias.length > 0 ? row.roleCode : ""
                color: "#6b6b75"
                font.family: "Geist Mono, JetBrains Mono, Cascadia Mono, Consolas, monospace"
                font.pointSize: 8
            }
        }

        MouseArea {
            id: hoverArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: (mouse) => {
                if (mouse.button === Qt.RightButton) {
                    // For the context menu we still want the keyboard
                    // selection to anchor here (so F2 / Delete target
                    // the right row).
                    list.currentIndex = index
                    rowMenu.popup()
                }
            }
            onDoubleClicked: (mouse) => {
                if (mouse.button === Qt.LeftButton) {
                    list.peerActivated(row.roleAlias, row.rolePubkey, row.roleCode)
                }
            }
        }

        Menu {
            id: rowMenu
            MenuItem {
                text: "Connect"
                onTriggered: list.peerActivated(row.roleAlias, row.rolePubkey, row.roleCode)
            }
            MenuSeparator {}
            MenuItem {
                text: row.roleAlias.length > 0 ? "Rename…" : "Set alias…"
                onTriggered: list.openRename(index, row.roleAlias, row.displayLabel)
            }
            MenuItem {
                text: "Copy peer code"
                enabled: row.roleCode.length > 0
                onTriggered: clipText.text = row.roleCode
            }
            MenuItem {
                text: "Copy pubkey hex"
                enabled: row.rolePubkey.length > 0
                onTriggered: clipText.text = row.rolePubkey
            }
            MenuSeparator {}
            MenuItem {
                text: "Forget"
                onTriggered: list.openForget(index, row.displayLabel)
            }
        }
    }

    // Keyboard shortcuts on the focused list.
    Keys.onPressed: (e) => {
        if (currentIndex < 0) return
        const item = itemAtIndex(currentIndex)
        if (!item) return
        if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) {
            list.peerActivated(item.roleAlias, item.rolePubkey, item.roleCode)
            e.accepted = true
        } else if (e.key === Qt.Key_F2) {
            list.openRename(currentIndex, item.roleAlias, item.displayLabel)
            e.accepted = true
        } else if (e.key === Qt.Key_Delete) {
            list.openForget(currentIndex, item.displayLabel)
            e.accepted = true
        }
    }

    Label {
        anchors.centerIn: parent
        visible: list.count === 0
        color: "#6b6b75"
        text: "no recent peers yet"
    }

    // Hidden TextEdit used for clipboard "Copy peer code / pubkey hex" —
    // Qt 6 QML has no direct clipboard primitive without a C++ shim, but
    // TextEdit.copy() goes through QClipboard internally.
    TextEdit {
        id: clipText
        visible: false
        onTextChanged: { if (text.length > 0) { selectAll(); copy() } }
    }
}
