// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// Used both for "Add a new peer manually" and (future) "Rename existing".
// Phase A scope: the rename path is stubbed — we only invoke this for new
// entries today.  Add/edit logic lives here so Phase C can extend it.
Window {
    id: dlg
    width: 420
    height: 220
    title: "Add peer"

    property string initialAlias: ""
    property string initialCode: ""
    property string initialPubkey: ""

    signal closed()
    onVisibleChanged: if (!visible) closed()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label { text: "Alias (your name for this peer):" }
        TextField {
            id: aliasField
            Layout.fillWidth: true
            text: dlg.initialAlias
            placeholderText: "e.g. Mom's Mac"
        }

        Label { text: "Peer code or pubkey hex:" }
        TextField {
            id: idField
            Layout.fillWidth: true
            text: dlg.initialCode.length > 0 ? dlg.initialCode : dlg.initialPubkey
            placeholderText: "swift-tiger-4271 OR 64-char hex"
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            Button { text: "Cancel"; onClicked: dlg.close() }
            Button {
                text: "Save"
                enabled: idField.text.length > 0
                onClicked: {
                    // Phase C will resolve idField.text into pubkey via
                    // rendezvous lookup if it's a code.  For now we just
                    // store whatever the user typed verbatim — the
                    // address book accepts any pubkey hex; codes survive
                    // as the lastPeerCode field.
                    App.peers.touch(idField.text, idField.text)
                    dlg.close()
                }
            }
        }
    }
}
