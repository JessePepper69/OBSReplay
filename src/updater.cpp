#include "updater.hpp"
#include "log.hpp"
#include "build_info.hpp"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>

namespace replay {
namespace {
constexpr char repository[] = "JessePepper69/OBSReplay";
constexpr std::int64_t launch_delay = 10, fresh_for = 3600, daily = 86400, retry_after = 3600;
constexpr std::size_t notes_limit = 30000;

std::wstring environment(const wchar_t* name) {
    std::wstring value(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    value.resize(length < value.size() ? length : 0);
    return value;
}
std::int64_t now_seconds() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// "v1.2.3", "1.2.3" or "1.2": three numbers. A suffix after "-" or "+" is ignored.
std::optional<std::array<int, 3>> parse_version(std::string_view text) {
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.remove_prefix(1);
    std::array<int, 3> version{};
    std::size_t part = 0;
    bool digits = false;
    for (const char c : text) {
        if (c >= '0' && c <= '9') { if (version[part] > 99999) return std::nullopt; version[part] = version[part] * 10 + (c - '0'); digits = true; }
        else if (c == '.' && digits && part < 2) { ++part; digits = false; }
        else if ((c == '-' || c == '+') && digits) break;
        else return std::nullopt;
    }
    if (!digits) return std::nullopt;
    return version;
}
std::string version_text(const std::array<int, 3>& v) { return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]); }

struct Handle {
    HINTERNET value = nullptr;
    ~Handle() { if (value) WinHttpCloseHandle(value); }
    operator HINTERNET() const { return value; }
};
struct Response { DWORD status = 0; std::uint64_t length = 0; };

// One GET. Redirects are followed (never from https to http). `sink` gets each piece of the body and stops the
// transfer by returning false. Throws when the server cannot be reached.
Response http_get(const std::string& url, const wchar_t* headers, bool allow_plain, const std::function<bool(const char*, DWORD, std::uint64_t)>& sink) {
    const auto address = wide(url);
    std::wstring host(256, L'\0'), path(4096, L'\0'), extra(4096, L'\0');
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host.data(); parts.dwHostNameLength = static_cast<DWORD>(host.size());
    parts.lpszUrlPath = path.data(); parts.dwUrlPathLength = static_cast<DWORD>(path.size());
    parts.lpszExtraInfo = extra.data(); parts.dwExtraInfoLength = static_cast<DWORD>(extra.size());
    if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts)) throw std::runtime_error("The update address is not valid.");
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && !allow_plain) throw std::runtime_error("The update address is not secure.");
    host.resize(parts.dwHostNameLength);
    path.resize(parts.dwUrlPathLength);
    extra.resize(parts.dwExtraInfoLength);
    const auto agent = wide(std::string("OBSReplay/") + OBSREPLAY_VERSION);
    const Handle session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) throw std::runtime_error("Windows could not start a connection.");
    WinHttpSetTimeouts(session, 8000, 8000, 15000, 20000);
    const Handle connection{WinHttpConnect(session, host.c_str(), parts.nPort, 0)};
    if (!connection) throw std::runtime_error("Windows could not start a connection.");
    const Handle request{WinHttpOpenRequest(connection, L"GET", (path + extra).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)};
    if (!request) throw std::runtime_error("Windows could not start a connection.");
    if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr))
        throw std::runtime_error("GitHub could not be reached. Check your internet connection.");
    Response response;
    DWORD size = sizeof(response.status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &size, WINHTTP_NO_HEADER_INDEX);
    wchar_t length[32]{};
    size = sizeof(length);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &size, WINHTTP_NO_HEADER_INDEX)) response.length = _wcstoui64(length, nullptr, 10);
    if (response.status != 200) return response;
    std::string buffer(64 * 1024, '\0');
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) throw std::runtime_error("The connection dropped. Check your internet connection.");
        if (!read) break;
        if (!sink(buffer.data(), read, response.length)) break;
    }
    return response;
}

struct Sha256 {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 || BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("SHA-256 is unavailable.");
    }
    ~Sha256() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    void add(const char* data, DWORD size) { if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), size, 0) < 0) throw std::runtime_error("SHA-256 failed."); }
    std::string hex() {
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) throw std::runtime_error("SHA-256 failed.");
        std::string text;
        for (const unsigned char byte : digest) { text += "0123456789abcdef"[byte >> 4]; text += "0123456789abcdef"[byte & 15]; }
        return text;
    }
};
std::string lower(std::string text) { std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return text; }
}

