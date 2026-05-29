#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "process_loopback_capture.hpp"

#include <thread>

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <filesystem>
#include <string>
#include <iostream>

using Microsoft::WRL::Callback;

namespace {

ICoreWebView2Controller* g_controller = nullptr;
ICoreWebView2* g_webview = nullptr;

std::wstring exe_dir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path{path}.parent_path().wstring();
}

std::wstring file_uri(const std::wstring& path) {
    std::wstring uri = L"file:///";
    for (wchar_t ch : path) {
        uri += (ch == L'\\') ? L'/' : ch;
    }
    return uri;
}

void resize_webview(HWND hwnd) {
    if (!g_controller) return;

    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    g_controller->put_Bounds(bounds);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_SIZE:
        resize_webview(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_webview) {
            g_webview->Release();
            g_webview = nullptr;
        }

        if (g_controller) {
            g_controller->Close();
            g_controller->Release();
            g_controller = nullptr;
        }

        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

void init_webview(HWND hwnd) {
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    MessageBoxW(hwnd, L"Failed to create WebView2 environment.",
                                L"FH6 Apple Helper", MB_ICONERROR);
                    return result;
                }

                env->CreateCoreWebView2Controller(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                MessageBoxW(hwnd, L"Failed to create WebView2 controller.",
                                            L"FH6 Apple Helper", MB_ICONERROR);
                                return result;
                            }

                            g_controller = controller;
                            g_controller->AddRef();

                            g_controller->get_CoreWebView2(&g_webview);

                            EventRegistrationToken token{};

                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*,
                                       ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR msg = nullptr;
                                        args->TryGetWebMessageAsString(&msg);

                                        std::wstring message = msg ? msg : L"";

                                        if (msg) {
                                            CoTaskMemFree(msg);
                                        }

                                        if (message.find(L"capture-test") != std::wstring::npos) {
                                            std::thread([] {
                                                try {
                                                    const auto out =
                                                        std::filesystem::path{exe_dir()} /
                                                        L"capture_test.wav";

                                                    const bool ok = fh6::apple_helper::
                                                        capture_self_process_tree_to_wav(out, 10);

                                                    if (ok) {
                                                        const auto stats =
                                                            fh6::apple_helper::last_capture_error();

                                                        const auto msg =
                                                            out.wstring() + L"\n\n" +
                                                            (stats.empty()
                                                                 ? L"No capture stats available."
                                                                 : stats);

                                                        MessageBoxW(nullptr, msg.c_str(),
                                                                    L"Capture complete", MB_OK);
                                                    } else {
                                                        const auto err =
                                                            fh6::apple_helper::last_capture_error();

                                                        MessageBoxW(
                                                            nullptr,
                                                            err.empty() ? L"Capture failed with no "
                                                                          L"error detail."
                                                                        : err.c_str(),
                                                            L"Capture failed", MB_ICONERROR);
                                                    }
                                                } catch (const std::exception& e) {
                                                    MessageBoxA(nullptr, e.what(),
                                                                "Capture exception", MB_ICONERROR);
                                                } catch (...) {
                                                    MessageBoxW(nullptr,
                                                                L"Unknown capture exception.",
                                                                L"Capture exception", MB_ICONERROR);
                                                }
                                            }).detach();
                                        }

                                        return S_OK;
                                    })
                                    .Get(),
                                &token);

                            resize_webview(hwnd);

                            g_webview->Navigate(L"http://127.0.0.1:8421/");

                            return S_OK;
                        })
                        .Get());

                return S_OK;
            })
            .Get());
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const wchar_t CLASS_NAME[] = L"FH6AppleHelperWindow";

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"FH6 Apple Music Helper",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1000,
        750,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    init_webview(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}