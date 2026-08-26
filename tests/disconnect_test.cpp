// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// disconnect_test.cpp — unit test for the Disconnect control packet (VIV-52).
// Verifies the single reason-byte payload round-trips through Packet
// serialize/deserialize and that the reason enum maps to the expected wire
// values, including forward-compat handling of an unknown reason.

#include "common/protocol/packet.h"
#include <cstdio>
#include "check.h"

using namespace vivora::protocol;

// Build a Disconnect packet exactly as the host does, round-trip it, and
// confirm the reason byte survives.
static void test_reason_roundtrip(DisconnectReason reason) {
    Packet pkt;
    pkt.header.type        = PacketType::Disconnect;
    pkt.header.seq_no      = 0;
    pkt.header.timestamp   = 0;
    pkt.header.flags       = 0;
    pkt.payload.resize(1);
    pkt.payload[0]         = static_cast<uint8_t>(reason);
    pkt.header.payload_len = 1;

    auto wire = pkt.serialize();
    CHECK(wire.size() == PacketHeader::WIRE_SIZE + 1);

    Packet back = Packet::deserialize(wire.data(), wire.size());
    CHECK(back.header.type == PacketType::Disconnect);
    CHECK(back.header.payload_len == 1);
    CHECK(back.payload.size() == 1);
    CHECK(static_cast<DisconnectReason>(back.payload[0]) == reason);
}

int main() {
    printf("disconnect_test\n");

    // Stable wire values — clients and hosts on different builds must agree.
    printf("  reason enum wire values...\n");
    CHECK(static_cast<uint8_t>(DisconnectReason::Rejected)     == 1);
    CHECK(static_cast<uint8_t>(DisconnectReason::Kicked)       == 2);
    CHECK(static_cast<uint8_t>(DisconnectReason::HostShutdown) == 3);
    CHECK(static_cast<uint8_t>(PacketType::Disconnect)         == 0x1B);

    printf("  reason round-trip...\n");
    test_reason_roundtrip(DisconnectReason::Rejected);
    test_reason_roundtrip(DisconnectReason::Kicked);
    test_reason_roundtrip(DisconnectReason::HostShutdown);

    // Forward-compat: an unknown reason byte still parses as a valid Disconnect
    // packet; the client treats it as a plain terminal disconnect (no
    // reconnect).  Here we only assert the byte survives the wire unchanged.
    printf("  unknown reason forward-compat...\n");
    {
        Packet pkt;
        pkt.header.type        = PacketType::Disconnect;
        pkt.payload            = {0x7F};
        pkt.header.payload_len = 1;
        auto wire = pkt.serialize();
        Packet back = Packet::deserialize(wire.data(), wire.size());
        CHECK(back.header.type == PacketType::Disconnect);
        CHECK(back.payload.size() == 1 && back.payload[0] == 0x7F);
    }

    return check_report("PASS");
}
