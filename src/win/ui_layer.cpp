#include "win/ui_layer.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <format>

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

    if (!create_render_target()) {
        error = "Creating the UI render target failed";
        return false;
    }

    if (!ImGui_ImplDX11_Init(device_.get(), context_.get())) {
        error = "ImGui DX11 backend failed to initialize";
        return false;
    }

    log::info("UI layer ready ({}x{})", width_, height_);
    return true;
}

bool UiLayer::create_render_target() {
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.put())))) {
        return false;
    }
    return SUCCEEDED(device_->CreateRenderTargetView(back_buffer.get(), nullptr,
                                                     render_target_.put()));
}

void UiLayer::resize(int width, int height) {
    width  = clamp_dimension(width);
    height = clamp_dimension(height);
    if (!swapchain_ || (width == width_ && height == height_)) {
        return;
    }

    width_  = width;
    height_ = height;

    render_target_.reset();
    const HRESULT hr = swapchain_->ResizeBuffers(0, static_cast<UINT>(width_),
                                                 static_cast<UINT>(height_),
                                                 DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        log::error("ResizeBuffers failed (0x{:08X})", static_cast<unsigned>(hr));
        return;
    }
    create_render_target();
}

void UiLayer::begin_frame() {
    ImGui_ImplDX11_NewFrame();
}

void UiLayer::end_frame() {
    if (!render_target_) {
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

    swapchain_->Present(1, 0);
}

void UiLayer::destroy() {
    ImGui_ImplDX11_Shutdown();
    render_target_.reset();
    swapchain_.reset();
    dxgi_device_.reset();
    context_.reset();
    device_.reset();
}

}  // namespace coax::win
