// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

import QtQuick
import QtQuick.Controls

// Small tier pill used in the block header and the Settings account strip
// (devices.jsx `.dev-tier`).  tier: "pro" | "trial" | "locked".
Rectangle {
    id: badge
    property DevicePalette pal: DevicePalette {}
    property string tier: "pro"
    property string text: "Pro"

    readonly property string _glyph: tier === "trial" ? "spark"
                                    : tier === "locked" ? "lock" : "shield"

    implicitHeight: 20
    implicitWidth: row.implicitWidth + 16
    radius: 10
    color: tier === "trial" ? pal.amberSoft : pal.paperSoft
    border.width: 1
    border.color: tier === "trial" ? pal.amberBorder
                  : tier === "pro" ? pal.hairStrong : pal.hair

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 5
        Glyph {
            name: badge._glyph
            size: 11
            color: badge.tier === "trial" ? badge.pal.amber
                   : badge.tier === "locked" ? badge.pal.inkMid : badge.pal.ink
            anchors.verticalCenter: parent.verticalCenter
        }
        Label {
            text: badge.text
            color: badge.tier === "trial" ? badge.pal.amber
                   : badge.tier === "locked" ? badge.pal.inkMid : badge.pal.ink
            font.family: badge.pal.mono
            font.pixelSize: 10
            font.letterSpacing: 0.5
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
