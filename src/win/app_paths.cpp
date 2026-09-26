#include "win/app_paths.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

namespace coax::win {

std::wstring app_data_dir() {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data))) {
        return {};
    }

    std::wstring directory = local_app_data;
    CoTaskMemFree(local_app_data);

    directory += L"\\Coax";
    // Return value ignored deliberately: the interesting failure is the one the
    // caller hits when it opens the file, and ERROR_ALREADY_EXISTS is the normal
    // case rather than a problem.
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

void reveal_in_explorer(const std::wstring& path) {
    const std::wstring arguments = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

}  // namespace coax::win
