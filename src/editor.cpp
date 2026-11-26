#include "editor.hpp"
#include "log.hpp"
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
#include <map>
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
struct ProcessResult { DWORD code = 1; std::string errors; bool canceled = false; bool started = false; bool fit_failed = false; };
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

struct FFmpeg { std::string status = "checking"; fs::path exe; std::string version, message; bool nvenc = false, hevc_nvenc = false, x264 = false, x265 = false, mp3 = false; };

// Decodes the first audio track to 8 kHz mono and keeps one peak (0..255) per 80 samples: 100 peaks per second.
// The editor and the replay viewer draw their waveform lanes from this.
struct Peaks { std::vector<unsigned char> values; bool complete = false; };
Peaks audio_peaks(const FFmpeg& tool, const fs::path& source, const std::function<bool()>& canceled) {
    Peaks result;
    int16_t peak = 0; unsigned counted = 0; std::string carry;
    const auto wave = run_tool(tool.exe, L"-hide_banner -nostdin -loglevel error -i " + quoted(source) + L" -map 0:a:0 -ac 1 -ar 8000 -f s16le -acodec pcm_s16le pipe:1",
        [&](std::string_view part) {
            carry.append(part);
            const size_t usable = carry.size() & ~size_t{1};
            for (size_t i = 0; i < usable; i += 2) {
                int16_t sample; std::memcpy(&sample, carry.data() + i, 2);
                const int16_t magnitude = sample == INT16_MIN ? INT16_MAX : static_cast<int16_t>(std::abs(sample));
                peak = std::max(peak, magnitude);
                if (++counted == 80) { result.values.push_back(static_cast<unsigned char>(std::min(255, peak / 128))); peak = 0; counted = 0; }
            }
            carry.erase(0, usable);
        }, canceled);
    if (wave.canceled) return result;
    if (counted) result.values.push_back(static_cast<unsigned char>(std::min(255, peak / 128)));
    result.complete = wave.code == 0;
    return result;
}

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
    info.mp3 = encoders.find(" libmp3lame ") != std::string::npos;
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
// Which recorded audio tracks an export carries, and how loud. With every listed track ticked at full volume the
// file keeps its tracks as recorded (OBS's own Mix included). Any other choice leads with a mix of the ticked
// tracks at their set volumes, because most players only play the first track, followed by each ticked track
// under its own name at its set volume. One ticked track is exported alone, and none gives a silent video.
struct AudioPlan {
    std::vector<int> tracks;  // recorded track numbers, in order
    bool mix = false;
    std::vector<double> gains;  // volume of each track, 0 to 1, in the order of `tracks`; empty means all at 1
    double gain(size_t i) const { return i < gains.size() ? gains[i] : 1.0; }
    int streams() const { return static_cast<int>(tracks.size()) + (mix ? 1 : 0); }
};
// MP4 stores a track's name from its "title" tag; the recorded names are re-applied to the new audio tracks.
std::wstring track_titles(const Json& info, const AudioPlan& plan) {
    std::vector<std::string> titles;
    if (plan.mix) titles.emplace_back("Mix");
    const auto names = info.value("audio_names", Json::array());
    for (const int t : plan.tracks) titles.push_back(t < static_cast<int>(names.size()) && names[t].is_string() ? names[t].get<std::string>() : std::string{});
    std::wstring result;
    for (size_t i = 0; i < titles.size(); ++i) {
        auto name = titles[i];
        std::erase(name, '"'); std::erase(name, '\\');
        if (!name.empty()) result += L" -metadata:s:a:" + std::to_wstring(i) + L" title=\"" + wide(name) + L"\"";
    }
    return result;
}
// Adds the mix, when there is one, to a filter graph whose joined tracks are labelled [o<track>], and returns
// the audio output labels in file order. normalize=0 adds the tracks at their own levels, as OBS builds its Mix.
std::vector<std::string> audio_outputs(const AudioPlan& plan, std::string& graph) {
    std::vector<std::string> labels;
    // A track with its volume turned down passes through a volume filter first: the same linear gain the preview
    // applies, so the export sounds as the editor did. The mix is then built from the adjusted tracks.
    std::vector<std::string> sources;
    for (size_t i = 0; i < plan.tracks.size(); ++i) {
        const int t = plan.tracks[i];
        if (std::abs(plan.gain(i) - 1.0) < 0.0005) { sources.push_back(std::format("o{}", t)); continue; }
        graph += std::format(";[o{0}]volume={1:.4f}[g{0}]", t, plan.gain(i));
        sources.push_back(std::format("g{}", t));
    }
    if (!plan.mix) return sources;
    std::string inputs;
    labels.emplace_back("mix");
    for (size_t i = 0; i < plan.tracks.size(); ++i) {
        const int t = plan.tracks[i];
        graph += std::format(";[{1}]asplit=2[m{0}][k{0}]", t, sources[i]);
        inputs += std::format("[m{}]", t);
        labels.push_back(std::format("k{}", t));
    }
    graph += std::format(";{}amix=inputs={}:duration=longest:normalize=0[mix]", inputs, plan.tracks.size());
    return labels;
}

// Size limits (Up to 10 MB ... 500 MB, 1 MB = 1,000,000 bytes). The limit, less 3% held back, less what MP4 adds
// around the media, less the audio, is what the video may use. Below 500 kbps a video looks too broken to offer,
// so a limit that leaves less than that for this length is shown as unavailable. The page mirrors these numbers.
constexpr std::array<int, 5> size_limits{10, 25, 50, 100, 500};
constexpr int limit_min_video_kbps = 500;
// A fixed header plus about 16 bytes of index per video frame and per AAC frame (about 47 a second per track).
double container_bytes(double seconds, double fps, int streams) { return 64 * 1024 + 16.0 * seconds * (std::max(fps, 1.0) + 47.0 * streams); }
struct LimitPlan { int video_kbps = 0, audio_kbps = 0; bool possible = false; };
LimitPlan limit_plan(double limit_bytes, double seconds, double fps, int streams) {
    const double usable = (limit_bytes * 0.97 - container_bytes(seconds, fps, streams)) * 8 / 1000 / std::max(seconds, 0.1);
    const int audio = usable < 2000 ? 64 : usable < 6000 ? 96 : 128;
    // AAC can run a few percent over its nominal rate.
    const int video = static_cast<int>(usable - streams * audio * 1.05);
    return {video, audio, video >= limit_min_video_kbps};
}
// The tallest standard height (never taller than the clip) that still gets enough bits per pixel at this bitrate;
// below that the picture breaks into blocks, and a smaller picture looks better at the same size. 0 keeps the height.
int limit_height(const Json& info, int kbps, bool hevc) {
    const int height = info.value("height", 0), width = info.value("width", 0);
    if (height <= 0) return 0;
    const double fps = std::max(1.0, info.value("fps", 30.0)), aspect = width > 0 ? static_cast<double>(width) / height : 16.0 / 9;
    const double needed = hevc ? 0.03 : 0.045;
    std::vector<int> steps{height};
    for (const int step : {2160, 1440, 1080, 720, 540, 480, 360}) if (step < height) steps.push_back(step);
    for (const int step : steps)
        if (kbps * 1000.0 / (aspect * step * step * fps) >= needed) return step == height ? 0 : step;
    return steps.back() == height ? 0 : steps.back();
}
std::string concat_entry(const fs::path& file) {
    auto text = path_string(file);
    std::replace(text.begin(), text.end(), '\\', '/');
    std::string escaped;
    for (const char c : text) escaped += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return "file '" + escaped + "'\n";
}

// The frame shown at `time`, as a full-size PNG. The seek lands a quarter frame before that frame, so FFmpeg's first
// frame at or after it is the frame the player shows. At the end of the clip the playhead can sit past the last
// frame's start (the container often runs a little longer than the video), so the frame number is held to the last
// frame (`frames`, 0 when unknown), and a seek that still finds nothing takes the last frame from the end instead.
ProcessResult extract_frame(const FFmpeg& tool, const fs::path& source, double time, double fps, double duration, int64_t frames,
                            const fs::path& output, const std::function<bool()>& cancelled) {
    if (frames <= 0 && fps > 0 && duration > 0) frames = static_cast<int64_t>(std::ceil(duration * fps - 1e-6));
    double seek = time;
    if (fps > 0) {
        auto frame = static_cast<int64_t>(std::floor(time * fps + 1e-3));
        if (frames > 0) frame = std::min(frame, frames - 1);
        seek = std::max(0.0, (static_cast<double>(frame) - 0.25) / fps);
    }
    const std::wstring picture = L" -map 0:v:0 -vf \"scale=in_color_matrix=auto:in_range=auto,format=rgb24\" -c:v png -f image2 -update 1 ";
    auto result = run_tool(tool.exe, L"-hide_banner -nostdin -y -loglevel error -ss " + number(seek) + L" -i " + quoted(source) + L" -frames:v 1" + picture + quoted(output), {}, cancelled, 60000);
    std::error_code error;
    if (result.code == 0 && !result.canceled && (fs::file_size(output, error) == 0 || error))
        // -update 1 keeps overwriting the picture, so the last frame decoded is the one left.
        result = run_tool(tool.exe, L"-hide_banner -nostdin -y -loglevel error -sseof -1 -i " + quoted(source) + picture + quoted(output), {}, cancelled, 60000);
    return result;
}

// What an export makes. "mp4" is the edited video; the others come from the export menu.
const std::array<std::string_view, 6> export_formats{"mp4", "vertical", "gif", "mp3", "wav", "png"};
bool video_format(std::string_view format) { return format == "mp4" || format == "vertical"; }
std::wstring format_extension(std::string_view format) {
    return format == "gif" ? L".gif" : format == "mp3" ? L".mp3" : format == "wav" ? L".wav" : format == "png" ? L".png" : L".mp4";
}
// The word a file gets when it keeps the clip's name: "Replay (edited).mp4", "Replay (vertical).mp4", "Replay (frame).png".
std::wstring format_label(std::string_view format) {
    return format == "vertical" ? L"vertical" : format == "mp3" || format == "wav" ? L"audio" : format == "png" ? L"frame" : L"edited";
}

