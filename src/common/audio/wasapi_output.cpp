#include "common/audio/wasapi_output.h"
#include "common/utils/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>

#pragma comment(lib, "avrt")
#pragma comment(lib, "ole32")

#include <algorithm>
#include <cstring>

namespace vivora::audio {

static const CLSID kClsidMMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIidIMMDeviceEnumerator =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID kIidIAudioClient =
    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID kIidIAudioRenderClient =
    {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

#define CHK(hr, what)                                        \
    do {                                                     \
        HRESULT _hr = (hr);                                  \
        if (FAILED(_hr)) {                                   \
            log::error("WASAPIOut", "%s failed: 0x%08lx",    \
                       (what), static_cast<long>(_hr));      \
            return false;                                    \
        }                                                    \
    } while (0)

WasapiOutput::WasapiOutput() = default;
WasapiOutput::~WasapiOutput() { stop(); }

bool WasapiOutput::open_endpoint() {
    IMMDeviceEnumerator* enumerator = nullptr;
    CHK(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                         kIidIMMDeviceEnumerator,
                         reinterpret_cast<void**>(&enumerator)),
        "CoCreateInstance(MMDeviceEnumerator)");

    CHK(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_),
        "GetDefaultAudioEndpoint");
    enumerator->Release();

    CHK(device_->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&client_)),
        "IMMDevice::Activate");

    WAVEFORMATEX* mix = nullptr;
    CHK(client_->GetMixFormat(&mix), "GetMixFormat");

    uint32_t new_rate  = mix->nSamplesPerSec;
    uint16_t new_chans = mix->nChannels;

    const REFERENCE_TIME buffer_duration = 300000; // 30 ms
    HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     buffer_duration, 0, mix, nullptr);
    CoTaskMemFree(mix);
    CHK(hr, "IAudioClient::Initialize");

    UINT32 ep_frames = 0;
    CHK(client_->GetBufferSize(&ep_frames), "GetBufferSize");
    endpoint_frames_ = ep_frames;

    CHK(client_->SetEventHandle(static_cast<HANDLE>(event_)), "SetEventHandle");

    CHK(client_->GetService(kIidIAudioRenderClient,
                            reinterpret_cast<void**>(&render_)),
        "GetService(IAudioRenderClient)");

    {
        std::lock_guard<std::mutex> lk(ring_mu_);
        sample_rate_ = new_rate;
        channels_    = new_chans;
        // Ring buffer sized for ~80ms of audio at device rate.
        ring_size_frames_ = sample_rate_ * 80 / 1000;
        ring_.assign(ring_size_frames_ * channels_, 0.0f);
        ring_read_  = 0;
        ring_write_ = 0;
    }

    CHK(client_->Start(), "IAudioClient::Start");
    log::info("WASAPIOut", "endpoint opened: %u Hz, %u ch, endpoint=%u frames",
              sample_rate_, channels_, endpoint_frames_);
    return true;
}

void WasapiOutput::close_endpoint() {
    if (client_) { client_->Stop(); client_->Release(); client_ = nullptr; }
    if (render_) { render_->Release();                  render_ = nullptr; }
    if (device_) { device_->Release();                  device_ = nullptr; }
}

bool WasapiOutput::start(uint32_t /*requested_rate*/, uint16_t /*requested_ch*/) {
    if (running_.load()) return true;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        log::error("WASAPIOut", "CreateEvent failed");
        return false;
    }

    if (!open_endpoint()) {
        close_endpoint();
        CloseHandle(static_cast<HANDLE>(event_));
        event_ = nullptr;
        return false;
    }

    running_.store(true);
    worker_ = std::thread(&WasapiOutput::thread_proc, this);
    return true;
}

void WasapiOutput::stop() {
    if (!running_.load() && !worker_.joinable()) return;
    running_.store(false);
    if (event_) SetEvent(static_cast<HANDLE>(event_));
    if (worker_.joinable()) worker_.join();

    close_endpoint();
    if (event_)  { CloseHandle(static_cast<HANDLE>(event_)); event_ = nullptr; }
}

bool WasapiOutput::poll_device_change(uint32_t& new_rate, uint16_t& new_channels) {
    std::lock_guard<std::mutex> lk(ring_mu_);
    if (!device_changed_) return false;
    device_changed_ = false;
    new_rate     = sample_rate_;
    new_channels = channels_;
    return true;
}

