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

    // Accepts the swap chain mpv reports through its display-swapchain
    // property. DirectComposition takes its own reference on the content, so
    // the borrowed pointer does not need to outlive this call.
    void set_video_content(IUnknown* swapchain);
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
