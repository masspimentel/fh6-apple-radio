#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "process_loopback_capture.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <propidl.h>

#include <wrl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <limits>
#include <tlhelp32.h>
#include <mutex>

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace fh6::apple_helper {
namespace {

std::wstring g_last_error;

std::atomic_bool g_stream_running{false};
std::thread g_stream_thread;
std::mutex g_stream_mutex;

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\fh6_apple_radio_pcm";

void set_error(const std::wstring& msg) { g_last_error = msg; }

std::wstring hr_hex(HRESULT hr) {
    wchar_t buf[32]{};
    swprintf_s(buf, L"0x%08X", static_cast<unsigned>(hr));
    return buf;
}

constexpr std::uint32_t kOutRate       = 48000;
constexpr std::uint16_t kOutChannels   = 2;
constexpr std::uint16_t kOutBits       = 16;
constexpr std::uint16_t kOutBlockAlign = kOutChannels * (kOutBits / 8);
constexpr std::uint32_t kOutByteRate   = kOutRate * kOutBlockAlign;

struct WavHeader {
    char riff[4]            = {'R', 'I', 'F', 'F'};
    std::uint32_t riff_size = 0;
    char wave[4]            = {'W', 'A', 'V', 'E'};

    char fmt[4]                   = {'f', 'm', 't', ' '};
    std::uint32_t fmt_size        = 16;
    std::uint16_t audio_format    = 1;
    std::uint16_t channels        = kOutChannels;
    std::uint32_t sample_rate     = kOutRate;
    std::uint32_t byte_rate       = kOutByteRate;
    std::uint16_t block_align     = kOutBlockAlign;
    std::uint16_t bits_per_sample = kOutBits;

    char data[4]            = {'d', 'a', 't', 'a'};
    std::uint32_t data_size = 0;
};

bool write_wav_header(std::ofstream& out, std::uint32_t data_bytes) {
    WavHeader h{};
    h.data_size = data_bytes;
    h.riff_size = 36 + data_bytes;

    out.seekp(0, std::ios::beg);
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    return static_cast<bool>(out);
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

float clamp_sample(float v) { return std::clamp(v, -1.0f, 1.0f); }

std::int16_t float_to_s16(float v) {
    v = clamp_sample(v);

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
            std::int32_t v = (static_cast<std::int32_t>(p[0])) |
                             (static_cast<std::int32_t>(p[1]) << 8) |
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
        default: return 0.0f;
    }
}

std::vector<std::int16_t> convert_to_48k_s16_stereo(const WAVEFORMATEX* wf, const BYTE* data,
                                                    UINT32 in_frames) {
    std::vector<std::int16_t> out;

    if (!wf || !data || in_frames == 0) {
        return out;
    }

    const auto in_rate     = wf->nSamplesPerSec;
    const auto in_channels = wf->nChannels;
    const auto bits        = wf->wBitsPerSample;
    const auto block_align = wf->nBlockAlign;

    if (!in_rate || !in_channels || !bits || !block_align) {
        return out;
    }

    const bool as_float = is_float_format(wf);
    const bool as_pcm   = is_pcm_format(wf);

    if (!as_float && !as_pcm) {
        return out;
    }

    const double ratio      = static_cast<double>(in_rate) / static_cast<double>(kOutRate);
    const UINT32 out_frames = static_cast<UINT32>(std::ceil(in_frames / ratio));

    out.resize(static_cast<std::size_t>(out_frames) * 2);

    for (UINT32 of = 0; of < out_frames; ++of) {
        UINT32 inf = static_cast<UINT32>(of * ratio);

        if (inf >= in_frames) {
            inf = in_frames - 1;
        }

        const BYTE* frame = data + static_cast<std::size_t>(inf) * block_align;

        auto sample_at = [&](WORD ch) -> float {
            if (ch >= in_channels) {
                ch = 0;
            }

            const BYTE* sample = frame + static_cast<std::size_t>(ch) * (bits / 8);

            if (as_float && bits == 32) {
                return read_float_sample(sample);
            }

            return read_pcm_sample(sample, bits);
        };

        float left  = 0.0f;
        float right = 0.0f;

        if (in_channels == 1) {
            left = right = sample_at(0);
        } else {
            left  = sample_at(0);
            right = sample_at(1);
        }

        out[static_cast<std::size_t>(of) * 2 + 0] = float_to_s16(left);
        out[static_cast<std::size_t>(of) * 2 + 1] = float_to_s16(right);
    }

    return out;
}

class ActivateAudioInterfaceCompletionHandler final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, Microsoft::WRL::FtmBase,
                          IActivateAudioInterfaceCompletionHandler> {
public:
    ActivateAudioInterfaceCompletionHandler()
        : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~ActivateAudioInterfaceCompletionHandler() override {
        if (event_) {
            CloseHandle(event_);
        }
    }

    IFACEMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activate_hr = E_FAIL;
        ComPtr<IUnknown> activated;

        HRESULT hr = operation->GetActivateResult(&activate_hr, &activated);

        if (SUCCEEDED(hr)) {
            result_ = activate_hr;

            if (SUCCEEDED(activate_hr) && activated) {
                activated.As(&audio_client_);
            }
        } else {
            result_ = hr;
        }

        SetEvent(event_);
        return S_OK;
    }

    bool wait(DWORD timeout_ms = 10000) {
        return WaitForSingleObject(event_, timeout_ms) == WAIT_OBJECT_0;
    }

    HRESULT result() const { return result_; }

    ComPtr<IAudioClient> audio_client() const { return audio_client_; }

private:
    HANDLE event_   = nullptr;
    HRESULT result_ = E_PENDING;
    ComPtr<IAudioClient> audio_client_;
};

ComPtr<IAudioClient> activate_process_loopback_client(DWORD pid) {
    AUDIOCLIENT_ACTIVATION_PARAMS activation_params{};
    activation_params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activation_params.ProcessLoopbackParams.TargetProcessId = pid;
    activation_params.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activate_param{};
    PropVariantInit(&activate_param);

    activate_param.vt             = VT_BLOB;
    activate_param.blob.cbSize    = sizeof(activation_params);
    activate_param.blob.pBlobData = static_cast<BYTE*>(CoTaskMemAlloc(sizeof(activation_params)));

    if (!activate_param.blob.pBlobData) {
        set_error(L"CoTaskMemAlloc failed for activation params");
        return {};
    }

    std::memcpy(activate_param.blob.pBlobData, &activation_params, sizeof(activation_params));

    auto completion = Microsoft::WRL::Make<ActivateAudioInterfaceCompletionHandler>();

    ComPtr<IActivateAudioInterfaceAsyncOperation> async_op;

    HRESULT hr =
        ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                    &activate_param, completion.Get(), &async_op);

    if (FAILED(hr)) {
        set_error(L"ActivateAudioInterfaceAsync failed: " + hr_hex(hr));
        PropVariantClear(&activate_param);
        return {};
    }

    const bool completed = completion->wait();

    PropVariantClear(&activate_param);

    if (!completed) {
        set_error(L"ActivateAudioInterfaceAsync timed out");
        return {};
    }

    if (FAILED(completion->result())) {
        set_error(L"ActivateCompleted failed: " + hr_hex(completion->result()));
        return {};
    }

    return completion->audio_client();
}

