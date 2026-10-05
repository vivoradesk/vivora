// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef VIVORA_WINDOWS

#include "host/capture/dxgi_capture.h"
#include "common/utils/log.h"
#include "common/utils/metrics.h"
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace vivora {

static const char* TAG = "CAPTURE";

DxgiCapture::DxgiCapture() = default;

DxgiCapture::~DxgiCapture() {
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
    }
}

bool DxgiCapture::init(uint32_t monitor_index) {
    monitor_index_ = monitor_index;
    if (!init_d3d11()) return false;
    if (!init_output_duplication(monitor_index)) return false;
    wanted_name_ = output_name_;   // the display we were asked to capture

    log::info(TAG, "Initialized DXGI capture: %ux%u on monitor %u",
              resolution_.width, resolution_.height, monitor_index);
    return true;
}

bool DxgiCapture::init_d3d11() {
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                    // no software rasterizer
        flags,
        feature_levels,
        _countof(feature_levels),
        D3D11_SDK_VERSION,
        device_.GetAddressOf(),
        nullptr,
        context_.GetAddressOf()
    );

    if (FAILED(hr)) {
        log::error(TAG, "D3D11CreateDevice failed: 0x%08X", hr);
        return false;
    }

    return true;
}

namespace {

// Cheap, always-fresh signature of the desktop's monitor layout: count plus
// each monitor's rectangle, straight from user32.  DXGI cannot be used for
// this — IDXGIFactory1::IsCurrent() reports adapter-set changes and, measured
// on Windows 11, does NOT flip when a display is merely attached or detached,
// so a DXGI-only poll noticed a hot-plug tens of seconds late or not at all
// (VIV-147).  EnumDisplayMonitors sees it immediately and costs microseconds.
struct MonitorSig {
    std::string s;
};

BOOL CALLBACK sig_proc(HMONITOR, HDC, LPRECT rc, LPARAM param) {
    auto* sig = reinterpret_cast<MonitorSig*>(param);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%ld,%ld,%ld,%ld;",
                  rc->left, rc->top, rc->right, rc->bottom);
    sig->s += buf;
    return TRUE;
}

std::string desktop_layout_signature() {
    MonitorSig sig;
    EnumDisplayMonitors(nullptr, nullptr, sig_proc, reinterpret_cast<LPARAM>(&sig));
    return std::move(sig.s);
}

} // namespace

bool DxgiCapture::ensure_factory() {
    // A DXGI factory is a snapshot of the display topology: outputs attached
    // or detached after it was created are invisible to it, and the adapters
    // it hands out keep serving the stale list.  The only cure is a fresh
    // factory (VIV-147).
    //
    // IsCurrent() is DXGI's own "your snapshot expired" flag, but measured on
    // Windows 11 it does NOT flip when a display is merely attached or
    // detached — a DXGI-only poll saw a hot-plug tens of seconds late, or
    // never.  So the desktop layout is what we actually watch, and DXGI is
    // made to catch up whenever it moves.
    std::string layout = desktop_layout_signature();
    const bool moved = !layout_sig_.empty() && layout != layout_sig_;
    layout_sig_ = std::move(layout);

    if (!moved && factory_ && factory_->IsCurrent() && adapter_) return true;

    if (adapter_luid_.LowPart == 0 && adapter_luid_.HighPart == 0) {
        // First call — learn which adapter our D3D11 device lives on.  The
        // LUID is stable across factory re-creation; the adapter *object* is
        // not, which is exactly why we re-resolve instead of caching it.
        ComPtr<IDXGIDevice> dxgi_device;
        if (FAILED(device_.As(&dxgi_device))) {
            log::error(TAG, "ensure_factory: no IDXGIDevice");
            return false;
        }
        ComPtr<IDXGIAdapter> dev_adapter;
        if (FAILED(dxgi_device->GetAdapter(dev_adapter.GetAddressOf()))) {
            log::error(TAG, "ensure_factory: GetAdapter failed");
            return false;
        }
        DXGI_ADAPTER_DESC ad;
        if (FAILED(dev_adapter->GetDesc(&ad))) {
            log::error(TAG, "ensure_factory: adapter GetDesc failed");
            return false;
        }
        adapter_luid_ = ad.AdapterLuid;
    }

    factory_.Reset();
    adapter_.Reset();
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(factory_.GetAddressOf()));
    if (FAILED(hr)) {
        log::error(TAG, "CreateDXGIFactory1 failed: 0x%08X", hr);
        return false;
    }

    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; factory_->EnumAdapters1(i, a.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 ad;
        if (SUCCEEDED(a->GetDesc1(&ad))
            && ad.AdapterLuid.LowPart == adapter_luid_.LowPart
            && ad.AdapterLuid.HighPart == adapter_luid_.HighPart) {
            adapter_ = a;
            return true;
        }
        a.Reset();
    }
    log::error(TAG, "ensure_factory: our adapter is gone from the refreshed factory");
    return false;
}

