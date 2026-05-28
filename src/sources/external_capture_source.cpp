#include "fh6/sources/external_capture_source.hpp"

#include "fh6/log.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cmath>
#include <cstring>

WAVEFORMATEX* mix_format_as_wave(void* p) {
    return reinterpret_cast<WAVEFORMATEX*>(p);
}

const WAVEFORMATEX* mix_format_as_wave(const void* p) {
    return reinterpret_cast<const WAVEFORMATEX*>(p);
}

namespace fh6::sources {
namespace {

constexpr std::uint32_t kOutputRate = 48000;
constexpr std::uint16_t kOutputChannels = 2;
constexpr std::size_t kOutputFrameBytes = 4; // s16 stereo

template <class T>
void safe_release(T*& p) noexcept {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool is_float_format(const WAVEFORMATEX* wf) {
    if (!wf) return false;

    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }

    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }

    return false;
}

bool is_pcm_format(const WAVEFORMATEX* wf) {
    if (!wf) return false;

    if (wf->wFormatTag == WAVE_FORMAT_PCM) {
        return true;
    }

    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
    }

    return false;
}

float clamp_sample(float v) {
    if (v > 1.0f) return 1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

std::int16_t float_to_s16(float v, float gain) {
    v = clamp_sample(v * gain);

    if (v >= 0.0f) {
        return static_cast<std::int16_t>(v * 32767.0f);
    }

    return static_cast<std::int16_t>(v * 32768.0f);
}

float read_float_sample(const BYTE* p) {
    float v = 0.0f;
    std::memcpy(&v, p, sizeof(float));
    return v;
}

float read_pcm_sample(const BYTE* p, WORD bits) {
    switch (bits) {
    case 16: {
        std::int16_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v) / 32768.0f;
    }
    case 24: {
        std::int32_t v =
            (static_cast<std::int32_t>(p[0])      ) |
            (static_cast<std::int32_t>(p[1]) <<  8) |
            (static_cast<std::int32_t>(p[2]) << 16);

        if (v & 0x00800000) {
            v |= 0xFF000000;
        }

        return static_cast<float>(v) / 8388608.0f;
    }
    case 32: {
        std::int32_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v) / 2147483648.0f;
    }
    default:
        return 0.0f;
    }
}

} // namespace

ExternalCaptureSource::ExternalCaptureSource(ExternalCaptureConfig cfg)
    : cfg_{std::move(cfg)} {
    info_.title = "External Capture";
    info_.artist = "System audio";
    info_.album = "WASAPI loopback";
}

ExternalCaptureSource::~ExternalCaptureSource() {
    shutdown();
}

bool ExternalCaptureSource::initialize() {
    if (!cfg_.enabled) {
        return false;
    }

    auth_.store(AuthState::none_required, std::memory_order_release);
    state_.store(PlaybackState::stopped, std::memory_order_release);

    log::info("[external_capture] initialized; device='{}', gain={}", cfg_.device, cfg_.gain);
    return true;
}

void ExternalCaptureSource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    stop_capture_locked();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void ExternalCaptureSource::play() {
    std::scoped_lock lk{mu_};

    if (!start_capture_locked()) {
        state_.store(PlaybackState::stopped, std::memory_order_release);
        auth_.store(AuthState::error, std::memory_order_release);
        return;
    }

    state_.store(PlaybackState::playing, std::memory_order_release);
}

