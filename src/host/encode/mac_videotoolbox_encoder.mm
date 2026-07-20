#ifdef VIVORA_MACOS

#include "host/encode/mac_videotoolbox_encoder.h"
#include "common/utils/log.h"

#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreFoundation/CoreFoundation.h>

#include <mutex>
#include <deque>
#include <cstring>

namespace vivora::host {

namespace {
constexpr const char* TAG = "MAC_ENC";

// Append a NAL unit to `out` in Annex-B format (0x00 00 00 01 start code).
static void append_annexb(std::vector<uint8_t>& out, const uint8_t* nal, size_t nal_len) {
    static const uint8_t start_code[4] = {0, 0, 0, 1};
    out.insert(out.end(), start_code, start_code + 4);
    out.insert(out.end(), nal, nal + nal_len);
}

// Extract VPS/SPS/PPS from a HEVC CMFormatDescription and append as Annex-B.
static bool append_param_sets_hevc(CMFormatDescriptionRef fmt, std::vector<uint8_t>& out) {
    size_t count = 0;
    int nal_hdr_len = 0;
    OSStatus st = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
        fmt, 0, nullptr, nullptr, &count, &nal_hdr_len);
    if (st != noErr || count == 0) return false;

    for (size_t i = 0; i < count; ++i) {
        const uint8_t* ps = nullptr;
        size_t ps_len = 0;
        st = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
            fmt, i, &ps, &ps_len, nullptr, nullptr);
        if (st != noErr) return false;
        append_annexb(out, ps, ps_len);
    }
    return true;
}

// Extract SPS/PPS from an H.264 CMFormatDescription and append as Annex-B (VIV-7).
static bool append_param_sets_h264(CMFormatDescriptionRef fmt, std::vector<uint8_t>& out) {
    size_t count = 0;
    int nal_hdr_len = 0;
    OSStatus st = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
        fmt, 0, nullptr, nullptr, &count, &nal_hdr_len);
    if (st != noErr || count == 0) return false;

    for (size_t i = 0; i < count; ++i) {
        const uint8_t* ps = nullptr;
        size_t ps_len = 0;
        st = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            fmt, i, &ps, &ps_len, nullptr, nullptr);
        if (st != noErr) return false;
        append_annexb(out, ps, ps_len);
    }
    return true;
}

// Convert a length-prefixed (AVCC-style) CMBlockBuffer to Annex-B.
// Both HEVC and H.264 use 4-byte length prefixes by default in CoreMedia.
static bool append_avcc_to_annexb(CMBlockBufferRef bb, std::vector<uint8_t>& out) {
    size_t total = CMBlockBufferGetDataLength(bb);
    size_t offset = 0;
    while (offset + 4 <= total) {
        uint8_t hdr[4];
        if (CMBlockBufferCopyDataBytes(bb, offset, 4, hdr) != kCMBlockBufferNoErr) return false;
        uint32_t nal_len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16)
                         | ((uint32_t)hdr[2] << 8)  |  (uint32_t)hdr[3];
        offset += 4;
        if (nal_len == 0 || offset + nal_len > total) return false;

        size_t dst = out.size();
        out.resize(dst + 4 + nal_len);
        static const uint8_t sc[4] = {0, 0, 0, 1};
        std::memcpy(out.data() + dst, sc, 4);
        if (CMBlockBufferCopyDataBytes(bb, offset, nal_len, out.data() + dst + 4) != kCMBlockBufferNoErr) {
            return false;
        }
        offset += nal_len;
    }
    return offset == total;
}

static bool sample_is_keyframe(CMSampleBufferRef sb) {
    CFArrayRef attach = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (!attach || CFArrayGetCount(attach) == 0) return true; // default: keyframe
    CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(attach, 0);
    CFBooleanRef not_sync = (CFBooleanRef)CFDictionaryGetValue(d, kCMSampleAttachmentKey_NotSync);
    // NotSync absent or false => keyframe
    return (not_sync == nullptr) || !CFBooleanGetValue(not_sync);
}

} // namespace

struct MacVideoToolboxEncoder::Impl {
    VTCompressionSessionRef session = nullptr;
    std::mutex mutex;
    std::deque<MacEncodedPacket> queue;
    // Which parameter-set layout the output callback should extract on
    // keyframes (VPS/SPS/PPS for HEVC vs SPS/PPS for H.264). Set once in
    // init() before the session starts emitting frames (VIV-7).
    bool is_hevc = true;
};

