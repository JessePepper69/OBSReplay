#include "editor.hpp"
#include "media.hpp"

#include <saucer/smartview.hpp>
#include <saucer/embedded/all.hpp>
#include <windows.h>
#include <objidl.h>
#include <saucer/modules/stable/webview2.hpp>
#include <shobjidl.h>
#include <wincrypt.h>
#include <wrl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <format>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <thread>

namespace replay {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

struct Owned {
    HANDLE value = nullptr;
    explicit Owned(HANDLE h = nullptr) : value(h) {}
    ~Owned() { reset(); }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    void reset(HANDLE next = nullptr) { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = next; }
};

std::wstring quoted(const fs::path& path) {
    auto text = path.wstring();
    if (text.find(L'"') != std::wstring::npos) throw std::runtime_error("The file path contains an unsupported quote character.");
    return L"\"" + text + L"\"";
}

std::wstring number(double value) { return wide(std::format("{:.6f}", value)); }
std::wstring short_number(double value) { return wide(std::format("{:.3f}", value)); }

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string last_line(const std::string& text) {
    std::string result;
    size_t end = text.size();
    while (end) {
        const auto start = text.rfind('\n', end - 1);
        auto line = text.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) { result = line; break; }
        if (start == std::string::npos) break;
        end = start;
    }
    if (result.size() > 300) result = result.substr(0, 300) + "…";
    return result;
}

std::string base64(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return {};
    DWORD length = 0;
    if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length)) return {};
    std::string result(length, '\0');
    if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &length)) return {};
    while (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

// Runs a console tool hidden, streaming its stdout to a callback. The child is
// placed in a kill-on-close job so FFmpeg never outlives OBSReplay.
struct ProcessResult { DWORD code = 1; std::string errors; bool canceled = false; bool started = false; };
ProcessResult run_tool(const fs::path& exe, const std::wstring& arguments, const std::function<void(std::string_view)>& output,
                       const std::function<bool()>& canceled, unsigned timeout_ms = 0) {
    ProcessResult result;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE out_read = nullptr, out_write = nullptr, err_read = nullptr, err_write = nullptr;
    if (!CreatePipe(&out_read, &out_write, &security, 1 << 16)) return result;
    Owned out_reader(out_read), out_writer(out_write);
    if (!CreatePipe(&err_read, &err_write, &security, 1 << 16)) return result;
    Owned err_reader(err_read), err_writer(err_write);
    SetHandleInformation(out_reader.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_reader.value, HANDLE_FLAG_INHERIT, 0);
    Owned null_input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = null_input.value; startup.StartupInfo.hStdOutput = out_writer.value; startup.StartupInfo.hStdError = err_writer.value;
    // Only this child's three standard handles are inherited, never another tool's pipes.
    HANDLE inherited[] = {null_input.value, out_writer.value, err_writer.value};
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<unsigned char> attribute_storage(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) return result;
    struct AttributeGuard { LPPROC_THREAD_ATTRIBUTE_LIST list; ~AttributeGuard() { DeleteProcThreadAttributeList(list); } } attribute_guard{attributes};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) return result;
    startup.lpAttributeList = attributes;
    auto command = quoted(exe) + L" " + arguments;
    if (command.size() >= 32000) { result.errors = "The export command is too long. Join some sections and try again."; return result; }
    const auto directory = exe.parent_path().wstring();
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(), &startup.StartupInfo, &process)) return result;
    Owned child(process.hProcess), thread(process.hThread);
    Owned job(CreateJobObjectW(nullptr, nullptr));
    if (job.value) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job.value, child.value);
    }
    ResumeThread(thread.value);
    result.started = true;
    out_writer.reset(); err_writer.reset();
    const auto start = Clock::now();
    std::array<char, 65536> buffer{};
    const auto drain = [&](HANDLE pipe, bool is_output) {
        bool any = false;
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || !available) return any;
            DWORD read = 0;
            if (!ReadFile(pipe, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr) || !read) return any;
            any = true;
            if (is_output) { if (output) output(std::string_view(buffer.data(), read)); }
            else {
                result.errors.append(buffer.data(), read);
                if (result.errors.size() > 256 * 1024) result.errors.erase(0, result.errors.size() - 128 * 1024);
            }
        }
    };
    bool ended = false;
    for (;;) {
        const bool got = drain(out_reader.value, true) | drain(err_reader.value, false);
        if (ended && !got) break;
        if (!ended && WaitForSingleObject(child.value, got ? 0 : 12) == WAIT_OBJECT_0) ended = true;
        if (!ended && ((canceled && canceled()) || (timeout_ms && Clock::now() - start > std::chrono::milliseconds(timeout_ms)))) {
            TerminateProcess(child.value, 1);
            WaitForSingleObject(child.value, 3000);
            result.canceled = true;
            return result;
        }
    }
    GetExitCodeProcess(child.value, &result.code);
    return result;
}

struct FFmpeg { std::string status = "checking"; fs::path exe; std::string version, message; bool nvenc = false, hevc_nvenc = false, x264 = false, x265 = false; };

FFmpeg inspect_ffmpeg(const fs::path& exe) {
    FFmpeg info; info.exe = exe;
    std::error_code error;
    if (exe.empty() || !fs::is_regular_file(exe, error)) { info.status = "missing"; return info; }
    std::string text;
    const auto version = run_tool(exe, L"-hide_banner -version", [&](std::string_view part) { text.append(part); }, {}, 10000);
    if (version.code != 0 || !text.starts_with("ffmpeg version")) {
        info.status = "invalid"; info.message = "This file did not respond like FFmpeg. Choose ffmpeg.exe from an FFmpeg build."; return info;
    }
    const auto end = text.find_first_of(" \r\n", 15);
    info.version = text.substr(15, end == std::string::npos ? std::string::npos : end - 15);
    std::string encoders;
    run_tool(exe, L"-hide_banner -encoders", [&](std::string_view part) { encoders.append(part); }, {}, 10000);
    info.x264 = encoders.find(" libx264 ") != std::string::npos;
    if (encoders.find(" h264_nvenc ") != std::string::npos) {
        const auto test = run_tool(exe, L"-hide_banner -nostdin -loglevel error -f lavfi -i color=c=black:s=256x144:r=30:d=0.2 -c:v h264_nvenc -preset p4 -rc vbr -cq 23 -b:v 0 -spatial-aq 1 -f null -", {}, {}, 15000);
        info.nvenc = test.code == 0 && !test.canceled;
    }
    info.x265 = encoders.find(" libx265 ") != std::string::npos;
    if (info.nvenc && encoders.find(" hevc_nvenc ") != std::string::npos) {
        const auto test = run_tool(exe, L"-hide_banner -nostdin -loglevel error -f lavfi -i color=c=black:s=256x144:r=30:d=0.2 -c:v hevc_nvenc -preset p4 -rc vbr -cq 23 -b:v 0 -f null -", {}, {}, 15000);
        info.hevc_nvenc = test.code == 0 && !test.canceled;
    }
    if (!info.x264 && !info.nvenc) {
        info.status = "invalid"; info.message = "This FFmpeg build has no H.264 encoder. Install a full FFmpeg build."; return info;
    }
    info.status = "ready";
    return info;
}

std::vector<fs::path> ffmpeg_candidates(const fs::path& saved) {
    std::vector<fs::path> candidates;
    if (!saved.empty()) candidates.push_back(saved);
    const auto environment = [](const wchar_t* name) {
        std::wstring value(32768, L'\0');
        const DWORD size = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        value.resize(size < value.size() ? size : 0);
        return fs::path(value);
    };
    // Absolute PATH entries only; never the current or application directory.
    const auto search = environment(L"PATH").wstring();
    for (size_t start = 0; start <= search.size();) {
        auto end = search.find(L';', start);
        if (end == std::wstring::npos) end = search.size();
        auto entry = search.substr(start, end - start);
        std::erase(entry, L'"');
        if (!entry.empty() && fs::path(entry).is_absolute()) candidates.push_back(fs::path(entry) / L"ffmpeg.exe");
        start = end + 1;
    }
    const auto local = environment(L"LOCALAPPDATA");
    if (!local.empty()) {
        candidates.push_back(local / L"Microsoft/WinGet/Links/ffmpeg.exe");
        std::error_code error;
        for (const auto& package : fs::directory_iterator(local / L"Microsoft/WinGet/Packages", error)) {
            if (lower(path_string(package.path().filename())).find("ffmpeg") == std::string::npos) continue;
            std::error_code inner;
            for (const auto& build : fs::directory_iterator(package.path(), inner)) candidates.push_back(build.path() / L"bin/ffmpeg.exe");
        }
    }
    const auto profile = environment(L"USERPROFILE");
    if (!profile.empty()) candidates.push_back(profile / L"scoop/shims/ffmpeg.exe");
    const auto programs = environment(L"ProgramFiles");
    if (!programs.empty()) candidates.push_back(programs / L"ffmpeg/bin/ffmpeg.exe");
    const auto data = environment(L"ProgramData");
    if (!data.empty()) candidates.push_back(data / L"chocolatey/bin/ffmpeg.exe");
    candidates.emplace_back(L"C:/ffmpeg/bin/ffmpeg.exe");
    return candidates;
}

