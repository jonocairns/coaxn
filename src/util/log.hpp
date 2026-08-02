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

// Most recent messages, oldest first.
const std::vector<std::string>& recent();

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
