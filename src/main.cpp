#include <windows.h>
#include <shellapi.h>

#include <imgui.h>
#include <imgui_impl_win32.h>

#include <string>

#include "app/app.hpp"
#include "util/log.hpp"

namespace {

// Sized for a television viewing distance rather than a desktop one.
constexpr float kUiScale = 1.45f;

void configure_style() {
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.ScrollbarRounding = 8.0f;
    style.WindowPadding     = ImVec2(14.0f, 12.0f);
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ScaleAllSizes(kUiScale);

    ImGui::GetIO().FontGlobalScale = kUiScale;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Must happen before the window exists so the client area is not scaled by
    // the compositor behind our back.
    ImGui_ImplWin32_EnableDpiAwareness();

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) {
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;  // no layout file beside the executable
    configure_style();

    coax::log::info("Coax native POC starting");

    int exit_code = 1;
    {
        coax::app::App app;

        // A single argument is treated as a media URL to play immediately,
        // which is how the presentation path gets exercised without a provider.
        int    argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            if (argc > 1) {
                const int size = WideCharToMultiByte(CP_UTF8, 0, argv[1], -1,
                                                     nullptr, 0, nullptr, nullptr);
                std::string url(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
                WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, url.data(), size, nullptr, nullptr);
                app.set_direct_media(url);
            }
            LocalFree(argv);
        }

        exit_code = app.run();
    }

    ImGui::DestroyContext();
    CoUninitialize();
    return exit_code;
}