std::string pick_ffmpeg_file(HWND owner, const fs::path& current) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) throw std::runtime_error("Windows could not open the file picker.");
    dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR);
    dialog->SetTitle(L"Locate FFmpeg (ffmpeg.exe)");
    const COMDLG_FILTERSPEC filters[] = {{L"FFmpeg", L"ffmpeg.exe"}, {L"Applications", L"*.exe"}};
    dialog->SetFileTypes(2, filters);
    if (!current.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(current.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder)))) dialog->SetFolder(folder.Get());
    }
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    if (FAILED(shown)) throw std::runtime_error("Windows could not complete the file picker.");
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return {};
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected))) throw std::runtime_error("The selected item is not a local file.");
    const auto result = utf8(selected);
    CoTaskMemFree(selected);
    return result;
}

Json probe_from_output(const std::string& text) {
    Json probe = {{"status", "failed"}, {"duration", 0.0}, {"fps", 0.0}, {"width", 0}, {"height", 0}, {"audio_tracks", 0}, {"codec", ""}};
    std::smatch match;
    static const std::regex duration(R"(Duration:\s*(\d+):(\d{2}):(\d{2}(?:\.\d+)?))");
    if (std::regex_search(text, match, duration))
        probe["duration"] = std::stod(match[1]) * 3600 + std::stod(match[2]) * 60 + std::stod(match[3]);
    static const std::regex overall(R"(Duration:[^\n]*?bitrate:\s*(\d+)\s*kb/s)");
    if (std::regex_search(text, match, overall)) probe["bitrate"] = std::stoi(match[1]);
    int audio = 0; bool video = false;
    Json names = Json::array();
    int naming = -1;  // audio track whose metadata block is being read
    static const std::regex tag(R"(^\s+(title|name)\s*:\s*(.*?)\s*$)");
    size_t position = 0;
    while (position < text.size()) {
        auto end = text.find('\n', position);
        if (end == std::string::npos) end = text.size();
        auto line = text.substr(position, end - position);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        position = end + 1;
        if (line.find("Stream #0:") == std::string::npos) {
            if (naming >= 0 && std::regex_match(line, match, tag) && (match[1] == "title" || names[naming].get<std::string>().empty()))
                names[naming] = match[2].str().substr(0, 64);
            continue;
        }
        naming = -1;
        if (line.find(": Audio:") != std::string::npos) { naming = audio++; names.push_back(""); }
        else if (!video && line.find(": Video:") != std::string::npos && line.find("attached pic") == std::string::npos) {
            video = true;
            static const std::regex codec(R"(Video:\s*([A-Za-z0-9_]+))"), size(R"(,\s*(\d{2,5})x(\d{2,5}))"), fps(R"(([0-9.]+)\s*fps)"), rate(R"((\d+)\s*kb/s)");
            if (std::regex_search(line, match, rate)) probe["kbps"] = std::stoi(match[1]);
            if (std::regex_search(line, match, codec)) probe["codec"] = match[1].str();
            static const std::regex pixels(R"(,\s*((?:yuv|nv|p0|gbr|rgb|bgr)[a-z0-9]*)[(,\s])");
            if (std::regex_search(line, match, pixels)) probe["pix_fmt"] = match[1].str();
            if (std::regex_search(line, match, size)) { probe["width"] = std::stoi(match[1]); probe["height"] = std::stoi(match[2]); }
            if (std::regex_search(line, match, fps)) probe["fps"] = std::stod(match[1]);
        }
    }
    probe["audio_tracks"] = audio;
    probe["audio_names"] = names;
    if (video && probe["duration"].get<double>() > 0) probe["status"] = "ready";
    return probe;
}

// Preview proxies and audio stems live under %TEMP%\OBSReplay-preview\<pid>-<editor>; each editor
// deletes its folder on close, and folders left by a process that is no longer running are swept.
fs::path preview_root() {
    std::wstring temp(MAX_PATH + 1, L'\0');
    const DWORD length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    temp.resize(length && length < temp.size() ? length : 0);
    return fs::path(temp.empty() ? L"." : temp) / L"OBSReplay-preview";
}
void sweep_previews() {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(preview_root(), error)) {
        const auto name = entry.path().filename().wstring();
        const auto dash = name.find(L'-');
        DWORD pid = 0;
        try { pid = static_cast<DWORD>(std::stoul(name.substr(0, dash))); } catch (...) { continue; }
        if (pid == GetCurrentProcessId()) continue;
        bool running = false;
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            DWORD code = 0;
            running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
            CloseHandle(process);
        }
        if (!running) { std::error_code ignored; fs::remove_all(entry.path(), ignored); }
    }
}

// Quality 10 keeps the original video (frame-exact smart cut). 1-9 re-encode the whole video at a share
// of the original bitrate, so every step down gives a visibly smaller file.
struct QualityPlan { double share; int audio_kbps, max_height; };
QualityPlan plan_for(int quality) {
    static constexpr std::array<QualityPlan, 10> plans{{
        {0.10, 96, 720}, {0.14, 96, 720}, {0.19, 128, 0}, {0.25, 128, 0}, {0.32, 160, 0},
        {0.40, 160, 0}, {0.50, 192, 0}, {0.62, 192, 0}, {0.78, 192, 0}, {1.0, 192, 0}}};
    return plans[static_cast<size_t>(std::clamp(quality, 1, 10) - 1)];
}
int reference_kbps(const Json& info) {
    int kbps = info.value("kbps", 0);
    if (kbps <= 0) kbps = static_cast<int>(info.value("width", 0) * info.value("height", 0) * std::max(24.0, info.value("fps", 30.0)) * 0.1 / 1000);
    return std::max(kbps, 500);
}
struct EncodeChoice { bool hevc; int video_kbps; };
// Re-encodes keep HEVC when the original is HEVC and the GPU can encode it; H.264 needs about a third
// more bitrate than HEVC/AV1 for the same picture.
EncodeChoice choose_encode(const FFmpeg& tool, const Json& info, int quality, bool hardware) {
    const auto codec = lower(info.value("codec", ""));
    const bool hevc = hardware && tool.hevc_nvenc && codec == "hevc";
    const double factor = !hevc && codec != "h264" ? 1.35 : 1.0;
    return {hevc, std::max(300, static_cast<int>(std::lround(reference_kbps(info) * plan_for(quality).share * factor)))};
}
// FFmpeg 7+ reads an option's value from a file with "-/option"; older builds use -filter_complex_script.
std::wstring filter_file(const FFmpeg& tool, const fs::path& file) {
    static const std::regex numbered(R"(^(\d+)\.)");
    std::smatch match;
    const bool old = std::regex_search(tool.version, match, numbered) && std::stoi(match[1]) < 7;
    return (old ? L" -filter_complex_script " : L" -/filter_complex ") + quoted(file);
}
// MP4 stores a track's name from its "title" tag; the recorded names are re-applied to the new audio tracks.
std::wstring track_titles(const Json& info, int tracks) {
    std::wstring result;
    const auto names = info.value("audio_names", Json::array());
    for (int t = 0; t < tracks && t < static_cast<int>(names.size()); ++t) {
        auto name = names[t].is_string() ? names[t].get<std::string>() : std::string{};
        std::erase(name, '"'); std::erase(name, '\\');
        if (!name.empty()) result += L" -metadata:s:a:" + std::to_wstring(t) + L" title=\"" + wide(name) + L"\"";
    }
    return result;
}
std::string concat_entry(const fs::path& file) {
    auto text = path_string(file);
    std::replace(text.begin(), text.end(), '\\', '/');
    std::string escaped;
    for (const char c : text) escaped += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return "file '" + escaped + "'\n";
}
}

