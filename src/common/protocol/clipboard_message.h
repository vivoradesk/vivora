#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vivora::protocol {

// Clipboard sync (VIV-22).  Bidirectional host <-> viewer clipboard over the
// same Noise-sealed UDP transport as every other non-handshake packet.
//
// One logical clipboard update is a ClipboardMessage{mime, data}.  Text is
// always normalised to UTF-8 by the GUI layer before it reaches the wire
// (QClipboard abstracts the CF_UNICODETEXT / NSPasteboard / X11-selection
// zoo), so `mime` is "text/plain;charset=utf-8" today.  The mime field is
// carried on the wire so images/files can reuse the same envelope later.
//
// The serialized body can exceed the MTU (cap: 256 KiB of payload), so —
// exactly like CursorShape — it is fragmented at the application layer:
//
//   per-fragment payload:
//     clip_id(4B LE) | frag_idx(1B) | frag_count(1B) | chunk bytes...
//
// clip_id is a per-sender monotonically increasing id: the receiver keys
// reassembly on it and dedups repeat deliveries (the sender re-broadcasts
// once after ~150ms for UDP-loss resilience).
//
// Body wire format (concatenation of all chunks):
//   mime_len(1B) | mime bytes | data_len(4B LE) | data bytes
struct ClipboardMessage {
    std::string mime;             // e.g. "text/plain;charset=utf-8"
    std::vector<uint8_t> data;    // UTF-8 text bytes (no NUL terminator)

    static constexpr size_t MAX_PAYLOAD = 256 * 1024;  // refuse bigger sends
    static constexpr size_t MAX_MIME    = 255;         // 1-byte length prefix

    // MIME used for plain text (the only kind VIV-22 ships).
    static const char* text_mime() { return "text/plain;charset=utf-8"; }

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t len, ClipboardMessage& out);
};

// Application-layer fragmentation shared by host and client send paths.

// Fragment chunk size — safely under typical MTU after the 10B packet
// header + 6B fragment header + AEAD tag.
constexpr size_t CLIPBOARD_CHUNK_SIZE  = 1200;
constexpr size_t CLIPBOARD_FRAG_HEADER = 6;   // clip_id(4) + idx(1) + count(1)

// Split a serialized ClipboardMessage into fragment payloads ready to be
// carried as PacketType::Clipboard payloads.  Returns an empty vector when
// the message violates the wire caps (payload too big, mime too long, or
// more than 255 fragments needed).
std::vector<std::vector<uint8_t>> fragment_clipboard(const ClipboardMessage& msg,
                                                     uint32_t clip_id);

// Receiver-side reassembler.  Feed every PacketType::Clipboard payload from
// one sender into feed(); it returns true exactly once per completed clip_id
// and fills `out`.  Duplicate fragments and repeat deliveries of an
// already-completed clip are silently dropped, so the sender's loss-resilience
// re-broadcast is idempotent.  One instance per remote peer.
class ClipboardReassembler {
public:
    bool feed(const uint8_t* payload, size_t len, ClipboardMessage& out);

private:
    struct Entry {
        std::vector<std::vector<uint8_t>> fragments;
        size_t received = 0;
    };
    std::unordered_map<uint32_t, Entry> pending_;
    uint32_t last_completed_id_ = 0;
    bool     has_completed_     = false;
};

} // namespace vivora::protocol
