#include "win/composition.hpp"

#include <format>

#include "util/log.hpp"

namespace coax::win {

bool CompositionTree::create(HWND window, IDXGIDevice* dxgi_device, std::string& error) {
    HRESULT hr = DCompositionCreateDevice(dxgi_device, IID_PPV_ARGS(device_.put()));
    if (FAILED(hr)) {
        error = std::format("DCompositionCreateDevice failed (0x{:08X})",
                            static_cast<unsigned>(hr));
        return false;
    }

    // TRUE: this target owns the whole window, so nothing else draws beneath it.
    hr = device_->CreateTargetForHwnd(window, TRUE, target_.put());
    if (FAILED(hr)) {
        error = std::format("CreateTargetForHwnd failed (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    if (FAILED(device_->CreateVisual(root_.put())) ||
        FAILED(device_->CreateVisual(video_.put())) ||
        FAILED(device_->CreateVisual(ui_.put()))) {
        error = "CreateVisual failed";
        return false;
    }

    // Video first, UI second: later children composite on top.
    root_->AddVisual(video_.get(), TRUE, nullptr);
    root_->AddVisual(ui_.get(), FALSE, nullptr);
    target_->SetRoot(root_.get());

    commit();
    log::info("DirectComposition tree created");
    return true;
}

void CompositionTree::set_video_content(IUnknown* swapchain) {
    if (!video_) {
        return;
    }
    const HRESULT hr = video_->SetContent(swapchain);
    if (FAILED(hr)) {
        log::error("Attaching video content failed (0x{:08X})", static_cast<unsigned>(hr));
        return;
    }
    commit();
    log::info("Video swap chain {} composition tree",
              swapchain ? "attached to" : "detached from");
}

void CompositionTree::set_ui_content(IUnknown* swapchain) {
    if (!ui_) {
        return;
    }
    if (FAILED(ui_->SetContent(swapchain))) {
        log::error("Attaching UI content failed");
        return;
    }
    commit();
}

void CompositionTree::commit() {
    if (device_) {
        device_->Commit();
    }
}

}  // namespace coax::win
