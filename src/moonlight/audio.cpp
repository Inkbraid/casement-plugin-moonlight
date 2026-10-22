#include "moonlight/audio.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <opus/opus_multistream.h>

#include <Limelight.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "casement/log.h"
#include "casement/text.h"
#include "moonlight/hresult.h"

namespace moonlight {

namespace {

using clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// The COM identities used here, spelled out so no GUID library is needed.
constexpr GUID clsid_device_enumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
constexpr GUID iid_device_enumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
constexpr GUID iid_audio_client = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
constexpr GUID iid_render_client = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
constexpr GUID iid_notification_client = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};
constexpr GUID iid_unknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID subtype_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID subtype_pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

/// The device buffer, and how much of it may fill before packets are
/// dropped to keep the delay down.
constexpr REFERENCE_TIME buffer_duration = 100 * 10000;  // 100 ms in 100 ns units
constexpr int max_queued_ms = 60;

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

/// Notes when the user picks another default output device.
class DeviceWatcher final : public IMMNotificationClient {
public:
    std::atomic<bool> changed{false};

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (IsEqualIID(riid, iid_unknown) || IsEqualIID(riid, iid_notification_client)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) changed = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<ULONG> refs_{1};
};

/// Linear interpolation from the stream's rate to a device that insists on
/// its own (only when Windows can't convert for us).
class Resampler {
public:
    void reset(int channels, int in_rate, int out_rate) {
        channels_ = channels;
        step_ = static_cast<double>(in_rate) / out_rate;
        pos_ = 0;
        last_.assign(static_cast<std::size_t>(channels), 0.0f);
    }
    void process(const float* in, int frames, std::vector<float>& out) {
        out.clear();
        if (frames <= 0) return;
        for (;;) {
            int i = static_cast<int>(std::floor(pos_));
            if (i + 1 >= frames) break;
            auto f = static_cast<float>(pos_ - i);
            for (int c = 0; c < channels_; ++c) {
                float a = i < 0 ? last_[c] : in[i * channels_ + c];
                float b = in[(i + 1) * channels_ + c];
                out.push_back(a + (b - a) * f);
            }
            pos_ += step_;
        }
        pos_ -= frames;
        std::copy_n(in + std::size_t(frames - 1) * channels_, channels_, last_.begin());
    }

private:
    int channels_ = 2;
    double step_ = 1;
    double pos_ = 0;
    std::vector<float> last_;
};

}  // namespace

struct AudioPlayer::Impl {
    OpusMSDecoder* decoder = nullptr;
    int channels = 2;
    int rate = 48000;
    int samples_per_frame = 240;
    std::vector<float> pcm;
    std::vector<float> converted;

    // Owned by the audio thread.
    bool com = false;
    IMMDeviceEnumerator* enumerator = nullptr;
    DeviceWatcher* watcher = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    UINT32 buffer_frames = 0;
    int device_channels = 2;
    int device_rate = 48000;
    enum class Sample { f32, s16, s32 } sample = Sample::f32;
    bool convert_rate = false;
    Resampler resampler;
    bool started = false;
    bool paused = false;
    bool warned = false;
    clock::time_point next_attempt{};
    std::atomic<bool> muted{false};
    // For the log when the stream ends.
    std::uint64_t packets = 0;
    std::uint64_t played = 0;
    float peak = 0;

    void close_device() {
        if (client && started) client->Stop();
        started = false;
        release(render);
        release(client);
    }

