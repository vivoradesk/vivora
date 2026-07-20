#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vivora::host {

enum class ApprovalState : int {
    Pending  = 0,
    Approved = 1,
    Rejected = 2,
};

// Per-connection capabilities granted when the user accepts (VIV-60).
//   input         — inject keyboard/mouse from this viewer (else view-only)
//   clipboard     — sync clipboard with this viewer (enforced, VIV-22)
//   audio         — stream host audio to this viewer (enforced, VIV-65 —
//                   when false the viewer's audio destination is never
//                   registered with AudioSender, so no audio is sent)
//   file_transfer — accept file-transfer offers (honoured once VIV-39 lands)
// input/clipboard/audio are enforced today; file_transfer flag is carried +
// stored so the feature can gate on it when implemented.
struct CapabilityGrant {
    bool input         = true;
    bool clipboard     = true;
    bool audio         = true;
    bool file_transfer = false;
};

// Shared object that gates incoming host connections behind a user
// approval prompt.  Lives on the heap, owned by the GUI side, passed
// into HostLoopConfig so the worker thread can read state + fire
// notifications without owning any Qt machinery itself.
//
// Thread-safe — both worker thread (HostSession poll) and GUI thread
// (AppController approve/reject button handlers) touch it.
class HostApprovalGate {
public:
    // Worker thread fires this when a new client lands in Pending.
    // Marshalled to the GUI thread by the AppController hook below.
    using OnPendingCallback = std::function<void(
        uint64_t key,
        const std::string& peer_code,
        const std::string& pubkey_hex,
        const std::string& ip_port,
        const std::string& device_name)>;

    // Address-key helper.  Stable per (ip, port) tuple.
    static uint64_t make_key(uint32_t ip_be, uint16_t port) {
        return (static_cast<uint64_t>(ip_be) << 16) | port;
    }

    void set_callback(OnPendingCallback cb) {
        std::lock_guard<std::mutex> lock(mu_);
        cb_ = std::move(cb);
    }

    // Called by HostSession when a new client has just finished its
    // handshake but is held back from receiving frames until approval.
    // Records Pending and fires the callback (which must be safe to
    // invoke from the worker thread — typically posts to the GUI loop).
    void notify_pending(uint64_t key,
                        const std::string& peer_code,
                        const std::string& pubkey_hex,
                        const std::string& ip_port,
                        const std::string& device_name) {
        OnPendingCallback cb_copy;
        {
            std::lock_guard<std::mutex> lock(mu_);
            states_[key] = ApprovalState::Pending;
            cb_copy = cb_;
        }
        if (cb_copy) cb_copy(key, peer_code, pubkey_hex, ip_port, device_name);
    }

    // GUI thread sets the resolution after the popup closes.  No-op if
    // the key was already removed (client timed out before user clicked).
    void set_state(uint64_t key, ApprovalState s) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = states_.find(key);
        if (it != states_.end()) it->second = s;
    }

    // HostSession queries this every poll for each pending client.
    ApprovalState get_state(uint64_t key) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = states_.find(key);
        return it == states_.end() ? ApprovalState::Pending : it->second;
    }

    // GUI thread records the capability grant for a key alongside the
    // Approved state (VIV-60).  HostSession reads it once on the
    // Pending→Approved transition.
    void set_grant(uint64_t key, const CapabilityGrant& g) {
        std::lock_guard<std::mutex> lock(mu_);
        grants_[key] = g;
    }
    CapabilityGrant get_grant(uint64_t key) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = grants_.find(key);
        return it == grants_.end() ? CapabilityGrant{} : it->second;
    }

    // Pre-register an Approved entry, bypassing the prompt.  Used by:
    //   - VIVORA_AUTO_ACCEPT=1 dev override
    //   - future auto_accept / prompt_unknown_only settings paths
    void preapprove(uint64_t key) {
        std::lock_guard<std::mutex> lock(mu_);
        states_[key] = ApprovalState::Approved;
    }

    // HostSession calls this when a client is erased (timeout or
    // explicit reject) so we don't leak stale entries.
    void forget(uint64_t key) {
        std::lock_guard<std::mutex> lock(mu_);
        states_.erase(key);
        grants_.erase(key);
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<uint64_t, ApprovalState> states_;
    std::unordered_map<uint64_t, CapabilityGrant> grants_;
    OnPendingCallback cb_;
};

} // namespace vivora::host
