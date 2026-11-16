#include "controller.hpp"
#include "media.hpp"
#include "editor.hpp"
#include "audio_sessions.hpp"
#include "installer.hpp"
#include "tray_menu.hpp"
#include "log.hpp"
#include "resources.hpp"
#include "updater.hpp"

#include <saucer/smartview.hpp>
#include <saucer/embedded/all.hpp>
#include <windows.h>
#include <objidl.h>
#include <saucer/modules/stable/webview2.hpp>
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>

namespace {
using replay::Json;
using Microsoft::WRL::ComPtr;
constexpr UINT tray_message = WM_APP + 17;
constexpr UINT save_id = 201, show_id = 202;
constexpr UINT timer_id = 301;
constexpr UINT reveal_timer_id = 302;
constexpr UINT suspend_timer_id = 303;
constexpr UINT tray_sample_timer_id = 304;
constexpr UINT update_timer_id = 305;
constexpr int tray_sample_seconds = 90;
constexpr UINT menu_show = 401, menu_save = 402, menu_toggle = 403, menu_quit = 404, menu_quit_all = 405, menu_update = 406;
constexpr auto& instance_property = replay::app_window_property;
// Value of that property: 1 for the normal instance, a hash of OBSREPLAY_DATA_DIR for an isolated one,
// so a second launch only raises the window of its own instance.
std::uintptr_t instance_tag = 1;
const UINT show_message = RegisterWindowMessageW(L"OBSReplay.ShowWindow.0.1");
// Sent by the installer before it replaces or removes the executable.
const UINT quit_message = RegisterWindowMessageW(replay::quit_message_name);
const UINT taskbar_message = RegisterWindowMessageW(L"TaskbarCreated");
bool start_hidden = false;
LRESULT CALLBACK desktop_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);

struct Binding {
    UINT modifiers = 0;
    UINT key = 0;
    bool operator==(const Binding&) const = default;
};

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

Binding parse_hotkey(const std::string& value) {
    Binding result;
    size_t start = 0;
    for (;;) {
        const auto end = value.find('+', start);
        auto token = trim(value.substr(start, end == std::string::npos ? end : end - start));
        std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        UINT modifier = 0;
        if (token == "ALT") modifier = MOD_ALT;
        else if (token == "CTRL" || token == "CONTROL") modifier = MOD_CONTROL;
        else if (token == "SHIFT") modifier = MOD_SHIFT;
        if (modifier) {
            if (result.modifiers & modifier) throw std::runtime_error("Shortcut repeats a modifier.");
            result.modifiers |= modifier;
        } else {
            if (result.key) throw std::runtime_error("Shortcut must contain one key.");
            if (token.size() == 1 && ((token[0] >= 'A' && token[0] <= 'Z') || (token[0] >= '0' && token[0] <= '9'))) result.key = static_cast<UINT>(token[0]);
            else if (token.size() >= 2 && token[0] == 'F' && token.size() <= 3 && token.find_first_not_of("0123456789", 1) == std::string::npos) {
                const int number = std::stoi(token.substr(1));
                if (number >= 1 && number <= 24) result.key = VK_F1 + number - 1;
            }
            if (!result.key) throw std::runtime_error("Use a shortcut such as Alt+F10 or Ctrl+Shift+R.");
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!result.key || !result.modifiers) throw std::runtime_error("Include Alt, Ctrl or Shift in each shortcut.");
    return result;
}

std::string pick_path(HWND owner, const std::string& current, bool folder) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) throw std::runtime_error("Windows could not open the file picker.");
    DWORD options = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    options |= folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
    dialog->SetOptions(options);
    dialog->SetTitle(folder ? L"Choose replay folder" : L"Choose OBS Studio (obs64.exe)");
    if (!folder) {
        const COMDLG_FILTERSPEC filters[] = {{L"OBS Studio",L"obs64.exe"},{L"Applications",L"*.exe"}};
        dialog->SetFileTypes(2, filters);
    }
    if (!current.empty()) {
        auto location = replay::from_string(current);
        if (!folder) location = location.parent_path();
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(location.c_str(), nullptr, IID_PPV_ARGS(&item)))) dialog->SetFolder(item.Get());
    }
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    if (FAILED(shown)) throw std::runtime_error("Windows could not complete the file picker.");
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) throw std::runtime_error("No file was selected.");
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected))) throw std::runtime_error("The selected item is not a local path.");
    const auto result = replay::utf8(selected);
    CoTaskMemFree(selected);
    return result;
}

// What OBSReplay uses while it sits in the tray: {memory, processes}, measured the last time the window was hidden
// (this run or an earlier one, kept in tray-usage.json). Null until it has been measured once.
Json tray_usage;
void load_tray_usage() {
    try { const auto saved=Json::parse(replay::read_text(replay::paths().data/L"tray-usage.json")); // Readings taken after a shorter wait were too high and are ignored.
        if (saved.is_object() && saved.contains("memory") && saved.contains("processes") && saved.value("after",0)==tray_sample_seconds) tray_usage=saved; } catch (...) {}
}
void measure_tray_usage() {
    try {
        const auto app=replay::resource_usage({}).at("app");
        tray_usage={{"memory",app.at("memory")},{"processes",app.at("processes")},{"after",tray_sample_seconds}};
        replay::log_info("In tray: memory_mb={} processes={}",app.at("memory").get<uint64_t>()/(1024*1024),app.at("processes").get<unsigned>());
        replay::atomic_write(replay::paths().data/L"tray-usage.json",tray_usage.dump());
    } catch (...) {}
}

