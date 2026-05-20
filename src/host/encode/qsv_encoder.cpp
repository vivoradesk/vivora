#ifdef VIVORA_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "host/encode/qsv_encoder.h"
#include "common/codec/bitrate_controller.h"
#include "common/utils/log.h"
#include <algorithm>

#include <windows.h>
#include <d3d11_4.h>
#include <cstring>
#include <cstdio>

#include "vpl/mfxdispatcher.h"
#include "vpl/mfxvideo.h"
#include "vpl/mfxmemory.h"
#include "vpl/mfxstructures.h"
#include "vpl/mfxcommon.h"

namespace vivora {

static constexpr const char* TAG = "QSV";

// -------- oneVPL function pointer table --------
// libvpl.dll is loaded once per process. Function pointers are cached here.

namespace vpl {

typedef mfxLoader  (MFX_CDECL* PFN_MFXLoad)(void);
typedef void       (MFX_CDECL* PFN_MFXUnload)(mfxLoader);
typedef mfxConfig  (MFX_CDECL* PFN_MFXCreateConfig)(mfxLoader);
typedef mfxStatus  (MFX_CDECL* PFN_MFXSetConfigFilterProperty)(mfxConfig, const mfxU8*, mfxVariant);
typedef mfxStatus  (MFX_CDECL* PFN_MFXCreateSession)(mfxLoader, mfxU32, mfxSession*);
typedef mfxStatus  (MFX_CDECL* PFN_MFXClose)(mfxSession);
typedef mfxStatus  (MFX_CDECL* PFN_MFXVideoCORE_SetHandle)(mfxSession, mfxHandleType, mfxHDL);
typedef mfxStatus  (MFX_CDECL* PFN_MFXVideoCORE_SyncOperation)(mfxSession, mfxSyncPoint, mfxU32);
typedef mfxStatus  (MFX_CDECL* PFN_MFXVideoENCODE_Query)(mfxSession, mfxVideoParam*, mfxVideoParam*);
typedef mfxStatus  (MFX_CDECL* PFN_MFXVideoENCODE_Init)(mfxSession, mfxVideoParam*);
typedef mfxStatus  (MFX_CDECL* PFN_MFXVideoENCODE_Reset)(mfxSession, mfxVideoParam*);
typedef mfxStatus  (MFX_CDECL* PFN_MFXVideoENCODE_Close)(mfxSession);
typedef mfxStatus  (MFX_CDECL* PFN_MFXVideoENCODE_EncodeFrameAsync)(mfxSession, mfxEncodeCtrl*,
                                                                    mfxFrameSurface1*, mfxBitstream*,
                                                                    mfxSyncPoint*);
typedef mfxStatus  (MFX_CDECL* PFN_MFXVideoVPP_Query)(mfxSession, mfxVideoParam*, mfxVideoParam*);
typedef mfxStatus  (MFX_CDECL* PFN_MFXVideoVPP_Init)(mfxSession, mfxVideoParam*);
typedef mfxStatus  (MFX_CDECL* PFN_MFXVideoVPP_Reset)(mfxSession, mfxVideoParam*);
typedef mfxStatus  (MFX_CDECL* PFN_MFXVideoVPP_Close)(mfxSession);
typedef mfxStatus  (MFX_CDECL* PFN_MFXVideoVPP_RunFrameVPPAsync)(mfxSession, mfxFrameSurface1*,
                                                                 mfxFrameSurface1*, mfxExtVppAuxData*,
                                                                 mfxSyncPoint*);
typedef mfxStatus  (MFX_CDECL* PFN_MFXMemory_GetSurfaceForVPP)(mfxSession, mfxFrameSurface1**);
typedef mfxStatus  (MFX_CDECL* PFN_MFXMemory_GetSurfaceForVPPOut)(mfxSession, mfxFrameSurface1**);

struct Table {
    PFN_MFXLoad                              Load = nullptr;
    PFN_MFXUnload                            Unload = nullptr;
    PFN_MFXCreateConfig                      CreateConfig = nullptr;
    PFN_MFXSetConfigFilterProperty           SetFilter = nullptr;
    PFN_MFXCreateSession                     CreateSession = nullptr;
    PFN_MFXClose                             Close = nullptr;
    PFN_MFXVideoCORE_SetHandle               SetHandle = nullptr;
    PFN_MFXVideoCORE_SyncOperation           SyncOp = nullptr;
    PFN_MFXVideoENCODE_Query                 EncQuery = nullptr;
    PFN_MFXVideoENCODE_Init                  EncInit = nullptr;
    PFN_MFXVideoENCODE_Reset                 EncReset = nullptr;
    PFN_MFXVideoENCODE_Close                 EncClose = nullptr;
    PFN_MFXVideoENCODE_EncodeFrameAsync      EncFrame = nullptr;
    PFN_MFXVideoVPP_Query                    VppQuery = nullptr;
    PFN_MFXVideoVPP_Init                     VppInit = nullptr;
    PFN_MFXVideoVPP_Reset                    VppReset = nullptr;
    PFN_MFXVideoVPP_Close                    VppClose = nullptr;
    PFN_MFXVideoVPP_RunFrameVPPAsync         VppRun = nullptr;
    PFN_MFXMemory_GetSurfaceForVPP           GetVppIn = nullptr;
    PFN_MFXMemory_GetSurfaceForVPPOut        GetVppOut = nullptr;
};

static Table g_fn;
// libvpl.dll is loaded exactly once for the lifetime of the process.  We
// deliberately never FreeLibrary on the success path — the dispatcher pins
// GPU driver DLLs, device contexts, and async queues whose teardown order
// is not obvious, and unloading in ~QsvEncoder would also defeat the whole
// point of `load_once()` on sessions that reconnect.  The module pin is
// released by the OS at process exit, which is correct for a process-wide
// shared dependency.  FreeLibrary remains in the failure path only because
// there it cancels a partially-initialized load.
static HMODULE g_dll = nullptr;

template <typename T>
static bool resolve(HMODULE dll, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(dll, name));
    return out != nullptr;
}

static bool load_once() {
    if (g_dll) return true;
    g_dll = LoadLibraryA("libvpl.dll");
    if (!g_dll) return false;

    bool ok = true;
    ok &= resolve(g_dll, "MFXLoad",                          g_fn.Load);
    ok &= resolve(g_dll, "MFXUnload",                        g_fn.Unload);
    ok &= resolve(g_dll, "MFXCreateConfig",                  g_fn.CreateConfig);
    ok &= resolve(g_dll, "MFXSetConfigFilterProperty",       g_fn.SetFilter);
    ok &= resolve(g_dll, "MFXCreateSession",                 g_fn.CreateSession);
    ok &= resolve(g_dll, "MFXClose",                         g_fn.Close);
    ok &= resolve(g_dll, "MFXVideoCORE_SetHandle",           g_fn.SetHandle);
    ok &= resolve(g_dll, "MFXVideoCORE_SyncOperation",       g_fn.SyncOp);
    ok &= resolve(g_dll, "MFXVideoENCODE_Query",             g_fn.EncQuery);
    ok &= resolve(g_dll, "MFXVideoENCODE_Init",              g_fn.EncInit);
    ok &= resolve(g_dll, "MFXVideoENCODE_Reset",             g_fn.EncReset);
    ok &= resolve(g_dll, "MFXVideoENCODE_Close",             g_fn.EncClose);
    ok &= resolve(g_dll, "MFXVideoENCODE_EncodeFrameAsync",  g_fn.EncFrame);
    ok &= resolve(g_dll, "MFXVideoVPP_Query",                g_fn.VppQuery);
    ok &= resolve(g_dll, "MFXVideoVPP_Init",                 g_fn.VppInit);
    ok &= resolve(g_dll, "MFXVideoVPP_Reset",                g_fn.VppReset);
    ok &= resolve(g_dll, "MFXVideoVPP_Close",                g_fn.VppClose);
    ok &= resolve(g_dll, "MFXVideoVPP_RunFrameVPPAsync",     g_fn.VppRun);
    ok &= resolve(g_dll, "MFXMemory_GetSurfaceForVPP",       g_fn.GetVppIn);
    ok &= resolve(g_dll, "MFXMemory_GetSurfaceForVPPOut",    g_fn.GetVppOut);

    if (!ok) {
        log::error(TAG, "libvpl.dll missing expected oneVPL entry points");
        FreeLibrary(g_dll);
        g_dll = nullptr;
        return false;
    }
    return true;
}

} // namespace vpl

