#include "util/log.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <iterator>
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

// Retained for the process lifetime. Windows closes it on process teardown,
// which also removes the delete-on-close claim file after a crash.
HANDLE g_primary_log_claim = INVALID_HANDLE_VALUE;

// Whichever file session_log() settled on, so it can be copied out later.
std::wstring g_session_log_path;

std::wstring executable_directory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD  length = GetModuleFileNameW(nullptr, path.data(),
                                             static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    path.resize(length);

    const auto slash = path.find_last_of(L'\\');
    return (slash == std::wstring::npos) ? std::wstring{} : path.substr(0, slash);
}

std::wstring path_in(std::wstring_view directory, std::wstring_view filename) {
    std::wstring path{directory};
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path += L'\\';
    }
    path += filename;
    return path;
}

std::FILE* open_session_log_in(std::wstring_view directory) {
    // The claim is a file rather than a process-local mutex so installed and
    // portable copies, and separate Windows sessions, all coordinate on the
    // actual destination. Delete-on-close also makes a killed process release
    // it without relying on graceful shutdown.
    const std::wstring claim_path = path_in(directory, L"coax.log.lock");
    const HANDLE claim = CreateFileW(
        claim_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);

    if (claim != INVALID_HANDLE_VALUE) {
        const std::wstring primary_path = path_in(directory, L"coax.log");
        if (std::FILE* primary = _wfopen(primary_path.c_str(), L"w")) {
            g_primary_log_claim = claim;
            g_session_log_path = primary_path;
            return primary;
        }
        CloseHandle(claim);
    }

    // A concurrent process owns coax.log. Give this session a process-specific
    // file so neither process can truncate or interleave the other's evidence.
    const std::wstring collision_name =
        L"coax-" + std::to_wstring(GetCurrentProcessId()) + L".log";
    const std::wstring collision_path = path_in(directory, collision_name);
    std::FILE* collision = _wfopen(collision_path.c_str(), L"w");
    if (collision) g_session_log_path = collision_path;
    return collision;
}

// A GUI-subsystem process has no console, so the session log is the only way
// to see what happened after the fact. Installed builds cannot write beside
// their executable under Program Files, so keep it with the other per-user
// application data. Concurrent instances use separate process-specific files,
// and a portable build can still log beside itself when the known folder cannot
// be resolved or opened.
std::FILE* session_log() {
    static std::FILE* file = [] () -> std::FILE* {
        const std::wstring directory = win::app_data_dir();
        if (!directory.empty()) {
            if (std::FILE* preferred = open_session_log_in(directory)) {
                return preferred;
            }
        }

        const std::wstring fallback = executable_directory();
        return fallback.empty() ? nullptr : open_session_log_in(fallback);
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

std::optional<SavedLog> save_copy(std::string& error) {
    std::FILE* file = session_log();
    // Read once: a detached save can still be running as the process exits.
    const std::wstring source_path = file ? g_session_log_path : std::wstring{};
    if (!file || source_path.empty()) {
        error = "There is no session log to save";
        return std::nullopt;
    }

    // Only the flush and the length are taken under the writer's lock. The
    // copy then reads exactly that many bytes -- ending on a whole line --
    // without holding up every thread that logs while it runs.
    long long length = -1;
    {
        std::scoped_lock lock(g_file_mutex);
        if (std::fflush(file) == 0) length = _ftelli64(file);
    }
    if (length < 0) {
        error = "Could not flush the session log";
        return std::nullopt;
    }

    const auto slash = source_path.find_last_of(L'\\');
    const std::wstring directory = path_in(
        slash == std::wstring::npos ? std::wstring{} : source_path.substr(0, slash),
        L"logs");
    // Already existing is the normal case; a real failure surfaces below.
    CreateDirectoryW(directory.c_str(), nullptr);

    const HANDLE source = CreateFileW(
        source_path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        error = std::format("Could not read the session log (error {})", GetLastError());
        return std::nullopt;
    }

    // The process id keeps two sessions saving in the same second apart; the
    // suffix does the same for one session saving twice. CREATE_NEW never
    // overwrites an earlier save.
    SYSTEMTIME local{};
    GetLocalTime(&local);
    std::wstring destination;
    HANDLE target = INVALID_HANDLE_VALUE;
    for (int copy = 1; copy <= 9 && target == INVALID_HANDLE_VALUE; ++copy) {
        wchar_t name[80];
        swprintf(name, std::size(name), copy == 1 ? L"coax-%04u%02u%02u-%02u%02u%02u-%lu.log"
                                                  : L"coax-%04u%02u%02u-%02u%02u%02u-%lu-%d.log",
                 local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
                 local.wSecond, GetCurrentProcessId(), copy);
        destination = path_in(directory, name);
        target = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (target == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (target == INVALID_HANDLE_VALUE) {
        error = std::format("Could not create the saved log (error {})", GetLastError());
        CloseHandle(source);
        return std::nullopt;
    }

    std::vector<char> buffer(1 << 20);
    long long remaining = length;
    bool copied = true;
    while (remaining > 0 && copied) {
        const DWORD want = static_cast<DWORD>(
            std::min<long long>(remaining, static_cast<long long>(buffer.size())));
        DWORD read = 0;
        DWORD written = 0;
        copied = ReadFile(source, buffer.data(), want, &read, nullptr) && read > 0 &&
                 WriteFile(target, buffer.data(), read, &written, nullptr) && written == read;
        remaining -= read;
    }
    const DWORD copy_error = copied ? ERROR_SUCCESS : GetLastError();
    CloseHandle(source);
    CloseHandle(target);
    if (!copied) {
        DeleteFileW(destination.c_str());
        error = std::format("Could not save the log (error {})", copy_error);
        return std::nullopt;
    }

    std::string display(static_cast<std::size_t>(WideCharToMultiByte(
        CP_UTF8, 0, destination.c_str(), static_cast<int>(destination.size()),
        nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, destination.c_str(), static_cast<int>(destination.size()),
                        display.data(), static_cast<int>(display.size()), nullptr, nullptr);
    return SavedLog{destination, std::move(display)};
}

std::vector<std::string> recent() {
    return g_recent.snapshot();
}

void recent_into(std::vector<std::string>& out) {
    g_recent.snapshot_into(out);
}

}  // namespace coax::log