// What the app and OBS are using, with OBS's own figures and the clips drive, for the Debug tab and exported logs.
Json usage(replay::Controller& controller) {
    std::string clips;
    try { clips=Json::parse(controller.snapshot()).at("settings").value("clips_dir",std::string{}); } catch (...) {}
    auto result=replay::resource_usage(replay::from_string(clips));
    result["obs_stats"]=controller.obs_stats();
    // Before the first measurement: the hidden window keeps its processes and holds about 20 MB (measured on a typical PC).
    result["tray"]=tray_usage.is_object() ? tray_usage : Json{{"memory",20*1024*1024},{"processes",result["app"]["processes"]},{"estimated",true}};
    return result;
}

// Saves this session's log as a text file the user picks, with a closing summary of the app's state. Empty when cancelled.
std::string export_log(HWND owner,replay::Controller& controller) {
    std::string text=replay::log_text();
    text+="\r\n======== State when exported ========\r\n";
    try {
        auto state=Json::parse(controller.snapshot());
        if (state.contains("clips")) state["clips"]=std::to_string(state["clips"].size())+" listed";
        text+=state.dump(2)+"\r\n";
    } catch (...) {}
    text+="\r\n======== Resource use when exported ========\r\n";
    try { usage(controller);Sleep(300);text+=usage(controller).dump(2)+"\r\n"; } catch (...) {}
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[64]{};
    swprintf_s(name,L"OBSReplay-log-%04d%02d%02d-%02d%02d%02d.txt",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);
    replay::fs::path target;
    if (replay::headless_mode()) target=replay::log_folder()/name;  // checks never open a dialog
    else {
        ComPtr<IFileSaveDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) throw std::runtime_error("Windows could not open the save dialog.");
        const COMDLG_FILTERSPEC filters[]={{L"Text file",L"*.txt"}};
        dialog->SetFileTypes(1,filters);
        dialog->SetDefaultExtension(L"txt");
        dialog->SetFileName(name);
        dialog->SetTitle(L"Export OBSReplay log");
        dialog->SetOptions(FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_OVERWRITEPROMPT|FOS_NOCHANGEDIR);
        const HRESULT shown=dialog->Show(owner);
        if (shown==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
        if (FAILED(shown)) throw std::runtime_error("Windows could not complete the save dialog.");
        ComPtr<IShellItem> item;
        PWSTR selected=nullptr;
        if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&selected))) throw std::runtime_error("Choose a folder on this PC for the log.");
        target=selected;
        CoTaskMemFree(selected);
    }
    replay::atomic_write(target,text);
    replay::log_info("Log exported: file={} bytes={}",replay::quoted_value(replay::path_string(target)),text.size());
    return replay::path_string(target);
}

struct Desktop {
    saucer::application* app;
    std::shared_ptr<saucer::window> window;
    saucer::smartview* view;
    replay::Controller* controller;
    replay::EditorHost* editors = nullptr;
    replay::Updater* updater = nullptr;
    unsigned update_revision = 0;
    HWND hwnd;
    NOTIFYICONDATAW tray{};
    bool tray_ready = false;
    bool tray_v4 = false;
    bool menu_open = false;
    bool registered = false;
    Binding save_binding, show_binding;
    std::optional<std::pair<std::string,std::string>> pending_keys;
    bool pending_seen_busy = false;
    std::string hotkey_error;
    std::string last_error;
    std::string last_clip;
    bool observed_state = false;
    bool recording_hotkey = false;
    std::chrono::steady_clock::time_point recording_started{};
    bool first_frame_ready = false;
    bool reveal_requested = !start_hidden;

    ~Desktop() {
        KillTimer(hwnd,timer_id);
        KillTimer(hwnd,reveal_timer_id);
        KillTimer(hwnd,suspend_timer_id);
        KillTimer(hwnd,tray_sample_timer_id);
        KillTimer(hwnd,update_timer_id);
        UnregisterHotKey(hwnd,save_id);UnregisterHotKey(hwnd,show_id);
        if (tray_ready) Shell_NotifyIconW(NIM_DELETE,&tray);
        RemovePropW(hwnd,instance_property);
        RemoveWindowSubclass(hwnd,desktop_proc,1);
    }

