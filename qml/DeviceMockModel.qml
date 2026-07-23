import QtQuick

// ===========================================================================
//  ISOLATED MOCK — front-end ahead of backend.
//
//  The personal device-mesh backend (account-linked device sync, presence,
//  remote rename/remove) does NOT exist yet — that is the cloud side of
//  VIV-52.  This component is the ONE place that fabricates device data and
//  the entitlement tier so the "My Devices" UI can be built and previewed now.
//
//  To wire the REAL backend later, this is the only file that changes:
//    1. Replace `listModel` / `settingsModel` with a C++ QAbstractListModel
//       exposed as e.g. App.myDevices (same role names: devId, devName, os,
//       online, current, warned, seen, peerCode, host).
//    2. Drive `variant` from the real entitlement instead of the env var —
//       e.g. App.deviceMeshTier ("pro" | "trial" | "free") plus a "cached"
//       flag while a refresh is in flight and an "empty" case for a lone
//       device.
//  No other My Devices component references anything but this object's
//  `variant`, `tier`, `model`, and `settingsModel`.
// ===========================================================================
Item {
    id: mock

    // variant: "pro" | "trial" | "free" | "empty" | "cached"
    // Read once from VIVORA_DEVICES_VARIANT (surfaced as a context property by
    // gui_main.cpp) so every state is previewable from a live build; defaults
    // to "pro" (the design's default) when unset or unrecognised.
    property string variant: {
        var v = (typeof VivoraDevicesVariant !== "undefined") ? String(VivoraDevicesVariant) : ""
        v = v.toLowerCase()
        return (v === "pro" || v === "trial" || v === "free" || v === "empty" || v === "cached")
               ? v : "pro"
    }

    // Coarse entitlement derived from the variant.  Later: App.deviceMeshTier.
    readonly property string tier: variant === "trial" ? "trial"
                                  : variant === "free"  ? "free"
                                  : "pro"
    // Placeholder for the real Pro entitlement gate (later: App.licensePro or a
    // mesh-specific flag).  When false the surfaces show the ProGate.
    readonly property bool hasProEntitlement: variant !== "free"

    // Current device's OS, so "this device" reflects the running platform.
    readonly property string platform: Qt.platform.os === "osx" ? "mac"
                                     : Qt.platform.os === "windows" ? "win"
                                     : "linux"

    property alias model: listModel
    property alias settingsModel: settingsModel_

    // Main-window list (devices.jsx buildDevices): current + a couple of peers.
    ListModel { id: listModel }
    // Settings table (devices.jsx buildSettingsDevices): richer host column.
    ListModel { id: settingsModel_ }

    Component.onCompleted: _rebuild()
    onVariantChanged: _rebuild()

    function _thisName() {
        return platform === "mac" ? "maxim-mbp"
             : platform === "linux" ? "maxim-thinkpad"
             : "maxim-desktop"
    }

    function _rebuild() {
        listModel.clear()
        settingsModel_.clear()
        if (variant === "empty")
            return

        // --- main-window seed (sorted: current, then online, then offline) ---
        var main = [
            { devId: "this", devName: _thisName(), os: platform, online: true,  current: true,  warned: false, seen: "now",    peerCode: "swift-tiger-4271" },
            { devId: "d2",   devName: "Office desktop", os: "win",   online: true,  current: false, warned: false, seen: "now",    peerCode: "brave-otter-8823" },
            { devId: "d3",   devName: "Render box",     os: "linux", online: false, current: false, warned: true,  seen: "3h ago", peerCode: "calm-eagle-5190" }
        ]
        for (var i = 0; i < main.length; ++i)
            listModel.append(main[i])

        // --- settings-table seed (extra rows + host column) ---
        var tbl = [
            { devId: "this", devName: _thisName(),        os: platform, online: true,  current: true,  warned: false, seen: "now",       host: "10.42.0.3 · this device" },
            { devId: "d2",   devName: "Office desktop",   os: "win",    online: true,  current: false, warned: false, seen: "now",       host: "10.42.0.8 · Berlin" },
            { devId: "d3",   devName: "Render box",       os: "linux",  online: false, current: false, warned: true,  seen: "3h ago",    host: "10.42.0.11 · datacenter" },
            { devId: "d4",   devName: "John's MacBook Pro", os: "mac",  online: true,  current: false, warned: false, seen: "now",       host: "10.42.0.5 · home" },
            { devId: "d5",   devName: "Backup NAS",       os: "linux",  online: true,  current: false, warned: false, seen: "now",       host: "10.42.0.20 · closet" },
            { devId: "d6",   devName: "Gaming rig",       os: "win",    online: false, current: false, warned: false, seen: "Yesterday", host: "10.42.0.14 · home" },
            { devId: "d7",   devName: "Studio iMac",      os: "mac",    online: false, current: false, warned: false, seen: "2d ago",    host: "10.42.0.9 · studio" }
        ]
        for (var j = 0; j < tbl.length; ++j)
            settingsModel_.append(tbl[j])
    }

    // Local (visual-only) mutations so rename / remove feel live in the mock.
    // Once the backend lands these become calls into App.myDevices.
    function renameMain(devId, newName) { _rename(listModel, devId, newName); _rename(settingsModel_, devId, newName) }
    function removeMain(devId)          { _remove(listModel, devId);          _remove(settingsModel_, devId) }

    function _rename(m, devId, newName) {
        for (var i = 0; i < m.count; ++i)
            if (m.get(i).devId === devId) { m.setProperty(i, "devName", newName); return }
    }
    function _remove(m, devId) {
        for (var i = 0; i < m.count; ++i)
            if (m.get(i).devId === devId) { m.remove(i); return }
    }
}
