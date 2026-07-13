#include "common/protocol/clipboard_message.h"

#include <cstring>

namespace vivora::protocol {

std::vector<uint8_t> ClipboardMessage::serialize() const {
    std::vector<uint8_t> buf;
    const size_t mime_len = mime.size() > MAX_MIME ? MAX_MIME : mime.size();
    buf.reserve(1 + mime_len + 4 + data.size());
    buf.push_back(static_cast<uint8_t>(mime_len));
    buf.insert(buf.end(), mime.begin(), mime.begin() + mime_len);
    const uint32_t dlen = static_cast<uint32_t>(data.size());
    buf.push_back(static_cast<uint8_t>(dlen & 0xFF));
    buf.push_back(static_cast<uint8_t>((dlen >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((dlen >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((dlen >> 24) & 0xFF));
    buf.insert(buf.end(), data.begin(), data.end());
    return buf;
}

bool ClipboardMessage::deserialize(const uint8_t* data, size_t len,
                                   ClipboardMessage& out) {
    out.mime.clear();
    out.data.clear();
    if (!data || len < 1) return false;
    const size_t mime_len = data[0];
    if (len < 1 + mime_len + 4) return false;
    const uint8_t* p = data + 1;
    out.mime.assign(reinterpret_cast<const char*>(p), mime_len);
    p += mime_len;
    const uint32_t dlen = static_cast<uint32_t>(p[0])
                        | (static_cast<uint32_t>(p[1]) << 8)
                        | (static_cast<uint32_t>(p[2]) << 16)
                        | (static_cast<uint32_t>(p[3]) << 24);
    p += 4;
    if (dlen > MAX_PAYLOAD) return false;                     // wire cap
    if (len < 1 + mime_len + 4 + static_cast<size_t>(dlen)) return false;
    out.data.assign(p, p + dlen);
    return true;
}

std::vector<std::vector<uint8_t>> fragment_clipboard(const ClipboardMessage& msg,
                                                     uint32_t clip_id) {
    std::vector<std::vector<uint8_t>> frags;
    if (msg.data.size() > ClipboardMessage::MAX_PAYLOAD) return frags;
    if (msg.mime.size() > ClipboardMessage::MAX_MIME) return frags;

    const std::vector<uint8_t> body = msg.serialize();
    size_t count = (body.size() + CLIPBOARD_CHUNK_SIZE - 1) / CLIPBOARD_CHUNK_SIZE;
    if (count == 0) count = 1;   // empty body still ships one fragment
    if (count > 255) return frags;   // can't happen under MAX_PAYLOAD; belt+braces

    frags.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t off = i * CLIPBOARD_CHUNK_SIZE;
        const size_t n   = body.size() > off
            ? (body.size() - off < CLIPBOARD_CHUNK_SIZE ? body.size() - off
                                                        : CLIPBOARD_CHUNK_SIZE)
            : 0;
        std::vector<uint8_t> f(CLIPBOARD_FRAG_HEADER + n);
        f[0] = static_cast<uint8_t>(clip_id & 0xFF);
        f[1] = static_cast<uint8_t>((clip_id >> 8) & 0xFF);
        f[2] = static_cast<uint8_t>((clip_id >> 16) & 0xFF);
        f[3] = static_cast<uint8_t>((clip_id >> 24) & 0xFF);
        f[4] = static_cast<uint8_t>(i);
        f[5] = static_cast<uint8_t>(count);
        if (n > 0) std::memcpy(f.data() + CLIPBOARD_FRAG_HEADER,
                               body.data() + off, n);
        frags.push_back(std::move(f));
    }
    return frags;
}

bool ClipboardReassembler::feed(const uint8_t* payload, size_t len,
                                ClipboardMessage& out) {
    if (!payload || len < CLIPBOARD_FRAG_HEADER) return false;
    const uint32_t clip_id = static_cast<uint32_t>(payload[0])
                           | (static_cast<uint32_t>(payload[1]) << 8)
                           | (static_cast<uint32_t>(payload[2]) << 16)
                           | (static_cast<uint32_t>(payload[3]) << 24);
    const uint8_t frag_idx   = payload[4];
    const uint8_t frag_count = payload[5];
    if (frag_count == 0 || frag_idx >= frag_count) return false;

    // Repeat delivery of an already-completed clip (loss-resilience
    // re-broadcast) — drop before touching the pending map.
    if (has_completed_ && clip_id == last_completed_id_) return false;

    auto& e = pending_[clip_id];
    if (e.fragments.size() != frag_count) {
        e.fragments.assign(frag_count, std::vector<uint8_t>{});
        e.received = 0;
    }
    // Distinguish "never seen" from "seen with an empty chunk" is moot: only
    // the LAST fragment may carry zero bytes, and only for an empty body
    // (frag_count == 1).  Treat a duplicate non-empty fragment as a no-op.
    if (frag_count > 1 && !e.fragments[frag_idx].empty()) return false;
    if (frag_count == 1 && e.received > 0) return false;
    e.fragments[frag_idx].assign(payload + CLIPBOARD_FRAG_HEADER, payload + len);
    e.received++;

    if (e.received < frag_count) return false;

    // All chunks arrived — concatenate and deserialize.
    std::vector<uint8_t> body;
    size_t total = 0;
    for (const auto& f : e.fragments) total += f.size();
    body.reserve(total);
    for (const auto& f : e.fragments) body.insert(body.end(), f.begin(), f.end());
    pending_.erase(clip_id);
    last_completed_id_ = clip_id;
    has_completed_     = true;

    // Also drop any stale partial clips older than the completed one — the
    // clipboard is state, not a stream, so an older half-received clip is
    // worthless once a newer one landed.  (Ids are monotonic per sender.)
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->first < clip_id) it = pending_.erase(it);
        else ++it;
    }

    return ClipboardMessage::deserialize(body.data(), body.size(), out);
}

} // namespace vivora::protocol
