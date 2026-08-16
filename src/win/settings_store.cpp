#include "win/settings_store.hpp"

#include <windows.h>

#include <string>
#include <vector>

#include "util/log.hpp"
#include "win/app_paths.hpp"
#include "win/file_write.hpp"

namespace coax::win {
namespace {

// Matches the cap the credential store applies to its own blob. The file this
// writes is one short line; anything approaching a megabyte is not a settings
// file, and reading it into memory is not worth finding out what it is.
constexpr LONGLONG kMaxFileBytes = 1 << 20;

std::wstring storage_path() {
    const std::wstring directory = app_data_dir();
    if (directory.empty()) {
        return {};
    }
    return directory + L"\\settings.txt";
}

}  // namespace

core::Settings SettingsStore::load() {
    const std::wstring path = storage_path();
    if (path.empty()) {
        return {};
    }

    // FILE_SHARE_READ so that having the file open in an editor — which is half
    // the reason it is plaintext — does not stop the application starting.
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        // The first run, and the common case. Not worth a log line.
        return {};
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > kMaxFileBytes) {
        CloseHandle(file);
        return {};
    }

    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD       read = 0;
    const bool  read_ok =
        ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(file);

    if (!read_ok) {
        log::warn("Settings could not be read; using defaults");
        return {};
    }

    // A short read is not a failure to parse: the parser tolerates a truncated
    // last line, so whatever did arrive is still worth reading.
    text.resize(read);
    return core::parse(text);
}

bool SettingsStore::save(const core::Settings& settings) {
    const std::wstring path = storage_path();
    if (path.empty()) {
        return false;
    }

    // Replaced rather than rewritten in place. The old file is worth more than
    // the new one until the new one is complete: a write cut short would
    // otherwise cost the frame and volume someone had chosen, and hand them
    // defaults on the next launch with nothing to say why.
    return write_file_atomically(path, core::serialize(settings), "Settings");
}

}  // namespace coax::win
