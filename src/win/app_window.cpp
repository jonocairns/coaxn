#include "win/app_window.hpp"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
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

// The system metric at a particular DPI. GetSystemMetricsForDpi is Windows 10
// 1607 and mingw's headers predate it, so it is resolved rather than linked;
// the fallback is the same metric at the system DPI, which is only wrong on a
// mixed-scale desktop and only by a couple of pixels of resize border.
int metric_for_dpi(int index, UINT dpi) {
    using MetricForDpi = int(WINAPI*)(int, UINT);
    static const auto resolved = reinterpret_cast<MetricForDpi>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi")));
    return resolved ? resolved(index, dpi) : GetSystemMetrics(index);
}

// Which edge an auto-hiding taskbar occupies on this window's monitor, or -1
// for none. A maximised window that covers every pixel of a monitor leaves the
// shell nowhere to notice the pointer arriving, so an auto-hidden bar can never
// come back — the client area has to stop one pixel short of that edge.
int autohide_taskbar_edge(HWND window) {
    APPBARDATA state{};
    state.cbSize = sizeof(state);
    if ((SHAppBarMessage(ABM_GETSTATE, &state) & ABS_AUTOHIDE) == 0) {
        return -1;
    }

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return -1;
    }

    for (const UINT edge : {ABE_TOP, ABE_BOTTOM, ABE_LEFT, ABE_RIGHT}) {
        APPBARDATA query{};
        query.cbSize = sizeof(query);
        query.uEdge  = edge;
        query.rc     = monitor.rcMonitor;
        if (SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &query) != 0) {
            return static_cast<int>(edge);
        }
    }
    return -1;
}

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
        case WM_NCCALCSIZE: {
            // What removes the caption. The frame's pixels are still there —
            // the style keeps every WS_OVERLAPPEDWINDOW bit, so DWM goes on
            // providing the shadow, the snap behaviour and the animations —
            // but the client area is given all of them, and the composition
            // tree draws over what Windows would have drawn a title bar on.
            //
            // wparam FALSE asks about a rectangle rather than the window, and
            // DefWindowProc answers that case correctly on its own.
            if (!custom_frame() || wparam == FALSE) {
                break;
            }

            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
            RECT& client = params->rgrc[0];

            // Restored, the proposed rectangle is taken whole: that is the one
            // move that makes client and window the same rectangle. Maximised
            // it cannot be, because Windows sizes a maximised window to the
            // monitor *plus* the frame it expects to be drawn outside it. Kept
            // whole, the top of the application would be off the screen.
            if (IsZoomed(window)) {
                const int border_x = frame_thickness(window, false);
                const int border_y = frame_thickness(window, true);
                client.left   += border_x;
                client.top    += border_y;
                client.right  -= border_x;
                client.bottom -= border_y;

                switch (autohide_taskbar_edge(window)) {
                    case ABE_TOP:    client.top    += 1; break;
                    case ABE_BOTTOM: client.bottom -= 1; break;
                    case ABE_LEFT:   client.left   += 1; break;
                    case ABE_RIGHT:  client.right  -= 1; break;
                    default: break;
                }
            }
            return 0;
        }

        case WM_NCHITTEST: {
            // With the frame consumed above, every pixel of the window is
            // client area — including the ones that used to resize it and the
            // one that used to be the title bar. This is what gives them back.
            if (!custom_frame()) {
                break;
            }
            return hit_test(window, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        }

        case WM_SIZE: {
            const bool minimized = wparam == SIZE_MINIMIZED;
            if (minimized != minimized_) {
                minimized_ = minimized;
                if (minimized_handler_) minimized_handler_(minimized_);
            }
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

        case WM_DISPLAYCHANGE: {
            // A mode change, a monitor arriving or one being unplugged. The
            // window can end up on a different display at a different scale
            // without a WM_SIZE, so the client size is re-read here rather than
            // taken from the message: its parameters describe the *screen*.
            RECT client{};
            GetClientRect(window, &client);
            width_  = client.right - client.left;
            height_ = client.bottom - client.top;
            log::info("Display configuration changed ({}x{} client)", width_, height_);
            if (display_handler_) {
                display_handler_();
            }
            return 0;
        }

        case WM_POWERBROADCAST:
            // Resume, in both the forms Windows sends it: automatic wake and
            // user-initiated wake. What follows is a check, not a rebuild — the
            // adapter is usually fine, and treating every resume as a loss
            // would throw playback away for nothing.
            if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
                log::info("System resumed from suspend");
                if (resume_handler_) {
                    resume_handler_();
                }
            }
            return TRUE;

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

    // Outer size from the requested client size. With a minimal frame the two
    // are the same rectangle — WM_NCCALCSIZE gives the client everything — so
    // adjusting for a caption that will not exist would open the window short
    // by its height.
    RECT bounds{0, 0, width, height};
    if (!minimal_frame_) {
        AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
    }

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
    if (!minimal_frame_) {
        AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, FALSE);
    }
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

    apply_frame_appearance();

    // Nothing has asked the window for its non-client size yet, so a window
    // created minimal still has the frame Windows gave it. This is what makes
    // WM_NCCALCSIZE run.
    if (custom_frame()) {
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED |
                         SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    }

    RECT client{};
    GetClientRect(window_, &client);
    width_  = client.right - client.left;
    height_ = client.bottom - client.top;

    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);

    log::info("Window created ({}x{}, {} frame)", width_, height_,
              minimal_frame_ ? "minimal" : "system");
    return true;
}

