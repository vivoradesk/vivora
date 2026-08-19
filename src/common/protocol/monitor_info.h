#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vivora::protocol {

// Monitor selection (VIV-50).  The host advertises the displays it can
// capture; the client picks one from the in-stream "Remote monitors" panel.
//
// Three control messages travel over the same Noise-sealed UDP transport as
// every other non-handshake packet:
//   * MonitorListRequest (client -> host) — empty payload, "tell me your
//     displays".  Sent when the user opens the panel and on Refresh.
//   * MonitorList        (host -> client) — the enumerated displays below.
//   * SelectMonitor      (client -> host) — 1-byte display index to switch to.
//
// Indices are the host's capture-layer monitor index (DXGI EnumOutputs order
// / SCShareableContent.displays order).  They are stable for the life of a
// session barring a hot-plug; a fresh MonitorList after a configuration change
// re-syncs the client.

// One display entry as carried on the wire.
struct MonitorDesc {
    uint8_t  index   = 0;       // host capture index (argument to SelectMonitor)
    uint16_t width   = 0;       // native resolution, px
    uint16_t height  = 0;
    bool     primary = false;   // host's primary display
    bool     viewing = false;   // the display currently being captured/streamed
    // Whether SelectMonitor on this host will actually do anything.  Hosts
    // that enumerate displays but cannot switch between them (Linux today)
    // clear it, so the client shows the list without a control that silently
    // does nothing.  Older clients mask this bit off and behave as before.
    bool     switchable = true;
};

// Host -> client: the full set of capturable displays.
//
// Wire format:
//   count(1B) | repeated count times:
//     index(1B) | width(2B LE) | height(2B LE) | flags(1B)
//   flags: bit0 = primary, bit1 = viewing, bit2 = switchable
// = 1 + count * 6 bytes
struct MonitorListMessage {
    std::vector<MonitorDesc> monitors;

    static constexpr size_t REC_SIZE = 6;
    static constexpr size_t MAX_MONITORS = 16;  // wire cap; plenty for any host

    std::vector<uint8_t> serialize() const {
        size_t n = monitors.size();
        if (n > MAX_MONITORS) n = MAX_MONITORS;
        std::vector<uint8_t> buf;
        buf.reserve(1 + n * REC_SIZE);
        buf.push_back(static_cast<uint8_t>(n));
        for (size_t i = 0; i < n; ++i) {
            const MonitorDesc& m = monitors[i];
            buf.push_back(m.index);
            buf.push_back(static_cast<uint8_t>(m.width  & 0xFF));
            buf.push_back(static_cast<uint8_t>((m.width  >> 8) & 0xFF));
            buf.push_back(static_cast<uint8_t>(m.height & 0xFF));
            buf.push_back(static_cast<uint8_t>((m.height >> 8) & 0xFF));
            uint8_t flags = 0;
            if (m.primary)    flags |= 0x01;
            if (m.viewing)    flags |= 0x02;
            if (m.switchable) flags |= 0x04;
            buf.push_back(flags);
        }
        return buf;
    }

    static bool deserialize(const uint8_t* data, size_t len, MonitorListMessage& out) {
        out.monitors.clear();
        if (len < 1) return false;
        size_t n = data[0];
        if (len < 1 + n * REC_SIZE) return false;
        out.monitors.reserve(n);
        const uint8_t* p = data + 1;
        for (size_t i = 0; i < n; ++i) {
            MonitorDesc m;
            m.index   = p[0];
            m.width   = static_cast<uint16_t>(p[1] | (p[2] << 8));
            m.height  = static_cast<uint16_t>(p[3] | (p[4] << 8));
            m.primary = (p[5] & 0x01) != 0;
            m.viewing = (p[5] & 0x02) != 0;
            m.switchable = (p[5] & 0x04) != 0;
            out.monitors.push_back(m);
            p += REC_SIZE;
        }
        return true;
    }
};

// Client -> host: switch capture to this display index.
//
// Wire format: index(1B)
struct SelectMonitorMessage {
    uint8_t index = 0;

    static constexpr size_t WIRE_SIZE = 1;

    std::vector<uint8_t> serialize() const {
        return std::vector<uint8_t>{index};
    }

    static bool deserialize(const uint8_t* data, size_t len, SelectMonitorMessage& out) {
        if (len < WIRE_SIZE) return false;
        out.index = data[0];
        return true;
    }
};

} // namespace vivora::protocol