struct EditorHost::Impl {
    struct Session;
    saucer::application* app;
    Controller& controller;
    Paths paths;
    std::mutex ffmpeg_mutex;
    FFmpeg ffmpeg;
    std::vector<std::thread> detectors;
    uint64_t detection = 0;
    std::mutex prefs_mutex;
    std::optional<Json> prefs;
    std::list<std::unique_ptr<Session>> sessions;
    uint64_t next_id = 1;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    Impl(saucer::application* a, Controller& c, Paths p) : app(a), controller(c), paths(std::move(p)) { try { sweep_previews(); } catch (...) {} }
    // Editor preferences that the page may change. Values are validated before they are saved.
    std::expected<bool, std::string> save_page_preference(const std::string& key, const Json& value) {
        const auto number = [&](double low, double high) { return value.is_number() && value.get<double>() >= low && value.get<double>() <= high; };
        bool valid = false;
        if (key == "quality") valid = value.is_number_integer() && number(1, 10);
        else if (key == "timecode") valid = value.is_string() && (value == "duration" || value == "clock" || value == "smpte");
        else if (key == "volume_master") valid = number(0, 1);
        else if (key == "track_volumes") {
            valid = value.is_object() && value.size() <= 32;
            if (valid) for (const auto& [name, level] : value.items()) valid = valid && name.size() <= 64 && level.is_number() && level.get<double>() >= 0 && level.get<double>() <= 1;
        }
        else if (key == "scale") valid = value.is_number() && (value == 0.75 || value == 1.0 || value == 1.25 || value == 1.5);
        else if (key == "preview_cuts") valid = value.is_boolean();
        else return std::unexpected("Unknown editor preference.");
        if (!valid) return std::unexpected("That preference value isn't supported.");
        store_preference(key.c_str(), value);
        return true;
    }
    ~Impl();

    Json preferences() {
        std::lock_guard lock(prefs_mutex);
        if (!prefs) {
            prefs = Json::object();
            try {
                const auto file = paths.data / L"editor.json";
                std::error_code error;
                if (fs::exists(file, error)) { auto value = Json::parse(read_text(file)); if (value.is_object()) prefs = value; }
            } catch (...) {}
        }
        return *prefs;
    }
    void store_preference(const char* key, Json value) {
        preferences();
        std::lock_guard lock(prefs_mutex);
        (*prefs)[key] = std::move(value);
        try { atomic_write(paths.data / L"editor.json", prefs->dump(2)); } catch (...) {}
    }
    FFmpeg ffmpeg_info() { std::lock_guard lock(ffmpeg_mutex); return ffmpeg; }
    void detect(fs::path preferred = {}) {
        uint64_t generation;
        { std::lock_guard lock(ffmpeg_mutex); ffmpeg = FFmpeg{}; generation = ++detection; }
        const auto saved = preferred.empty() ? from_string(preferences().value("ffmpeg_path", "")) : preferred;
        detectors.emplace_back([this, saved, generation, explicit_choice = !preferred.empty()] {
            FFmpeg found; found.status = "missing";
            for (const auto& candidate : ffmpeg_candidates(saved)) {
                std::error_code error;
                if (!fs::is_regular_file(candidate, error)) continue;
                auto info = inspect_ffmpeg(candidate);
                if (info.status == "ready") { found = info; break; }
                if (explicit_choice && candidate == saved) { found = info; break; }
            }
            {
                std::lock_guard lock(ffmpeg_mutex);
                if (generation != detection) return;  // a newer detection (e.g. a located file) wins
                ffmpeg = found;
            }
            if (found.status == "ready") store_preference("ffmpeg_path", path_string(found.exe));
        });
    }
    bool detecting() { std::lock_guard lock(ffmpeg_mutex); return ffmpeg.status == "checking"; }
    void remove(uint64_t id);
};

struct EditorHost::Impl::Session {
    Impl& host;
    uint64_t id;
    Json clip;
    fs::path source;
    std::shared_ptr<saucer::window> window;
    std::unique_ptr<saucer::smartview> view;
    std::unique_ptr<ClipMedia> media;
    bool allow_close = false;
    std::mutex mutex;
    Json probe = {{"status", "pending"}};
    std::string wave_status = "pending";
    std::vector<unsigned char> peaks;
    Json export_state = {{"status", "idle"}};
    Json preview = {{"status", "pending"}, {"progress", 0.0}};
    fs::path preview_folder, proxy;
    std::vector<int64_t> cut_frames;  // keyframes where the original video can be copied from or to cleanly
    int64_t frame_count = 0;
    double reorder_delay = 0;         // seconds a packet is decoded before it is shown (B-frames)
    std::vector<std::pair<int, fs::path>> stems;
    std::atomic<bool> stopping{}, cancel_export{}, analyzing{}, dirty{};
    std::atomic<int64_t> last_seen{0};
    bool animations = true;
    Clock::time_point animations_checked{};
    std::thread analysis, exporter;
    uint64_t revision = 1;

    Session(Impl& h, uint64_t i, Json c) : host(h), id(i), clip(std::move(c)), source(from_string(clip.value("path", ""))) {}
    ~Session() {
        stopping = true; cancel_export = true;
        if (analysis.joinable()) analysis.join();
        if (exporter.joinable()) exporter.join();
        media.reset();
        view.reset();
        window.reset();
        if (!preview_folder.empty()) { std::error_code ignored; fs::remove_all(preview_folder, ignored); }
    }
    template<class F> void change(F&& action) { std::lock_guard lock(mutex); action(); ++revision; }

    static int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
    bool page_alive() const { return now_ms() - last_seen.load() < 5000; }
    bool exporting() { std::lock_guard lock(mutex); return export_state.value("status", "") == "running"; }

    Json prefs_for_page() {
        auto page = host.preferences();
        page.erase("ffmpeg_path");
        return page;
    }
    std::string state() {
        last_seen = now_ms();
        const auto tool = host.ffmpeg_info();
        const auto quality = std::clamp(host.preferences().value("quality", 10), 1, 10);
        Json info;
        { std::lock_guard lock(mutex); info = probe; }
        const auto plans = quality_plans(tool, info);
        if (Clock::now() - animations_checked > std::chrono::seconds(3)) {
            animations_checked = Clock::now();
            try { animations = Json::parse(host.controller.snapshot()).at("settings").value("animations_enabled", true); } catch (...) {}
        }
        std::error_code error;
        const bool missing = !fs::is_regular_file(source, error);
        std::lock_guard lock(mutex);
        return Json{
            {"clip", clip}, {"animations", animations}, {"quality", quality}, {"source_missing", missing},
            {"ffmpeg", {{"status", tool.status}, {"path", path_string(tool.exe)}, {"version", tool.version}, {"message", tool.message},
                        {"encoder", tool.nvenc ? "NVIDIA NVENC" : tool.x264 ? "x264 (CPU)" : ""}}},
            {"probe", probe}, {"waveform", {{"status", wave_status}, {"rate", 100}}}, {"export", export_state}, {"preview", preview},
            {"prefs", prefs_for_page()}, {"quality_plans", plans}, {"revision", revision}}.dump();
    }

    void start_analysis() {
        if (analyzing.exchange(true)) return;
        if (analysis.joinable()) analysis.join();
        change([&] { probe = {{"status", "pending"}}; wave_status = "pending"; });
        analysis = std::thread([this] { analyze(); analyzing = false; });
    }
    void analyze() {
        while (!stopping) {
            const auto tool = host.ffmpeg_info();
            if (tool.status == "ready") {
                const auto result = run_tool(tool.exe, L"-hide_banner -nostdin -i " + quoted(source), {}, [&] { return stopping.load(); }, 20000);
                if (stopping) return;
                auto parsed = probe_from_output(result.errors);
                if (parsed["status"] != "ready") parsed["message"] = result.canceled ? "FFmpeg took too long to read this clip." : "FFmpeg couldn't read this clip's video details.";
                const int tracks = parsed.value("audio_tracks", 0);
                change([&] { probe = parsed; wave_status = tracks ? "pending" : "none"; });
                if (parsed["status"] != "ready") { change([&] { preview = {{"status", "unavailable"}}; }); return; }
                list_keyframes(tool, parsed.value("fps", 0.0));
                if (stopping) return;
                build_stems(tool, tracks);
                if (stopping) return;
                if (tracks) waveform_pass(tool);
                if (stopping) return;
                build_proxy(tool, parsed);
                return;
            }
            if (tool.status == "missing" || tool.status == "invalid") change([&] {
                if (probe.value("status", "") != "unavailable") { probe = {{"status", "unavailable"}}; wave_status = "unavailable"; preview = {{"status", "unavailable"}}; }
            });
            for (int i = 0; i < 5 && !stopping; ++i) Sleep(100);
        }
    }