void AppWindow::apply_frame_appearance() {
    if (!window_) {
        return;
    }

    // Windows draws the caption from the system theme, so a light-themed
    // desktop puts a white title bar above a black application. 20 is
    // DWMWA_USE_IMMERSIVE_DARK_MODE, which mingw's dwmapi.h predates; older
    // Windows builds ignore the attribute rather than failing the call. Set
    // even with a minimal frame, because the frame is a setting and the caption
    // has to be right the moment it comes back.
    const BOOL dark_caption = TRUE;
    DwmSetWindowAttribute(window_, 20, &dark_caption, sizeof(dark_caption));

    // 34 is DWMWA_BORDER_COLOR, Windows 11 and ignored before it. Without it a
    // light-themed desktop draws a pale hairline around a black window, which
    // with no caption above it is the only part of the frame anyone can see.
    // The same colour as the class brush: the border reads as the edge of the
    // application rather than as a line drawn on top of it.
    const COLORREF border = RGB(0x04, 0x06, 0x0A);
    DwmSetWindowAttribute(window_, 34, &border, sizeof(border));
}

int AppWindow::frame_thickness(HWND window, bool vertical) {
    const UINT dpi = static_cast<UINT>(
        (window ? ImGui_ImplWin32_GetDpiScaleForHwnd(window) : 1.0f) * 96.0f);
    // The padded border is the invisible part of the grab handle and applies to
    // both axes, which is why it is CX on a vertical measurement too.
    return metric_for_dpi(vertical ? SM_CYFRAME : SM_CXFRAME, dpi) +
           metric_for_dpi(SM_CXPADDEDBORDER, dpi);
}

