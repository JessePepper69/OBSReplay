#include "platform.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <mmsystem.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace replay {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
};
std::runtime_error failure(const std::string& what) {
    return std::runtime_error(what + " (Windows error " + std::to_string(GetLastError()) + ").");
}
fs::path folder(REFKNOWNFOLDERID id) {
    PWSTR result = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &result)))
        throw std::runtime_error("Windows could not locate the user folders.");
    fs::path value(result);
    CoTaskMemFree(result);
    return value;
}
std::wstring quote(const fs::path& path) {
    auto value = path.wstring();
    if (value.find(L'"') != std::wstring::npos) throw std::runtime_error("Invalid executable path.");
    return L"\"" + value + L"\"";
}
bool hotkey_shape(const std::string& text) {
    if (text.empty() || text.size() > 40) return false;
    std::istringstream input(text);
    std::string token;
    bool modifier = false, key = false;
    std::set<std::string> seen;
    while (std::getline(input, token, '+')) {
        if (!seen.insert(token).second) return false;
        if (token == "Ctrl" || token == "Alt" || token == "Shift") {
            if (key) return false;
            modifier = true;
        } else {
            if (key) return false;
            bool function = false;
            if (token.size() >= 2 && token[0] == 'F') {
                try { const auto n = std::stoi(token.substr(1)); function = n >= 1 && n <= 24 && token == "F" + std::to_string(n); }
                catch (...) {}
            }
            key = function || (token.size() == 1 && ((token[0] >= 'A' && token[0] <= 'Z') || (token[0] >= '0' && token[0] <= '9')));
            if (!key) return false;
        }
    }
    return modifier && key;
}
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw failure("Invalid Unicode text");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) throw failure("Invalid UTF-8 text");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
std::string path_string(const fs::path& value) { return utf8(value.wstring()); }
fs::path from_string(const std::string& value) { return fs::path(wide(value)); }

Paths paths() {
    std::wstring exe(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!length || length >= exe.size()) throw failure("Cannot locate OBSReplay.exe");
    exe.resize(length);
    Paths result{fs::path(exe), folder(FOLDERID_LocalAppData) / L"OBSReplay", folder(FOLDERID_RoamingAppData) / L"obs-studio"};
    wchar_t custom[32768]{};
    DWORD count = GetEnvironmentVariableW(L"OBSREPLAY_DATA_DIR", custom, 32768);
    if (count && count < 32768) {
        fs::path requested(custom);
        if (!requested.is_absolute()) throw std::runtime_error("OBSREPLAY_DATA_DIR must be an absolute path.");
        result.data = requested;
    }
    fs::create_directories(result.data);
    return result;
}
bool headless_mode() {
    static const bool enabled = GetEnvironmentVariableW(L"OBSREPLAY_HEADLESS", nullptr, 0) > 0;
    return enabled;
}
void show_headless(void* hwnd) {
    SetWindowPos(static_cast<HWND>(hwnd), HWND_BOTTOM, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
std::optional<fs::path> isolated_webview_folder() {
    wchar_t custom[32768]{};
    if (!GetEnvironmentVariableW(L"OBSREPLAY_DATA_DIR", custom, 32768)) return std::nullopt;
    return paths().data / L"WebView2";
}

std::string read_text(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read " + path_string(path) + ".");
    stream.seekg(0, std::ios::end);
    auto size = stream.tellg();
    if (size < 0 || size > 8 * 1024 * 1024) throw std::runtime_error("Configuration file is too large: " + path_string(path));
    stream.seekg(0);
    std::string result(static_cast<size_t>(size), '\0');
    if (!result.empty() && !stream.read(result.data(), size)) throw std::runtime_error("Cannot finish reading " + path_string(path));
    if (result.starts_with("\xEF\xBB\xBF")) result.erase(0, 3);
    return result;
}

void atomic_write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + wide(random_secret().substr(0, 12));
    try {
        Handle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (output.value == INVALID_HANDLE_VALUE) throw failure("Cannot create " + path_string(path));
        DWORD written = 0;
        if (text.size() > MAXDWORD || !WriteFile(output.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) || written != text.size())
            throw failure("Cannot write " + path_string(path));
        if (!FlushFileBuffers(output.value)) throw failure("Cannot flush " + path_string(path));
        CloseHandle(output.value);
        output.value = nullptr;
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw failure("Cannot replace " + path_string(path));
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

fs::path find_obs() {
    std::vector<fs::path> candidates;
    wchar_t value[32768]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\OBS Studio", nullptr, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value, &bytes) == ERROR_SUCCESS)
        candidates.emplace_back(fs::path(value) / L"bin\\64bit\\obs64.exe");
    candidates.emplace_back(folder(FOLDERID_ProgramFiles) / L"obs-studio\\bin\\64bit\\obs64.exe");
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (fs::is_regular_file(candidate, error)) return candidate;
    }
    return {};
}

std::optional<std::array<int,3>> file_version(const fs::path& file) {
    DWORD unused = 0;
    const DWORD length = GetFileVersionInfoSizeW(file.c_str(), &unused);
    if (!length) return std::nullopt;
    std::vector<unsigned char> buffer(length);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT size = 0;
    if (!GetFileVersionInfoW(file.c_str(), 0, length, buffer.data()) ||
        !VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &size) ||
        size < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xfeef04bd) return std::nullopt;
    return std::array<int,3>{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS)};
}

