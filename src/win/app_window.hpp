#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace coax::win {

// A single top-level window. Everything the user sees — video and UI — is
// composited into this one HWND, which is the property application-window
// capture depends on.
class AppWindow {
public:
    using ResizeHandler = std::function<void(int width, int height)>;
    using CloseHandler  = std::function<void()>;
    using PaintHandler  = std::function<void()>;
    using DpiHandler    = std::function<void(float scale)>;
    using DisplayHandler = std::function<void()>;
    using MinimizedHandler = std::function<void(bool minimized)>;
    using ResumeHandler  = std::function<void()>;

    // The size is in logical pixels; the window is created at that size scaled
    // by whatever its monitor is running at.
    bool create(const wchar_t* title, int width, int height, std::string& error);
    void destroy();

    // Processes pending messages, first waiting up to `timeout_ms` for one to
    // arrive. Zero drains and returns immediately, which is what a turn ending
    // in a present wants: the vsync wait inside that present is the throttle,
    // and waiting here as well would drop frames. A turn that will *not*
    // present has no throttle at all, and passing the time until its next
    // deadline is what keeps it from spinning a core. Returns false once the
    // window has closed.
    bool pump_messages(DWORD timeout_ms = 0);

    void set_fullscreen(bool fullscreen);
    [[nodiscard]] bool fullscreen() const { return fullscreen_; }

    // Whether the window keeps its caption. Off, the client area covers the
    // whole window and the strip below stands in for the title bar. Every
    // WS_OVERLAPPEDWINDOW style bit is kept either way — what changes is only
    // whether WM_NCCALCSIZE hands the frame's pixels back — so the shadow,
    // Aero Snap, the minimise animation and the Alt-Tab thumbnail are the
    // system's in both modes. Safe to call before the window exists, and
    // applied immediately when it does.
    void set_minimal_frame(bool minimal);
    [[nodiscard]] bool minimal_frame() const { return minimal_frame_; }

    // The band along the top edge that behaves as a title bar: drag, double
    // click to maximise, drag to an edge to snap, and the system menu on right
    // click. Physical pixels, because the caller has already scaled it; zero
    // means the window has no draggable strip at all.
    void set_caption_height(int pixels) { caption_height_ = pixels; }

    // Whether the interface has something under the pointer that wants the
    // click. Published once per frame rather than asked for, because the hit
    // test runs inside the window procedure and must not reach into the UI.
    // One frame stale by construction, which the hit test is built to tolerate.
    void set_caption_blocked(bool blocked) { caption_blocked_ = blocked; }

    void minimize();
    // Posts rather than destroys, so the request leaves through WM_CLOSE and
    // the ordinary shutdown path exactly as the system button did. Unguarded
    // below because posting is what makes it safe: the message is handled after
    // whatever is running now, which is the same thing the guard enforces.
    void close();
    [[nodiscard]] bool maximized() const;
    void toggle_maximize();
    // Whether the desktop rounds window corners — Windows 11 and later. False
    // on the versions that square them, where rounding what is drawn inside
    // would be the mismatch rather than the fix.
    [[nodiscard]] bool rounds_windows() const { return rounds_windows_; }

    // Held for as long as a frame is being drawn. The reason every call above
    // has to be made from the loop rather than from anything drawing, and the
    // only place that reason is written down.
    //
    // They all resize the client area, and the SetWindowPos that does it
    // delivers WM_SIZE synchronously — from which this class draws, because a
    // resize drag owns the thread and the picture would otherwise stretch for
    // the length of the gesture. Called mid-frame that is a frame begun inside
    // a frame and a render target rebuilt under a live draw list. ImGui checks
    // for exactly this and the check is compiled out of release builds, so the
    // result is a crash with nothing to read.
    //
    // Hence the refusal: a call from the wrong place is dropped and logged
    // instead of taking the process with it. Scoped rather than paired calls
    // for the same reason ScopedStyle is — a return added to the draw path
    // later would leave the flag raised and refuse everything after it.
    class FrameScope {
    public:
        explicit FrameScope(AppWindow& window) : window_(window) {
            window_.frame_in_progress_ = true;
        }
        ~FrameScope() { window_.frame_in_progress_ = false; }

        FrameScope(const FrameScope&)            = delete;
        FrameScope& operator=(const FrameScope&) = delete;

    private:
        AppWindow& window_;
    };

    void on_resize(ResizeHandler handler) { resize_handler_ = std::move(handler); }
    void on_close(CloseHandler handler)   { close_handler_  = std::move(handler); }
    // Called when the window needs a frame while the application's own loop
    // cannot run — the modal loop Windows enters for a resize drag owns the
    // thread until the mouse is released, and delivers WM_SIZE from inside it.
    void on_paint(PaintHandler handler)   { paint_handler_  = std::move(handler); }
    void on_dpi_changed(DpiHandler handler) { dpi_handler_   = std::move(handler); }
    // A monitor was added, removed or changed mode. Neither WM_SIZE nor
    // WM_DPICHANGED is guaranteed to follow, so the client size and the scale
    // both have to be re-read rather than waited for.
    void on_display_change(DisplayHandler handler) { display_handler_ = std::move(handler); }
    void on_minimized_changed(MinimizedHandler handler) {
        minimized_handler_ = std::move(handler);
    }
    // The machine resumed from sleep. The adapter may have been reset while it
    // was suspended, which nothing reports until work is submitted to it.
    void on_resume(ResumeHandler handler) { resume_handler_ = std::move(handler); }

    [[nodiscard]] HWND handle() const { return window_; }
    [[nodiscard]] int  width()  const { return width_; }
    [[nodiscard]] int  height() const { return height_; }
    [[nodiscard]] bool minimized() const { return minimized_; }
    // 1.0 at 96 DPI, 1.5 at 150%, and so on.
    [[nodiscard]] float dpi_scale() const;

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    // Whether the frame is being drawn by this class rather than by Windows.
    // Fullscreen is already a WS_POPUP with no frame to remove, so the two
    // never overlap.
    [[nodiscard]] bool custom_frame() const { return minimal_frame_ && !fullscreen_; }
    // The resize border, which is also the amount a maximised window overhangs
    // its monitor by. Takes the window rather than reading the member so it can
    // be called from the messages that arrive during CreateWindowEx.
    [[nodiscard]] static int frame_thickness(HWND window, bool vertical);
    // The narrowed resize band left showing past a control in the strip, so the
    // window stays resizable along the edge its buttons occupy.
    [[nodiscard]] static int control_resize_band(HWND window);
    [[nodiscard]] LRESULT hit_test(HWND window, POINT screen) const;
    void apply_frame_appearance();
    // Whether `what` is being asked for from inside a frame, in which case it
    // has already been logged and must not be carried out.
    [[nodiscard]] bool refuse_during_frame(const char* what) const;

    HWND          window_ = nullptr;
    HBRUSH        background_brush_ = nullptr;
    int           width_  = 0;
    int           height_ = 0;
    bool          running_ = true;
    bool          fullscreen_ = false;
    bool          minimized_ = false;
    bool          minimal_frame_ = false;
    int           caption_height_ = 0;
    bool          caption_blocked_ = false;
    bool          frame_in_progress_ = false;
    bool          rounds_windows_ = false;
    WINDOWPLACEMENT saved_placement_{};
    ResizeHandler  resize_handler_;
    CloseHandler   close_handler_;
    PaintHandler   paint_handler_;
    DpiHandler     dpi_handler_;
    DisplayHandler display_handler_;
    MinimizedHandler minimized_handler_;
    ResumeHandler  resume_handler_;
};

}  // namespace coax::win
