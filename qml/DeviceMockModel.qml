// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

import QtQuick

// ===========================================================================
//  Device-mesh provider (VIV-52).
//
//  Selects between the REAL account device list (App.myDevices, a C++
//  DeviceMeshModel populated from /devices/me) and the isolated MOCK data
//  below, then exposes ONE uniform surface the "My Devices" UI binds to:
//    · variant      "pro" | "trial" | "free" | "empty" | "cached"
//    · tier         "pro" | "trial" | "free"
//    · model        main-window list (roles: devId, devName, os, online,
//                   current, warned, seen, peerCode, host)
//    · settingsModel settings-table list (same roles + host)
//    · renameMain(id,name) / removeMain(id)
//
//  Selection:
//    · VIVORA_DEVICES_VARIANT env var set  → PREVIEW: force that variant +
//      mock data, so every visual state renders in a build with no account
//      (keeps the screenshot workflow working).
//    · else signed in                      → REAL: App.myDevices, variant from
//      App.deviceMeshTier (+ meshRefreshing → "cached", + lone/empty device
//      list → "empty").
//    · else (signed out, no preview)       → "free" gate with mock data behind
//      the ProGate.
//
//  The type is still named DeviceMockModel so MyDevicesBlock / MyDevicesSettings
//  need no edits (they declare `property DeviceMockModel mock`).
// ===========================================================================
Item {
    id: provider

    // Preview override: read once from VIVORA_DEVICES_VARIANT (surfaced as a
    // context property by gui_main.cpp).  Empty string = not previewing.
    readonly property string _envVariant: {
        var v = (typeof VivoraDevicesVariant !== "undefined") ? String(VivoraDevicesVariant) : ""
        v = v.toLowerCase()
        return (v === "pro" || v === "trial" || v === "free" || v === "empty" || v === "cached")
               ? v : ""
    }
    readonly property bool _preview: _envVariant.length > 0
    readonly property bool _real: !_preview && (typeof App !== "undefined") && App.accountLoggedIn

    // Number of OTHER devices (real path) — drives the empty state.
    function _nonCurrentCount() {
        if (typeof App === "undefined" || !App.myDevices) return 0
        var m = App.myDevices, n = 0
        for (var i = 0; i < m.count; ++i) {
            var d = m.get(i)
            if (!d.current) n++
        }
        return n
    }

    // variant: "pro" | "trial" | "free" | "empty" | "cached"
    property string variant: {
        if (_preview) return _envVariant
        if (typeof App === "undefined" || !App.accountLoggedIn) return "free"
        if (App.deviceMeshTier !== "pro") return "free"
        // Reading .count here binds the variant to model changes.
        var total = App.myDevices ? App.myDevices.count : 0
        if (App.meshRefreshing && total > 0) return "cached"
        if (_nonCurrentCount() === 0) return "empty"
        return "pro"
    }

    // Coarse entitlement for the Settings surface.
    readonly property string tier: {
        if (_preview) return _envVariant === "trial" ? "trial"
                            : _envVariant === "free"  ? "free" : "pro"
        if (typeof App === "undefined" || !App.accountLoggedIn) return "free"
        return App.deviceMeshTier === "pro" ? "pro" : "free"
    }
    readonly property bool hasProEntitlement: tier !== "free"

    // Current device's OS, so "this device" reflects the running platform.
    readonly property string platform: Qt.platform.os === "osx" ? "mac"
                                     : Qt.platform.os === "windows" ? "win"
                                     : "linux"

    // Uniform model handles: real C++ model when signed in (both surfaces read
    // the same account list), mock ListModels in preview / signed-out.
    readonly property var model: (!_preview && _real) ? App.myDevices : mockList
    readonly property var settingsModel: (!_preview && _real) ? App.myDevices : mockSettings

    // --- Mock data (preview / signed-out) --------------------------------
    ListModel { id: mockList }
    ListModel { id: mockSettings }

    Component.onCompleted: _rebuildMock()

    function _thisName() {
        return platform === "mac" ? "maxim-mbp"
             : platform === "linux" ? "maxim-thinkpad"
             : "maxim-desktop"
    }

    // Seed the mock lists.  Only meaningful in preview / signed-out; the "empty"
    // preview variant intentionally leaves them cleared.
    function _rebuildMock() {
        mockList.clear()
        mockSettings.clear()
        if (_preview && _envVariant === "empty")
            return

        // pubkey is empty in the mock so a preview Connect tap falls through
        // to the plain connectToPeer path (no real key to pre-pin).
        var main = [
            { devId: "this", devName: _thisName(), os: platform, online: true,  current: true,  warned: false, seen: "now",    peerCode: "swift-tiger-4271", pubkey: "" },
            { devId: "d2",   devName: "Office desktop", os: "win",   online: true,  current: false, warned: false, seen: "now",    peerCode: "brave-otter-8823", pubkey: "" },
            { devId: "d3",   devName: "Render box",     os: "linux", online: false, current: false, warned: true,  seen: "3h ago", peerCode: "calm-eagle-5190", pubkey: "" }
        ]
        for (var i = 0; i < main.length; ++i)
            mockList.append(main[i])

        var tbl = [
            { devId: "this", devName: _thisName(),        os: platform, online: true,  current: true,  warned: false, seen: "now",       host: "swift-tiger-4271 · this device" },
            { devId: "d2",   devName: "Office desktop",   os: "win",    online: true,  current: false, warned: false, seen: "now",       host: "brave-otter-8823" },
            { devId: "d3",   devName: "Render box",       os: "linux",  online: false, current: false, warned: true,  seen: "3h ago",    host: "calm-eagle-5190" },
            { devId: "d4",   devName: "John's MacBook Pro", os: "mac",  online: true,  current: false, warned: false, seen: "now",       host: "gentle-lynx-3372" },
            { devId: "d5",   devName: "Backup NAS",       os: "linux",  online: true,  current: false, warned: false, seen: "now",       host: "quiet-heron-6690" },
            { devId: "d6",   devName: "Gaming rig",       os: "win",    online: false, current: false, warned: false, seen: "Yesterday", host: "bold-marten-1147" },
            { devId: "d7",   devName: "Studio iMac",      os: "mac",    online: false, current: false, warned: false, seen: "2d ago",    host: "still-osprey-9021" }
        ]
        for (var j = 0; j < tbl.length; ++j)
            mockSettings.append(tbl[j])
    }

    // rename / remove.  Real path calls the backend (App.myDevices re-fetches
    // via SSE); preview path mutates the mock lists locally so it feels live.
    function renameMain(devId, newName) {
        if (!_preview && _real) { App.myDevices.rename(devId, newName); return }
        _rename(mockList, devId, newName); _rename(mockSettings, devId, newName)
    }
    function removeMain(devId) {
        if (!_preview && _real) { App.myDevices.remove(devId); return }
        _remove(mockList, devId); _remove(mockSettings, devId)
    }

    function _rename(m, devId, newName) {
        for (var i = 0; i < m.count; ++i)
            if (m.get(i).devId === devId) { m.setProperty(i, "devName", newName); return }
    }
    function _remove(m, devId) {
        for (var i = 0; i < m.count; ++i)
            if (m.get(i).devId === devId) { m.remove(i); return }
    }
}
