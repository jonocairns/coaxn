#include <windows.h>
#include <shellapi.h>

#include <imgui.h>
#include <imgui_impl_win32.h>

#include <string>

#include "app/app.hpp"
#include "app/theme.hpp"
#include "util/log.hpp"

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
    coax::app::theme::configure_style();

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
