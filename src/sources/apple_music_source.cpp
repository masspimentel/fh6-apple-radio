#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "fh6/sources/apple_music_source.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <array>

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
    start_pipe_server();
    return true;
}

void AppleMusicSource::shutdown() noexcept {
    stop_pipe_server();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

AppleMusicSource::~AppleMusicSource() { shutdown(); }

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

void AppleMusicSource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing) {
        return;
    }

    std::scoped_lock lk{pcm_mu_};

    if (pcm_queue_.empty()) {
        return;
    }

    const auto writable = ring.writable();

    if (writable == 0) {
        return;
    }

    const auto bytes_to_write = std::min<std::size_t>(writable, pcm_queue_.size());

    ring.write(pcm_queue_.data(), bytes_to_write);

    static std::uint64_t total_pumped_bytes  = 0;
    total_pumped_bytes                      += bytes_to_write;

    static auto last_pump_log = GetTickCount64();
    const auto now            = GetTickCount64();

    if (now - last_pump_log > 2000) {
        last_pump_log = now;

        log::info("[apple_music] pump wrote total_bytes={}, last_write={}, remaining_queue={}, "
                  "ring_writable={}",
                  total_pumped_bytes, bytes_to_write, pcm_queue_.size(), ring.writable());
    }

    pcm_queue_.erase(pcm_queue_.begin(),
                     pcm_queue_.begin() + static_cast<std::ptrdiff_t>(bytes_to_write));
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

namespace {
constexpr wchar_t kAppleMusicPipeName[] = L"\\\\.\\pipe\\fh6_apple_radio_pcm";
constexpr std::size_t kMaxPcmQueueBytes = 48000 * 2 * 2 * 10; // 10 seconds
} // namespace

void AppleMusicSource::start_pipe_server() {
    bool expected = false;

    if (!pipe_running_.compare_exchange_strong(expected, true)) {
        return;
    }

    pipe_thread_ = std::thread([this] { pipe_server_loop(); });

    log::info("[apple_music] PCM pipe server started");
}

void AppleMusicSource::stop_pipe_server() noexcept {
    pipe_running_.store(false, std::memory_order_release);

    if (pipe_thread_.joinable()) {
        pipe_thread_.join();
    }
}

void AppleMusicSource::pipe_server_loop() {
    while (pipe_running_.load(std::memory_order_acquire)) {
        HANDLE pipe = CreateNamedPipeW(kAppleMusicPipeName, PIPE_ACCESS_INBOUND,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                                       64 * 1024, 64 * 1024, 0, nullptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            log::warn("[apple_music] CreateNamedPipeW failed: {}",
                      static_cast<unsigned>(GetLastError()));

            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        log::info("[apple_music] waiting for PCM helper pipe connection...");

        const BOOL connected =
            ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!connected) {
            log::warn("[apple_music] ConnectNamedPipe failed: {}",
                      static_cast<unsigned>(GetLastError()));

            CloseHandle(pipe);
            continue;
        }

        log::info("[apple_music] PCM helper connected");

        state_.store(PlaybackState::playing, std::memory_order_release);

        std::array<std::uint8_t, 16 * 1024> buffer{};

        while (pipe_running_.load(std::memory_order_acquire)) {
            DWORD read = 0;

            const BOOL ok =
                ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);

            if (!ok || read == 0) {
                break;
            }

            {
                std::scoped_lock lk{pcm_mu_};

                pcm_queue_.insert(pcm_queue_.end(), buffer.begin(),
                                  buffer.begin() + static_cast<std::ptrdiff_t>(read));

                static std::uint64_t total_pipe_bytes  = 0;
                total_pipe_bytes                      += read;

                static auto last_pipe_log = GetTickCount64();
                const auto now            = GetTickCount64();

                if (now - last_pipe_log > 2000) {
                    last_pipe_log = now;

                    log::info(
                        "[apple_music] pipe read total_bytes={}, last_read={}, queued_bytes={}",
                        total_pipe_bytes, read, pcm_queue_.size());
                }

                if (pcm_queue_.size() > kMaxPcmQueueBytes) {
                    const auto drop = pcm_queue_.size() - kMaxPcmQueueBytes;
                    pcm_queue_.erase(pcm_queue_.begin(),
                                     pcm_queue_.begin() + static_cast<std::ptrdiff_t>(drop));
                }
            }
        }

        log::info("[apple_music] PCM helper disconnected");

        state_.store(PlaybackState::stopped, std::memory_order_release);

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    log::info("[apple_music] PCM pipe server stopped");
}

} // namespace fh6::sources