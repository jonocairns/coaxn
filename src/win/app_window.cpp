#include "win/app_window.hpp"

#include <imgui.h>

#include <format>

#include "util/log.hpp"

// Provided by imgui_impl_win32; declared here to avoid pulling the backend
// header into every translation unit that includes this one.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg,
                                                             WPARAM wparam, LPARAM lparam);

namespace coax::win {
namespace {

constexpr const wchar_t* kWindowClass = L"CoaxNativeWindow";

}  // namespace

LRESULT CALLBACK AppWindow::window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self) {
        return self->handle_message(window, message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT AppWindow::handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) {
        return 1;
    }

    switch (message) {
        case WM_SIZE: {
            if (wparam != SIZE_MINIMIZED) {
                width_  = LOWORD(lparam);
                height_ = HIWORD(lparam);
                if (resize_handler_) {
                    resize_handler_(width_, height_);
                }
            }
            return 0;
        }

        case WM_SYSKEYDOWN:
            // Alt+Enter toggles fullscreen; swallow it so Windows does not beep.
            if (wparam == VK_RETURN) {
                set_fullscreen(!fullscreen_);
                return 0;
            }
            break;

        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE && fullscreen_) {
                set_fullscreen(false);
                return 0;
            }
            break;

        case WM_ERASEBKGND:
            // The composition tree paints every pixel; erasing would flash.
            return 1;

        case WM_CLOSE:
            running_ = false;
            if (close_handler_) {
                close_handler_();
            }
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            running_ = false;
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

bool AppWindow::create(const wchar_t* title, int width, int height, std::string& error) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW window_class{};
    window_class.cbSize        = sizeof(window_class);
    window_class.style         = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc   = &AppWindow::window_proc;
    window_class.hInstance     = instance;
    window_class.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClass;
    // No background brush: DirectComposition owns the surface.
    window_class.hbrBackground = nullptr;

    if (!RegisterClassExW(&window_class)) {
        error = std::format("RegisterClassEx failed ({})", GetLastError());
        return false;
    }

    RECT bounds{0, 0, width, height};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);

    window_ = CreateWindowExW(
        0, kWindowClass, title, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, this);

    if (!window_) {
        error = std::format("CreateWindowEx failed ({})", GetLastError());
        return false;
    }

    RECT client{};
    GetClientRect(window_, &client);
    width_  = client.right - client.left;
    height_ = client.bottom - client.top;

    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);

    log::info("Window created ({}x{})", width_, height_);
    return true;
}

void AppWindow::set_fullscreen(bool fullscreen) {
    if (!window_ || fullscreen == fullscreen_) {
        return;
    }

    if (fullscreen) {
        saved_placement_.length = sizeof(saved_placement_);
        GetWindowPlacement(window_, &saved_placement_);

        const HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO    info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            return;
        }
        SetWindowLongW(window_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(window_, HWND_TOP,
                     info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left,
                     info.rcMonitor.bottom - info.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(window_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(window_, &saved_placement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED |
                         SWP_NOOWNERZORDER);
    }

    fullscreen_ = fullscreen;
}

bool AppWindow::pump_messages() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            running_ = false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return running_;
}

void AppWindow::destroy() {
    if (window_) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
}

}  // namespace coax::win
