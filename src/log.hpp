#pragma once
#include "platform.hpp"
#include <format>
#include <string>
#include <string_view>

// Session log. One text file per run in <data>\logs, written line by line so it survives a crash.
// Every line: local time, level, message. Values follow the message as key=value; text values are quoted.
namespace replay {
enum class LogLevel { debug, info, warn, error };

// "0.1.0+abc1234": version, then the Git commit the build came from (".dirty" when built from changed files).
std::string app_version();
// {version, commit, commit_short, dirty, built}
Json build_info();

// Opens this run's log file and writes the header (build, Windows, hardware). Lines logged earlier are kept and written first.
void log_start(const Paths& paths);
void log_write(LogLevel level, std::string_view message);
// The last `lines` lines of this session, oldest first.
std::string log_tail(std::size_t lines);
// The whole session so far, as written to the file.
std::string log_text();
fs::path log_folder();
// A text value for a log line: quoted, with quotes and line breaks escaped. Backslashes stay, so paths read as they are.
std::string quoted_value(std::string_view text);

template <class... Args> void log_debug(std::format_string<Args...> text, Args&&... args) { log_write(LogLevel::debug, std::format(text, std::forward<Args>(args)...)); }
template <class... Args> void log_info(std::format_string<Args...> text, Args&&... args) { log_write(LogLevel::info, std::format(text, std::forward<Args>(args)...)); }
template <class... Args> void log_warn(std::format_string<Args...> text, Args&&... args) { log_write(LogLevel::warn, std::format(text, std::forward<Args>(args)...)); }
template <class... Args> void log_error(std::format_string<Args...> text, Args&&... args) { log_write(LogLevel::error, std::format(text, std::forward<Args>(args)...)); }
}