fs::path winget_path() {
    try {
        const auto alias = folder(FOLDERID_LocalAppData) / L"Microsoft/WindowsApps/winget.exe";
        return GetFileAttributesW(alias.c_str()) != INVALID_FILE_ATTRIBUTES ? alias.lexically_normal() : fs::path{};
    } catch (...) { return {}; }
}

Json default_settings(const Paths&) {
    return Json{{"obs_path", path_string(find_obs())}, {"clips_dir", path_string(folder(FOLDERID_Videos) / L"OBSReplay")},
        {"clip_seconds",120},{"capture","auto"},{"monitor_id",""},{"output_height",1080},{"fps",60},
        {"encoder","auto"},{"codec","h264"},{"quality",20},{"mic_enabled",false},{"mic_device","default"},
        {"desktop_device","default"},{"noise","off"},{"separate_tracks",true},{"sort_by_app",true},
        {"save_sound",true},{"start_at_login",true},{"save_hotkey","Alt+Z"},{"show_hotkey","Alt+X"},{"replay_enabled",false},{"animations_enabled",true}};
}

void validate_settings(const Json& settings) {
    if (!settings.is_object()) throw std::runtime_error("Settings must be an object.");
    const std::array string_keys{"obs_path","clips_dir","capture","monitor_id","encoder","codec","mic_device","desktop_device","noise","save_hotkey","show_hotkey"};
    for (const auto key : string_keys) {
        if (!settings.contains(key) || !settings[key].is_string()) throw std::runtime_error(std::string("Invalid setting: ") + key);
        auto text = settings[key].get<std::string>();
        if (text.size() > 32700 || std::any_of(text.begin(), text.end(), [](unsigned char c){return c < 32;}))
            throw std::runtime_error(std::string("Invalid characters in ") + key + ".");
        wide(text);
    }
    for (const auto key : {"mic_enabled","separate_tracks","sort_by_app","save_sound","start_at_login","replay_enabled"})
        if (!settings.contains(key) || !settings[key].is_boolean()) throw std::runtime_error(std::string("Invalid setting: ") + key);
    if (settings.contains("animations_enabled") && !settings["animations_enabled"].is_boolean())
        throw std::runtime_error("Invalid setting: animations_enabled");
    for (const auto key : {"clip_seconds","output_height","fps","quality"})
        if (!settings.contains(key) || !settings[key].is_number_integer()) throw std::runtime_error(std::string("Invalid setting: ") + key);
    const int duration=settings["clip_seconds"].get<int>(), height=settings["output_height"].get<int>(), fps=settings["fps"].get<int>(), quality=settings["quality"].get<int>();
    if (duration < 30 || duration > 600) throw std::runtime_error("Replay length must be between 30 and 600 seconds.");
    if (height!=0 && height!=720 && height!=1080 && height!=1440) throw std::runtime_error("Choose an available output resolution.");
    if (fps!=30 && fps!=60 && fps!=120 && fps!=144) throw std::runtime_error("Choose 30, 60, 120 or 144 FPS.");
    if (quality!=16 && quality!=18 && quality!=20 && quality!=24 && quality!=28) throw std::runtime_error("Choose an available quality preset.");
    const auto member=[&](const char* key, std::initializer_list<const char*> values) {
        const auto value=settings[key].get<std::string>();
        if (std::none_of(values.begin(),values.end(),[&](const char* allowed){return value==allowed;})) throw std::runtime_error(std::string("Unsupported ")+key+".");
    };
    member("capture",{"auto","game","desktop"}); member("encoder",{"auto","nvidia","amd","intel","cpu"});
    member("codec",{"h264","hevc","av1"}); member("noise",{"off","rnnoise","speex"});
    if (settings["encoder"]=="cpu" && settings["codec"]!="h264") throw std::runtime_error("CPU encoding supports H.264. Choose H.264 or a supported hardware encoder.");
    for (const auto key : {"save_hotkey","show_hotkey"})
        if (!hotkey_shape(settings[key].get<std::string>())) throw std::runtime_error("Shortcuts need Ctrl, Alt or Shift plus a letter, number or function key.");
    if (settings["save_hotkey"]==settings["show_hotkey"]) throw std::runtime_error("Save and show shortcuts must be different.");
    auto destination=from_string(settings["clips_dir"].get<std::string>());
    if (destination.empty() || !destination.is_absolute()) throw std::runtime_error("Choose an absolute clips folder path.");
    const auto obs=from_string(settings["obs_path"].get<std::string>());
    if (!obs.empty() && (!obs.is_absolute() || _wcsicmp(obs.filename().c_str(),L"obs64.exe")!=0))
        throw std::runtime_error("Choose the installed obs64.exe executable.");
}

