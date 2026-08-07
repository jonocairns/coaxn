#include "win/ui_layer.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <format>
#include <utility>

#include "util/log.hpp"

namespace coax::win {
namespace {

constexpr int clamp_dimension(int value) {
    return value < 1 ? 1 : value;
}

}  // namespace

bool UiLayer::create(int width, int height, std::string& error) {
    width_  = clamp_dimension(width);
    height_ = clamp_dimension(height);

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
    // Falls back below if the debug layer is not installed.
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, static_cast<UINT>(std::size(levels)),
                                   D3D11_SDK_VERSION, device_.put(), nullptr, context_.put());
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               levels, static_cast<UINT>(std::size(levels)),
                               D3D11_SDK_VERSION, device_.put(), nullptr, context_.put());
    }
    if (FAILED(hr)) {
        error = std::format("D3D11CreateDevice failed (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    hr = device_.get()->QueryInterface(IID_PPV_ARGS(dxgi_device_.put()));
    if (FAILED(hr)) {
        error = "Querying IDXGIDevice failed";
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device_->GetAdapter(adapter.put()))) {
        error = "GetAdapter failed";
        return false;
    }

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter.get()->GetParent(IID_PPV_ARGS(factory.put())))) {
        error = "Getting IDXGIFactory2 failed";
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width       = static_cast<UINT>(width_);
    desc.Height      = static_cast<UINT>(height_);
    desc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc  = {1, 0};
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    // Premultiplied alpha lets the video visual show through wherever the UI
    // has not drawn.
    desc.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;

    hr = factory->CreateSwapChainForComposition(device_.get(), &desc, nullptr,
                                                swapchain_.put());
    if (FAILED(hr)) {
        error = std::format("CreateSwapChainForComposition failed (0x{:08X})",
                            static_cast<unsigned>(hr));
        return false;
    }

    if (const HRESULT target = create_render_target(); FAILED(target)) {
        error = std::format("Creating the UI render target failed (0x{:08X})",
                            static_cast<unsigned>(target));
        return false;
    }

    if (!ImGui_ImplDX11_Init(device_.get(), context_.get())) {
        error = "ImGui DX11 backend failed to initialize";
        return false;
    }
    imgui_ready_ = true;

    log::info("UI layer ready ({}x{})", width_, height_);
    return true;
}

void UiLayer::note_result(HRESULT hr, const char* operation) {
    if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) {
        if (FAILED(hr)) {
            log::warn("{} failed (0x{:08X})", operation, static_cast<unsigned>(hr));
        }
        return;
    }

    const core::DeviceLossKind kind = hr == DXGI_ERROR_DEVICE_REMOVED
                                          ? core::DeviceLossKind::Removed
                                          : core::DeviceLossKind::Reset;
    // Presenting into a removed device fails every frame from here on, so this
    // is where the repetition stops: one report per loss, not one per frame.
    if (!loss_.raise(kind)) {
        return;
    }

    // The result of Present says only that the device is gone. The removal
    // reason says what took it — a hang, a driver upgrade, an internal driver
    // error — which is the part worth having in a log after the fact.
    const HRESULT reason = device_ ? device_->GetDeviceRemovedReason() : hr;
    pending_loss_ = DeviceLossReport{
        kind, std::format("{} on {} (removal reason 0x{:08X})", core::to_string(kind),
                          operation, static_cast<unsigned>(reason))};
    log::error("D3D11 device lost: {}", pending_loss_->detail);
}

bool UiLayer::verify_device() {
    if (!device_) {
        return false;
    }
    const HRESULT reason = device_->GetDeviceRemovedReason();
    if (SUCCEEDED(reason)) {
        return true;
    }
    // GetDeviceRemovedReason returns the specific cause, not the two results
    // Present reports. Anything other than a reset is treated as a removal;
    // the exact HRESULT is carried in the report either way.
    note_result(reason == DXGI_ERROR_DEVICE_RESET ? DXGI_ERROR_DEVICE_RESET
                                                  : DXGI_ERROR_DEVICE_REMOVED,
                "device verification");
    return false;
}

