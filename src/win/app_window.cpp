#include "win/app_window.hpp"

#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <format>

#include "util/log.hpp"
#include "win/resource.h"

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
                // Drawn here rather than left to the frame loop. A resize drag
                // runs inside a modal loop in DefWindowProc that does not
                // return until the mouse is released, so the loop gets no turn
                // for the whole gesture and the window would show a stretched
                // copy of the last frame while its buffers were resized under
                // it. Reached only from the message pump, which never runs
                // mid-frame, so this cannot re-enter a frame in progress.
                if (paint_handler_) {
                    paint_handler_();
                }
            }
            return 0;
        }

        case WM_PAINT: {
            // The composition tree owns every pixel, so this exists to satisfy
            // the invalid region and to repaint on the exposures the modal
            // loop generates. Without it DefWindowProc validates and nothing
            // is drawn.
            PAINTSTRUCT paint;
            BeginPaint(window, &paint);
            if (paint_handler_) {
                paint_handler_();
            }
            EndPaint(window, &paint);
            return 0;
        }

        case WM_DPICHANGED: {
            // Windows hands over the rectangle that keeps the window the same
            // physical size on the new monitor; taking it is what stops a drag
            // between displays from resizing the window under the pointer.
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (dpi_handler_) {
                dpi_handler_(static_cast<float>(HIWORD(wparam)) / 96.0f);
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

        // WM_ERASEBKGND is deliberately left to DefWindowProc so the class
        // brush below gets used. Suppressing it leaves the window's own
        // surface uninitialised, which shows as white in any region the
        // composition tree does not cover — the gap a resize opens up before
        // mpv has caught up with the new size, most visibly.

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
    // Icon comes from the executable's own resources (see coax.rc.in). Absent
    // it, the taskbar and Alt-Tab fall back to the generic application icon.
    window_class.hIcon         = LoadIconW(instance, MAKEINTRESOURCEW(IDI_COAX_APP));
    window_class.hIconSm       = window_class.hIcon;
    // The composition target is created topmost, so the visuals draw above
    // whatever the window itself paints. That makes this brush the floor of
    // the whole application: it is only ever seen where video and UI do not
    // reach, and it matches the bottom of the backdrop so those moments read
    // as the application rather than as a hole in it.
    background_brush_          = CreateSolidBrush(RGB(0x04, 0x06, 0x0A));
    window_class.hbrBackground = background_brush_;

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

    // The requested size is logical, but the process is per-monitor DPI aware,
    // so CreateWindowEx took it as physical pixels and produced a window that
    // is too small on a scaled display. Which monitor it landed on is only
    // knowable once it exists, hence correcting the size rather than computing
    // it up front.
    const float scale = dpi_scale();
    RECT        desired{0, 0, static_cast<LONG>(static_cast<float>(width) * scale),
                 static_cast<LONG>(static_cast<float>(height) * scale)};
    AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, FALSE);
    int outer_width  = desired.right - desired.left;
    int outer_height = desired.bottom - desired.top;

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        // A scaled-up default can be larger than the display it is opening on,
        // and CW_USEDEFAULT placement plus a size change can leave the window
        // hanging off the edge either way. Fit it, then centre it.
        const int work_width  = monitor.rcWork.right - monitor.rcWork.left;
        const int work_height = monitor.rcWork.bottom - monitor.rcWork.top;
        outer_width           = std::min(outer_width, work_width);
        outer_height          = std::min(outer_height, work_height);
        SetWindowPos(window_, nullptr,
                     monitor.rcWork.left + (work_width - outer_width) / 2,
                     monitor.rcWork.top + (work_height - outer_height) / 2,
                     outer_width, outer_height, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowPos(window_, nullptr, 0, 0, outer_width, outer_height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Windows draws the caption from the system theme, so a light-themed
    // desktop puts a white title bar above a black application. 20 is
    // DWMWA_USE_IMMERSIVE_DARK_MODE, which mingw's dwmapi.h predates; older
    // Windows builds ignore the attribute rather than failing the call.
    const BOOL dark_caption = TRUE;
    DwmSetWindowAttribute(window_, 20, &dark_caption, sizeof(dark_caption));

    RECT client{};
    GetClientRect(window_, &client);
    width_  = client.right - client.left;
    height_ = client.bottom - client.top;

    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);

    log::info("Window created ({}x{})", width_, height_);
    return true;
}

float AppWindow::dpi_scale() const {
    // The backend already picks the best of the several APIs Windows has
    // accumulated for this and falls back cleanly on older builds.
    return window_ ? ImGui_ImplWin32_GetDpiScaleForHwnd(window_) : 1.0f;
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
    // The class held it, so it can only go once the class is gone.
    if (background_brush_) {
        DeleteObject(background_brush_);
        background_brush_ = nullptr;
    }
}

}  // namespace coax::win