static void vt_output_callback(void* outputCallbackRefCon,
                               void* /*sourceFrameRefCon*/,
                               OSStatus status,
                               VTEncodeInfoFlags /*infoFlags*/,
                               CMSampleBufferRef sampleBuffer) {
    auto* impl = static_cast<MacVideoToolboxEncoder::Impl*>(outputCallbackRefCon);
    if (status != noErr || !sampleBuffer) {
        vivora::log::warn(TAG, "Encoder output status=%d", (int)status);
        return;
    }
    if (!CMSampleBufferDataIsReady(sampleBuffer)) return;

    bool keyframe = sample_is_keyframe(sampleBuffer);

    MacEncodedPacket pkt;
    pkt.keyframe = keyframe;

    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    pkt.pts = CMTIME_IS_VALID(pts) ? (uint64_t)(CMTimeGetSeconds(pts) * 1'000'000.0) : 0;

    // On keyframes, prepend the parameter sets as Annex-B: VPS/SPS/PPS for
    // HEVC, SPS/PPS for H.264 (VIV-7).
    if (keyframe) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (fmt) {
            bool ok = impl->is_hevc ? append_param_sets_hevc(fmt, pkt.data)
                                    : append_param_sets_h264(fmt, pkt.data);
            if (!ok) {
                vivora::log::warn(TAG, "Failed to extract %s parameter sets",
                                  impl->is_hevc ? "HEVC" : "H.264");
            }
        }
    }

    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!bb || !append_avcc_to_annexb(bb, pkt.data)) {
        vivora::log::warn(TAG, "Failed to convert AVCC to Annex-B");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->queue.push_back(std::move(pkt));
    }
}

MacVideoToolboxEncoder::MacVideoToolboxEncoder() : impl_(new Impl()) {}

MacVideoToolboxEncoder::~MacVideoToolboxEncoder() {
    shutdown();
    delete impl_;
}

static void set_prop_bool(VTCompressionSessionRef s, CFStringRef key, bool val) {
    VTSessionSetProperty(s, key, val ? kCFBooleanTrue : kCFBooleanFalse);
}
static void set_prop_int(VTCompressionSessionRef s, CFStringRef key, int32_t val) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &val);
    VTSessionSetProperty(s, key, n);
    CFRelease(n);
}

bool MacVideoToolboxEncoder::init(const MacEncoderConfig& config) {
    config_ = config;

    const bool is_hevc = (config.codec != vivora::VideoCodec::H264);
    // H.264 is our SDR 8-bit fallback path — HDR (Main10/BT.2020/PQ) is HEVC
    // only, so ignore the hdr flag when encoding H.264 (VIV-7).
    const bool hdr = is_hevc && config.hdr;
    impl_->is_hevc = is_hevc;
    const CMVideoCodecType codec_type = is_hevc ? kCMVideoCodecType_HEVC
                                                : kCMVideoCodecType_H264;

    CFMutableDictionaryRef encoder_specs = nullptr;
#if TARGET_OS_OSX
    encoder_specs = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(encoder_specs,
        kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
        kCFBooleanTrue);
    CFDictionarySetValue(encoder_specs,
        kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
        kCFBooleanTrue);
#endif

    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        (int32_t)config.width,
        (int32_t)config.height,
        codec_type,
        encoder_specs,
        nullptr, // source pixel buffer attributes (nil = accept what we give)
        nullptr,
        vt_output_callback,
        impl_,
        &impl_->session);

    if (encoder_specs) CFRelease(encoder_specs);

    if (st != noErr || !impl_->session) {
        log::error(TAG, "VTCompressionSessionCreate failed: %d", (int)st);
        return false;
    }

    // Real-time, low-latency tuning.
    set_prop_bool(impl_->session, kVTCompressionPropertyKey_RealTime, true);
    set_prop_bool(impl_->session, kVTCompressionPropertyKey_AllowFrameReordering, false);
    if (@available(macOS 11.0, *)) {
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
    }

    // Profile / color.
    if (!is_hevc) {
        // H.264 SDR 8-bit fallback (VIV-7). High profile with auto level; the
        // AVC High profile allows CABAC, which the SDR 709 property set below
        // pairs with for a small bitrate win. Baseline-only decoders won't
        // reach this path (they'd have negotiated a different stream), but the
        // entropy-mode set is best-effort anyway — VideoToolbox silently keeps
        // CAVLC if the chosen profile can't do CABAC.
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_H264_High_AutoLevel);
        if (@available(macOS 10.9, *)) {
            VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_H264EntropyMode,
                                 kVTH264EntropyMode_CABAC);
        }
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_ColorPrimaries,
                             kCVImageBufferColorPrimaries_ITU_R_709_2);
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_TransferFunction,
                             kCVImageBufferTransferFunction_ITU_R_709_2);
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_YCbCrMatrix,
                             kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    } else if (hdr) {
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_HEVC_Main10_AutoLevel);
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_ColorPrimaries,
                             kCVImageBufferColorPrimaries_ITU_R_2020);
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_TransferFunction,
                             kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ);
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_YCbCrMatrix,
                             kCVImageBufferYCbCrMatrix_ITU_R_2020);
    } else {
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_HEVC_Main_AutoLevel);
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_ColorPrimaries,
                             kCVImageBufferColorPrimaries_ITU_R_709_2);
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_TransferFunction,
                             kCVImageBufferTransferFunction_ITU_R_709_2);
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_YCbCrMatrix,
                             kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    }

    // Bitrate (CBR-like).
    set_prop_int(impl_->session, kVTCompressionPropertyKey_AverageBitRate, (int32_t)config.bitrate_bps);

    // Data rate limit: [bytes, seconds] — cap peak to ~1.5x over 1 second.
    {
        int64_t bytes = (int64_t)((config.bitrate_bps * 3 / 2) / 8);
        CFNumberRef b = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &bytes);
        double secs = 1.0;
        CFNumberRef s = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &secs);
        const void* vals[2] = { b, s };
        CFArrayRef arr = CFArrayCreate(kCFAllocatorDefault, vals, 2, &kCFTypeArrayCallBacks);
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_DataRateLimits, arr);
        CFRelease(arr); CFRelease(b); CFRelease(s);
    }

    // Keyframe interval.
    set_prop_int(impl_->session, kVTCompressionPropertyKey_MaxKeyFrameInterval, (int32_t)config.idr_period);
    set_prop_int(impl_->session, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, (int32_t)(config.idr_period / config.fps + 1));

    // Expected frame rate hint.
    {
        int32_t fps = (int32_t)config.fps;
        CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &fps);
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_ExpectedFrameRate, n);
        CFRelease(n);
    }

    VTCompressionSessionPrepareToEncodeFrames(impl_->session);

    log::info(TAG, "Encoder ready: %ux%u @ %u fps, %u bps, %s",
              config.width, config.height, config.fps, config.bitrate_bps,
              is_hevc ? (hdr ? "HEVC HDR10 (Main10)" : "HEVC SDR (Main)")
                      : "H.264 SDR (High)");
    return true;
}

