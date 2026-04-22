#include "common/audio/wasapi_loopback.h"
#include "common/utils/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#pragma comment(lib, "avrt")
#pragma comment(lib, "ole32")

#include <cstring>
#include <vector>

namespace deskbeam::audio {

static const CLSID kClsidMMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIidIMMDeviceEnumerator =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID kIidIAudioClient =
    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID kIidIAudioCaptureClient =
    {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};

#define CHK(hr, what)                                        \
    do {                                                     \
        HRESULT _hr = (hr);                                  \
        if (FAILED(_hr)) {                                   \
            log::error("WASAPICap", "%s failed: 0x%08lx",    \
                       (what), static_cast<long>(_hr));      \
            return false;                                    \
        }                                                    \
    } while (0)

WasapiLoopbackCapture::WasapiLoopbackCapture() = default;

WasapiLoopbackCapture::~WasapiLoopbackCapture() { stop(); }

bool WasapiLoopbackCapture::open_endpoint() {
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

    sample_rate_     = mix->nSamplesPerSec;
    channels_        = mix->nChannels;
    bits_per_sample_ = mix->wBitsPerSample;

    WORD fmt_tag = mix->wFormatTag;
    if (fmt_tag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* ext =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
        is_float_ = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    } else {
        is_float_ = (fmt_tag == WAVE_FORMAT_IEEE_FLOAT);
    }

    const REFERENCE_TIME buffer_duration = 200000; // 20 ms low-latency buffer

    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK |
                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                     buffer_duration, 0, mix, nullptr);
    CoTaskMemFree(mix);
    CHK(hr, "IAudioClient::Initialize");

    CHK(client_->SetEventHandle(static_cast<HANDLE>(event_)),
        "SetEventHandle");

    CHK(client_->GetService(kIidIAudioCaptureClient,
                            reinterpret_cast<void**>(&capture_)),
        "GetService(IAudioCaptureClient)");

    CHK(client_->Start(), "IAudioClient::Start");

    log::info("WASAPICap", "loopback endpoint opened: %u Hz, %u ch, %u bps, %s",
              sample_rate_, channels_, bits_per_sample_,
              is_float_ ? "float" : "int");
    return true;
}

void WasapiLoopbackCapture::close_endpoint() {
    if (client_)  { client_->Stop(); client_->Release();  client_  = nullptr; }
    if (capture_) { capture_->Release();                  capture_ = nullptr; }
    if (device_)  { device_->Release();                   device_  = nullptr; }
}

bool WasapiLoopbackCapture::start(AudioCaptureCallback cb) {
    if (running_.load()) return true;
    cb_ = std::move(cb);

    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // S_FALSE means already initialised on this thread — still OK.
    (void)co;

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        log::error("WASAPICap", "CreateEvent failed");
        return false;
    }

    if (!open_endpoint()) {
        close_endpoint();
        CloseHandle(static_cast<HANDLE>(event_));
        event_ = nullptr;
        return false;
    }

    running_.store(true);
    worker_ = std::thread(&WasapiLoopbackCapture::thread_proc, this);
    return true;
}

void WasapiLoopbackCapture::stop() {
    if (!running_.load() && !worker_.joinable()) return;
    running_.store(false);
    if (event_) SetEvent(static_cast<HANDLE>(event_));
    if (worker_.joinable()) worker_.join();

    close_endpoint();
    if (event_)   { CloseHandle(static_cast<HANDLE>(event_)); event_ = nullptr; }
}