    void show(bool replay_entry=true) {
        reveal_requested=true;
        if (!first_frame_ready) return;
        KillTimer(hwnd,suspend_timer_id);
        KillTimer(hwnd,tray_sample_timer_id);
        auto native = view->native();
        ComPtr<ICoreWebView2_3> suspendable;
        if (SUCCEEDED(native.webview->QueryInterface(IID_PPV_ARGS(&suspendable)))) suspendable->Resume();
        native.controller->put_IsVisible(TRUE);
        if (replay::headless_mode()) replay::show_headless(hwnd);
        else {
            window->set_minimized(false);
            window->show();
            window->focus();
            SetForegroundWindow(hwnd);
        }
        if (replay_entry) { replay::log_info("Window shown");view->execute("window.dispatchEvent(new CustomEvent('obsreplay:reopen'))"); }
    }
    void hide() {
        reveal_requested=false;
        record_hotkey(false);
        replay::log_info("Window hidden: to={}",tray_ready ? "tray" : "taskbar");
        if (!tray_ready) { window->set_minimized(true); return; }
        // The page first holds its content ready for the opening animation; the WebView is hidden and suspended a moment
        // later, so the frame it shows again on reopen is that held state and not the old page.
        view->execute("window.dispatchEvent(new CustomEvent('obsreplay:hiding'))");
        window->hide();
        SetTimer(hwnd,suspend_timer_id,180,nullptr);
    }
    void suspend() {
        KillTimer(hwnd,suspend_timer_id);
        if (reveal_requested) return;
        view->native().controller->put_IsVisible(FALSE);
        ComPtr<ICoreWebView2_3> suspendable;
        if (SUCCEEDED(view->native().webview->QueryInterface(IID_PPV_ARGS(&suspendable)))) {
            suspendable->TrySuspend(Microsoft::WRL::Callback<ICoreWebView2TrySuspendCompletedHandler>([](HRESULT result,BOOL suspended) {
                replay::log_info("Interface suspended: ok={} result=0x{:08X}",suspended!=FALSE,static_cast<unsigned>(result));
                return S_OK;
            }).Get());
        }
        // A suspended page gives its memory back over the next 30 to 45 seconds. Once it has, note what the app holds
        // in the tray (shown on the Debug tab). A reading taken sooner is several times too high.
        SetTimer(hwnd,tray_sample_timer_id,tray_sample_seconds*1000,nullptr);
    }
    void ready() {
        if (first_frame_ready) return;
        first_frame_ready=true;
        replay::log_info("Interface ready: shown={}",reveal_requested);
        KillTimer(hwnd,reveal_timer_id);
        if (reveal_requested) show(false);
        else hide();
    }
    void add_tray() {
        tray = {};
        tray.cbSize = sizeof(tray);
        tray.hWnd = hwnd;
        tray.uID = 1;
        tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        tray.uCallbackMessage = tray_message;
        tray.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        wcscpy_s(tray.szTip, L"OBSReplay — capture controls");
        tray_ready = Shell_NotifyIconW(NIM_ADD, &tray) != FALSE;
        replay::log_info("Tray icon: added={}",tray_ready);
        if (tray_ready) { tray.uVersion = NOTIFYICON_VERSION_4; tray_v4 = Shell_NotifyIconW(NIM_SETVERSION, &tray) != FALSE; }
    }
    void command(const std::string& action) {
        const auto result = controller->dispatch(action);
        if (!result) { replay::log_warn("Command refused: action={} error={}",action,replay::quoted_value(result.error()));notify(result.error()); }
    }
    void notify(const std::string& message) {
        if (!tray_ready) return;
        replay::log_info("Tray notification: {}",replay::quoted_value(message));
        const auto text = replay::wide(message);
        tray.uFlags = NIF_INFO;
        wcsncpy_s(tray.szInfoTitle, L"OBSReplay", _TRUNCATE);
        wcsncpy_s(tray.szInfo, text.c_str(), _TRUNCATE);
        tray.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &tray);
    }
    std::expected<bool,std::string> bind(const std::string& save, const std::string& show_key) {
        try {
            const auto next_save = parse_hotkey(save), next_show = parse_hotkey(show_key);
            if (next_save == next_show) return std::unexpected("Save and show shortcuts must be different.");
            if (registered && next_save == save_binding && next_show == show_binding) return true;
            const bool had = registered;
            if (had) { UnregisterHotKey(hwnd,save_id); UnregisterHotKey(hwnd,show_id); }
            const bool first = RegisterHotKey(hwnd,save_id,next_save.modifiers | MOD_NOREPEAT,next_save.key) != FALSE;
            const bool second = first && RegisterHotKey(hwnd,show_id,next_show.modifiers | MOD_NOREPEAT,next_show.key) != FALSE;
            if (first && second) { save_binding=next_save;show_binding=next_show;registered=true;hotkey_error.clear();replay::log_info("Shortcuts registered: save={} show={}",save,show_key);return true; }
            UnregisterHotKey(hwnd,save_id); UnregisterHotKey(hwnd,show_id);
            registered=false;
            if (had) {
                const bool restored_save = RegisterHotKey(hwnd,save_id,save_binding.modifiers | MOD_NOREPEAT,save_binding.key) != FALSE;
                const bool restored_show = RegisterHotKey(hwnd,show_id,show_binding.modifiers | MOD_NOREPEAT,show_binding.key) != FALSE;
                registered=restored_save && restored_show;
                if (!registered) { UnregisterHotKey(hwnd,save_id);UnregisterHotKey(hwnd,show_id); }
            }
            const auto shortcut=first ? "Show shortcut ("+show_key+")" : "Save shortcut ("+save+")";
            replay::log_warn("Shortcut not registered: {} is in use; previous_restored={}",shortcut,registered);
            return std::unexpected(shortcut+" is already used by Windows or another app. Choose another combination.");
        } catch (const std::exception& e) { return std::unexpected(e.what()); }
    }
    void reconcile() {
        try {
            const auto state = Json::parse(controller->snapshot());
            if (state.value("quit",false)) { app->post([app=app] { app->quit(); });return; }
            const auto error=state.value("error",std::string{});
            std::string clip;
            if (state.contains("clips") && state["clips"].is_array() && !state["clips"].empty()) clip=state["clips"][0].value("path",std::string{});
            if (observed_state && (!window->visible() || window->minimized())) {
                if (!error.empty() && error!=last_error) notify(error);
                else if (!clip.empty() && clip!=last_clip) notify("Replay saved: "+replay::path_string(replay::from_string(clip).filename()));
            }
            // A new replay or error reaches an open window at once: the page is told to read the state now
            // and does not wait for its own one-second timer, which Windows may slow down for a window without focus.
            if (observed_state && first_frame_ready && reveal_requested && (clip!=last_clip || error!=last_error)) view->execute("window.dispatchEvent(new CustomEvent('obsreplay:state'))");
            last_error=error;last_clip=clip;observed_state=true;
            if (recording_hotkey) {
                if (std::chrono::steady_clock::now()-recording_started<std::chrono::seconds(45)) return;
                recording_hotkey=false;
            }
            const auto& settings = state.at("settings");
            if (!settings.contains("save_hotkey") || !settings.contains("show_hotkey")) return;
            const std::pair<std::string,std::string> keys{settings.at("save_hotkey").get<std::string>(),settings.at("show_hotkey").get<std::string>()};
            if (pending_keys) {
                pending_seen_busy = pending_seen_busy || state.value("busy",false);
                if (keys == *pending_keys) pending_keys.reset();
                else if (pending_seen_busy && !state.value("busy",false)) pending_keys.reset();
                else return;
            }
            const auto result = bind(keys.first,keys.second);
            if (!result) hotkey_error=result.error();
        } catch (const std::exception& e) { hotkey_error=e.what(); }
    }
    void record_hotkey(bool enabled) {
        recording_hotkey=enabled;
        if (enabled) {
            recording_started=std::chrono::steady_clock::now();
            UnregisterHotKey(hwnd,save_id);UnregisterHotKey(hwnd,show_id);
            registered=false;
        } else reconcile();
    }
    std::string snapshot() {
        auto state=Json::parse(controller->snapshot());
        if (!hotkey_error.empty()) state["hotkey_error"]=hotkey_error;
        return state.dump();
    }
    std::expected<bool,std::string> dispatch(std::string action,std::string payload) {
        try {
            if (action=="setup" || action=="apply") {
                if (pending_keys) return std::unexpected("Wait for the current settings change to finish.");
                // Guided setup sends {settings, start}; Settings sends the settings object itself.
                const auto request=Json::parse(payload);
                const auto settings=request.is_object() && request.contains("settings") && request["settings"].is_object() ? request["settings"] : request;
                replay::validate_settings(settings);
                const auto previous=Json::parse(controller->snapshot()).at("settings");
                const auto result=bind(settings.at("save_hotkey"),settings.at("show_hotkey"));
                if (!result) return result;
                const auto accepted=controller->dispatch(action,payload);
                if (!accepted) {
                    const auto restored=bind(previous.at("save_hotkey"),previous.at("show_hotkey"));
                    if (!restored) hotkey_error=restored.error();
                    return accepted;
                }
                pending_keys=std::pair{settings.at("save_hotkey").get<std::string>(),settings.at("show_hotkey").get<std::string>()};
                pending_seen_busy=true;
                return accepted;
            }
            return controller->dispatch(std::move(action),std::move(payload));
        } catch (const std::exception& e) { return std::unexpected(e.what()); }
    }
    // The tray menu's content for a controller state: the capture state in the header, then the actions.
    static replay::TrayMenu tray_model(const Json& state,const std::string& update={}) {
        const bool active=state.value("replay_active",false),configured=state.value("integration",false);
        const Json settings=state.value("settings",Json::object());
        const int seconds=settings.value("clip_seconds",120);
        const auto length=seconds<60 ? std::to_wstring(seconds)+L" seconds" : seconds==60 ? std::wstring(L"minute") : std::to_wstring(seconds/60)+L" minutes";
        replay::TrayMenu model;
        model.theme=settings.value("theme",std::string{"original"});
        model.animate=settings.value("animations_enabled",true);
        model.state=active ? replay::TrayMenu::State::capturing : !configured||!state.value("error",std::string{}).empty() ? replay::TrayMenu::State::attention : replay::TrayMenu::State::idle;
        model.status=active ? L"Capturing the last "+length : !configured ? L"Not set up yet" : L"Capture is stopped";
        model.items={
            {static_cast<int>(menu_save),L"Save replay",replay::wide(settings.value("save_hotkey",std::string{})),active,false},
            {static_cast<int>(menu_toggle),active ? L"Stop capture" : L"Start capture",active ? L"Clears the buffer" : L"",configured,false},
            {static_cast<int>(menu_show),L"Open OBSReplay",L"",true,false},
            {static_cast<int>(menu_quit),L"Close OBSReplay",L"OBS keeps running",true,true},
            {static_cast<int>(menu_quit_all),L"Close OBSReplay and OBS",L"",true,false}};
        // A newer release that was not skipped: one row that opens the window on the update.
        if (!update.empty()) model.items.insert(model.items.begin()+3,{static_cast<int>(menu_update),L"Update to "+replay::wide(update),L"",true,true});
        return model;
    }
    // Tells the page the update state changed. `open` also asks it to show the update dialog.
    void announce_update(bool open=false) {
        if (!first_frame_ready || !reveal_requested) return;
        const std::string script=std::string("window.dispatchEvent(new CustomEvent('obsreplay:update',{detail:")+(open ? "'open'" : "''")+"}))";
        static_cast<saucer::webview&>(*view).execute(script);
    }
    void update_tick() {
        if (!updater) return;
        updater->tick();
        if (const auto revision=updater->revision(); revision!=update_revision) { update_revision=revision;announce_update(); }
    }
    void menu() {
        // One menu at a time: a second open while the first is tracking would nest menus and swallow the choice.
        if (menu_open) return;
        menu_open=true;
        struct Reset { bool& flag; ~Reset() { flag=false; } } reset{menu_open};
        Json state=Json::object();
        try { state=Json::parse(controller->snapshot()); } catch (...) {}
        const bool active=state.value("replay_active",false);
        const auto model=tray_model(state,updater ? updater->offered() : std::string{});
        const int selected=replay::show_tray_menu(hwnd,model);
        replay::log_info("Tray menu: choice={}",selected==menu_show ? "open" : selected==menu_save ? "save" : selected==menu_toggle ? (active ? "stop" : "start") : selected==menu_quit ? "close" : selected==menu_quit_all ? "close_with_obs" : selected==menu_update ? "update" : "none");
        if (selected==menu_show) show();
        else if (selected==menu_update) { show();announce_update(true); }
        else if (selected==menu_save) command("save");
        else if (selected==menu_toggle) command(active ? "stop" : "start");
        else if (selected==menu_quit || selected==menu_quit_all) {
            const auto pending=editors ? editors->pending() : std::string{};
            if (!pending.empty() && MessageBoxW(hwnd,replay::wide(pending+"\n\nQuit OBSReplay anyway?").c_str(),L"OBSReplay",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) return;
            // The controller closes OBS first, then sets "quit" in its state; reconcile() ends the app when it sees it.
            if (selected==menu_quit_all) { command("shutdown");return; }
            // Quit after the menu has fully closed, so its message loop can't swallow the request.
            app->post([app=app] { app->quit(); });
        }
    }
};

