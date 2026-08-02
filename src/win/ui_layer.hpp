#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>

#include <string>

#include "win/com_ptr.hpp"

namespace coax::win {

// Owns the application's D3D11 device and the transparent swap chain the UI
// draws into. The swap chain is created for composition (no HWND of its own)
// so it can be placed in the application's DirectComposition tree above the
// video visual.
class UiLayer {
public:
    bool create(int width, int height, std::string& error);
    void destroy();

    void resize(int width, int height);

    // Clears to transparent and starts an ImGui frame.
    void begin_frame();
    // Renders the ImGui draw data and presents.
    void end_frame();

    [[nodiscard]] ID3D11Device*    device()       const { return device_.get(); }
    [[nodiscard]] IDXGIDevice*     dxgi_device()  const { return dxgi_device_.get(); }
    [[nodiscard]] IDXGISwapChain1* swapchain()    const { return swapchain_.get(); }

private:
    bool create_render_target();

    ComPtr<ID3D11Device>           device_;
    ComPtr<ID3D11DeviceContext>    context_;
    ComPtr<IDXGIDevice>            dxgi_device_;
    ComPtr<IDXGISwapChain1>        swapchain_;
    ComPtr<ID3D11RenderTargetView> render_target_;

    int width_  = 0;
    int height_ = 0;
};

}  // namespace coax::win
