#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace replay {
using Json = nlohmann::json;
namespace fs = std::filesystem;
struct Paths { fs::path exe, data, obs_config; };
Paths paths();
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
bool obs_running();
void launch_obs(const fs::path& exe);
Json monitors();
Json audio_devices(bool input);
std::string run_capture(const fs::path& exe, const std::wstring& arguments, unsigned timeout_ms = 10000);
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
