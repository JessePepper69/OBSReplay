#include "installer.hpp"
#include "integration.hpp"
#include "platform.hpp"

#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace replay {
namespace {
using Microsoft::WRL::ComPtr;

constexpr wchar_t uninstall_root[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
constexpr wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr UINT progress_message = WM_APP + 1;  // wParam: percent, lParam: index into `stages`
constexpr UINT finished_message = WM_APP + 2;  // wParam: 1 on success
constexpr UINT_PTR frame_timer = 1;
constexpr const wchar_t* stages[] = {L"Getting ready…", L"Closing OBSReplay…", L"Copying OBSReplay…", L"Adding the Start menu shortcut…", L"Registering with Windows…", L"Opening OBSReplay…"};

// Where an install lives. A check (OBSREPLAY_INSTALL_TEST=<empty folder>) installs into that folder under its own
// registry key, shows no window, opens nothing and leaves the user's real install, settings and running app alone.
struct Target {
    fs::path dir, exe, shortcut;
    std::wstring key;
    bool test = false;
};

std::wstring environment(const wchar_t* name) {
    std::wstring value(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    value.resize(length < value.size() ? length : 0);
    return value;
}
fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) throw std::runtime_error("Windows could not find a required folder.");
    fs::path result(raw);
    CoTaskMemFree(raw);
    return result;
}
Target target() {
    Target t;
    if (const auto root = environment(L"OBSREPLAY_INSTALL_TEST"); !root.empty()) {
        if (!fs::path(root).is_absolute()) throw std::runtime_error("OBSREPLAY_INSTALL_TEST must be an absolute path.");
        t.dir = fs::path(root) / L"app";
        t.shortcut = fs::path(root) / L"OBSReplay.lnk";
        t.key = L"OBSReplay.Test";
        t.test = true;
    } else {
        t.dir = known_folder(FOLDERID_LocalAppData) / L"Programs" / L"OBSReplay";
        t.shortcut = known_folder(FOLDERID_Programs) / L"OBSReplay.lnk";
        t.key = L"OBSReplay";
    }
    t.exe = t.dir / L"OBSReplay.exe";
    return t;
}
fs::path self_path() {
    std::wstring exe(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!length || length >= exe.size()) throw std::runtime_error("Cannot locate OBSReplay.exe.");
    exe.resize(length);
    return fs::path(exe);
}
bool same_file(const fs::path& a, const fs::path& b) {
    std::error_code error;
    return fs::equivalent(a, b, error) && !error;
}
bool same_content(const fs::path& a, const fs::path& b) {
    std::error_code error;
    const auto size = fs::file_size(a, error);
    if (error || size != fs::file_size(b, error) || error) return false;
    try { return read_text(a) == read_text(b); } catch (...) { return false; }
}
std::wstring quoted(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }

void set_value(HKEY key, const wchar_t* name, const std::wstring& value) {
    if (RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS)
        throw std::runtime_error("Windows could not save the install details.");
}
void set_value(HKEY key, const wchar_t* name, DWORD value) {
    if (RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) != ERROR_SUCCESS)
        throw std::runtime_error("Windows could not save the install details.");
}
bool registered(const Target& t) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (uninstall_root + t.key).c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    std::error_code error;
    return fs::exists(t.shortcut, error);
}
// The entry Windows shows under Settings > Apps > Installed apps.
void register_app(const Target& t) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, (uninstall_root + t.key).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Windows could not register OBSReplay.");
    try {
        const auto version = file_version(t.exe).value_or(std::array<int, 3>{0, 0, 0});
        std::error_code error;
        const auto bytes = fs::file_size(t.exe, error);
        set_value(key, L"DisplayName", t.test ? L"OBSReplay (install check)" : L"OBSReplay");
        set_value(key, L"DisplayVersion", std::to_wstring(version[0]) + L"." + std::to_wstring(version[1]) + L"." + std::to_wstring(version[2]));
        set_value(key, L"Publisher", L"OBSReplay");
        set_value(key, L"DisplayIcon", t.exe.wstring() + L",0");
        set_value(key, L"InstallLocation", t.dir.wstring());
        set_value(key, L"UninstallString", quoted(t.exe) + L" --uninstall");
        set_value(key, L"QuietUninstallString", quoted(t.exe) + L" --uninstall --quiet");
        set_value(key, L"EstimatedSize", static_cast<DWORD>(error ? 0 : bytes / 1024));
        set_value(key, L"NoModify", DWORD{1});
        set_value(key, L"NoRepair", DWORD{1});
    } catch (...) { RegCloseKey(key); throw; }
    RegCloseKey(key);
}
void create_shortcut(const Target& t) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) throw std::runtime_error("Windows could not create the Start menu shortcut.");
    link->SetPath(t.exe.c_str());
    link->SetWorkingDirectory(t.dir.c_str());
    link->SetDescription(L"Instant replay for OBS Studio");
    link->SetIconLocation(t.exe.c_str(), 0);
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file)) || FAILED(file->Save(t.shortcut.c_str(), TRUE))) throw std::runtime_error("Windows could not save the Start menu shortcut.");
}
// "Start with Windows" stores the executable's path. Point an existing entry at the installed copy.
void repoint_login_startup(const Target& t) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, run_key, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return;
    bool present = RegQueryValueExW(key, L"OBSReplay", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    auto p = paths();
    // Settings kept from an earlier install that had "Start with Windows" on: an uninstall removed the entry.
    std::error_code error;
    if (!present && fs::exists(p.data / L"settings.json", error)) { try { present = load_settings(p).value("start_at_login", false); } catch (...) {} }
    if (!present) return;
    p.exe = t.exe;
    set_login_startup(p, true);
}