    bool open_device() {
        if (!enumerator) {
            HRESULT hr = CoCreateInstance(clsid_device_enumerator, nullptr, CLSCTX_ALL, iid_device_enumerator,
                                          reinterpret_cast<void**>(&enumerator));
            if (FAILED(hr)) return fail("no audio device enumerator", hr);
            watcher = new DeviceWatcher();
            enumerator->RegisterEndpointNotificationCallback(watcher);
        }
        IMMDevice* device = nullptr;
        HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) return fail("no default audio output", hr);
        hr = device->Activate(iid_audio_client, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        if (FAILED(hr)) {
            device->Release();
            return fail("can't open the audio output", hr);
        }

        // Ask for the stream's own format and let Windows convert.
        WAVEFORMATEXTENSIBLE want{};
        want.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        want.Format.nChannels = static_cast<WORD>(channels);
        want.Format.nSamplesPerSec = static_cast<DWORD>(rate);
        want.Format.wBitsPerSample = 32;
        want.Format.nBlockAlign = static_cast<WORD>(channels * 4);
        want.Format.nAvgBytesPerSec = static_cast<DWORD>(rate * channels * 4);
        want.Format.cbSize = static_cast<WORD>(sizeof(want) - sizeof(WAVEFORMATEX));
        want.Samples.wValidBitsPerSample = 32;
        want.dwChannelMask = channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
        want.SubFormat = subtype_float;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                buffer_duration, 0, &want.Format, nullptr);
        if (SUCCEEDED(hr)) {
            device_channels = channels;
            device_rate = rate;
            sample = Sample::f32;
            convert_rate = false;
        } else {
            // Then the mix format, converted here.
            release(client);
            hr = device->Activate(iid_audio_client, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
            if (SUCCEEDED(hr)) {
                device_channels = mix->nChannels;
                device_rate = static_cast<int>(mix->nSamplesPerSec);
                bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                                (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                 IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat, subtype_float));
                bool is_pcm = mix->wFormatTag == WAVE_FORMAT_PCM ||
                              (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                               IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat, subtype_pcm));
                if (is_float && mix->wBitsPerSample == 32) sample = Sample::f32;
                else if (is_pcm && mix->wBitsPerSample == 16) sample = Sample::s16;
                else if (is_pcm && mix->wBitsPerSample == 32) sample = Sample::s32;
                else hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
                if (SUCCEEDED(hr))
                    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, buffer_duration, 0, mix, nullptr);
                CoTaskMemFree(mix);
            }
            convert_rate = device_rate != rate;
            if (convert_rate) resampler.reset(channels, rate, device_rate);
        }
        device->Release();
        if (FAILED(hr)) {
            release(client);
            return fail("can't start the audio output", hr);
        }
        if (FAILED(hr = client->GetBufferSize(&buffer_frames)) ||
            FAILED(hr = client->GetService(iid_render_client, reinterpret_cast<void**>(&render)))) {
            close_device();
            return fail("can't start the audio output", hr);
        }
        casement::log::info("audio: {} Hz, {} channels on the default output{}", device_rate, device_channels,
                            convert_rate ? " (resampled)" : "");
        warned = false;
        return true;
    }

    bool fail(const char* what, HRESULT hr) {
        if (!warned) casement::log::warn("{}: {}", what, hresult_text(hr));
        warned = true;
        return false;
    }

    bool ensure_device() {
        if (!com) {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            com = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        }
        if (watcher && watcher->changed.exchange(false)) {
            casement::log::info("audio: the default output changed");
            close_device();
            next_attempt = {};
        }
        if (client) return true;
        auto now = clock::now();
        if (now < next_attempt) return false;
        // Without a device, try again now and then rather than per packet.
        next_attempt = now + 2s;
        return open_device();
    }

    void write(const float* data, int frames) {
        const float* in = data;
        if (convert_rate) {
            resampler.process(data, frames, converted);
            in = converted.data();
            frames = static_cast<int>(converted.size() / static_cast<std::size_t>(channels));
        }
        UINT32 padding = 0;
        HRESULT hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr)) return lost(hr);
        // Drop rather than let the delay grow (clock drift, a stalled device).
        if (padding > static_cast<UINT32>(device_rate * max_queued_ms / 1000)) return;
        auto count = std::min<UINT32>(static_cast<UINT32>(frames), buffer_frames - padding);
        if (count == 0) return;
        BYTE* out = nullptr;
        if (FAILED(hr = render->GetBuffer(count, &out))) return lost(hr);
        for (UINT32 i = 0; i < count; ++i) {
            for (int c = 0; c < device_channels; ++c) {
                // Stereo into the first two channels; mono gets the average.
                float v = 0;
                if (device_channels == 1)
                    v = channels >= 2 ? (in[i * channels] + in[i * channels + 1]) / 2 : in[i * channels];
                else if (c < channels) v = in[i * channels + c];
                v = std::clamp(v, -1.0f, 1.0f);
                std::size_t k = std::size_t(i) * device_channels + c;
                switch (sample) {
                    case Sample::f32: reinterpret_cast<float*>(out)[k] = v; break;
                    case Sample::s16:
                        reinterpret_cast<std::int16_t*>(out)[k] = static_cast<std::int16_t>(v * 32767);
                        break;
                    case Sample::s32:
                        reinterpret_cast<std::int32_t*>(out)[k] = static_cast<std::int32_t>(v * 2147483520.0f);
                        break;
                }
            }
        }
        render->ReleaseBuffer(count, 0);
        played += count;
    }

    void lost(HRESULT hr) {
        casement::log::warn("audio output lost: {}", hresult_text(hr));
        close_device();
        next_attempt = {};
    }
};

AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}

AudioPlayer::~AudioPlayer() { cleanup(); }

int AudioPlayer::init(const _OPUS_MULTISTREAM_CONFIGURATION& config) {
    Impl& d = *impl_;
    int err = 0;
    d.channels = config.channelCount;
    d.rate = config.sampleRate;
    d.samples_per_frame = config.samplesPerFrame;
    d.decoder = opus_multistream_decoder_create(config.sampleRate, config.channelCount, config.streams,
                                                config.coupledStreams, config.mapping, &err);
    if (!d.decoder) {
        casement::log::error("can't start the Opus decoder: {}", opus_strerror(err));
        return -1;
    }
    d.pcm.resize(std::size_t(d.samples_per_frame) * d.channels);
    casement::log::info("audio: Opus {} Hz, {} channels, {} samples per packet", d.rate, d.channels,
                        d.samples_per_frame);
    return 0;
}

void AudioPlayer::play(const char* data, int length) {
    Impl& d = *impl_;
    if (!d.decoder) return;
    int frames = opus_multistream_decode_float(d.decoder, reinterpret_cast<const unsigned char*>(data),
                                               data ? length : 0, d.pcm.data(), d.samples_per_frame, 0);
    if (frames <= 0) {
        casement::log::debug("audio: can't decode a packet: {}", opus_strerror(frames));
        return;
    }
    ++d.packets;
    for (int i = 0; i < frames * d.channels; ++i) d.peak = std::max(d.peak, std::fabs(d.pcm[std::size_t(i)]));
    if (!d.ensure_device()) return;
    if (d.muted.load()) {
        if (d.started) {
            d.client->Stop();
            d.client->Reset();  // what was queued would play late
            d.started = false;
            d.paused = true;
            casement::log::info("audio: output paused");
        }
        return;
    }
    d.write(d.pcm.data(), frames);
    if (d.client && !d.started) {
        if (SUCCEEDED(d.client->Start())) {
            if (d.paused) casement::log::info("audio: output resumed");
            d.started = true;
            d.paused = false;
        }
    }
}

void AudioPlayer::cleanup() {
    Impl& d = *impl_;
    if (d.decoder)
        casement::log::info("audio: {} packets decoded, {} frames played, peak level {:.3f}", d.packets, d.played,
                            d.peak);
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    d.close_device();
    if (d.enumerator && d.watcher) d.enumerator->UnregisterEndpointNotificationCallback(d.watcher);
    release(d.watcher);
    release(d.enumerator);
    if (com) CoUninitialize();
    if (d.decoder) opus_multistream_decoder_destroy(d.decoder);
    d.decoder = nullptr;
}

void AudioPlayer::set_muted(bool muted) { impl_->muted = muted; }

}  // namespace moonlight