// Static bitstream helper — kept out of the header to avoid leaking the
// real mfxBitstream type into include paths of unrelated translation units.
struct QsvBitstream {
    mfxBitstream bs;
};

// Cached encoder parameters. Init and Reset must share the exact same
// mfxVideoParam + ExtBuffers, so we own them here and hand the .cpp a
// stable pointer via QsvEncoder::enc_params_.
struct QsvEncParams {
    mfxVideoParam       p{};
    mfxExtCodingOption  co{};
    mfxExtCodingOption2 co2{};
    mfxExtCodingOption3 co3{};
    mfxExtBuffer*       ext[3]{};
};

QsvEncoder::QsvEncoder() = default;

QsvEncoder::~QsvEncoder() {
    if (have_encoder_ && session_) {
        vpl::g_fn.EncClose(reinterpret_cast<mfxSession>(session_));
        have_encoder_ = false;
    }
    if (have_vpp_ && session_) {
        vpl::g_fn.VppClose(reinterpret_cast<mfxSession>(session_));
        have_vpp_ = false;
    }
    if (session_) {
        vpl::g_fn.Close(reinterpret_cast<mfxSession>(session_));
        session_ = nullptr;
    }
    if (loader_) {
        vpl::g_fn.Unload(reinterpret_cast<mfxLoader>(loader_));
        loader_ = nullptr;
    }
    delete static_cast<QsvBitstream*>(bitstream_);
    bitstream_ = nullptr;
    delete static_cast<QsvEncParams*>(enc_params_);
    enc_params_ = nullptr;
    d3d_context_.Reset();
}

