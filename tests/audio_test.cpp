// Audio roundtrip test: sine wave -> resample to transport rate -> Opus encode
// -> Opus decode -> resample to device rate -> compare energy.
// Not bit-exact (lossy codec), but checks the pipeline runs and preserves tone.

#include "common/audio/audio_codec.h"
#include "common/audio/jitter_buffer.h"
#include "common/audio/resampler.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace vivora::audio;

static void make_sine(std::vector<float>& buf, int rate, int channels,
                      int ms, float freq) {
    int frames = rate * ms / 1000;
    buf.assign(frames * channels, 0.0f);
    for (int i = 0; i < frames; ++i) {
        float s = 0.3f * std::sin(2.0f * 3.14159265f * freq * i / rate);
        for (int c = 0; c < channels; ++c) {
            buf[i * channels + c] = s;
        }
    }
}

static float rms(const float* data, size_t n) {
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) acc += double(data[i]) * data[i];
    return static_cast<float>(std::sqrt(acc / n));
}

static bool test_codec_roundtrip_48k() {
    OpusAudioEncoder enc;
    OpusAudioDecoder dec;
    if (!enc.init(128000)) return false;
    if (!dec.init())       return false;

    std::vector<float> sine;
    make_sine(sine, TRANSPORT_SAMPLE_RATE, TRANSPORT_CHANNELS, 100, 440.0f);

    std::vector<uint8_t> pkt;
    std::vector<float> recovered(FRAME_SAMPLES_STEREO);

    int frames_per_packet = FRAME_SAMPLES;
    int total_frames = static_cast<int>(sine.size()) / TRANSPORT_CHANNELS;
    int packets = 0;
    double sum_rms_in = 0, sum_rms_out = 0;

    for (int off = 0; off + frames_per_packet <= total_frames;
         off += frames_per_packet) {
        const float* in = sine.data() + off * TRANSPORT_CHANNELS;
        int n = enc.encode(in, pkt);
        if (n <= 0) { std::fprintf(stderr, "encode failed\n"); return false; }
        int decoded = dec.decode(pkt.data(), n, recovered.data(), false);
        if (decoded != FRAME_SAMPLES) {
            std::fprintf(stderr, "decoded %d samples, expected %d\n",
                         decoded, FRAME_SAMPLES);
            return false;
        }
        sum_rms_in  += rms(in, FRAME_SAMPLES_STEREO);
        sum_rms_out += rms(recovered.data(), FRAME_SAMPLES_STEREO);
        packets++;
    }
    double ratio = sum_rms_out / sum_rms_in;
    std::printf("codec_roundtrip_48k: packets=%d  rms_in=%.3f  rms_out=%.3f  ratio=%.3f\n",
                packets, sum_rms_in / packets, sum_rms_out / packets, ratio);
    if (ratio < 0.7 || ratio > 1.3) {
        std::fprintf(stderr, "ratio out of tolerance\n");
        return false;
    }
    return true;
}

static bool test_resampler_44k_to_48k() {
    Resampler up;
    if (!up.init(2, 44100, 48000)) return false;
    if (up.is_passthrough()) { std::fprintf(stderr, "unexpected passthrough\n"); return false; }

    std::vector<float> in;
    make_sine(in, 44100, 2, 100, 440.0f);

    std::vector<float> out(48000 * 2 / 10 + 256, 0.0f); // 100ms + slack

    unsigned int in_frames  = static_cast<unsigned int>(in.size() / 2);
    unsigned int out_frames = static_cast<unsigned int>(out.size() / 2);
    if (!up.process(in.data(), in_frames, out.data(), out_frames)) {
        std::fprintf(stderr, "resample failed\n"); return false;
    }
    float rin  = rms(in.data(),  in.size());
    float rout = rms(out.data(), out_frames * 2);
    std::printf("resample_44k->48k: in_frames=%u out_frames=%u  rms_in=%.3f  rms_out=%.3f\n",
                in_frames, out_frames, rin, rout);
    // Expected output frames ≈ 48000/44100 * in_frames ≈ 1.0884 * in_frames.
    if (out_frames < static_cast<unsigned>(in_frames * 1.05) ||
        out_frames > static_cast<unsigned>(in_frames * 1.12)) {
        std::fprintf(stderr, "output frame count out of range\n"); return false;
    }
    if (std::abs(rout - rin) / rin > 0.2f) {
        std::fprintf(stderr, "energy mismatch\n"); return false;
    }
    return true;
}

static bool test_resampler_passthrough() {
    Resampler r;
    if (!r.init(2, 48000, 48000)) return false;
    if (!r.is_passthrough()) { std::fprintf(stderr, "expected passthrough\n"); return false; }
    std::vector<float> in(960, 0.5f), out(960, 0.0f);
    unsigned int i = 480, o = 480;
    if (!r.process(in.data(), i, out.data(), o)) return false;
    if (i != 480 || o != 480 || out[0] != 0.5f) return false;
    std::printf("resample_passthrough: ok\n");
    return true;
}

