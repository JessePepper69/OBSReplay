#pragma once
#include <optional>

namespace replay {
// Window property that marks the main window of a running OBSReplay, and the message that asks it to quit.
// The installer uses both to close a running copy before it replaces the executable.
inline constexpr wchar_t app_window_property[] = L"OBSReplay.OwnedWindow.0.1";
inline constexpr wchar_t quit_message_name[] = L"OBSReplay.QuitWindow.0.1";

// OBSReplay.exe installs itself. Started from anywhere but its install folder
// (%LOCALAPPDATA%\Programs\OBSReplay), it copies itself there, adds a Start menu shortcut, registers with
// Windows "Installed apps" and opens the installed copy. Started with --uninstall it removes all of that.
// Returns the exit code when this launch was an install, update or uninstall, and nothing when the app should run.
// The app runs in place, without installing, with --portable or in an isolated check (OBSREPLAY_DATA_DIR).
std::optional<int> handle_install(int argc, char** argv);
// True when this process is the installed copy.
bool installed();
// Starts the uninstaller (the installed copy with --uninstall). It asks for confirmation, then closes this app.
bool start_uninstall();
}
