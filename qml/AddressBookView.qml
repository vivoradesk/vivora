import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

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
    currentIndex: count > 0 ? 0 : -1

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
        radius: 4
        color: ListView.isCurrentItem
                  ? "#552196f3"
                  : (hoverArea.containsMouse ? "#22ffffff" : "transparent")

        property string roleAlias:  model.alias   || ""
        property string roleCode:   model.code    || ""
        property string rolePubkey: model.pubkey  || ""
        property string displayLabel: roleAlias.length > 0 ? roleAlias : roleCode

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 8

            Label {
                text: row.displayLabel
                font.bold: row.roleAlias.length > 0
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Label {
                text: row.roleAlias.length > 0 ? row.roleCode : ""
                opacity: 0.6
                font.family: "monospace"
                font.pointSize: 8
            }
        }

        MouseArea {
            id: hoverArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: (mouse) => {
                list.currentIndex = index
                if (mouse.button === Qt.RightButton) {
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
        opacity: 0.5
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
