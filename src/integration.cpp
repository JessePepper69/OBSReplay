#include "integration.hpp"
#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace replay {
namespace {
constexpr auto product = "OBSReplay";
constexpr auto owner_type = "OBSReplay.integration.v1";
const fs::path profile_relative = fs::path("basic") / "profiles" / product;
const fs::path scene_relative = fs::path("basic") / "scenes" / "OBSReplay.json";
const fs::path scene_marker_relative = fs::path("basic") / "scenes" / "OBSReplay.owner";
const fs::path socket_relative = fs::path("plugin_config") / "obs-websocket" / "config.json";

Json json_file(const fs::path& path) {
    return Json::parse(read_text(path));
}

std::string uuid() {
    GUID value{};
    if (FAILED(CoCreateGuid(&value))) throw std::runtime_error("Could not create an integration identifier.");
    wchar_t text[40]{};
    StringFromGUID2(value, text, 40);
    std::wstring result(text);
    return utf8(result.substr(1, result.size() - 2));
}

void verify_version(const fs::path& exe) {
    if (!fs::is_regular_file(exe) || _wcsicmp(exe.filename().c_str(), L"obs64.exe") != 0)
        throw std::runtime_error("Choose OBS Studio's bin/64bit/obs64.exe.");
    DWORD unused = 0;
    const DWORD length = GetFileVersionInfoSizeW(exe.c_str(), &unused);
    std::vector<unsigned char> buffer(length);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT size = 0;
    if (!length || !GetFileVersionInfoW(exe.c_str(), 0, length, buffer.data()) ||
        !VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &size) ||
        size < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xfeef04bd || HIWORD(info->dwFileVersionMS) < 31)
        throw std::runtime_error("OBS Studio 31 or newer is required. Update OBS and choose its executable.");
}

using Sections = std::map<std::string, std::map<std::string, std::string>>;
Sections parse_probe(const std::string& output) {
    Sections sections;
    std::istringstream lines(output);
    std::string line, section;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') section = line.substr(1, line.size() - 2);
        else if (const auto equals = line.find('='); equals != std::string::npos)
            sections[section][line.substr(0, equals)] = line.substr(equals + 1);
    }
    return sections;
}

bool yes(const Sections& sections, const std::string& section, const std::string& key) {
    auto group = sections.find(section);
    if (group == sections.end()) return false;
    auto item = group->second.find(key);
    return item != group->second.end() && (item->second == "true" || item->second == "1");
}

struct Adapter { unsigned vendor = 0; std::wstring luid; std::string name; unsigned same_vendor_count = 0; };
Adapter default_adapter() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapters1(0, &adapter))) return {};
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return {};
    const auto value = (static_cast<unsigned long long>(static_cast<unsigned>(desc.AdapterLuid.HighPart)) << 32) |
                       desc.AdapterLuid.LowPart;
    std::wostringstream text;
    text << std::hex << value;
    unsigned count = 0;
    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 candidate_desc{};
        if (candidate && SUCCEEDED(candidate->GetDesc1(&candidate_desc)) && candidate_desc.VendorId == desc.VendorId &&
            !(candidate_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) ++count;
    }
    return {desc.VendorId, text.str(), utf8(desc.Description), count};
}

