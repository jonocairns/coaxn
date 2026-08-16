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

bool CompositionTree::set_video_content(IUnknown* swapchain) {
    if (!video_) {
        return false;
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
        // The clear above is already pending on the visual. Commit it rather
        // than leaving it for whatever unrelated call happens to commit next:
        // until then the visual would keep presenting a swap chain the tree no
        // longer references, and the frame that eventually landed would be an
        // empty video visual at an unpredictable moment.
        commit();
        return false;
    }
    commit();
    // Success is not logged here. The caller knows which acquisition path
    // produced the pointer, which epoch it belongs to and whether it is a real
    // replacement, and says so; a second line per call would only duplicate
    // that with less, and a reconfiguration burst calls this repeatedly.
    return true;
}

void CompositionTree::set_corner_radius(int width, int height, float top, float bottom) {
    if (!device_ || !root_) {
        return;
    }

    if (top <= 0.0f && bottom <= 0.0f) {
        // The cast is not decoration: SetClip is overloaded on IDCompositionClip*
        // and on a rectangle, and a bare nullptr picks neither.
        root_->SetClip(static_cast<IDCompositionClip*>(nullptr));
        commit();
        return;
    }

    if (!clip_ && FAILED(device_->CreateRectangleClip(clip_.put()))) {
        log::warn("Rounded corners are unavailable; drawing square ones");
        return;
    }

    clip_->SetLeft(0.0f);
    clip_->SetTop(0.0f);
    clip_->SetRight(static_cast<float>(width));
    clip_->SetBottom(static_cast<float>(height));

    clip_->SetTopLeftRadiusX(top);
    clip_->SetTopLeftRadiusY(top);
    clip_->SetTopRightRadiusX(top);
    clip_->SetTopRightRadiusY(top);
    clip_->SetBottomLeftRadiusX(bottom);
    clip_->SetBottomLeftRadiusY(bottom);
    clip_->SetBottomRightRadiusX(bottom);
    clip_->SetBottomRightRadiusY(bottom);

    root_->SetClip(clip_.get());
    commit();
}

void CompositionTree::destroy() {
    // Content first, then the tree, then the device. The visuals hold
    // references on both swap chains, and those have to go before the device
    // they were created from.
    if (video_) video_->SetContent(nullptr);
    if (ui_) ui_->SetContent(nullptr);
    if (root_) {
        root_->SetClip(static_cast<IDCompositionClip*>(nullptr));
        root_->RemoveAllVisuals();
    }
    if (target_) target_->SetRoot(nullptr);
    // A device whose D3D11 device has been removed fails this; the teardown is
    // unconditional either way, so the result is not worth reporting.
    commit();

    clip_.reset();
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
