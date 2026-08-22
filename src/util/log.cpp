#include "util/log.hpp"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <mutex>

#include "util/log_ring.hpp"
#include "win/app_paths.hpp"

namespace coax::log {
namespace {

constexpr std::size_t kMaxRetained = 400;

Ring g_recent{kMaxRetained};

// Guards the session log stream only. Separate from the ring's own lock so a
// blocking write to a file on a stalled disk does not also hold up the
// diagnostics panel, and so write() no longer locks the same mutex twice.
std::mutex g_file_mutex;

std::wstring executable_log_path() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD  length = GetModuleFileNameW(nullptr, path.data(),
                                             static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    path.resize(length);

    const auto slash = path.find_last_of(L'\\');
    path = (slash == std::wstring::npos) ? std::wstring{} : path.substr(0, slash + 1);
    path += L"coax.log";
    return path;
}

// A GUI-subsystem process has no console, so the session log is the only way
// to see what happened after the fact. Installed builds cannot write beside
// their executable under Program Files, so keep it with the other per-user
// application data. A portable build can still log beside itself when the
// known folder cannot be resolved or opened.
std::FILE* session_log() {
    static std::FILE* file = [] () -> std::FILE* {
        const std::wstring directory = win::app_data_dir();
        if (!directory.empty()) {
            const std::wstring path = directory + L"\\coax.log";
            if (std::FILE* preferred = _wfopen(path.c_str(), L"w")) {
                return preferred;
            }
        }

        const std::wstring fallback = executable_log_path();
        return fallback.empty() ? nullptr : _wfopen(fallback.c_str(), L"w");
    }();
    return file;
}

const char* level_tag(Level level) {
    switch (level) {
        case Level::Debug: return "DBG";
        case Level::Info:  return "INF";
        case Level::Warn:  return "WRN";
        case Level::Error: return "ERR";
    }
    return "???";
}

}  // namespace

void write(Level level, std::string_view message) {
    const auto now  = std::chrono::system_clock::now();
    const auto secs = std::chrono::floor<std::chrono::seconds>(now);
    const auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(now - secs);

    std::string line = std::format("[{:%H:%M:%S}.{:03}] {} {}",
                                   secs, ms.count(), level_tag(level), message);

    g_recent.push(line);

    line.push_back('\n');
    OutputDebugStringA(line.c_str());

    if (std::FILE* file = session_log()) {
        std::scoped_lock lock(g_file_mutex);
        std::fputs(line.c_str(), file);
        // Flushed per line because the process this records is the one that may
        // be about to die; a buffered tail is exactly what would be lost.
        std::fflush(file);
    }
}

std::vector<std::string> recent() {
    return g_recent.snapshot();
}

void recent_into(std::vector<std::string>& out) {
    g_recent.snapshot_into(out);
}

}  // namespace coax::log
