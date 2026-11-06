#pragma once
#include "integration.hpp"
#include <memory>

namespace replay {
class ObsSocket {
public:
    ObsSocket();
    ~ObsSocket();
    ObsSocket(const ObsSocket&) = delete;
    ObsSocket& operator=(const ObsSocket&) = delete;
    void connect(const Credentials& credentials);
    void close();
    bool connected() const;
    Json request(const std::string& type, const Json& data = Json::object(), unsigned timeout_ms = 5000);
    std::vector<Json> events();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