uint32_t WasapiOutput::write(const float* samples, uint32_t frames) {
    if (!running_.load() || !samples || frames == 0) return 0;
    std::lock_guard<std::mutex> lk(ring_mu_);

    // Available frames to write without overrun.
    size_t used;
    if (ring_write_ >= ring_read_) used = ring_write_ - ring_read_;
    else                           used = ring_size_frames_ - (ring_read_ - ring_write_);
    size_t free_frames = ring_size_frames_ - used - 1; // leave one slot

    uint32_t to_write = static_cast<uint32_t>(
        std::min<size_t>(frames, free_frames));

    for (uint32_t i = 0; i < to_write; ++i) {
        size_t idx = ring_write_ * channels_;
        for (uint16_t c = 0; c < channels_; ++c) {
            ring_[idx + c] = samples[i * channels_ + c];
        }
        ring_write_ = (ring_write_ + 1) % ring_size_frames_;
    }
    return to_write;
}

void WasapiOutput::thread_proc() {
    // COM must be initialised on THIS thread: try_reopen()/open_endpoint() call
    // CoCreateInstance() on the worker during device-invalidation recovery, and
    // without a per-thread CoInitializeEx that fails with CO_E_NOTINITIALIZED,
    // permanently breaking playback after a device change (VIV-94).
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Every successful call — including S_FALSE — must be balanced at exit.
    const bool com_owned = SUCCEEDED(com);

    DWORD task_index = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    auto try_reopen = [this]() -> bool {
        uint32_t old_rate = sample_rate_;
        uint16_t old_ch   = channels_;
        close_endpoint();
        if (!open_endpoint()) return false;
        if (sample_rate_ != old_rate || channels_ != old_ch) {
            std::lock_guard<std::mutex> lk(ring_mu_);
            device_changed_ = true;
        }
        return true;
    };

    while (running_.load()) {
        DWORD wr = WaitForSingleObject(static_cast<HANDLE>(event_), 200);
        if (!running_.load()) break;
        if (wr != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        HRESULT hr_pad = client_->GetCurrentPadding(&padding);
        if (hr_pad == AUDCLNT_E_DEVICE_INVALIDATED) {
            log::warn("WASAPIOut", "default render endpoint invalidated — reopening");
            if (!try_reopen()) {
                log::error("WASAPIOut", "reopen after invalidation failed — stopping output");
                break;
            }
            continue;
        }
        if (FAILED(hr_pad)) continue;
        UINT32 avail = endpoint_frames_ - padding;
        if (avail == 0) continue;

        BYTE* out_bytes = nullptr;
        HRESULT hr_buf = render_->GetBuffer(avail, &out_bytes);
        if (hr_buf == AUDCLNT_E_DEVICE_INVALIDATED) {
            log::warn("WASAPIOut", "GetBuffer reports device invalidated — reopening");
            if (!try_reopen()) {
                log::error("WASAPIOut", "reopen after invalidation failed — stopping output");
                break;
            }
            continue;
        }
        if (FAILED(hr_buf)) continue;

        float* out = reinterpret_cast<float*>(out_bytes);
        uint32_t written = 0;
        {
            std::lock_guard<std::mutex> lk(ring_mu_);
            while (written < avail && ring_read_ != ring_write_) {
                size_t idx = ring_read_ * channels_;
                for (uint16_t c = 0; c < channels_; ++c) {
                    out[written * channels_ + c] = ring_[idx + c];
                }
                ring_read_ = (ring_read_ + 1) % ring_size_frames_;
                written++;
            }
        }
        DWORD flags = 0;
        if (written < avail) {
            // Underrun — fill remainder with silence.
            std::memset(out + written * channels_, 0,
                        (avail - written) * channels_ * sizeof(float));
        }
        render_->ReleaseBuffer(avail, flags);
    }

    if (mm) AvRevertMmThreadCharacteristics(mm);
    if (com_owned) CoUninitialize();
}

std::unique_ptr<AudioOutput> create_default_audio_output() {
    return std::make_unique<WasapiOutput>();
}

} // namespace vivora::audio
