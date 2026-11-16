#pragma once
#include "platform.hpp"
#include <expected>
#include <memory>

namespace replay {
class Controller {
public:
    explicit Controller(Paths paths);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    std::string snapshot() const;
    bool knows_clip(const std::string& path) const;
    std::expected<bool, std::string> add_clip(const std::string& path, const std::string& app);
    std::expected<bool, std::string> dispatch(std::string action, std::string payload = "{}");
    // OBS's own figures (frame rate, render time, skipped frames) from its last answer, or null when OBS is not connected.
    // Asking keeps them fresh for a few seconds; nobody asking, OBS is not polled for them.
    Json obs_stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
