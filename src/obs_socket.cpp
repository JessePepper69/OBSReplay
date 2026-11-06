#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include "obs_socket.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace replay {
namespace {
void require_http(bool ok, const char* operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + " failed (Windows " + std::to_string(GetLastError()) + ").");
}
}
struct ObsSocket::Impl {
    HINTERNET session{}, connection{};
    std::atomic<HINTERNET> socket{};
    std::atomic<bool> alive{};
    std::thread reader;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Json> hello;
    bool identified{};
    std::string waiting;
    std::optional<Json> response;
    std::deque<Json> incoming;
    uint64_t sequence{};

    void send(const Json& message) {
        const auto text = message.dump();
        const auto handle = socket.load();
        if (!handle || !alive.load()) throw std::runtime_error("OBS is disconnected.");
        const auto result = WinHttpWebSocketSend(handle, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            const_cast<char*>(text.data()), static_cast<DWORD>(text.size()));
        if (result != NO_ERROR) throw std::runtime_error("Couldn't send the OBS request (Windows " + std::to_string(result) + ").");
    }

    void receive() {
        try {
            std::string message;
            char buffer[16384];
            while (alive.load()) {
                DWORD count{};
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
                const auto handle = socket.load();
                if (!handle) break;
                const auto result = WinHttpWebSocketReceive(handle, buffer, sizeof(buffer), &count, &type);
                if (result != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
                if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE && type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) break;
                if (message.size() + count > 1024 * 1024) break;
                message.append(buffer, count);
                if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) continue;
                auto packet = Json::parse(message);
                message.clear();
                const auto op = packet.at("op").get<int>();
                auto data = packet.at("d");
                {
                    std::lock_guard lock(mutex);
                    if (op == 0) hello = std::move(data);
                    else if (op == 2) identified = true;
                    else if (op == 7 && data.value("requestId", "") == waiting) response = std::move(data);
                    else if (op == 5) {
                        if (incoming.size() == 128) incoming.pop_front();
                        incoming.push_back(std::move(data));
                    }
                }
                changed.notify_all();
            }
        } catch (...) {}
        alive.store(false);
        changed.notify_all();
    }
};

ObsSocket::ObsSocket() : impl_(std::make_unique<Impl>()) {}
ObsSocket::~ObsSocket() { close(); }

void ObsSocket::close() {
    auto& s = *impl_;
    s.alive.store(false);
    if (const auto handle = s.socket.exchange(nullptr)) WinHttpCloseHandle(handle);
    s.changed.notify_all();
    if (s.reader.joinable()) s.reader.join();
    if (s.connection) { WinHttpCloseHandle(s.connection); s.connection = nullptr; }
    if (s.session) { WinHttpCloseHandle(s.session); s.session = nullptr; }
    std::lock_guard lock(s.mutex);
    s.hello.reset(); s.identified = false; s.response.reset(); s.waiting.clear(); s.incoming.clear();
}

void ObsSocket::connect(const Credentials& credentials) {
    close();
    auto& s = *impl_;
    HINTERNET request{};
    try {
        s.session = WinHttpOpen(L"OBSReplay/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
        require_http(s.session != nullptr, "Opening local OBS transport");
        require_http(WinHttpSetTimeouts(s.session, 1500, 1500, 3000, 3000), "Setting transport timeouts");
        s.connection = WinHttpConnect(s.session, L"127.0.0.1", credentials.port, 0);
        require_http(s.connection != nullptr, "Connecting to OBS");
        request = WinHttpOpenRequest(s.connection, L"GET", L"/", nullptr, nullptr, nullptr, 0);
        require_http(request != nullptr, "Opening OBS handshake");
        require_http(WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0), "Upgrading OBS transport");
        const auto header = L"Sec-WebSocket-Protocol: obswebsocket.json\r\n";
        require_http(WinHttpSendRequest(request, header, static_cast<DWORD>(-1), nullptr, 0, 0, 0), "Sending OBS handshake");
        require_http(WinHttpReceiveResponse(request, nullptr), "Receiving OBS handshake");
        auto socket = WinHttpWebSocketCompleteUpgrade(request, 0);
        require_http(socket != nullptr, "Completing OBS handshake");
        s.socket.store(socket);
        WinHttpCloseHandle(request); request = nullptr;
        s.alive.store(true);
        s.reader = std::thread([&s] { s.receive(); });
        Json hello;
        {
            std::unique_lock lock(s.mutex);
            if (!s.changed.wait_for(lock, std::chrono::seconds(5), [&] { return s.hello.has_value() || !s.alive.load(); }) || !s.hello)
                throw std::runtime_error("OBS did not complete its WebSocket greeting.");
            hello = *s.hello;
        }
        if (hello.value("rpcVersion", 0) < 1) throw std::runtime_error("This OBS WebSocket version is unsupported. Update OBS Studio.");
        Json identify{{"rpcVersion", 1}, {"eventSubscriptions", 67}};
        if (!hello.contains("authentication"))
            throw std::runtime_error("OBS WebSocket authentication is disabled. Close OBS and run setup again.");
        const auto& auth = hello.at("authentication");
        identify["authentication"] = sha256_base64(sha256_base64(credentials.password + auth.at("salt").get<std::string>()) + auth.at("challenge").get<std::string>());
        s.send(Json{{"op", 1}, {"d", identify}});
        {
            std::unique_lock lock(s.mutex);
            if (!s.changed.wait_for(lock, std::chrono::seconds(5), [&] { return s.identified || !s.alive.load(); }) || !s.identified)
                throw std::runtime_error("OBS authentication failed. Close OBS and repair setup, then reconnect.");
        }
    } catch (...) {
        if (request) WinHttpCloseHandle(request);
        close();
        throw;
    }
}

bool ObsSocket::connected() const { return impl_->alive.load(); }

Json ObsSocket::request(const std::string& type, const Json& data, unsigned timeout_ms) {
    auto& s = *impl_;
    std::string id;
    {
        std::lock_guard lock(s.mutex);
        if (!s.waiting.empty()) throw std::runtime_error("An OBS request is already pending.");
        id = "replay-" + std::to_string(++s.sequence);
        s.waiting = id;
        s.response.reset();
    }
    try {
        s.send(Json{{"op", 6}, {"d", {{"requestType", type}, {"requestId", id}, {"requestData", data}}}});
        Json response;
        {
            std::unique_lock lock(s.mutex);
            if (!s.changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return s.response.has_value() || !s.alive.load(); }) || !s.response)
                throw std::runtime_error("OBS did not confirm " + type + ". Reconnect to check its state.");
            response = std::move(*s.response);
            s.waiting.clear(); s.response.reset();
        }
        const auto status = response.at("requestStatus");
        if (!status.value("result", false)) throw std::runtime_error(type + ": " + status.value("comment", "OBS rejected the request") + " (" + std::to_string(status.value("code", 0)) + ").");
        return response.value("responseData", Json::object());
    } catch (...) {
        std::lock_guard lock(s.mutex);
        s.waiting.clear(); s.response.reset();
        throw;
    }
}

std::vector<Json> ObsSocket::events() {
    auto& s = *impl_;
    std::lock_guard lock(s.mutex);
    std::vector<Json> result;
    result.reserve(s.incoming.size());
    while (!s.incoming.empty()) { result.push_back(std::move(s.incoming.front())); s.incoming.pop_front(); }
    return result;
}
}