struct Updater::Impl {
    fs::path file, folder;
    std::array<int, 3> current{};
    std::string api, release_page;
    bool overridden = false;  // a check: the address comes from OBSREPLAY_UPDATE_URL
    bool network = true;      // false in an isolated run with no address: the app never contacts GitHub from a check
    std::int64_t started = now_seconds();

    mutable std::mutex mutex;
    bool enabled = true;
    std::string status = "idle", error, error_stage, skipped, updated_from, previous;
    Json latest;              // the newest release seen: {version, notes, page, url, size, digest, published}
    std::int64_t checked_at = 0, attempted_at = 0;
    std::uint64_t received = 0, total = 0;
    bool just_updated = false, launch_check_done = false, cleaned = false;
    HANDLE installer = nullptr;

    std::atomic<unsigned> revision{0};
    std::atomic<bool> working{false}, cancelled{false};
    std::mutex callback_mutex;
    bool alive = true;  // guarded by callback_mutex

    void changed() { ++revision; }
    bool newer() const { const auto v = latest.is_object() ? parse_version(latest.value("version", std::string{})) : std::nullopt; return v && *v > current; }
    void save() const {
        try {
            const Json value{{"enabled", enabled}, {"skipped", skipped}, {"checked_at", checked_at}, {"latest", latest}, {"updated_from", updated_from}};
            atomic_write(file, value.dump(2, ' ', false, Json::error_handler_t::replace));
        } catch (const std::exception& e) { log_warn("Update state not saved: error={}", quoted_value(e.what())); }
    }
    void load() {
        try {
            const auto value = Json::parse(read_text(file));
            enabled = value.value("enabled", true);
            skipped = value.value("skipped", std::string{});
            checked_at = value.value("checked_at", std::int64_t{0});
            updated_from = value.value("updated_from", std::string{});
            if (value.contains("latest") && value["latest"].is_object() && parse_version(value["latest"].value("version", std::string{}))) latest = value["latest"];
        } catch (...) {}
    }

    // The release as GitHub describes it, reduced to what the app uses. Throws when it is not usable.
    Json read_release(const std::string& body) const {
        const auto release = Json::parse(body);
        const auto version = parse_version(release.value("tag_name", std::string{}));
        if (!version) throw std::runtime_error("The latest release has no version number.");
        Json result{{"version", version_text(*version)}, {"published", release.value("published_at", std::string{})}, {"page", release_page}, {"url", ""}, {"size", 0}, {"digest", ""}};
        if (const auto page = release.value("html_url", std::string{}); page.starts_with("https://github.com/")) result["page"] = page;
        auto notes = release.contains("body") && release["body"].is_string() ? release["body"].get<std::string>() : std::string{};
        if (notes.size() > notes_limit) { auto cut = notes_limit; while (cut > 0 && (static_cast<unsigned char>(notes[cut]) & 0xC0) == 0x80) --cut; notes.resize(cut); notes += "\n\n…"; }
        result["notes"] = notes;
        const Json* chosen = nullptr;
        if (release.contains("assets") && release["assets"].is_array()) {
            for (const auto& asset : release["assets"]) { if (lower(asset.value("name", std::string{})) == "obsreplay.exe") { chosen = &asset; break; } }
            if (!chosen) for (const auto& asset : release["assets"]) { if (lower(asset.value("name", std::string{})).ends_with(".exe")) { chosen = &asset; break; } }
        }
        if (chosen) {
            const auto url = chosen->value("browser_download_url", std::string{});
            const bool trusted = overridden ? url.starts_with("http://127.0.0.1:") || url.starts_with("https://") : url.starts_with(std::string("https://github.com/") + repository + "/releases/download/");
            if (trusted) {
                result["url"] = url;
                result["size"] = chosen->value("size", std::uint64_t{0});
                if (chosen->contains("digest") && (*chosen)["digest"].is_string()) result["digest"] = lower((*chosen)["digest"].get<std::string>());
            }
        }
        return result;
    }

