#ifdef DESKBEAM_WINDOWS

#include "host/encode/fallback_encoder.h"
#include "common/utils/log.h"

namespace deskbeam {

static const char* TAG = "ENCODE";

FallbackEncoder::FallbackEncoder(std::unique_ptr<IVideoEncoder> primary,
                                 EncoderKind fallback_kind)
    : primary_(std::move(primary)),
      fallback_kind_(fallback_kind) {
    active_ = primary_.get();
}

FallbackEncoder::~FallbackEncoder() = default;

bool FallbackEncoder::init(const EncoderConfig& config, ID3D11Device* device) {
    config_ = config;
    device_ = device;
    if (!primary_ || !primary_->init(config, device)) {
        log::error(TAG, "FallbackEncoder: primary init failed");
        return false;
    }
    return true;
}

bool FallbackEncoder::encode(ID3D11Texture2D* texture, uint64_t pts_us) {
    const bool ok = active_->encode(texture, pts_us);
    if (!ok) {
        ++consecutive_encode_fails_;
        if (!switched_ && consecutive_encode_fails_ >= kFailureThreshold) {
            if (try_switch_to_fallback()) {
                // Feed the same frame into the fresh encoder so we don't lose
                // it — new session needs an IDR anyway.
                return active_->encode(texture, pts_us);
            }
        }
    } else {
        consecutive_encode_fails_ = 0;
    }
    return ok;
}

bool FallbackEncoder::get_packet(EncodedPacket& packet) {
    return active_ ? active_->get_packet(packet) : false;
}

void FallbackEncoder::request_idr() {
    if (active_) active_->request_idr();
}

void FallbackEncoder::set_bitrate(uint32_t bitrate_bps) {
    if (active_) active_->set_bitrate(bitrate_bps);
    config_.bitrate_bps = bitrate_bps;
}

const EncoderConfig& FallbackEncoder::get_config() const {
    return active_ ? active_->get_config() : config_;
}

bool FallbackEncoder::try_switch_to_fallback() {
    log::warn(TAG, "Primary encoder failed %u consecutive frames — switching to fallback",
              consecutive_encode_fails_);

    auto secondary = IVideoEncoder::create(fallback_kind_);
    if (!secondary) {
        log::error(TAG, "Fallback encoder (%d) not available — keeping primary",
                   (int)fallback_kind_);
        // Reset counter so we don't keep thrashing the create() call every
        // 120 frames forever.  Primary may still recover; if it doesn't and
        // we're this far in, the user is already suffering a freeze.
        consecutive_encode_fails_ = 0;
        return false;
    }
    if (!secondary->init(config_, device_)) {
        log::error(TAG, "Fallback encoder init failed — keeping primary");
        consecutive_encode_fails_ = 0;
        return false;
    }
    // First output from the new encoder must be IDR: the decoder has been
    // consuming a P-chain from the primary and will choke on anything else.
    secondary->request_idr();

    // Destroy primary outright (freeing its GPU resources + any NVENC session
    // that was blocking Map).  The old encoder's packet queue goes with it.
    primary_.reset();

    secondary_ = std::move(secondary);
    active_ = secondary_.get();
    switched_ = true;
    log::info(TAG, "FallbackEncoder: now using fallback backend (kind=%d)",
              (int)fallback_kind_);
    return true;
}

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
