#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include "controller.hpp"
#include "integration.hpp"
#include "obs_socket.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace replay {
namespace {
using Clock = std::chrono::steady_clock;
bool media_file(const fs::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return extension == L".mp4" || extension == L".mkv" || extension == L".mov" || extension == L".flv" || extension == L".ts";
}
// Recent history keeps up to 50 recorded replays and 50 edited exports (newest first), so exports never push
// recordings out of the Replays list.
constexpr std::size_t history_per_kind = 50;
void trim_history(Json& list) {
    std::size_t replays = 0, edits = 0;
    for (auto it = list.begin(); it != list.end();) {
        auto& count = it->value("edited", false) ? edits : replays;
        if (++count > history_per_kind) it = list.erase(it); else ++it;
    }
}
bool capture_changed(const Json& before, const Json& after) {
    for (const auto key : {"obs_path", "clips_dir", "clip_seconds", "capture", "monitor_id", "output_height", "fps", "encoder", "codec", "quality", "mic_enabled", "mic_device", "desktop_device", "noise", "separate_tracks"})
        if (before.at(key) != after.at(key)) return true;
    return false;
}
}

struct Controller::Impl {
    struct Action { std::string name; Json payload; std::string app; };
    struct Save { std::string app; bool sort{}, sound{}; bool confirmed{}; std::string path; };
    Paths paths;
    Integration integration;
    ObsSocket socket;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Action> queue;
    Json state;
    std::thread worker;
    std::atomic<bool> stopping{};
    std::optional<Save> pending_save;
    Clock::time_point next_connect{}, next_refresh{}, next_detect{};
    unsigned failures{};
    bool refresh_needed{};

    explicit Impl(Paths value) : paths(std::move(value)), integration(paths) {
        state = {{"settings", Json::object()}, {"options", {{"monitors", Json::array()}, {"audio_inputs", Json::array()}, {"audio_outputs", Json::array()}, {"encoders", Json::array()}}},
            {"integration", false}, {"obs_found", false}, {"obs_version", ""}, {"obs_supported", false}, {"winget", false},
            {"install_stage", ""}, {"setup_stage", ""}, {"obs_running", false}, {"connected", false}, {"owned", false}, {"replay_active", false}, {"capture_ready", false}, {"capture_issue", ""},
            {"busy", true}, {"operation", "initializing"}, {"status", "initializing"}, {"message", "Checking your OBS setup…"}, {"error", ""}, {"revision", 0}, {"clips", Json::array()}};
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        stopping.store(true);
        wake.notify_all();
        if (worker.joinable()) worker.join();
        socket.close();
    }