    void run_check(bool manual) {
        struct Done { std::atomic<bool>& flag; ~Done() { flag = false; } } done{working};
        std::string problem;
        try {
            if (!network) throw std::runtime_error("Update checks are off in this test run.");
            std::string body;
            const auto response = http_get(api, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n", overridden, [&](const char* data, DWORD size, std::uint64_t) { body.append(data, size); return body.size() < 4 * 1024 * 1024; });
            std::lock_guard lock(mutex);
            attempted_at = now_seconds();
            if (response.status == 404) { latest = nullptr; }  // no release published yet
            else if (response.status == 403 || response.status == 429) throw std::runtime_error("GitHub is limiting update checks from your network. Try again in an hour.");
            else if (response.status != 200) throw std::runtime_error("GitHub answered with an error (" + std::to_string(response.status) + "). Try again later.");
            else latest = read_release(body);
            checked_at = attempted_at;
            status = newer() ? "available" : "current";
            error.clear(); error_stage.clear();
            log_info("Update check: manual={} latest={} current={} available={}", manual, latest.is_object() ? latest.value("version", std::string{}) : std::string("none"), version_text(current), newer());
            save();
        } catch (const std::exception& e) { problem = e.what(); }
        if (!problem.empty()) {
            std::lock_guard lock(mutex);
            attempted_at = now_seconds();
            log_warn("Update check failed: manual={} error={}", manual, quoted_value(problem));
            // A scheduled check fails quietly: what was known before stays on screen.
            if (manual) { status = "error"; error = problem; error_stage = "check"; }
            else status = newer() ? "available" : latest.is_object() ? "current" : "idle";
        }
        changed();
    }

    void run_download(const std::function<void(const fs::path&)>& ready) {
        Json release;
        { std::lock_guard lock(mutex); release = latest; }
        const auto version = release.value("version", std::string{});
        const auto part = folder / wide("OBSReplay-" + version + ".exe.part"), target = folder / wide("OBSReplay-" + version + ".exe");
        std::string problem;
        bool stopped = false;
        try {
            std::error_code ignored;
            fs::remove_all(folder, ignored);
            fs::create_directories(folder);
            const HANDLE out = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (out == INVALID_HANDLE_VALUE) throw std::runtime_error("The update could not be saved to " + path_string(folder) + ".");
            struct Close { HANDLE handle; ~Close() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } } close{out};
            Sha256 hash;
            const std::uint64_t expected = release.value("size", std::uint64_t{0});
            std::uint64_t written = 0;
            auto painted = std::chrono::steady_clock::now();
            log_info("Update download started: version={} bytes={}", version, expected);
            const auto response = http_get(release.value("url", std::string{}), L"Accept: application/octet-stream\r\n", overridden, [&](const char* data, DWORD size, std::uint64_t length) {
                if (cancelled) return false;
                DWORD saved = 0;
                if (!WriteFile(out, data, size, &saved, nullptr) || saved != size) throw std::runtime_error("The update could not be saved. Check the free space on your system drive.");
                hash.add(data, size);
                written += size;
                if (written > 512ull * 1024 * 1024) throw std::runtime_error("The download is larger than an OBSReplay update can be.");
                if (const auto now = std::chrono::steady_clock::now(); now - painted > std::chrono::milliseconds(80)) {
                    painted = now;
                    { std::lock_guard lock(mutex); received = written; total = expected ? expected : length; }
                    changed();
                }
                return true;
            });
            CloseHandle(out);
            close.handle = INVALID_HANDLE_VALUE;
            if (cancelled) stopped = true;
            else {
                if (response.status != 200) throw std::runtime_error("GitHub did not send the update (" + std::to_string(response.status) + "). Try again later.");
                { std::lock_guard lock(mutex); status = "verifying"; received = written; total = written; }
                changed();
                if (expected && written != expected) throw std::runtime_error("The download was incomplete. Try again.");
                const auto digest = release.value("digest", std::string{});
                const auto actual = hash.hex();
                if (digest.starts_with("sha256:") && digest.substr(7) != actual) throw std::runtime_error("The download did not match the file GitHub published, so it was deleted. Try again.");
                if (!MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) throw std::runtime_error("The update could not be saved to " + path_string(folder) + ".");
                // The file must be the OBSReplay it claims to be: same version in its own resources.
                const auto inside = file_version(target);
                if (!inside || version_text(*inside) != version) throw std::runtime_error("The file in this release is not OBSReplay " + version + ". Download it from the releases page instead.");
                log_info("Update download checked: version={} bytes={} sha256={} digest_published={}", version, written, actual, digest.starts_with("sha256:"));
            }
        } catch (const std::exception& e) { problem = e.what(); }
        if (stopped || !problem.empty()) {
            std::error_code ignored;
            fs::remove(part, ignored);
            fs::remove(target, ignored);
            std::lock_guard lock(mutex);
            if (stopped) { status = "available"; log_info("Update download cancelled"); }
            else { status = "error"; error = problem; error_stage = "download"; log_warn("Update download failed: error={}", quoted_value(problem)); }
            received = total = 0;
        }
        working = false;
        changed();
        if (!stopped && problem.empty()) { std::lock_guard lock(callback_mutex); if (alive) ready(target); }
    }
};