static bool test_full_pipeline() {
    // device 44100 stereo -> 48000 -> encode -> decode -> 48000 -> 44100
    Resampler up, down;
    if (!up.init(2, 44100, 48000)) return false;
    if (!down.init(2, 48000, 44100)) return false;
    OpusAudioEncoder enc;
    OpusAudioDecoder dec;
    if (!enc.init(128000)) return false;
    if (!dec.init())       return false;

    // 441 samples @ 44100 = 10ms -> 480 @ 48000
    std::vector<float> in;
    make_sine(in, 44100, 2, 200, 440.0f);

    std::vector<float> up_buf(1024 * 2);
    std::vector<float> recovered(FRAME_SAMPLES_STEREO);
    std::vector<float> down_buf(1024 * 2);
    std::vector<uint8_t> pkt;

    int packets = 0;
    double sum_rms_in = 0, sum_rms_out = 0;
    const unsigned int DEV_CHUNK = 441; // 10ms @ 44100

    for (size_t off = 0; off + DEV_CHUNK * 2 <= in.size(); off += DEV_CHUNK * 2) {
        unsigned int in_f = DEV_CHUNK;
        unsigned int out_f = static_cast<unsigned int>(up_buf.size() / 2);
        up.process(in.data() + off, in_f, up_buf.data(), out_f);
        if (out_f != FRAME_SAMPLES) {
            // first frame may be slightly off due to filter warmup; skip
            continue;
        }
        int n = enc.encode(up_buf.data(), pkt);
        if (n <= 0) return false;
        if (dec.decode(pkt.data(), n, recovered.data(), false) != FRAME_SAMPLES)
            return false;

        unsigned int dec_in = FRAME_SAMPLES;
        unsigned int dec_out = static_cast<unsigned int>(down_buf.size() / 2);
        down.process(recovered.data(), dec_in, down_buf.data(), dec_out);

        sum_rms_in  += rms(in.data() + off, DEV_CHUNK * 2);
        sum_rms_out += rms(down_buf.data(), dec_out * 2);
        packets++;
    }
    double ratio = (packets > 0) ? (sum_rms_out / sum_rms_in) : 0.0;
    std::printf("full_pipeline 44k<->48k: packets=%d ratio=%.3f\n", packets, ratio);
    return packets > 5 && ratio > 0.6 && ratio < 1.4;
}

static bool test_jitter_buffer_inorder() {
    JitterBuffer jb;
    if (!jb.init(10, 30, 200)) return false;
    uint8_t data[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    std::vector<uint8_t> out; uint16_t seq;

    // Before prebuffer hits target — Empty.
    jb.push(100, data, 4);
    if (jb.pop(out, seq) != JitterBuffer::Status::Empty) return false;
    jb.push(101, data, 4);
    if (jb.pop(out, seq) != JitterBuffer::Status::Empty) return false;
    jb.push(102, data, 4);
    // Now 3 buffered -> play starts.
    if (jb.pop(out, seq) != JitterBuffer::Status::Data) return false;
    if (seq != 100) return false;
    if (jb.pop(out, seq) != JitterBuffer::Status::Data || seq != 101) return false;
    if (jb.pop(out, seq) != JitterBuffer::Status::Data || seq != 102) return false;
    if (jb.pop(out, seq) != JitterBuffer::Status::Missing) return false; // 103 not yet
    std::printf("jitter_inorder: ok\n");
    return true;
}

static bool test_jitter_buffer_reorder_and_gap() {
    JitterBuffer jb;
    jb.init(10, 30, 200);
    uint8_t d[2] = {1, 2};
    std::vector<uint8_t> out; uint16_t seq;

    jb.push(10, d, 2);
    jb.push(12, d, 2);           // out of order
    jb.push(11, d, 2);           // fills gap
    // started
    if (jb.pop(out, seq) != JitterBuffer::Status::Data || seq != 10) return false;
    if (jb.pop(out, seq) != JitterBuffer::Status::Data || seq != 11) return false;
    if (jb.pop(out, seq) != JitterBuffer::Status::Data || seq != 12) return false;
    // seq 13 never arrived -> Missing.
    if (jb.pop(out, seq) != JitterBuffer::Status::Missing) return false;
    // seq 14 arrives but 13 already skipped.
    jb.push(14, d, 2);
    // pop 14 requires buffer to still consider stream started; stored==1, but
    // target_frames==3 → starts only once. started_ stays true.
    if (jb.pop(out, seq) != JitterBuffer::Status::Data || seq != 14) return false;
    std::printf("jitter_reorder_gap: ok\n");
    return true;
}

static bool test_jitter_buffer_drop_stale() {
    JitterBuffer jb;
    jb.init(10, 20, 200);
    uint8_t d[2] = {1, 2};
    std::vector<uint8_t> out; uint16_t seq;
    jb.push(50, d, 2);
    jb.push(51, d, 2);           // starts here (target=2 frames)
    if (jb.pop(out, seq) != JitterBuffer::Status::Data || seq != 50) return false;
    if (jb.pop(out, seq) != JitterBuffer::Status::Data || seq != 51) return false;
    // Late duplicate of 50 — must be dropped.
    jb.push(50, d, 2);
    if (jb.pop(out, seq) != JitterBuffer::Status::Missing) return false;
    std::printf("jitter_drop_stale: ok\n");
    return true;
}

int main() {
    int ok = 0, fail = 0;
    auto run = [&](const char* name, bool (*fn)()) {
        std::printf("== %s ==\n", name);
        if (fn()) { ok++; std::printf("  PASS\n"); }
        else      { fail++; std::printf("  FAIL\n"); }
    };
    run("codec_roundtrip_48k",       test_codec_roundtrip_48k);
    run("resampler_44k_to_48k",      test_resampler_44k_to_48k);
    run("resampler_passthrough",     test_resampler_passthrough);
    run("full_pipeline",             test_full_pipeline);
    run("jitter_buffer_inorder",     test_jitter_buffer_inorder);
    run("jitter_buffer_reorder_gap", test_jitter_buffer_reorder_and_gap);
    run("jitter_buffer_drop_stale",  test_jitter_buffer_drop_stale);
    std::printf("\n%d passed, %d failed\n", ok, fail);
    return fail == 0 ? 0 : 1;
}