Json load_settings(const Paths& p) {
    const auto target=p.data/L"settings.json";
    if (!fs::exists(target)) return default_settings(p);
    try {
        auto settings=default_settings(p);
        auto stored=Json::parse(read_text(target));
        if (!stored.is_object()) throw std::runtime_error("Invalid settings format");
        settings.update(stored);
        validate_settings(settings);
        return settings;
    } catch (...) {
        throw std::runtime_error("Saved settings are unreadable. The original settings.json is preserved in " + path_string(p.data) + ". Choose valid settings and Apply to recover.");
    }
}
void save_settings(const Paths& p,const Json& settings) {
    validate_settings(settings);
    const auto target=p.data/L"settings.json";
    if (fs::exists(target)) {
        try { auto previous=Json::parse(read_text(target)); validate_settings(previous); atomic_write(p.data/L"settings.backup.json",previous.dump(2)); }
        catch (const Json::exception&) {}
        catch (const std::runtime_error& e) {
            if (std::string(e.what()).starts_with("Cannot")) throw;
        }
    }
    atomic_write(target,settings.dump(2));
}

bool obs_running() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    if (snapshot.value==INVALID_HANDLE_VALUE) throw failure("Cannot check whether OBS is running");
    PROCESSENTRY32W entry{}; entry.dwSize=sizeof(entry);
    if (!Process32FirstW(snapshot.value,&entry)) return false;
    do { if (_wcsicmp(entry.szExeFile,L"obs64.exe")==0) return true; } while(Process32NextW(snapshot.value,&entry));
    return false;
}
void launch_obs(const fs::path& exe) {
    if (obs_running()) throw std::runtime_error("OBS is already open. Connect to the OBSReplay profile or close OBS before starting it here.");
    if (!fs::is_regular_file(exe)) throw std::runtime_error("OBS was not found. Browse for obs64.exe in Settings.");
    // The dedicated session runs in the tray; an update prompt would appear over games, so it checks for updates only when the user opens OBS normally.
    auto command=quote(exe)+L" --profile \"OBSReplay\" --collection \"OBSReplay\" --minimize-to-tray --disable-updater";
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
    auto directory=exe.parent_path().wstring();
    if (!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,directory.c_str(),&startup,&process)) throw failure("Could not launch OBS");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}