bool QsvEncoder::load_dispatcher() {
    if (!vpl::load_once()) {
        log::error(TAG, "Failed to load libvpl.dll");
        return false;
    }
    return true;
}

bool QsvEncoder::create_session() {
    mfxLoader loader = vpl::g_fn.Load();
    if (!loader) {
        log::error(TAG, "MFXLoad failed");
        return false;
    }
    loader_ = loader;

    auto set_u32 = [&](const char* prop, mfxU32 v) -> bool {
        mfxConfig cfg = vpl::g_fn.CreateConfig(loader);
        if (!cfg) return false;
        mfxVariant var{};
        var.Type = MFX_VARIANT_TYPE_U32;
        var.Data.U32 = v;
        return vpl::g_fn.SetFilter(cfg, reinterpret_cast<const mfxU8*>(prop), var) == MFX_ERR_NONE;
    };

    if (!set_u32("mfxImplDescription.Impl",                 MFX_IMPL_TYPE_HARDWARE))   return false;
    if (!set_u32("mfxImplDescription.AccelerationMode",     MFX_ACCEL_MODE_VIA_D3D11)) return false;

    mfxU32 codec_id = (config_.codec == VideoCodec::HEVC) ? MFX_CODEC_HEVC : MFX_CODEC_AVC;
    if (!set_u32("mfxImplDescription.mfxEncoderDescription.encoder.CodecID", codec_id)) return false;

    mfxSession s = nullptr;
    mfxStatus st = vpl::g_fn.CreateSession(loader, 0, &s);
    if (st != MFX_ERR_NONE || !s) {
        log::error(TAG, "MFXCreateSession failed: %d (no HW impl supporting %s?)",
                   (int)st, config_.codec == VideoCodec::HEVC ? "HEVC" : "H.264");
        return false;
    }
    session_ = s;

    // Intel oneVPL requires multithread-protected D3D11 device before SetHandle.
    ID3D11Multithread* mt = nullptr;
    if (SUCCEEDED(device_->QueryInterface(__uuidof(ID3D11Multithread),
                                          reinterpret_cast<void**>(&mt))) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    // Hand our D3D11 device over to the session so internal surfaces are
    // allocated on the right adapter.
    st = vpl::g_fn.SetHandle(s, MFX_HANDLE_D3D11_DEVICE, device_);
    if (st != MFX_ERR_NONE) {
        log::error(TAG, "SetHandle(D3D11_DEVICE) failed: %d", (int)st);
        return false;
    }
    return true;
}

static void fill_frame_info(mfxFrameInfo& fi, mfxU32 fourcc,
                            uint32_t w, uint32_t h, uint32_t fps) {
    fi.FourCC        = fourcc;
    fi.ChromaFormat  = (fourcc == MFX_FOURCC_NV12) ? (mfxU16)MFX_CHROMAFORMAT_YUV420
                                                   : (mfxU16)MFX_CHROMAFORMAT_YUV444;
    fi.BitDepthLuma   = 8;
    fi.BitDepthChroma = 8;
    fi.Width   = static_cast<mfxU16>((w + 15) & ~15);  // 16-byte aligned
    fi.Height  = static_cast<mfxU16>((h + 15) & ~15);
    fi.CropX   = 0;
    fi.CropY   = 0;
    fi.CropW   = static_cast<mfxU16>(w);
    fi.CropH   = static_cast<mfxU16>(h);
    fi.FrameRateExtN = fps;
    fi.FrameRateExtD = 1;
    fi.AspectRatioW  = 1;
    fi.AspectRatioH  = 1;
    fi.PicStruct     = MFX_PICSTRUCT_PROGRESSIVE;
}

bool QsvEncoder::configure_vpp() {
    mfxVideoParam p{};
    p.AsyncDepth = 1;
    p.IOPattern  = MFX_IOPATTERN_IN_VIDEO_MEMORY | MFX_IOPATTERN_OUT_VIDEO_MEMORY;
    fill_frame_info(p.vpp.In,  MFX_FOURCC_RGB4, config_.width, config_.height, config_.fps);
    fill_frame_info(p.vpp.Out, MFX_FOURCC_NV12, config_.width, config_.height, config_.fps);

    mfxStatus st = vpl::g_fn.VppInit(reinterpret_cast<mfxSession>(session_), &p);
    if (st < MFX_ERR_NONE) {
        log::error(TAG, "VPP Init failed: %d", (int)st);
        return false;
    }
    if (st > MFX_ERR_NONE) {
        log::warn(TAG, "VPP Init warning: %d", (int)st);
    }
    have_vpp_ = true;
    return true;
}

bool QsvEncoder::configure_encoder() {
    if (!enc_params_) enc_params_ = new QsvEncParams();
    auto* ep = static_cast<QsvEncParams*>(enc_params_);
    *ep = {};

    mfxVideoParam& p = ep->p;
    p.AsyncDepth = 1;
    p.IOPattern  = MFX_IOPATTERN_IN_VIDEO_MEMORY;
    p.mfx.CodecId           = (config_.codec == VideoCodec::HEVC) ? MFX_CODEC_HEVC : MFX_CODEC_AVC;
    p.mfx.TargetUsage       = MFX_TARGETUSAGE_BEST_SPEED;  // TU=7, lowest latency
    p.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
    p.mfx.TargetKbps        = static_cast<mfxU16>(config_.bitrate_bps / 1000);
    p.mfx.MaxKbps           = p.mfx.TargetKbps;
    // VBV sized for 1 second at current bitrate. LowDelayBRC needs the
    // buffer proportional to the actual rate — a much larger ceiling makes
    // the BRC think it has seconds of budget and produces blocky output at
    // low bitrates. set_bitrate() does Reset when the new rate fits the
    // current VBV, and falls back to Close+Init when it doesn't.
    p.mfx.BufferSizeInKB    = static_cast<mfxU16>(
        std::max<uint32_t>(config_.bitrate_bps / 8000u, 128u));
    p.mfx.InitialDelayInKB  = p.mfx.BufferSizeInKB / 2;
    vbv_kb_                 = p.mfx.BufferSizeInKB;
    // Default: try intra refresh with a long natural GOP. If the driver
    // rejects intra refresh (HEVC support is patchy on Intel), we fall
    // back to a long plain GOP below and rely on loss-triggered IDR from
    // the client for recovery. Either way we avoid the 1s-IDR burst
    // pattern that kills FEC on WiFi.
    const mfxU16 wanted_cycle = static_cast<mfxU16>(config_.fps);
    const mfxU16 wanted_gop   = static_cast<mfxU16>(
        config_.idr_period ? config_.idr_period
                           : std::min<uint32_t>(config_.fps * 30, 0xFFFE));
    p.mfx.GopPicSize        = wanted_gop;
    p.mfx.GopRefDist        = 1;      // no B-frames
    p.mfx.NumRefFrame       = 1;
    p.mfx.IdrInterval       = 0;      // every I is IDR (HEVC: interval in number of I-frames)
    p.mfx.NumSlice          = 1;
    p.mfx.CodecProfile      = (config_.codec == VideoCodec::HEVC) ? (mfxU16)MFX_PROFILE_HEVC_MAIN
                                                                  : (mfxU16)MFX_PROFILE_AVC_HIGH;
    fill_frame_info(p.mfx.FrameInfo, MFX_FOURCC_NV12,
                    config_.width, config_.height, config_.fps);

    // Low-latency BRC + no lookahead.
    ep->co.Header.BufferId  = MFX_EXTBUFF_CODING_OPTION;   ep->co.Header.BufferSz  = sizeof(ep->co);
    ep->co.NalHrdConformance   = MFX_CODINGOPTION_OFF;
    ep->co.PicTimingSEI        = MFX_CODINGOPTION_OFF;
    ep->co.VuiNalHrdParameters = MFX_CODINGOPTION_OFF;

    ep->co2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;  ep->co2.Header.BufferSz = sizeof(ep->co2);
    ep->co2.LookAheadDepth    = 0;
    ep->co2.RepeatPPS         = MFX_CODINGOPTION_ON;
    ep->co2.IntRefType        = MFX_REFRESH_SLICE;
    ep->co2.IntRefCycleSize   = wanted_cycle;

    ep->co3.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;  ep->co3.Header.BufferSz = sizeof(ep->co3);
    ep->co3.LowDelayBRC       = MFX_CODINGOPTION_ON;

    ep->ext[0] = reinterpret_cast<mfxExtBuffer*>(&ep->co);
    ep->ext[1] = reinterpret_cast<mfxExtBuffer*>(&ep->co2);
    ep->ext[2] = reinterpret_cast<mfxExtBuffer*>(&ep->co3);
    p.ExtParam    = ep->ext;
    p.NumExtParam = 3;

    mfxStatus st = vpl::g_fn.EncQuery(reinterpret_cast<mfxSession>(session_), &p, &p);
    if (st < MFX_ERR_NONE) {
        // Intra refresh rejected — try again without it.
        log::warn(TAG, "Intra refresh rejected (err %d), falling back to long GOP", (int)st);
        ep->co2.IntRefType      = MFX_REFRESH_NO;
        ep->co2.IntRefCycleSize = 0;
        p.mfx.GopPicSize        = wanted_gop;
        st = vpl::g_fn.EncQuery(reinterpret_cast<mfxSession>(session_), &p, &p);
    }
    if (st < MFX_ERR_NONE) {
        log::error(TAG, "ENCODE Query failed: %d", (int)st);
        return false;
    }
    const char* ir_name = "off";
    switch (ep->co2.IntRefType) {
        case MFX_REFRESH_VERTICAL:   ir_name = "vertical";   break;
        case MFX_REFRESH_HORIZONTAL: ir_name = "horizontal"; break;
        case MFX_REFRESH_SLICE:      ir_name = "slice";      break;
        default: break;
    }
    log::info(TAG, "Intra refresh: %s (cycle=%u), GOP=%u",
              ir_name,
              (unsigned)ep->co2.IntRefCycleSize,
              (unsigned)p.mfx.GopPicSize);
    if (st > MFX_ERR_NONE) {
        log::warn(TAG, "ENCODE Query adjusted params (warning %d)", (int)st);
    }

    st = vpl::g_fn.EncInit(reinterpret_cast<mfxSession>(session_), &p);
    if (st < MFX_ERR_NONE) {
        log::error(TAG, "ENCODE Init failed: %d", (int)st);
        return false;
    }
    if (st > MFX_ERR_NONE) {
        log::warn(TAG, "ENCODE Init warning: %d", (int)st);
    }
    have_encoder_ = true;

    // Output bitstream buffer must be >= mfx.BufferSizeInKB * 1024 or
    // EncodeFrameAsync returns MFX_ERR_NOT_ENOUGH_BUFFER. 2× VBV gives
    // room for keyframe bursts on top of the VBV ceiling.
    bitstream_buf_.resize(static_cast<size_t>(vbv_kb_) * 1024u * 2u);
    auto* bsw = new QsvBitstream();
    std::memset(&bsw->bs, 0, sizeof(bsw->bs));
    bsw->bs.Data      = bitstream_buf_.data();
    bsw->bs.MaxLength = static_cast<mfxU32>(bitstream_buf_.size());
    bitstream_ = bsw;
    return true;
}

bool QsvEncoder::init(const EncoderConfig& config, ID3D11Device* device) {
    config_ = config;
    device_ = device;
    if (!device_) {
        log::error(TAG, "D3D11 device is null");
        return false;
    }
    device_->GetImmediateContext(&d3d_context_);

    if (!load_dispatcher())  return false;
    if (!create_session())   return false;
    if (!configure_vpp())    return false;
    if (!configure_encoder()) return false;

    log::info(TAG, "QSV %s encoder initialized: %ux%u @ %u fps, %u kbps",
              config_.codec == VideoCodec::HEVC ? "HEVC" : "H.264",
              config_.width, config_.height, config_.fps, config_.bitrate_bps / 1000);
    return true;
}

bool QsvEncoder::run_vpp(ID3D11Texture2D* bgra_input, void** nv12_surface_out) {
    mfxSession s = reinterpret_cast<mfxSession>(session_);

    mfxFrameSurface1* in_surf = nullptr;
    mfxStatus st = vpl::g_fn.GetVppIn(s, &in_surf);
    if (st != MFX_ERR_NONE || !in_surf) {
        log::error(TAG, "GetSurfaceForVPP failed: %d", (int)st);
        return false;
    }

    // Native D3D11 handle of the VPP input surface — copy our capture
    // texture into it in-GPU.
    mfxHDL hdl = nullptr;
    mfxResourceType rtype{};
    st = in_surf->FrameInterface->GetNativeHandle(in_surf, &hdl, &rtype);
    if (st != MFX_ERR_NONE || !hdl) {
        log::error(TAG, "GetNativeHandle failed: %d", (int)st);
        in_surf->FrameInterface->Release(in_surf);
        return false;
    }
    auto* dst_tex = reinterpret_cast<ID3D11Texture2D*>(hdl);
    d3d_context_->CopySubresourceRegion(dst_tex, 0, 0, 0, 0, bgra_input, 0, nullptr);

    mfxFrameSurface1* out_surf = nullptr;
    st = vpl::g_fn.GetVppOut(s, &out_surf);
    if (st != MFX_ERR_NONE || !out_surf) {
        log::error(TAG, "GetSurfaceForVPPOut failed: %d", (int)st);
        in_surf->FrameInterface->Release(in_surf);
        return false;
    }

    mfxSyncPoint sp = nullptr;
    st = vpl::g_fn.VppRun(s, in_surf, out_surf, nullptr, &sp);
    // Release our refcount on the input — VPP took its own.
    in_surf->FrameInterface->Release(in_surf);

    if (st < MFX_ERR_NONE) {
        log::error(TAG, "VPP RunFrameVPPAsync failed: %d", (int)st);
        out_surf->FrameInterface->Release(out_surf);
        return false;
    }
    if (sp) {
        // Synchronously wait for VPP so encoder sees finished NV12.
        vpl::g_fn.SyncOp(s, sp, 1000);
    }

    *nv12_surface_out = out_surf;
    return true;
}

void QsvEncoder::drain_bitstream() {
    auto* bsw = static_cast<QsvBitstream*>(bitstream_);
    if (!bsw || bsw->bs.DataLength == 0) return;

    EncodedPacket pkt;
    pkt.data.assign(bsw->bs.Data + bsw->bs.DataOffset,
                    bsw->bs.Data + bsw->bs.DataOffset + bsw->bs.DataLength);
    pkt.pts      = bsw->bs.TimeStamp;
    pkt.keyframe = (bsw->bs.FrameType & MFX_FRAMETYPE_IDR) != 0 ||
                   (bsw->bs.FrameType & MFX_FRAMETYPE_I)   != 0;

    // Reset for next frame.
    bsw->bs.DataOffset = 0;
    bsw->bs.DataLength = 0;

    output_packets_.push(std::move(pkt));
}

bool QsvEncoder::encode(ID3D11Texture2D* texture, uint64_t pts_us) {
    if (!have_encoder_ || !have_vpp_ || !texture) return false;
    mfxSession s = reinterpret_cast<mfxSession>(session_);

    // VPP: BGRA -> NV12.
    void* nv12_opaque = nullptr;
    if (!run_vpp(texture, &nv12_opaque)) return false;
    auto* nv12_surf = static_cast<mfxFrameSurface1*>(nv12_opaque);
    nv12_surf->Data.TimeStamp = pts_us * 90 / 1000;  // 90kHz ticks

    // Per-frame control: force IDR if requested.
    mfxEncodeCtrl ctrl{};
    mfxEncodeCtrl* ctrl_ptr = nullptr;
    if (idr_requested_) {
        ctrl.FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_REF;
        ctrl_ptr = &ctrl;
        idr_requested_ = false;
    }

    auto* bsw = static_cast<QsvBitstream*>(bitstream_);
    mfxSyncPoint sp = nullptr;

    mfxStatus st = vpl::g_fn.EncFrame(s, ctrl_ptr, nv12_surf, &bsw->bs, &sp);
    // We can release our VPP-out reference once EncodeFrameAsync has taken
    // its own ref on the surface.
    nv12_surf->FrameInterface->Release(nv12_surf);

    if (st == MFX_ERR_MORE_DATA) {
        // Encoder is holding this frame; next call may yield a packet.
        return true;
    }
    if (st < MFX_ERR_NONE) {
        log::error(TAG, "EncodeFrameAsync failed: %d", (int)st);
        return false;
    }
    if (sp) {
        st = vpl::g_fn.SyncOp(s, sp, 1000);
        if (st < MFX_ERR_NONE) {
            log::error(TAG, "SyncOperation(encode) failed: %d", (int)st);
            return false;
        }
        drain_bitstream();
    }
    return true;
}

bool QsvEncoder::get_packet(EncodedPacket& packet) {
    if (output_packets_.empty()) return false;
    packet = std::move(output_packets_.front());
    output_packets_.pop();
    return true;
}

void QsvEncoder::request_idr() { idr_requested_ = true; }

void QsvEncoder::set_bitrate(uint32_t bps) {
    if (!have_encoder_ || !enc_params_) { config_.bitrate_bps = bps; return; }

    // Patch the cached params in place so Reset sees the same structure
    // (and the same ExtBuffers) that Init used — otherwise Intel returns
    // MFX_ERR_INCOMPATIBLE_VIDEO_PARAM.
    // Patch TargetKbps/MaxKbps in the cached params. Keep BufferSizeInKB
    // constant (sized for the configured ceiling at init time) so Intel
    // doesn't have to reallocate the VBV buffer — that's what triggers
    // MFX_ERR_INCOMPATIBLE_VIDEO_PARAM on Reset for this driver.
    auto* ep = static_cast<QsvEncParams*>(enc_params_);
    ep->p.mfx.TargetKbps = static_cast<mfxU16>(bps / 1000);
    ep->p.mfx.MaxKbps    = ep->p.mfx.TargetKbps;

    mfxStatus st = vpl::g_fn.EncReset(reinterpret_cast<mfxSession>(session_), &ep->p);
    if (st < MFX_ERR_NONE) {
        log::warn(TAG, "ENCODE Reset (keep-buf) failed: %d, falling back to re-init", (int)st);
        vpl::g_fn.EncClose(reinterpret_cast<mfxSession>(session_));
        have_encoder_ = false;
        config_.bitrate_bps = bps;
        if (!configure_encoder()) {
            log::warn(TAG, "ENCODE re-init after bitrate change failed");
            return;
        }
        idr_requested_ = true;
        return;
    }
    config_.bitrate_bps = bps;
}

} // namespace vivora

#endif // VIVORA_WINDOWS