    template<class F> void update(F&& action) {
        std::lock_guard lock(mutex);
        auto before = state;
        action(state);
        if (before != state) state["revision"] = state.at("revision").get<uint64_t>() + 1;
    }
    Json settings() const { std::lock_guard lock(mutex); return state.at("settings"); }
    bool flag(const char* name) const { std::lock_guard lock(mutex); return state.value(name, false); }
    std::string state_value(const char* name) const { std::lock_guard lock(mutex); return state.value(name, ""); }
    void message(const std::string& text) { update([&](Json& s) { s["message"] = text; }); }
    void settle() {
        update([](Json& s) {
            s["status"] = !s.value("integration", false) ? "Setup required" : !s.value("connected", false) ? "Disconnected" : !s.value("owned", false) ? "Different OBS session" : s.value("replay_active", false) ? "Capturing" : "Ready";
        });
    }
    void fail(const std::string& error) { update([&](Json& s) { s["error"] = error; s["message"] = "Action needs attention"; }); }
    bool pause(unsigned ms) {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(ms), [&] { return stopping.load(); });
        return !stopping.load();
    }
    void set_settings(const Json& value) {
        std::error_code error;
        const auto executable = from_string(value.value("obs_path", ""));
        const bool found = !executable.empty() && fs::is_regular_file(executable, error);
        std::string version;
        bool supported{};
        if (found) if (const auto parts = file_version(executable)) {
            version = std::to_string((*parts)[0]) + "." + std::to_string((*parts)[1]) + "." + std::to_string((*parts)[2]);
            supported = (*parts)[0] >= 31;
        }
        update([&](Json& s) { s["settings"] = value; s["obs_found"] = found; s["obs_version"] = version; s["obs_supported"] = supported; });
    }
    void store_settings(const Json& value) { save_settings(paths, value); set_settings(value); }
    void stage(const char* name, const char* key = "setup_stage") { update([&](Json& s) { s[key] = name; }); }

    // Picks up an OBS installed outside the app (for example from obsproject.com) while setup is open.
    void detect_obs() {
        if (flag("obs_supported")) return;
        try {
            auto value = settings();
            const auto found = find_obs();
            if (found.empty()) return;
            if (path_string(found) != value.value("obs_path", "")) {
                value["obs_path"] = path_string(found);
                try { store_settings(value); } catch (...) { set_settings(value); }
            } else set_settings(value);
        } catch (...) {}
    }

    void use_obs(const fs::path& exe) {
        if (!exe.is_absolute() || _wcsicmp(exe.filename().c_str(), L"obs64.exe") != 0 || !fs::is_regular_file(exe))
            throw std::runtime_error("Choose obs64.exe, in OBS Studio's bin\\64bit folder.");
        const auto version = file_version(exe);
        if (!version || (*version)[0] < 31) throw std::runtime_error("This copy of OBS Studio is older than version 31. Update it, then try again.");
        auto value = settings();
        value["obs_path"] = path_string(exe);
        store_settings(value);
    }

    void install_obs() {
        const auto winget = winget_path();
        if (winget.empty())
            throw std::runtime_error("Automatic install isn't available because Windows Package Manager (winget) is missing on this PC. Download OBS Studio from obsproject.com; setup continues as soon as it's installed.");
        const bool upgrade = flag("obs_found");
        if (upgrade && obs_running()) { message("Closing OBS so it can be updated…"); socket.close(); close_all_obs(); }
        stage("preparing", "install_stage");
        message(upgrade ? "Preparing the OBS Studio update…" : "Preparing to install OBS Studio…");
        std::string tail;
        const std::wstring args = std::wstring(upgrade ? L"upgrade" : L"install") +
            L" --id OBSProject.OBSStudio --exact --source winget --silent --accept-package-agreements --accept-source-agreements";
        int reached = 0;
        const int code = run_process(winget, args, 30 * 60 * 1000, [&](const std::string& chunk) {
            tail += chunk;
            if (tail.size() > 16384) tail.erase(0, tail.size() - 16384);
            if (reached < 1 && tail.find("Downloading") != std::string::npos) {
                reached = 1; stage("downloading", "install_stage"); message("Downloading OBS Studio…");
            }
            if (reached < 2 && (tail.find("Starting package install") != std::string::npos || tail.find("Successfully verified") != std::string::npos)) {
                reached = 2; stage("installing", "install_stage"); message("Installing OBS Studio. Approve the Windows permission prompt if it appears.");
            }
        }, [&] { return stopping.load(); });
        if (code == -2) return;
        stage("verifying", "install_stage");
        message("Checking the OBS Studio installation…");
        const auto found = find_obs();
        const auto version = found.empty() ? std::nullopt : file_version(found);
        if (version && (*version)[0] >= 31) {
            auto value = settings();
            value["obs_path"] = path_string(found);
            store_settings(value);
            stage("", "install_stage");
            message("OBS Studio " + std::to_string((*version)[0]) + "." + std::to_string((*version)[1]) + " is ready.");
            return;
        }
        stage("", "install_stage");
        if (code == -1) throw std::runtime_error("The OBS Studio installer didn't finish. Check your internet connection and try again, or download OBS Studio from obsproject.com.");
        if (tail.find("No available upgrade") != std::string::npos || tail.find("No newer package") != std::string::npos)
            throw std::runtime_error("winget has no newer OBS Studio for this copy. Update OBS from obsproject.com, or choose a newer obs64.exe.");
        if (tail.find("already installed") != std::string::npos)
            throw std::runtime_error("OBS Studio seems to be installed in a location OBSReplay can't find. Choose Locate OBS and select its obs64.exe.");
        std::ostringstream text;
        text << "OBS Studio wasn't installed (winget code 0x" << std::hex << static_cast<unsigned>(code)
             << "). If you declined the Windows permission prompt, select Install again. You can also download OBS Studio from obsproject.com.";
        throw std::runtime_error(text.str());
    }

    // Asks the user's own OBS session whether it is streaming or recording before closing it for setup.
    bool other_session_busy() {
        const auto credentials = integration.shared_credentials();
        if (!credentials) return false;
        ObsSocket probe;
        try {
            probe.connect(*credentials);
            const bool busy = probe.request("GetStreamStatus", Json::object(), 3000).value("outputActive", false) ||
                              probe.request("GetRecordStatus", Json::object(), 3000).value("outputActive", false);
            probe.close();
            return busy;
        } catch (...) { probe.close(); return false; }
    }

    void reconcile_clips() {
        Json clips;
        { std::lock_guard lock(mutex); clips = state.at("clips"); }
        const auto old_size = clips.size();
        std::set<std::string> missing;
        for (auto it = clips.begin(); it != clips.end();) {
            std::error_code error;
            const bool exists = fs::is_regular_file(from_string(it->at("path").get<std::string>()), error);
            if ((!error && !exists) || error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
                missing.insert(it->at("path").get<std::string>());
                it = clips.erase(it);
            } else ++it;
        }
        if (clips.size() == old_size) return;
        update([&](Json& s) {
            // Remove only the missing files; clips added meanwhile (edited exports) stay.
            auto& list = s["clips"];
            for (auto it = list.begin(); it != list.end();) {
                if (missing.contains(it->value("path", ""))) it = list.erase(it); else ++it;
            }
            clips = list;
        });
        try { atomic_write(paths.data / L"clips.json", clips.dump(2)); }
        catch (...) { fail("Deleted replays were removed from this view, but the recent-clips history could not be saved."); }
    }

    // Lists videos already in the clips folder that the history does not know: after a fresh install, after the
    // history was deleted, or after the clips folder changed. Looks two folders deep (game, then Edited Exports).
    void discover_clips() {
        try {
            const auto root = from_string(settings().value("clips_dir", ""));
            std::error_code error;
            if (root.empty() || !root.is_absolute() || !fs::is_directory(root, error)) return;
            Json listed;
            { std::lock_guard lock(mutex); listed = state.at("clips"); }
            std::set<std::string> known;
            std::vector<Json> clips;
            for (const auto& clip : listed) { known.insert(clip.value("path", "")); clips.push_back(clip); }
            std::size_t seen = 0, found = 0;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
                if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
                if (++seen > 20000) break;
                std::error_code ignored;
                if (!it->is_regular_file(ignored) || !media_file(it->path())) continue;
                const auto text = path_string(it->path());
                if (known.contains(text)) continue;
                // The folder under the clips folder is the game; an "Edited Exports" folder marks an export.
                const auto relative = it->path().lexically_relative(root);
                bool edited = false;
                std::string app;
                for (auto part = relative.begin(); part != relative.end() && std::next(part) != relative.end(); ++part) {
                    if (_wcsicmp(part->c_str(), L"Edited Exports") == 0) edited = true;
                    else if (app.empty()) app = path_string(*part);
                }
                WIN32_FILE_ATTRIBUTE_DATA details{};
                SYSTEMTIME utc{}, local{};
                char saved[32]{};
                if (GetFileAttributesExW(it->path().c_str(), GetFileExInfoStandard, &details) && FileTimeToSystemTime(&details.ftLastWriteTime, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
                    snprintf(saved, sizeof(saved), "%04d-%02d-%02dT%02d:%02d:%02d", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
                const auto bytes = (static_cast<uint64_t>(details.nFileSizeHigh) << 32) | details.nFileSizeLow;
                clips.push_back(Json{{"path", text}, {"name", path_string(it->path().filename())}, {"app", app}, {"bytes", bytes}, {"saved_at", saved}});
                if (edited) clips.back()["edited"] = true;
                ++found;
            }
            if (!found) return;
            std::stable_sort(clips.begin(), clips.end(), [](const Json& a, const Json& b) { return a.value("saved_at", "") > b.value("saved_at", ""); });
            Json merged = clips;
            trim_history(merged);
            update([&](Json& s) { s["clips"] = merged; });
            atomic_write(paths.data / L"clips.json", merged.dump(2));
        } catch (...) {}
    }

    void initialize() {
        Json value;
        try { value = load_settings(paths); }
        catch (const std::exception& e) { value = default_settings(paths); fail(e.what()); }
        set_settings(value);
        detect_obs();
        const bool configured = integration.configured();
        const bool winget = !winget_path().empty();
        update([&](Json& s) { s["integration"] = configured; s["compression_update"] = integration.needs_update(); s["winget"] = winget; });
        try {
            const auto history_path = paths.data / L"clips.json";
            if (fs::exists(history_path)) {
                const auto stored = Json::parse(read_text(history_path));
                Json clips = Json::array();
                if (stored.is_array()) for (const auto& clip : stored) {
                    if (clips.size() >= history_per_kind * 2) break;
                    if (!clip.is_object() || !clip.contains("path") || !clip["path"].is_string()) continue;
                    const auto file = from_string(clip["path"].get<std::string>());
                    if (!file.is_absolute() || !media_file(file)) continue;
                    clips.push_back({{"path", path_string(file)}, {"name", path_string(file.filename())}, {"app", clip.value("app", "")}, {"bytes", clip.value("bytes", uint64_t{})}, {"saved_at", clip.value("saved_at", "")}});
                    if (clip.value("edited", false)) clips.back()["edited"] = true;
                }
                trim_history(clips);
                update([&](Json& s) { s["clips"] = clips; });
            }
        } catch (...) { fail("Recent clip history could not be read. Your recordings are still in the clips folder."); }
        reconcile_clips();
        discover_clips();
        try { const auto options = integration.options(value); update([&](Json& s) { s["options"] = options; }); }
        catch (const std::exception& e) { fail(e.what()); }
        if (stopping.load()) return;
        if (configured && value.value("replay_enabled", false)) {
            message("Starting your saved replay configuration…");
            try { start(); } catch (const std::exception& e) { fail(e.what()); }
        } else {
            try { background_connection(); } catch (const std::exception& e) { fail(e.what()); }
        }
        update([&](Json& s) {
            s["busy"] = !queue.empty();
            s["operation"] = queue.empty() ? "" : queue.front().name;
            if (s["error"] == "" && s["message"] == "Checking your OBS setup…") s["message"] = configured ? "Ready when you are" : "Set up replay capture to get started";
            s["revision"] = s.at("revision").get<uint64_t>() + 1;
        });
        settle();
    }

    struct DeviceIssue { std::string message; bool monitor{}; };
    DeviceIssue device_issue() {
        const auto value = settings();
        const auto contains = [](const Json& list, const std::string& id) {
            return std::any_of(list.begin(), list.end(), [&](const Json& item) { return item.value("id", "") == id; });
        };
        const auto endpoints = [](const Json& list) {
            return std::any_of(list.begin(), list.end(), [](const Json& item) { const auto id = item.value("id", ""); return id != "default" && id != "off" && !id.empty(); });
        };
        if (value.value("capture", "auto") != "game") {
            const auto selected = value.value("monitor_id", "");
            if (!selected.empty() && !contains(monitors(), selected)) return {"The selected display is disconnected. Choose an available display in Settings and Apply before starting capture.", true};
        }
        if (value.value("mic_enabled", false)) {
            const auto selected = value.value("mic_device", "default");
            const auto devices = audio_devices(true);
            if (selected == "default" ? !endpoints(devices) : !contains(devices, selected)) return {"The selected microphone is disconnected. Choose an available microphone or turn microphone capture off, then Apply.", false};
        }
        const auto selected = value.value("desktop_device", "default");
        if (selected != "off") {
            const auto devices = audio_devices(false);
            if (selected == "default" ? !endpoints(devices) : !contains(devices, selected)) return {"The selected desktop audio device is disconnected. Choose an available device or Off, then Apply.", false};
        }
        return {};
    }

    std::string scene_issue() {
        try {
        const auto value = settings();
        const std::string mode = value.value("capture", "auto");
        struct Expected { const char* name; const char* kind; };
        std::vector<Expected> expected;  // bottom to top
        if (mode != "game") expected.push_back({"OBSReplay Display", "monitor_capture"});
        if (mode != "desktop") expected.push_back({"OBSReplay Game", "game_capture"});
        const auto current = socket.request("GetCurrentProgramScene");
        if (current.value("currentProgramSceneName", "") != "OBSReplay") return "OBS is using a different scene. Select the OBSReplay scene or Apply your capture settings again.";
        const auto items = socket.request("GetSceneItemList", {{"sceneName", "OBSReplay"}}).at("sceneItems");
        if (!items.is_array() || items.size() != expected.size())
            return "The dedicated OBS scene was changed. Apply to restore the selected capture mode before starting or saving.";
        std::vector<Json> ordered(items.begin(), items.end());
        std::sort(ordered.begin(), ordered.end(), [](const Json& a, const Json& b) { return a.value("sceneItemIndex", 0) < b.value("sceneItemIndex", 0); });
        for (std::size_t i = 0; i != expected.size(); ++i)
            if (ordered[i].value("sourceName", "") != expected[i].name || !ordered[i].value("sceneItemEnabled", false))
                return "The dedicated OBS scene was changed. Apply to restore the selected capture mode before starting or saving.";
        for (const auto& item : expected) {
            const auto input = socket.request("GetInputSettings", {{"inputName", item.name}});
            const auto capture = input.value("inputSettings", Json::object());
            if (input.value("inputKind", "") != item.kind)
                return "The OBS capture source no longer matches your settings. Apply before starting or saving.";
            if (std::string_view(item.kind) == "monitor_capture") {
                if (_wcsicmp(wide(capture.value("monitor_id", "")).c_str(), wide(value.value("monitor_id", "")).c_str()) != 0)
                    return "OBS is configured for another display. Apply your selected display before starting or saving.";
            } else if (capture.value("capture_mode", "") != "any_fullscreen")
                return "OBS game capture no longer matches the fullscreen-game setting. Apply before starting or saving.";
        }
        return {};
        } catch (const std::exception&) {
            if (!socket.connected()) throw;
            return "The dedicated OBS capture scene could not be verified. Apply your capture settings again before starting or saving.";
        }
    }

    void require_capture_ready() {
        auto issue = device_issue().message;
        if (issue.empty()) issue = scene_issue();
        update([&](Json& s) { s["capture_ready"] = issue.empty(); s["capture_issue"] = issue; });
        if (!issue.empty()) throw std::runtime_error(issue);
    }

    void refresh() {
        if (!socket.connected()) throw std::runtime_error("OBS is disconnected. Reconnect before controlling capture.");
        const auto profile = socket.request("GetProfileList");
        const auto collection = socket.request("GetSceneCollectionList");
        const auto replay = socket.request("GetReplayBufferStatus");
        const bool own = integration.configured() && profile.value("currentProfileName", "") == "OBSReplay" && collection.value("currentSceneCollectionName", "") == "OBSReplay";
        update([&](Json& s) {
            s["connected"] = true; s["obs_running"] = true; s["owned"] = own;
            s["replay_active"] = replay.value("outputActive", false);
        });
        refresh_needed = false;
        next_refresh = Clock::now() + std::chrono::seconds(5);
        if (own) {
            const auto issue = device_issue();
            if (!issue.message.empty()) {
                if (issue.monitor && replay.value("outputActive", false)) {
                    try {
                        socket.request("StopReplayBuffer");
                        const bool active = socket.request("GetReplayBufferStatus").value("outputActive", false);
                        update([&](Json& s) { s["replay_active"] = active; });
                        if (active) throw std::runtime_error("Stop is pending");
                    } catch (...) { throw std::runtime_error("The selected display disconnected and OBS did not confirm a stop. Open OBS and stop capture immediately to avoid recording another display."); }
                }
                fail(issue.message);
            }
            const auto capture_issue = issue.message.empty() ? scene_issue() : issue.message;
            update([&](Json& s) { s["capture_ready"] = capture_issue.empty(); s["capture_issue"] = capture_issue; });
            if (!capture_issue.empty()) fail(capture_issue);
        } else {
            update([](Json& s) { s["capture_ready"] = false; s["capture_issue"] = "Select the OBSReplay profile and scene collection in OBS."; });
        }
        settle();
    }

    void connect_once() {
        if (!integration.configured()) throw std::runtime_error("Configure the OBSReplay integration in Settings first.");
        socket.connect(integration.credentials());
        refresh();
        failures = 0;
        next_connect = Clock::now() + std::chrono::seconds(2);
    }

    void ensure_connection(bool allow_launch) {
        if (!integration.configured()) throw std::runtime_error("Configure OBSReplay in Settings before starting capture.");
        bool running = obs_running();
        update([&](Json& s) { s["obs_running"] = running; });
        if (!running) {
            if (!allow_launch) throw std::runtime_error("OBS is closed. Start capture to launch your dedicated OBS session.");
            const auto value = settings();
            message("Launching the dedicated OBS session…");
            launch_obs(from_string(value.at("obs_path").get<std::string>()));
            const auto deadline = Clock::now() + std::chrono::seconds(25);
            std::string last_error;
            while (!stopping.load() && Clock::now() < deadline) {
                if (obs_running()) {
                    try { connect_once(); return; } catch (const std::exception& e) { last_error = e.what(); }
                }
                if (!pause(600)) break;
            }
            throw std::runtime_error("OBS opened but its controller did not become ready. Open OBS to check for a startup or plugin dialog. " + last_error);
        }
        if (!socket.connected()) connect_once();
        else refresh();
    }

    void require_owned() {
        events();
        refresh();
        events();
        if (!flag("owned")) throw std::runtime_error("OBS is using another profile or scene collection. Select both the OBSReplay profile and OBSReplay collection in OBS, then reconnect.");
    }

    void persist_intent(bool desired) {
        auto value = settings();
        value["replay_enabled"] = desired;
        store_settings(value);
    }

    void start() {
        ensure_connection(true);
        require_owned();
        require_capture_ready();
        if (!flag("replay_active")) socket.request("StartReplayBuffer");
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        do {
            const auto status = socket.request("GetReplayBufferStatus");
            if (status.value("outputActive", false)) {
                update([](Json& s) { s["replay_active"] = true; });
                persist_intent(true);
                message("Replay capture is running. New history builds from now.");
                return;
            }
        } while (Clock::now() < deadline && pause(150));
        throw std::runtime_error("OBS did not confirm that replay capture started. Check OBS for an encoder or capture error.");
    }

    void stop() {
        if (!obs_running()) {
            socket.close();
            update([](Json& s) { s["connected"] = false; s["owned"] = false; s["replay_active"] = false; s["obs_running"] = false; });
            persist_intent(false);
            message("Replay capture is stopped.");
            return;
        }
        ensure_connection(false);
        require_owned();
        if (flag("replay_active")) socket.request("StopReplayBuffer");
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        do {
            const auto status = socket.request("GetReplayBufferStatus");
            if (!status.value("outputActive", false)) {
                update([](Json& s) { s["replay_active"] = false; });
                persist_intent(false);
                message("Capture stopped. Unsaved replay history was discarded.");
                return;
            }
        } while (Clock::now() < deadline && pause(150));
        throw std::runtime_error("OBS did not confirm that replay capture stopped. Check OBS before assuming recording has stopped.");
    }

    void remember_clip(const fs::path& original, std::string app, bool sort, bool sound) {
        std::error_code error;
        if (!original.is_absolute() || !media_file(original) || !fs::is_regular_file(original, error))
            throw std::runtime_error("OBS reported a saved replay, but the video file is not available. Check the OBS output folder.");
        fs::path final = original;
        std::string warning;
        if (sort) {
            app = safe_name(app);
            const auto directory = original.parent_path() / from_string(app);
            fs::create_directories(directory, error);
            if (!error) {
                const auto stem = original.stem().wstring();
                bool moved{};
                for (unsigned i = 0; i != 1000; ++i) {
                    const auto suffix = i ? L" (" + std::to_wstring(i) + L")" : L"";
                    auto target = directory / (stem + suffix + original.extension().wstring());
                    if (MoveFileExW(original.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) { final = target; moved = true; break; }
                    if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
                }
                if (!moved) warning = "Saved in the original folder because app sorting failed.";
            } else warning = "Saved in the original folder because the app folder could not be created.";
        }
        const auto bytes = fs::file_size(final, error);
        if (error) throw std::runtime_error("OBS saved the replay, but its file details could not be read.");
        const auto text = path_string(final);
        Json clips;
        update([&](Json& s) {
            auto& list = s["clips"];
            for (auto it = list.begin(); it != list.end();) {
                if (it->value("path", "") == text) it = list.erase(it); else ++it;
            }
            list.insert(list.begin(), Json{{"path", text}, {"name", path_string(final.filename())}, {"app", app}, {"bytes", bytes}, {"saved_at", now_iso()}});
            trim_history(list);
            clips = list;
        });
        try { atomic_write(paths.data / L"clips.json", clips.dump(2)); }
        catch (...) { warning += " The recent-clips list could not be persisted; the video is saved."; }
        if (sound) save_feedback();
        message(warning.empty() ? "Saved " + path_string(final.filename()) : warning);
    }

    void events() {
        for (const auto& event : socket.events()) {
            const auto type = event.value("eventType", "");
            const auto data = event.value("eventData", Json::object());
            if (type == "CurrentProfileChanging" || type == "CurrentProfileChanged" || type == "CurrentSceneCollectionChanging" || type == "CurrentSceneCollectionChanged") {
                refresh_needed = true;
                update([](Json& s) { s["owned"] = false; });
            } else if (type == "ReplayBufferStateChanged") {
                update([&](Json& s) { s["replay_active"] = data.value("outputActive", false); });
            } else if (type == "ExitStarted") {
                update([](Json& s) { s["connected"] = false; s["owned"] = false; s["replay_active"] = false; });
            } else if (type == "ReplayBufferSaved" && flag("owned")) {
                const auto path = data.value("savedReplayPath", "");
                if (path.empty()) continue;
                if (pending_save) {
                    pending_save->confirmed = true;
                    pending_save->path = path;
                } else if (!pending_save) {
                    try { remember_clip(from_string(path), "", false, false); }
                    catch (const std::exception& e) { fail(e.what()); }
                }
            }
        }
    }

    void save(const std::string& app, bool before_settings_change = false) {
        ensure_connection(false);
        require_owned();
        if (!flag("replay_active")) throw std::runtime_error("Start replay capture before saving a replay.");
        if (before_settings_change) {
            if (!scene_issue().empty()) throw std::runtime_error("The current replay could not be backed up because the capture scene changed. Save it in OBS or stop capture before applying settings. OBS was left running.");
        } else require_capture_ready();
        const auto value = settings();
        events();
        if (!flag("owned")) throw std::runtime_error("OBS changed profiles before the save. Reconnect to check its current session.");
        pending_save = Save{app, value.value("sort_by_app", false), value.value("save_sound", false)};
        std::string request_error;
        try { socket.request("SaveReplayBuffer", Json::object(), 5000); }
        catch (const std::exception& e) { request_error = e.what(); }
        const auto deadline = Clock::now() + std::chrono::seconds(12);
        while (!stopping.load() && Clock::now() < deadline) {
            events();
            if (pending_save->confirmed) {
                const auto saved = *pending_save;
                const auto path = from_string(saved.path);
                std::error_code error;
                if (fs::is_regular_file(path, error)) {
                    pending_save.reset();
                    remember_clip(path, saved.app, saved.sort, saved.sound);
                    return;
                }
            }
            if (!socket.connected()) break;
            if (!pause(80)) break;
        }
        pending_save.reset();
        throw std::runtime_error("A saved video was not confirmed. Check the clips folder before saving again; this request will not be retried automatically." + (request_error.empty() ? std::string{} : " " + request_error));
    }

    void apply(const Json& payload, bool setup) {
        const auto previous = settings();
        Json value = payload.empty() ? previous : payload.contains("settings") ? payload.at("settings") : payload;
        validate_settings(value);
        const bool was_configured = integration.configured();
        const bool prepare = setup || !was_configured || integration.needs_update() || capture_changed(previous, value);
        const bool startup_changed = previous.value("start_at_login", false) != value.value("start_at_login", false);
        bool startup_applied{};
        bool restart{}, resume{};
        if (setup) stage("");
        try {
            bool own_session{};
            if (prepare && obs_running() && was_configured) {
                // The shared WebSocket login is ours once configured, so any OBS session should answer. If it can't be
                // reached, don't risk closing an active OBSReplay buffer without saving it.
                try { ensure_connection(false); events(); refresh(); own_session = flag("owned"); } catch (...) { socket.close(); throw; }
            }
            if (prepare && obs_running() && !own_session) {
                // Another OBS session (or one we can't verify) is open. Close it gracefully so setup can write the
                // dedicated profile; OBS's own exit confirmation still protects any stream or recording.
                stage("closing");
                message("Closing OBS so it can be set up…");
                socket.close();
                if (other_session_busy()) throw std::runtime_error("OBS is streaming or recording. Stop it in OBS, then try again. Nothing was changed.");
                close_all_obs();
                update([](Json& s) { s["connected"] = false; s["owned"] = false; s["replay_active"] = false; s["obs_running"] = false; });
            } else if (prepare && obs_running()) {
                stage("closing");
                if (socket.request("GetStreamStatus").value("outputActive", false) || socket.request("GetRecordStatus").value("outputActive", false))
                    throw std::runtime_error("Stop your stream or full recording before applying capture settings. OBS was left running.");
                resume = flag("replay_active");
                restart = true;
                if (resume) {
                    message("Pausing replay capture to apply settings…");
                    socket.request("StopReplayBuffer");
                    const auto deadline = Clock::now() + std::chrono::seconds(10);
                    while (socket.request("GetReplayBufferStatus").value("outputActive", false)) {
                        if (Clock::now() >= deadline || !pause(100)) throw std::runtime_error("OBS did not confirm a capture stop. Your settings were not changed.");
                    }
                }
                message("Closing OBS to apply settings…");
                socket.close();
                close_obs(from_string(previous.at("obs_path").get<std::string>()));
            }
            if (startup_changed || setup) { set_login_startup(paths, value.value("start_at_login", false)); startup_applied = true; }
            if (prepare) {
                socket.close();
                update([](Json& s) { s["connected"] = false; s["owned"] = false; s["replay_active"] = false; s["obs_running"] = false; });
                stage("configuring");
                message("Preparing your dedicated OBS profile…");
                value = integration.prepare(value);
                set_settings(value);
                update([](Json& s) { s["capture_ready"] = false; s["capture_issue"] = ""; });
            } else store_settings(value);
        } catch (...) {
            const auto cause = std::current_exception();
            bool rollback_failed{};
            if (startup_applied) try { set_login_startup(paths, previous.value("start_at_login", false)); } catch (...) { rollback_failed = true; }
            update([&](Json& s) { s["integration"] = integration.configured(); s["compression_update"] = integration.needs_update(); });
            if (restart) {
                try { ensure_connection(true); require_owned(); if (resume) start(); }
                catch (...) { throw std::runtime_error("Settings could not be applied, and OBS could not resume. Check OBS and choose Start capture to recover."); }
            }
            if (rollback_failed) throw std::runtime_error("Settings were not applied, and Windows could not restore the previous login startup entry. Review Start at Windows login and Apply again.");
            std::rethrow_exception(cause);
        }
        update([&](Json& s) { s["integration"] = integration.configured(); s["compression_update"] = integration.needs_update(); });
        if (prepare) {
            try { const auto options = integration.options(value); update([&](Json& s) { s["options"] = options; }); }
            catch (...) {}
        }
        if (restart) {
            message("Reopening OBS with your settings…");
            try { ensure_connection(true); require_owned(); if (resume) start(); }
            catch (const std::exception& e) { throw std::runtime_error(std::string("Settings were saved, but OBS could not resume. Choose Start capture to retry. ") + e.what()); }
        }
        if (prepare) update([](Json& s) { s["error"] = ""; });
        if (previous.value("clips_dir", "") != value.value("clips_dir", "")) discover_clips();
        message(restart ? resume ? "Settings applied. Replay capture resumed." : "Settings applied. OBS is ready." : prepare ? "OBS is configured. Start capture when you're ready." : "Settings saved.");
        next_connect = Clock::now();
        if (setup && payload.value("start", false) && !flag("replay_active")) {
            stage("launching");
            message("Opening OBS in the tray…");
            ensure_connection(true);
            stage("starting");
            message("Starting replay capture…");
            start();
        }
        if (setup) stage("done");
    }

    void reset() {
        if (obs_running()) throw std::runtime_error("Close OBS before resetting its integration. Your clips will remain untouched.");
        auto value = settings();
        integration.reset();
        socket.close();
        update([](Json& s) { s["integration"] = false; s["compression_update"] = false; s["connected"] = false; s["owned"] = false; s["replay_active"] = false; s["capture_ready"] = false; s["capture_issue"] = ""; });
        value["replay_enabled"] = false;
        value["start_at_login"] = false;
        set_login_startup(paths, false);
        store_settings(value);
        message("OBSReplay integration reset. Your saved clips are unchanged.");
    }

    // Tray "Close OBSReplay and OBS": stop the buffer, close the OBSReplay session of OBS, then mark the state so the
    // window quits. The saved "capture on" choice is kept, so capture resumes at the next launch.
    void shutdown() {
        if (obs_running()) {
            try { ensure_connection(false); } catch (...) {}
            if (flag("connected")) {
                if (!flag("owned")) throw std::runtime_error("OBS is running a different session, so it was left open. Close it from OBS.");
                if (socket.request("GetStreamStatus").value("outputActive", false) || socket.request("GetRecordStatus").value("outputActive", false))
                    throw std::runtime_error("OBS is streaming or recording, so it was left open.");
                if (flag("replay_active")) {
                    socket.request("StopReplayBuffer");
                    const auto deadline = Clock::now() + std::chrono::seconds(10);
                    while (socket.request("GetReplayBufferStatus").value("outputActive", false))
                        if (Clock::now() >= deadline || !pause(100)) throw std::runtime_error("OBS did not confirm a capture stop, so it was left open.");
                }
                socket.close();
            }
            close_obs(from_string(settings().at("obs_path").get<std::string>()));
        }
        update([](Json& s) { s["connected"] = false; s["owned"] = false; s["replay_active"] = false; s["obs_running"] = false; s["quit"] = true; });
    }

    void perform(const Action& action) {
        if (action.name == "setup" || action.name == "apply") apply(action.payload, action.name == "setup");
        else if (action.name == "install_obs") install_obs();
        else if (action.name == "locate_obs") { use_obs(from_string(action.payload.value("path", ""))); message("Using OBS Studio " + std::string(state_value("obs_version")) + "."); }
        else if (action.name == "start") start();
        else if (action.name == "stop") stop();
        else if (action.name == "shutdown") shutdown();
        else if (action.name == "save") save(action.app);
        else if (action.name == "reset") reset();
        else if (action.name == "reconnect") {
            socket.close(); ensure_connection(false); message(flag("owned") ? "Connected to your OBSReplay session." : "Connected to a different OBS session. Select OBSReplay in OBS to control capture.");
        } else if (action.name == "refresh_options") {
            const auto options = integration.options(settings());
            update([&](Json& s) { s["options"] = options; }); message("Devices and encoders refreshed.");
        } else if (action.name == "open_folder") {
            const auto directory = from_string(settings().at("clips_dir").get<std::string>());
            fs::create_directories(directory); open_path(directory);
        } else if (action.name == "open_obs") {
            if (!obs_running()) launch_obs(from_string(settings().at("obs_path").get<std::string>()));
            else {
                bool shown{};
                EnumWindows([](HWND window, LPARAM param) -> BOOL {
                    DWORD id{}; GetWindowThreadProcessId(window, &id);
                    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
                    if (!process) return TRUE;
                    wchar_t path[32768]{}; DWORD size = 32768;
                    const bool obs = QueryFullProcessImageNameW(process, 0, path, &size) && _wcsicmp(fs::path(path).filename().c_str(), L"obs64.exe") == 0;
                    CloseHandle(process);
                    wchar_t title[256]{}; GetWindowTextW(window, title, 256);
                    if (obs && std::wstring_view(title).starts_with(L"OBS ")) {
                        ShowWindowAsync(window, SW_RESTORE); SetForegroundWindow(window);
                        *reinterpret_cast<bool*>(param) = true; return FALSE;
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&shown));
                if (!shown) throw std::runtime_error("OBS is running but its window is not ready. Open OBS from its tray icon.");
            }
        } else if (action.name == "open_clip" || action.name == "reveal_clip" || action.name == "delete_clip") {
            const auto path = action.payload.value("path", "");
            bool known{};
            { std::lock_guard lock(mutex); for (const auto& clip : state.at("clips")) if (clip.value("path", "") == path) { known = true; break; } }
            const auto file = from_string(path);
            if (!known || !media_file(file)) throw std::runtime_error("Choose a file from your confirmed recent clips.");
            if (action.name == "delete_clip") {
                std::error_code error;
                if (!fs::is_regular_file(file, error)) {
                    reconcile_clips();
                    throw std::runtime_error("This replay is no longer available or cannot be deleted.");
                }
                if (!fs::remove(file, error) || error)
                    throw std::runtime_error("Windows could not delete this replay. Close it in other apps and check your folder permissions.");
                reconcile_clips();
                message("Replay deleted.");
            } else open_path(file, action.name == "reveal_clip");
        } else if (action.name == "help") open_help(action.payload.value("topic", "obs"));
    }

    void background_connection() {
        if (!socket.connected()) update([](Json& s) { s["connected"] = false; s["owned"] = false; s["replay_active"] = false; });
        const bool running = obs_running();
        update([&](Json& s) { s["obs_running"] = running; });
        if (!running) {
            if (socket.connected()) socket.close();
            update([](Json& s) { s["connected"] = false; s["owned"] = false; s["replay_active"] = false; });
            next_connect = Clock::now() + std::chrono::seconds(2);
            settle(); return;
        }
        if (!integration.configured()) return;
        if (socket.connected()) {
            events();
            if (refresh_needed || Clock::now() >= next_refresh) refresh();
        } else if (Clock::now() >= next_connect) {
            update([](Json& s) { s["connected"] = false; s["owned"] = false; s["replay_active"] = false; });
            try { connect_once(); }
            catch (const std::exception& e) {
                socket.close();
                next_connect = Clock::now() + std::chrono::seconds(std::min(15u, 2u + ++failures * 2u));
                update([&](Json& s) { s["message"] = std::string("OBS is running; waiting for its controller. ") + e.what(); });
            }
        }
        settle();
    }

    void run() {
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try { initialize(); }
        catch (const std::exception& e) { fail(e.what()); update([](Json& s) { s["busy"] = false; s["operation"] = ""; }); }
        auto next_poll = Clock::now();
        while (!stopping.load()) {
            std::optional<Action> action;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping.load() || !queue.empty(); });
                if (stopping.load()) break;
                if (!queue.empty()) { action = std::move(queue.front()); queue.pop_front(); state["operation"] = action->name; }
            }
            if (action) {
                update([](Json& s) { s["error"] = ""; });
                try { perform(*action); }
                catch (const std::exception& e) { fail(e.what()); }
                catch (...) { fail("The action could not be completed. Reconnect and try again."); }
                update([&](Json& s) {
                    s["busy"] = !queue.empty(); s["operation"] = queue.empty() ? "" : queue.front().name;
                    s["revision"] = s.at("revision").get<uint64_t>() + 1;
                });
                settle();
            }
            if (stopping.load()) break;
            try {
                events();
                if (Clock::now() >= next_poll) {
                    reconcile_clips();
                    if (Clock::now() >= next_detect) { detect_obs(); next_detect = Clock::now() + std::chrono::seconds(3); }
                    background_connection();
                    next_poll = Clock::now() + std::chrono::seconds(1);
                }
            } catch (const std::exception& e) {
                socket.close();
                update([](Json& s) { s["connected"] = false; s["owned"] = false; s["replay_active"] = false; });
                fail(e.what());
                next_connect = Clock::now() + std::chrono::seconds(2);
                settle();
            }
        }
        socket.close();
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

Controller::Controller(Paths paths) : impl_(std::make_unique<Impl>(std::move(paths))) {}
Controller::~Controller() = default;
std::string Controller::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->state.dump(); }
bool Controller::knows_clip(const std::string& path) const {
    std::lock_guard lock(impl_->mutex);
    const auto& clips = impl_->state.at("clips");
    return std::any_of(clips.begin(), clips.end(), [&](const Json& clip) { return clip.value("path", "") == path; });
}

std::expected<bool, std::string> Controller::add_clip(const std::string& path, const std::string& app) {
    const auto file = from_string(path);
    std::error_code error;
    if (!file.is_absolute() || !media_file(file) || !fs::is_regular_file(file, error)) return std::unexpected("The exported video could not be added to recent clips.");
    const auto bytes = fs::file_size(file, error);
    const auto text = path_string(file);
    Json clips;
    impl_->update([&](Json& s) {
        auto& list = s["clips"];
        for (auto it = list.begin(); it != list.end();) { if (it->value("path", "") == text) it = list.erase(it); else ++it; }
        list.insert(list.begin(), Json{{"path", text}, {"name", path_string(file.filename())}, {"app", app}, {"bytes", error ? 0 : bytes}, {"saved_at", now_iso()}, {"edited", true}});
        trim_history(list);
        clips = list;
    });
    try { atomic_write(impl_->paths.data / L"clips.json", clips.dump(2)); }
    catch (...) { return std::unexpected("The edited video was saved, but recent clips could not be updated."); }
    return true;
}

std::expected<bool, std::string> Controller::dispatch(std::string action, std::string payload) {
    static const std::set<std::string> allowed{"setup", "apply", "install_obs", "locate_obs", "start", "stop", "shutdown", "save", "reconnect", "reset", "open_folder", "open_obs", "open_clip", "reveal_clip", "delete_clip", "help", "refresh_options"};
    if (!allowed.contains(action)) return std::unexpected("Unknown controller action.");
    if (payload.size() > 128 * 1024) return std::unexpected("The submitted settings are too large.");
    Json data;
    try { data = Json::parse(payload.empty() ? "{}" : payload); }
    catch (...) { return std::unexpected("The action contains invalid JSON."); }
    if (!data.is_object()) return std::unexpected("The action payload must be an object.");
    std::string app;
    if (action == "save") { try { app = foreground_app(); } catch (...) { app = "Desktop"; } }
    auto& s = *impl_;
    {
        std::lock_guard lock(s.mutex);
        if (s.stopping.load()) return std::unexpected("OBSReplay is closing.");
        if (s.state.value("busy", false)) return std::unexpected("Wait for the current action to finish.");
        if (s.queue.size() >= 4) return std::unexpected("The controller is busy. Try again shortly.");
        s.queue.push_back({action, std::move(data), std::move(app)});
        s.state["busy"] = true; s.state["operation"] = action;
        s.state["error"] = ""; s.state["message"] = action == "save" ? "Waiting for OBS to save your replay…" : "Working…";
        if (action == "install_obs") s.state["install_stage"] = "preparing";
        if (action == "setup") s.state["setup_stage"] = "";
        s.state["revision"] = s.state.at("revision").get<uint64_t>() + 1;
    }
    s.wake.notify_one();
    return true;
}
}