    // Lists every video packet once (well under a second) to find keyframes where the original video can
    // be copied from and to without re-encoding: closed-GOP keyframes whose place in decode order equals
    // their frame number, so copied frame counts and timestamps line up exactly.
    void list_keyframes(const FFmpeg& tool, double fps) {
        if (fps <= 0) return;
        std::string text;
        bool overflow = false;
        const auto result = run_tool(tool.exe, L"-hide_banner -nostdin -loglevel error -i " + quoted(source) + L" -map 0:v:0 -c copy -f framecrc -",
            [&](std::string_view part) { if (text.size() < 96 * 1024 * 1024) text.append(part); else overflow = true; }, [&] { return stopping.load(); }, 120000);
        if (result.code != 0 || result.canceled || overflow) return;
        struct Packet { int64_t dts, pts; bool key; };
        std::vector<Packet> packets;
        double timebase = 0;
        size_t position = 0;
        while (position < text.size()) {
            auto end = text.find('\n', position);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(position, end - position);
            position = end + 1;
            if (line.starts_with("#tb 0:")) {
                const auto slash = line.find('/');
                try { timebase = std::stod(line.substr(6, slash - 6)) / std::stod(line.substr(slash + 1)); } catch (...) {}
                continue;
            }
            if (line.empty() || line[0] == '#') continue;
            // stream, dts, pts, duration, size, checksum[, F=0x<flags>]; no flags field means a keyframe.
            int stream = 0; long long dts = 0, pts = 0;
            if (sscanf_s(line.c_str(), "%d, %lld, %lld", &stream, &dts, &pts) != 3 || pts == INT64_MIN || dts == INT64_MIN) return;
            const auto flags = line.find("F=0x");
            packets.push_back({dts, pts, flags == std::string::npos || (std::strtoul(line.c_str() + flags + 4, nullptr, 16) & 1) != 0});
        }
        if (packets.empty() || timebase <= 0) return;
        int64_t origin = INT64_MAX;
        for (const auto& packet : packets) origin = std::min(origin, packet.pts);
        if (origin * timebase > 0.5) origin = 0;  // container start offset
        std::vector<int64_t> later(packets.size() + 1, INT64_MAX);
        for (size_t i = packets.size(); i-- > 0;) later[i] = std::min(later[i + 1], packets[i].pts);
        std::vector<int64_t> cuts;
        int64_t earlier = INT64_MIN, lag = 0;
        for (size_t i = 0; i < packets.size(); ++i) {
            const auto& packet = packets[i];
            if (packet.key) {
                const double frame = (packet.pts - origin) * timebase * fps;
                lag = std::max(lag, packet.pts - packet.dts);
                // Nothing decoded before it is shown after it, nothing decoded after it is shown before it.
                if (earlier < packet.pts && later[i + 1] > packet.pts && std::llround(frame) == static_cast<int64_t>(i) && std::abs(frame - std::round(frame)) < 0.2)
                    cuts.push_back(static_cast<int64_t>(i));
            }
            earlier = std::max(earlier, packet.pts);
        }
        std::lock_guard lock(mutex);
        cut_frames = std::move(cuts);
        frame_count = static_cast<int64_t>(packets.size());
        reorder_delay = lag * timebase;
        ++revision;
    }

    bool smart_cut_possible(const FFmpeg& tool, const Json& info) {
        const auto codec = lower(info.value("codec", ""));
        if (info.value("pix_fmt", "") != "yuv420p" || info.value("fps", 0.0) <= 0) return false;
        if (codec == "hevc" ? !(tool.hevc_nvenc || tool.x265) : codec == "h264" ? !(tool.nvenc || tool.x264) : true) return false;
        std::lock_guard lock(mutex);
        return !cut_frames.empty();
    }
    // What each quality level will produce, so the page can show file sizes before exporting.
    Json quality_plans(const FFmpeg& tool, const Json& info) {
        Json plans = Json::array();
        if (tool.status != "ready" || info.value("status", "") != "ready") return plans;
        const bool smart = smart_cut_possible(tool, info);
        const auto codec = lower(info.value("codec", ""));
        const int height = info.value("height", 0);
        for (int quality = 1; quality <= 10; ++quality) {
            const auto plan = plan_for(quality);
            if (quality == 10 && smart) {
                plans.push_back({{"video_kbps", reference_kbps(info)}, {"audio_kbps", plan.audio_kbps}, {"codec", codec == "hevc" ? "HEVC" : "H.264"}, {"height", height}, {"original", true}});
                continue;
            }
            const auto choice = choose_encode(tool, info, quality, tool.nvenc);
            plans.push_back({{"video_kbps", choice.video_kbps}, {"audio_kbps", plan.audio_kbps}, {"codec", choice.hevc ? "HEVC" : "H.264"},
                             {"height", plan.max_height && height > plan.max_height ? plan.max_height : height}, {"original", false}});
        }
        return plans;
    }

    // Tracks the preview plays separately. When the first track is a mix of the others (OBS's
    // Mix + Desktop + Microphone layout), only the individual tracks get stems and sliders.
    std::vector<int> preview_tracks(int tracks) {
        Json names;
        { std::lock_guard lock(mutex); names = probe.value("audio_names", Json::array()); }
        std::string first = names.size() ? names[0].get<std::string>() : "";
        std::transform(first.begin(), first.end(), first.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const int from = tracks >= 3 || (tracks == 2 && first == "mix") ? 1 : 0;
        std::vector<int> result;
        for (int t = from; t < tracks && result.size() < 6; ++t) result.push_back(t);
        return result;
    }
    bool ensure_preview_folder() {
        if (!preview_folder.empty()) return true;
        std::error_code error;
        auto folder = preview_root() / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(id) + L"-" + wide(random_secret().substr(0, 8)));
        fs::create_directories(folder, error);
        if (error) return false;
        preview_folder = folder;
        return true;
    }
    void build_stems(const FFmpeg& tool, int tracks) {
        if (!tracks || !ensure_preview_folder()) return;
        change([&] { preview = {{"status", "building"}, {"stage", "audio"}, {"progress", 0.0}}; });
        std::wstring outputs;
        std::vector<std::pair<int, fs::path>> made;
        for (const int track : preview_tracks(tracks)) {
            auto file = preview_folder / (L"audio-" + std::to_wstring(track) + L".m4a");
            outputs += L" -map 0:a:" + std::to_wstring(track) + L" -c copy -vn -movflags +faststart " + quoted(file);
            made.emplace_back(track, file);
        }
        const auto result = run_tool(tool.exe, L"-hide_banner -nostdin -y -loglevel error -i " + quoted(source) + outputs, {}, [&] { return stopping.load(); }, 120000);
        if (result.code == 0 && !result.canceled) { std::lock_guard lock(mutex); stems = made; ++revision; }
    }
    void build_proxy(const FFmpeg& tool, const Json& info) {
        if (!ensure_preview_folder()) { change([&] { preview = {{"status", "failed"}}; }); return; }
        const double duration = info.value("duration", 0.0);
        const int height = info.value("height", 0);
        const int target = height > 0 ? std::min(height, 720) : 720;
        const auto file = preview_folder / L"preview.mp4";
        const auto partial = preview_folder / L"preview.part.mp4";
        // Short GOP without B-frames: any frame decodes from at most 5 neighbours, so scrubbing is immediate.
        const std::wstring gop = L" -g 6 -bf 0 -an -movflags +faststart -f mp4 " + quoted(partial);
        const auto nvenc = L" -c:v h264_nvenc -preset p1 -rc vbr -cq 30 -b:v 0" + gop;
        std::vector<std::pair<const char*, std::wstring>> attempts;
        if (tool.nvenc) {
            attempts.emplace_back("GPU", L"-hwaccel cuda -hwaccel_output_format cuda -i " + quoted(source) + L" -map 0:v:0 -vf scale_cuda=-2:" + std::to_wstring(target) + L":format=yuv420p" + nvenc);
            attempts.emplace_back("GPU encode", L"-i " + quoted(source) + L" -map 0:v:0 -vf scale=-2:" + std::to_wstring(target) + L",format=yuv420p" + nvenc);
        }
        if (tool.x264) attempts.emplace_back("CPU", L"-i " + quoted(source) + L" -map 0:v:0 -vf scale=-2:" + std::to_wstring(target) + L",format=yuv420p -c:v libx264 -preset ultrafast -tune fastdecode -crf 26" + gop);
        for (const auto& [label, input] : attempts) {
            change([&] { preview = {{"status", "building"}, {"stage", "video"}, {"progress", 0.0}, {"engine", label}}; });
            std::string carry;
            const auto result = run_tool(tool.exe, L"-hide_banner -nostdin -y -loglevel error -progress pipe:1 -nostats " + input, [&](std::string_view part) {
                carry.append(part);
                size_t newline;
                while ((newline = carry.find('\n')) != std::string::npos) {
                    auto line = carry.substr(0, newline);
                    carry.erase(0, newline + 1);
                    if (line.starts_with("out_time_us=") && duration > 0) {
                        try { const double done = std::clamp(std::stod(line.substr(12)) / 1e6 / duration, 0.0, 1.0); change([&] { preview["progress"] = done; }); } catch (...) {}
                    }
                }
            }, [&] { return stopping.load(); });
            if (stopping || result.canceled) return;
            std::error_code error;
            if (result.code == 0 && fs::file_size(partial, error) > 0 && !error) {
                fs::rename(partial, file, error);
                if (!error) {
                    std::lock_guard lock(mutex);
                    proxy = file;
                    preview = {{"status", "ready"}, {"progress", 1.0}, {"engine", label}};
                    ++revision;
                    return;
                }
            }
            std::error_code ignored; fs::remove(partial, ignored);
        }
        change([&] { preview = {{"status", "failed"}}; });
    }
    // URLs for the generated preview files, served only to this editor's page.
    std::string preview_urls() {
        Json result = {{"video", ""}, {"stems", Json::array()}};
        std::lock_guard lock(mutex);
        if (!proxy.empty()) result["video"] = media->generated_url(proxy);
        for (const auto& [track, file] : stems) result["stems"].push_back({{"track", track}, {"url", media->generated_url(file)}});
        return result.dump();
    }