DWORD find_direct_webview2_child_pid(DWORD parent_pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    DWORD found_pid = 0;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            const bool parent_matches = entry.th32ParentProcessID == parent_pid;

            const bool is_webview = _wcsicmp(entry.szExeFile, L"msedgewebview2.exe") == 0;

            if (parent_matches && is_webview) {
                found_pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found_pid;
}

DWORD find_capture_pid() {
    const DWORD self_pid         = GetCurrentProcessId();
    const DWORD webview_root_pid = find_direct_webview2_child_pid(self_pid);
    return webview_root_pid ? webview_root_pid : self_pid;
}

void pcm_pipe_stream_worker() {
    set_error(L"Starting PCM pipe stream...");

    const DWORD capture_pid = find_capture_pid();

    HRESULT hr                = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_initialized = SUCCEEDED(hr);

    ComPtr<IAudioClient> audio_client = activate_process_loopback_client(capture_pid);

    if (!audio_client) {
        set_error(L"Pipe stream failed: activate_process_loopback_client failed. target PID=" +
                  std::to_wstring(capture_pid));
        g_stream_running.store(false);
        if (co_initialized) CoUninitialize();
        return;
    }

    WAVEFORMATEXTENSIBLE fallback_format{};
    fallback_format.Format.wFormatTag     = WAVE_FORMAT_EXTENSIBLE;
    fallback_format.Format.nChannels      = 2;
    fallback_format.Format.nSamplesPerSec = 48000;
    fallback_format.Format.wBitsPerSample = 32;
    fallback_format.Format.nBlockAlign =
        fallback_format.Format.nChannels * fallback_format.Format.wBitsPerSample / 8;
    fallback_format.Format.nAvgBytesPerSec =
        fallback_format.Format.nSamplesPerSec * fallback_format.Format.nBlockAlign;
    fallback_format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fallback_format.Samples.wValidBitsPerSample = 32;
    fallback_format.dwChannelMask               = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    fallback_format.SubFormat                   = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    WAVEFORMATEX* mix_format = nullptr;
    bool free_mix_format     = false;

    hr = audio_client->GetMixFormat(&mix_format);

    if (FAILED(hr) || !mix_format) {
        mix_format      = &fallback_format.Format;
        free_mix_format = false;
    } else {
        free_mix_format = true;
    }

    const REFERENCE_TIME buffer_duration = 10000000;

    hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  buffer_duration, 0, mix_format, nullptr);

    if (FAILED(hr)) {
        set_error(L"Pipe stream failed: IAudioClient::Initialize failed: " + hr_hex(hr));
        if (free_mix_format) CoTaskMemFree(mix_format);
        g_stream_running.store(false);
        if (co_initialized) CoUninitialize();
        return;
    }

    ComPtr<IAudioCaptureClient> capture_client;
    hr = audio_client->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void**>(capture_client.GetAddressOf()));

    if (FAILED(hr) || !capture_client) {
        set_error(L"Pipe stream failed: GetService failed: " + hr_hex(hr));
        if (free_mix_format) CoTaskMemFree(mix_format);
        g_stream_running.store(false);
        if (co_initialized) CoUninitialize();
        return;
    }

    HANDLE pipe = CreateFileW(kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (pipe == INVALID_HANDLE_VALUE) {
        set_error(L"Pipe stream failed: could not connect to named pipe. Start FH6/mod first.");
        if (free_mix_format) CoTaskMemFree(mix_format);
        g_stream_running.store(false);
        if (co_initialized) CoUninitialize();
        return;
    }

    hr = audio_client->Start();

    if (FAILED(hr)) {
        set_error(L"Pipe stream failed: IAudioClient::Start failed: " + hr_hex(hr));
        CloseHandle(pipe);
        if (free_mix_format) CoTaskMemFree(mix_format);
        g_stream_running.store(false);
        if (co_initialized) CoUninitialize();
        return;
    }

    set_error(L"PCM pipe stream running. target PID=" + std::to_wstring(capture_pid));

    while (g_stream_running.load()) {
        UINT32 packet_frames = 0;
        hr                   = capture_client->GetNextPacketSize(&packet_frames);

        if (FAILED(hr)) {
            break;
        }

        if (packet_frames == 0) {
            Sleep(5);
            continue;
        }

        BYTE* data             = nullptr;
        DWORD flags            = 0;
        UINT64 device_position = 0;
        UINT64 qpc_position    = 0;

        hr = capture_client->GetBuffer(&data, &packet_frames, &flags, &device_position,
                                       &qpc_position);

        if (FAILED(hr)) {
            break;
        }

        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            auto converted = convert_to_48k_s16_stereo(mix_format, data, packet_frames);

            if (!converted.empty()) {
                DWORD written     = 0;
                const DWORD bytes = static_cast<DWORD>(converted.size() * sizeof(std::int16_t));

                const BOOL ok = WriteFile(pipe, converted.data(), bytes, &written, nullptr);

                if (!ok) {
                    capture_client->ReleaseBuffer(packet_frames);
                    break;
                }
            }
        }

        capture_client->ReleaseBuffer(packet_frames);
    }

    audio_client->Stop();

    CloseHandle(pipe);

    if (free_mix_format) {
        CoTaskMemFree(mix_format);
    }

    if (co_initialized) {
        CoUninitialize();
    }

    g_stream_running.store(false);
    set_error(L"PCM pipe stream stopped.");
}

} // namespace