std::optional<DeviceLossReport> UiLayer::take_device_loss() {
    return std::exchange(pending_loss_, std::nullopt);
}

HRESULT UiLayer::create_render_target() {
    ComPtr<ID3D11Texture2D> back_buffer;
    // Both results are carried out rather than collapsed to a Boolean. One of
    // them can be a device removal, which has to reach note_result for recovery
    // to start at all; the rest have to reach a log to be diagnosable. A bare
    // false says neither.
    if (const HRESULT hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.put()));
        FAILED(hr)) {
        return hr;
    }
    return device_->CreateRenderTargetView(back_buffer.get(), nullptr, render_target_.put());
}

void UiLayer::resize(int width, int height) {
    width  = clamp_dimension(width);
    height = clamp_dimension(height);
    if (!swapchain_ || (width == width_ && height == height_)) {
        return;
    }

    render_target_.reset();
    const HRESULT resized = swapchain_->ResizeBuffers(0, static_cast<UINT>(width),
                                                      static_cast<UINT>(height),
                                                      DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(resized)) {
        // Resizing is the other place a removed adapter surfaces, and a window
        // being dragged between displays hits it before any present does.
        note_result(resized, "ResizeBuffers");
        return;
    }

    if (const HRESULT target = create_render_target(); FAILED(target)) {
        // The buffers are the new size and nothing can draw into them. Nothing
        // later reports that either, because end_frame stops before Present, so
        // this is the one place the failure can be classified.
        note_result(target, "render target creation on resize");
        return;
    }

    // Recorded only now. A resize is complete once the buffers and a target
    // over them both exist; committing the size before the target means a
    // same-size retry short-circuits on the check above and the surface stays
    // unusable for the rest of the process.
    width_  = width;
    height_ = height;
}

void UiLayer::begin_frame() {
    ImGui_ImplDX11_NewFrame();
}

void UiLayer::end_frame() {
    // Nothing submitted to a lost device can succeed, and the rebuild is
    // already queued. Drawing anyway would burn a frame's work to produce the
    // same error the latch has already reported.
    if (!render_target_ || loss_.lost()) {
        return;
    }

    ID3D11RenderTargetView* target = render_target_.get();
    context_->OMSetRenderTargets(1, &target, nullptr);

    // Very nearly transparent, but deliberately NOT alpha zero.
    //
    // DWM optimises fully transparent content out of composition. If the UI
    // layer contributes nothing — which is exactly what happens when the
    // channel list is hidden over fullscreen video — DWM is free to promote
    // the video to Independent Flip or a hardware overlay plane and bypass
    // composition entirely. Capture APIs (Windows.Graphics.Capture, Desktop
    // Duplication) read the *composited* output, so a bypassed video plane
    // captures as black while the UI still captures fine. That is precisely
    // the failure this architecture exists to eliminate.
    //
    // One alpha step keeps the layer composited. Premultiplied against black
    // it darkens the video by 0.4%, which is not perceptible.
    constexpr float kAlmostTransparent[4] = {0.0f, 0.0f, 0.0f, 1.0f / 255.0f};
    context_->ClearRenderTargetView(target, kAlmostTransparent);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    note_result(swapchain_->Present(1, 0), "Present");
}

void UiLayer::destroy() {
    if (imgui_ready_) {
        ImGui_ImplDX11_Shutdown();
        imgui_ready_ = false;
    }
    render_target_.reset();
    swapchain_.reset();
    dxgi_device_.reset();
    context_.reset();
    device_.reset();
    loss_.clear();
    pending_loss_.reset();
    // Zeroed so a rebuild at the same size still creates its render target
    // rather than being short-circuited by resize()'s no-change check.
    width_  = 0;
    height_ = 0;
}

}  // namespace coax::win
