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
    // Cleared first even when a replacement follows. SetContent is what holds
    // DirectComposition's reference, so this is the only way to drop it on
    // whatever was there — and mpv's previous swap chain has to leave the
    // visual before it can be released. Both calls land in the single Commit
    // below, so the compositor never sees a frame with an empty video visual
    // and the swap is invisible.
    if (swapchain) {
        video_->SetContent(nullptr);
    }
    const HRESULT hr = video_->SetContent(swapchain);
    if (FAILED(hr)) {
        log::error("Attaching video content failed (0x{:08X})", static_cast<unsigned>(hr));
        return;
    }
    commit();
    // Success is not logged here. The caller knows which acquisition path
    // produced the pointer, which epoch it belongs to and whether it is a real
    // replacement, and says so; a second line per call would only duplicate
    // that with less, and a reconfiguration burst calls this repeatedly.
}

void CompositionTree::destroy() {
    // Content first, then the tree, then the device. The visuals hold
    // references on both swap chains, and those have to go before the device
    // they were created from.
    if (video_) video_->SetContent(nullptr);
    if (ui_) ui_->SetContent(nullptr);
    if (root_) root_->RemoveAllVisuals();
    if (target_) target_->SetRoot(nullptr);
    // A device whose D3D11 device has been removed fails this; the teardown is
    // unconditional either way, so the result is not worth reporting.
    commit();

    ui_.reset();
    video_.reset();
    root_.reset();
    target_.reset();
    device_.reset();
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