bool capture_self_process_tree_to_wav(const std::filesystem::path& output_path, int seconds) {
    if (seconds <= 0) {
        seconds = 10;
    }

    set_error(L"");

    HRESULT hr                = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_initialized = SUCCEEDED(hr);

    const DWORD self_pid         = GetCurrentProcessId();
    const DWORD webview_root_pid = find_direct_webview2_child_pid(self_pid);
    const DWORD capture_pid      = webview_root_pid ? webview_root_pid : self_pid;

    set_error(L"Capture target PID=" + std::to_wstring(capture_pid) + L", self PID=" +
              std::to_wstring(self_pid) + L", webview root PID=" +
              std::to_wstring(webview_root_pid));

    ComPtr<IAudioClient> audio_client = activate_process_loopback_client(capture_pid);

    if (!audio_client) {
        set_error(L"activate_process_loopback_client failed. target PID=" +
                  std::to_wstring(capture_pid) + L", self PID=" + std::to_wstring(self_pid) +
                  L", webview root PID=" + std::to_wstring(webview_root_pid));

        if (co_initialized) {
            CoUninitialize();
        }

        return false;
    }

    WAVEFORMATEXTENSIBLE fallback_format{};
    fallback_format.Format.wFormatTag     = WAVE_FORMAT_EXTENSIBLE;
    fallback_format.Format.nChannels      = 2;
    fallback_format.Format.nSamplesPerSec = 48000;
    fallback_format.Format.wBitsPerSample = 32;
    fallback_format.Format.nBlockAlign =
        fallback_format.Format.nChannels * fallback_format.Format.wBitsPerSample / 8;
    fallback_format.Format.nAvgBytesPerSec =
        fallback_format.Format.nSamplesPerSec * fallback_format.Format.nBlockAlign;
    fallback_format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fallback_format.Samples.wValidBitsPerSample = 32;
    fallback_format.dwChannelMask               = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    fallback_format.SubFormat                   = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    WAVEFORMATEX* mix_format = nullptr;
    bool free_mix_format     = false;

    hr = audio_client->GetMixFormat(&mix_format);

    if (FAILED(hr) || !mix_format) {
        // Process-loopback virtual audio clients may return E_NOTIMPL here.
        // Use our own known-good format instead.
        mix_format      = &fallback_format.Format;
        free_mix_format = false;
        set_error(L"GetMixFormat unavailable; using fallback 48k stereo 32-bit float format");
    } else {
        free_mix_format = true;
    }

    const REFERENCE_TIME buffer_duration = 10000000; // 1 second

    hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  buffer_duration, 0, mix_format, nullptr);

    if (FAILED(hr)) {
        set_error(L"IAudioClient::Initialize failed: " + hr_hex(hr));

        if (free_mix_format) {
            CoTaskMemFree(mix_format);
        }

        if (co_initialized) {
            CoUninitialize();
        }

        return false;
    }

    ComPtr<IAudioCaptureClient> capture_client;
    hr = audio_client->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void**>(capture_client.GetAddressOf()));

    if (FAILED(hr) || !capture_client) {
        set_error(L"GetService(IAudioCaptureClient) failed: " + hr_hex(hr));

        if (free_mix_format) {
            CoTaskMemFree(mix_format);
        }

        if (co_initialized) {
            CoUninitialize();
        }

        return false;
    }

    std::ofstream wav{output_path, std::ios::binary | std::ios::trunc};

    if (!wav) {
        set_error(L"Could not open output WAV file");

        if (free_mix_format) {
            CoTaskMemFree(mix_format);
        }

        if (co_initialized) {
            CoUninitialize();
        }

        return false;
    }

    write_wav_header(wav, 0);

    std::uint32_t data_bytes = 0;

    hr = audio_client->Start();

    if (FAILED(hr)) {
        set_error(L"IAudioClient::Start failed: " + hr_hex(hr));

        if (free_mix_format) {
            CoTaskMemFree(mix_format);
        }

        if (co_initialized) {
            CoUninitialize();
        }

        return false;
    }

    const auto end_tick = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000ull;

    std::uint32_t packet_count            = 0;
    std::uint32_t silent_packet_count     = 0;
    std::uint32_t non_silent_packet_count = 0;
    std::int16_t max_abs_sample           = 0;

    while (GetTickCount64() < end_tick) {
        UINT32 packet_frames = 0;
        hr                   = capture_client->GetNextPacketSize(&packet_frames);

        if (FAILED(hr)) {
            break;
        }

        if (packet_frames == 0) {
            Sleep(10);
            continue;
        }

        BYTE* data             = nullptr;
        DWORD flags            = 0;
        UINT64 device_position = 0;
        UINT64 qpc_position    = 0;

        hr = capture_client->GetBuffer(&data, &packet_frames, &flags, &device_position,
                                       &qpc_position);

        if (FAILED(hr)) {
            break;
        }

        packet_count++;

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            silent_packet_count++;
        } else {
            non_silent_packet_count++;
        }

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            capture_client->ReleaseBuffer(packet_frames);
            continue;
        }

        auto converted = convert_to_48k_s16_stereo(mix_format, data, packet_frames);

        for (const auto sample : converted) {
            const int sample_int = static_cast<int>(sample);
            const int abs_sample = sample_int == std::numeric_limits<std::int16_t>::min()
                                     ? std::numeric_limits<std::int16_t>::max()
                                     : std::abs(sample_int);

            if (abs_sample > max_abs_sample) {
                max_abs_sample = static_cast<std::int16_t>(abs_sample);
            }
        }

        if (!converted.empty()) {
            const auto bytes = static_cast<std::uint32_t>(converted.size() * sizeof(std::int16_t));

            wav.write(reinterpret_cast<const char*>(converted.data()), bytes);
            data_bytes += bytes;
        }

        capture_client->ReleaseBuffer(packet_frames);
    }

    audio_client->Stop();

    write_wav_header(wav, data_bytes);

    if (free_mix_format) {
        CoTaskMemFree(mix_format);
    }

    if (co_initialized) {
        CoUninitialize();
    }

    if (data_bytes == 0) {
        set_error(L"Capture produced 0 bytes. packets=" + std::to_wstring(packet_count) +
                  L", silent=" + std::to_wstring(silent_packet_count) + L", non_silent=" +
                  std::to_wstring(non_silent_packet_count));
        return false;
    }

    set_error(L"Capture complete. bytes=" + std::to_wstring(data_bytes) + L", packets=" +
              std::to_wstring(packet_count) + L", silent=" + std::to_wstring(silent_packet_count) +
              L", non_silent=" + std::to_wstring(non_silent_packet_count) + L", max_abs_sample=" +
              std::to_wstring(max_abs_sample));

    return true;
}

void start_pcm_pipe_stream() {
    std::scoped_lock lock{g_stream_mutex};

    if (g_stream_running.load()) {
        set_error(L"PCM pipe stream is already running.");
        return;
    }

    g_stream_running.store(true);

    g_stream_thread = std::thread([] { pcm_pipe_stream_worker(); });

    g_stream_thread.detach();
}

void stop_pcm_pipe_stream() {
    g_stream_running.store(false);
    set_error(L"PCM pipe stream stop requested.");
}

std::wstring last_capture_error() { return g_last_error; }

} // namespace fh6::apple_helper