void close_obs(const fs::path& exe) {
    auto expected=exe.lexically_normal(); expected.make_preferred();
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    if(snapshot.value==INVALID_HANDLE_VALUE) throw failure("Cannot locate OBS");
    PROCESSENTRY32W entry{}; entry.dwSize=sizeof(entry);
    DWORD target{};
    if(Process32FirstW(snapshot.value,&entry)) do {
        if(_wcsicmp(entry.szExeFile,L"obs64.exe")!=0) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID));
        wchar_t name[32768]{}; DWORD size=32768;
        if(!process.value || !QueryFullProcessImageNameW(process.value,0,name,&size)) continue;
        if(_wcsicmp(name,expected.c_str())!=0) continue;
        if(target) throw std::runtime_error("More than one OBS process is running. Close the extra session before applying capture settings.");
        target=entry.th32ProcessID;
    } while(Process32NextW(snapshot.value,&entry));
    if(!target) throw std::runtime_error("The configured OBS process could not be found. Your settings were not changed.");
    Handle process(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,target));
    if(!process.value) throw failure("Cannot wait for OBS to close");
    struct Request { DWORD process; bool sent{}; } request{target};
    EnumWindows([](HWND window,LPARAM param)->BOOL {
        auto& request=*reinterpret_cast<Request*>(param);
        DWORD id{};GetWindowThreadProcessId(window,&id);
        if(id!=request.process) return TRUE;
        wchar_t title[256]{};GetWindowTextW(window,title,256);
        if(!std::wstring_view(title).starts_with(L"OBS ")) return TRUE;
        request.sent=PostMessageW(window,WM_CLOSE,0,0)!=FALSE;
        return !request.sent;
    },reinterpret_cast<LPARAM>(&request));
    if(!request.sent) throw std::runtime_error("OBS's main window is not ready to close. Your settings were not changed.");
    if(WaitForSingleObject(process.value,20000)!=WAIT_OBJECT_0)
        throw std::runtime_error("OBS has not finished closing. Check for an OBS dialog, then try Apply again. No process was forced to stop.");
}

unsigned close_all_obs(unsigned timeout_ms) {
    std::vector<DWORD> ids;
    {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
        if(snapshot.value==INVALID_HANDLE_VALUE) throw failure("Cannot locate OBS");
        PROCESSENTRY32W entry{}; entry.dwSize=sizeof(entry);
        if(Process32FirstW(snapshot.value,&entry)) do { if(_wcsicmp(entry.szExeFile,L"obs64.exe")==0) ids.push_back(entry.th32ProcessID); } while(Process32NextW(snapshot.value,&entry));
    }
    std::vector<HANDLE> waits;
    struct Guard { std::vector<HANDLE>& list; ~Guard() { for(auto h:list) CloseHandle(h); } } guard{waits};
    for(const auto id:ids) {
        HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,id);
        if(!process) continue;
        waits.push_back(process);
        struct Request { DWORD process; bool sent{}; } request{id};
        EnumWindows([](HWND window,LPARAM param)->BOOL {
            auto& request=*reinterpret_cast<Request*>(param);
            DWORD owner{}; GetWindowThreadProcessId(window,&owner);
            if(owner!=request.process) return TRUE;
            wchar_t title[256]{}; GetWindowTextW(window,title,256);
            if(!std::wstring_view(title).starts_with(L"OBS ")) return TRUE;
            request.sent=PostMessageW(window,WM_CLOSE,0,0)!=FALSE;
            return !request.sent;
        },reinterpret_cast<LPARAM>(&request));
        if(!request.sent && WaitForSingleObject(process,0)!=WAIT_OBJECT_0)
            throw std::runtime_error("OBS is still starting, so it can't be closed yet. Wait a moment, then try again.");
    }
    if(waits.empty()) return 0;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms);
    for(auto handle:waits) {
        const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        if(WaitForSingleObject(handle,static_cast<DWORD>(std::max<long long>(0,left)))!=WAIT_OBJECT_0)
            throw std::runtime_error("OBS didn't close. It may be asking for confirmation because it's recording or streaming. Answer in OBS, then try again. Nothing was forced to stop.");
    }
    return static_cast<unsigned>(waits.size());
}

