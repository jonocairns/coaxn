#pragma once

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coax::log {

enum class Level { Debug, Info, Warn, Error };

// Appends to the session log and to an in-memory ring the diagnostics overlay
// reads. Messages are expected to be already free of credentials: callers
// redact before logging rather than relying on a filter here.
void write(Level level, std::string_view message);

struct SavedLog {
    std::wstring path;
    // UTF-8, for showing to the viewer.
    std::string display_path;
};

// Copies the session log so far into a new timestamped file in a "logs" folder
// beside it. The live log is truncated when the next session starts, so this
// is how the evidence from a run outlives it. Empty with `error` set when there
// is no session log or the copy fails. It does file I/O: call it off the UI
// thread. Logging continues while it runs.
std::optional<SavedLog> save_copy(std::string& error);

// Most recent messages, oldest first, as a copy taken under the ring's lock.
// It has to be a copy: any thread may be logging while the UI thread reads
// these. See util/log_ring.hpp, where the ring and its tests live.
std::vector<std::string> recent();

// The same, into a buffer the caller keeps across calls -- for the diagnostics
// panel, which asks once per frame for as long as it is open.
void recent_into(std::vector<std::string>& out);

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace coax::log
