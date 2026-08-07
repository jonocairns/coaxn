#pragma once

#include <format>
#include <string>
#include <string_view>
#include <vector>

// Callers redact through these before logging; see the note on write().
#include "util/redact.hpp"

namespace coax::log {

enum class Level { Debug, Info, Warn, Error };

// Appends to the session log and to an in-memory ring the diagnostics overlay
// reads. Messages are expected to be already free of credentials: callers
// redact before logging rather than relying on a filter here.
void write(Level level, std::string_view message);

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