// Asks a running OBSReplay to quit and waits. A copy that does not answer (an older build) is stopped.
// OBS keeps running either way, so the replay buffer survives an update.
BOOL CALLBACK collect_app_windows(HWND hwnd, LPARAM data) {
    if (reinterpret_cast<std::uintptr_t>(GetPropW(hwnd, app_window_property)) == 1) reinterpret_cast<std::vector<HWND>*>(data)->push_back(hwnd);
    return TRUE;
}
bool app_running() {
    std::vector<HWND> windows;
    EnumWindows(collect_app_windows, reinterpret_cast<LPARAM>(&windows));
    return !windows.empty();
}
void close_running_app() {
    std::vector<HWND> windows;
    EnumWindows(collect_app_windows, reinterpret_cast<LPARAM>(&windows));
    const UINT quit = RegisterWindowMessageW(quit_message_name);
    for (const HWND hwnd : windows) {
        DWORD process_id = 0;
        GetWindowThreadProcessId(hwnd, &process_id);
        if (!process_id || process_id == GetCurrentProcessId()) continue;
        const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, process_id);
        PostMessageW(hwnd, quit, 0, 0);
        if (!process) { Sleep(1500); continue; }
        if (WaitForSingleObject(process, 6000) != WAIT_OBJECT_0) { TerminateProcess(process, 0); WaitForSingleObject(process, 3000); }
        CloseHandle(process);
    }
}
// The old executable can stay locked for a moment after its process ends.
void copy_executable(const fs::path& from, const Target& t) {
    fs::create_directories(t.dir);
    const auto staged = t.dir / L"OBSReplay.exe.new";
    if (!CopyFileW(from.c_str(), staged.c_str(), FALSE)) throw std::runtime_error("OBSReplay could not be copied to " + path_string(t.dir) + ".");
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (MoveFileExW(staged.c_str(), t.exe.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
        Sleep(150);
    }
    DeleteFileW(staged.c_str());
    throw std::runtime_error("The installed OBSReplay.exe is in use. Close OBSReplay, then run this file again.");
}
bool launch(const fs::path& exe, const std::wstring& arguments = {}) {
    auto command = quoted(exe) + (arguments.empty() ? L"" : L" " + arguments);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

// The install window: the app icon, its name, one line that says what is happening, and a progress bar.
struct InstallWindow {
    HWND hwnd = nullptr;
    HICON icon = nullptr;
    HFONT title_font = nullptr, text_font = nullptr;
    int dpi = 96;
    int stage = 0;
    double target = 0.02, shown = 0;
    bool finished = false, succeeded = false;
    std::chrono::steady_clock::time_point opened = std::chrono::steady_clock::now();
    std::string error;
    int scale(int value) const { return MulDiv(value, dpi, 96); }
};
void paint(InstallWindow& w) {
    PAINTSTRUCT area;
    const HDC screen = BeginPaint(w.hwnd, &area);
    RECT client;
    GetClientRect(w.hwnd, &client);
    const HDC dc = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
    const HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    const HBRUSH ground = CreateSolidBrush(RGB(0x09, 0x0d, 0x13));
    FillRect(dc, &client, ground);
    DeleteObject(ground);
    SetBkMode(dc, TRANSPARENT);
    const int margin = w.scale(24), icon_size = w.scale(36);
    if (w.icon) DrawIconEx(dc, margin, margin, w.icon, icon_size, icon_size, 0, nullptr, DI_NORMAL);
    const int text_left = margin + icon_size + w.scale(14);
    HGDIOBJ old_font = SelectObject(dc, w.title_font);
    SetTextColor(dc, RGB(0xf5, 0xf7, 0xfc));
    RECT title{text_left, margin - w.scale(2), client.right - margin, margin + w.scale(20)};
    DrawTextW(dc, w.finished && !w.succeeded ? L"OBSReplay wasn’t installed" : L"Installing OBSReplay", -1, &title, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, w.text_font);
    SetTextColor(dc, RGB(0x8e, 0x9d, 0xb2));
    RECT state{text_left, margin + w.scale(20), client.right - margin, margin + w.scale(40)};
    DrawTextW(dc, stages[w.stage], -1, &state, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
    const int bar_height = w.scale(6), bar_top = client.bottom - margin - bar_height, bar_width = client.right - margin * 2;
    const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    const HBRUSH track = CreateSolidBrush(RGB(0x19, 0x22, 0x2e)), fill = CreateSolidBrush(RGB(0x00, 0x74, 0xd9));
    HGDIOBJ old_brush = SelectObject(dc, track);
    RoundRect(dc, margin, bar_top, margin + bar_width + 1, bar_top + bar_height + 1, bar_height, bar_height);
    const int filled = std::max(bar_height, static_cast<int>(bar_width * std::clamp(w.shown, 0.0, 1.0)));
    SelectObject(dc, fill);
    RoundRect(dc, margin, bar_top, margin + filled + 1, bar_top + bar_height + 1, bar_height, bar_height);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(track);
    DeleteObject(fill);
    BitBlt(screen, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(w.hwnd, &area);
}
LRESULT CALLBACK install_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* w = reinterpret_cast<InstallWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!w) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_PAINT: paint(*w); return 0;
    case WM_ERASEBKGND: return 1;
    case progress_message:
        w->target = static_cast<double>(wparam) / 100.0;
        w->stage = static_cast<int>(lparam);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case finished_message:
        w->finished = true;
        w->succeeded = wparam == 1;
        if (w->succeeded) w->target = 1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wparam == frame_timer) {
            // The bar eases toward the step the work has reached, so a one-second install still reads as progress.
            if (w->shown < w->target) { w->shown = std::min(w->target, w->shown + std::max(0.006, (w->target - w->shown) * 0.14)); InvalidateRect(hwnd, nullptr, FALSE); }
            const auto open_for = std::chrono::steady_clock::now() - w->opened;
            if (w->finished && (!w->succeeded || (w->shown >= 0.999 && open_for > std::chrono::milliseconds(1100)))) DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE: return 0;  // the install takes about a second; it is not interrupted halfway
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// Returns an empty string on success, the reason otherwise.
std::string run_install(const fs::path& source, const Target& t) {
    InstallWindow w;
    const HINSTANCE module = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = install_proc;
    window_class.hInstance = module;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(module, MAKEINTRESOURCEW(1));
    window_class.lpszClassName = L"OBSReplay.Installer";
    RegisterClassExW(&window_class);
    w.dpi = static_cast<int>(GetDpiForSystem());
    RECT frame{0, 0, w.scale(420), w.scale(132)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, static_cast<UINT>(w.dpi));
    const int width = frame.right - frame.left, height = frame.bottom - frame.top;
    w.hwnd = CreateWindowExW(0, window_class.lpszClassName, L"OBSReplay Setup", style, (GetSystemMetrics(SM_CXSCREEN) - width) / 2, (GetSystemMetrics(SM_CYSCREEN) - height) / 2, width, height, nullptr, nullptr, module, nullptr);
    if (!w.hwnd) return "Windows could not open the install window.";
    SetWindowLongPtrW(w.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&w));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(w.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    w.icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(1), IMAGE_ICON, w.scale(36), w.scale(36), LR_DEFAULTCOLOR));
    w.title_font = CreateFontW(-w.scale(15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    w.text_font = CreateFontW(-w.scale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SetTimer(w.hwnd, frame_timer, 16, nullptr);
    if (!t.test) { ShowWindow(w.hwnd, SW_SHOWNORMAL); UpdateWindow(w.hwnd); }

    std::thread worker([&w, &source, &t] {
        const HWND hwnd = w.hwnd;
        const auto step = [hwnd](int percent, int stage) { PostMessageW(hwnd, progress_message, static_cast<WPARAM>(percent), stage); };
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = false;
        try {
            if (!t.test && app_running()) { step(10, 1); close_running_app(); }
            step(35, 2);
            copy_executable(source, t);
            step(60, 3);
            create_shortcut(t);
            step(80, 4);
            register_app(t);
            if (!t.test) repoint_login_startup(t);
            step(100, 5);
            ok = true;
        } catch (const std::exception& e) { w.error = e.what(); }
        if (SUCCEEDED(apartment)) CoUninitialize();
        PostMessageW(hwnd, finished_message, ok ? 1 : 0, 0);
    });
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    worker.join();
    if (w.icon) DestroyIcon(w.icon);
    DeleteObject(w.title_font);
    DeleteObject(w.text_font);
    UnregisterClassW(window_class.lpszClassName, module);
    return w.succeeded ? std::string{} : (w.error.empty() ? std::string("The install did not finish.") : w.error);
}

int uninstall(const Target& t, bool quiet) {
    bool remove_settings = false;
    if (!t.test) {
        if (!quiet) {
            if (MessageBoxW(nullptr, L"Remove OBSReplay from this PC?\n\nYour saved clips stay where they are. OBS Studio and FFmpeg stay installed.", L"Uninstall OBSReplay", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return 0;
            remove_settings = MessageBoxW(nullptr, L"Delete your OBSReplay settings too?\n\nSelect No to keep them for a later install.", L"Uninstall OBSReplay", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
        }
        close_running_app();
    }
    std::wstring note;
    fs::path data;
    if (!t.test) {
        try {
            const auto p = paths();
            data = p.data;
            // The OBSReplay profile and scene collection inside OBS. OBS must be closed to remove them safely.
            if (obs_running()) note = L"\n\nOBS is open, so the OBSReplay profile inside OBS was left in place.";
            else Integration(p).reset();
            set_login_startup(p, false);
        } catch (...) {}
    }
    std::error_code error;
    fs::remove(t.shortcut, error);
    RegDeleteKeyW(HKEY_CURRENT_USER, (uninstall_root + t.key).c_str());
    if (remove_settings && !data.empty()) fs::remove_all(data, error);
    if (!t.test && !quiet) MessageBoxW(nullptr, (L"OBSReplay was removed." + note).c_str(), L"Uninstall OBSReplay", MB_OK | MB_ICONINFORMATION);
    // A running executable cannot delete itself. A hidden command waits for this process to end, then removes the folder.
    std::wstring command = L"cmd.exe /d /c ping 127.0.0.1 -n 3 >nul & rmdir /s /q " + quoted(t.dir);
    if (remove_settings && !data.empty()) command += L" & rmdir /s /q " + quoted(data);
    std::wstring shell(32768, L'\0');
    shell.resize(GetSystemDirectoryW(shell.data(), static_cast<UINT>(shell.size())));
    const auto cmd = fs::path(shell) / L"cmd.exe";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(cmd.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, shell.c_str(), &startup, &process)) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
    return 0;
}
}

bool installed() {
    try { return same_file(self_path(), target().exe); } catch (...) { return false; }
}
bool start_uninstall() {
    try { return installed() && launch(target().exe, L"--uninstall"); } catch (...) { return false; }
}

std::optional<int> handle_install(int argc, char** argv) {
    bool remove = false, portable = false, quiet = false;
    std::wstring passed;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--uninstall") remove = true;
        else if (argument == "--portable") portable = true;
        else if (argument == "--quiet") quiet = true;
        else if (argument == "--tray") passed = L"--tray";
    }
    const bool check = !environment(L"OBSREPLAY_INSTALL_TEST").empty();
    try {
        // An isolated check of the app itself runs in place.
        if (!check && (portable || !environment(L"OBSREPLAY_DATA_DIR").empty() || headless_mode())) return std::nullopt;
        const auto t = target();
        const auto self = self_path();
        const bool installed = same_file(self, t.exe);
        if (remove) return installed ? uninstall(t, quiet) : 0;
        if (installed) return std::nullopt;
        // The same build is already installed: open it.
        if (same_content(self, t.exe) && registered(t)) {
            if (!t.test) launch(t.exe, passed);
            return 0;
        }
        const auto problem = run_install(self, t);
        if (problem.empty()) {
            if (!t.test && !launch(t.exe, passed)) MessageBoxW(nullptr, L"OBSReplay is installed. Open it from the Start menu.", L"OBSReplay", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (t.test) return 1;
        MessageBoxW(nullptr, wide(problem + "\n\nOBSReplay will run from this file instead.").c_str(), L"OBSReplay Setup", MB_OK | MB_ICONWARNING);
    } catch (const std::exception& e) {
        if (check) return 1;
        MessageBoxW(nullptr, wide(std::string(e.what()) + "\n\nOBSReplay will run from this file instead.").c_str(), L"OBSReplay Setup", MB_OK | MB_ICONWARNING);
    }
    return std::nullopt;
}
}
