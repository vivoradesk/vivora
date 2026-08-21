// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "common/protocol/clipboard_message.h"

#include <mutex>
#include <optional>

namespace vivora {

// Thread-safe handoff of clipboard updates between the GUI thread (where
// QClipboard lives — see gui/clipboard_sync.*) and the session loop that
// owns the network transport (VIV-22).
//
//   outbound: GUI → loop.   ClipboardSync pushes on QClipboard::dataChanged;
//             host_loop / view_loop drains it each tick and broadcasts.
//   inbound:  loop → GUI.   The session's reassembler completes a remote
//             clipboard; the loop pushes it here and ClipboardSync's poll
//             timer picks it up and writes the local clipboard.
//
// Single-slot, latest-wins semantics on both directions: the clipboard is
// state, not a stream — if the user copies twice between ticks only the
// newest content matters.  Mirrors the HostApprovalGate pattern: plain
// std mutex, no Qt, shared_ptr-owned by the GUI side and handed into the
// loop config so worker threads never touch Qt machinery.
class ClipboardBridge {
public:
    void push_outbound(protocol::ClipboardMessage msg) {
        std::lock_guard<std::mutex> lock(mu_);
        outbound_ = std::move(msg);
    }
    bool take_outbound(protocol::ClipboardMessage& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!outbound_) return false;
        out = std::move(*outbound_);
        outbound_.reset();
        return true;
    }

    void push_inbound(protocol::ClipboardMessage msg) {
        std::lock_guard<std::mutex> lock(mu_);
        inbound_ = std::move(msg);
    }
    bool take_inbound(protocol::ClipboardMessage& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!inbound_) return false;
        out = std::move(*inbound_);
        inbound_.reset();
        return true;
    }

private:
    std::mutex mu_;
    std::optional<protocol::ClipboardMessage> outbound_;
    std::optional<protocol::ClipboardMessage> inbound_;
};

} // namespace vivora