Updater::Updater(const Paths& paths) : impl_(std::make_shared<Impl>()) {
    auto& i = *impl_;
    i.file = paths.data / L"updates.json";
    i.folder = paths.data / L"updates";
    i.current = parse_version(OBSREPLAY_VERSION).value_or(std::array<int, 3>{});
    i.release_page = std::string("https://github.com/") + repository + "/releases/latest";
    i.api = std::string("https://api.github.com/repos/") + repository + "/releases/latest";
    // Checks of the app itself (an isolated data folder) never reach GitHub. They may name a local stand-in for it
    // and the version this build should pretend to be.
    if (!environment(L"OBSREPLAY_DATA_DIR").empty()) {
        const auto address = utf8(environment(L"OBSREPLAY_UPDATE_URL"));
        i.network = !address.empty();
        i.overridden = true;
        if (i.network) i.api = address;
        if (const auto as = parse_version(utf8(environment(L"OBSREPLAY_UPDATE_AS")))) i.current = *as;
    }
    i.load();
    if (!i.updated_from.empty()) {
        // The last run handed over to an installer. A higher version now means it worked.
        const auto before = parse_version(i.updated_from);
        if (before && i.current > *before) { i.just_updated = true; i.previous = i.updated_from; log_info("Updated: from={} to={}", i.updated_from, version_text(i.current)); }
        else log_warn("Update did not install: still={}", version_text(i.current));
        i.updated_from.clear();
        i.save();
    }
    if (i.newer()) i.status = "available";
    else if (i.latest.is_object()) i.status = "current";
}
Updater::~Updater() {
    impl_->cancelled = true;
    std::lock_guard lock(impl_->callback_mutex);
    impl_->alive = false;
}

Json Updater::state() const {
    const auto& i = *impl_;
    std::lock_guard lock(i.mutex);
    Json latest = nullptr;
    if (i.latest.is_object()) latest = {{"version", i.latest.value("version", std::string{})}, {"notes", i.latest.value("notes", std::string{})}, {"page", i.latest.value("page", i.release_page)},
        {"size", i.latest.value("size", std::uint64_t{0})}, {"published", i.latest.value("published", std::string{})}, {"downloadable", !i.latest.value("url", std::string{}).empty()}};
    const bool available = i.newer();
    return {{"enabled", i.enabled}, {"status", i.status}, {"current", version_text(i.current)}, {"latest", latest}, {"available", available},
        {"skipped", available && i.latest.value("version", std::string{}) == i.skipped}, {"received", i.received}, {"total", i.total}, {"error", i.error}, {"error_stage", i.error_stage},
        {"checked_at", i.checked_at}, {"just_updated", i.just_updated}, {"previous", i.previous}, {"releases", i.release_page}};
}
unsigned Updater::revision() const { return impl_->revision; }
std::string Updater::offered() const {
    const auto& i = *impl_;
    std::lock_guard lock(i.mutex);
    const auto version = i.latest.is_object() ? i.latest.value("version", std::string{}) : std::string{};
    return i.newer() && version != i.skipped ? version : std::string{};
}

void Updater::check(bool manual) {
    auto& i = *impl_;
    if (i.working.exchange(true)) return;
    {
        std::lock_guard lock(i.mutex);
        if (i.status == "installing") { i.working = false; return; }
        if (manual || !i.newer()) i.status = "checking";
        i.error.clear(); i.error_stage.clear();
    }
    i.changed();
    std::thread([impl = impl_, manual] { impl->run_check(manual); }).detach();
}

