#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

struct IAudioClient;
struct IAudioCaptureClient;
struct IMMDevice;
struct WAVEFORMATEX;

namespace fh6::sources {

class ExternalCaptureSource final : public IAudioSource {
public:
    explicit ExternalCaptureSource(ExternalCaptureConfig cfg);
    ~ExternalCaptureSource() override;

    std::string_view name() const noexcept override { return "external_capture"; }
    std::string_view display_name() const noexcept override { return "External Capture"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;

    void pump(RingBuffer& ring) override;

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }

    AuthState auth_state() const noexcept override {
        return auth_.load(std::memory_order_acquire);
    }

    std::string auth_instructions() const override;

    SourceCapabilities capabilities() const noexcept override {
        return {
            false,
            false,
            false
        };
    }

private:
    bool start_capture_locked();
    void stop_capture_locked() noexcept;
    bool ensure_capture_locked();
    void release_capture_locked() noexcept;

    std::size_t convert_packet_to_s16(
        const BYTE* data,
        std::uint32_t frames,
        std::vector<std::int16_t>& out
    );

    ExternalCaptureConfig cfg_;

    mutable std::mutex mu_;

    IAudioClient* audio_client_ = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    IMMDevice* device_ = nullptr;
    WAVEFORMATEX* mix_format_ = nullptr;

    std::atomic<PlaybackState> state_{PlaybackState::stopped};
    std::atomic<AuthState> auth_{AuthState::none_required};

    TrackInfo info_{};
    std::uint64_t captured_frames_ = 0;
};

} // namespace fh6::sources