void WasapiLoopbackCapture::thread_proc() {
    // Raise thread priority for audio work.
    DWORD task_index = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    std::vector<float> tmp;
    uint64_t callbacks = 0;
    uint64_t silent_callbacks = 0;
    uint64_t last_log_callbacks = 0;
    auto last_log = std::chrono::steady_clock::now();

    while (running_.load()) {
        DWORD wr = WaitForSingleObject(static_cast<HANDLE>(event_), 200);
        if (!running_.load()) break;
        if (wr == WAIT_TIMEOUT) {
            // No audio playing — WASAPI loopback delivers nothing when idle.
            // Don't inject silence here (timer granularity ruins cadence).
            // Client jitter buffer handles gaps via resync.
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - last_log).count() >= 5.0) {
                log::info("WASAPICap", "idle (no audio playing), callbacks=%llu, silent=%llu",
                          (unsigned long long)callbacks, (unsigned long long)silent_callbacks);
                last_log = now;
            }
            continue;
        }
        if (wr != WAIT_OBJECT_0) continue;

        UINT32 packet_frames = 0;
        HRESULT hr_next = capture_->GetNextPacketSize(&packet_frames);
        if (hr_next == AUDCLNT_E_DEVICE_INVALIDATED) {
            log::warn("WASAPICap", "default render endpoint invalidated — reopening");
            close_endpoint();
            if (!open_endpoint()) {
                log::error("WASAPICap", "reopen after invalidation failed — stopping capture");
                break;
            }
            continue;
        }
        if (FAILED(hr_next)) {
            log::error("WASAPICap", "GetNextPacketSize failed: 0x%08lx — stopping capture",
                       static_cast<long>(hr_next));
            break;
        }
        while (packet_frames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            HRESULT hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                log::warn("WASAPICap", "GetBuffer reports device invalidated — reopening");
                close_endpoint();
                if (!open_endpoint()) {
                    log::error("WASAPICap", "reopen after invalidation failed — stopping capture");
                    goto done;
                }
                break; // exit inner loop, continue outer event wait
            }
            if (FAILED(hr)) {
                log::error("WASAPICap", "GetBuffer failed: 0x%08lx — stopping capture",
                           static_cast<long>(hr));
                goto done;
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                silent_callbacks++;
            }

            tmp.assign(static_cast<size_t>(frames) * channels_, 0.0f);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                if (is_float_ && bits_per_sample_ == 32) {
                    std::memcpy(tmp.data(), data, tmp.size() * sizeof(float));
                } else if (!is_float_ && bits_per_sample_ == 16) {
                    const int16_t* in = reinterpret_cast<const int16_t*>(data);
                    for (size_t i = 0; i < tmp.size(); ++i) {
                        tmp[i] = static_cast<float>(in[i]) / 32768.0f;
                    }
                } else if (!is_float_ && bits_per_sample_ == 32) {
                    const int32_t* in = reinterpret_cast<const int32_t*>(data);
                    for (size_t i = 0; i < tmp.size(); ++i) {
                        tmp[i] = static_cast<float>(in[i]) / 2147483648.0f;
                    }
                }
            }
            if (cb_) cb_(tmp.data(), frames, sample_rate_, channels_);
            callbacks++;

            capture_->ReleaseBuffer(frames);

            hr_next = capture_->GetNextPacketSize(&packet_frames);
            if (hr_next == AUDCLNT_E_DEVICE_INVALIDATED) {
                log::warn("WASAPICap", "GetNextPacketSize reports device invalidated — reopening");
                close_endpoint();
                if (!open_endpoint()) {
                    log::error("WASAPICap", "reopen after invalidation failed — stopping capture");
                    goto done;
                }
                break;
            }
            if (FAILED(hr_next)) {
                log::error("WASAPICap", "GetNextPacketSize failed: 0x%08lx — stopping capture",
                           static_cast<long>(hr_next));
                goto done;
            }
        }

        // Periodic stats every ~5 seconds.
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_log).count() >= 5.0) {
            uint64_t delta = callbacks - last_log_callbacks;
            log::info("WASAPICap", "stats: callbacks=%llu(+%llu), silent=%llu",
                      (unsigned long long)callbacks, (unsigned long long)delta,
                      (unsigned long long)silent_callbacks);
            last_log_callbacks = callbacks;
            last_log = now;
        }
    }

done:
    if (mm) AvRevertMmThreadCharacteristics(mm);
}

std::unique_ptr<AudioCapture> create_default_loopback_capture() {
    return std::make_unique<WasapiLoopbackCapture>();
}

} // namespace deskbeam::audio
