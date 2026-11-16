#pragma once
#include "controller.hpp"
#include <expected>
#include <functional>
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
    // FFmpeg as the editor host found it, with the progress of a winget install: {status, version, install{stage,error,available}}.
    // Guided setup shows it beside OBS Studio. `refresh` looks again when FFmpeg is missing.
    std::string ffmpeg_state(bool refresh);
    std::expected<bool, std::string> install_ffmpeg();
    // Audio peaks for the replay viewer's waveform lane, as JSON: {"status":"ready","rate":100,"peaks":"<base64>"}
    // or {"status":"unavailable"}. Decoded on a worker thread; `done` is called once, on the UI thread.
    // Asking for another clip stops a decode that is still running. Only clips in the recent history are read.
    void waveform(const std::string& path, std::function<void(std::string)> done);
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};
}
