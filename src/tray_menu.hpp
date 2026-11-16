#pragma once
#include <string>
#include <vector>

namespace replay {
// One row of the tray menu. `hint` is shown dim at the right (a shortcut). `divider` draws a line above the row.
struct TrayMenuItem {
    int id = 0;
    std::wstring label, hint;
    bool enabled = true, divider = false;
};
// What the menu shows: the capture state in its header, then the rows.
struct TrayMenu {
    enum class State { idle, capturing, attention };
    State state = State::idle;
    std::wstring status;
    std::vector<TrayMenuItem> items;
    std::string theme = "original";  // a theme id from frontend/themes.css
    bool animate = true;
};
// Opens the menu beside the cursor, in the app's own style, and waits like TrackPopupMenu does.
// Returns the id of the chosen row, or 0 when the menu was dismissed (Escape, a click elsewhere, focus lost).
int show_tray_menu(void* owner, const TrayMenu& menu);
// Draws the menu to a PNG at 2x, with row `hover` highlighted (-1 for none), without opening a window.
// Used to check the menu in every theme (OBSREPLAY_TRAY_PREVIEW=<folder>).
bool write_tray_menu_image(const TrayMenu& menu, int hover, const std::wstring& file);
}