void ExternalCaptureSource::pause() {
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void ExternalCaptureSource::stop() {
    std::scoped_lock lk{mu_};
    stop_capture_locked();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

bool ExternalCaptureSource::ensure_capture_locked() {
    if (audio_client_ && capture_client_) {
        return true;
    }

    return start_capture_locked();
}

bool ExternalCaptureSource::start_capture_locked() {
    if (audio_client_ && capture_client_) {
        return true;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        log::warn("[external_capture] CoInitializeEx failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator)
    );

    if (FAILED(hr) || !enumerator) {
        log::warn("[external_capture] CoCreateInstance(MMDeviceEnumerator) failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    safe_release(enumerator);

    if (FAILED(hr) || !device_) {
        log::warn("[external_capture] GetDefaultAudioEndpoint failed: 0x{:08X}", static_cast<unsigned>(hr));
        release_capture_locked();
        return false;
    }

    hr = device_->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(&audio_client_)
    );

    if (FAILED(hr) || !audio_client_) {
        log::warn("[external_capture] Activate(IAudioClient) failed: 0x{:08X}", static_cast<unsigned>(hr));
        release_capture_locked();
        return false;
    }

    WAVEFORMATEX* wf = nullptr;
    hr = audio_client_->GetMixFormat(&wf);
    mix_format_ = wf;
    if (FAILED(hr) || !mix_format_) {
        log::warn("[external_capture] GetMixFormat failed: 0x{:08X}", static_cast<unsigned>(hr));
        release_capture_locked();
        return false;
    }

    const REFERENCE_TIME buffer_duration = 10000000; // 1 second

    hr = audio_client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        buffer_duration,
        0,
        mix_format_as_wave(mix_format_),
        nullptr
    );

    if (FAILED(hr)) {
        log::warn("[external_capture] IAudioClient::Initialize(loopback) failed: 0x{:08X}", static_cast<unsigned>(hr));
        release_capture_locked();
        return false;
    }

    hr = audio_client_->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(&capture_client_)
    );

    if (FAILED(hr) || !capture_client_) {
        log::warn("[external_capture] GetService(IAudioCaptureClient) failed: 0x{:08X}", static_cast<unsigned>(hr));
        release_capture_locked();
        return false;
    }

    hr = audio_client_->Start();

    if (FAILED(hr)) {
        log::warn("[external_capture] IAudioClient::Start failed: 0x{:08X}", static_cast<unsigned>(hr));
        release_capture_locked();
        return false;
    }

    captured_frames_ = 0;

    log::info(
        "[external_capture] started loopback: {} Hz, {} channels, {} bits, tag=0x{:04X}",
        mix_format_as_wave(mix_format_)->nSamplesPerSec,
        mix_format_as_wave(mix_format_)->nChannels,
        mix_format_as_wave(mix_format_)->wBitsPerSample,
        mix_format_as_wave(mix_format_)->wFormatTag
    );

    return true;
}

void ExternalCaptureSource::stop_capture_locked() noexcept {
    if (audio_client_) {
        audio_client_->Stop();
    }

    release_capture_locked();
}

void ExternalCaptureSource::release_capture_locked() noexcept {
    safe_release(capture_client_);
    safe_release(audio_client_);
    safe_release(device_);

    if (mix_format_) {
        CoTaskMemFree(mix_format_);
        mix_format_ = nullptr;
    }
}

std::size_t ExternalCaptureSource::convert_packet_to_s16(
    const unsigned char* data,
    std::uint32_t frames,
    std::vector<std::int16_t>& out
) {
    const WAVEFORMATEX* wf = mix_format_as_wave(mix_format_);
    out.clear();

    if (!mix_format_ || !data || frames == 0) {
        return 0;
    }

    const auto in_rate = wf->nSamplesPerSec;
    const auto in_channels = wf->nChannels;
    const auto bits = wf->wBitsPerSample;
    const auto block_align = wf->nBlockAlign;

    if (in_rate == 0 || in_channels == 0 || block_align == 0) {
        return 0;
    }

    const bool as_float = is_float_format(wf);
    const bool as_pcm = is_pcm_format(wf);

    if (!as_float && !as_pcm) {
        return 0;
    }

    const double ratio = static_cast<double>(in_rate) / static_cast<double>(kOutputRate);
    const std::uint32_t out_frames = static_cast<std::uint32_t>(std::ceil(frames / ratio));

    out.resize(static_cast<std::size_t>(out_frames) * kOutputChannels);

    const float gain = std::clamp(cfg_.gain, 0.0f, 4.0f);

    for (std::uint32_t of = 0; of < out_frames; ++of) {
        std::uint32_t in_frame = static_cast<std::uint32_t>(of * ratio);

        if (in_frame >= frames) {
            in_frame = frames - 1;
        }

        const BYTE* frame = data + static_cast<std::size_t>(in_frame) * block_align;

        auto sample_at = [&](std::uint16_t ch) -> float {
            if (ch >= in_channels) {
                ch = 0;
            }

            const BYTE* sample = frame + static_cast<std::size_t>(ch) * (bits / 8);

            if (as_float && bits == 32) {
                return read_float_sample(sample);
            }

            return read_pcm_sample(sample, bits);
        };

        float left = 0.0f;
        float right = 0.0f;

        if (in_channels == 1) {
            left = right = sample_at(0);
        } else {
            left = sample_at(0);
            right = sample_at(1);
        }

        out[static_cast<std::size_t>(of) * 2 + 0] = float_to_s16(left, gain);
        out[static_cast<std::size_t>(of) * 2 + 1] = float_to_s16(right, gain);
    }

    return out.size() * sizeof(std::int16_t);
}

void ExternalCaptureSource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing) {
        return;
    }

    std::scoped_lock lk{mu_};

    if (!ensure_capture_locked()) {
        return;
    }

    std::vector<std::int16_t> converted;

    while (ring.writable() >= kOutputFrameBytes) {
        UINT32 packet_frames = 0;
        HRESULT hr = capture_client_->GetNextPacketSize(&packet_frames);

        if (FAILED(hr) || packet_frames == 0) {
            break;
        }

        BYTE* data = nullptr;
        DWORD flags = 0;
        UINT64 device_pos = 0;
        UINT64 qpc_pos = 0;

        hr = capture_client_->GetBuffer(
            &data,
            &packet_frames,
            &flags,
            &device_pos,
            &qpc_pos
        );

        if (FAILED(hr)) {
            break;
        }

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            converted.assign(static_cast<std::size_t>(packet_frames) * 2, 0);
        } else {
            convert_packet_to_s16(data, packet_frames, converted);
        }

        const std::size_t bytes = converted.size() * sizeof(std::int16_t);

        if (bytes > 0 && ring.writable() >= bytes) {
            ring.write(converted.data(), bytes);
            captured_frames_ += bytes / kOutputFrameBytes;
        }

        capture_client_->ReleaseBuffer(packet_frames);

        if (bytes == 0 || ring.writable() < kOutputFrameBytes) {
            break;
        }
    }
}

TrackInfo ExternalCaptureSource::current_track() const {
    std::scoped_lock lk{mu_};

    TrackInfo info = info_;
    info.position_ms = (captured_frames_ * 1000ull) / kOutputRate;
    return info;
}

std::string ExternalCaptureSource::auth_instructions() const {
    if (auth_.load(std::memory_order_acquire) == AuthState::error) {
        return "Could not start Windows loopback capture. Make sure the default playback device is active.";
    }

    return {};
}

} // namespace fh6::sources