LRESULT CALLBACK desktop_proc(HWND hwnd,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR ref) {
    auto& desktop=*reinterpret_cast<Desktop*>(ref);
    try {
        if (message==show_message) { desktop.show();return 0; }
        if (message==quit_message) { replay::log_info("Quit requested by the installer");desktop.app->post([app=desktop.app] { app->quit(); });return 0; }
        if (message==taskbar_message) { desktop.add_tray();return 0; }
        if (message==tray_message) {
            const UINT event=LOWORD(l);
            if (event==NIN_SELECT || event==NIN_KEYSELECT || event==WM_LBUTTONDBLCLK) desktop.show();
            // Version 4 icons send WM_CONTEXTMENU after WM_RBUTTONUP for the same right-click; open the menu once.
            else if (desktop.tray_v4 ? event==WM_CONTEXTMENU : event==WM_RBUTTONUP) desktop.menu();
            return 0;
        }
        if (message==WM_HOTKEY) {
            if (w==save_id) { replay::log_info("Shortcut pressed: save");desktop.command("save"); }
            else if (w==show_id) { if (desktop.window->visible() && !desktop.window->minimized()) desktop.hide();else desktop.show(); }
            return 0;
        }
        if (message==WM_TIMER && w==timer_id) { desktop.reconcile();return 0; }
        if (message==WM_TIMER && w==reveal_timer_id) { desktop.ready();return 0; }
        if (message==WM_TIMER && w==suspend_timer_id) { desktop.suspend();return 0; }
        if (message==WM_TIMER && w==update_timer_id) { desktop.update_tick();return 0; }
        if (message==WM_TIMER && w==tray_sample_timer_id) { KillTimer(hwnd,tray_sample_timer_id);if (!desktop.reveal_requested) measure_tray_usage();return 0; }
    } catch (...) {}
    return DefSubclassProc(hwnd,message,w,l);
}

