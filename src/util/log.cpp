#include "util/log.hpp"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <mutex>

namespace coax::log {
namespace {

constexpr std::size_t kMaxRetained = 400;

std::mutex               g_mutex;
std::vector<std::string> g_recent;

// A GUI-subsystem process has no console, so the session log is the only way
// to see what happened after the fact. It sits beside the executable rather
// than in a known folder so it is findable without resolving shell paths --
// which is itself something that can fail before any logging exists to say so.
std::FILE* session_log() {
    static std::FILE* file = [] () -> std::FILE* {
        std::wstring path(MAX_PATH, L'\0');
        const DWORD  length = GetModuleFileNameW(nullptr, path.data(),
                                                 static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) {
            return nullptr;
        }
        path.resize(length);

        const auto slash = path.find_last_of(L'\\');
        path = (slash == std::wstring::npos) ? std::wstring{} : path.substr(0, slash + 1);
        path += L"coax.log";

        return _wfopen(path.c_str(), L"w");
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

    {
        std::scoped_lock lock(g_mutex);
        if (g_recent.size() >= kMaxRetained) {
            g_recent.erase(g_recent.begin());
        }
        g_recent.push_back(line);
    }

    line.push_back('\n');
    OutputDebugStringA(line.c_str());

    if (std::FILE* file = session_log()) {
        std::scoped_lock lock(g_mutex);
        std::fputs(line.c_str(), file);
        std::fflush(file);
    }
}

const std::vector<std::string>& recent() {
    return g_recent;
}

std::string redact_stream_url(std::string_view url) {
    // Xtream live URLs are /live/<user>/<pass>/<id>.<ext>; mask the two
    // segments after the /live/ (or /movie/, /series/) marker.
    for (std::string_view marker : {"/live/", "/movie/", "/series/"}) {
        const auto pos = url.find(marker);
        if (pos == std::string_view::npos) {
            continue;
        }

        const auto creds_start = pos + marker.size();
        auto       cursor      = creds_start;
        int        segments    = 0;
        while (segments < 2 && cursor < url.size()) {
            const auto slash = url.find('/', cursor);
            if (slash == std::string_view::npos) {
                break;
            }
            cursor = slash + 1;
            ++segments;
        }

        if (segments == 2) {
            std::string out(url.substr(0, creds_start));
            out += "***/***/";
            out += url.substr(cursor);
            return out;
        }
    }
    return std::string(url);
}

}  // namespace coax::log