    void waveform_pass(const FFmpeg& tool) {
        std::vector<unsigned char> values;
        int16_t peak = 0; unsigned counted = 0; std::string carry;
        const auto wave = run_tool(tool.exe, L"-hide_banner -nostdin -loglevel error -i " + quoted(source) + L" -map 0:a:0 -ac 1 -ar 8000 -f s16le -acodec pcm_s16le pipe:1",
            [&](std::string_view part) {
                carry.append(part);
                const size_t usable = carry.size() & ~size_t{1};
                for (size_t i = 0; i < usable; i += 2) {
                    int16_t sample; std::memcpy(&sample, carry.data() + i, 2);
                    const int16_t magnitude = sample == INT16_MIN ? INT16_MAX : static_cast<int16_t>(std::abs(sample));
                    peak = std::max(peak, magnitude);
                    if (++counted == 80) { values.push_back(static_cast<unsigned char>(std::min(255, peak / 128))); peak = 0; counted = 0; }
                }
                carry.erase(0, usable);
            }, [&] { return stopping.load(); });
        if (stopping) return;
        if (counted) values.push_back(static_cast<unsigned char>(std::min(255, peak / 128)));
        change([&] { peaks = std::move(values); wave_status = wave.code == 0 && !peaks.empty() ? "ready" : "failed"; });
    }

    std::string waveform() { std::lock_guard lock(mutex); return wave_status == "ready" ? base64(peaks) : std::string{}; }

    fs::path output_for() const {
        auto folder = source.parent_path();
        if (_wcsicmp(folder.filename().c_str(), L"Edited Exports") != 0) folder /= L"Edited Exports";
        std::error_code created;
        fs::create_directories(folder, created);
        if (created) throw std::runtime_error("The Edited Exports folder couldn't be created next to the original clip. Check that folder's permissions.");
        const auto stem = source.stem().wstring();
        for (unsigned i = 1; i < 1000; ++i) {
            auto candidate = folder / (stem + (i == 1 ? std::wstring(L" (edited)") : L" (edited " + std::to_wstring(i) + L")") + L".mp4");
            std::error_code error;
            if (!fs::exists(candidate, error) && !fs::exists(fs::path(candidate.wstring() + L".part"), error)) return candidate;
        }
        throw std::runtime_error("Too many edited copies of this clip already exist in Edited Exports.");
    }

