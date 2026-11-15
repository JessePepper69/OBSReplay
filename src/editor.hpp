#pragma once
#include "controller.hpp"
#include <expected>
#include <memory>
#include <string>

namespace saucer { struct application; }

namespace replay {
// Owns the clip editor windows. Each editor plays one confirmed clip, keeps its
// edit decisions in the page and exports the kept sections through FFmpeg.
class EditorHost {
public:
    EditorHost(saucer::application* app, Controller& controller, Paths paths);
    ~EditorHost();
    EditorHost(const EditorHost&) = delete;
    EditorHost& operator=(const EditorHost&) = delete;
    std::expected<bool, std::string> open(const std::string& path);
    // Describes running exports or unexported cuts that quitting would discard; empty when nothing is pending.
    std::string pending() const;
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};
}
