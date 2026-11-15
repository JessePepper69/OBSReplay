#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace replay {
using Json = nlohmann::json;
namespace fs = std::filesystem;
struct Paths { fs::path exe, data, obs_config; };
Paths paths();
// WebView2 profile for an isolated OBSREPLAY_DATA_DIR run; empty means the default per-user profile.
std::optional<fs::path> isolated_webview_folder();
// Test-only (OBSREPLAY_HEADLESS set): windows open off-screen without focus and Explorer is never opened.
bool headless_mode();
void show_headless(void* hwnd);
std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
std::string path_string(const fs::path& path);
fs::path from_string(const std::string& value);
std::string read_text(const fs::path& path);
void atomic_write(const fs::path& path, const std::string& text);
Json default_settings(const Paths& p);
Json load_settings(const Paths& p);
void validate_settings(const Json& settings);
void save_settings(const Paths& p, const Json& settings);
fs::path find_obs();
// File version (major, minor, patch) from the executable's version resource.
std::optional<std::array<int, 3>> file_version(const fs::path& file);
// Windows Package Manager's app execution alias; empty when winget is unavailable.
fs::path winget_path();
bool obs_running();
void launch_obs(const fs::path& exe);
void close_obs(const fs::path& exe);
// Asks every running OBS to close through its main window and waits; never forces a process to stop.
unsigned close_all_obs(unsigned timeout_ms = 20000);
Json monitors();
Json audio_devices(bool input);
std::string run_capture(const fs::path& exe, const std::wstring& arguments, unsigned timeout_ms = 10000);
// Runs a hidden process, streaming its output. Returns the exit code, -1 when it could not start or timed out
// (then it is terminated), or -2 when `cancelled` returned true (the process is left running).
int run_process(const fs::path& exe, const std::wstring& arguments, unsigned timeout_ms,
                const std::function<void(const std::string&)>& output, const std::function<bool()>& cancelled = {},
                const fs::path& directory = {});
std::string foreground_app();
std::string safe_name(const std::string& name);
void open_path(const fs::path& path, bool reveal = false);
void open_help(const std::string& topic);
void set_login_startup(const Paths& p, bool enabled);
std::string random_secret();
std::string sha256_base64(const std::string& input);
std::string now_iso();
void save_feedback();
}