LRESULT AppWindow::hit_test(HWND window, POINT screen) const {
    RECT bounds{};
    GetWindowRect(window, &bounds);

    // A maximised window has no resize edges — restoring it is what a drag on
    // one would mean, and Windows does not offer that either.
    if (!IsZoomed(window)) {
        const int  border_x = frame_thickness(window, false);
        const int  border_y = frame_thickness(window, true);
        const bool left     = screen.x < bounds.left + border_x;
        const bool right    = screen.x >= bounds.right - border_x;
        const bool top      = screen.y < bounds.top + border_y;
        const bool bottom   = screen.y >= bounds.bottom - border_y;

        // Corners first. The bands overlap, so whichever is asked about first
        // is the one the pointer gets, and a corner tested after its two edges
        // can never be reached.
        if (top && left)     return HTTOPLEFT;
        if (top && right)    return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (top)             return HTTOP;
        if (bottom)          return HTBOTTOM;
        if (left)            return HTLEFT;
        if (right)           return HTRIGHT;
    }

    // The strip. One return value buys the whole of what a title bar does:
    // dragging, double click to maximise, drag to an edge to snap, Alt+Space,
    // and the system menu on right click — all of it DefWindowProc's.
    //
    // Blocked whenever the interface has something under the pointer, so a
    // control drawn inside the strip keeps its clicks. The answer is a frame
    // old, which is survivable here because the backend goes on feeding ImGui
    // the pointer position through WM_NCMOUSEMOVE even while this returns
    // HTCAPTION: hover state stays live under the strip rather than freezing at
    // the boundary and trapping the pointer outside the client area.
    if (!caption_blocked_ && caption_height_ > 0 &&
        screen.y < bounds.top + caption_height_) {
        return HTCAPTION;
    }

    return HTCLIENT;
}

void AppWindow::set_minimal_frame(bool minimal) {
    if (minimal == minimal_frame_) {
        return;
    }
    minimal_frame_ = minimal;

    // Fullscreen is a WS_POPUP that already has no frame, so there is nothing
    // to apply until it ends — and set_fullscreen asks for the frame again on
    // the way out, which is where this takes effect.
    if (!window_ || fullscreen_) {
        return;
    }

    // No style change is needed: the frame is decided entirely by whether
    // WM_NCCALCSIZE hands its pixels back, and this is what makes Windows ask
    // again. The client area changes size without the window moving, so the
    // WM_SIZE that follows is what resizes the surfaces.
    SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED |
                     SWP_NOOWNERZORDER);
    apply_frame_appearance();
}

void AppWindow::minimize() {
    if (window_) {
        ShowWindow(window_, SW_MINIMIZE);
    }
}

void AppWindow::close() {
    if (window_) {
        PostMessageW(window_, WM_CLOSE, 0, 0);
    }
}

bool AppWindow::maximized() const {
    return window_ && IsZoomed(window_);
}

void AppWindow::toggle_maximize() {
    if (window_) {
        ShowWindow(window_, maximized() ? SW_RESTORE : SW_MAXIMIZE);
    }
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

    // Recorded before the calls below rather than after them. Both paths end in
    // an SWP_FRAMECHANGED, which asks the window procedure for its frame
    // synchronously — and the answer depends on this flag. Set afterwards, the
    // window leaving fullscreen would be measured as though it were still in
    // it, and a minimal frame would come back wearing a caption.
    fullscreen_ = fullscreen;

    if (fullscreen) {
        saved_placement_.length = sizeof(saved_placement_);
        GetWindowPlacement(window_, &saved_placement_);

        const HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO    info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            // Nothing has changed, so the flag set above has to be put back or
            // the window would be described as fullscreen while it is not.
            fullscreen_ = false;
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
}

bool AppWindow::pump_messages(DWORD timeout_ms) {
    if (timeout_ms != 0 && running_) {
        // MWMO_INPUTAVAILABLE is required here, not defensive. PeekMessage
        // marks everything it sees as old, and Microsoft documents that without
        // this flag "the existing unread input (received prior to the last time
        // the thread checked the queue) is ignored" — so a wait entered after
        // the drain below would sleep through anything the drain left behind.
        // The flag makes the wait return whenever input exists at all.
        //
        // Zero handles is the documented "waits only for an input event" form.
        // The window procedure validates every WM_PAINT it is given, so a
        // pending paint cannot hold QS_PAINT set and turn this back into the
        // spin it exists to remove.
        MsgWaitForMultipleObjectsEx(0, nullptr, timeout_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

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
