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

    // The size is in logical pixels; the window is created at that size scaled
    // by whatever its monitor is running at.
    bool create(const wchar_t* title, int width, int height, std::string& error);
    void destroy();

    // Processes pending messages. Returns false once the window has closed.
    bool pump_messages();

    void set_fullscreen(bool fullscreen);
    [[nodiscard]] bool fullscreen() const { return fullscreen_; }

    void on_resize(ResizeHandler handler) { resize_handler_ = std::move(handler); }
    void on_close(CloseHandler handler)   { close_handler_  = std::move(handler); }
    // Called when the window needs a frame while the application's own loop
    // cannot run — the modal loop Windows enters for a resize drag owns the
    // thread until the mouse is released, and delivers WM_SIZE from inside it.
    void on_paint(PaintHandler handler)   { paint_handler_  = std::move(handler); }
    void on_dpi_changed(DpiHandler handler) { dpi_handler_   = std::move(handler); }

    [[nodiscard]] HWND handle() const { return window_; }
    [[nodiscard]] int  width()  const { return width_; }
    [[nodiscard]] int  height() const { return height_; }
    // 1.0 at 96 DPI, 1.5 at 150%, and so on.
    [[nodiscard]] float dpi_scale() const;

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    HWND          window_ = nullptr;
    HBRUSH        background_brush_ = nullptr;
    int           width_  = 0;
    int           height_ = 0;
    bool          running_ = true;
    bool          fullscreen_ = false;
    WINDOWPLACEMENT saved_placement_{};
    ResizeHandler resize_handler_;
    CloseHandler  close_handler_;
    PaintHandler  paint_handler_;
    DpiHandler    dpi_handler_;
};

}  // namespace coax::win
