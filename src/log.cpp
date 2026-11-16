#include "log.hpp"
#include "build_info.hpp"

#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <vector>

namespace replay {
namespace {
constexpr std::size_t kept_lines = 600;   // held in memory for the Debug tab
constexpr std::size_t kept_sessions = 10; // log files kept on disk, this run included

std::mutex log_mutex;
HANDLE log_file = INVALID_HANDLE_VALUE;
fs::path log_directory;
std::deque<std::string> recent;
std::vector<std::string> early; // lines logged before the file was open

std::string stamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
}

void append(const std::string& line) {
    if (log_file == INVALID_HANDLE_VALUE) { early.push_back(line); return; }
    const std::string text = line + "\r\n";
    DWORD written{};
    WriteFile(log_file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

std::string registry_text(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t value[512]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return {};
    auto text = utf8(value);
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

std::string windows_version() {
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    using Query = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    if (const auto ntdll = GetModuleHandleW(L"ntdll.dll"))
        if (const auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))) query(&info);
    auto name = registry_text(HKEY_LOCAL_MACHINE, key, L"ProductName");
    // The registry still says "Windows 10" on Windows 11.
    if (info.dwBuildNumber >= 22000 && name.starts_with("Windows 10")) name.replace(0, 10, "Windows 11");
    return std::format("{} {} (build {}.{}.{})", name.empty() ? "Windows" : name, registry_text(HKEY_LOCAL_MACHINE, key, L"DisplayVersion"), info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
}

std::string graphics_adapters() {
    std::string text;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return "unknown";
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            text += std::format("{}{} ({} MB)", text.empty() ? "" : "; ", utf8(description.Description), description.DedicatedVideoMemory / (1024 * 1024));
        adapter.Reset();
    }
    return text.empty() ? "none found" : text;
}
}

std::string app_version() { return std::string(OBSREPLAY_VERSION "+" OBSREPLAY_COMMIT_SHORT) + (OBSREPLAY_DIRTY ? ".dirty" : ""); }

Json build_info() {
    return {{"version", OBSREPLAY_VERSION}, {"commit", OBSREPLAY_COMMIT}, {"commit_short", OBSREPLAY_COMMIT_SHORT}, {"dirty", OBSREPLAY_DIRTY != 0}, {"built", __DATE__ " " __TIME__}, {"full", app_version()}};
}

std::string quoted_value(std::string_view text) {
    std::string result = "\"";
    for (const char c : text) {
        if (c == '"') result += "\\\"";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') continue;
        else result += c;
    }
    return result + '"';
}

void log_write(LogLevel level, std::string_view message) {
    static constexpr const char* names[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
    std::string line = stamp() + " " + names[static_cast<int>(level)] + " ";
    for (const char c : message) { if (c == '\n') line += " | "; else if (c != '\r') line += c; }
    std::lock_guard lock(log_mutex);
    recent.push_back(line);
    while (recent.size() > kept_lines) recent.pop_front();
    append(line);
}

void log_start(const Paths& paths) {
    {
        std::lock_guard lock(log_mutex);
        if (log_file != INVALID_HANDLE_VALUE) return;
        std::error_code error;
        log_directory = paths.data / L"logs";
        fs::create_directories(log_directory, error);
        // Older sessions beyond the newest few are removed; names sort by start time.
        std::vector<fs::path> sessions;
        for (const auto& entry : fs::directory_iterator(log_directory, error))
            if (entry.path().filename().wstring().starts_with(L"session-") && entry.path().extension() == L".log") sessions.push_back(entry.path());
        std::sort(sessions.begin(), sessions.end());
        for (std::size_t i = 0; i + (kept_sessions - 1) < sessions.size(); ++i) fs::remove(sessions[i], error);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const auto name = std::format(L"session-{:04}{:02}{:02}-{:02}{:02}{:02}-{}.log", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
        log_file = CreateFileW((log_directory / name).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        const auto held = std::move(early);
        early.clear();
        if (log_file == INVALID_HANDLE_VALUE) return;
        for (const auto& line : held) append(line);
    }
    TIME_ZONE_INFORMATION zone{};
    const DWORD daylight = GetTimeZoneInformation(&zone);
    const long offset = -(zone.Bias + (daylight == TIME_ZONE_ID_DAYLIGHT ? zone.DaylightBias : zone.StandardBias));
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    wchar_t isolated[8]{};
    const bool test_data = GetEnvironmentVariableW(L"OBSREPLAY_DATA_DIR", isolated, 8) != 0 || GetLastError() == ERROR_INSUFFICIENT_BUFFER;
    log_info("OBSReplay {} started: commit={} built={} pid={}", app_version(), OBSREPLAY_COMMIT, quoted_value(__DATE__ " " __TIME__), GetCurrentProcessId());
    log_info("Clock: local time, UTC{}{:02}:{:02}", offset < 0 ? '-' : '+', std::abs(offset) / 60, std::abs(offset) % 60);
    log_info("Executable: path={} data={} isolated={} headless={}", quoted_value(path_string(paths.exe)), quoted_value(path_string(paths.data)), test_data, headless_mode());
    log_info("System: os={} cpu={} threads={} memory_mb={}", quoted_value(windows_version()),
             quoted_value(registry_text(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString")),
             GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), memory.ullTotalPhys / (1024 * 1024));
    log_info("Graphics: adapters={}", quoted_value(graphics_adapters()));
    try {
        std::string displays;
        for (const auto& monitor : monitors()) displays += (displays.empty() ? "" : "; ") + monitor.value("label", std::string{});
        log_info("Displays: {}", quoted_value(displays));
    } catch (...) {}
}

std::string log_tail(std::size_t lines) {
    std::lock_guard lock(log_mutex);
    std::string text;
    for (std::size_t i = recent.size() > lines ? recent.size() - lines : 0; i < recent.size(); ++i) text += recent[i] + "\n";
    return text;
}

std::string log_text() {
    fs::path folder;
    { std::lock_guard lock(log_mutex); folder = log_directory; }
    if (folder.empty()) return log_tail(kept_lines);
    // The newest file of this process: its name ends with this process id.
    const auto suffix = std::format(L"-{}.log", GetCurrentProcessId());
    std::error_code error;
    fs::path newest;
    for (const auto& entry : fs::directory_iterator(folder, error))
        if (entry.path().filename().wstring().ends_with(suffix) && entry.path().filename() > newest.filename()) newest = entry.path();
    try { if (!newest.empty()) return read_text(newest); } catch (...) {}
    return log_tail(kept_lines);
}

fs::path log_folder() { std::lock_guard lock(log_mutex); return log_directory; }
}
