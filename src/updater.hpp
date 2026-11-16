#pragma once
#include "platform.hpp"
#include <expected>
#include <functional>
#include <memory>
#include <string>

// Updates from GitHub Releases. The app asks for the latest release, compares its tag with the version built in,
// and on the user's request downloads the release's OBSReplay.exe, checks it and runs it. That executable is its own
// installer (installer.hpp): it closes this copy, replaces the installed file and opens the new version.
// Nothing is downloaded or installed without the user asking.
namespace replay {
class Updater {
public:
    explicit Updater(const Paths& paths);
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;
    // {enabled, status, current, latest{version,notes,page,size,published}|null, available, skipped, received, total,
    //  error, error_stage, checked_at, just_updated, previous}
    // status: idle, checking, current, available, downloading, verifying, installing, error.
    Json state() const;
    // Changes whenever state() would.
    unsigned revision() const;
    // Call about once a second on the UI thread: runs the check that is due (10 seconds after launch, then daily)
    // and notices an installer that ended without replacing this copy.
    void tick();
    // Asks GitHub now. A manual check reports its failure; a scheduled one only logs it.
    void check(bool manual);
    // Hides the available version until a newer one is released. `skipped` false brings it back.
    void skip(bool skipped);
    void set_enabled(bool enabled);
    // Downloads and checks the available version on a worker thread, then calls `ready` (on that thread) with the file.
    std::expected<bool, std::string> start(std::function<void(const fs::path&)> ready);
    void cancel();
    // Runs the downloaded installer. It closes this app within a moment.
    void install(const fs::path& file);
    void fail(const std::string& error);
    // The "Updated to" note has been shown.
    void seen();
    // The version of the latest release that is newer than this build and not skipped; empty otherwise.
    std::string offered() const;
    struct Impl;
private:
    std::shared_ptr<Impl> impl_;
};
}
