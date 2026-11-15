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
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