Json monitors() {
    Json result=Json::array();
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM param)->BOOL {
        auto& output=*reinterpret_cast<Json*>(param);
        MONITORINFOEXW info{}; info.cbSize=sizeof(info);
        if (!GetMonitorInfoW(monitor,&info)) return TRUE;
        DISPLAY_DEVICEW device{}; device.cb=sizeof(device);
        EnumDisplayDevicesW(info.szDevice,0,&device,EDD_GET_DEVICE_INTERFACE_NAME);
        const int width=info.rcMonitor.right-info.rcMonitor.left,height=info.rcMonitor.bottom-info.rcMonitor.top;
        const auto label=utf8(device.DeviceString[0]?device.DeviceString:info.szDevice)+" ("+std::to_string(width)+" × "+std::to_string(height)+")"+((info.dwFlags&MONITORINFOF_PRIMARY)?" · Primary":"");
        Json row{{"id",utf8(device.DeviceID[0]?device.DeviceID:info.szDevice)},{"label",label},{"width",width},{"height",height},{"primary",(info.dwFlags&MONITORINFOF_PRIMARY)!=0}};
        if(info.dwFlags&MONITORINFOF_PRIMARY) output.insert(output.begin(),row); else output.push_back(row);
        return TRUE;
    },reinterpret_cast<LPARAM>(&result));
    return result;
}

Json audio_devices(bool input) {
    Json result=Json::array({Json{{"id","default"},{"label","System default"}}});
    if(!input) result.push_back(Json{{"id","off"},{"label","Off"}});
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator=nullptr; IMMDeviceCollection* collection=nullptr;
    if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)))) {
        if(SUCCEEDED(enumerator->EnumAudioEndpoints(input?eCapture:eRender,DEVICE_STATE_ACTIVE,&collection))) {
            UINT count=0; collection->GetCount(&count);
            for(UINT index=0;index<count;++index) {
                IMMDevice* device=nullptr;
                if(FAILED(collection->Item(index,&device))) continue;
                LPWSTR id=nullptr; IPropertyStore* properties=nullptr;
                if(SUCCEEDED(device->GetId(&id))&&SUCCEEDED(device->OpenPropertyStore(STGM_READ,&properties))) {
                    PROPVARIANT value; PropVariantInit(&value);
                    if(SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,&value))&&value.vt==VT_LPWSTR)
                        result.push_back(Json{{"id",utf8(id)},{"label",utf8(value.pwszVal)}});
                    PropVariantClear(&value);
                }
                if(properties) properties->Release();
                if(id) CoTaskMemFree(id);
                device->Release();
            }
            collection->Release();
        }
        enumerator->Release();
    }
    if(SUCCEEDED(initialized)) CoUninitialize();
    return result;
}