BOOL CALLBACK find_instance(HWND hwnd,LPARAM) {
    if (reinterpret_cast<std::uintptr_t>(GetPropW(hwnd,instance_property))==instance_tag) { PostMessageW(hwnd,show_message,0,0);return FALSE; }
    return TRUE;
}

coco::stray start(saucer::application* app) {
    try {
    auto created=saucer::window::create(app);
    if (!created) { MessageBoxW(nullptr,L"Windows could not create the OBSReplay window.",L"OBSReplay",MB_OK|MB_ICONERROR);app->quit();co_return; }
    auto window=std::move(*created);
    window->set_title("OBSReplay");
    window->set_decorations(saucer::window::decoration::partial);
    const HWND startup_hwnd=window->native().hwnd;
    MONITORINFO startup_monitor{};
    startup_monitor.cbSize=sizeof(startup_monitor);
    const bool have_work_area=GetMonitorInfoW(MonitorFromWindow(startup_hwnd,MONITOR_DEFAULTTONEAREST),&startup_monitor)!=FALSE;
    saucer::size initial_size{1040,760};
    if (have_work_area) {
        const UINT dpi=GetDpiForWindow(startup_hwnd);
        const int work_width=startup_monitor.rcWork.right-startup_monitor.rcWork.left;
        const int work_height=startup_monitor.rcWork.bottom-startup_monitor.rcWork.top;
        initial_size.w=std::min(initial_size.w,std::max(1,MulDiv(work_width,96,dpi ? static_cast<int>(dpi) : 96)-24));
        initial_size.h=std::min(initial_size.h,std::max(1,MulDiv(work_height,96,dpi ? static_cast<int>(dpi) : 96)-24));
    }
    window->set_size(initial_size);
    window->set_min_size({.w=std::min(840,initial_size.w),.h=std::min(620,initial_size.h)});
    if (have_work_area) {
        RECT bounds{};
        if (GetWindowRect(startup_hwnd,&bounds)) {
            const int x=startup_monitor.rcWork.left+std::max(0,static_cast<int>((startup_monitor.rcWork.right-startup_monitor.rcWork.left-(bounds.right-bounds.left))/2));
            const int y=startup_monitor.rcWork.top+std::max(0,static_cast<int>((startup_monitor.rcWork.bottom-startup_monitor.rcWork.top-(bounds.bottom-bounds.top))/2));
            SetWindowPos(startup_hwnd,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
        }
    }
    const auto ground = replay::theme_ground(replay::saved_theme());
    window->set_background({ground.r,ground.g,ground.b,255});
    auto view_created=saucer::smartview::create({.window=window,.storage_path=replay::isolated_webview_folder()});
    if (!view_created) { MessageBoxW(nullptr,L"The WebView2 interface could not start. Repair or update Microsoft Edge WebView2 Runtime and try again.",L"OBSReplay",MB_OK|MB_ICONERROR);app->quit();co_return; }
    auto view=std::move(*view_created);
    view.set_force_dark(!ground.light);
    view.set_background({ground.r,ground.g,ground.b,255});
    view.set_context_menu(false);
    {
        // Accidental Ctrl+wheel zoom would rescale the whole interface.
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(view.native().webview->get_Settings(&settings)) && settings) settings->put_IsZoomControlEnabled(FALSE);
    }
    replay::Controller controller(replay::paths());
    replay::ClipMedia media(view.native().webview, controller);
    replay::EditorHost editors(app, controller, replay::paths());
    replay::Updater updater(replay::paths());
    Desktop desktop{app,window,&view,&controller,&editors,&updater,0,window->native().hwnd};
    SetPropW(desktop.hwnd,instance_property,reinterpret_cast<HANDLE>(instance_tag));
    SendMessageW(desktop.hwnd,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1))));
    SendMessageW(desktop.hwnd,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1))));
    if (SetWindowSubclass(desktop.hwnd,desktop_proc,1,reinterpret_cast<DWORD_PTR>(&desktop))) desktop.add_tray();
    replay::ignore_alt_menu(desktop.hwnd);
    SetTimer(desktop.hwnd,timer_id,500,nullptr);
    SetTimer(desktop.hwnd,update_timer_id,1000,nullptr);
    desktop.reconcile();
    window->on<saucer::window::event::close>([&] { desktop.hide();return saucer::policy::block; });
    window->on<saucer::window::event::focus>([&](bool focused) { if (!focused && desktop.recording_hotkey) desktop.record_hotkey(false); });
    view.on<saucer::webview::event::navigate>([](const saucer::navigation& navigation) {
        const auto url=navigation.url();
        return !navigation.new_window() && url.scheme()=="saucer" && url.host()=="embedded" && url.path()==std::filesystem::path("/index.html") ? saucer::policy::allow : saucer::policy::block;
    });
    view.on<saucer::webview::event::permission>([](const std::shared_ptr<saucer::permission::request>& request) { request->accept(false);return saucer::status::handled; });
    view.expose("snapshot",[&] { return desktop.snapshot(); });
    view.expose("media_url",[&](std::string path) { return media.url(path); });
    view.expose("open_editor",[&](std::string path) { return editors.open(path); });
    view.expose("editors_pending",[&] { return editors.pending(); });
    view.expose("ffmpeg_state",[&](bool refresh) { return editors.ffmpeg_state(refresh); });
    view.expose("install_ffmpeg",[&] { return editors.install_ffmpeg(); });
    view.expose("pick_ffmpeg",[&] { return editors.pick_ffmpeg(desktop.hwnd); });
    view.expose("clip_waveform",[&](std::string path,saucer::executor<std::string> executor) {
        editors.waveform(path,[executor=std::move(executor)](std::string result) { executor.resolve(std::move(result)); });
    });
    view.expose("dispatch",[&](std::string action,std::string payload) { return desktop.dispatch(std::move(action),std::move(payload)); });
    view.expose("pick_folder",[&](std::string current) { return pick_path(desktop.hwnd,current,true); });
    view.expose("pick_obs",[&](std::string current) { return pick_path(desktop.hwnd,current,false); });
    view.expose("hide",[&] { desktop.hide(); });
    view.expose("hotkey_recording",[&](bool enabled) { desktop.record_hotkey(enabled); });
    view.expose("ui_ready",[&] { desktop.ready(); });
    view.expose("theme_changed",[&](std::string id) {
        const auto next = replay::theme_ground(id);
        window->set_background({next.r,next.g,next.b,255});
        view.set_background({next.r,next.g,next.b,255});
        view.set_force_dark(!next.light);
    });
    view.expose("set_theme",[&](std::string id) { return controller.set_theme(id); });
    view.expose("installed",[] { return replay::installed(); });
    view.expose("uninstall",[]() -> std::expected<bool,std::string> {
        if (!replay::start_uninstall()) return std::unexpected("The uninstaller could not start. Use Installed apps in Windows Settings instead.");
        return true;
    });
    view.expose("quit",[&] { replay::log_info("Quit requested from the window");app->post([app] { app->quit(); }); });
    view.expose("app_info",[] { auto info=replay::build_info();info["logs"]=replay::path_string(replay::log_folder());return info.dump(); });
    view.expose("resources",[&] { return usage(controller).dump(); });
    view.expose("log_tail",[](int lines) { return replay::log_tail(static_cast<std::size_t>(std::clamp(lines,1,600))); });
    view.expose("export_log",[&]() -> std::expected<std::string,std::string> {
        try { return export_log(desktop.hwnd,controller); } catch (const std::exception& e) { replay::log_error("Log export failed: error={}",replay::quoted_value(e.what()));return std::unexpected(e.what()); }
    });
    view.expose("open_logs",[]() -> std::expected<bool,std::string> {
        try { replay::open_path(replay::log_folder());return true; } catch (const std::exception& e) { return std::unexpected(e.what()); }
    });
    // Lines from the page: navigation and script errors.
    view.expose("ui_log",[](std::string level,std::string text) {
        if (text.size()>2000) text.resize(2000);
        replay::log_write(level=="error" ? replay::LogLevel::error : level=="warn" ? replay::LogLevel::warn : level=="debug" ? replay::LogLevel::debug : replay::LogLevel::info,"UI: "+text);
    });
    // Updates. The page reads the state when told it changed (obsreplay:update) and while its dialog is open.
    const auto update_blocked=[&]() -> std::string { return editors.pending().empty() ? std::string{} : "An editor has unfinished work. Export or close it, then update."; };
    view.expose("update_state",[&] { auto state=updater.state();state["blocked"]=update_blocked();return state.dump(-1,' ',false,Json::error_handler_t::replace); });
    view.expose("update_check",[&] { updater.check(true); });
    view.expose("update_skip",[&](bool skipped) { updater.skip(skipped); });
    view.expose("update_enabled",[&](bool enabled) { updater.set_enabled(enabled); });
    view.expose("update_cancel",[&] { updater.cancel(); });
    view.expose("update_seen",[&] { updater.seen(); });
    view.expose("update_start",[&,app]() -> std::expected<bool,std::string> {
        if (const auto blocked=update_blocked(); !blocked.empty()) return std::unexpected(blocked);
        // The download ends on a worker thread; the installer is started from this one, after one more look at the editors.
        return updater.start([&,app](const replay::fs::path& file) { app->post([&,file] {
            if (const auto blocked=update_blocked(); !blocked.empty()) { updater.fail(blocked);return; }
            updater.install(file);
        }); });
    });
    // Links in release notes and the releases page: GitHub only, in the default browser.
    view.expose("open_link",[](std::string url) -> std::expected<bool,std::string> {
        const bool plain=std::all_of(url.begin(),url.end(),[](unsigned char c) { return c>32 && c<127 && c!='"' && c!='\\' && c!='<' && c!='>' && c!='|' && c!='^'; });
        if (!url.starts_with("https://github.com/") || url.size()>600 || !plain) return std::unexpected("Only GitHub links open from here.");
        replay::log_info("Link opened: url={}",replay::quoted_value(url));
        if (replay::headless_mode()) return true;  // checks never open a browser
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",replay::wide(url).c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32) return std::unexpected("Windows could not open your browser.");
        return true;
    });
    // "Play video" in an editor: bring this window forward and let the page open the clip in its viewer.
    editors.on_play([&](const std::string& path) {
        desktop.show();
        // Through the plain webview call: the smartview one treats braces in the script as format fields.
        const std::string script="window.dispatchEvent(new CustomEvent('obsreplay:play',{detail:"+Json(path).dump()+"}))";
        static_cast<saucer::webview&>(view).execute(script);
    });
    view.embed(saucer::embedded::all());
    view.serve("/index.html");
    SetTimer(desktop.hwnd,reveal_timer_id,2000,nullptr);
    co_await app->finish();
    replay::log_info("Window loop ended");
    } catch (const std::exception& e) {
        replay::log_error("Startup failed: error={}",replay::quoted_value(e.what()));
        MessageBoxW(nullptr,replay::wide(e.what()).c_str(),L"OBSReplay",MB_OK|MB_ICONERROR);
        app->quit();
    }
}
}

