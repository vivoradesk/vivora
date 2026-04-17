#include "common/audio/audio_codec.h"
#include "common/utils/log.h"

#include <opus.h>

namespace deskbeam::audio {

// ---------- Encoder ---------------------------------------------------------

OpusAudioEncoder::OpusAudioEncoder() = default;

OpusAudioEncoder::~OpusAudioEncoder() {
    if (enc_) opus_encoder_destroy(enc_);
}

bool OpusAudioEncoder::init(int bitrate_bps) {
    int err = 0;
    enc_ = opus_encoder_create(TRANSPORT_SAMPLE_RATE,
                               TRANSPORT_CHANNELS,
                               OPUS_APPLICATION_RESTRICTED_LOWDELAY,
                               &err);
    if (err != OPUS_OK || !enc_) {
        log::error("AudioEnc", "opus_encoder_create failed: %s", opus_strerror(err));
        enc_ = nullptr;
        return false;
    }
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate_bps));
    opus_encoder_ctl(enc_, OPUS_SET_VBR(1));
    opus_encoder_ctl(enc_, OPUS_SET_VBR_CONSTRAINT(1));
    opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(5));
    opus_encoder_ctl(enc_, OPUS_SET_DTX(0));
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(5)); // balance quality vs CPU
    log::info("AudioEnc", "Opus encoder %d Hz, %d ch, %d bps, FEC on",
              TRANSPORT_SAMPLE_RATE, TRANSPORT_CHANNELS, bitrate_bps);
    return true;
}

void OpusAudioEncoder::set_bitrate(int bitrate_bps) {
    if (enc_) opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate_bps));
}

int OpusAudioEncoder::encode(const float* pcm, std::vector<uint8_t>& out) {
    if (!enc_) return -1;
    out.resize(MAX_PACKET_BYTES);
    int n = opus_encode_float(enc_, pcm, FRAME_SAMPLES,
                              out.data(), static_cast<opus_int32>(out.size()));
    if (n < 0) {
        log::error("AudioEnc", "opus_encode_float: %s", opus_strerror(n));
        out.clear();
        return -1;
    }
    out.resize(n);
    return n;
}

// ---------- Decoder ---------------------------------------------------------

OpusAudioDecoder::OpusAudioDecoder() = default;

OpusAudioDecoder::~OpusAudioDecoder() {
    if (dec_) opus_decoder_destroy(dec_);
}

bool OpusAudioDecoder::init() {
    int err = 0;
    dec_ = opus_decoder_create(TRANSPORT_SAMPLE_RATE, TRANSPORT_CHANNELS, &err);
    if (err != OPUS_OK || !dec_) {
        log::error("AudioDec", "opus_decoder_create failed: %s", opus_strerror(err));
        dec_ = nullptr;
        return false;
    }
    log::info("AudioDec", "Opus decoder %d Hz, %d ch",
              TRANSPORT_SAMPLE_RATE, TRANSPORT_CHANNELS);
    return true;
}

int OpusAudioDecoder::decode(const uint8_t* data, int len, float* pcm_out, bool fec) {
    if (!dec_) return -1;
    int n = opus_decode_float(dec_, data, len, pcm_out, FRAME_SAMPLES, fec ? 1 : 0);
    if (n < 0) {
        log::error("AudioDec", "opus_decode_float: %s", opus_strerror(n));
        return -1;
    }
    return n;
}

} // namespace deskbeam::audio
