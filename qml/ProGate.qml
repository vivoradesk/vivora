import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Free-tier conversion teaser (devices.jsx ProGate).  Blurred "ghost" device
// rows behind a lock chip, the value prop, and the upgrade CTA.  Real QtQuick
// blur needs the GraphicsEffects module; we approximate with low-opacity
// placeholder blocks under a fade so no extra module has to be linked.
//
// The CTA used to read "Start 14-day free trial" and open nothing -- the
// trial entitlement is server-side work that has not shipped.  Advertising a
// trial we cannot grant is worse than advertising the price we can charge.
Rectangle {
    id: gate
    property DevicePalette pal: DevicePalette {}

    signal upgradeRequested()

    implicitWidth: 320
    implicitHeight: content.implicitHeight + 36
    radius: 12
    color: pal.paper
    border.width: 1
    border.color: pal.hairStrong
    clip: true

    // ── Ghost rows (decorative, behind) ─────────────────────────────────
    Column {
        id: ghost
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        spacing: 10
        opacity: 0.4
        Repeater {
            model: 3
            RowLayout {
                width: ghost.width
                spacing: 10
                Rectangle { Layout.preferredWidth: 30; Layout.preferredHeight: 30; radius: 8; color: gate.pal.paperDeep }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Rectangle { Layout.preferredHeight: 9; radius: 4; color: gate.pal.paperDeep
                                Layout.preferredWidth: (60 - index * 8) * ghost.width / 100 }
                    Rectangle { Layout.preferredHeight: 7; radius: 4; color: gate.pal.paperDeep
                                Layout.preferredWidth: ghost.width * 0.4 }
                }
                Rectangle { Layout.preferredWidth: 60; Layout.preferredHeight: 22; radius: 6; color: gate.pal.paperDeep }
            }
        }
    }
    // Fade the ghost out toward the bottom so it reads as "more below".
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 0.7; color: gate.pal.paper }
            GradientStop { position: 1.0; color: gate.pal.paper }
        }
    }

    // ── Content (front) ─────────────────────────────────────────────────
    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 18
        spacing: 0

        // Lock chip
        Rectangle {
            Layout.preferredHeight: 22
            Layout.preferredWidth: lockRow.implicitWidth + 18
            radius: 11
            color: gate.pal.blueSoft
            border.width: 1
            border.color: gate.pal.blueBorder
            Row {
                id: lockRow
                anchors.centerIn: parent
                spacing: 7
                Glyph { name: "lock"; size: 12; color: gate.pal.blue; anchors.verticalCenter: parent.verticalCenter }
                Label {
                    text: "VIVORA PRO"
                    color: gate.pal.blue
                    font.family: gate.pal.mono
                    font.pixelSize: 10
                    font.letterSpacing: 0.6
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.topMargin: 12
            text: "Reach every device you own — in one click"
            color: gate.pal.ink
            font.family: gate.pal.sans
            font.pixelSize: 17
            font.weight: Font.Medium
            wrapMode: Text.WordWrap
            lineHeight: 1.15
        }
        Label {
            Layout.fillWidth: true
            Layout.topMargin: 6
            text: "Sign in once and all your machines see each other automatically. "
                  + "No peer codes, no approval prompts — like a private network only you control."
            color: gate.pal.inkMid
            font.family: gate.pal.sans
            font.pixelSize: 13
            wrapMode: Text.WordWrap
            lineHeight: 1.35
        }

        // Bullets
        ColumnLayout {
            Layout.topMargin: 14
            Layout.bottomMargin: 16
            spacing: 7
            Repeater {
                model: [
                    "One-click connect, no code exchange",
                    "Skip the approval dialog on your own devices",
                    "Live online status across your fleet"
                ]
                RowLayout {
                    spacing: 9
                    Glyph { name: "check"; size: 13; color: gate.pal.green; Layout.alignment: Qt.AlignVCenter }
                    Label {
                        text: modelData
                        color: gate.pal.inkSoft
                        font.family: gate.pal.sans
                        font.pixelSize: 13
                    }
                }
            }
        }

        // CTA row
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Rectangle {
                Layout.preferredHeight: 40
                Layout.preferredWidth: ctaRow.implicitWidth + 28
                radius: 8
                color: ctaHover.containsMouse ? gate.pal.inkSoft : gate.pal.ink
                Row {
                    id: ctaRow
                    anchors.centerIn: parent
                    spacing: 8
                    Glyph { name: "spark"; size: 13; color: gate.pal.paper; anchors.verticalCenter: parent.verticalCenter }
                    Label {
                        text: "Upgrade to Pro"
                        color: gate.pal.paper
                        font.family: gate.pal.sans
                        font.pixelSize: 13
                        font.weight: Font.Medium
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                MouseArea {
                    id: ctaHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: gate.upgradeRequested()
                }
            }
            ColumnLayout {
                spacing: 0
                Label {
                    // Real Vivora Pro price — the handoff mockup's "$6/mo" is a
                    // placeholder.
                    text: "<b>$9.90</b>/month"
                    textFormat: Text.RichText
                    color: gate.pal.inkMid
                    font.family: gate.pal.mono
                    font.pixelSize: 11
                }
                Label {
                    text: "Cancel anytime"
                    color: gate.pal.inkMid
                    font.family: gate.pal.mono
                    font.pixelSize: 11
                }
            }
        }
    }
}