int main(int argc,char** argv) {
    // Check only: writes the tray menu in every theme to PNG files, then exits. Opens no window.
    if (wchar_t folder[32768]{}; GetEnvironmentVariableW(L"OBSREPLAY_TRAY_PREVIEW",folder,32768)) {
        for (const auto theme : {"original","mono","light","rose","amethyst","ember"}) {
            const Json on{{"replay_active",true},{"integration",true},{"settings",{{"theme",theme},{"clip_seconds",120},{"save_hotkey","Alt+Z"}}}};
            const Json off{{"replay_active",false},{"integration",true},{"settings",{{"theme",theme},{"clip_seconds",120},{"save_hotkey","Alt+Z"}}}};
            replay::write_tray_menu_image(Desktop::tray_model(on),0,std::wstring(folder)+L"\\"+replay::wide(theme)+L"-capturing.png");
            replay::write_tray_menu_image(Desktop::tray_model(off),-1,std::wstring(folder)+L"\\"+replay::wide(theme)+L"-stopped.png");
            replay::write_tray_menu_image(Desktop::tray_model(on,"1.1.0"),3,std::wstring(folder)+L"\\"+replay::wide(theme)+L"-update.png");
        }
        return 0;
    }
    // Started outside its install folder, the executable installs itself and opens the installed copy.
    if (const auto handled=replay::handle_install(argc,argv)) return *handled;
    for (int i=1;i<argc;++i) if (std::string_view(argv[i])=="--tray") start_hidden=true;
    // An isolated data folder (OBSREPLAY_DATA_DIR) runs as its own instance so checks never touch the user's session.
    std::wstring instance_name=L"Local\\OBSReplay.Controller.0.1";
    if (wchar_t isolated[32768]{}; GetEnvironmentVariableW(L"OBSREPLAY_DATA_DIR",isolated,32768)) { const auto hash=std::hash<std::wstring>{}(isolated); instance_name+=L"."+std::to_wstring(hash); instance_tag=static_cast<std::uintptr_t>(hash)|2; }
    HANDLE instance=CreateMutexW(nullptr,FALSE,instance_name.c_str());
    if (!instance) return 1;
    if (GetLastError()==ERROR_ALREADY_EXISTS) { EnumWindows(find_instance,0);CloseHandle(instance);return 0; }
    const HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    try { replay::log_start(replay::paths()); } catch (...) {}
    load_tray_usage();
    // A crash still leaves its reason as the last line of the session log.
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* info) -> LONG {
        replay::log_error("Crash: code=0x{:08X} address={}",static_cast<unsigned>(info->ExceptionRecord->ExceptionCode),info->ExceptionRecord->ExceptionAddress);
        return EXCEPTION_CONTINUE_SEARCH;
    });
    std::set_terminate([] { replay::log_error("Crash: an unhandled C++ exception ended the app");std::abort(); });
    LPWSTR runtime=nullptr;
    const HRESULT available=GetAvailableCoreWebView2BrowserVersionString(nullptr,&runtime);
    replay::log_info("WebView2 runtime: {} start_hidden={}",runtime ? replay::quoted_value(replay::utf8(runtime)) : std::string("missing"),start_hidden);
    if (runtime) CoTaskMemFree(runtime);
    int result=1;
    if (FAILED(available)) {
        if (MessageBoxW(nullptr,L"OBSReplay needs Microsoft Edge WebView2 Runtime. Open Microsoft's download page?",L"WebView2 Runtime required",MB_YESNO|MB_ICONINFORMATION)==IDYES)
            ShellExecuteW(nullptr,L"open",L"https://developer.microsoft.com/microsoft-edge/webview2/",nullptr,nullptr,SW_SHOWNORMAL);
    } else {
        try {
            const replay::AudioSessionLabel audio_label;  // volume mixer shows OBSReplay, not Microsoft Edge WebView2
            auto app=saucer::application::create({.id="OBSReplay",.quit_on_last_window_closed=false});
            if (app) result=app->run(start);
            else MessageBoxW(nullptr,L"The OBSReplay application could not start.",L"OBSReplay",MB_OK|MB_ICONERROR);
        } catch (const std::exception& e) { MessageBoxW(nullptr,replay::wide(e.what()).c_str(),L"OBSReplay",MB_OK|MB_ICONERROR); }
    }
    replay::log_info("OBSReplay closed: exit_code={}",result);
    if (SUCCEEDED(apartment)) CoUninitialize();
    CloseHandle(instance);
    return result;
}
