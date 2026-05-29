#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <filesystem>
#include <string>

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
        nullptr,
        nullptr,
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    MessageBoxW(hwnd, L"Failed to create WebView2 environment.", L"FH6 Apple Helper", MB_ICONERROR);
                    return result;
                }

                env->CreateCoreWebView2Controller(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                MessageBoxW(hwnd, L"Failed to create WebView2 controller.", L"FH6 Apple Helper", MB_ICONERROR);
                                return result;
                            }

                            g_controller = controller;
                            g_controller->AddRef();

                            g_controller->get_CoreWebView2(&g_webview);

                            resize_webview(hwnd);

                            const auto html_path =
                                std::filesystem::path{exe_dir()} /
                                L"apple_helper" /
                                L"index.html";

                            const auto uri = file_uri(html_path.wstring());
                            g_webview->Navigate(uri.c_str());

                            return S_OK;
                        }
                    ).Get()
                );

                return S_OK;
            }
        ).Get()
    );
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