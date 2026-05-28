#include "fh6/sources/apple_music_source.hpp"

#include "fh6/log.hpp"

namespace fh6::sources {

AppleMusicSource::AppleMusicSource(AppleMusicConfig cfg)
    : cfg_{std::move(cfg)} {}

bool AppleMusicSource::initialize() {
    auth_.store(
        cfg_.developer_token.empty()
            ? AuthState::needs_auth
            : AuthState::authenticated,
        std::memory_order_release
    );

    state_.store(PlaybackState::stopped, std::memory_order_release);

    log::info("[apple_music] initialized; playback_mode={}", cfg_.playback_mode);
    return true;
}

void AppleMusicSource::shutdown() noexcept {
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void AppleMusicSource::play() {
    // v1 does not generate PCM. MusicKit JS handles playback in the dashboard.
    state_.store(PlaybackState::playing, std::memory_order_release);
}

void AppleMusicSource::pause() {
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void AppleMusicSource::stop() {
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void AppleMusicSource::next() {
    // v1 transport is delegated to MusicKit JS.
    log::info("[apple_music] next requested");
}

void AppleMusicSource::previous() {
    // v1 transport is delegated to MusicKit JS.
    log::info("[apple_music] previous requested");
}

void AppleMusicSource::pump(RingBuffer& /*ring*/) {
    // Important:
    // Apple Music does not provide a raw PCM stream through MusicKit.
    // v1 intentionally leaves the game radio bus silent for this source.
}

TrackInfo AppleMusicSource::current_track() const {
    std::scoped_lock lk{mu_};
    return info_;
}

std::string AppleMusicSource::auth_instructions() const {
    if (cfg_.developer_token.empty()) {
        return "Apple Music needs a MusicKit developer token. Add one in config.toml or the dashboard settings.";
    }

    if (auth_.load(std::memory_order_acquire) == AuthState::needs_auth) {
        return "Sign in to Apple Music from the dashboard.";
    }

    return {};
}

void AppleMusicSource::update_now_playing(TrackInfo info, PlaybackState state) {
    {
        std::scoped_lock lk{mu_};
        info_ = std::move(info);
    }

    state_.store(state, std::memory_order_release);

    log::info(
        "[apple_music] now playing: '{}' - '{}'",
        info_.artist,
        info_.title
    );
}

void AppleMusicSource::set_authenticated(bool authenticated) {
    auth_.store(
        authenticated ? AuthState::authenticated : AuthState::needs_auth,
        std::memory_order_release
    );
}

} // namespace fh6::sources