    std::expected<bool, std::string> start_export(const std::string& payload) {
        try {
            const auto request = Json::parse(payload);
            const int quality = request.at("quality").get<int>();
            if (quality < 1 || quality > 10) return std::unexpected("Choose a quality from 1 to 10.");
            const auto tool = host.ffmpeg_info();
            if (tool.status != "ready") return std::unexpected("FFmpeg is needed to export. Locate ffmpeg.exe first.");
            Json info;
            { std::lock_guard lock(mutex); info = probe; if (export_state.value("status", "") == "running") return std::unexpected("An export is already running."); }
            if (info.value("status", "") != "ready") return std::unexpected("The clip is still being analyzed. Try again in a moment.");
            const double duration = info.value("duration", 0.0);
            std::vector<std::pair<double, double>> segments;
            double previous = -1, total = 0;
            for (const auto& item : request.at("segments")) {
                const double start = item.at(0).get<double>(), end = item.at(1).get<double>();
                if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end <= start || start < previous - 1e-6)
                    return std::unexpected("The kept sections couldn't be read. Undo the last change and try again.");
                previous = end;
                const double clamped = duration > 0 ? std::min(end, duration) : end;
                if (clamped - start < 0.01) continue;
                total += clamped - start;
                segments.emplace_back(start, clamped);
            }
            if (segments.empty() || total < 0.1) return std::unexpected("Keep at least one section to export.");
            if (segments.size() > 200) return std::unexpected("Exports support up to 200 kept sections. Restore or join some sections first.");
            std::error_code missing;
            if (!fs::is_regular_file(source, missing)) return std::unexpected("The original replay was moved or deleted, so it can't be exported.");
            host.store_preference("quality", quality);
            if (exporter.joinable()) exporter.join();
            cancel_export = false;
            const bool original = quality == 10 && smart_cut_possible(tool, info);
            change([&] { export_state = {{"status", "running"}, {"progress", 0.0}, {"speed", 0.0}, {"quality", quality}, {"total", total}, {"encoder", original ? "Original video" : encoder_label(tool, info, quality, tool.nvenc)}}; });
            exporter = std::thread([this, tool, info, segments, total, quality, original] { run_export(tool, info, segments, total, quality, original); });
            return true;
        } catch (const std::exception& e) { return std::unexpected(std::string("The export request could not be read. ") + e.what()); }
    }

    static std::string encoder_label(const FFmpeg& tool, const Json& info, int quality, bool hardware) {
        if (!hardware) return "H.264 · CPU";
        return std::string(choose_encode(tool, info, quality, true).hevc ? "HEVC" : "H.264") + " · NVIDIA NVENC";
    }
    // Reads FFmpeg's -progress output: reports out_time to a callback and remembers the last frame count.
    static std::function<void(std::string_view)> progress_reader(std::string& carry, int64_t& frames, std::function<void(double, double)> report) {
        return [&carry, &frames, report](std::string_view part) {
            carry.append(part);
            size_t newline;
            double speed = -1;
            while ((newline = carry.find('\n')) != std::string::npos) {
                auto line = carry.substr(0, newline);
                carry.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                try {
                    if (line.starts_with("out_time_us=")) report(std::stod(line.substr(12)) / 1e6, speed);
                    else if (line.starts_with("frame=")) frames = std::stoll(line.substr(6));
                    else if (line.starts_with("speed=")) { speed = std::stod(line.substr(6)); report(-1, speed); }
                } catch (...) {}
            }
        };
    }

    // Quality 10: frame-exact cuts that keep the original video. Between the first and last clean keyframe
    // of each kept section the original packets are copied untouched; only the frames before that first
    // keyframe and after the last one (at most one keyframe interval each) are re-encoded with the same
    // codec at near-lossless quality. Audio is re-encoded so every track is cut to the exact sample.
    ProcessResult smart_export(const FFmpeg& tool, const Json& info, const std::vector<std::pair<double, double>>& segments, double total,
                               const fs::path& work, const fs::path& temporary) {
        const auto cancelled = [&] { return cancel_export.load() || stopping.load(); };
        const double fps = info.value("fps", 0.0);
        const int tracks = info.value("audio_tracks", 0);
        const bool hevc = lower(info.value("codec", "")) == "hevc";
        std::vector<int64_t> cuts; int64_t frames; double delay;
        { std::lock_guard lock(mutex); cuts = cut_frames; frames = frame_count; delay = reorder_delay; }
        struct Part { int64_t first, count; bool copy; };
        struct Section { std::vector<Part> parts; int64_t first, last; };
        std::vector<Section> sections;
        int64_t all_frames = 0;
        for (const auto& [start, end] : segments) {
            const int64_t s = std::clamp<int64_t>(std::llround(start * fps), 0, frames), e = std::clamp<int64_t>(std::llround(end * fps), 0, frames);
            if (e <= s) continue;
            Section section{{}, s, e};
            const auto after = std::lower_bound(cuts.begin(), cuts.end(), s);
            const int64_t first_key = after == cuts.end() ? -1 : *after;
            int64_t last_key = e == frames ? e : -1;  // the end of the clip is always a clean place to stop copying
            if (last_key < 0) { const auto before = std::upper_bound(cuts.begin(), cuts.end(), e); if (before != cuts.begin()) last_key = *std::prev(before); }
            if (first_key >= 0 && first_key < e && last_key > first_key) {
                if (s < first_key) section.parts.push_back({s, first_key - s, false});
                section.parts.push_back({first_key, last_key - first_key, true});
                if (last_key < e) section.parts.push_back({last_key, e - last_key, false});
            } else section.parts.push_back({s, e - s, false});
            all_frames += e - s;
            sections.push_back(std::move(section));
        }
        ProcessResult result;
        if (sections.empty()) { result.started = true; result.errors = "Nothing to export."; return result; }
        // Re-encoded edges carry the same decode delay as the copied video so timestamps stay in order.
        const std::wstring retime = delay > 1e-6 ? L" -bsf:v \"setts=dts=PTS-round(" + number(delay) + L"/TB)\"" : L"";
        const int reference = reference_kbps(info);
        const std::wstring ceiling = L" -maxrate " + std::to_wstring(reference * 2) + L"k -bufsize " + std::to_wstring(reference * 4) + L"k";
        const auto encoder = [&](int level) -> std::wstring {
            if (level < 2) return std::wstring(hevc ? L" -c:v hevc_nvenc -profile:v main" : L" -c:v h264_nvenc -profile:v high") + L" -preset p4 -tune hq -rc vbr -cq 18 -b:v 0 -bf 0 -g 100000" + ceiling;
            return std::wstring(hevc ? L" -c:v libx265 -preset fast -crf 16 -x265-params log-level=error:bframes=0:keyint=100000:min-keyint=100000:scenecut=0"
                                     : L" -c:v libx264 -preset fast -crf 15 -profile:v high -bf 0 -g 100000 -sc_threshold 0") + L" -pix_fmt yuv420p" + ceiling;
        };
        std::vector<int> levels;  // 0: GPU decode + NVENC, 1: CPU decode + NVENC, 2: CPU encoder
        if (hevc ? tool.hevc_nvenc : tool.nvenc) { levels.push_back(0); levels.push_back(1); }
        if (hevc ? tool.x265 : tool.x264) levels.push_back(2);
        std::vector<std::pair<fs::path, int64_t>> files;
        int64_t finished = 0;
        for (const auto& section : sections) {
            for (const int level : levels) {
                std::wstring inputs, outputs;
                std::vector<std::pair<fs::path, int64_t>> made;
                for (const auto& part : section.parts) {
                    const auto file = work / (L"part-" + std::to_wstring(files.size() + made.size()) + L".ts");
                    const auto input = std::to_wstring(made.size());
                    if (part.copy) {
                        // Seeking a quarter frame past a keyframe lands exactly on it when stream copying.
                        inputs += L" -ss " + number((static_cast<double>(part.first) + 0.25) / fps) + L" -i " + quoted(source);
                        outputs += L" -map " + input + L":v:0 -c:v copy -frames:v " + std::to_wstring(part.count) + L" -f mpegts " + quoted(file);
                    } else {
                        inputs += std::wstring(level == 0 ? L" -hwaccel cuda -hwaccel_output_format cuda" : L"") + L" -ss " + number(std::max(0.0, (static_cast<double>(part.first) - 0.5) / fps)) + L" -i " + quoted(source);
                        outputs += L" -map " + input + L":v:0 -frames:v " + std::to_wstring(part.count) + encoder(level) + retime + L" -f mpegts " + quoted(file);
                    }
                    made.emplace_back(file, part.count);
                }
                result = run_tool(tool.exe, L"-hide_banner -nostdin -y -loglevel error" + inputs + outputs, {}, cancelled);
                if (result.canceled) return result;
                bool complete = result.code == 0;
                for (const auto& [file, count] : made) { std::error_code error; complete = complete && fs::file_size(file, error) > 0 && !error; }
                if (complete) { files.insert(files.end(), made.begin(), made.end()); break; }
                if (result.code == 0) result.code = 1;
            }
            if (levels.empty() || result.code != 0) return result;
            finished += section.last - section.first;
            change([&] { export_state["progress"] = 0.8 * static_cast<double>(finished) / static_cast<double>(all_frames); });
        }
        std::string list = "ffconcat version 1.0\n";
        for (const auto& [file, count] : files) list += concat_entry(file) + std::format("duration {:.6f}\n", static_cast<double>(count) / fps);
        std::wstring audio_input, audio;
        if (tracks) {
            std::string graph, chain;
            for (size_t i = 0; i < sections.size(); ++i)
                for (int t = 0; t < tracks; ++t) {
                    graph += std::format("[1:a:{}]atrim=start={:.6f}:end={:.6f},asetpts=PTS-STARTPTS[a{}_{}];", t, sections[i].first / fps, sections[i].last / fps, i, t);
                    chain += std::format("[a{}_{}]", i, t);
                }
            graph += chain + std::format("concat=n={}:v=0:a={}", sections.size(), tracks);
            for (int t = 0; t < tracks; ++t) graph += std::format("[o{}]", t);
            atomic_write(work / L"audio.txt", graph);
            audio_input = L" -i " + quoted(source) + filter_file(tool, work / L"audio.txt");
            for (int t = 0; t < tracks; ++t) audio += L" -map \"[o" + std::to_wstring(t) + L"]\"";
            audio += L" -c:a aac -b:a " + std::to_wstring(plan_for(10).audio_kbps) + L"k";
            audio += track_titles(info, tracks);
        }
        atomic_write(work / L"list.ffconcat", list);
        std::string carry;
        int64_t written = -1;
        result = run_tool(tool.exe, L"-hide_banner -nostdin -y -loglevel error -progress pipe:1 -nostats -f concat -safe 0 -i " + quoted(work / L"list.ffconcat") + audio_input +
            L" -map 0:v:0 -c:v copy" + audio + L" -movflags +faststart -f mp4 " + quoted(temporary),
            progress_reader(carry, written, [&](double seconds, double) {
                if (seconds >= 0 && total > 0) change([&] { export_state["progress"] = 0.8 + 0.2 * std::clamp(seconds / total, 0.0, 1.0); });
            }), cancelled);
        // Every kept frame must be there; anything else falls back to a full re-encode.
        if (result.code == 0 && !result.canceled && written >= 0 && written != all_frames) {
            result.code = 1;
            result.errors = std::format("The joined video has {} frames instead of {}.", written, all_frames);
        }
        return result;
    }

    // Quality 1-9 (and 10 when the original can't be kept): re-encode the kept sections into one video.
    ProcessResult encode_export(const FFmpeg& tool, const Json& info, const std::vector<std::pair<double, double>>& segments, double total, int quality,
                                const fs::path& work, const fs::path& temporary) {
        const auto plan = plan_for(quality);
        const int tracks = info.value("audio_tracks", 0);
        const int height = info.value("height", 0);
        std::vector<int> levels;  // 0: GPU decode + NVENC, 1: CPU decode + NVENC, 2: x264
        if (tool.nvenc) { levels.push_back(0); levels.push_back(1); }
        if (tool.x264) levels.push_back(2);
        ProcessResult result;
        for (const int level : levels) {
            std::error_code ignored; fs::remove(temporary, ignored);
            const bool gpu = level == 0, hardware = level < 2;
            const auto choice = choose_encode(tool, info, quality, hardware);
            // Separate seeking inputs decode only the kept sections; each GPU input holds a decoder, so cap them lower.
            const bool seeking = segments.size() <= (gpu ? 4u : 16u);
            const std::wstring hw = gpu ? L" -hwaccel cuda -hwaccel_output_format cuda" : L"";
            std::wstring inputs = seeking ? L"" : hw + L" -i " + quoted(source);
            std::string filter, chain;
            for (size_t i = 0; i < segments.size(); ++i) {
                const auto& [first, last] = segments[i];
                const auto input = seeking ? std::to_string(i) : std::string("0");
                if (seeking) inputs += hw + L" -ss " + number(first) + L" -t " + number(last - first) + L" -i " + quoted(source);
                const auto video_trim = seeking ? std::string{} : std::format("trim=start={:.6f}:end={:.6f},", first, last);
                const auto audio_trim = seeking ? std::string{} : std::format("atrim=start={:.6f}:end={:.6f},", first, last);
                filter += std::format("[{}:v:0]{}setpts=PTS-STARTPTS[v{}];", input, video_trim, i);
                chain += std::format("[v{}]", i);
                for (int t = 0; t < tracks; ++t) {
                    filter += std::format("[{}:a:{}]{}asetpts=PTS-STARTPTS,aresample=async=1:first_pts=0[a{}_{}];", input, t, audio_trim, i, t);
                    chain += std::format("[a{}_{}]", i, t);
                }
            }
            filter += chain + std::format("concat=n={}:v=1:a={}[vc]", segments.size(), tracks);
            for (int t = 0; t < tracks; ++t) filter += std::format("[o{}]", t);
            const bool scale = plan.max_height && height > plan.max_height;
            filter += gpu ? (scale ? std::format(";[vc]scale_cuda=-2:{}:format=yuv420p[v]", plan.max_height) : std::string(";[vc]scale_cuda=format=yuv420p[v]"))
                          : std::format(";[vc]{}format=yuv420p[v]", scale ? std::format("scale=-2:{}:flags=lanczos,", plan.max_height) : std::string{});
            atomic_write(work / L"video.txt", filter);
            std::wstring maps = L" -map \"[v]\"";
            for (int t = 0; t < tracks; ++t) maps += L" -map \"[o" + std::to_wstring(t) + L"]\"";
            maps += track_titles(info, tracks);
            const auto rate = std::to_wstring(choice.video_kbps);
            const std::wstring bitrate = L" -b:v " + rate + L"k -maxrate " + std::to_wstring(choice.video_kbps * 3 / 2) + L"k -bufsize " + std::to_wstring(choice.video_kbps * 2) + L"k";
            const std::wstring video = hardware
                ? std::wstring(choice.hevc ? L" -c:v hevc_nvenc -profile:v main -tag:v hvc1" : L" -c:v h264_nvenc -profile:v high") + L" -preset p4 -tune hq -rc vbr -spatial-aq 1" + bitrate
                : L" -c:v libx264 -preset faster" + bitrate;
            const std::wstring audio = tracks ? L" -c:a aac -b:a " + std::to_wstring(plan.audio_kbps) + L"k" : L"";
            change([&] { export_state["encoder"] = encoder_label(tool, info, quality, hardware); export_state["progress"] = 0.0; });
            std::string carry;
            int64_t written = -1;
            const auto started = Clock::now();
            result = run_tool(tool.exe, L"-hide_banner -nostdin -y -loglevel error -progress pipe:1 -nostats" + inputs + filter_file(tool, work / L"video.txt") + maps + video + audio +
                L" -movflags +faststart -f mp4 " + quoted(temporary), progress_reader(carry, written, [&](double seconds, double speed) {
                    if (seconds < 0) { if (speed >= 0) change([&] { export_state["speed"] = speed; }); return; }
                    const double progress = std::clamp(seconds / total, 0.0, 1.0);
                    const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
                    change([&] { export_state["progress"] = progress; export_state["eta"] = progress > 0.02 ? elapsed * (1 - progress) / progress : -1.0; });
                }), [&] { return cancel_export.load() || stopping.load(); });
            if (result.canceled || result.code == 0 || !result.started) return result;
        }
        return result;
    }

    void run_export(const FFmpeg& tool, const Json& info, const std::vector<std::pair<double, double>>& segments, double total, int quality, bool original) {
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        fs::path temporary, work;
        try {
            auto final = output_for();
            temporary = fs::path(final.wstring() + L".part");
            std::error_code made;
            work = preview_root() / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(id) + L"-export-" + wide(random_secret().substr(0, 8)));
            fs::create_directories(work, made);
            if (made) throw std::runtime_error("A temporary folder for the export could not be created.");
            ProcessResult result;
            bool finished = false;
            if (original) {
                result = smart_export(tool, info, segments, total, work, temporary);
                finished = result.canceled || result.code == 0;
                if (!finished) {
                    // Some files can't be cut this way; re-encode at full quality instead of failing.
                    change([&] { export_state["note"] = "The original video couldn't be kept for this clip, so it was re-encoded at full quality."; });
                }
            }
            if (!finished) result = encode_export(tool, info, segments, total, quality, work, temporary);
            if (result.canceled) {
                std::error_code ignored; fs::remove(temporary, ignored);
                change([&] { export_state = {{"status", "canceled"}}; });
            } else if (!result.started) {
                throw std::runtime_error(result.errors.empty() ? "FFmpeg could not be started. Locate ffmpeg.exe again." : result.errors);
            } else if (result.code != 0) {
                std::error_code ignored; fs::remove(temporary, ignored);
                const auto detail = last_line(result.errors);
                throw std::runtime_error("FFmpeg could not export this video." + (detail.empty() ? std::string{} : " " + detail));
            } else {
                bool moved = false;
                for (int i = 0; i < 20 && !moved; ++i) {
                    if (MoveFileExW(temporary.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH)) moved = true;
                    else if (GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_FILE_EXISTS) final = output_for();
                    else break;
                }
                if (!moved) { std::error_code ignored; fs::remove(temporary, ignored); throw std::runtime_error("The export finished, but Windows could not save the file in Edited Exports."); }
                std::error_code error;
                const auto bytes = fs::file_size(final, error);
                std::string history_note;
                const auto added = host.controller.add_clip(path_string(final), clip.value("app", ""));
                if (!added) history_note = added.error();
                { std::lock_guard lock(mutex); if (export_state.contains("note")) history_note = export_state["note"].get<std::string>() + (history_note.empty() ? "" : " " + history_note); }
                bool revealed = true;
                try { if (headless_mode()) revealed = false; else open_path(final, true); } catch (...) { revealed = false; }
                change([&] {
                    export_state = {{"status", "done"}, {"output", path_string(final)}, {"name", path_string(final.filename())}, {"bytes", error ? 0 : bytes},
                                    {"duration", total}, {"revealed", revealed}, {"note", history_note}, {"quality", quality}};
                });
            }
        } catch (const std::exception& e) {
            if (!temporary.empty()) { std::error_code ignored; fs::remove(temporary, ignored); }
            change([&] { export_state = {{"status", "failed"}, {"error", e.what()}}; });
        }
        if (!work.empty()) { std::error_code ignored; fs::remove_all(work, ignored); }
        if (SUCCEEDED(com)) CoUninitialize();
    }

    std::string exported_path() { std::lock_guard lock(mutex); return export_state.value("status", "") == "done" ? export_state.value("output", "") : std::string{}; }
};