Json encoder_options(const fs::path& obs) {
    Json hardware;
    Json codecs = Json::array({"h264"});
    const auto adapter = default_adapter();
    const auto bin = obs.parent_path();
    try {
        if (adapter.vendor == 0x10de) {
            const auto caps = parse_probe(run_capture(bin / "obs-nvenc-test.exe", L"", 10000));
            std::vector<std::string> matching;
            for (const auto& [section, fields] : caps)
                if (section.starts_with("device.") && fields.contains("name") && fields.at("name") == adapter.name) matching.push_back(section);
            auto supported = [&](const std::string& codec) {
                if (!matching.empty()) return std::all_of(matching.begin(), matching.end(), [&](const auto& section) { return yes(caps, section + "." + codec, "codec_supported"); });
                return adapter.same_vendor_count == 1 && yes(caps, codec, "codec_supported");
            };
            if (yes(caps, "general", "nvenc_supported") && supported("h264")) {
                if (supported("hevc")) codecs.push_back("hevc");
                if (supported("av1")) codecs.push_back("av1");
                hardware = {{"id", "nvidia"}, {"label", "NVIDIA NVENC"}, {"codecs", codecs}};
            }
        } else if (adapter.vendor == 0x1002) {
            const auto caps = parse_probe(run_capture(bin / "obs-amf-test.exe", adapter.luid, 10000));
            if (!caps.contains("error") && yes(caps, "0", "is_amd") && yes(caps, "0", "supports_avc")) {
                if (yes(caps, "0", "supports_hevc")) codecs.push_back("hevc");
                if (yes(caps, "0", "supports_av1")) codecs.push_back("av1");
                hardware = {{"id", "amd"}, {"label", "AMD AMF"}, {"codecs", codecs}};
            }
        } else if (adapter.vendor == 0x8086) {
            const auto caps = parse_probe(run_capture(bin / "obs-qsv-test.exe", adapter.luid, 10000));
            if (!caps.contains("error") && yes(caps, "0", "is_intel")) {
                if (yes(caps, "0", "supports_hevc")) codecs.push_back("hevc");
                if (yes(caps, "0", "supports_av1")) codecs.push_back("av1");
                hardware = {{"id", "intel"}, {"label", "Intel Quick Sync"}, {"codecs", codecs}};
            }
        }
    } catch (const std::exception&) {
        hardware = nullptr;
        codecs = Json::array({"h264"});
    }
    Json result = Json::array();
    result.push_back({{"id", "auto"}, {"label", hardware.is_null() ? "Automatic · CPU fallback" : "Automatic · " + hardware["label"].get<std::string>()}, {"codecs", codecs}});
    if (!hardware.is_null()) result.push_back(hardware);
    result.push_back({{"id", "cpu"}, {"label", "CPU · x264"}, {"codecs", Json::array({"h264"})}});
    return result;
}

std::string encoder_id(const std::string& brand, const std::string& codec) {
    if (brand == "nvidia") return "obs_nvenc_" + codec + "_tex";
    if (brand == "amd") return (codec == "hevc" ? "h265" : codec) + std::string("_texture_amf");
    if (brand == "intel") return codec == "h264" ? "obs_qsv11_v2" : "obs_qsv11_" + codec;
    return "obs_x264";
}

Json encoder_settings(const Json& settings) {
    const std::string brand = settings["encoder"], codec = settings["codec"];
    const int quality = settings["quality"];
    if (brand == "cpu") return {{"rate_control", "CRF"}, {"crf", quality}, {"preset", "veryfast"}, {"profile", "high"}, {"keyint_sec", 2}};
    Json value = {{"rate_control", "CQP"}, {"cqp", quality}, {"keyint_sec", 2}};
    if (brand == "nvidia") {
        value.update({{"preset", "p4"}, {"tune", "hq"}, {"multipass", "disabled"}, {"lookahead", false}, {"device", -1}, {"bf", 2}});
    } else if (brand == "amd") value["preset"] = "balanced";
    else value.update({{"target_usage", "TU4"}, {"bframes", 2}});
    if (codec != "av1") value["profile"] = codec == "h264" ? "high" : "main";
    return value;
}

int memory_limit(int seconds) {
    MEMORYSTATUSEX memory{sizeof(memory)};
    const int available = GlobalMemoryStatusEx(&memory) ? static_cast<int>(memory.ullTotalPhys / (1024 * 1024 * 10)) : 1024;
    return std::clamp(std::min(seconds * 8, available), 256, 2048);
}

