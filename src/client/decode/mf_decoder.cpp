#ifdef DESKBEAM_WINDOWS

#include "client/decode/mf_decoder.h"
#include "common/utils/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <d3d11.h>
#include <codecapi.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")

namespace deskbeam {

// Will find HEVC decoder via MFTEnum

MfDecoder::~MfDecoder() {
    if (transform_) {
        transform_->Release();
        transform_ = nullptr;
    }
    if (device_manager_) {
        device_manager_->Release();
        device_manager_ = nullptr;
    }
    MFShutdown();
}

bool MfDecoder::init(ID3D11Device* device) {
    // COM may already be initialized by Qt (STA). Accept either mode.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
            log::error("DECODE", "CoInitializeEx failed: 0x%08X", hr);
            return false;
        }
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        log::error("DECODE", "MFStartup failed: 0x%08X", hr);
        return false;
    }

    if (device) {
        device_.Attach(device);
        device_->AddRef(); // we took a raw ptr, need our own ref
        device_->GetImmediateContext(context_.GetAddressOf());
    } else {
        if (!create_device()) return false;
    }

    if (!create_decoder()) return false;

    log::info("DECODE", "Media Foundation HEVC decoder initialized (D3D11VA)");
    return true;
}

bool MfDecoder::create_device() {
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, 2, D3D11_SDK_VERSION,
        device_.GetAddressOf(), nullptr, context_.GetAddressOf());

    if (FAILED(hr)) {
        log::error("DECODE", "D3D11CreateDevice failed: 0x%08X", hr);
        return false;
    }

    // Enable multi-threaded protection (required for MF D3D11)
    ComPtr<ID3D10Multithread> mt;
    hr = device_.As(&mt);
    if (SUCCEEDED(hr)) mt->SetMultithreadProtected(TRUE);

    return true;
}

