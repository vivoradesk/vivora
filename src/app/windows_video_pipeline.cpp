// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef VIVORA_WINDOWS

#include "app/windows_video_pipeline.h"
#include "client/render/stream_window.h"
#include "common/utils/log.h"

#include <objbase.h>
#include <d3d11_4.h>

#pragma comment(lib, "ole32.lib")

namespace vivora {

using Microsoft::WRL::ComPtr;

WindowsVideoPipeline::WindowsVideoPipeline(StreamWindow* window) : window_(window) {
    // Prime the free list with every slot index.  Runs on the main thread
    // before the decode thread starts, so this is the producer side.
    for (uint32_t i = 0; i < static_cast<uint32_t>(kSlots); ++i) free_.try_push(i);
}

WindowsVideoPipeline::~WindowsVideoPipeline() = default;

bool WindowsVideoPipeline::create_device() {
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, 2, D3D11_SDK_VERSION,
        device_.GetAddressOf(), nullptr, context_.GetAddressOf());
    if (FAILED(hr)) {
        log::error("VIEW", "WindowsVideoPipeline: D3D11CreateDevice failed: 0x%08X", hr);
        return false;
    }
    // Multithread protection is mandatory here: the decode thread copies into
    // pool textures on the immediate context while the main thread runs the
    // renderer's VideoProcessorBlt on the same context.  (MfDecoder sets this
    // itself only when it creates its own device — we pass ours in.)
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(context_.As(&mt))) mt->SetMultithreadProtected(TRUE);
    return true;
}

bool WindowsVideoPipeline::init_decoder(VideoCodec codec) {
    if (dec_) return true;
    codec_ = codec;
    if (!device_ && !create_device()) return false;
    auto dec = IVideoDecoder::create();
    if (!dec->init(codec, device_.Get())) {
        log::error("VIEW", "WindowsVideoPipeline: decoder init failed");
        return false;
    }
    dec_ = std::move(dec);
    return true;
}

SubmitStatus WindowsVideoPipeline::submit(const uint8_t* data, size_t len,
                                          uint32_t /*timestamp*/, bool /*keyframe*/,
                                          uint16_t seq_no) {
    if (!dec_) return SubmitStatus::Rejected;
    // seq rides the MF sample time (pts) and round-trips out of get_frame —
    // same trick as the serial path — so FrameHandle.seq works for FTRACE.
    return dec_->decode(data, len, seq_no) ? SubmitStatus::Accepted
                                           : SubmitStatus::Rejected;
}

PollStatus WindowsVideoPipeline::poll_frame(FrameHandle& out) {
    if (!dec_) return PollStatus::Empty;
    // Reserve a slot for this attempt and HOLD it across empty polls — pushing
    // it back into free_ here would make the decode thread a second producer
    // and corrupt the SPSC free-list (VIV-82).
    if (reserved_idx_ == FrameHandle::kInvalid) {
        uint32_t idx;
        if (!free_.try_pop(idx)) return PollStatus::PoolFull;  // back-pressure
        reserved_idx_ = idx;
    }

    DecodedFrame f;
    if (!dec_->get_frame(f)) return PollStatus::Empty;  // keep reserved_idx_
    if (!f.texture) return PollStatus::Empty;

    D3D11_TEXTURE2D_DESC src_desc = {};
    f.texture->GetDesc(&src_desc);

    Slot& s = slots_[reserved_idx_];
    if (!s.tex || s.w != src_desc.Width || s.h != src_desc.Height
               || s.fmt != src_desc.Format) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width            = src_desc.Width;
        d.Height           = src_desc.Height;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = src_desc.Format;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_DEFAULT;
        // BIND_DECODER matches the MF decode textures the renderer already
        // consumes — CreateVideoProcessorInputView accepts it.
        d.BindFlags        = D3D11_BIND_DECODER;
        s.tex.Reset();
        HRESULT hr = device_->CreateTexture2D(&d, nullptr, s.tex.GetAddressOf());
        if (FAILED(hr)) {
            log::error("VIEW", "WindowsVideoPipeline: pool CreateTexture2D "
                       "%ux%u fmt=%u failed: 0x%08X",
                       src_desc.Width, src_desc.Height, src_desc.Format, hr);
            return PollStatus::Empty;  // frame dropped; slot stays reserved
        }
        s.w = src_desc.Width; s.h = src_desc.Height; s.fmt = src_desc.Format;
    }

    // GPU→GPU: detach the frame from MF's texture-array pool so the decoder
    // can't scribble into it while the handle sits in Q2.  Serialized against
    // the main thread's renderer by the device's multithread protection.
    context_->CopySubresourceRegion(s.tex.Get(), 0, 0, 0, 0,
                                    f.texture.Get(), f.subresource, nullptr);

    out.id  = reserved_idx_;
    out.seq = static_cast<uint16_t>(f.pts);  // seq round-tripped via pts
    reserved_idx_ = FrameHandle::kInvalid;   // consumed into Q2
    return PollStatus::Produced;
}

