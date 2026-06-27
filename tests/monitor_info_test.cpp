// monitor_info_test.cpp — unit test for the VIV-50 monitor-selection wire
// messages (MonitorListMessage / SelectMonitorMessage): round-trip,
// flag packing, truncation rejection, and the empty list.

#include "common/protocol/monitor_info.h"
#include <cassert>
#include <cstdio>

using namespace vivora::protocol;

static void test_list_roundtrip() {
    printf("  monitor list round-trip...\n");
    MonitorListMessage msg;
    msg.monitors.push_back({0, 3024, 1964, /*primary=*/true,  /*viewing=*/false});
    msg.monitors.push_back({1, 2560, 1440, /*primary=*/false, /*viewing=*/true});

    auto wire = msg.serialize();
    assert(wire.size() == 1 + 2 * MonitorListMessage::REC_SIZE);
    assert(wire[0] == 2);

    MonitorListMessage out;
    assert(MonitorListMessage::deserialize(wire.data(), wire.size(), out));
    assert(out.monitors.size() == 2);

    assert(out.monitors[0].index == 0);
    assert(out.monitors[0].width == 3024);
    assert(out.monitors[0].height == 1964);
    assert(out.monitors[0].primary == true);
    assert(out.monitors[0].viewing == false);

    assert(out.monitors[1].index == 1);
    assert(out.monitors[1].width == 2560);
    assert(out.monitors[1].height == 1440);
    assert(out.monitors[1].primary == false);
    assert(out.monitors[1].viewing == true);
}

static void test_empty_list() {
    printf("  empty list...\n");
    MonitorListMessage msg;
    auto wire = msg.serialize();
    assert(wire.size() == 1);
    assert(wire[0] == 0);

    MonitorListMessage out;
    assert(MonitorListMessage::deserialize(wire.data(), wire.size(), out));
    assert(out.monitors.empty());
}

static void test_truncated_list() {
    printf("  truncated list rejected...\n");
    MonitorListMessage msg;
    msg.monitors.push_back({0, 1920, 1080, true, true});
    auto wire = msg.serialize();
    // Claims one record but cut a byte short → deserialize must fail.
    MonitorListMessage out;
    assert(!MonitorListMessage::deserialize(wire.data(), wire.size() - 1, out));
    // Zero-length buffer → fail (no count byte).
    assert(!MonitorListMessage::deserialize(wire.data(), 0, out));
}

static void test_select_roundtrip() {
    printf("  select monitor round-trip...\n");
    SelectMonitorMessage msg;
    msg.index = 3;
    auto wire = msg.serialize();
    assert(wire.size() == SelectMonitorMessage::WIRE_SIZE);

    SelectMonitorMessage out;
    assert(SelectMonitorMessage::deserialize(wire.data(), wire.size(), out));
    assert(out.index == 3);

    assert(!SelectMonitorMessage::deserialize(wire.data(), 0, out));
}

int main() {
    printf("monitor_info_test:\n");
    test_list_roundtrip();
    test_empty_list();
    test_truncated_list();
    test_select_roundtrip();
    printf("All monitor_info tests passed.\n");
    return 0;
}
