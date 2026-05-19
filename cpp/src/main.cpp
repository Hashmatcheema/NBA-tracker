#include <exception>
#include <cstdint>
#include <string>
#ifndef _WIN32
#include <chrono>
#include <thread>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "j2k/config.hpp"
#include "j2k/dll_placeholder.hpp"
#include "j2k/injector_placeholder.hpp"
#include "j2k/logger.hpp"
#include "j2k/python_module_ports.hpp"
#include "j2k/runtime.hpp"

#ifdef _WIN32
namespace {

LRESULT CALLBACK j2k_status_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            SetBkMode(hdc, TRANSPARENT);
            DrawTextW(
                hdc,
                L"J2K Vision Safe C++ Runtime\n\nStatus: running\nMode: local module/plugin runtime\nNo injection or remote process operations.",
                -1,
                &rect,
                DT_CENTER | DT_VCENTER | DT_WORDBREAK);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

int run_status_window() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"J2KVisionSafeRuntimeWindow";

    WNDCLASSW wc{};
    wc.lpfnWndProc = j2k_status_window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    const ATOM reg_atom = RegisterClassW(&wc);
    const DWORD reg_err = GetLastError();
    if (!reg_atom && reg_err != ERROR_CLASS_ALREADY_EXISTS) {
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        class_name,
        L"J2K Vision Runtime",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        460,
        220,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (hwnd == nullptr) {
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

}  // namespace
#endif

int main(int argc, char** argv) {
    using namespace j2k;

    const std::string config_path = (argc > 1) ? std::string(argv[1]) : "config/module_loader.example.json";

    try {
        log(LogLevel::Info, "J2K Vision Safe C++ runtime starting.");
        log(LogLevel::Info, "Loading config: " + config_path);

        const AppConfig cfg = load_config_file(config_path);
        const ModuleLoadResult validation = validate_module_loading_inputs(cfg.module);
        if (!validation.ok) {
            log(LogLevel::Error, validation.message);
            return 1;
        }
        log(LogLevel::Info, validation.message);
        log(LogLevel::Info, describe_dll_placeholder_contract(cfg.module));
        log(LogLevel::Info, describe_python_module_ports());

        log(LogLevel::Info, "Runtime initialized; opening status window.");
#ifdef _WIN32
        return run_status_window();
#else
        log(LogLevel::Info, "Status window is only available on Windows; keeping runtime alive.");
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
#endif
    } catch (const std::exception& ex) {
        log(LogLevel::Error, std::string("Fatal error: ") + ex.what());
        return 2;
    }
}
