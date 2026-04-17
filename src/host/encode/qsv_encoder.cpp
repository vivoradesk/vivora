#ifdef DESKBEAM_WINDOWS

#include "host/encode/qsv_encoder.h"
#include "common/utils/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// TODO: full oneVPL implementation lands in the next commit.  For now
// this stub returns false from init() so the factory can still compile
// and --encoder=qsv reports a clean error.

namespace deskbeam {

static constexpr const char* TAG = "QSV";

QsvEncoder::QsvEncoder() = default;

QsvEncoder::~QsvEncoder() {
    if (vpl_dll_) {
        FreeLibrary(reinterpret_cast<HMODULE>(vpl_dll_));
        vpl_dll_ = nullptr;
    }
}

bool QsvEncoder::init(const EncoderConfig& config, ID3D11Device* device) {
    config_ = config;
    device_ = device;
    log::error(TAG, "QSV encoder implementation not yet available");
    return false;
}

bool QsvEncoder::encode(ID3D11Texture2D*, uint64_t) { return false; }
bool QsvEncoder::get_packet(EncodedPacket&) { return false; }
void QsvEncoder::request_idr() { idr_requested_ = true; }
void QsvEncoder::set_bitrate(uint32_t bps) { config_.bitrate_bps = bps; }

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