std::string profile_ini(const Json& s, int width, int height) {
    std::ostringstream ini;
    const std::string id = encoder_id(s["encoder"], s["codec"]);
    std::string folder = path_string(from_string(s["clips_dir"]));
    if (folder.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("The clip folder contains invalid characters.");
    std::replace(folder.begin(), folder.end(), '\\', '/');
    int out_height = s["output_height"];
    if (out_height == 0 || out_height > height) out_height = height;
    out_height -= out_height % 2;
    const int out_width = std::max(2, static_cast<int>(std::round(width * static_cast<double>(out_height) / height)) & ~1);
    ini << "[General]\nName=OBSReplay\n\n[Output]\nMode=Advanced\nFilenameFormatting=%CCYY-%MM-%DD %hh-%mm-%ss\nOverwriteIfExists=false\n"
        << "\n[SimpleOutput]\nFilePath=" << folder << "\nRecRBPrefix=Replay\nRecRBSuffix=\n"
        << "\n[AdvOut]\nEncoder=" << id << "\nAudioEncoder=ffmpeg_aac\nRecType=Standard\nRecFilePath=" << folder
        << "\nRecFormat2=mkv\nRecUseRescale=false\nRecRescaleFilter=0\nRecEncoder=" << id
        << "\nRecAudioEncoder=ffmpeg_aac\nRecTracks=" << (s["separate_tracks"].get<bool>() ? 7 : 1)
        << "\nRecRB=true\nRecRBTime=" << s["clip_seconds"].get<int>() << "\nRecRBSize=" << memory_limit(s["clip_seconds"])
        << "\nTrack1Bitrate=160\nTrack2Bitrate=160\nTrack3Bitrate=160\nTrack1Name=Mix\nTrack2Name=Desktop\nTrack3Name=Microphone\n"
        << "\n[Video]\nBaseCX=" << width << "\nBaseCY=" << height << "\nOutputCX=" << out_width << "\nOutputCY=" << out_height
        << "\nFPSType=1\nFPSInt=" << s["fps"].get<int>() << "\nScaleType=bicubic\nColorFormat=NV12\nColorSpace=709\nColorRange=Partial\n"
        << "\n[Audio]\nSampleRate=48000\nChannelSetup=Stereo\n\n[Hotkeys]\nReplayBuffer={}\n";
    return ini.str();
}

Json source(const std::string& kind, const std::string& name, const std::string& id, Json settings, int mixers = 0) {
    return {{"id", kind}, {"versioned_id", kind}, {"name", name}, {"uuid", id}, {"settings", std::move(settings)},
            {"enabled", true}, {"volume", 1.0}, {"muted", false}, {"mixers", mixers}, {"sync", 0},
            {"flags", 0}, {"monitoring_type", 0}, {"hotkeys", Json::object()}, {"private_settings", Json::object()}};
}

Json scene_collection(const Json& s, const Json& display) {
    const bool desktop = s["capture"] == "desktop";
    const std::string input_uuid = uuid(), scene_uuid = uuid();
    const std::string input_name = desktop ? "OBSReplay Display" : "OBSReplay Game";
    Json capture = desktop
        ? Json{{"monitor_id", display["id"]}, {"capture_cursor", true}, {"method", 0}}
        : Json{{"capture_mode", "any_fullscreen"}, {"capture_cursor", true}, {"capture_overlays", false},
               {"anti_cheat_hook", true}, {"allow_transparency", false}, {"limit_framerate", false}, {"capture_audio", false}};
    Json item = {{"name", input_name}, {"source_uuid", input_uuid}, {"id", 1}, {"visible", true}, {"locked", true},
                 {"pos", {{"x", 0.0}, {"y", 0.0}}}, {"scale", {{"x", 1.0}, {"y", 1.0}}}, {"rot", 0.0},
                 {"align", 5}, {"bounds_type", 2}, {"bounds_align", 0}, {"bounds_crop", false},
                 {"bounds", {{"x", display["width"]}, {"y", display["height"]}}},
                 {"crop_left", 0}, {"crop_right", 0}, {"crop_top", 0}, {"crop_bottom", 0}, {"scale_filter", "disable"},
                 {"blend_method", "default"}, {"blend_type", "normal"}, {"private_settings", Json::object()}};
    Json scene = source("scene", product, scene_uuid, {{"id_counter", 1}, {"custom_size", false}, {"items", Json::array({item})}});
    Json value = {{"name", product}, {"current_scene", product}, {"current_program_scene", product},
                  {"scene_order", Json::array({{{"name", product}}})}, {"sources", Json::array({scene, source(desktop ? "monitor_capture" : "game_capture", input_name, input_uuid, capture)})},
                  {"current_transition", "Fade"}, {"transition_duration", 200}, {"transitions", Json::array()},
                  {"groups", Json::array()}, {"quick_transitions", Json::array()}, {"saved_projectors", Json::array()},
                  {"preview_locked", true}, {"scaling_enabled", false}, {"scaling_level", 0}, {"scaling_off_x", 0.0}, {"scaling_off_y", 0.0},
                  {"modules", Json::object()}, {"resolution", {{"x", display["width"]}, {"y", display["height"]}}}, {"version", 2}};
    const bool split = s["separate_tracks"];
    if (s["desktop_device"] != "off")
        value["DesktopAudioDevice1"] = source("wasapi_output_capture", "OBSReplay Desktop Audio", uuid(), {{"device_id", s["desktop_device"]}}, split ? 3 : 1);
    if (s["mic_enabled"].get<bool>()) {
        auto microphone = source("wasapi_input_capture", "OBSReplay Microphone", uuid(), {{"device_id", s["mic_device"]}}, split ? 5 : 1);
        if (s["noise"] != "off") {
            auto filter = source("noise_suppress_filter", "Noise Suppression", uuid(), {{"method", s["noise"]}, {"suppress_level", -30}});
            filter["versioned_id"] = "noise_suppress_filter_v2";
            microphone["filters"] = Json::array({filter});
        }
        value["AuxAudioDevice1"] = microphone;
    }
    return value;
}

void require_closed() {
    if (obs_running()) throw std::runtime_error("Close OBS before applying capture settings. Your active OBS session was left untouched.");
}

void verify_destination(const fs::path& directory) {
    fs::create_directories(directory);
    const auto probe = directory / from_string(".obsreplay-writecheck-" + uuid() + ".tmp");
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("The clip folder is not writable. Choose a folder where you can save files.");
    const char value[] = "OBSReplay";
    DWORD written = 0;
    const bool valid = WriteFile(file, value, sizeof(value) - 1, &written, nullptr) &&
                       written == sizeof(value) - 1 && FlushFileBuffers(file);
    CloseHandle(file);
    if (!valid) throw std::runtime_error("The clip folder could not save a file. Check disk space and folder permissions.");
}

void require_plain_path(const fs::path& root, const fs::path& relative) {
    auto current = root;
    if (fs::is_symlink(fs::symlink_status(root)) || (fs::exists(root) && (GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error("The OBS configuration directory cannot be a linked directory.");
    for (const auto& part : relative) {
        current /= part;
        const auto status = fs::symlink_status(current);
        if (fs::is_symlink(status) || (fs::exists(status) && (GetFileAttributesW(current.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)))
            throw std::runtime_error("The OBS integration path contains a linked directory or file.");
    }
}

bool marker_matches(const fs::path& path, const std::string& owner) {
    try { const auto marker = json_file(path); return marker.value("type", "") == owner_type && marker.value("owner", "") == owner; }
    catch (...) { return false; }
}

struct Change { fs::path path; bool existed; std::string before; std::string after; bool remove = false; };
void add_change(std::vector<Change>& changes, const fs::path& path, std::string after, bool remove = false) {
    const bool exists = fs::exists(path);
    if (exists && !fs::is_regular_file(path)) throw std::runtime_error("An integration file path is occupied by a directory.");
    changes.push_back({path, exists, exists ? read_text(path) : "", std::move(after), remove});
}

void commit_changes(const std::vector<Change>& changes) {
    std::size_t applied = 0;
    std::vector<fs::path> created_directories;
    for (const auto& change : changes) {
        for (auto parent = change.path.parent_path(); !parent.empty() && !fs::exists(parent); parent = parent.parent_path())
            if (std::find(created_directories.begin(), created_directories.end(), parent) == created_directories.end()) created_directories.push_back(parent);
    }
    try {
        for (const auto& change : changes) {
            require_closed();
            if (change.remove) fs::remove(change.path);
            else atomic_write(change.path, change.after);
            ++applied;
        }
    } catch (...) {
        const auto cause = std::current_exception();
        bool restored = true;
        while (applied > 0) {
            const auto& change = changes[--applied];
            try {
                if (change.existed) atomic_write(change.path, change.before);
                else fs::remove(change.path);
            } catch (...) { restored = false; }
        }
        for (const auto& directory : created_directories) {
            std::error_code error;
            if (fs::is_directory(directory, error) && fs::is_empty(directory, error)) fs::remove(directory, error);
        }
        if (!restored) throw std::runtime_error("Setup failed and could not restore every file. Keep OBS closed and retry setup; the original WebSocket backup is in the app data folder.");
        std::rethrow_exception(cause);
    }
}
}

Integration::Integration(Paths paths) : paths_(std::move(paths)) {}

bool Integration::configured() const {
    try {
        const auto manifest = json_file(paths_.data / "integration.json");
        const std::string owner = manifest.at("owner");
        return manifest.value("type", "") == owner_type &&
               marker_matches(paths_.obs_config / profile_relative / ".obsreplay-owner.json", owner) &&
               marker_matches(paths_.obs_config / scene_marker_relative, owner) &&
               fs::is_regular_file(paths_.obs_config / profile_relative / "basic.ini") &&
               fs::is_regular_file(paths_.obs_config / scene_relative);
    } catch (...) { return false; }
}

Json Integration::options(const Json& settings) const {
    fs::path obs = from_string(settings.value("obs_path", ""));
    if (obs.empty()) obs = find_obs();
    return {{"monitors", monitors()}, {"audio_inputs", audio_devices(true)}, {"audio_outputs", audio_devices(false)},
            {"encoders", encoder_options(obs)}};
}

Json Integration::prepare(const Json& settings) {
    validate_settings(settings);
    require_closed();
    Json resolved = settings;
    fs::path exe = from_string(resolved["obs_path"]);
    if (exe.empty()) exe = find_obs();
    verify_version(exe);
    resolved["obs_path"] = path_string(exe);
    const auto choices = options(resolved);
    const auto& displays = choices["monitors"];
    if (displays.empty()) throw std::runtime_error("No active monitor was found. Connect a display and try again.");
    auto selected = displays.begin();
    const std::string monitor = resolved["monitor_id"];
    if (!monitor.empty()) {
        selected = std::find_if(displays.begin(), displays.end(), [&](const auto& item) { return item["id"] == monitor; });
        if (selected == displays.end()) throw std::runtime_error("The selected monitor is disconnected. Choose an available monitor.");
    }
    resolved["monitor_id"] = (*selected)["id"];
    for (const auto& [key, list, enabled] : std::array<std::tuple<const char*, const char*, bool>, 2>{{
             {"desktop_device", "audio_outputs", resolved["desktop_device"] != "off"},
             {"mic_device", "audio_inputs", resolved["mic_enabled"].get<bool>()}}}) {
        const std::string device = resolved[key];
        if (enabled && device == "default" && std::none_of(choices[list].begin(), choices[list].end(), [](const auto& item) {
                const std::string id = item["id"];
                return id != "default" && id != "off";
            })) throw std::runtime_error("No active audio device is available. Connect a device or turn off this audio source.");
        if (enabled && device != "default" && std::none_of(choices[list].begin(), choices[list].end(), [&](const auto& item) { return item["id"] == device; }))
            throw std::runtime_error("The selected audio device is disconnected. Choose an available device.");
    }
    std::string encoder = resolved["encoder"];
    const auto& encoders = choices["encoders"];
    if (encoder == "auto") encoder = encoders.size() > 2 ? encoders[1]["id"].get<std::string>() : "cpu";
    auto hardware = std::find_if(encoders.begin(), encoders.end(), [&](const auto& item) { return item["id"] == encoder; });
    if (hardware == encoders.end()) throw std::runtime_error("The selected hardware encoder is unavailable on the default graphics adapter. Choose Automatic or CPU encoding.");
    const std::string codec = resolved["codec"];
    if (std::find((*hardware)["codecs"].begin(), (*hardware)["codecs"].end(), codec) == (*hardware)["codecs"].end())
        throw std::runtime_error("The selected codec is unavailable on this encoder. Choose H.264 or another supported codec.");
    resolved["encoder"] = encoder;

    const auto profile = paths_.obs_config / profile_relative;
    const auto scene = paths_.obs_config / scene_relative;
    const auto marker_path = paths_.obs_config / scene_marker_relative;
    const auto socket = paths_.obs_config / socket_relative;
    const auto manifest_path = paths_.data / "integration.json";
    for (const auto& relative : {profile_relative / ".obsreplay-owner.json", profile_relative / "basic.ini", profile_relative / "recordEncoder.json", profile_relative / "streamEncoder.json", scene_relative, scene_marker_relative, socket_relative})
        require_plain_path(paths_.obs_config, relative);

    Json manifest = fs::exists(manifest_path) ? json_file(manifest_path) : Json::object();
    if (!manifest.empty() && manifest.value("type", "") != owner_type) throw std::runtime_error("The integration state is unrecognized. No OBS files were changed.");
    const bool existing = manifest.contains("owner");
    const std::string owner = existing ? manifest.at("owner").get<std::string>() : uuid();
    if ((fs::exists(profile) && (!existing || !marker_matches(profile / ".obsreplay-owner.json", owner))) ||
        ((fs::exists(scene) || fs::exists(fs::path(scene.wstring() + L".bak")) || fs::exists(marker_path)) && (!existing || !marker_matches(marker_path, owner))))
        throw std::runtime_error("An unrelated OBSReplay profile or scene collection already exists. Rename it in OBS before setup.");

    Json shared = fs::exists(socket) ? json_file(socket) : Json::object();
    if (!shared.is_object()) throw std::runtime_error("OBS's WebSocket settings are invalid. Open OBS and repair its WebSocket settings first.");
    if (!existing) {
        manifest = {{"type", owner_type}, {"owner", owner}, {"created_at", now_iso()}, {"socket_existed", fs::exists(socket)},
                    {"socket_before", shared}, {"socket_applied", Json::object()}};
    }
    Json next_socket = shared;
    next_socket["server_enabled"] = true;
    next_socket["auth_required"] = true;
    next_socket["first_load"] = false;
    if (!next_socket.contains("server_password") || !next_socket["server_password"].is_string() || next_socket["server_password"].get<std::string>().empty())
        next_socket["server_password"] = random_secret();
    if (!next_socket.contains("server_port") || !next_socket["server_port"].is_number_integer() ||
        next_socket["server_port"].get<int>() < 1 || next_socket["server_port"].get<int>() > 65535) next_socket["server_port"] = 4455;
    for (const auto& key : {"server_enabled", "auth_required", "first_load", "server_password", "server_port"}) {
        if (!shared.contains(key) || shared[key] != next_socket[key]) {
            if (!manifest["socket_applied"].contains(key)) {
                if (shared.contains(key)) manifest["socket_before"][key] = shared[key];
                else manifest["socket_before"].erase(key);
            }
            manifest["socket_applied"][key] = next_socket[key];
        }
    }
    verify_destination(from_string(resolved["clips_dir"]));
    std::vector<Change> changes;
    if (!existing && fs::exists(socket)) add_change(changes, paths_.data / "websocket.original.json", read_text(socket));
    add_change(changes, profile / "basic.ini", profile_ini(resolved, (*selected)["width"], (*selected)["height"]));
    const std::string encoder_json = encoder_settings(resolved).dump(2);
    add_change(changes, profile / "recordEncoder.json", encoder_json);
    add_change(changes, profile / "streamEncoder.json", encoder_json);
    add_change(changes, scene, scene_collection(resolved, *selected).dump(2));
    const std::string marker = Json{{"type", owner_type}, {"owner", owner}}.dump(2);
    add_change(changes, profile / ".obsreplay-owner.json", marker);
    add_change(changes, marker_path, marker);
    if (next_socket != shared) add_change(changes, socket, next_socket.dump(2));
    const auto settings_path = paths_.data / "settings.json";
    if (fs::is_regular_file(settings_path)) add_change(changes, paths_.data / "settings.backup.json", read_text(settings_path));
    add_change(changes, settings_path, resolved.dump(2));
    manifest["updated_at"] = now_iso();
    add_change(changes, manifest_path, manifest.dump(2));
    commit_changes(changes);
    return resolved;
}

Credentials Integration::credentials() const {
    const auto config = json_file(paths_.obs_config / socket_relative);
    if (!config.value("server_enabled", false) || !config.value("auth_required", false)) throw std::runtime_error("Enable authenticated WebSocket control through setup.");
    const int port = config.value("server_port", 4455);
    const auto password = config.value("server_password", "");
    if (port < 1 || port > 65535 || password.empty()) throw std::runtime_error("OBS WebSocket settings are incomplete. Run setup again with OBS closed.");
    return {static_cast<unsigned short>(port), password};
}

void Integration::reset() {
    require_closed();
    const auto manifest_path = paths_.data / "integration.json";
    if (!fs::exists(manifest_path)) return;
    const auto manifest = json_file(manifest_path);
    if (manifest.value("type", "") != owner_type) throw std::runtime_error("The integration state is unrecognized. No files were removed.");
    const std::string owner = manifest.at("owner");
    const auto profile = paths_.obs_config / profile_relative;
    const auto marker_path = paths_.obs_config / scene_marker_relative;
    const auto socket = paths_.obs_config / socket_relative;
    for (const auto& relative : {profile_relative / ".obsreplay-owner.json", scene_marker_relative, socket_relative}) require_plain_path(paths_.obs_config, relative);
    if ((fs::exists(profile) && !marker_matches(profile / ".obsreplay-owner.json", owner)) ||
        ((fs::exists(paths_.obs_config / scene_relative) || fs::exists(marker_path)) && !marker_matches(marker_path, owner)))
        throw std::runtime_error("The OBSReplay ownership markers changed. No OBS files were removed.");
    std::vector<Change> changes;
    if (fs::is_regular_file(socket)) {
        const auto current = json_file(socket);
        auto restored = current;
        const auto& original = manifest.at("socket_before");
        for (const auto& [key, applied] : manifest.at("socket_applied").items()) {
            if (current.contains(key) && current[key] == applied) {
                if (original.contains(key)) restored[key] = original[key];
                else restored.erase(key);
            }
        }
        if (restored != current) add_change(changes, socket, restored.dump(2), !manifest.value("socket_existed", true) && restored.empty());
    }
    if (marker_matches(profile / ".obsreplay-owner.json", owner)) {
        for (const auto& name : {"basic.ini", "basic.ini.bak", "recordEncoder.json", "recordEncoder.json.bak", "streamEncoder.json", "streamEncoder.json.bak", ".obsreplay-owner.json"}) {
            const auto relative = profile_relative / name;
            require_plain_path(paths_.obs_config, relative);
            if (fs::is_regular_file(paths_.obs_config / relative)) add_change(changes, paths_.obs_config / relative, "", true);
        }
    }
    if (marker_matches(marker_path, owner)) {
        for (const auto& relative : {scene_relative, fs::path(scene_relative.wstring() + L".bak"), scene_marker_relative}) {
            require_plain_path(paths_.obs_config, relative);
            if (fs::is_regular_file(paths_.obs_config / relative)) add_change(changes, paths_.obs_config / relative, "", true);
        }
    }
    add_change(changes, manifest_path, "", true);
    commit_changes(changes);
    std::error_code ignored;
    if (fs::exists(profile) && fs::is_empty(profile)) fs::remove(profile, ignored);
}
}