int run_process(const fs::path& exe,const std::wstring& arguments,unsigned timeout_ms,const std::function<void(const std::string&)>& output,const std::function<bool()>& cancelled,const fs::path& directory) {
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE}; HANDLE read_raw=nullptr,write_raw=nullptr;
    if(!CreatePipe(&read_raw,&write_raw,&security,0)) throw failure("Cannot create process output pipe");
    Handle reader(read_raw),writer(write_raw);
    if(!SetHandleInformation(reader.value,HANDLE_FLAG_INHERIT,0)) throw failure("Cannot protect process pipe");
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    startup.wShowWindow=SW_HIDE; startup.hStdOutput=writer.value; startup.hStdError=writer.value;
    Handle null_input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr));
    startup.hStdInput=null_input.value;
    PROCESS_INFORMATION process{}; auto command=quote(exe)+(arguments.empty()?L"":L" "+arguments);
    const auto working=directory.wstring();
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,working.empty()?nullptr:working.c_str(),&startup,&process)) return -1;
    Handle child(process.hProcess),thread(process.hThread);
    CloseHandle(writer.value); writer.value=nullptr;
    const auto start=std::chrono::steady_clock::now(); bool ended=false;
    for(;;) {
        DWORD available=0;
        if(PeekNamedPipe(reader.value,nullptr,0,nullptr,&available,nullptr)&&available) {
            std::array<char,8192> buffer{}; DWORD read=0;
            if(ReadFile(reader.value,buffer.data(),std::min<DWORD>(available,static_cast<DWORD>(buffer.size())),&read,nullptr)&&read&&output) output(std::string(buffer.data(),read));
            continue;
        }
        if(ended) break;
        if(cancelled&&cancelled()) return -2;
        if(WaitForSingleObject(child.value,10)==WAIT_OBJECT_0) ended=true;
        if(std::chrono::steady_clock::now()-start>std::chrono::milliseconds(timeout_ms)) {
            TerminateProcess(child.value,1); WaitForSingleObject(child.value,1000); return -1;
        }
    }
    DWORD code=1; GetExitCodeProcess(child.value,&code);
    return static_cast<int>(code);
}

std::string run_capture(const fs::path& exe,const std::wstring& arguments,unsigned timeout_ms) {
    if(!fs::is_regular_file(exe)) return {};
    std::string output; bool overflow=false;
    const int code=run_process(exe,arguments,timeout_ms,[&](const std::string& chunk) { if(output.size()+chunk.size()>1024*1024) overflow=true; else output+=chunk; },{},exe.parent_path());
    return code==0&&!overflow?output:std::string{};
}

std::string safe_name(const std::string& text) {
    auto value=wide(text);
    for(auto& c:value) if(c<32 || std::wstring_view(L"<>:\"/\\|?*").find(c)!=std::wstring_view::npos) c=L'_';
    while(!value.empty()&&(value.back()==L'.'||value.back()==L' ')) value.pop_back();
    while(!value.empty()&&(value.front()==L'.'||value.front()==L' ')) value.erase(value.begin());
    if(value.size()>80) { value.resize(80); if(!value.empty()&&value.back()>=0xD800&&value.back()<=0xDBFF) value.pop_back(); }
    if(value.empty()) return "Desktop";
    auto base=value.substr(0,value.find(L'.'));
    std::transform(base.begin(),base.end(),base.begin(),[](wchar_t c){return static_cast<wchar_t>(towupper(c));});
    if(base==L"CON"||base==L"PRN"||base==L"AUX"||base==L"NUL"||base==L"CONIN$"||base==L"CONOUT$"||
       (base.size()==4&&(base.starts_with(L"COM")||base.starts_with(L"LPT"))&&base[3]>=L'0'&&base[3]<=L'9')) value=L"_"+value;
    return utf8(value);
}
std::string foreground_app() {
    HWND window=GetForegroundWindow(); DWORD id=0; if(!window) return "Desktop";
    GetWindowThreadProcessId(window,&id);
    if(!id || id==GetCurrentProcessId()) return "Desktop";
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,id));
    if(!process.value) return "Desktop";
    std::wstring name(32768,L'\0'); DWORD length=static_cast<DWORD>(name.size());
    if(!QueryFullProcessImageNameW(process.value,0,name.data(),&length)) return "Desktop";
    name.resize(length); const auto stem=fs::path(name).stem().wstring();
    if(_wcsicmp(stem.c_str(),L"explorer")==0 || _wcsicmp(stem.c_str(),L"obs64")==0) return "Desktop";
    return safe_name(utf8(stem));
}

