#pragma once
#include "platform.hpp"
#include <expected>
#include <memory>

namespace replay {
// A typed name reduced to a file stem Windows allows: no reserved characters or device names, the extension dropped,
// at most 120 characters. Empty when nothing usable is left.
std::wstring clean_file_stem(const std::string& typed, const std::wstring& extension);
class Controller {
public:
    explicit Controller(Paths paths);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    std::string snapshot() const;
    bool knows_clip(const std::string& path) const;
    std::expected<bool, std::string> add_clip(const std::string& path, const std::string& app);
    // Lists a picture saved from a clip (the viewer's screenshot button) on the Screenshots tab.
    std::expected<bool, std::string> add_screenshot(const std::string& path, const std::string& app);
    // Deletes a listed clip at once, outside the action queue. The editor uses it for "delete original after exporting".
    std::expected<bool, std::string> remove_clip(const std::string& path);
    std::expected<bool, std::string> dispatch(std::string action, std::string payload = "{}");
    // OBS's own figures (frame rate, render time, skipped frames) from its last answer, or null when OBS is not connected.
    // Asking keeps them fresh for a few seconds; nobody asking, OBS is not polled for them.
    Json obs_stats() const;
    // Saves a colour theme at once, outside the action queue. Refused while an action is running.
    std::expected<bool, std::string> set_theme(const std::string& id);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
