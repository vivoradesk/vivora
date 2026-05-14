import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Compact list of remembered peers.  Single-click selects, double-click
// activates (= connect).  Right-click menu for rename / forget — Phase C.
ListView {
    id: list
    model: App.peers
    clip: true
    spacing: 2

    signal peerActivated(string alias, string pubkey, string code)
    signal peerRenameRequested(int row)

    delegate: Rectangle {
        id: row
        width: list.width
        height: 36
        color: hoverArea.containsMouse ? "#33ffffff" : "transparent"
        radius: 4

        property string roleAlias:  model.alias    || ""
        property string roleCode:   model.code     || ""
        property string rolePubkey: model.pubkey   || ""

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 8

            Label {
                text: row.roleAlias.length > 0 ? row.roleAlias : row.roleCode
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
            acceptedButtons: Qt.LeftButton
            onDoubleClicked: list.peerActivated(row.roleAlias, row.rolePubkey, row.roleCode)
        }
    }

    Label {
        anchors.centerIn: parent
        visible: list.count === 0
        opacity: 0.5
        text: "no recent peers yet"
    }
}