void open_path(const fs::path& path,bool reveal) {
    if(!fs::exists(path)) throw std::runtime_error("This file or folder is no longer available: "+path_string(path));
    if(reveal) {
        auto shell_path=path;
        shell_path.make_preferred();
        PIDLIST_ABSOLUTE item=ILCreateFromPathW(shell_path.c_str());
        if(!item) throw std::runtime_error("Cannot locate the saved file.");
        HRESULT result=SHOpenFolderAndSelectItems(item,0,nullptr,0); CoTaskMemFree(item);
        if(FAILED(result)) throw std::runtime_error("Windows could not show this file in its folder.");
    } else if(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)
        throw failure("Windows could not open "+path_string(path));
}
void open_help(const std::string& topic) {
    if(topic=="notices") {
        HRSRC resource=FindResourceW(nullptr,MAKEINTRESOURCEW(101),RT_RCDATA);
        HGLOBAL loaded=resource?LoadResource(nullptr,resource):nullptr;
        const auto data=loaded?static_cast<const char*>(LockResource(loaded)):nullptr;
        const DWORD length=resource?SizeofResource(nullptr,resource):0;
        if(!data||!length) throw std::runtime_error("The embedded software notices are unavailable.");
        const auto target=paths().data/L"THIRD-PARTY-NOTICES.txt";
        atomic_write(target,std::string(data,length));
        open_path(target);
        return;
    }
    const wchar_t* url=topic=="webview2"?L"https://developer.microsoft.com/microsoft-edge/webview2/":
        topic=="obs"?L"https://obsproject.com/download":topic=="ffmpeg"?L"https://ffmpeg.org/download.html":topic=="capture"?L"https://obsproject.com/kb/game-capture-guide":nullptr;
    if(!url) throw std::runtime_error("Unknown help topic.");
    if(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",url,nullptr,nullptr,SW_SHOWNORMAL))<=32) throw failure("Could not open the help page");
}
void set_login_startup(const Paths& p,bool enabled) {
    HKEY key=nullptr;
    auto result=RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if(result!=ERROR_SUCCESS) throw std::runtime_error("Windows could not update login startup.");
    if(enabled) {
        auto command=quote(p.exe)+L" --tray";
        result=RegSetValueExW(key,L"OBSReplay",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)));
    } else { result=RegDeleteValueW(key,L"OBSReplay"); if(result==ERROR_FILE_NOT_FOUND) result=ERROR_SUCCESS; }
    RegCloseKey(key);
    if(result!=ERROR_SUCCESS) throw std::runtime_error("Windows could not save the login startup option.");
}

std::string random_secret() {
    std::array<unsigned char,32> bytes{};
    if(BCryptGenRandom(nullptr,bytes.data(),static_cast<ULONG>(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
        throw std::runtime_error("Windows could not generate a secure connection secret.");
    static constexpr char hex[]="0123456789abcdef";
    std::string result; result.reserve(64);
    for(auto value:bytes) { result+=hex[value>>4]; result+=hex[value&15]; }
    return result;
}
std::string sha256_base64(const std::string& input) {
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    std::array<unsigned char,32> digest{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("SHA-256 is unavailable.");
    NTSTATUS status=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0);
    if(status>=0) status=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),static_cast<ULONG>(input.size()),0);
    if(status>=0) status=BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0);
    if(hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm,0);
    if(status<0) throw std::runtime_error("Connection authentication failed to compute its challenge.");
    DWORD length=0;
    if(!CryptBinaryToStringA(digest.data(),static_cast<DWORD>(digest.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&length)) throw failure("Cannot encode authentication challenge");
    std::string result(length,'\0');
    CryptBinaryToStringA(digest.data(),static_cast<DWORD>(digest.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,result.data(),&length);
    if(!result.empty()&&result.back()=='\0') result.pop_back();
    return result;
}
std::string now_iso() {
    SYSTEMTIME value{}; GetLocalTime(&value); char result[32]{};
    snprintf(result,sizeof(result),"%04d-%02d-%02dT%02d:%02d:%02d",value.wYear,value.wMonth,value.wDay,value.wHour,value.wMinute,value.wSecond);
    return result;
}
void save_feedback() { PlaySoundW(MAKEINTRESOURCEW(102),GetModuleHandleW(nullptr),SND_RESOURCE|SND_ASYNC|SND_NODEFAULT); }
}
