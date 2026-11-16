#include "tray_menu.hpp"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objidl.h>

#include <algorithm>
#include <map>
#include <memory>

// GDI+ expects the min and max macros that NOMINMAX removes.
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

namespace replay {
namespace {
// Colours of each theme, from frontend/app.css (Original) and frontend/themes.css. Keep them in step with those files.
struct Palette { COLORREF card, raise, line, text, dim, accent, green, warning; };
constexpr COLORREF hex(unsigned value) { return RGB((value >> 16) & 255, (value >> 8) & 255, value & 255); }
const Palette& palette(const std::string& theme) {
    static const std::map<std::string, Palette> palettes{
        {"original", {hex(0x121924), hex(0x1f2a38), hex(0x344254), hex(0xf5f7fc), hex(0x8e9db2), hex(0x6bb6ff), hex(0x70dfad), hex(0xd3bd8b)}},
        {"mono", {hex(0x161616), hex(0x262626), hex(0x3e3e3e), hex(0xf2f2f2), hex(0x9c9c9c), hex(0xffffff), hex(0x70dfad), hex(0xd3bd8b)}},
        {"light", {hex(0xffffff), hex(0xe4e8ee), hex(0x8a94a3), hex(0x0e151f), hex(0x566173), hex(0x0061ba), hex(0x17734a), hex(0x875600)}},
        {"rose", {hex(0x1e1619), hex(0x31252a), hex(0x4b3c42), hex(0xfaf6f7), hex(0xa7969d), hex(0xf48eb6), hex(0x6fe0b4), hex(0xdccd6c)}},
        {"amethyst", {hex(0x191721), hex(0x292734), hex(0x413e4f), hex(0xf7f6fb), hex(0x9c99ac), hex(0xb6a1f5), hex(0x70dfad), hex(0xd3bd8b)}},
        {"ember", {hex(0x1d1713), hex(0x2f2722), hex(0x493f38), hex(0xf9f6f5), hex(0xa59992), hex(0xfb9363), hex(0x62dfb9), hex(0xe2d35f)}}};
    const auto found = palettes.find(theme);
    return found == palettes.end() ? palettes.at("original") : found->second;
}
Gdiplus::Color colour(COLORREF value, BYTE alpha = 255) { return Gdiplus::Color(alpha, GetRValue(value), GetGValue(value), GetBValue(value)); }

constexpr UINT_PTR fade_timer = 1;
struct MenuWindow {
    const TrayMenu* menu = nullptr;
    const Palette* colours = nullptr;
    HWND hwnd = nullptr;
    HICON icon = nullptr;
    int dpi = 96, hover = -1, result = 0, alpha = 255;
    bool tracking = false, closing = false;
    int px(int value) const { return MulDiv(value, dpi, 96); }
    // Layout, in 96-dpi units: a header, then rows; a row with a divider is taller by the divider's space.
    static constexpr int width = 268, pad = 6, header = 56, row = 34, divider = 9;
    int row_top(int index) const {
        int top = pad + header;
        for (int i = 0; i <= index; ++i) { if (menu->items[i].divider) top += divider; if (i < index) top += row; }
        return top;
    }
    int height() const { return row_top(static_cast<int>(menu->items.size()) - 1) + row + pad; }
    int row_at(int x, int y) const {
        if (x < px(pad) || x >= px(width - pad)) return -1;
        for (int i = 0; i < static_cast<int>(menu->items.size()); ++i) {
            const int top = px(row_top(i));
            if (y >= top && y < top + px(row)) return menu->items[i].enabled ? i : -1;
        }
        return -1;
    }
    void move(int step) {
        const int count = static_cast<int>(menu->items.size());
        for (int tries = 0, index = hover; tries < count; ++tries) {
            index = index < 0 ? (step > 0 ? 0 : count - 1) : (index + step + count) % count;
            if (menu->items[index].enabled) { hover = index; InvalidateRect(hwnd, nullptr, FALSE); return; }
        }
    }
    void close(int chosen) { if (closing) return; closing = true; result = chosen; DestroyWindow(hwnd); }
};
void rounded(Gdiplus::GraphicsPath& path, Gdiplus::REAL x, Gdiplus::REAL y, Gdiplus::REAL w, Gdiplus::REAL h, Gdiplus::REAL r) {
    const Gdiplus::REAL d = r * 2;
    path.AddArc(x, y, d, d, 180, 90); path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90); path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}
void draw(const MenuWindow& w, Gdiplus::Bitmap& buffer) {
    {
        Gdiplus::Graphics g(&buffer);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        const auto& c = *w.colours;
        g.Clear(colour(c.card));
        const auto real = [&w](int value) { return static_cast<Gdiplus::REAL>(w.px(value)); };
        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font title(&family, real(14), Gdiplus::FontStyleBold, Gdiplus::UnitPixel), text(&family, real(13), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel), note(&family, real(12), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush ink(colour(c.text)), dim(colour(c.dim)), faint(colour(c.dim, 120));
        Gdiplus::StringFormat left, right;
        left.SetLineAlignment(Gdiplus::StringAlignmentCenter); left.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap); left.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        right.SetLineAlignment(Gdiplus::StringAlignmentCenter); right.SetAlignment(Gdiplus::StringAlignmentFar); right.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        // Header: the app icon, its name, and the capture state with a coloured dot.
        const int icon_size = w.px(26), icon_left = w.px(MenuWindow::pad + 10), icon_top = w.px(MenuWindow::pad) + (w.px(MenuWindow::header) - icon_size) / 2 - w.px(2);
        if (w.icon) { const HDC dc = g.GetHDC(); DrawIconEx(dc, icon_left, icon_top, w.icon, icon_size, icon_size, 0, nullptr, DI_NORMAL); g.ReleaseHDC(dc); }
        const Gdiplus::REAL copy_left = static_cast<Gdiplus::REAL>(icon_left + icon_size) + real(11), copy_width = real(MenuWindow::width - MenuWindow::pad - 12) - copy_left;
        g.DrawString(L"OBSReplay", -1, &title, Gdiplus::RectF(copy_left, real(MenuWindow::pad + 8), copy_width, real(20)), &left, &ink);
        const COLORREF dot = w.menu->state == TrayMenu::State::capturing ? c.green : w.menu->state == TrayMenu::State::attention ? c.warning : c.dim;
        Gdiplus::SolidBrush dot_brush(colour(dot));
        g.FillEllipse(&dot_brush, copy_left + real(1), real(MenuWindow::pad + 35), real(7), real(7));
        g.DrawString(w.menu->status.c_str(), -1, &note, Gdiplus::RectF(copy_left + real(13), real(MenuWindow::pad + 28), copy_width - real(13), real(20)), &left, &dim);
        Gdiplus::Pen line(colour(c.line, 150), 1);
        const auto rule = [&](int top) { const Gdiplus::REAL y = real(top) + 0.5f; g.DrawLine(&line, real(MenuWindow::pad + 8), y, real(MenuWindow::width - MenuWindow::pad - 8), y); };
        rule(MenuWindow::pad + MenuWindow::header - 5);
        for (int i = 0; i < static_cast<int>(w.menu->items.size()); ++i) {
            const auto& item = w.menu->items[i];
            const int top = w.row_top(i);
            if (item.divider) rule(top - MenuWindow::divider / 2 - 1);
            if (i == w.hover && item.enabled) {
                Gdiplus::GraphicsPath shape;
                rounded(shape, real(MenuWindow::pad), real(top), real(MenuWindow::width - MenuWindow::pad * 2), real(MenuWindow::row), real(8));
                Gdiplus::SolidBrush fill(colour(c.raise));
                g.FillPath(&fill, &shape);
            }
            const Gdiplus::RectF box(real(MenuWindow::pad + 12), real(top), real(MenuWindow::width - MenuWindow::pad * 2 - 24), real(MenuWindow::row));
            g.DrawString(item.label.c_str(), -1, &text, box, &left, item.enabled ? &ink : &faint);
            if (!item.hint.empty()) g.DrawString(item.hint.c_str(), -1, &note, box, &right, item.enabled ? &dim : &faint);
        }
    }
}
void paint(MenuWindow& w) {
    PAINTSTRUCT area;
    const HDC screen = BeginPaint(w.hwnd, &area);
    RECT client;
    GetClientRect(w.hwnd, &client);
    Gdiplus::Bitmap buffer(client.right, client.bottom, PixelFormat32bppPARGB);
    draw(w, buffer);
    Gdiplus::Graphics out(screen);
    out.DrawImage(&buffer, 0, 0);
    EndPaint(w.hwnd, &area);
}
LRESULT CALLBACK menu_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* w = reinterpret_cast<MenuWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!w) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_PAINT: paint(*w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        if (!w->tracking) { TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track); w->tracking = true; }
        const int over = w->row_at(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        if (over != w->hover) { w->hover = over; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE: w->tracking = false; if (w->hover != -1) { w->hover = -1; InvalidateRect(hwnd, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: {
        const int over = w->row_at(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        if (over >= 0) w->close(w->menu->items[over].id);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) w->close(0);
        else if (wparam == VK_DOWN || wparam == VK_TAB) w->move(1);
        else if (wparam == VK_UP) w->move(-1);
        else if ((wparam == VK_RETURN || wparam == VK_SPACE) && w->hover >= 0) w->close(w->menu->items[w->hover].id);
        return 0;
    case WM_ACTIVATE: if (LOWORD(wparam) == WA_INACTIVE) w->close(0); return 0;
    case WM_TIMER:
        if (wparam == fade_timer) {
            w->alpha = std::min(255, w->alpha + 52);
            SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(w->alpha), LWA_ALPHA);
            if (w->alpha >= 255) KillTimer(hwnd, fade_timer);
        }
        return 0;
    case WM_DESTROY: PostMessageW(nullptr, WM_NULL, 0, 0); return 0;  // wakes the wait loop
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}

int show_tray_menu(void* owner, const TrayMenu& menu) {
    if (menu.items.empty()) return 0;
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) return 0;
    const HINSTANCE module = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_DROPSHADOW;
    window_class.lpfnWndProc = menu_proc;
    window_class.hInstance = module;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"OBSReplay.TrayMenu";
    RegisterClassExW(&window_class);

    MenuWindow w;
    w.menu = &menu;
    w.colours = &palette(menu.theme);
    POINT cursor{};
    GetCursorPos(&cursor);
    w.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, window_class.lpszClassName, L"OBSReplay", WS_POPUP, cursor.x, cursor.y, 0, 0, static_cast<HWND>(owner), nullptr, module, nullptr);
    int result = 0;
    if (w.hwnd) {
        SetWindowLongPtrW(w.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&w));
        w.dpi = static_cast<int>(GetDpiForWindow(w.hwnd));
        w.icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(1), IMAGE_ICON, w.px(26), w.px(26), LR_DEFAULTCOLOR));
        // Windows 11 rounds the corners and draws the border; Windows 10 shows a square menu.
        const int corners = 2 /* DWMWCP_ROUND */;
        DwmSetWindowAttribute(w.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
        const COLORREF border = w.colours->line;
        DwmSetWindowAttribute(w.hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
        // Above the cursor when there is room (the tray sits at the bottom), inside the work area either way.
        const int width = w.px(MenuWindow::width), height = w.px(w.height()), gap = w.px(10), edge = w.px(8);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT& area = monitor.rcWork;
        int x = cursor.x - width / 2, y = cursor.y - height - gap;
        if (y < area.top + edge) y = cursor.y + gap;
        x = std::clamp(x, static_cast<int>(area.left) + edge, std::max(static_cast<int>(area.left) + edge, static_cast<int>(area.right) - width - edge));
        y = std::clamp(y, static_cast<int>(area.top) + edge, std::max(static_cast<int>(area.top) + edge, static_cast<int>(area.bottom) - height - edge));
        w.alpha = menu.animate ? 0 : 255;
        SetLayeredWindowAttributes(w.hwnd, 0, static_cast<BYTE>(w.alpha), LWA_ALPHA);
        SetWindowPos(w.hwnd, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
        SetForegroundWindow(w.hwnd);
        SetFocus(w.hwnd);
        if (menu.animate) SetTimer(w.hwnd, fade_timer, 15, nullptr);
        MSG message;
        while (IsWindow(w.hwnd)) {
            const BOOL got = GetMessageW(&message, nullptr, 0, 0);
            // A quit that arrives while the menu is open belongs to the app's own loop.
            if (got == 0) { PostQuitMessage(static_cast<int>(message.wParam)); DestroyWindow(w.hwnd); break; }
            if (got < 0) break;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        result = w.result;
        if (w.icon) DestroyIcon(w.icon);
    }
    UnregisterClassW(window_class.lpszClassName, module);
    Gdiplus::GdiplusShutdown(token);
    return result;
}

bool write_tray_menu_image(const TrayMenu& menu, int hover, const std::wstring& file) {
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) return false;
    bool saved = false;
    {
        MenuWindow w;
        w.menu = &menu;
        w.colours = &palette(menu.theme);
        w.dpi = 192;
        w.hover = hover;
        w.icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, w.px(26), w.px(26), LR_DEFAULTCOLOR));
        Gdiplus::Bitmap buffer(w.px(MenuWindow::width), w.px(w.height()), PixelFormat32bppPARGB);
        draw(w, buffer);
        const CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        saved = buffer.Save(file.c_str(), &png, nullptr) == Gdiplus::Ok;
        if (w.icon) DestroyIcon(w.icon);
    }
    Gdiplus::GdiplusShutdown(token);
    return saved;
}
}