bool DxgiCapture::init_output_duplication(uint32_t monitor_index) {
    if (!ensure_factory()) return false;

    probe_next_frame_ = true;

    ComPtr<IDXGIOutput> output;
    HRESULT hr = adapter_->EnumOutputs(monitor_index, output.GetAddressOf());
    if (FAILED(hr)) {
        log::error(TAG, "Failed to enumerate output %u: 0x%08X", monitor_index, hr);
        return false;
    }

    // Get output description for resolution + desktop origin.  The origin
    // matters for input injection on multi-monitor hosts (VIV-50): SendInput
    // absolute coords span the virtual desktop, so a non-primary display's
    // offset must be added to the normalized client coordinates.
    DXGI_OUTPUT_DESC desc;
    output->GetDesc(&desc);
    note_resolution(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left,
                    desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
    origin_x_ = desc.DesktopCoordinates.left;
    origin_y_ = desc.DesktopCoordinates.top;
    output_name_ = desc.DeviceName;
    output_rect_ = desc.DesktopCoordinates;

    // Try IDXGIOutput5::DuplicateOutput1 for FP16 HDR capture
    hr = output.As(&output5_);
    if (SUCCEEDED(hr)) {
        // Ask for the display's own depth and send that on: we never make DXGI
        // convert for us.  The encoder is built for whatever comes back (see
        // the first-frame probe below and codec_for_capture on the host side).
        DXGI_FORMAT formats[] = {
            DXGI_FORMAT_R16G16B16A16_FLOAT,  // HDR preferred
            DXGI_FORMAT_B8G8R8A8_UNORM,      // SDR fallback
        };
        hr = output5_->DuplicateOutput1(device_.Get(), 0,
                                         _countof(formats), formats,
                                         duplication_.GetAddressOf());
        if (SUCCEEDED(hr)) {
            DXGI_OUTDUPL_DESC dup_desc;
            duplication_->GetDesc(&dup_desc);
            capture_format_ = dup_desc.ModeDesc.Format;
            adopt_duplication_geometry(dup_desc);
            // ModeDesc is the DISPLAY MODE, not necessarily what duplication
            // hands us — on an HDR display it says FP16 even when DXGI is
            // converting to BGRA for us.  The first frame settles it; say so
            // here rather than logging a format we may be about to correct.
            log::info(TAG, "DuplicateOutput1: display mode format=%u (%s) — "
                      "confirming against the first frame", capture_format_,
                      capture_format_ == DXGI_FORMAT_R16G16B16A16_FLOAT ? "FP16 HDR" : "BGRA SDR");
        } else {
            log::warn(TAG, "DuplicateOutput1 failed: 0x%08X, falling back", hr);
        }
    } else {
        log::warn(TAG, "IDXGIOutput5 not available: 0x%08X", hr);
    }

    // Fallback to DuplicateOutput (always BGRA)
    if (!duplication_) {
        ComPtr<IDXGIOutput1> output1;
        hr = output.As(&output1);
        if (FAILED(hr)) {
            log::error(TAG, "Failed to get IDXGIOutput1: 0x%08X", hr);
            return false;
        }
        hr = output1->DuplicateOutput(device_.Get(), duplication_.GetAddressOf());
        if (FAILED(hr)) {
            log::error(TAG, "DuplicateOutput failed: 0x%08X. "
                       "Ensure running as desktop app (not UWP) and no other capture active.", hr);
            return false;
        }
        capture_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
        DXGI_OUTDUPL_DESC dup_desc;
        duplication_->GetDesc(&dup_desc);
        adopt_duplication_geometry(dup_desc);
        log::info(TAG, "DuplicateOutput fallback: BGRA SDR");
    }

    return true;
}

void DxgiCapture::note_resolution(uint32_t w, uint32_t h) {
    // Single place that writes resolution_, so nothing can move the capture
    // geometry without the encoder being told to rebuild for it.  The latch
    // must happen here rather than at the end of a successful duplication:
    // during a display reconfiguration DuplicateOutput returns E_ACCESSDENIED
    // for a second or two, and an early return there used to leave the new
    // resolution recorded but the encoder still built for the old one — the
    // host then streamed the new display through a stale encoder (VIV-147).
    if (w == 0 || h == 0) return;
    if (w == resolution_.width && h == resolution_.height) return;
    if (resolution_.width != 0 && resolution_.height != 0) {
        log::info(TAG, "Capture geometry changed %ux%u -> %ux%u",
                  resolution_.width, resolution_.height, w, h);
        geometry_dirty_ = true;
    }
    resolution_.width  = w;
    resolution_.height = h;
}

void DxgiCapture::adopt_duplication_geometry(const DXGI_OUTDUPL_DESC& dup_desc) {
    // DXGI_OUTPUT_DESC::DesktopCoordinates is what the *desktop* thinks the
    // display spans; DXGI_OUTDUPL_DESC::ModeDesc is the size of the texture
    // duplication actually hands us.  They disagree after a mode change the
    // factory hasn't caught up with, and it is the texture size the encoder
    // must be built for — a mismatch is the diagonally-sheared picture.
    // Rotated displays are the exception: there ModeDesc is the unrotated
    // mode, so the desktop rectangle is the honest one.
    if (dup_desc.Rotation != DXGI_MODE_ROTATION_IDENTITY
        && dup_desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        log::warn(TAG, "Captured display is rotated (%u) — using desktop rect %ux%u",
                  (unsigned)dup_desc.Rotation, resolution_.width, resolution_.height);
        return;
    }
    if (dup_desc.ModeDesc.Width == 0 || dup_desc.ModeDesc.Height == 0) return;
    if (dup_desc.ModeDesc.Width != resolution_.width
        || dup_desc.ModeDesc.Height != resolution_.height) {
        log::warn(TAG, "Desktop rect says %ux%u but duplication is %ux%u — trusting duplication",
                  resolution_.width, resolution_.height,
                  dup_desc.ModeDesc.Width, dup_desc.ModeDesc.Height);
        note_resolution(dup_desc.ModeDesc.Width, dup_desc.ModeDesc.Height);
    }
}

bool DxgiCapture::poll_display_change() {
    // Own signature, independent of the one ensure_factory() keeps: a client
    // asking for the monitor list also refreshes the factory, and if that
    // consumed the change we would never push the new list to the clients that
    // did NOT ask.
    std::string layout = desktop_layout_signature();
    const bool layout_moved = !poll_sig_.empty() && layout != poll_sig_;
    poll_sig_ = std::move(layout);

    if (!ensure_factory()) return layout_moved || geometry_dirty_;

    // Re-find the display we are capturing.  A monitor added or removed
    // renumbers EnumOutputs, so trust the GDI device name over the index.
    ComPtr<IDXGIOutput> output;
    uint32_t found_index = monitor_index_;
    bool found = false;
    if (!output_name_.empty()) {
        ComPtr<IDXGIOutput> o;
        for (UINT i = 0; adapter_->EnumOutputs(i, o.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_OUTPUT_DESC d;
            if (SUCCEEDED(o->GetDesc(&d)) && output_name_ == d.DeviceName) {
                output = o;
                found_index = i;
                found = true;
                break;
            }
            o.Reset();
        }
    }
    // If we are on a fallback display because the chosen one was unplugged,
    // go back to the chosen one the moment it reappears — otherwise the user
    // has to re-pick it by hand after every cable wiggle.
    if (found && !wanted_name_.empty() && wanted_name_ != output_name_) {
        ComPtr<IDXGIOutput> o;
        for (UINT i = 0; adapter_->EnumOutputs(i, o.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_OUTPUT_DESC d;
            if (SUCCEEDED(o->GetDesc(&d)) && wanted_name_ == d.DeviceName) {
                log::info(TAG, "Chosen display is back — returning to output %u", i);
                output = o;
                found_index = i;
                break;
            }
            o.Reset();
        }
    }
    if (!found) {
        // Our display is gone (unplugged) — fall back to the first output the
        // refreshed adapter offers so the session keeps streaming something.
        // wanted_name_ is deliberately left pointing at the chosen display so
        // the block above can return to it.
        if (FAILED(adapter_->EnumOutputs(0, output.GetAddressOf()))) {
            log::error(TAG, "poll_display_change: no outputs left");
            return layout_moved || geometry_dirty_;
        }
        found_index = 0;
        log::warn(TAG, "Captured display disappeared — falling back to output 0");
    }

    DXGI_OUTPUT_DESC desc;
    if (FAILED(output->GetDesc(&desc))) return layout_moved || geometry_dirty_;

    const bool moved = !found
        || found_index != monitor_index_
        || desc.DesktopCoordinates.left   != output_rect_.left
        || desc.DesktopCoordinates.top    != output_rect_.top
        || desc.DesktopCoordinates.right  != output_rect_.right
        || desc.DesktopCoordinates.bottom != output_rect_.bottom;

    if (moved) {
        log::info(TAG, "Display configuration changed: capture output %u -> %u, %dx%d",
                  monitor_index_, found_index,
                  desc.DesktopCoordinates.right - desc.DesktopCoordinates.left,
                  desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
        monitor_index_ = found_index;
        // Drop the duplication so it is re-created against the new geometry;
        // init_output_duplication() refreshes resolution_/origin_ with it.
        if (frame_acquired_ && duplication_) {
            duplication_->ReleaseFrame();
            frame_acquired_ = false;
        }
        duplication_.Reset();
        output5_.Reset();
        if (!init_output_duplication(monitor_index_)) {
            // Next capture_frame() retries; report the change either way so
            // the caller re-advertises the (now different) monitor list.
            log::warn(TAG, "Re-duplication after display change failed — retrying on next tick");
        }
        // init_output_duplication() latched geometry_dirty_ if the size moved.
    }

    return layout_moved || moved || geometry_dirty_;
}

bool DxgiCapture::take_geometry_change() {
    const bool v = geometry_dirty_;
    geometry_dirty_ = false;
    return v;
}

bool DxgiCapture::capture_frame(CapturedFrame& frame, uint32_t timeout_ms) {
    // Silently recreate the duplication if it was dropped on a previous tick
    // (exclusive fullscreen enter/exit, UAC, secure-desktop, mode switch).
    // init_output_duplication logs on success, so we stay quiet here on the
    // failure path — next tick will retry.
    if (!duplication_) {
        // Back off between attempts.  While the desktop is being reconfigured
        // (monitor plugged, mode changed, secure desktop) DuplicateOutput
        // returns E_ACCESSDENIED for a second or two, and retrying at the full
        // capture rate floods the log and starves the loop that still has to
        // send heartbeats — long enough for the client to time us out.
        auto now = Clock::now();
        if (last_dup_attempt_.time_since_epoch().count() != 0
            && std::chrono::duration_cast<std::chrono::milliseconds>(
                   now - last_dup_attempt_).count() < DUP_RETRY_MS)
            return false;
        last_dup_attempt_ = now;
        if (!init_output_duplication(monitor_index_)) return false;
    }

    // Release previous frame if held
    if (frame_acquired_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info;
    ComPtr<IDXGIResource> desktop_resource;

    HRESULT hr = duplication_->AcquireNextFrame(
        timeout_ms, &frame_info, desktop_resource.GetAddressOf());

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false; // no new frame
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        log::warn(TAG, "Access lost, will reinit on next tick");
        duplication_.Reset();
        return false;
    }

    if (FAILED(hr)) {
        // Any other failure (observed: DXGI_ERROR_INVALID_CALL 0x887A0001 after
        // language-switcher popup + game minimize) means the duplication handle
        // is permanently broken — drop it so the next tick re-runs init.
        log::error(TAG, "AcquireNextFrame failed: 0x%08X, will reinit on next tick", hr);
        duplication_.Reset();
        return false;
    }

    frame_acquired_ = true;

    // Get the texture — stays in GPU memory
    ComPtr<ID3D11Texture2D> texture;
    hr = desktop_resource.As(&texture);
    if (FAILED(hr)) {
        log::error(TAG, "Failed to get texture from resource: 0x%08X", hr);
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
        return false;
    }

    // First frame off a fresh duplication: believe the texture, not the
    // description.  DXGI_OUTDUPL_DESC::ModeDesc describes the DISPLAY MODE —
    // on an HDR display it reads FP16 even when DuplicateOutput1 was asked
    // for BGRA and is dutifully converting for us.  The encoder is configured
    // from capture_format_, and feeding NVENC/AMF the wrong input format is a
    // whole-frame corruption, so reconcile here and let the platform rebuild
    // (VIV-147).  Same for the dimensions, which cost nothing to re-check.
    if (probe_next_frame_) {
        probe_next_frame_ = false;
        D3D11_TEXTURE2D_DESC td = {};
        texture->GetDesc(&td);
        if (td.Format != capture_format_) {
            log::warn(TAG, "Duplication described format %u but delivers %u — trusting the texture",
                      (unsigned)capture_format_, (unsigned)td.Format);
            capture_format_ = td.Format;
            geometry_dirty_ = true;
        }
        if (td.Width != resolution_.width || td.Height != resolution_.height) {
            log::warn(TAG, "Duplication described %ux%u but delivers %ux%u — trusting the texture",
                      resolution_.width, resolution_.height, td.Width, td.Height);
            note_resolution(td.Width, td.Height);
        }
    }

    // ComPtr operator= AddRef's texture — safe to outlive the local
    // ComPtr here, but the DXGI frame itself is released on next
    // release_frame() call so consumers must copy or process synchronously.
    frame.texture = texture;
    frame.resolution = resolution_;
    frame.frame_index = ++frame_count_;
    frame.capture_time = Clock::now();

    // Desktop image changed if a present occurred or frames accumulated
    frame.content_changed = (frame_info.LastPresentTime.QuadPart != 0) ||
                            (frame_info.AccumulatedFrames > 0);

    // Get dirty rects
    frame.dirty_rects.clear();
    if (frame_info.TotalMetadataBufferSize > 0) {
        UINT buf_size = frame_info.TotalMetadataBufferSize;
        std::vector<BYTE> meta_buf(buf_size);
        UINT move_rects_size = 0;

        // Skip move rects, get dirty rects
        hr = duplication_->GetFrameMoveRects(buf_size, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta_buf.data()), &move_rects_size);
        if (SUCCEEDED(hr)) {
            UINT dirty_rects_size = buf_size - move_rects_size;
            if (dirty_rects_size > 0) {
                std::vector<RECT> rects(dirty_rects_size / sizeof(RECT));
                hr = duplication_->GetFrameDirtyRects(
                    dirty_rects_size, rects.data(), &dirty_rects_size);
                if (SUCCEEDED(hr)) {
                    UINT count = dirty_rects_size / sizeof(RECT);
                    frame.dirty_rects.reserve(count);
                    for (UINT i = 0; i < count; ++i) {
                        frame.dirty_rects.push_back({
                            rects[i].left,
                            rects[i].top,
                            static_cast<uint32_t>(rects[i].right - rects[i].left),
                            static_cast<uint32_t>(rects[i].bottom - rects[i].top)
                        });
                    }
                }
            }
        }
    }

    // Cursor info: DXGI fills PointerPosition only on frames where the
    // cursor changed. Carry the last known state forward so every frame
    // has valid cursor data, otherwise downstream sees "invisible" flicker
    // on idle ticks and the client cursor flickers between the host shape
    // and BlankCursor.
    if (frame_info.LastMouseUpdateTime.QuadPart != 0) {
        sticky_cursor_x_       = frame_info.PointerPosition.Position.x;
        sticky_cursor_y_       = frame_info.PointerPosition.Position.y;
        sticky_cursor_visible_ = frame_info.PointerPosition.Visible != 0;
    }
    frame.cursor.x       = sticky_cursor_x_;
    frame.cursor.y       = sticky_cursor_y_;
    frame.cursor.visible = sticky_cursor_visible_;

    // Cursor shape: DXGI only fills the pointer-shape buffer when the
    // shape actually changed. Fetching it is cheap when it's empty.
    if (frame_info.PointerShapeBufferSize > 0) {
        update_cursor_shape(frame_info.PointerShapeBufferSize);
    }

    return true;
}

void DxgiCapture::update_cursor_shape(UINT buffer_size) {
    if (shape_scratch_.size() < buffer_size) shape_scratch_.resize(buffer_size);

    DXGI_OUTDUPL_POINTER_SHAPE_INFO info = {};
    UINT required = 0;
    HRESULT hr = duplication_->GetFramePointerShape(
        buffer_size, shape_scratch_.data(), &required, &info);
    if (FAILED(hr)) {
        log::warn(TAG, "GetFramePointerShape failed: 0x%08X", hr);
        return;
    }

    // DXGI reports Height doubled for monochrome (AND mask + XOR mask stacked).
    const bool is_mono = (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME);
    const uint32_t out_w = info.Width;
    const uint32_t out_h = is_mono ? (info.Height / 2) : info.Height;
    if (out_w == 0 || out_h == 0 || out_w > 256 || out_h > 256) {
        log::warn(TAG, "Implausible cursor shape %ux%u type=%u — ignored",
                  out_w, out_h, info.Type);
        return;
    }

    CursorShape s;
    s.width     = static_cast<uint16_t>(out_w);
    s.height    = static_cast<uint16_t>(out_h);
    s.hotspot_x = static_cast<uint16_t>(info.HotSpot.x);
    s.hotspot_y = static_cast<uint16_t>(info.HotSpot.y);
    s.bgra.assign(static_cast<size_t>(out_w) * out_h * 4u, 0);

    const uint8_t* src = shape_scratch_.data();
    uint8_t* dst = s.bgra.data();
    const UINT pitch = info.Pitch;

    switch (info.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: {
        // BGRA already. Copy row by row since source pitch may differ from
        // our tight `width*4` layout.
        const uint32_t row_bytes = out_w * 4u;
        for (uint32_t y = 0; y < out_h; ++y) {
            std::memcpy(dst + y * row_bytes, src + y * pitch, row_bytes);
        }
        break;
    }
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
        // Per DXGI spec the alpha byte is *inverted* vs. a normal BGRA:
        //   a == 0x00 → the pixel replaces the desktop (opaque draw).
        //   a == 0xFF → the pixel XORs the desktop (inversion cursor).
        // We can't do XOR on a flat cursor bitmap, so we approximate the
        // XOR region as transparent — for the Windows hand cursor this
        // corresponds to the area *outside* the visible hand, giving a
        // clean cutout instead of a black box.
        for (uint32_t y = 0; y < out_h; ++y) {
            const uint8_t* s_row = src + y * pitch;
            uint8_t* d_row = dst + y * out_w * 4u;
            for (uint32_t x = 0; x < out_w; ++x) {
                uint8_t b = s_row[x*4+0], g = s_row[x*4+1];
                uint8_t r = s_row[x*4+2], a = s_row[x*4+3];
                d_row[x*4+0] = b;
                d_row[x*4+1] = g;
                d_row[x*4+2] = r;
                d_row[x*4+3] = (a == 0) ? 255 : 0;
            }
        }
        break;
    }
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
    default: {
        // 1bpp AND/XOR masks. For each pixel:
        //   AND=0, XOR=0 -> opaque black
        //   AND=0, XOR=1 -> opaque white
        //   AND=1, XOR=0 -> transparent
        //   AND=1, XOR=1 -> inversion (not supported) -> opaque white
        const uint8_t* and_mask = src;
        const uint8_t* xor_mask = src + pitch * out_h;
        for (uint32_t y = 0; y < out_h; ++y) {
            const uint8_t* and_row = and_mask + y * pitch;
            const uint8_t* xor_row = xor_mask + y * pitch;
            uint8_t* d_row = dst + y * out_w * 4u;
            for (uint32_t x = 0; x < out_w; ++x) {
                uint8_t bit    = 0x80u >> (x & 7);
                bool and_bit = (and_row[x >> 3] & bit) != 0;
                bool xor_bit = (xor_row[x >> 3] & bit) != 0;
                uint8_t color = xor_bit ? 255 : 0;
                uint8_t alpha = and_bit ? (xor_bit ? 255 : 0) : 255;
                d_row[x*4+0] = color;
                d_row[x*4+1] = color;
                d_row[x*4+2] = color;
                d_row[x*4+3] = alpha;
            }
        }
        break;
    }
    }

    s.id = ++current_shape_id_;
    pending_shape_ = std::move(s);
    new_shape_pending_ = true;
}

bool DxgiCapture::take_new_cursor_shape(CursorShape& out) {
    if (!new_shape_pending_) return false;
    out = std::move(pending_shape_);
    new_shape_pending_ = false;
    return true;
}

void DxgiCapture::release_frame(CapturedFrame& frame) {
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }
    frame.texture.Reset();
}

std::vector<MonitorInfo> DxgiCapture::enumerate_monitors() {
    std::vector<MonitorInfo> monitors;

    // Through the refreshed factory, so a display plugged in after startup is
    // actually listed (the device's own adapter never sees it — VIV-147).
    if (!ensure_factory()) return monitors;

    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; adapter_->EnumOutputs(i, output.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_OUTPUT_DESC desc;
        output->GetDesc(&desc);

        MonitorInfo info;
        info.index = i;
        // Convert wide string to narrow
        char name_buf[128];
        wcstombs(name_buf, desc.DeviceName, sizeof(name_buf));
        info.name = name_buf;
        info.bounds.x = desc.DesktopCoordinates.left;
        info.bounds.y = desc.DesktopCoordinates.top;
        info.bounds.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        info.bounds.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        info.resolution.width = info.bounds.width;
        info.resolution.height = info.bounds.height;
        // The primary display is the one anchored at the virtual-desktop
        // origin — not necessarily EnumOutputs index 0, and definitely not
        // after a hot-plug renumbers the outputs.
        info.primary = (desc.DesktopCoordinates.left == 0
                     && desc.DesktopCoordinates.top == 0);

        monitors.push_back(std::move(info));
        output.Reset();
    }

    return monitors;
}

Resolution DxgiCapture::get_resolution() const {
    return resolution_;
}

bool DxgiCapture::switch_monitor(uint32_t monitor_index) {
    // Release any held frame and drop the current duplication before moving.
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }
    duplication_.Reset();
    output5_.Reset();
    const uint32_t prev_index = monitor_index_;
    monitor_index_ = monitor_index;
    if (!init_output_duplication(monitor_index)) {
        log::error(TAG, "switch_monitor: failed to duplicate output %u", monitor_index);
        // Put the index back before returning: capture_frame() re-duplicates
        // monitor_index_ on the next tick, and leaving it on the display that
        // just refused would freeze the stream for good rather than keeping
        // the old one alive as the caller expects.
        monitor_index_ = prev_index;
        init_output_duplication(prev_index);
        geometry_dirty_ = false;
        return false;
    }
    wanted_name_ = output_name_;   // the user's new choice
    log::info(TAG, "Switched capture to monitor %u: %ux%u",
              monitor_index, resolution_.width, resolution_.height);
    // The caller rebuilds the encoder for this switch itself — don't leave the
    // flag set or the next hot-plug poll would rebuild it a second time.
    geometry_dirty_ = false;
    return true;
}

// Factory implementation
std::unique_ptr<IScreenCapture> IScreenCapture::create() {
    return std::make_unique<DxgiCapture>();
}

} // namespace vivora

#endif // VIVORA_WINDOWS