void MacVideoToolboxEncoder::shutdown() {
    if (impl_ && impl_->session) {
        VTCompressionSessionCompleteFrames(impl_->session, kCMTimeInvalid);
        VTCompressionSessionInvalidate(impl_->session);
        CFRelease(impl_->session);
        impl_->session = nullptr;
    }
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->queue.clear();
    }
}

bool MacVideoToolboxEncoder::encode(CVPixelBufferRef frame, uint64_t pts_us) {
    if (!impl_->session || !frame) return false;

    CMTime pts = CMTimeMake((int64_t)pts_us, 1'000'000);
    CMTime dur = CMTimeMake(1, (int32_t)config_.fps);

    CFDictionaryRef frame_props = nullptr;
    if (idr_pending_) {
        const void* keys[] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
        const void* vals[] = { kCFBooleanTrue };
        frame_props = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        idr_pending_ = false;
    }

    VTEncodeInfoFlags flags = 0;
    OSStatus st = VTCompressionSessionEncodeFrame(
        impl_->session, frame, pts, dur, frame_props, nullptr, &flags);

    if (frame_props) CFRelease(frame_props);
    CFRelease(frame); // take ownership as documented

    if (st != noErr) {
        log::warn(TAG, "EncodeFrame failed: %d", (int)st);
        return false;
    }
    return true;
}

bool MacVideoToolboxEncoder::get_packet(MacEncodedPacket& out) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->queue.empty()) return false;
    out = std::move(impl_->queue.front());
    impl_->queue.pop_front();
    return true;
}

void MacVideoToolboxEncoder::set_bitrate(uint32_t bitrate_bps) {
    if (!impl_->session) return;
    config_.bitrate_bps = bitrate_bps;
    int32_t br = (int32_t)bitrate_bps;
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &br);
    VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_AverageBitRate, n);
    CFRelease(n);

    int64_t bytes = (int64_t)((bitrate_bps * 3 / 2) / 8);
    CFNumberRef b = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &bytes);
    double secs = 1.0;
    CFNumberRef s = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &secs);
    const void* vals[2] = { b, s };
    CFArrayRef arr = CFArrayCreate(kCFAllocatorDefault, vals, 2, &kCFTypeArrayCallBacks);
    VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_DataRateLimits, arr);
    CFRelease(arr); CFRelease(b); CFRelease(s);
}

} // namespace vivora::host

#endif // VIVORA_MACOS