bool MfDecoder::create_decoder() {
    // Create DXGI Device Manager for hardware decoding
    HRESULT hr = MFCreateDXGIDeviceManager(&reset_token_, &device_manager_);
    if (FAILED(hr)) {
        log::error("DECODE", "MFCreateDXGIDeviceManager failed: 0x%08X", hr);
        return false;
    }
    hr = device_manager_->ResetDevice(device_.Get(), reset_token_);
    if (FAILED(hr)) {
        log::error("DECODE", "ResetDevice failed: 0x%08X", hr);
        return false;
    }

    // Find HEVC decoder MFT via MFTEnum
    {
        MFT_REGISTER_TYPE_INFO input_info = { MFMediaType_Video, MFVideoFormat_HEVC };
        IMFActivate** activates = nullptr;
        UINT32 count = 0;

        // Use synchronous MFTs only (software + hardware that support sync)
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                       MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                       &input_info, nullptr, &activates, &count);
        if (FAILED(hr) || count == 0) {
            log::error("DECODE", "No HEVC decoder found (install HEVC Video Extensions?)");
            return false;
        }

        // Log all found decoders
        for (UINT32 i = 0; i < count; ++i) {
            WCHAR* name = nullptr;
            UINT32 name_len = 0;
            activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &name_len);
            if (name) {
                char name_a[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, name, -1, name_a, sizeof(name_a), nullptr, nullptr);
                log::info("DECODE", "  [%u] %s", i, name_a);
                CoTaskMemFree(name);
            }
        }

        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&transform_));
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        if (FAILED(hr)) {
            log::error("DECODE", "ActivateObject HEVC decoder failed: 0x%08X", hr);
            return false;
        }
        log::info("DECODE", "Using HEVC decoder #0 of %u", count);
    }

    // Check if MFT is async and unlock it if needed
    {
        IMFAttributes* attrs = nullptr;
        hr = transform_->GetAttributes(&attrs);
        if (SUCCEEDED(hr) && attrs) {
            UINT32 is_async = FALSE;
            attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
            if (is_async) {
                log::info("DECODE", "MFT is async, unlocking...");
                hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                log::info("DECODE", "Async unlock: 0x%08X", hr);
            } else {
                log::info("DECODE", "MFT is synchronous");
            }
            attrs->Release();
        }
    }

    // Set D3D11 device manager — required for HEVCVideoExtension
    hr = transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                     reinterpret_cast<ULONG_PTR>(device_manager_));
    if (FAILED(hr)) {
        log::warn("DECODE", "SET_D3D_MANAGER failed: 0x%08X", hr);
    } else {
        log::info("DECODE", "D3D11VA device manager set");
    }

    // Enable low-latency mode
    IMFAttributes* attrs = nullptr;
    hr = transform_->GetAttributes(&attrs);
    if (SUCCEEDED(hr) && attrs) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        attrs->Release();
        log::info("DECODE", "Low-latency mode enabled");
    }

    // Set input type: HEVC
    IMFMediaType* input_type = nullptr;
    MFCreateMediaType(&input_type);
    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC);
    // Provide nominal resolution (will be updated from bitstream)
    MFSetAttributeSize(input_type, MF_MT_FRAME_SIZE, 3840, 2160);
    MFSetAttributeRatio(input_type, MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(input_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = transform_->SetInputType(0, input_type, 0);
    input_type->Release();

    if (FAILED(hr)) {
        log::error("DECODE", "SetInputType HEVC failed: 0x%08X", hr);
        return false;
    }
    log::info("DECODE", "Input type set: HEVC");

    // Configure output type
    if (!configure_output()) {
        log::warn("DECODE", "Output type not available yet (will set on stream change)");
    }

    return true;
}

bool MfDecoder::configure_output() {
    // Enumerate output types
    for (DWORD i = 0; ; ++i) {
        IMFMediaType* out_type = nullptr;
        HRESULT hr = transform_->GetOutputAvailableType(0, i, &out_type);
        if (FAILED(hr)) break;

        GUID subtype = {};
        out_type->GetGUID(MF_MT_SUBTYPE, &subtype);
        // Accept NV12 (8-bit) or P010 (10-bit), or just take the first one
        if (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_P010 || i == 0) {
            hr = transform_->SetOutputType(0, out_type, 0);
            out_type->Release();
            if (SUCCEEDED(hr)) {
                log::info("DECODE", "Output format: %s",
                    subtype == MFVideoFormat_P010 ? "P010 (10-bit)" :
                    subtype == MFVideoFormat_NV12 ? "NV12 (8-bit)" : "other");
                return true;
            }
        } else {
            out_type->Release();
        }
    }
    return false;
}

// SEH-guarded leaf wrappers. These functions have NO C++ objects requiring
// destructor unwinding — that's why they can safely use __try/__except.
// Each returns true on success, false if the MF call access-violated.

static bool seh_process_input(IMFTransform* mft, IMFSample* sample, HRESULT* out_hr) {
    __try {
        *out_hr = mft->ProcessInput(0, sample, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool seh_process_output(IMFTransform* mft, MFT_OUTPUT_DATA_BUFFER* out,
                               DWORD* status, HRESULT* out_hr) {
    __try {
        *out_hr = mft->ProcessOutput(0, 1, out, status);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool seh_process_message(IMFTransform* mft, MFT_MESSAGE_TYPE msg, ULONG_PTR param) {
    __try {
        mft->ProcessMessage(msg, param);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void MfDecoder::flush() {
    if (!transform_ || failed_) return;

    // Drain any pending output frames and release their textures.
    drain_output();
    while (!output_frames_.empty()) {
        auto& f = output_frames_.front();
        if (f.texture) f.texture->Release();
        output_frames_.pop();
    }

    // Tell the MFT to drop all buffered data and reset reference pictures.
    if (!seh_process_message(transform_, MFT_MESSAGE_COMMAND_FLUSH, 0)) {
        log::error("DECODE", "SEH in FLUSH ProcessMessage — marking decoder dead");
        failed_ = true;
        return;
    }
    log::info("DECODE", "Decoder flushed (IDR reset)");
}

bool MfDecoder::decode(const uint8_t* data, size_t len, uint64_t pts) {
    if (!transform_ || failed_) return false;
    if (len == 0) return false;

    if (!started_) {
        started_ = true;
    }

    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(len), &buffer);
    if (FAILED(hr)) return false;

    BYTE* buf_ptr = nullptr;
    hr = buffer->Lock(&buf_ptr, nullptr, nullptr);
    if (FAILED(hr)) { buffer->Release(); return false; }
    memcpy(buf_ptr, data, len);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(len));

    IMFSample* sample = nullptr;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) { buffer->Release(); return false; }

    sample->AddBuffer(buffer);
    sample->SetSampleTime(static_cast<LONGLONG>(pts) * 10);
    buffer->Release();

    if (!seh_process_input(transform_, sample, &hr)) {
        log::error("DECODE", "SEH in ProcessInput — marking decoder dead");
        sample->Release();
        failed_ = true;
        return false;
    }

    if (hr == MF_E_NOTACCEPTING) {
        drain_output();
        if (failed_) { sample->Release(); return false; }
        if (!seh_process_input(transform_, sample, &hr)) {
            log::error("DECODE", "SEH in ProcessInput retry — marking decoder dead");
            sample->Release();
            failed_ = true;
            return false;
        }
    }

    sample->Release();

    if (FAILED(hr)) {
        log::error("DECODE", "ProcessInput failed: 0x%08X", hr);
        return false;
    }

    drain_output();
    return true;
}

void MfDecoder::drain_output() {
    if (!transform_ || failed_) return;

    for (;;) {
        MFT_OUTPUT_DATA_BUFFER output = {};
        DWORD status = 0;

        HRESULT hr = S_OK;
        if (!seh_process_output(transform_, &output, &status, &hr)) {
            log::error("DECODE", "SEH in ProcessOutput — marking decoder dead");
            failed_ = true;
            return;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            log::info("DECODE", "Stream format change, reconfiguring output");
            configure_output();
            continue;
        }

        if (FAILED(hr)) {
            log::debug("DECODE", "ProcessOutput: 0x%08X", hr);
            if (output.pSample) output.pSample->Release();
            if (output.pEvents) output.pEvents->Release();
            break;
        }

        if (!output.pSample) {
            if (output.pEvents) output.pEvents->Release();
            continue;
        }

        // Extract D3D11 texture from output sample
        IMFMediaBuffer* media_buf = nullptr;
        hr = output.pSample->GetBufferByIndex(0, &media_buf);
        if (SUCCEEDED(hr)) {
            IMFDXGIBuffer* dxgi_buf = nullptr;
            hr = media_buf->QueryInterface(IID_PPV_ARGS(&dxgi_buf));
            if (SUCCEEDED(hr)) {
                ID3D11Texture2D* tex = nullptr;
                UINT subresource = 0;
                dxgi_buf->GetResource(IID_PPV_ARGS(&tex));
                dxgi_buf->GetSubresourceIndex(&subresource);

                if (tex) {
                    D3D11_TEXTURE2D_DESC desc;
                    tex->GetDesc(&desc);

                    LONGLONG sample_time = 0;
                    output.pSample->GetSampleTime(&sample_time);

                    DecodedFrame frame;
                    frame.texture = tex;
                    frame.subresource = subresource;
                    frame.width = desc.Width;
                    frame.height = desc.Height;
                    frame.pts = static_cast<uint64_t>(sample_time / 10);

                    output_frames_.push(frame);
                    // Note: tex reference is held by the queue consumer
                    // who must Release() after use
                }
                dxgi_buf->Release();
            }
            media_buf->Release();
        }

        output.pSample->Release();
        if (output.pEvents) output.pEvents->Release();
    }
}

bool MfDecoder::get_frame(DecodedFrame& frame) {
    if (output_frames_.empty()) return false;
    frame = output_frames_.front();
    output_frames_.pop();
    return true;
}

// Factory
std::unique_ptr<IVideoDecoder> IVideoDecoder::create() {
    return std::make_unique<MfDecoder>();
}

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
