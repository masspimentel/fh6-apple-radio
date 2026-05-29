#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fh6::sources {

class AppleMusicSource final : public IAudioSource {
public:
    explicit AppleMusicSource(AppleMusicConfig cfg);
    ~AppleMusicSource() override;

    std::string_view name() const noexcept override { return "apple_music"; }
    std::string_view display_name() const noexcept override { return "Apple Music"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;

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
            false, // seek
            true,  // previous
            true   // queue
        };
    }

    void update_now_playing(TrackInfo info, PlaybackState state);
    void set_authenticated(bool authenticated);


private:
    void start_pipe_server();
    void stop_pipe_server() noexcept;
    void pipe_server_loop();

    AppleMusicConfig cfg_;

    mutable std::mutex mu_;
    TrackInfo info_{};

    std::atomic<PlaybackState> state_{PlaybackState::stopped};
    std::atomic<AuthState> auth_{AuthState::needs_auth};

    std::atomic_bool pipe_running_{false};
    std::thread pipe_thread_;

    mutable std::mutex pcm_mu_;
    std::vector<std::uint8_t> pcm_queue_;
};

} // namespace fh6::sources