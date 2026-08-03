#pragma once

#include <windows.h>
#include <dcomp.h>
#include <dxgi1_3.h>

#include <string>

#include "win/com_ptr.hpp"

namespace coax::win {

// The application's DirectComposition tree. Video and UI are sibling visuals
// under one root bound to one top-level HWND, which is what makes the window
// capture as a single surface.
//
// Ordering matters: the video visual is added first so the UI visual composites
// above it.
class CompositionTree {
public:
    bool create(HWND window, IDXGIDevice* dxgi_device, std::string& error);
    // Releases the tree and everything it holds, content first. Used when the
    // device underneath it is lost and the whole surface is rebuilt.
    void destroy();

    // Accepts the swap chain mpv reports through its display-swapchain
    // property. DirectComposition takes its own reference on the content, and
    // holds it until the content is replaced or cleared — so a caller may not
    // assume the visual is done with the pointer when this returns, and may not
    // release its own reference until after the content has been cleared.
    // Taking a reference is the caller's job regardless: the pointer arrives
    // from mpv unowned.
    //
    // Returns whether the visual now holds exactly what was asked for. A
    // refused attach is reported rather than only logged, because the caller
    // records attachment state that diagnostics are read from, and an
    // attachment the tree rejected would otherwise be reported as live.
    [[nodiscard]] bool set_video_content(IUnknown* swapchain);
    void set_ui_content(IUnknown* swapchain);

    void commit();

    [[nodiscard]] bool valid() const { return static_cast<bool>(device_); }

private:
    ComPtr<IDCompositionDevice> device_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual> root_;
    ComPtr<IDCompositionVisual> video_;
    ComPtr<IDCompositionVisual> ui_;
};

}  // namespace coax::win