void WindowsVideoPipeline::flush_decoder() {
    // MFT_MESSAGE_COMMAND_FLUSH: drops buffered input/output + resets the
    // DPB.  Cheap; no full recreation needed — MF has no libav POC leak
    // (reinit_on_keyframe() is false for this impl).
    if (dec_) dec_->flush();
}

bool WindowsVideoPipeline::reinit_decoder() {
    // Hard reset (decode-error recovery / SEH-dead MFT): recreate the decoder
    // on the SAME device, so the renderer and the pool textures stay valid.
    if (!device_) return false;
    dec_.reset();
    auto dec = IVideoDecoder::create();
    if (!dec->init(codec_, device_.Get())) {
        log::error("VIEW", "WindowsVideoPipeline: decoder reinit failed");
        return false;
    }
    dec_ = std::move(dec);
    return true;
}

void WindowsVideoPipeline::present(FrameHandle h) {
    if (h.id >= static_cast<uint32_t>(kSlots) || !window_) return;
    Slot& s = slots_[h.id];
    if (!s.tex) return;
    // A monitor switch (VIV-50) changes the decoded frame size mid-session —
    // the renderer must be rebuilt for the new geometry or it keeps blitting
    // with the old dimensions (squashed/cropped picture).
    if (renderer_ready_ && (s.w != renderer_w_ || s.h != renderer_h_
                            || s.fmt != renderer_fmt_)) {
        log::info("VIEW", "Decoded size changed %ux%u -> %ux%u — reinit renderer",
                  renderer_w_, renderer_h_, s.w, s.h);
        renderer_ready_ = false;
    }
    if (!renderer_ready_) {
        renderer_ready_ = window_->init_renderer(device_.Get(), s.w, s.h, s.fmt);
        if (renderer_ready_) {
            renderer_w_ = s.w; renderer_h_ = s.h; renderer_fmt_ = s.fmt;
            // Mirror the serial path: apply the real crop dims if StreamInfo
            // beat the first frame, else fall back to the (possibly padded)
            // decoded dims so mouse mapping works until StreamInfo lands.
            if (pending_stream_w_ != 0 && pending_stream_h_ != 0) {
                window_->set_stream_size(pending_stream_w_, pending_stream_h_);
            } else {
                window_->set_stream_size(s.w, s.h);
            }
            log::info("VIEW", "Renderer started: %ux%u, format=%u (threaded)",
                      s.w, s.h, s.fmt);
        }
    }
    if (renderer_ready_) window_->render_frame(s.tex.Get(), 0);
}

void WindowsVideoPipeline::recycle(FrameHandle h) {
    if (h.id < static_cast<uint32_t>(kSlots)) free_.try_push(h.id);
}

void WindowsVideoPipeline::unreserve(FrameHandle h) {
    // Re-reserve the slot for the next poll_frame instead of pushing it to the
    // free list — keeps the decode thread off the SPSC producer side (VIV-82).
    if (h.id < static_cast<uint32_t>(kSlots)) reserved_idx_ = h.id;
}

void WindowsVideoPipeline::on_decode_thread_start() {
    // Media Foundation wants COM on the calling thread.  MfDecoder::init does
    // its own CoInitializeEx, but that ran on the main thread — submit/poll
    // (and any reinit) run here.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_initialized_ = SUCCEEDED(hr);
}

void WindowsVideoPipeline::on_decode_thread_stop() {
    if (com_initialized_) {
        CoUninitialize();
        com_initialized_ = false;
    }
}

void WindowsVideoPipeline::set_pending_stream_size(uint32_t w, uint32_t h) {
    pending_stream_w_ = w;
    pending_stream_h_ = h;
}

} // namespace vivora

#endif // VIVORA_WINDOWS
