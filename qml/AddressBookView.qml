import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// Compact list of remembered peers.
//   left-click  / Enter / Return — select
//   double-click / Enter on selected — activate (connect)
//   right-click OR the ⋯ button at row end — context menu
//   F2 on selected — Set alias / Rename
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

    readonly property string monoFont: "JetBrains Mono, Cascadia Mono, Consolas, monospace"

    // Relative "seen 2h ago" formatting for the context-menu header.
    // model.lastSeen arrives as a JS Date (QDateTime, UTC instant).
    function relTime(d) {
        if (!d || isNaN(d.getTime()) || d.getTime() <= 0) return ""
        var s = Math.max(0, (Date.now() - d.getTime()) / 1000)
        if (s < 45)      return "just now"
        var m = Math.floor(s / 60)
        if (m < 60)      return m + "m ago"
        var h = Math.floor(m / 60)
        if (h < 24)      return h + "h ago"
        var dys = Math.floor(h / 24)
        if (dys < 7)     return dys + "d ago"
        var w = Math.floor(dys / 7)
        if (w < 5)       return w + "w ago"
        return Math.floor(dys / 30) + "mo ago"
    }

    // ── Inline rename / forget dialogs ─────────────────────────────────
    // Kept inside the view so the parent (main.qml) doesn't have to
    // wire row-index marshalling for what is effectively row-local UI.

    property int    pendingRow:     -1
    property string pendingAlias:   ""
    property string pendingLabel:   ""

    // Themed footer button shared by the rename / forget dialogs — custom
    // Rectangle background (stock Qt buttons are white pills that clash
    // with the cream theme).  primary = dark fill, danger = red.
    component DlgBtn: Rectangle {
        id: btn
        property string label: ""
        property bool primary: false
        property bool danger: false
        signal clicked
        implicitHeight: 38
        radius: 8
        color: primary
               ? (danger ? (ma.containsMouse ? "#b83232" : "#cf3b3b")
                         : (ma.containsMouse ? "#2a2a32" : "#1a1a1f"))
               : (ma.containsMouse ? "#efe9dc" : "#fbfaf7")
        border.color: primary ? "transparent" : "#d8d2c2"
        border.width: 1
        Label {
            anchors.centerIn: parent
            text: btn.label
            color: btn.primary ? "#ffffff" : "#1a1a1f"
            font.pixelSize: 13
            font.bold: btn.primary
        }
        MouseArea {
            id: ma
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: btn.clicked()
        }
    }

    Dialog {
        id: renameDlg
        modal: true
        // Centre in the window overlay — a Popup is reparented there on
        // open, so anchoring to parent.parent lands it in the top-left.
        anchors.centerIn: Overlay.overlay
        padding: 18
        width: 330
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: "#fbfaf7"
            radius: 13
            border.color: "#d8d2c2"
            border.width: 1
        }
        header: Item { implicitHeight: 0 }   // custom title below
        contentItem: ColumnLayout {
            spacing: 12
            Label {
                text: "Rename peer"
                color: "#1a1a1f"
                font.bold: true
                font.pixelSize: 15
            }
            Label {
                text: "Alias for " + list.pendingLabel
                color: "#6f6b60"
                font.pixelSize: 12
                font.family: list.monoFont
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            TextField {
                id: renameField
                Layout.fillWidth: true
                Layout.minimumWidth: 290
                text: list.pendingAlias
                placeholderText: "e.g. Mom's Mac"
                placeholderTextColor: "#9b9686"
                color: "#1a1a1f"
                font.pixelSize: 13
                selectByMouse: true
                selectionColor: "#3D6BFA"
                selectedTextColor: "#ffffff"
                background: Rectangle {
                    color: "#ffffff"
                    radius: 7
                    border.color: renameField.activeFocus ? "#3D6BFA" : "#d8d2c2"
                    border.width: renameField.activeFocus ? 2 : 1
                }
                onAccepted: renameDlg.accept()
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 2
                spacing: 9
                DlgBtn {
                    Layout.fillWidth: true
                    label: "Cancel"
                    onClicked: renameDlg.reject()
                }
                DlgBtn {
                    Layout.fillWidth: true
                    label: "Save"
                    primary: true
                    onClicked: renameDlg.accept()
                }
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
        // Centre in the window overlay — a Popup is reparented there on
        // open, so anchoring to parent.parent lands it in the top-left.
        anchors.centerIn: Overlay.overlay
        padding: 18
        width: 340
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: "#fbfaf7"
            radius: 13
            border.color: "#d8d2c2"
            border.width: 1
        }
        header: Item { implicitHeight: 0 }
        contentItem: ColumnLayout {
            spacing: 10
            Label {
                text: "Forget peer"
                color: "#1a1a1f"
                font.bold: true
                font.pixelSize: 15
            }
            Label {
                text: "Forget " + list.pendingLabel + "?"
                color: "#1a1a1f"
                font.pixelSize: 13
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                text: "Removes the local record and the trusted key pin — "
                      + "you'll re-verify its key on the next connect."
                color: "#6f6b60"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 9
                DlgBtn {
                    Layout.fillWidth: true
                    label: "Cancel"
                    onClicked: forgetDlg.reject()
                }
                DlgBtn {
                    Layout.fillWidth: true
                    label: "Forget"
                    primary: true
                    danger: true
                    onClicked: forgetDlg.accept()
                }
            }
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

    // ── Context-menu row, styled to match the cream theme ──────────────
    // A MenuItem with a glyph + label (+ optional shortcut hint), custom
    // hover highlight and danger/disabled colouring.  Declared at the
    // view root so the per-row Menu in the delegate can reuse it.
    component MItem: MenuItem {
        id: mi
        property string glyph: ""
        property string shortcut: ""
        property color  glyphColor: "#5a5750"
        property color  labelColor: "#1a1a1f"
        property bool   strong: false
        implicitHeight: 34
        indicator: Item {}   // suppress the default checkmark gutter
        arrow: Item {}
        contentItem: RowLayout {
            spacing: 10
            Label {
                text: mi.glyph
                color: mi.enabled ? mi.glyphColor : "#bcb6a6"
                font.pixelSize: 14
                Layout.preferredWidth: 16
                horizontalAlignment: Text.AlignHCenter
            }
            Label {
                text: mi.text
                color: mi.enabled ? mi.labelColor : "#bcb6a6"
                font.pixelSize: 13
                font.bold: mi.strong
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Label {
                text: mi.shortcut
                visible: mi.shortcut.length > 0
                color: "#a39e8e"
                font.pixelSize: 13
            }
        }
        background: Rectangle {
            radius: 6
            color: mi.highlighted ? "#e6ded0" : "transparent"
        }
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
        color: hoverHandler.hovered ? "#ded8c8" : "transparent"

        HoverHandler {
            id: hoverHandler
        }

        property string roleAlias:  model.alias   || ""
        property string roleCode:   model.code    || ""
        property string rolePubkey: model.pubkey  || ""
        property int    roleDirection: model.direction || 0  // 0=unknown 1=out 2=in
        property var    roleLastSeen:  model.lastSeen
        property bool   roleTrusted:   model.trusted || false
        property bool   rolePinned:    model.pinned || false
        property string displayLabel: roleAlias.length > 0 ? roleAlias : roleCode

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            // Reserve room on the right for the always-present ⋯ button.
            anchors.rightMargin: 32
            spacing: 8

            // Status dot (mirrors the mockup).  Pinned rows take the brand
            // blue (per the design's `.row.pinned .dot`); others stay a
            // neutral grey until an "online" presence indicator lands.
            Rectangle {
                width: 7; height: 7; radius: 4
                color: row.rolePinned ? "#3D6BFA" : "#9b9686"
                Layout.alignment: Qt.AlignVCenter
            }

            // Explicit text colour — without it macOS Dark mode QPalette
            // picks white-on-cream and the labels become unreadable on
            // the warm-bg window.
            Label {
                text: row.displayLabel
                color: "#1a1a1f"
                font.family: list.monoFont
                font.bold: row.roleAlias.length > 0
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Label {
                text: row.roleAlias.length > 0 ? row.roleCode : ""
                color: "#6f6b60"
                font.family: list.monoFont
                font.pointSize: 8
            }
            // Relative "last seen" — the design's `.when` column, right
            // of the code, mono + faint.
            Label {
                text: list.relTime(row.roleLastSeen)
                visible: text.length > 0
                color: "#8a8a90"
                font.family: list.monoFont
                font.pixelSize: 10
                Layout.alignment: Qt.AlignVCenter
            }
            // Pin marker: flat accent glyph shown only on pinned rows,
            // matching the ⇡ glyph used by the Pin/Unpin menu action.
            Label {
                text: "⇡"
                visible: row.rolePinned
                color: "#3D6BFA"
                font.pixelSize: 12
                Layout.alignment: Qt.AlignVCenter
            }
            // Direction arrow: ↑ outgoing, ↓ incoming, blank for
            // unknown / legacy entries.
            Label {
                text: row.roleDirection === 1 ? "↑"
                       : (row.roleDirection === 2 ? "↓" : "")
                color: "#6f6b60"
                font.pixelSize: 12
                Layout.alignment: Qt.AlignVCenter
            }
        }

        MouseArea {
            id: hoverArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: (mouse) => {
                if (mouse.button === Qt.RightButton) {
                    // Anchor keyboard selection here too so F2 / Delete
                    // target the right row.
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

        // ── ⋯ affordance ──────────────────────────────────────────────
        // Declared after hoverArea so it sits on top and gets the click.
        // Always faintly visible (discoverable without knowing about
        // right-click), full opacity on row hover or while its menu is
        // open.
        Rectangle {
            id: kebab
            width: 24; height: 24; radius: 6
            anchors.right: parent.right
            anchors.rightMargin: 5
            anchors.verticalCenter: parent.verticalCenter
            color: kebabArea.containsMouse ? "#cfc8b6" : "transparent"
            opacity: (hoverHandler.hovered || rowMenu.opened) ? 1.0 : 0.4
            Behavior on opacity { NumberAnimation { duration: 100 } }
            Label {
                anchors.centerIn: parent
                text: "⋯"
                color: "#5a5750"
                font.pixelSize: 16
                font.bold: true
            }
            MouseArea {
                id: kebabArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    list.currentIndex = index
                    // Right-align the menu under the button.
                    rowMenu.popup(kebab, kebab.width - rowMenu.width, kebab.height + 2)
                }
            }
        }

        // ── Context menu (right-click or ⋯) ───────────────────────────
        Menu {
            id: rowMenu
            width: 232
            padding: 6
            background: Rectangle {
                color: "#f6f3ec"
                radius: 11
                border.color: "#dcd6c5"
                border.width: 1
            }

            // Header: status dot + name + code · seen.
            Item {
                width: rowMenu.availableWidth
                implicitHeight: 44
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 9
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        color: "#3D6BFA"
                        Layout.alignment: Qt.AlignVCenter
                    }
                    ColumnLayout {
                        spacing: 1
                        Layout.fillWidth: true
                        Label {
                            text: row.displayLabel
                            color: "#1a1a1f"
                            font.bold: true
                            font.pixelSize: 13
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            text: {
                                var rt = list.relTime(row.roleLastSeen)
                                return row.roleCode + (rt.length > 0 ? "  ·  seen " + rt : "")
                            }
                            color: "#6f6b60"
                            font.family: list.monoFont
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            MenuSeparator {
                padding: 6
                contentItem: Rectangle { implicitHeight: 1; color: "#e2dccb" }
            }

            MItem {
                text: "Connect"
                glyph: "→"
                glyphColor: "#3D6BFA"
                strong: true
                shortcut: "↵"
                onTriggered: list.peerActivated(row.roleAlias, row.rolePubkey, row.roleCode)
            }

            MenuSeparator {
                padding: 6
                contentItem: Rectangle { implicitHeight: 1; color: "#e2dccb" }
            }

            MItem {
                text: row.roleAlias.length > 0 ? "Rename…" : "Set alias…"
                glyph: "⌨"
                onTriggered: list.openRename(index, row.roleAlias, row.displayLabel)
            }
            MItem {
                text: "Copy peer code"
                glyph: "⧉"
                enabled: row.roleCode.length > 0
                onTriggered: clipText.text = row.roleCode
            }
            MItem {
                text: "Copy pubkey hex"
                glyph: "⧉"
                enabled: row.rolePubkey.length > 0
                onTriggered: clipText.text = row.rolePubkey
            }
            // Auto-accept (trust) toggle — lets the user grant or revoke the
            // "don't ask again" permission set from the approval dialog.
            MItem {
                text: row.roleTrusted ? "Stop auto-accepting" : "Always auto-accept"
                glyph: row.roleTrusted ? "⊘" : "✓"
                enabled: row.rolePubkey.length > 0
                onTriggered: App.peers.setTrusted(index, !row.roleTrusted)
            }
            // Pin/unpin — pinned peers float to the top of the Recent list.
            MItem {
                text: row.rolePinned ? "Unpin" : "Pin"
                glyph: "⇡"
                enabled: row.rolePubkey.length > 0
                onTriggered: App.peers.setPinned(index, !row.rolePinned)
            }

            MenuSeparator {
                padding: 6
                contentItem: Rectangle { implicitHeight: 1; color: "#e2dccb" }
            }

            MItem {
                text: "Forget"
                glyph: "✕"
                glyphColor: "#cf3b3b"
                labelColor: "#cf3b3b"
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
        color: "#6f6b60"
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
