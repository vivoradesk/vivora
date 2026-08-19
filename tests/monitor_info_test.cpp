// monitor_info_test.cpp — unit test for the VIV-50 monitor-selection wire
// messages (MonitorListMessage / SelectMonitorMessage): round-trip,
// flag packing, truncation rejection, and the empty list.
//
// CHECK() rather than assert(): the shipped configuration defines NDEBUG,
// which compiles assert away and leaves the suite passing vacuously.

#include "common/protocol/monitor_info.h"
#include <cstdio>

using namespace vivora::protocol;

namespace {

int g_tests_run = 0;
int g_tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++g_tests_run;                                                         \
        if (!(expr)) {                                                         \
            std::fprintf(stderr,                                               \
                         "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);       \
            ++g_tests_failed;                                                  \
        }                                                                      \
    } while (0)

void test_list_roundtrip() {
    printf("  monitor list round-trip...\n");
    MonitorListMessage msg;
    msg.monitors.push_back({0, 3024, 1964, /*primary=*/true,  /*viewing=*/false,
                            /*switchable=*/true});
    msg.monitors.push_back({1, 2560, 1440, /*primary=*/false, /*viewing=*/true,
                            /*switchable=*/true});

    auto wire = msg.serialize();
    CHECK(wire.size() == 1 + 2 * MonitorListMessage::REC_SIZE);
    CHECK(wire[0] == 2);

    MonitorListMessage out;
    CHECK(MonitorListMessage::deserialize(wire.data(), wire.size(), out));
    CHECK(out.monitors.size() == 2);

    CHECK(out.monitors[0].index == 0);
    CHECK(out.monitors[0].width == 3024);
    CHECK(out.monitors[0].height == 1964);
    CHECK(out.monitors[0].primary == true);
    CHECK(out.monitors[0].viewing == false);
    CHECK(out.monitors[0].switchable == true);

    CHECK(out.monitors[1].index == 1);
    CHECK(out.monitors[1].width == 2560);
    CHECK(out.monitors[1].height == 1440);
    CHECK(out.monitors[1].primary == false);
    CHECK(out.monitors[1].viewing == true);
    CHECK(out.monitors[1].switchable == true);
}

// A host that enumerates displays but cannot retarget capture (Linux today)
// clears the switchable bit; the viewer uses it to stop offering a control
// that would silently do nothing.
void test_switchable_flag() {
    printf("  switchable flag...\n");
    MonitorListMessage msg;
    msg.monitors.push_back({0, 1920, 1080, /*primary=*/true,  /*viewing=*/true,
                            /*switchable=*/false});
    msg.monitors.push_back({1, 1920, 1080, /*primary=*/false, /*viewing=*/false,
                            /*switchable=*/false});

    auto wire = msg.serialize();
    // primary|viewing but not switchable on the first record.
    CHECK((wire[1 + 5] & 0x04) == 0);

    MonitorListMessage out;
    CHECK(MonitorListMessage::deserialize(wire.data(), wire.size(), out));
    CHECK(out.monitors.size() == 2);
    CHECK(out.monitors[0].switchable == false);
    CHECK(out.monitors[1].switchable == false);
    // The other flags must survive alongside it.
    CHECK(out.monitors[0].primary == true);
    CHECK(out.monitors[0].viewing == true);
    CHECK(out.monitors[1].primary == false);
    CHECK(out.monitors[1].viewing == false);
}

// The three flags are independent bits in one byte; make sure none of them
// bleeds into another.
void test_flag_independence() {
    printf("  flag independence...\n");
    for (int bits = 0; bits < 8; ++bits) {
        MonitorListMessage msg;
        MonitorDesc d;
        d.index      = 7;
        d.width      = 800;
        d.height     = 600;
        d.primary    = (bits & 1) != 0;
        d.viewing    = (bits & 2) != 0;
        d.switchable = (bits & 4) != 0;
        msg.monitors.push_back(d);

        auto wire = msg.serialize();
        MonitorListMessage out;
        CHECK(MonitorListMessage::deserialize(wire.data(), wire.size(), out));
        CHECK(out.monitors.size() == 1);
        CHECK(out.monitors[0].primary    == d.primary);
        CHECK(out.monitors[0].viewing    == d.viewing);
        CHECK(out.monitors[0].switchable == d.switchable);
    }
}

void test_empty_list() {
    printf("  empty list...\n");
    MonitorListMessage msg;
    auto wire = msg.serialize();
    CHECK(wire.size() == 1);
    CHECK(wire[0] == 0);

    MonitorListMessage out;
    CHECK(MonitorListMessage::deserialize(wire.data(), wire.size(), out));
    CHECK(out.monitors.empty());
}

void test_truncated_list() {
    printf("  truncated list rejected...\n");
    MonitorListMessage msg;
    msg.monitors.push_back({0, 1920, 1080, true, true, true});
    auto wire = msg.serialize();
    // Claims one record but cut a byte short → deserialize must fail.
    MonitorListMessage out;
    CHECK(!MonitorListMessage::deserialize(wire.data(), wire.size() - 1, out));
    // Zero-length buffer → fail (no count byte).
    CHECK(!MonitorListMessage::deserialize(wire.data(), 0, out));
}

void test_select_roundtrip() {
    printf("  select monitor round-trip...\n");
    SelectMonitorMessage msg;
    msg.index = 3;
    auto wire = msg.serialize();
    CHECK(wire.size() == SelectMonitorMessage::WIRE_SIZE);

    SelectMonitorMessage out;
    CHECK(SelectMonitorMessage::deserialize(wire.data(), wire.size(), out));
    CHECK(out.index == 3);

    CHECK(!SelectMonitorMessage::deserialize(wire.data(), 0, out));
}

} // namespace

int main() {
    printf("monitor_info_test:\n");
    test_list_roundtrip();
    test_switchable_flag();
    test_flag_independence();
    test_empty_list();
    test_truncated_list();
    test_select_roundtrip();
    printf("monitor_info_test: %d checks, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