// A vertical export is 9:16. Fill crops a 9:16 window out of the frame at a chosen place (0 left edge, 1 right edge);
// Fit shows the whole frame across the middle over a blurred, zoomed copy of itself.
struct Framing { bool vertical = false, fit = false; double crop = 0.5; };
// 1080x1920, or 720x1280 from a clip under 1000 lines, so a small clip is not blown up further.
int vertical_base(const Json& info) { return info.value("height", 0) >= 1000 ? 1920 : 1280; }
int vertical_width(int height) { return static_cast<int>(std::lround(height * 9.0 / 16 / 2)) * 2; }
// The tallest vertical size that still gets enough bits per pixel at this bitrate, as limit_height does for landscape.
int vertical_limit_height(const Json& info, int kbps) {
    const int base = vertical_base(info);
    const double fps = std::max(1.0, info.value("fps", 30.0));
    int last = base;
    for (const int step : {1920, 1280, 960, 854}) {
        if (step > base) continue;
        last = step;
        if (kbps * 1000.0 / (vertical_width(step) * static_cast<double>(step) * fps) >= 0.045) return step;
    }
    return last;
}
// The filter that turns the joined video [vc] into the vertical picture [v]. Runs on the CPU.
std::string vertical_filter(const Framing& framing, int height) {
    const int width = vertical_width(height);
    if (!framing.fit)
        return std::format("[vc]crop=w='min(iw,trunc(ih*9/16/2)*2)':h=ih:x='(iw-ow)*{:.4f}':y=0,scale={}:{}:flags=lanczos,setsar=1,format=yuv420p[v]",
                           std::clamp(framing.crop, 0.0, 1.0), width, height);
    // The background is blurred at a quarter size, which looks the same and costs a fraction of the work.
    const int small_width = std::max(2, width / 8 * 2), small_height = std::max(2, height / 8 * 2);
    return std::format("[vc]split=2[vb][vf];[vb]scale={0}:{1}:force_original_aspect_ratio=increase,crop={0}:{1},boxblur=10:2,scale={2}:{3},eq=brightness=-0.12:saturation=0.9[vbg];"
                       "[vf]scale={2}:-2:flags=lanczos[vfg];[vbg][vfg]overlay=(W-w)/2:(H-h)/2,setsar=1,format=yuv420p[v]",
                       small_width, small_height, width, height);
}
// GIF sizes: picture height and frame rate. GIFs of game footage grow fast, so the default is modest.
struct GifPreset { int height, fps; };
GifPreset gif_preset(std::string_view name) { return name == "small" ? GifPreset{360, 12} : name == "large" ? GifPreset{720, 20} : GifPreset{480, 15}; }
constexpr double gif_max_seconds = 60;
// GIF compression, 0 (none) to 4 (maximum): fewer colours, and from Strong on no dithering, which LZW packs far better.
// Measured on 6 s of 480p 15 fps game footage: 14.0, 10.5, 8.5, 6.4 and 5.1 MB.
struct GifCompression { int colors; const char* dither; };
GifCompression gif_compression(int level) {
    static constexpr std::array<GifCompression, 5> levels{{{256, "bayer:bayer_scale=4"}, {128, "bayer:bayer_scale=5"}, {64, "bayer:bayer_scale=5"}, {32, "none"}, {16, "none"}}};
    return levels[static_cast<size_t>(std::clamp(level, 0, 4))];
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
    std::function<void(const std::string&)> play;
    // Replay viewer waveform: one decode at a time on a worker; asking for another clip makes the running one stale.
    std::atomic<uint64_t> wave_run{0};
    std::map<uint64_t, std::thread> wave_workers;
    // The replay viewer's screenshot button: one frame at a time, on a worker joined on the UI thread.
    uint64_t frame_run = 0;
    std::map<uint64_t, std::thread> frame_workers;
    std::string wave_key, wave_json;  // last finished result (UI thread only)
    // One-click FFmpeg install through winget, the same way guided setup installs OBS Studio.
    std::mutex install_mutex;
    std::string install_stage, install_error;  // stage: "", preparing, downloading, installing, verifying
    std::thread installer;
    std::atomic<bool> closing{false};
    void set_install(std::string stage, std::string error = {}) { std::lock_guard lock(install_mutex); install_stage = std::move(stage); install_error = std::move(error); }
    Json install_state() {
        static const bool available = !winget_path().empty();
        std::lock_guard lock(install_mutex);
        return Json{{"stage", install_stage}, {"error", install_error}, {"available", available}};
    }
    std::expected<bool, std::string> install_ffmpeg() {
        { std::lock_guard lock(install_mutex); if (!install_stage.empty()) return std::unexpected("FFmpeg is already being installed."); }
        const auto winget = winget_path();
        if (winget.empty()) return std::unexpected("Automatic install isn't available because Windows Package Manager (winget) is missing on this PC. Select Get FFmpeg instead.");
        if (installer.joinable()) installer.join();
        set_install("preparing");
        log_info("FFmpeg install started: winget={}", quoted_value(path_string(winget)));
        installer = std::thread([this, winget] {
            std::string tail;
            int reached = 0;
            const int code = run_process(winget, L"install --id Gyan.FFmpeg --exact --source winget --silent --accept-package-agreements --accept-source-agreements",
                30 * 60 * 1000, [&](const std::string& chunk) {
                    tail += chunk;
                    if (tail.size() > 16384) tail.erase(0, tail.size() - 16384);
                    if (reached < 1 && tail.find("Downloading") != std::string::npos) { reached = 1; set_install("downloading"); }
                    if (reached < 2 && (tail.find("Extracting archive") != std::string::npos || tail.find("Starting package install") != std::string::npos || tail.find("Successfully verified") != std::string::npos)) { reached = 2; set_install("installing"); }
                }, [&] { return closing.load(); });
            if (code == -2) { set_install(""); return; }
            set_install("verifying");
            // winget unpacks FFmpeg under the user's WinGet packages folder, one of the places detection already looks.
            FFmpeg found; found.status = "missing";
            for (const auto& candidate : ffmpeg_candidates({})) {
                std::error_code error;
                if (!fs::is_regular_file(candidate, error)) continue;
                auto info = inspect_ffmpeg(candidate);
                if (info.status == "ready") { found = info; break; }
            }
            if (found.status == "ready") {
                { std::lock_guard lock(ffmpeg_mutex); ++detection; ffmpeg = found; }
                store_preference("ffmpeg_path", path_string(found.exe));
                set_install("");
                log_info("FFmpeg installed: path={} version={}", quoted_value(path_string(found.exe)), quoted_value(found.version));
                return;
            }
            std::string problem;
            if (code == -1) problem = "The FFmpeg download didn't finish. Check your internet connection and select Install FFmpeg again.";
            else if (tail.find("already installed") != std::string::npos) problem = "winget reports FFmpeg as installed, but OBSReplay can't find ffmpeg.exe. Select Locate ffmpeg.exe.";
            else { std::ostringstream text; text << "FFmpeg wasn't installed (winget code 0x" << std::hex << static_cast<unsigned>(code) << "). Select Install FFmpeg again, or Get FFmpeg to install it yourself."; problem = text.str(); }
            log_error("FFmpeg install failed: code={} error={}", code, quoted_value(problem));
            set_install("", problem);
        });
        return true;
    }

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
        else if (key == "preview_quality") valid = value.is_string() && (value == "full" || value == "half" || value == "quarter");
        else if (key == "size_limit") valid = value.is_number_integer() && (value == 0 || std::ranges::find(size_limits, value.get<int>()) != size_limits.end());
        else if (key == "vertical_framing") valid = value.is_string() && (value == "fill" || value == "fit");
        else if (key == "vertical_crop") valid = number(0, 1);
        else if (key == "gif_size") valid = value.is_string() && (value == "small" || value == "medium" || value == "large");
        else if (key == "gif_compression") valid = value.is_number_integer() && number(0, 4);
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
            log_info("FFmpeg checked: status={} path={} version={} nvenc={} x264={}{}", found.status, quoted_value(path_string(found.exe)), quoted_value(found.version), found.nvenc, found.x264, found.message.empty() ? "" : " message=" + quoted_value(found.message));
            if (found.status == "ready") store_preference("ffmpeg_path", path_string(found.exe));
        });
    }
    bool detecting() { std::lock_guard lock(ffmpeg_mutex); return ffmpeg.status == "checking"; }
    void detect_if_needed() {
        bool retry;
        { std::lock_guard lock(ffmpeg_mutex); retry = detection == 0 || (ffmpeg.status != "ready" && ffmpeg.status != "checking"); }
        if (retry) detect();
    }
    void waveform(const std::string& path, std::function<void(std::string)> done) {
        static const std::string unavailable = R"({"status":"unavailable"})";
        const uint64_t run = ++wave_run;  // a decode that is still running is now stale and stops
        if (!controller.knows_clip(path)) return done(unavailable);
        const auto source = from_string(path);
        std::error_code error;
        const auto bytes = fs::file_size(source, error);
        if (error) return done(unavailable);
        const auto written = fs::last_write_time(source, error);
        const auto key = path + "|" + std::to_string(bytes) + "|" + std::to_string(error ? 0 : written.time_since_epoch().count());
        if (key == wave_key) return done(wave_json);
        detect_if_needed();
        wave_workers.emplace(run, std::thread([this, run, source, key, token = std::weak_ptr<bool>(alive), done = std::move(done)]() mutable {
            std::string json = unavailable;
            try {
                const auto stale = [&] { return wave_run.load() != run; };
                for (int i = 0; i < 2000 && detecting() && !stale(); ++i) Sleep(10);
                const auto tool = ffmpeg_info();
                if (!stale() && tool.status == "ready") {
                    const auto wave = audio_peaks(tool, source, stale);
                    if (wave.complete && !wave.values.empty()) json = Json{{"status", "ready"}, {"rate", 100}, {"peaks", base64(wave.values)}}.dump();
                }
            } catch (...) {}
            // The promise is settled on the UI thread. This worker never waits for it, so it can always be joined.
            app->post([this, run, key, token, json = std::move(json), done = std::move(done)]() mutable {
                if (!token.lock()) return;
                if (const auto worker = wave_workers.find(run); worker != wave_workers.end()) { worker->second.join(); wave_workers.erase(worker); }
                if (json != unavailable) { wave_key = key; wave_json = json; }
                done(std::move(json));
            });
        }));
    }
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
    fs::path preview_folder;
    // Preview quality: "full" plays the original clip; "half"/"quarter" play a smaller copy with a short
    // keyframe interval for instant scrubbing. Each copy is built once per editor and kept until it closes.
    std::string preview_level = "full";
    std::map<std::string, fs::path> proxies;
    std::thread previewer;
    std::mutex preview_requests;
    std::atomic<bool> cancel_preview{};
    std::vector<int64_t> cut_frames;  // keyframes where the original video can be copied from or to cleanly
    int64_t frame_count = 0;
    double reorder_delay = 0;         // seconds a packet is decoded before it is shown (B-frames)
    std::vector<std::pair<int, fs::path>> stems;
    std::atomic<bool> stopping{}, cancel_export{}, analyzing{}, dirty{};
    std::atomic<int64_t> last_seen{0};
    bool animations = true;
    std::string theme = saved_theme();
    Clock::time_point animations_checked{};
    std::thread analysis, exporter;
    uint64_t revision = 1;

    Session(Impl& h, uint64_t i, Json c) : host(h), id(i), clip(std::move(c)), source(from_string(clip.value("path", ""))) {}
    ~Session() {
        stopping = true; cancel_export = true;
        if (analysis.joinable()) analysis.join();
        { std::lock_guard serial(preview_requests); cancel_preview = true; if (previewer.joinable()) previewer.join(); }
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
            try { const auto settings = Json::parse(host.controller.snapshot()).at("settings"); animations = settings.value("animations_enabled", true); theme = settings.value("theme", theme); } catch (...) {}
        }
        std::error_code error;
        const bool missing = !fs::is_regular_file(source, error);
        std::lock_guard lock(mutex);
        return Json{
            {"clip", clip}, {"animations", animations}, {"theme", theme}, {"quality", quality}, {"source_missing", missing},
            {"ffmpeg", {{"status", tool.status}, {"path", path_string(tool.exe)}, {"version", tool.version}, {"message", tool.message}, {"install", host.install_state()},
                        {"encoder", tool.nvenc ? "NVIDIA NVENC" : tool.x264 ? "x264 (CPU)" : ""}, {"mp3", tool.mp3}}},
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
                std::string level = "full";
                try { level = host.preferences().value("preview_quality", "full"); } catch (...) {}
                request_preview(level == "half" || level == "quarter" ? level : "full", true);
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
        // Export vertical: always re-encoded as H.264 at the vertical size.
        for (int quality = 1; quality <= 10; ++quality) {
            const auto rate = vertical_quality_rate(info, quality)(false);
            plans[quality - 1]["vertical_kbps"] = rate.video_kbps;
            plans[quality - 1]["vertical_height"] = rate.max_height;
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
    bool preview_halted() const { return stopping.load() || cancel_preview.load(); }
    // Switches the preview to a quality level, building that level's copy in the background when needed.
    // While the clip is still being analysed only the choice is recorded; analysis applies it when it finishes.
    void request_preview(const std::string& level, bool from_analysis = false) {
        if (!from_analysis && analyzing) { std::lock_guard lock(mutex); preview_level = level; return; }
        std::lock_guard serial(preview_requests);
        cancel_preview = true;
        if (previewer.joinable()) previewer.join();
        cancel_preview = false;
        Json info;
        {
            std::lock_guard lock(mutex);
            preview_level = level;
            info = probe;
            if (level == "full") { preview = {{"status", "original"}, {"level", level}}; ++revision; return; }
            if (proxies.contains(level)) { preview = {{"status", "ready"}, {"level", level}, {"progress", 1.0}}; ++revision; return; }
        }
        const auto tool = host.ffmpeg_info();
        if (info.value("status", "") != "ready" || tool.status != "ready") return;  // analysis requests it once the clip is read
        previewer = std::thread([this, tool, info, level] { build_proxy(tool, info, level); });
    }
    void preview_failed(const std::string& level) {
        change([&] { if (preview_level == level) preview = {{"status", "failed"}, {"level", level}}; });
    }
    void preview_built(const std::string& level, const fs::path& file, const char* engine) {
        std::lock_guard lock(mutex);
        proxies[level] = file;
        if (preview_level == level) preview = {{"status", "ready"}, {"level", level}, {"progress", 1.0}, {"engine", engine}};
        ++revision;
    }
    void build_proxy(const FFmpeg& tool, const Json& info, const std::string& level) {
        if (!ensure_preview_folder()) { preview_failed(level); return; }
        const double duration = info.value("duration", 0.0);
        const int height = info.value("height", 0);
        const int divisor = level == "quarter" ? 4 : 2;
        const int target = height > 0 ? std::max(144, (height / divisor) & ~1) : 540;
        const auto name = wide(level);
        const auto file = preview_folder / (L"preview-" + name + L".mp4");
        const auto partial = preview_folder / (L"preview-" + name + L".part.mp4");
        // Visually clean at its size: these copies are what the editor shows, not just a scrubbing aid.
        const std::wstring cq = L"21";
        if (tool.nvenc && build_proxy_halves(tool, info, level, target, cq, file, partial)) return;
        // Short GOP without B-frames: any frame decodes from at most 5 neighbours, so scrubbing is immediate.
        const std::wstring gop = L" -g 6 -bf 0 -an -movflags +faststart -f mp4 " + quoted(partial);
        const auto nvenc = L" -c:v h264_nvenc -preset p2 -rc vbr -cq " + cq + L" -b:v 0" + gop;
        std::vector<std::pair<const char*, std::wstring>> attempts;
        if (tool.nvenc) {
            attempts.emplace_back("GPU", L"-threads 1 -hwaccel cuda -hwaccel_output_format cuda -i " + quoted(source) + L" -map 0:v:0 -vf scale_cuda=-2:" + std::to_wstring(target) + L":format=yuv420p" + nvenc);
            attempts.emplace_back("GPU encode", L"-i " + quoted(source) + L" -map 0:v:0 -vf scale=-2:" + std::to_wstring(target) + L",format=yuv420p" + nvenc);
        }
        if (tool.x264) attempts.emplace_back("CPU", L"-i " + quoted(source) + L" -map 0:v:0 -vf scale=-2:" + std::to_wstring(target) + L",format=yuv420p -c:v libx264 -preset veryfast -tune fastdecode -crf 20" + gop);
        for (const auto& [label, input] : attempts) {
            change([&] { if (preview_level == level) preview = {{"status", "building"}, {"stage", "video"}, {"level", level}, {"progress", 0.0}, {"engine", label}}; });
            std::string carry;
            const auto result = run_tool(tool.exe, L"-hide_banner -nostdin -y -loglevel error -progress pipe:1 -nostats " + input, [&](std::string_view part) {
                carry.append(part);
                size_t newline;
                while ((newline = carry.find('\n')) != std::string::npos) {
                    auto line = carry.substr(0, newline);
                    carry.erase(0, newline + 1);
                    if (line.starts_with("out_time_us=") && duration > 0) {
                        try { const double done = std::clamp(std::stod(line.substr(12)) / 1e6 / duration, 0.0, 1.0); change([&] { if (preview_level == level) preview["progress"] = done; }); } catch (...) {}
                    }
                }
            }, [&] { return preview_halted(); });
            if (preview_halted() || result.canceled) { std::error_code ignored; fs::remove(partial, ignored); return; }
            std::error_code error;
            if (result.code == 0 && fs::file_size(partial, error) > 0 && !error) {
                fs::rename(partial, file, error);
                if (!error) { preview_built(level, file, label); return; }
            }
            std::error_code ignored; fs::remove(partial, ignored);
        }
        preview_failed(level);
    }
    // NVIDIA only: one FFmpeg process leaves the GPU video decoder about half idle, so the two halves of
    // the clip are encoded at the same time (about 40% faster), split on a clean keyframe so they join
    // without re-encoding. Returns false (nothing kept) when the clip doesn't qualify or anything goes
    // wrong, including GPU session limits, and the single-process build then runs as before.
    bool build_proxy_halves(const FFmpeg& tool, const Json& info, const std::string& level, int target, const std::wstring& cq, const fs::path& file, const fs::path& partial) {
        const double fps = info.value("fps", 0.0), duration = info.value("duration", 0.0);
        std::vector<int64_t> cuts; int64_t frames;
        { std::lock_guard lock(mutex); cuts = cut_frames; frames = frame_count; }
        if (fps <= 0 || duration < 20 || frames < 600 || cuts.empty()) return false;
        int64_t split = -1;
        for (const auto cut : cuts)
            if (cut >= frames * 3 / 10 && cut <= frames * 7 / 10 && (split < 0 || std::llabs(cut - frames / 2) < std::llabs(split - frames / 2))) split = cut;
        if (split < 0) return false;
        const std::array<std::pair<int64_t, int64_t>, 2> halves{{{0, split}, {split, frames - split}}};
        const auto name = wide(level);
        const std::array<fs::path, 2> parts{preview_folder / (L"preview-" + name + L"-0.ts"), preview_folder / (L"preview-" + name + L"-1.ts")};
        const auto list = preview_folder / (L"preview-" + name + L".ffconcat");
        const auto cleanup = [&] { std::error_code ignored; for (const auto& part : parts) fs::remove(part, ignored); fs::remove(list, ignored); fs::remove(partial, ignored); };
        std::array<std::atomic<double>, 2> encoded{};
        std::array<ProcessResult, 2> results;
        change([&] { if (preview_level == level) preview = {{"status", "building"}, {"stage", "video"}, {"level", level}, {"progress", 0.0}, {"engine", "GPU"}}; });
        const auto encode = [&](size_t index) {
            try {
                const auto [first, count] = halves[index];
                // Seeking half a frame early decodes from the keyframe before and drops frames until the split.
                const std::wstring seek = first > 0 ? L" -ss " + number((static_cast<double>(first) - 0.5) / fps) : L"";
                std::string carry;
                int64_t written = -1;
                results[index] = run_tool(tool.exe, L"-hide_banner -nostdin -y -loglevel error -progress pipe:1 -nostats -threads 1 -hwaccel cuda -hwaccel_output_format cuda" + seek +
                    L" -i " + quoted(source) + L" -map 0:v:0 -vf scale_cuda=-2:" + std::to_wstring(target) + L":format=yuv420p -frames:v " + std::to_wstring(count) +
                    L" -c:v h264_nvenc -preset p2 -rc vbr -cq " + cq + L" -b:v 0 -g 6 -bf 0 -an -f mpegts " + quoted(parts[index]),
                    progress_reader(carry, written, [&](double seconds, double) {
                        if (seconds < 0) return;
                        encoded[index] = seconds;
                        const double done = std::clamp((encoded[0] + encoded[1]) / duration, 0.0, 0.98);
                        change([&] { if (preview_level == level) preview["progress"] = done; });
                    }), [&] { return preview_halted(); });
            } catch (...) { results[index] = {}; }
        };
        std::thread other(encode, 1);
        encode(0);
        other.join();
        if (preview_halted()) { cleanup(); return true; }
        bool ready = true;
        for (size_t i = 0; i < 2; ++i) { std::error_code error; ready = ready && results[i].code == 0 && !results[i].canceled && fs::file_size(parts[i], error) > 0 && !error; }
        if (ready) {
            try {
                atomic_write(list, "ffconcat version 1.0\n" + concat_entry(parts[0]) + std::format("duration {:.6f}\n", static_cast<double>(split) / fps) + concat_entry(parts[1]));
                std::string carry;
                int64_t written = -1;
                const auto joined = run_tool(tool.exe, L"-hide_banner -nostdin -y -loglevel error -progress pipe:1 -nostats -f concat -safe 0 -i " + quoted(list) +
                    L" -map 0:v:0 -c copy -movflags +faststart -f mp4 " + quoted(partial), progress_reader(carry, written, [](double, double) {}), [&] { return preview_halted(); });
                if (preview_halted()) { cleanup(); return true; }
                // Every source frame must be in the joined preview, or it is rebuilt the usual way.
                ready = joined.code == 0 && written == frames;
            } catch (...) { ready = false; }
        }
        std::error_code error;
        if (ready) fs::rename(partial, file, error);
        if (!ready || error) { cleanup(); return false; }
        { std::error_code ignored; for (const auto& part : parts) fs::remove(part, ignored); fs::remove(list, ignored); }
        preview_built(level, file, "GPU (2 parts)");
        return true;
    }

    // URLs for the generated preview files, served only to this editor's page.
    std::string preview_urls() {
        Json result = {{"video", ""}, {"level", ""}, {"stems", Json::array()}};
        std::lock_guard lock(mutex);
        result["level"] = preview_level;
        if (const auto found = proxies.find(preview_level); found != proxies.end()) result["video"] = media->generated_url(found->second);
        for (const auto& [track, file] : stems) result["stems"].push_back({{"track", track}, {"url", media->generated_url(file)}});
        return result.dump();
    }

    void waveform_pass(const FFmpeg& tool) {
        auto wave = audio_peaks(tool, source, [&] { return stopping.load(); });
        if (stopping) return;
        change([&] { peaks = std::move(wave.values); wave_status = wave.complete && !peaks.empty() ? "ready" : "failed"; });
    }

    std::string waveform() { std::lock_guard lock(mutex); return wave_status == "ready" ? base64(peaks) : std::string{}; }

    // Name typed in the export dialog, already reduced to characters a Windows file name allows; empty keeps the default.
    std::wstring export_name;
    // Ticked in the export dialog: the source file is deleted once the export has been saved. A failed or canceled export keeps it.
    // Only a video export (mp4 or vertical) can delete the original.
    bool delete_original{};
    // What the running or last export makes (one of export_formats).
    std::string export_format = "mp4";
    fs::path output_for() const {
        auto folder = source.parent_path();
        if (_wcsicmp(folder.filename().c_str(), L"Edited Exports") != 0) folder /= L"Edited Exports";
        std::error_code created;
        fs::create_directories(folder, created);
        if (created) throw std::runtime_error("The Edited Exports folder couldn't be created next to the original clip. Check that folder's permissions.");
        const bool named = !export_name.empty();
        const auto stem = named ? export_name : source.stem().wstring();
        for (unsigned i = 1; i < 1000; ++i) {
            const auto label = format_label(export_format);
            const auto suffix = named ? (i == 1 ? std::wstring{} : L" (" + std::to_wstring(i) + L")") : (i == 1 ? L" (" + label + L")" : L" (" + label + L" " + std::to_wstring(i) + L")");
            auto candidate = folder / (stem + suffix + format_extension(export_format));
            std::error_code error;
            if (!fs::exists(candidate, error) && !fs::exists(fs::path(candidate.wstring() + L".part"), error)) return candidate;
        }
        throw std::runtime_error("Too many edited copies of this clip already exist in Edited Exports.");
    }

    std::expected<bool, std::string> start_export(const std::string& payload) {
        try {
            const auto request = Json::parse(payload);
            const auto format = request.value("format", std::string("mp4"));
            if (std::ranges::find(export_formats, format) == export_formats.end()) return std::unexpected("Choose an export type from the list.");
            const bool video = video_format(format);
            const int quality = request.at("quality").get<int>();
            if (quality < 1 || quality > 10) return std::unexpected("Choose a quality from 1 to 10.");
            const auto tool = host.ffmpeg_info();
            if (tool.status != "ready") return std::unexpected("FFmpeg is needed to export. Locate ffmpeg.exe first.");
            if (format == "mp3" && !tool.mp3) return std::unexpected("This FFmpeg build has no MP3 encoder. Export WAV instead, or install a full FFmpeg build.");
            Json info;
            { std::lock_guard lock(mutex); info = probe; if (export_state.value("status", "") == "running") return std::unexpected("An export is already running."); }
            if (info.value("status", "") != "ready") return std::unexpected("The clip is still being analyzed. Try again in a moment.");
            const double duration = info.value("duration", 0.0);
            std::vector<std::pair<double, double>> segments;
            double total = 0, frame_time = 0;
            if (format == "png") {
                // Save this frame: the frame under the playhead, whatever has been cut.
                frame_time = request.at("time").get<double>();
                if (!std::isfinite(frame_time) || frame_time < 0 || (duration > 0 && frame_time > duration + 0.5)) return std::unexpected("The playhead position couldn't be read. Move the playhead and try again.");
            } else {
                double previous = -1;
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
            }
            if (format == "gif" && total > gif_max_seconds + 0.05) return std::unexpected(std::format("A GIF can be up to {:.0f} seconds. Cut the video shorter first.", gif_max_seconds));
            // Ticked audio tracks: absent means every track as recorded.
            const int recorded = info.value("audio_tracks", 0);
            AudioPlan audio;
            for (int t = 0; t < recorded; ++t) audio.tracks.push_back(t);
            // The tracks you hear, at their volumes: what an audio export mixes into one stereo file.
            AudioPlan heard{preview_tracks(recorded), false, {}};
            if (request.contains("tracks") && !request["tracks"].is_null()) {
                std::vector<int> chosen;
                for (const auto& item : request.at("tracks")) {
                    const int t = item.get<int>();
                    if (t < 0 || t >= recorded) return std::unexpected("The chosen audio tracks couldn't be read. Open Audio tracks and tick them again.");
                    chosen.push_back(t);
                }
                std::ranges::sort(chosen);
                chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
                const auto listed = preview_tracks(recorded);
                // Volumes set in Audio tracks, keyed by track number; a track not named plays at full volume.
                std::vector<double> gains;
                bool turned_down = false;
                const auto volumes = request.value("volumes", Json::object());
                for (auto it = chosen.begin(); it != chosen.end();) {
                    const auto key = std::to_string(*it);
                    double level = 1.0;
                    if (volumes.is_object() && volumes.contains(key)) {
                        if (!volumes[key].is_number()) return std::unexpected("The track volumes couldn't be read. Open Audio tracks and set them again.");
                        level = std::clamp(volumes[key].get<double>(), 0.0, 1.0);
                    }
                    // A track turned all the way down is left out, like an unticked one.
                    if (level < 0.0005) { it = chosen.erase(it); turned_down = true; continue; }
                    if (std::abs(level - 1.0) >= 0.0005) turned_down = true;
                    gains.push_back(level);
                    ++it;
                }
                if (turned_down || !std::ranges::includes(chosen, listed)) audio = {chosen, chosen.size() >= 2, gains};
                heard = {chosen, chosen.size() >= 2, gains};
            }
            const bool sound_only = format == "mp3" || format == "wav";
            if (sound_only) {
                if (!recorded) return std::unexpected("This clip has no sound to export.");
                if (heard.tracks.empty()) return std::unexpected("No audio track is ticked. Open Audio tracks and tick the sound you want.");
            }
            if (format == "gif" || format == "png") audio = {};
            const int limit = video ? request.value("size_limit", 0) : 0;
            if (limit && std::ranges::find(size_limits, limit) == size_limits.end()) return std::unexpected("Choose a size from the list.");
            if (limit && !limit_plan(limit * 1e6, total, info.value("fps", 30.0), audio.streams()).possible)
                return std::unexpected(std::format("This video is too long to fit in {} MB. Cut it shorter or choose a bigger size.", limit));
            Framing framing;
            if (format == "vertical") {
                framing.vertical = true;
                const auto fit = request.value("framing", std::string("fill"));
                if (fit != "fill" && fit != "fit") return std::unexpected("Choose Fill or Fit for the vertical video.");
                framing.fit = fit == "fit";
                const auto crop = request.value("crop", 0.5);
                if (!std::isfinite(crop) || crop < 0 || crop > 1) return std::unexpected("The crop position couldn't be read. Move the crop box and try again.");
                framing.crop = crop;
            }
            const auto gif_size = request.value("gif_size", std::string("medium"));
            if (format == "gif" && gif_size != "small" && gif_size != "medium" && gif_size != "large") return std::unexpected("Choose a GIF size from the list.");
            const auto compression_value = request.value("gif_compression", Json(0));
            if (format == "gif" && (!compression_value.is_number_integer() || compression_value.get<int>() < 0 || compression_value.get<int>() > 4)) return std::unexpected("Choose a GIF compression from 0 to 4.");
            const int compression = compression_value.is_number_integer() ? std::clamp(compression_value.get<int>(), 0, 4) : 0;
            std::error_code missing;
            if (!fs::is_regular_file(source, missing)) return std::unexpected("The original replay was moved or deleted, so it can't be exported.");
            if (format == "mp4" && !limit) host.store_preference("quality", quality);
            if (exporter.joinable()) exporter.join();
            export_name = clean_file_stem(request.value("name", std::string{}), format_extension(format));
            delete_original = video && request.value("delete_original", false);
            export_format = format;
            cancel_export = false;
            const bool smart = format == "mp4" && smart_cut_possible(tool, info);
            // A size limit is for sharing, so its video is always H.264, which plays everywhere. It keeps the original
            // video only when that is H.264 already and the edit fits well under the limit (checked again after export).
            const bool original = limit ? smart && lower(info.value("codec", "")) == "h264" && original_bytes(info, total, audio) <= limit * 1e6 * 0.9 : quality == 10 && smart;
            const std::string label = format == "mp4" ? (original ? "Original video" : encoder_label(tool, info, quality, tool.nvenc, limit != 0))
                : format == "vertical" ? encoder_label(tool, info, quality, tool.nvenc, true)
                : format == "gif" ? "GIF" : format == "mp3" ? "MP3" : format == "wav" ? "WAV" : "PNG";
            change([&] { export_state = {{"status", "running"}, {"format", format}, {"progress", 0.0}, {"speed", 0.0}, {"quality", quality}, {"size_limit", limit}, {"attempt", 1}, {"total", total},
                                         {"encoder", label}}; });
            const auto& logged = sound_only ? heard : audio;
            std::string volumes_text;
            for (size_t i = 0; i < logged.gains.size(); ++i) volumes_text += std::format("{}{}:{:.0f}%", i ? "," : "", logged.tracks[i], logged.gains[i] * 100);
            const auto extra = framing.vertical ? std::format(" framing={} crop={:.3f}", framing.fit ? "fit" : "fill", framing.crop)
                : format == "gif" ? std::format(" gif_size={} gif_compression={}", gif_size, compression) : format == "png" ? std::format(" time={:.3f}", frame_time) : std::string{};
            log_info("Export started: format={} source={} sections={} seconds={:.2f} quality={} size_limit_mb={} audio_streams={}{}{} original_video={}{}", format, quoted_value(path_string(source)), segments.size(), total, quality, limit,
                     logged.streams(), logged.mix ? " (mix)" : "", volumes_text.empty() ? "" : " track_volumes=" + volumes_text, original, extra);
            const AudioPlan sound = sound_only ? heard : audio;
            exporter = std::thread([this, tool, info, segments, total, quality, limit, sound, original, format, framing, gif_size, compression, frame_time] {
                run_export(tool, info, segments, total, quality, limit, sound, original, format, framing, gif_size, compression, frame_time);
            });
            return true;
        } catch (const std::exception& e) { return std::unexpected(std::string("The export request could not be read. ") + e.what()); }
    }

    // Expected size of the quality 10 path (original video, AAC at 192 kbps per track), for a size limit.
    static double original_bytes(const Json& info, double seconds, const AudioPlan& audio) {
        return seconds * (reference_kbps(info) + plan_for(10).audio_kbps * audio.streams()) * 125 + container_bytes(seconds, info.value("fps", 30.0), audio.streams());
    }
    static std::string encoder_label(const FFmpeg& tool, const Json& info, int quality, bool hardware, bool h264_only = false) {
        if (!hardware) return "H.264 · CPU";
        return std::string(!h264_only && choose_encode(tool, info, quality, true).hevc ? "HEVC" : "H.264") + " · NVIDIA NVENC";
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
                               const AudioPlan& audio_plan, const fs::path& work, const fs::path& temporary) {
        const auto cancelled = [&] { return cancel_export.load() || stopping.load(); };
        const double fps = info.value("fps", 0.0);
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
        if (!audio_plan.tracks.empty()) {
            std::string graph, chain;
            for (size_t i = 0; i < sections.size(); ++i)
                for (const int t : audio_plan.tracks) {
                    graph += std::format("[1:a:{}]atrim=start={:.6f}:end={:.6f},asetpts=PTS-STARTPTS[a{}_{}];", t, sections[i].first / fps, sections[i].last / fps, i, t);
                    chain += std::format("[a{}_{}]", i, t);
                }
            graph += chain + std::format("concat=n={}:v=0:a={}", sections.size(), audio_plan.tracks.size());
            for (const int t : audio_plan.tracks) graph += std::format("[o{}]", t);
            const auto outputs = audio_outputs(audio_plan, graph);
            atomic_write(work / L"audio.txt", graph);
            audio_input = L" -i " + quoted(source) + filter_file(tool, work / L"audio.txt");
            for (const auto& label : outputs) audio += L" -map \"[" + wide(label) + L"]\"";
            audio += L" -c:a aac -b:a " + std::to_wstring(plan_for(10).audio_kbps) + L"k";
            audio += track_titles(info, audio_plan);
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

    // How one re-encode is set: bitrates in kbps and a height cap (0: none). Quality 1-9 take them from the quality
    // table; a size limit from its byte budget. The function gets whether the output is HEVC.
    struct EncodeRate { int video_kbps, maxrate_kbps, bufsize_kbps, audio_kbps, max_height; };
    using RateFor = std::function<EncodeRate(bool hevc)>;
    static RateFor quality_rate(const FFmpeg& tool, const Json& info, int quality) {
        return [tool, info, quality](bool hevc) {
            const int kbps = choose_encode(tool, info, quality, hevc).video_kbps;
            const auto plan = plan_for(quality);
            return EncodeRate{kbps, kbps * 3 / 2, kbps * 2, plan.audio_kbps, plan.max_height};
        };
    }
    // maxrate with a one-second buffer caps the whole video at maxrate x (length + 1 s) whatever the content,
    // so the first attempt nearly always fits. Calm scenes still come in under the cap.
    static RateFor limit_rate(const Json& info, double seconds, int video_kbps, int audio_kbps, bool vertical = false) {
        const int cap = std::max(1, static_cast<int>(video_kbps * seconds / (seconds + 1)));
        return [info, cap, audio_kbps, vertical](bool hevc) { return EncodeRate{cap, cap, cap, audio_kbps, vertical ? vertical_limit_height(info, cap) : limit_height(info, cap, hevc)}; };
    }
    // Export vertical at a quality: the clip's bitrate share for that quality, scaled by how many pixels the vertical
    // picture has against the clip, as H.264. max_height is the vertical picture's height.
    static RateFor vertical_quality_rate(const Json& info, int quality) {
        return [info, quality](bool) {
            const auto plan = plan_for(quality);
            const int height = vertical_base(info);
            const double source = std::max(1.0, static_cast<double>(info.value("width", 0)) * info.value("height", 0));
            const double pixels = std::min(2.0, vertical_width(height) * static_cast<double>(height) / source);
            const double factor = lower(info.value("codec", "")) == "h264" ? 1.0 : 1.35;
            const int kbps = std::max(1500, static_cast<int>(std::lround(reference_kbps(info) * plan.share * factor * pixels)));
            return EncodeRate{kbps, kbps * 3 / 2, kbps * 2, plan.audio_kbps, height};
        };
    }

    // Quality 1-9, 10 when the original can't be kept, and size limits: re-encode the kept sections into one video.
    ProcessResult encode_export(const FFmpeg& tool, const Json& info, const std::vector<std::pair<double, double>>& segments, double total, int quality,
                                const RateFor& rate_for, const AudioPlan& audio_plan, const fs::path& work, const fs::path& temporary, bool h264_only = false,
                                const Framing& framing = {}) {
        const int height = info.value("height", 0);
        std::vector<int> levels;  // 0: GPU decode + NVENC, 1: CPU decode + NVENC, 2: x264
        // A vertical picture is cropped and blurred on the CPU, so its frames are decoded there too.
        if (tool.nvenc) { if (!framing.vertical) levels.push_back(0); levels.push_back(1); }
        if (tool.x264) levels.push_back(2);
        ProcessResult result;
        for (const int level : levels) {
            std::error_code ignored; fs::remove(temporary, ignored);
            const bool gpu = level == 0, hardware = level < 2;
            const bool hevc = !h264_only && !framing.vertical && choose_encode(tool, info, quality, hardware).hevc;
            const auto plan = rate_for(hevc);
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
                for (const int t : audio_plan.tracks) {
                    filter += std::format("[{}:a:{}]{}asetpts=PTS-STARTPTS,aresample=async=1:first_pts=0[a{}_{}];", input, t, audio_trim, i, t);
                    chain += std::format("[a{}_{}]", i, t);
                }
            }
            filter += chain + std::format("concat=n={}:v=1:a={}[vc]", segments.size(), audio_plan.tracks.size());
            for (const int t : audio_plan.tracks) filter += std::format("[o{}]", t);
            const bool scale = !framing.vertical && plan.max_height && height > plan.max_height;
            if (framing.vertical) filter += ";" + vertical_filter(framing, plan.max_height);
            else filter += gpu ? (scale ? std::format(";[vc]scale_cuda=-2:{}:format=yuv420p[v]", plan.max_height) : std::string(";[vc]scale_cuda=format=yuv420p[v]"))
                               : std::format(";[vc]{}format=yuv420p[v]", scale ? std::format("scale=-2:{}:flags=lanczos,", plan.max_height) : std::string{});
            const auto outputs = audio_outputs(audio_plan, filter);
            atomic_write(work / L"video.txt", filter);
            std::wstring maps = L" -map \"[v]\"";
            for (const auto& label : outputs) maps += L" -map \"[" + wide(label) + L"]\"";
            maps += track_titles(info, audio_plan);
            const std::wstring bitrate = L" -b:v " + std::to_wstring(plan.video_kbps) + L"k -maxrate " + std::to_wstring(plan.maxrate_kbps) + L"k -bufsize " + std::to_wstring(plan.bufsize_kbps) + L"k";
            const std::wstring video = hardware
                ? std::wstring(hevc ? L" -c:v hevc_nvenc -profile:v main -tag:v hvc1" : L" -c:v h264_nvenc -profile:v high") + L" -preset p4 -tune hq -rc vbr -spatial-aq 1" + bitrate
                : L" -c:v libx264 -preset faster" + bitrate;
            const std::wstring audio = outputs.empty() ? L"" : L" -c:a aac -b:a " + std::to_wstring(plan.audio_kbps) + L"k";
            change([&] {
                export_state["encoder"] = encoder_label(tool, info, quality, hardware, h264_only || framing.vertical)
                    + (framing.vertical ? std::format(" · {}×{}", vertical_width(plan.max_height), plan.max_height) : scale ? std::format(" · {}p", plan.max_height) : std::string{});
                export_state["progress"] = 0.0; export_state.erase("eta");
            });
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

    // Size limit: encode, measure the real file, and while it is over the limit encode again with a video bitrate
    // lowered by the overshoot plus a margin. Never more than three encodes; a file that still doesn't fit is deleted.
    ProcessResult fit_export(const FFmpeg& tool, const Json& info, const std::vector<std::pair<double, double>>& segments, double total, int quality,
                             int limit, const AudioPlan& audio, bool original, const fs::path& work, const fs::path& temporary, int& attempts, const Framing& framing = {}) {
        const double limit_bytes = limit * 1e6;
        const auto size_of = [&] { std::error_code error; const auto bytes = fs::file_size(temporary, error); return error ? 0.0 : static_cast<double>(bytes); };
        const auto over = [&](double bytes) { std::error_code ignored; fs::remove(temporary, ignored); log_info("Export over its size limit: attempt={} bytes={:.0f} limit={:.0f}", attempts, bytes, limit_bytes); };
        ProcessResult result;
        if (original) {
            attempts = 1;
            result = smart_export(tool, info, segments, total, audio, work, temporary);
            if (result.canceled) return result;
            if (result.code == 0) {
                const double bytes = size_of();
                if (bytes > 0 && bytes <= limit_bytes) {
                    change([&] { export_state["note"] = std::format("It already fit under {} MB, so the original video was kept.", limit); });
                    return result;
                }
                over(bytes);
            }
        }
        auto plan = limit_plan(limit_bytes, total, info.value("fps", 30.0), audio.streams());
        // A big limit on a small clip must not blow the file up to the limit: the video never gets more than the
        // source needs as H.264 (its own bitrate, or 1.35 times it when the source is HEVC or AV1).
        const double enough = framing.vertical ? vertical_quality_rate(info, 10)(false).video_kbps : reference_kbps(info) * (lower(info.value("codec", "")) == "h264" ? 1.0 : 1.35);
        double video_kbps = std::min<double>(plan.video_kbps, std::max<double>(enough, limit_min_video_kbps));
        for (int pass = 0; pass < 3; ++pass) {
            ++attempts;
            change([&] { export_state["attempt"] = attempts; export_state["progress"] = 0.0; });
            result = encode_export(tool, info, segments, total, quality, limit_rate(info, total, static_cast<int>(video_kbps), plan.audio_kbps, framing.vertical), audio, work, temporary, true, framing);
            if (result.canceled || result.code != 0 || !result.started) return result;
            const double bytes = size_of();
            if (bytes > 0 && bytes <= limit_bytes) return result;
            over(bytes);
            // Take the overshoot (against 96% of the limit) off the video, with 15% more for safety.
            video_kbps -= (bytes - limit_bytes * 0.96) * 8 / 1000 / total * 1.15;
            if (video_kbps < 150) break;
        }
        std::error_code ignored; fs::remove(temporary, ignored);
        result.code = 1;
        result.errors = std::format("This video couldn't be made smaller than {} MB. Cut it shorter or choose a bigger size.", limit);
        result.fit_failed = true;
        return result;
    }

    // GIF: two passes over the kept sections. The first builds a 256-colour palette for this video, the second draws
    // the video with it. A GIF has no sound.
    ProcessResult gif_export(const FFmpeg& tool, const Json& info, const std::vector<std::pair<double, double>>& segments, double total,
                             const std::string& size, int compression, const fs::path& work, const fs::path& temporary) {
        const auto preset = gif_preset(size);
        const auto squeeze = gif_compression(compression);
        const int source_height = info.value("height", 0);
        const int height = source_height > 0 ? std::min(preset.height, source_height) : preset.height;
        // Separate seeking inputs decode only the kept sections, as encode_export does.
        const bool seeking = segments.size() <= 16;
        std::wstring inputs = seeking ? L"" : L" -i " + quoted(source);
        std::string picture, joins;
        for (size_t i = 0; i < segments.size(); ++i) {
            const auto& [first, last] = segments[i];
            const auto input = seeking ? std::to_string(i) : std::string("0");
            if (seeking) inputs += L" -ss " + number(first) + L" -t " + number(last - first) + L" -i " + quoted(source);
            const auto trim = seeking ? std::string{} : std::format("trim=start={:.6f}:end={:.6f},", first, last);
            picture += std::format("[{}:v:0]{}setpts=PTS-STARTPTS[v{}];", input, trim, i);
            joins += std::format("[v{}]", i);
        }
        picture += joins + std::format("concat=n={}:v=1:a=0,fps={},scale=-2:{}:flags=lanczos,setsar=1", segments.size(), preset.fps, height);
        const auto palette = work / L"palette.png";
        const size_t palette_input = seeking ? segments.size() : 1;
        const auto cancelled = [&] { return cancel_export.load() || stopping.load(); };
        ProcessResult result;
        for (int pass = 0; pass < 2; ++pass) {
            const auto graph = pass == 0 ? picture + std::format(",palettegen=max_colors={}:stats_mode=diff[p]", squeeze.colors)
                                         : picture + std::format("[x];[x][{}:v]paletteuse=dither={}:diff_mode=rectangle[p]", palette_input, squeeze.dither);
            const auto script = work / (pass == 0 ? L"palette.txt" : L"gif.txt");
            atomic_write(script, graph);
            const std::wstring output = pass == 0 ? L" -map \"[p]\" -frames:v 1 -update 1 -f image2 " + quoted(palette)
                                                  : L" -map \"[p]\" -loop 0 -f gif " + quoted(temporary);
            std::string carry;
            int64_t written = -1;
            const auto started = Clock::now();
            result = run_tool(tool.exe, L"-hide_banner -nostdin -y -loglevel error -progress pipe:1 -nostats" + inputs + (pass == 1 ? L" -i " + quoted(palette) : std::wstring{}) +
                filter_file(tool, script) + output, progress_reader(carry, written, [&](double seconds, double speed) {
                    if (seconds < 0) { if (speed >= 0) change([&] { export_state["speed"] = speed; }); return; }
                    if (pass == 0) return;  // the palette is written at the end, so the first pass has no running position
                    const double part = std::clamp(seconds / std::max(total, 0.1), 0.0, 1.0);
                    const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
                    change([&] { export_state["progress"] = 0.3 + 0.7 * part; export_state["eta"] = part > 0.02 ? elapsed * (1 - part) / part : -1.0; });
                }), cancelled);
            if (result.canceled || result.code != 0 || !result.started) return result;
            if (pass == 0) {
                std::error_code error;
                if (fs::file_size(palette, error) == 0 || error) { result.code = 1; result.errors = "FFmpeg couldn't build the GIF's colours."; return result; }
                change([&] { export_state["progress"] = 0.3; });
            }
        }
        return result;
    }

    // MP3 or WAV: the ticked tracks at their volumes, cut to the kept sections and mixed into one stereo file.
    ProcessResult audio_export(const FFmpeg& tool, const std::vector<std::pair<double, double>>& segments, double total, const AudioPlan& heard,
                               bool wav, const fs::path& work, const fs::path& temporary) {
        std::string graph, chain, mix;
        for (size_t i = 0; i < segments.size(); ++i)
            for (const int t : heard.tracks) {
                graph += std::format("[0:a:{}]atrim=start={:.6f}:end={:.6f},asetpts=PTS-STARTPTS[a{}_{}];", t, segments[i].first, segments[i].second, i, t);
                chain += std::format("[a{}_{}]", i, t);
            }
        graph += chain + std::format("concat=n={}:v=0:a={}", segments.size(), heard.tracks.size());
        for (const int t : heard.tracks) graph += std::format("[o{}]", t);
        for (size_t i = 0; i < heard.tracks.size(); ++i) {
            graph += std::format(";[o{0}]volume={1:.4f}[g{0}]", heard.tracks[i], heard.gain(i));
            mix += std::format("[g{}]", heard.tracks[i]);
        }
        // normalize=0 adds the tracks at their own levels, as OBS builds its Mix and as the video export does.
        graph += heard.tracks.size() >= 2 ? std::format(";{}amix=inputs={}:duration=longest:normalize=0,", mix, heard.tracks.size()) : ";" + mix;
        graph += "aresample=48000,aformat=channel_layouts=stereo[out]";
        atomic_write(work / L"audio.txt", graph);
        const std::wstring codec = wav ? L" -c:a pcm_s16le -f wav " : L" -c:a libmp3lame -b:a 192k -id3v2_version 3 -f mp3 ";
        std::string carry;
        int64_t written = -1;
        return run_tool(tool.exe, L"-hide_banner -nostdin -y -loglevel error -progress pipe:1 -nostats -i " + quoted(source) + filter_file(tool, work / L"audio.txt") +
            L" -map \"[out]\" -map_metadata -1" + codec + quoted(temporary), progress_reader(carry, written, [&](double seconds, double) {
                if (seconds >= 0 && total > 0) change([&] { export_state["progress"] = std::clamp(seconds / total, 0.0, 1.0); });
            }), [&] { return cancel_export.load() || stopping.load(); });
    }

    // Save this frame: the one frame under the playhead, as a full-size PNG. The seek lands a quarter frame before
    // that frame, so FFmpeg's first frame at or after it is the frame the editor shows.
    // At the end of the clip the playhead can sit past the last frame's start (the container often runs a little
    // longer than the video), so the frame number is held to the last frame, and a seek that still finds nothing
    // takes the last frame from the end instead.
    ProcessResult frame_export(const FFmpeg& tool, const Json& info, double time, const fs::path& temporary) {
        int64_t frames = 0;
        { std::lock_guard lock(mutex); frames = frame_count; }
        return extract_frame(tool, source, time, info.value("fps", 0.0), info.value("duration", 0.0), frames, temporary, [&] { return cancel_export.load() || stopping.load(); });
    }

    void run_export(const FFmpeg& tool, const Json& info, const std::vector<std::pair<double, double>>& segments, double total, int quality, int limit,
                    const AudioPlan& audio, bool original, const std::string& format, const Framing& framing, const std::string& gif_size, int compression, double frame_time) {
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        fs::path temporary, work;
        const bool video = video_format(format);
        try {
            auto final = output_for();
            temporary = fs::path(final.wstring() + L".part");
            std::error_code made;
            work = preview_root() / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(id) + L"-export-" + wide(random_secret().substr(0, 8)));
            fs::create_directories(work, made);
            if (made) throw std::runtime_error("A temporary folder for the export could not be created.");
            ProcessResult result;
            int attempts = 0;  // encodes a size limit needed
            if (format == "gif") result = gif_export(tool, info, segments, total, gif_size, compression, work, temporary);
            else if (format == "mp3" || format == "wav") result = audio_export(tool, segments, total, audio, format == "wav", work, temporary);
            else if (format == "png") result = frame_export(tool, info, frame_time, temporary);
            else if (limit) result = fit_export(tool, info, segments, total, quality, limit, audio, original, work, temporary, attempts, framing);
            else if (framing.vertical) result = encode_export(tool, info, segments, total, quality, vertical_quality_rate(info, quality), audio, work, temporary, true, framing);
            else {
                bool finished = false;
                if (original) {
                    result = smart_export(tool, info, segments, total, audio, work, temporary);
                    finished = result.canceled || result.code == 0;
                    if (!finished) {
                        // Some files can't be cut this way; re-encode at full quality instead of failing.
                        change([&] { export_state["note"] = "The original video couldn't be kept for this clip, so it was re-encoded at full quality."; });
                    }
                }
                if (!finished) result = encode_export(tool, info, segments, total, quality, quality_rate(tool, info, quality), audio, work, temporary);
            }
            // FFmpeg can finish without writing anything, for example a frame past the end of the clip.
            if (!result.canceled && result.started && result.code == 0) {
                std::error_code error;
                if (fs::file_size(temporary, error) == 0 || error) { result.code = 1; result.errors = "FFmpeg finished without writing the file."; }
            }
            if (result.fit_failed) {
                throw std::runtime_error(result.errors);
            } else if (result.canceled) {
                std::error_code ignored; fs::remove(temporary, ignored);
                log_info("Export canceled: source={}", quoted_value(path_string(source)));
                change([&] { export_state = {{"status", "canceled"}}; });
            } else if (!result.started) {
                throw std::runtime_error(result.errors.empty() ? "FFmpeg could not be started. Locate ffmpeg.exe again." : result.errors);
            } else if (result.code != 0) {
                std::error_code ignored; fs::remove(temporary, ignored);
                const auto detail = last_line(result.errors);
                throw std::runtime_error(std::string(format == "png" ? "FFmpeg could not save this frame." : format == "mp3" || format == "wav" ? "FFmpeg could not export the sound." : format == "gif" ? "FFmpeg could not make the GIF." : "FFmpeg could not export this video.") + (detail.empty() ? std::string{} : " " + detail));
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
                // Only videos go into the clip list; a GIF, sound file or frame is offered from the editor and the folder.
                if (video) {
                    const auto added = host.controller.add_clip(path_string(final), clip.value("app", ""));
                    if (!added) history_note = added.error();
                }
                { std::lock_guard lock(mutex); if (export_state.contains("note")) history_note = export_state["note"].get<std::string>() + (history_note.empty() ? "" : " " + history_note); }
                // The finished video is offered in the editor (Play video, Show in folder); File Explorer is not opened unasked.
                const bool revealed = false;
                log_info("Export finished: format={} file={} bytes={} seconds={:.2f} size_limit_mb={} attempts={}{}", format, quoted_value(path_string(final)), error ? 0 : bytes, total, limit, std::max(1, attempts), history_note.empty() ? "" : " note=" + quoted_value(history_note));
                bool original_deleted = false;
                if (delete_original && video) {
                    const auto removed = host.controller.remove_clip(path_string(source));
                    original_deleted = removed.has_value();
                    if (!removed) { history_note += (history_note.empty() ? "" : " ") + removed.error(); log_warn("Original kept after export: file={} error={}", quoted_value(path_string(source)), quoted_value(removed.error())); }
                }
                change([&] {
                    export_state = {{"status", "done"}, {"format", format}, {"output", path_string(final)}, {"name", path_string(final.filename())}, {"bytes", error ? 0 : bytes},
                                    {"duration", total}, {"revealed", revealed}, {"note", history_note}, {"quality", quality}, {"original_deleted", original_deleted},
                                    {"size_limit", limit}, {"attempts", std::max(1, attempts)}, {"audio_streams", format == "mp3" || format == "wav" ? 1 : audio.streams()}};
                });
                // With the original gone the editor has nothing left to edit: the main window opens the export on Clips
                // and plays it, and this editor closes.
                if (original_deleted) {
                    log_info("Editor closing: original deleted after export, playing file={}", quoted_value(path_string(final)));
                    host.app->post([impl = &host, id = id, output = path_string(final), token = std::weak_ptr<bool>(host.alive)] {
                        if (!token.lock()) return;
                        if (impl->play) impl->play(output);
                        for (auto& session : impl->sessions) if (session->id == id) { session->allow_close = true; session->window->close(); }
                    });
                }
            }
        } catch (const std::exception& e) {
            if (!temporary.empty()) { std::error_code ignored; fs::remove(temporary, ignored); }
            log_error("Export failed: source={} error={}", quoted_value(path_string(source)), quoted_value(e.what()));
            change([&] { export_state = {{"status", "failed"}, {"error", e.what()}}; });
        }
        if (!work.empty()) { std::error_code ignored; fs::remove_all(work, ignored); }
        if (SUCCEEDED(com)) CoUninitialize();
    }

    std::string exported_path() { std::lock_guard lock(mutex); return export_state.value("status", "") == "done" ? export_state.value("output", "") : std::string{}; }
    std::string exported_format() { std::lock_guard lock(mutex); return export_state.value("format", std::string("mp4")); }
};

EditorHost::Impl::~Impl() {
    alive.reset();
    closing = true;
    if (installer.joinable()) installer.join();
    ++wave_run;
    for (auto& [run, worker] : wave_workers) if (worker.joinable()) worker.join();
    for (auto& [run, worker] : frame_workers) if (worker.joinable()) worker.join();
    sessions.clear();
    for (auto& detector : detectors) if (detector.joinable()) detector.join();
}

void EditorHost::Impl::remove(uint64_t id) {
    sessions.remove_if([&](const std::unique_ptr<Session>& session) { return session->id == id; });
}

std::string EditorHost::ffmpeg_state(bool refresh) {
    bool never;
    { std::lock_guard lock(impl_->ffmpeg_mutex); never = impl_->detection == 0; }
    if (never || refresh) impl_->detect_if_needed();
    const auto tool = impl_->ffmpeg_info();
    return Json{{"status", tool.status}, {"version", tool.version}, {"install", impl_->install_state()}}.dump();
}

std::expected<bool, std::string> EditorHost::install_ffmpeg() { return impl_->install_ffmpeg(); }

std::expected<bool, std::string> EditorHost::pick_ffmpeg(void* owner) {
    try {
        const auto chosen = pick_ffmpeg_file(static_cast<HWND>(owner), impl_->ffmpeg_info().exe);
        if (chosen.empty()) return false;
        impl_->detect(from_string(chosen));
        return true;
    } catch (const std::exception& e) { return std::unexpected(e.what()); }
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

void EditorHost::on_play(std::function<void(const std::string&)> handler) { impl_->play = std::move(handler); }

void EditorHost::save_frame(const std::string& path, double time, std::function<void(std::string)> callback) {
    auto& host = *impl_;
    // Shared, so a worker that can't be started still answers the page.
    const auto done = std::make_shared<std::function<void(std::string)>>(std::move(callback));
    const auto fail = [&](const std::string& error) { (*done)(Json{{"status", "failed"}, {"error", error}}.dump()); };
    try {
        if (!host.controller.knows_clip(path)) return fail("This clip is no longer in your list.");
        if (!std::isfinite(time) || time < 0) return fail("The playhead position couldn't be read.");
        const auto source = from_string(path);
        auto extension = source.extension().wstring();
        for (auto& c : extension) c = static_cast<wchar_t>(towlower(c));
        if (extension == L".png" || extension == L".jpg" || extension == L".jpeg") return fail("This is already a picture.");
        // The game the clip belongs to, so the screenshot sorts with it on the Screenshots tab.
        std::string app;
        try { for (const auto& clip : Json::parse(host.controller.snapshot()).at("clips")) if (clip.value("path", "") == path) { app = clip.value("app", ""); break; } } catch (...) {}
        host.detect_if_needed();
        const uint64_t run = ++host.frame_run;
        host.frame_workers.emplace(run, std::thread([&host, run, source, path, time, app, token = std::weak_ptr<bool>(host.alive), done]() mutable {
            Json result = {{"status", "failed"}, {"error", "The frame couldn't be saved."}};
            fs::path temporary;
            try {
                for (int i = 0; i < 2000 && host.detecting() && !host.closing.load(); ++i) Sleep(10);
                const auto tool = host.ffmpeg_info();
                if (tool.status != "ready") throw std::runtime_error("FFmpeg is needed to save a frame. Open a clip in the editor to install it.");
                const auto cancelled = [&] { return host.closing.load(); };
                const auto probe = probe_from_output(run_tool(tool.exe, L"-hide_banner -nostdin -i " + quoted(source), {}, cancelled, 20000).errors);
                if (probe.value("status", "") != "ready") throw std::runtime_error("FFmpeg couldn't read this clip's video.");
                // Next to the clip; an export's frame goes beside its Edited Exports folder, where screenshots are listed.
                auto folder = source.parent_path();
                if (_wcsicmp(folder.filename().c_str(), L"Edited Exports") == 0) folder = folder.parent_path();
                const auto seconds = std::max(0.0, time);
                const auto minutes = static_cast<int>(seconds / 60);
                const auto stamp = wide(std::format("{}-{:05.2f}", minutes, seconds - minutes * 60));
                fs::path output;
                for (unsigned i = 1; i < 1000 && output.empty(); ++i) {
                    auto candidate = folder / (source.stem().wstring() + L" frame " + stamp + (i == 1 ? std::wstring{} : L" (" + std::to_wstring(i) + L")") + L".png");
                    std::error_code error;
                    if (!fs::exists(candidate, error) && !fs::exists(fs::path(candidate.wstring() + L".part"), error)) output = candidate;
                }
                if (output.empty()) throw std::runtime_error("Too many frames of this clip have that name.");
                temporary = fs::path(output.wstring() + L".part");
                const auto made = extract_frame(tool, source, time, probe.value("fps", 0.0), probe.value("duration", 0.0), 0, temporary, cancelled);
                std::error_code error;
                if (made.canceled) throw std::runtime_error("OBSReplay is closing.");
                if (made.code != 0 || fs::file_size(temporary, error) == 0 || error) {
                    const auto detail = last_line(made.errors);
                    throw std::runtime_error("FFmpeg couldn't save this frame." + (detail.empty() ? std::string{} : " " + detail));
                }
                if (!MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Windows couldn't save the frame next to the clip.");
                temporary.clear();
                const auto added = host.controller.add_screenshot(path_string(output), app);
                const auto bytes = fs::file_size(output, error);
                log_info("Frame saved from viewer: source={} time={:.3f} file={} bytes={}", quoted_value(path), time, quoted_value(path_string(output)), error ? 0 : bytes);
                result = {{"status", "saved"}, {"path", path_string(output)}, {"name", path_string(output.filename())}, {"bytes", error ? 0 : bytes}};
                if (!added) result["note"] = added.error();
            } catch (const std::exception& e) {
                result = {{"status", "failed"}, {"error", e.what()}};
                log_warn("Frame not saved from viewer: source={} error={}", quoted_value(path), quoted_value(e.what()));
            }
            if (!temporary.empty()) { std::error_code ignored; fs::remove(temporary, ignored); }
            // The promise is settled on the UI thread, which also joins this worker.
            host.app->post([&host, run, token, text = result.dump(), done]() mutable {
                if (!token.lock()) return;
                if (const auto worker = host.frame_workers.find(run); worker != host.frame_workers.end()) { worker->second.join(); host.frame_workers.erase(worker); }
                (*done)(std::move(text));
            });
        }));
    } catch (const std::exception& e) { fail(e.what()); }
}
void EditorHost::waveform(const std::string& path, std::function<void(std::string)> done) {
    try { impl_->waveform(path, done); } catch (...) { done(R"({"status":"unavailable"})"); }
}

std::expected<bool, std::string> EditorHost::open(const std::string& path) {
    auto& host = *impl_;
    try {
        log_info("Editor requested: file={}", quoted_value(path));
        if (!host.controller.knows_clip(path)) return std::unexpected("This replay is no longer in your recent clips.");
        {
            auto extension = from_string(path).extension().wstring();
            for (auto& c : extension) c = static_cast<wchar_t>(towlower(c));
            if (extension == L".png" || extension == L".jpg" || extension == L".jpeg") return std::unexpected("A screenshot can't be opened in the clip editor.");
        }
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
        host.detect_if_needed();
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
        // Chrome: title bar 38, transport 55, toolbar 55, timeline 166, export bar 95 (borders included). The titlebar
        // Scale menu zooms the page, so the chrome grows with it while the video area is counted in window pixels.
        const double zoom = [&] { const auto saved = host.preferences().value("scale", 1.0); return saved == 0.75 || saved == 1.25 || saved == 1.5 ? saved : 1.0; }();
        const int chrome_height = static_cast<int>(409 * zoom + 0.5);
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
        ignore_alt_menu(hwnd);
        const HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        const auto ground = theme_ground(saved_theme());
        window.set_background({ground.r, ground.g, ground.b, 255});
        auto view = saucer::smartview::create({.window = self->window, .storage_path = isolated_webview_folder()});
        if (!view) return std::unexpected("The editor's WebView2 surface could not start.");
        self->view = std::make_unique<saucer::smartview>(std::move(*view));
        auto& web = *self->view;
        web.set_force_dark(!ground.light);
        web.set_background({ground.r, ground.g, ground.b, 255});
        web.set_context_menu(false);
        // The page reads its theme from localStorage before its first paint. Write the saved theme there first,
        // so an editor always opens in the app's theme whatever the page last stored.
        web.inject({.code = "try{localStorage.setItem('obsreplay.theme'," + Json(saved_theme()).dump() + ")}catch(e){}", .run_at = saucer::script::time::creation});
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
            if (self->allow_close || !self->page_alive()) {
                // Forget a fullscreen placement, so a later window that gets the same handle starts clean.
                set_window_fullscreen(self->window->native().hwnd, false);
                return saucer::policy::allow;
            }
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
        // Fullscreen is handled here, as in the main window: saucer 8.0.5 swaps the monitor's x and y, which moves a
        // window on a monitor away from 0,0 off every screen.
        web.on<saucer::webview::event::fullscreen>([self](bool enabled) { set_window_fullscreen(self->window->native().hwnd, enabled); return saucer::policy::block; });
        web.expose("theme_changed", [self](std::string id) {
            const auto next = theme_ground(id);
            self->window->set_background({next.r, next.g, next.b, 255});
            self->view->set_background({next.r, next.g, next.b, 255});
            self->view->set_force_dark(!next.light);
        });
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
        web.expose("install_ffmpeg", [impl] { return impl->install_ffmpeg(); });
        web.expose("reveal_output", [self](bool reveal) -> std::expected<bool, std::string> {
            const auto output = self->exported_path();
            if (output.empty()) return std::unexpected("Export a video first.");
            try { open_path(from_string(output), reveal); return true; } catch (const std::exception& e) { return std::unexpected(e.what()); }
        });
        // Copy: puts the exported file on the clipboard as a file, so it pastes into Discord, a chat or a folder.
        web.expose("copy_output", [self]() -> std::expected<bool, std::string> {
            const auto output = self->exported_path();
            if (output.empty()) return std::unexpected("Export a video first.");
            try { copy_files_to_clipboard({from_string(output)}); return true; } catch (const std::exception& e) { return std::unexpected(e.what()); }
        });
        web.expose("play_output", [self, impl, token = std::weak_ptr<bool>(impl->alive)]() -> std::expected<bool, std::string> {
            const auto output = self->exported_path();
            if (output.empty()) return std::unexpected("Export a video first.");
            if (!video_format(self->exported_format())) {
                log_info("Open exported file: file={}", quoted_value(output));
                try { open_path(from_string(output), false); return true; } catch (const std::exception& e) { return std::unexpected(e.what()); }
            }
            log_info("Play exported video: file={}", quoted_value(output));
            impl->app->post([impl, output, token] { if (token.lock() && impl->play) impl->play(output); });
            return true;
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
                if (saved && key == "preview_quality") self->request_preview(parsed.get<std::string>());
                if (saved) { std::lock_guard lock(self->mutex); ++self->revision; }
                return saved;
            } catch (const std::exception&) { return std::unexpected("That preference couldn't be read."); }
        });
        web.expose("retry_analysis", [self] { self->start_analysis(); });
        // Lines from the editor page for the session log: messages it showed, script errors and video errors.
        web.expose("ui_log", [](std::string level, std::string text) {
            if (text.size() > 2000) text.resize(2000);
            log_write(level == "error" ? LogLevel::error : level == "warn" ? LogLevel::warn : level == "debug" ? LogLevel::debug : LogLevel::info, "Editor: " + text);
        });
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