EditorHost::Impl::~Impl() {
    alive.reset();
    sessions.clear();
    for (auto& detector : detectors) if (detector.joinable()) detector.join();
}

void EditorHost::Impl::remove(uint64_t id) {
    sessions.remove_if([&](const std::unique_ptr<Session>& session) { return session->id == id; });
}

std::string EditorHost::pending() const {
    int exporting = 0, edited = 0;
    for (const auto& session : impl_->sessions) {
        if (session->allow_close) continue;
        if (session->exporting()) ++exporting;
        else if (session->dirty) ++edited;
    }
    if (exporting) return exporting == 1 ? "An editor is still exporting a video. Quitting cancels the export." : std::to_string(exporting) + " editors are still exporting. Quitting cancels those exports.";
    if (edited) return edited == 1 ? "An editor has cuts that haven't been exported. Quitting discards them." : std::to_string(edited) + " editors have cuts that haven't been exported. Quitting discards them.";
    return {};
}

EditorHost::EditorHost(saucer::application* app, Controller& controller, Paths paths) : impl_(std::make_unique<Impl>(app, controller, std::move(paths))) {}
EditorHost::~EditorHost() = default;

std::expected<bool, std::string> EditorHost::open(const std::string& path) {
    auto& host = *impl_;
    try {
        if (!host.controller.knows_clip(path)) return std::unexpected("This replay is no longer in your recent clips.");
        for (auto& session : host.sessions) {
            if (session->clip.value("path", "") == path && !session->allow_close) {
                if (headless_mode()) return true;
                session->window->set_minimized(false);
                session->window->show();
                session->window->focus();
                SetForegroundWindow(session->window->native().hwnd);
                return true;
            }
        }
        Json clip = {{"path", path}};
        try {
            for (const auto& item : Json::parse(host.controller.snapshot()).at("clips")) if (item.value("path", "") == path) { clip = item; break; }
        } catch (...) {}
        if (!clip.contains("name")) clip["name"] = path_string(from_string(path).filename());
        {
            bool retry;
            { std::lock_guard lock(host.ffmpeg_mutex); retry = host.detection == 0 || (host.ffmpeg.status != "ready" && host.ffmpeg.status != "checking"); }
            if (retry) host.detect();
        }
        auto created = saucer::window::create(host.app);
        if (!created) return std::unexpected("Windows could not create the editor window.");
        auto session = std::make_unique<Impl::Session>(host, host.next_id++, clip);
        auto* self = session.get();
        self->window = std::move(*created);
        auto& window = *self->window;
        window.set_title("Edit " + clip.value("name", std::string("replay")) + " – OBSReplay");
        window.set_decorations(saucer::window::decoration::partial);
        const HWND hwnd = window.native().hwnd;
        MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
        // Size the window so a 16:9 preview fits completely above the fixed editor chrome
        // (titlebar, transport, toolbar, timeline and export bar), within the monitor's work area.
        constexpr int chrome_height = 38 + 52 + 52 + 166 + 76;
        constexpr int preferred_width = 1200;
        saucer::size size{preferred_width, chrome_height + preferred_width * 9 / 16};
        POINT cursor{};
        GetCursorPos(&cursor);
        const bool have_area = GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor) != FALSE;
        const UINT dpi = GetDpiForWindow(hwnd);
        if (have_area) {
            const int scale = dpi ? static_cast<int>(dpi) : 96;
            const int work_width = std::max(1, MulDiv(monitor.rcWork.right - monitor.rcWork.left, 96, scale) - 32);
            const int work_height = std::max(1, MulDiv(monitor.rcWork.bottom - monitor.rcWork.top, 96, scale) - 32);
            size.h = std::min(size.h, work_height);
            // A shorter window gets a matching width, so the video fills the preview instead of letterboxing.
            size.w = std::min({preferred_width, work_width, std::max(900, (size.h - chrome_height) * 16 / 9)});
        }
        window.set_size(size);
        window.set_min_size({.w = std::min(900, size.w), .h = std::min(640, size.h)});
        if (have_area) {
            RECT bounds{};
            if (GetWindowRect(hwnd, &bounds)) {
                const int offset = static_cast<int>((host.sessions.size() % 5) * 28);
                const int x = monitor.rcWork.left + std::max(0, static_cast<int>((monitor.rcWork.right - monitor.rcWork.left - (bounds.right - bounds.left)) / 2)) + offset;
                const int y = monitor.rcWork.top + std::max(0, static_cast<int>((monitor.rcWork.bottom - monitor.rcWork.top - (bounds.bottom - bounds.top)) / 2)) + offset;
                SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
        }
        const HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        window.set_background({16, 21, 29, 255});
        auto view = saucer::smartview::create({.window = self->window, .storage_path = isolated_webview_folder()});
        if (!view) return std::unexpected("The editor's WebView2 surface could not start.");
        self->view = std::make_unique<saucer::smartview>(std::move(*view));
        auto& web = *self->view;
        web.set_force_dark(true);
        web.set_background({16, 21, 29, 255});
        web.set_context_menu(false);
        // Ctrl+wheel / pinch must not zoom the editor by accident; the titlebar Scale menu sets the zoom instead.
        {
            ComPtr<ICoreWebView2Settings> settings;
            if (SUCCEEDED(web.native().webview->get_Settings(&settings)) && settings) settings->put_IsZoomControlEnabled(FALSE);
            const auto scale = host.preferences().value("scale", 1.0);
            if (scale == 0.75 || scale == 1.25 || scale == 1.5) web.native().controller->put_ZoomFactor(scale);
        }
        self->media = std::make_unique<ClipMedia>(web.native().webview, host.controller);
        const auto id = self->id;
        auto* impl = &host;
        window.on<saucer::window::event::close>([self] {
            if (self->allow_close || !self->page_alive()) return saucer::policy::allow;
            self->view->execute("window.dispatchEvent(new CustomEvent('editor:close-request'))");
            return saucer::policy::block;
        });
        window.on<saucer::window::event::closed>([impl, id, token = std::weak_ptr<bool>(impl->alive)] {
            impl->app->post([impl, id, token] { if (token.lock()) impl->remove(id); });
        });
        web.on<saucer::webview::event::navigate>([](const saucer::navigation& navigation) {
            const auto url = navigation.url();
            return !navigation.new_window() && url.scheme() == "saucer" && url.host() == "embedded" && url.path() == std::filesystem::path("/editor.html") ? saucer::policy::allow : saucer::policy::block;
        });
        web.on<saucer::webview::event::permission>([](const std::shared_ptr<saucer::permission::request>& request) { request->accept(false); return saucer::status::handled; });
        web.expose("editor_state", [self] { return self->state(); });
        web.expose("editor_waveform", [self] { return self->waveform(); });
        web.expose("media_url", [self](std::string requested) -> std::expected<std::string, std::string> {
            if (requested != self->clip.value("path", "")) return std::unexpected("This editor can only play its own clip.");
            return self->media->url(requested);
        });
        web.expose("start_export", [self](std::string payload) { return self->start_export(payload); });
        web.expose("cancel_export", [self] { self->cancel_export = true; });
        web.expose("pick_ffmpeg", [self, impl]() -> std::expected<bool, std::string> {
            try {
                const auto chosen = pick_ffmpeg_file(self->window->native().hwnd, impl->ffmpeg_info().exe);
                if (chosen.empty()) return false;
                impl->detect(from_string(chosen));
                return true;
            } catch (const std::exception& e) { return std::unexpected(e.what()); }
        });
        web.expose("ffmpeg_help", [] { try { open_help("ffmpeg"); } catch (...) {} });
        web.expose("reveal_output", [self](bool reveal) -> std::expected<bool, std::string> {
            const auto output = self->exported_path();
            if (output.empty()) return std::unexpected("Export a video first.");
            try { open_path(from_string(output), reveal); return true; } catch (const std::exception& e) { return std::unexpected(e.what()); }
        });
        web.expose("close_editor", [self, impl, id, token = std::weak_ptr<bool>(impl->alive)] {
            self->cancel_export = true;
            impl->app->post([impl, id, token] {
                if (!token.lock()) return;
                for (auto& session : impl->sessions) if (session->id == id) { session->allow_close = true; session->window->close(); }
            });
        });
        web.expose("editor_dirty", [self](bool value) { self->dirty = value; });
        web.expose("preview_urls", [self] { return self->preview_urls(); });
        web.expose("save_pref", [self, impl](std::string key, std::string value) -> std::expected<bool, std::string> {
            try {
                const auto parsed = Json::parse(value);
                const auto saved = impl->save_page_preference(key, parsed);
                if (saved && key == "scale") self->view->native().controller->put_ZoomFactor(parsed.get<double>());
                if (saved) { std::lock_guard lock(self->mutex); ++self->revision; }
                return saved;
            } catch (const std::exception&) { return std::unexpected("That preference couldn't be read."); }
        });
        web.expose("retry_analysis", [self] { self->start_analysis(); });
        web.embed(saucer::embedded::all());
        web.serve("/editor.html");
        self->last_seen = Impl::Session::now_ms();
        self->start_analysis();
        host.sessions.push_back(std::move(session));
        if (headless_mode()) show_headless(hwnd);
        else { window.show(); window.focus(); }
        return true;
    } catch (const std::exception& e) { return std::unexpected(std::string("The editor could not open. ") + e.what()); }
}
}
