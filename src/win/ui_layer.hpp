#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>

#include <optional>
#include <string>

#include "core/presentation.hpp"
#include "win/com_ptr.hpp"

namespace coax::win {

struct DeviceLossReport {
    core::DeviceLossKind kind;
    // What failed and what GetDeviceRemovedReason said about it. The HRESULT is
    // the only part that distinguishes a driver upgrade from a hang, so it is
    // carried verbatim rather than folded into the kind.
    std::string detail;
};

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

    // A resume can find the adapter already gone with no call having failed
    // yet, because DXGI only reports removal through work submitted to it.
    // Returns false when the device is dead, having latched the loss.
    bool verify_device();

    // Takes the pending loss report, if this is the first time it has been
    // asked for since the loss. Exactly one report per loss episode, however
    // many frames present into a dead device in the meantime.
    [[nodiscard]] std::optional<DeviceLossReport> take_device_loss();
    [[nodiscard]] bool device_lost() const { return loss_.lost(); }

    [[nodiscard]] ID3D11Device*    device()       const { return device_.get(); }
    [[nodiscard]] IDXGIDevice*     dxgi_device()  const { return dxgi_device_.get(); }
    [[nodiscard]] IDXGISwapChain1* swapchain()    const { return swapchain_.get(); }

private:
    bool create_render_target();
    // Classifies a DXGI result and latches a loss report if it is one.
    void note_result(HRESULT hr, const char* operation);

    ComPtr<ID3D11Device>           device_;
    ComPtr<ID3D11DeviceContext>    context_;
    ComPtr<IDXGIDevice>            dxgi_device_;
    ComPtr<IDXGISwapChain1>        swapchain_;
    ComPtr<ID3D11RenderTargetView> render_target_;

    core::DeviceLossLatch          loss_;
    std::optional<DeviceLossReport> pending_loss_;
    // Whether ImGui's D3D11 backend was initialized against this device. A
    // rebuild that fails partway must not shut down a backend that was never
    // started, which trips an assertion inside ImGui rather than failing.
    bool imgui_ready_ = false;

    int width_  = 0;
    int height_ = 0;
};

}  // namespace coax::win