void Updater::tick() {
    auto& i = *impl_;
    const auto now = now_seconds();
    bool due = false;
    {
        std::lock_guard lock(i.mutex);
        if (i.installer && WaitForSingleObject(i.installer, 0) == WAIT_OBJECT_0) {
            // The installer closes this app before it copies anything. Still running after it ended: it did not install.
            DWORD code = 0;
            GetExitCodeProcess(i.installer, &code);
            CloseHandle(i.installer);
            i.installer = nullptr;
            log_warn("Update installer ended without replacing this copy: exit_code={}", code);
            i.status = "error"; i.error = "The installer closed before it finished. Try again, or download the update from the releases page."; i.error_stage = "download";
            i.updated_from.clear();
            i.save();
            i.changed();
        }
        if (now - i.started < launch_delay || i.working) return;
        if (!i.cleaned && i.status != "installing") { i.cleaned = true; std::error_code ignored; fs::remove_all(i.folder, ignored); }
        if (!i.enabled || !i.network || i.status == "installing" || i.status == "downloading" || i.status == "verifying") return;
        if (!i.launch_check_done) { i.launch_check_done = true; due = now - i.checked_at >= fresh_for; }
        else due = now - i.checked_at >= daily && now - i.attempted_at >= retry_after;
    }
    if (due) check(false);
}

void Updater::skip(bool skipped) {
    auto& i = *impl_;
    {
        std::lock_guard lock(i.mutex);
        if (!i.newer()) return;
        i.skipped = skipped ? i.latest.value("version", std::string{}) : std::string{};
        log_info("Update {}: version={}", skipped ? "skipped" : "no longer skipped", i.latest.value("version", std::string{}));
        i.save();
    }
    i.changed();
}
void Updater::set_enabled(bool enabled) {
    auto& i = *impl_;
    { std::lock_guard lock(i.mutex); i.enabled = enabled; log_info("Automatic update checks: enabled={}", enabled); i.save(); }
    i.changed();
}

std::expected<bool, std::string> Updater::start(std::function<void(const fs::path&)> ready) {
    auto& i = *impl_;
    {
        std::lock_guard lock(i.mutex);
        if (i.status == "downloading" || i.status == "verifying" || i.status == "installing") return true;
        if (!i.newer()) return std::unexpected("There is no update to install.");
        if (i.latest.value("url", std::string{}).empty()) return std::unexpected("This release has no OBSReplay.exe to download. Open the releases page instead.");
    }
    if (i.working.exchange(true)) return std::unexpected("OBSReplay is still checking for updates. Try again in a moment.");
    {
        std::lock_guard lock(i.mutex);
        i.status = "downloading"; i.error.clear(); i.error_stage.clear(); i.received = 0; i.total = i.latest.value("size", std::uint64_t{0});
    }
    i.cancelled = false;
    i.changed();
    std::thread([impl = impl_, ready = std::move(ready)] { impl->run_download(ready); }).detach();
    return true;
}
void Updater::cancel() { impl_->cancelled = true; }

void Updater::install(const fs::path& file) {
    auto& i = *impl_;
    std::wstring command = L"\"" + file.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    {
        // Written first: the new version reads it on its first start to say what it replaced.
        std::lock_guard lock(i.mutex);
        i.updated_from = version_text(i.current);
        i.save();
    }
    if (!CreateProcessW(file.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, file.parent_path().c_str(), &startup, &process)) {
        const auto code = GetLastError();
        { std::lock_guard lock(i.mutex); i.updated_from.clear(); i.save(); }
        log_error("Update installer did not start: code={}", code);
        fail("Windows could not start the installer. Try again, or download the update from the releases page.");
        return;
    }
    CloseHandle(process.hThread);
    log_info("Update installer started: file={}", quoted_value(path_string(file)));
    { std::lock_guard lock(i.mutex); i.status = "installing"; i.installer = process.hProcess; }
    i.changed();
}
void Updater::fail(const std::string& error) {
    auto& i = *impl_;
    { std::lock_guard lock(i.mutex); i.status = "error"; i.error = error; i.error_stage = "download"; }
    i.changed();
}
void Updater::seen() {
    auto& i = *impl_;
    { std::lock_guard lock(i.mutex); if (!i.just_updated) return; i.just_updated = false; }
    i.changed();
}
}
