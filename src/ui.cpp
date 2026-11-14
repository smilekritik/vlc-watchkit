#include "ui.hpp"
#include "windows.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <utility>

namespace addon {
namespace {
constexpr UINT_PTR opacity_timer = 1;
constexpr UINT opacity_interval_ms = 16;
constexpr wchar_t window_class[] = L"VLCAddonWindow";
enum Id {
    confirm = 100, dismiss, notes, search, list, open_item, edit_item, remove_item, bind_list, change_bind,
    reset_binds, application, image, publish, capture, save_settings, publish_label
};
enum class Kind { prompt, add, library, edit, settings, capture, status, speed };

bool card(Kind kind)
{
    return kind == Kind::prompt || kind == Kind::add || kind == Kind::edit || kind == Kind::speed;
}

struct Palette {
    COLORREF surface, field, text, muted, accent, toolbar, button_pressed, accent_pressed, accent_text;
};

constexpr Palette dark_palette{
    RGB(33, 33, 33), RGB(49, 49, 49), RGB(245, 245, 245), RGB(192, 192, 192),
    RGB(255, 176, 101), RGB(33, 33, 33), RGB(68, 68, 68), RGB(236, 149, 77), RGB(30, 23, 18)
};
constexpr Palette light_palette{
    RGB(249, 249, 249), RGB(232, 232, 232), RGB(26, 26, 26), RGB(75, 75, 75),
    RGB(255, 176, 101), RGB(239, 239, 239), RGB(214, 214, 214), RGB(236, 149, 77), RGB(30, 23, 18)
};

void rounded_fill(HDC dc, RECT rect, int radius, COLORREF color)
{
    const auto brush = CreateSolidBrush(color);
    const auto old_brush = SelectObject(dc, brush);
    const auto old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
}

std::wstring get_text(HWND window)
{
    const auto size = GetWindowTextLengthW(window);
    std::wstring value(size + 1, L'\0');
    value.resize(GetWindowTextW(window, value.data(), size + 1));
    return value;
}

std::wstring sort_key(std::wstring text)
{
    if (!text.empty())
        CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

Binding key_message(const MSG& message)
{
    return {static_cast<unsigned>(message.wParam),
        (GetKeyState(VK_SHIFT) < 0 ? shift : 0) |
        (GetKeyState(VK_CONTROL) < 0 ? control : 0) |
        (GetKeyState(VK_MENU) < 0 ? alt : 0)};
}
}

struct Ui::Impl {
    struct Window {
        Impl* ui;
        Kind kind;
        HWND hwnd = nullptr;
        HWND focus = nullptr;
        int dpi = 96;
        int rendered_dpi = 0;
        HFONT body = nullptr, heading = nullptr;
        HWND heading_label = nullptr, secondary_label = nullptr;
        double progress_fraction = 0;
        int px(int value) const { return MulDiv(value, dpi, 96); }
        explicit Window(Impl* parent, Kind type) : ui(parent), kind(type) {}
    };
    explicit Impl(UiCallbacks callbacks, bool dark) : callbacks(std::move(callbacks)),
        dark(dark), palette(dark ? dark_palette : light_palette),
        module(GetModuleHandleW(L"libvlc_addon_plugin.dll")),
        prompt_window(this, Kind::prompt), add_window(this, Kind::add), library_window(this, Kind::library),
        edit_window(this, Kind::edit), settings_window(this, Kind::settings), capture_window(this, Kind::capture), status_window(this, Kind::status), speed_window(this, Kind::speed)
    {
        WNDCLASSW cls{};
        cls.lpfnWndProc = window_proc;
        cls.hInstance = module;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        cls.lpszClassName = window_class;
        registered = RegisterClassW(&cls) != 0;
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
        InitCommonControlsEx(&controls);
    }

    ~Impl()
    {
        for (auto* window : {&speed_window, &status_window, &capture_window, &edit_window, &prompt_window, &add_window, &library_window, &settings_window})
            close(*window);
        DeleteObject(surface_brush);
        DeleteObject(field_brush);
        if (registered)
            UnregisterClassW(window_class, module);
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) noexcept
    {
        if (message == WM_NCCREATE) {
            auto* window = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            window->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        }
        auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!window)
            return DefWindowProcW(hwnd, message, wp, lp);
        try {
            if (window->kind == Kind::status) {
                if (message == WM_TIMER && wp == opacity_timer) {
                    window->ui->sync_controller();
                    return 0;
                }
                if (message == WM_SETCURSOR) {
                    SetCursor(LoadCursorW(nullptr, window->ui->over_speed(hwnd) ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
                if (message == WM_LBUTTONUP && PtInRect(&window->ui->speed_area, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) {
                    window->ui->edit_speed();
                    return 0;
                }
                if (message == WM_ERASEBKGND) return 1;
            }
            if (window->kind != Kind::status) {
                if (message == WM_ERASEBKGND) return 1;
                if (message == WM_PAINT) {
                    window->ui->paint_card(*window);
                    return 0;
                }
                if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORBTN) {
                    const auto dc = reinterpret_cast<HDC>(wp);
                    const auto child = reinterpret_cast<HWND>(lp);
                    const bool editing = message == WM_CTLCOLOREDIT;
                    SetBkColor(dc, editing ? window->ui->palette.field : window->ui->palette.surface);
                    SetTextColor(dc, child == window->secondary_label ? window->ui->palette.muted : window->ui->palette.text);
                    return reinterpret_cast<LRESULT>(editing ? window->ui->field_brush : window->ui->surface_brush);
                }
                if (message == WM_DRAWITEM) {
                    window->ui->paint_button(*window, *reinterpret_cast<DRAWITEMSTRUCT*>(lp));
                    return TRUE;
                }
            }
            if (message == WM_CLOSE) {
                window->ui->cancel(*window);
                return 0;
            }
            if (message == WM_COMMAND) {
                window->ui->command(*window, LOWORD(wp), HIWORD(wp));
                return 0;
            }
            if (message == WM_NOTIFY) {
                const auto& notification = *reinterpret_cast<NMHDR*>(lp);
                if (window->ui->dark && notification.code == NM_CUSTOMDRAW &&
                    (notification.idFrom == list || notification.idFrom == bind_list))
                    return window->ui->draw_table_row(*reinterpret_cast<NMLVCUSTOMDRAW*>(lp));
                if (notification.idFrom == list && notification.code == NM_DBLCLK)
                    window->ui->command(*window, open_item, 0);
                if (notification.idFrom == bind_list && notification.code == NM_DBLCLK)
                    window->ui->command(*window, change_bind, 0);
                return 0;
            }
            if (message == WM_NCDESTROY) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                window->hwnd = nullptr;
                window->rendered_dpi = 0;
                if (window->kind == Kind::status && window->ui->status_tip) {
                    DestroyWindow(window->ui->status_tip);
                    window->ui->status_tip = nullptr;
                }
                if (window->body) DeleteObject(std::exchange(window->body, nullptr));
                if (window->heading) DeleteObject(std::exchange(window->heading, nullptr));
                window->heading_label = window->secondary_label = nullptr;
            }
        } catch (...) {
            MessageBoxW(hwnd, L"The operation could not be completed.", L"VLC WatchKit", MB_OK | MB_ICONERROR);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }

    static BYTE controller_opacity(HWND controller)
    {
        if (!IsWindowVisible(controller)) return 0;
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(controller, nullptr, &alpha, &flags) && (flags & LWA_ALPHA))
            return alpha;
        return 255;
    }

    void sync_controller()
    {
        const auto controller = GetWindow(status_window.hwnd, GW_OWNER);
        if (!IsWindowVisible(controller)) {
            close(status_window);
            return;
        }
        if (controller_opacity(controller) != status_opacity)
            playback_status(controller, status_text, true, true);
    }

    bool render_status(Window& window, POINT origin, SIZE size, std::wstring_view caption, COLORREF background, COLORREF foreground,
                       bool toolbar, bool compact, BYTE opacity)
    {
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
        bitmap.bmiHeader.biWidth = size.cx;
        bitmap.bmiHeader.biHeight = -size.cy;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        const auto dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        const auto pixels = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!pixels) {
            DeleteDC(dc);
            return false;
        }
        const auto old_bitmap = SelectObject(dc, pixels);
        const auto old_font = SelectObject(dc, window.body ? window.body : GetStockObject(DEFAULT_GUI_FONT));
        const auto brush = CreateSolidBrush(toolbar ? RGB(0, 0, 0) : background);
        RECT rect{0, 0, size.cx, size.cy};
        FillRect(dc, &rect, brush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, toolbar ? RGB(255, 255, 255) : foreground);
        constexpr auto align = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
        const auto separator = caption.find(L"  \u00b7  ");
        if (compact && separator != std::wstring_view::npos) {
            RECT top{0, 0, size.cx, size.cy / 2};
            RECT bottom{0, top.bottom, size.cx, size.cy};
            const auto time = caption.substr(separator + 5);
            DrawTextW(dc, caption.data(), static_cast<int>(separator), &top, align);
            DrawTextW(dc, time.data(), static_cast<int>(time.size()), &bottom, align);
        } else {
            DrawTextW(dc, caption.data(), static_cast<int>(caption.size()), &rect, align);
        }
        GdiFlush();
        auto* row = static_cast<std::uint32_t*>(bits);
        for (LONG i = 0; i < size.cx * size.cy; ++i) {
            if (!toolbar) {
                row[i] |= 0xff000000u;
                continue;
            }
            // GDI draws the glyph mask. Convert its coverage to premultiplied BGRA.
            const auto alpha = row[i] & 0xff;
            row[i] = (std::max(alpha, 1u) << 24) | ((GetRValue(foreground) * alpha / 255) << 16) |
                     ((GetGValue(foreground) * alpha / 255) << 8) | (GetBValue(foreground) * alpha / 255);
        }
        POINT source{};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
        const bool rendered = UpdateLayeredWindow(window.hwnd, nullptr, &origin, &size, dc, &source, 0, &blend, ULW_ALPHA) != FALSE;
        DeleteObject(brush);
        SelectObject(dc, old_font);
        SelectObject(dc, old_bitmap);
        DeleteObject(pixels);
        DeleteDC(dc);
        return rendered;
    }

    void playback_status(HWND player, std::wstring text, bool fullscreen, bool known_controller = false)
    {
        auto& window = status_window;
        if (fullscreen && player && !known_controller)
            player = fullscreen_controls(player);
        wchar_t class_name[64]{};
        GetClassNameW(player, class_name, static_cast<int>(std::size(class_name)));
        if (text.empty() || !player || !IsWindowVisible(player) || !wcsstr(class_name, L"Qt")) {
            close(window);
            return;
        }
        if (window.hwnd && GetWindow(window.hwnd, GW_OWNER) != player) close(window);
        RECT rect{};
        if (!GetClientRect(player, &rect)) return;
        window.dpi = std::max(96u, GetDpiForWindow(player));
        const int free_width = rect.right - window.px(110 + 288);
        const bool compact = !fullscreen && free_width < window.px(162);
        const bool fits_toolbar = fullscreen || free_width >= window.px(70);
        const int width = window.px(compact && fits_toolbar ? 70 : 162);
        const int height = window.px(compact && fits_toolbar ? 28 : 22);
        const int x = fullscreen ? (rect.right - width) / 2 :
            fits_toolbar ? rect.right - window.px(110) - width : rect.right - width - window.px(8);
        const int y = rect.bottom - window.px(fullscreen ? 27 : fits_toolbar ? (compact ? 30 : 27) : 75);
        if (x < 0 || y < height || IsIconic(player)) {
            if (window.hwnd) ShowWindow(window.hwnd, SW_HIDE);
            return;
        }

        POINT origin{x, y};
        if (!ClientToScreen(player, &origin)) return;
        if (!window.hwnd) {
            CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, window_class, text.c_str(), WS_POPUP,
                            origin.x, origin.y, width, height, player, nullptr, module, &window);
            if (!window.hwnd) return;
            if (fullscreen) SetTimer(window.hwnd, opacity_timer, opacity_interval_ms, nullptr);
        }
        const bool dpi_changed = window.rendered_dpi != window.dpi;
        if (dpi_changed) {
            if (window.body) DeleteObject(window.body);
            // VLC's Qt TimeLabel inherits the Windows application message font.
            NONCLIENTMETRICSW metrics{};
            metrics.cbSize = sizeof(metrics);
            if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, window.dpi)) {
                auto font = metrics.lfMessageFont;
                font.lfQuality = ANTIALIASED_QUALITY;
                window.body = CreateFontIndirectW(&font);
            } else {
                window.body = CreateFontW(-window.px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, 0, L"Segoe UI");
            }
        }
        const auto background = fits_toolbar ? palette.toolbar : palette.surface;
        // VLC's floating Qt controller shows its dark palette's inactive text.
        const auto foreground = dark && fullscreen ? RGB(138, 138, 138) : palette.text;
        const BYTE opacity = fullscreen ? controller_opacity(player) : 255;
        RECT current{};
        GetWindowRect(window.hwnd, &current);
        const bool moved = current.left != origin.x || current.top != origin.y;
        if (dpi_changed || status_text != text || status_on_toolbar != fits_toolbar ||
            status_compact != compact || status_foreground != foreground || status_opacity != opacity) {
            if (!render_status(window, origin, SIZE{width, height}, text, background, foreground, fits_toolbar, compact && fits_toolbar, opacity)) {
                ShowWindow(window.hwnd, SW_HIDE);
                return;
            }
            window.rendered_dpi = window.dpi;
            status_on_toolbar = fits_toolbar;
            status_foreground = foreground;
            status_opacity = opacity;
            status_compact = compact;
            status_text = std::move(text);
            SetWindowTextW(window.hwnd, status_text.c_str());
            update_speed_tool(window, SIZE{width, height}, compact && fits_toolbar, !fullscreen);
        } else if (moved) {
            SetWindowPos(window.hwnd, nullptr, origin.x, origin.y, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
        }
        if (!IsWindowVisible(window.hwnd)) ShowWindow(window.hwnd, SW_SHOWNOACTIVATE);
    }

    bool over_speed(HWND status) const
    {
        POINT point{};
        return GetCursorPos(&point) && ScreenToClient(status, &point) && PtInRect(&speed_area, point);
    }

    void update_speed_tool(Window& window, SIZE size, bool compact, bool show_tip)
    {
        const auto separator = status_text.find(L"  \u00b7  ");
        const auto value = status_text.substr(0, separator);
        const auto dc = GetDC(window.hwnd);
        if (!dc) return;
        const auto old_font = SelectObject(dc, window.body ? window.body : GetStockObject(DEFAULT_GUI_FONT));
        SIZE full{}, rate_width{};
        GetTextExtentPoint32W(dc, status_text.c_str(), static_cast<int>(status_text.size()), &full);
        GetTextExtentPoint32W(dc, value.c_str(), static_cast<int>(value.size()), &rate_width);
        SelectObject(dc, old_font);
        ReleaseDC(window.hwnd, dc);
        const LONG start = (size.cx - full.cx) / 2;
        speed_area = compact ? RECT{0, 0, size.cx, size.cy / 2} :
            RECT{std::max(0L, start - 3), 0, std::min(size.cx, start + rate_width.cx + 3), size.cy};
        if (!show_tip) return;
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.hwnd = window.hwnd;
        tool.uId = 1;
        tool.rect = speed_area;
        if (!status_tip) {
            status_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                         0, 0, 0, 0, window.hwnd, nullptr, module, nullptr);
            if (!status_tip) return;
            static wchar_t caption[] = L"Playback speed. Click to change.";
            tool.uFlags = TTF_SUBCLASS;
            tool.lpszText = caption;
            SendMessageW(status_tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        } else {
            SendMessageW(status_tip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
        }
    }

    void edit_speed()
    {
        if (!create(speed_window, L"Playback speed", 476, 198)) return;
        heading(speed_window, L"Playback speed");
        label(speed_window, L"Enter a playback speed from 0.03 to 31.25.", 24, 62, 428);
        const auto value = status_text.substr(0, status_text.find(L'\u00d7'));
        speed_window.focus = input(speed_window, value.c_str(), notes, 24, 90, 428, 30);
        button(speed_window, L"Apply speed", confirm, 24, 139, 167, true);
        button(speed_window, L"Cancel", dismiss, 344, 139, 108);
        show(speed_window, true);
        SendMessageW(speed_window.focus, EM_SETSEL, 0, -1);
    }

    void apply_speed()
    {
        const auto rate = parse_speed(get_text(speed_window.focus));
        if (!rate) {
            MessageBoxW(speed_window.hwnd, L"Enter a playback speed from 0.03 to 31.25.",
                        L"Playback speed", MB_OK | MB_ICONINFORMATION);
            return;
        }
        callbacks.set_speed(*rate);
        close(speed_window);
    }

    void paint_card(Window& window)
    {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window.hwnd, &paint);
        RECT rect{};
        GetClientRect(window.hwnd, &rect);
        FillRect(dc, &rect, surface_brush);
        const auto x = window.px(24);
        if (window.kind == Kind::prompt && window.progress_fraction >= 0) {
            RECT track{x, window.px(134), rect.right - x, window.px(140)};
            rounded_fill(dc, track, window.px(6), palette.field);
            track.right = track.left + static_cast<LONG>((track.right - track.left) * window.progress_fraction);
            if (track.right > track.left) rounded_fill(dc, track, window.px(6), palette.accent);
        }
        EndPaint(window.hwnd, &paint);
    }

    void paint_button(Window& window, const DRAWITEMSTRUCT& button)
    {
        const bool primary = button.CtlID == confirm || button.CtlID == save_settings || button.CtlID == open_item;
        FillRect(button.hDC, &button.rcItem, surface_brush);
        const bool down = (button.itemState & ODS_SELECTED) != 0;
        const auto background = primary ? (down ? palette.accent_pressed : palette.accent) : (down ? palette.button_pressed : palette.field);
        rounded_fill(button.hDC, button.rcItem, window.px(16), background);
        const auto prior = SelectObject(button.hDC, window.body ? window.body : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(button.hDC, TRANSPARENT);
        SetTextColor(button.hDC, primary ? palette.accent_text : palette.text);
        auto rect = button.rcItem;
        auto caption = get_text(button.hwndItem);
        DrawTextW(button.hDC, caption.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (button.itemState & ODS_FOCUS) {
            InflateRect(&rect, -window.px(4), -window.px(4));
            DrawFocusRect(button.hDC, &rect);
        }
        SelectObject(button.hDC, prior);
    }

    void close(Window& window)
    {
        if (window.kind == Kind::settings && capture_window.hwnd) close(capture_window);
        if (window.kind == Kind::library && edit_window.hwnd) close(edit_window);
        if (window.hwnd) DestroyWindow(window.hwnd);
    }

    RECT bounds(HWND parent) const
    {
        RECT rect{};
        if (!parent || !GetWindowRect(parent, &rect))
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &rect, 0);
        return rect;
    }

    void center(HWND hwnd, HWND parent) const
    {
        auto outer = bounds(parent);
        RECT current{};
        if (!GetWindowRect(hwnd, &current))
            return;
        const auto width = current.right - current.left;
        const auto height = current.bottom - current.top;
        SetWindowPos(hwnd, nullptr, outer.left + (outer.right - outer.left - width) / 2,
                     outer.top + (outer.bottom - outer.top - height) / 2, 0, 0,
                     SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    bool create(Window& window, const wchar_t* title, int width, int height, HWND parent = nullptr)
    {
        if (window.hwnd) {
            SetForegroundWindow(window.hwnd);
            return false;
        }
        if (!parent) parent = owner;
        if (!registered || !parent || !IsWindow(parent))
            return false;
        window.dpi = std::max(96u, GetDpiForWindow(parent));
        window.focus = nullptr;
        const DWORD style = card(window.kind) ? WS_POPUP : WS_POPUP | WS_CAPTION | WS_SYSMENU;
        const DWORD exstyle = WS_EX_CONTROLPARENT | (card(window.kind) ? 0 : WS_EX_DLGMODALFRAME);
        RECT adjusted{0, 0, MulDiv(width, window.dpi, 96), MulDiv(height, window.dpi, 96)};
        AdjustWindowRectExForDpi(&adjusted, style, FALSE, exstyle, window.dpi);
        CreateWindowExW(exstyle, window_class, title, style, 0, 0,
                        adjusted.right - adjusted.left, adjusted.bottom - adjusted.top, parent, nullptr, module, &window);
        if (window.hwnd) {
            if (!card(window.kind) && dark) {
                const BOOL enabled = TRUE;
                if (FAILED(DwmSetWindowAttribute(window.hwnd, 20, &enabled, sizeof(enabled))))
                    DwmSetWindowAttribute(window.hwnd, 19, &enabled, sizeof(enabled));
            }
            if (card(window.kind)) {
                window.body = CreateFontW(-window.px(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe UI");
                window.heading = CreateFontW(-window.px(24), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe UI");
                const auto region = CreateRoundRectRgn(0, 0, adjusted.right + 1, adjusted.bottom + 1, window.px(22), window.px(22));
                if (region && !SetWindowRgn(window.hwnd, region, FALSE)) DeleteObject(region);
            }
            center(window.hwnd, parent);
        }
        return window.hwnd != nullptr;
    }

    HWND widget(Window& window, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h,
                DWORD exstyle = 0)
    {
        const auto scaled = [&](int value) { return MulDiv(value, window.dpi, 96); };
        HWND control = CreateWindowExW(exstyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                                       scaled(x), scaled(y), scaled(w), scaled(h), window.hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(window.body ? window.body : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return control;
    }

    HWND label(Window& window, std::wstring_view text, int x, int y, int w, int h = 22)
    {
        return widget(window, L"STATIC", std::wstring(text).c_str(), SS_NOPREFIX | SS_ENDELLIPSIS, 0, x, y, w, h);
    }

    HWND input(Window& window, const wchar_t* text, int id, int x, int y, int w, int h, bool multiline = false)
    {
        auto handle = widget(window, L"EDIT", text, WS_TABSTOP | (multiline ? ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL :
                             ES_AUTOHSCROLL), id, x, y, w, h, WS_EX_CLIENTEDGE);
        if (dark) SetWindowTheme(handle, L"DarkMode_Explorer", nullptr);
        SendMessageW(handle, EM_SETLIMITTEXT, multiline ? 2000 : 160, 0);
        return handle;
    }

    void button(Window& window, const wchar_t* text, int id, int x, int y, int w, bool primary = false)
    {
        widget(window, L"BUTTON", text,
            WS_TABSTOP | (card(window.kind) || dark ? BS_OWNERDRAW : primary ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
            id, x, y, w, card(window.kind) ? 40 : 28);

    }

    static LRESULT CALLBACK dark_table(HWND control, UINT message, WPARAM wp, LPARAM lp,
                                        UINT_PTR id, DWORD_PTR) noexcept
    {
        if (message == WM_NCDESTROY)
            RemoveWindowSubclass(control, dark_table, id);
        if (message == WM_NOTIFY) {
            auto& header = *reinterpret_cast<NMHDR*>(lp);
            if (header.code == NM_CUSTOMDRAW && header.hwndFrom == ListView_GetHeader(control)) {
                auto& draw = *reinterpret_cast<NMCUSTOMDRAW*>(lp);
                if (draw.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (draw.dwDrawStage == CDDS_ITEMPREPAINT) {
                    wchar_t title[96]{};
                    HDITEMW item{};
                    item.mask = HDI_TEXT;
                    item.pszText = title;
                    item.cchTextMax = static_cast<int>(std::size(title));
                    Header_GetItem(header.hwndFrom, static_cast<int>(draw.dwItemSpec), &item);
                    const auto brush = CreateSolidBrush(dark_palette.surface);
                    FillRect(draw.hdc, &draw.rc, brush);
                    DeleteObject(brush);
                    auto rect = draw.rc;
                    rect.left += MulDiv(8, GetDpiForWindow(control), 96);
                    SetBkMode(draw.hdc, TRANSPARENT);
                    SetTextColor(draw.hdc, dark_palette.muted);
                    DrawTextW(draw.hdc, title, -1, &rect, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                    return CDRF_SKIPDEFAULT;
                }
            }
        }
        return DefSubclassProc(control, message, wp, lp);
    }

    LRESULT draw_table_row(NMLVCUSTOMDRAW& draw)
    {
        if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (draw.nmcd.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
        const auto control = draw.nmcd.hdr.hwndFrom;
        const auto index = static_cast<int>(draw.nmcd.dwItemSpec);
        const bool selected = (ListView_GetItemState(control, index, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        if (!selected) return CDRF_DODEFAULT;

        const auto dc = draw.nmcd.hdc;
        auto row = draw.nmcd.rc;
        RECT bounds{};
        GetClientRect(control, &bounds);
        row.left = 0;
        row.right = bounds.right;
        const auto selected_brush = CreateSolidBrush(RGB(70, 55, 44));
        FillRect(dc, &row, selected_brush);
        DeleteObject(selected_brush);
        SetTextColor(dc, palette.text);
        SetBkMode(dc, TRANSPARENT);
        const auto header = ListView_GetHeader(control);
        for (int column = 0; column < Header_GetItemCount(header); ++column) {
            RECT rect{};
            if (!Header_GetItemRect(header, column, &rect)) continue;
            rect.top = row.top;
            rect.bottom = row.bottom;
            rect.left += MulDiv(7, GetDpiForWindow(control), 96);
            wchar_t text[2048]{};
            ListView_GetItemText(control, index, column, text, static_cast<int>(std::size(text)));
            DrawTextW(dc, text, -1, &rect, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        if (GetFocus() == control && (draw.nmcd.uItemState & CDIS_FOCUS)) DrawFocusRect(dc, &row);
        return CDRF_SKIPDEFAULT;
    }

    HWND table(Window& window, int id, int x, int y, int w, int h,
               std::initializer_list<std::pair<const wchar_t*, int>> columns)
    {
        HWND control = widget(window, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                              id, x, y, w, h, WS_EX_CLIENTEDGE);
        ListView_SetBkColor(control, palette.field);
        ListView_SetTextBkColor(control, palette.field);
        ListView_SetTextColor(control, palette.text);
        if (dark) {
            SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
            SetWindowTheme(ListView_GetHeader(control), L"DarkMode_ItemsView", nullptr);
            SetWindowSubclass(control, dark_table, 1, 0);
        }
        ListView_SetExtendedListViewStyle(control, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        int index = 0;
        for (const auto& [title, width] : columns) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.cx = MulDiv(width, window.dpi, 96);
            column.pszText = const_cast<wchar_t*>(title);
            ListView_InsertColumn(control, index++, &column);
        }
        return control;
    }

    static void row(HWND control, int index, std::initializer_list<std::wstring> fields)
    {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = index;
        item.pszText = const_cast<wchar_t*>(fields.begin()->c_str());
        int row_id = ListView_InsertItem(control, &item);
        int column = 0;
        for (const auto& field : fields) {
            if (column)
                ListView_SetItemText(control, row_id, column, const_cast<wchar_t*>(field.c_str()));
            ++column;
        }
    }

    static int selected(HWND control)
    {
        return ListView_GetNextItem(control, -1, LVNI_SELECTED);
    }

    void show(Window& window, bool focus)
    {
        ShowWindow(window.hwnd, focus ? SW_SHOW : SW_SHOWNOACTIVATE);
        if (focus) {
            SetForegroundWindow(window.hwnd);
            if (window.focus) SetFocus(window.focus);
        }
    }

    void heading(Window& window, const wchar_t* title)
    {
        window.heading_label = label(window, title, 24, 20, 402, 34);
        if (window.heading)
            SendMessageW(window.heading_label, WM_SETFONT, reinterpret_cast<WPARAM>(window.heading), TRUE);
    }

    void prompt(Progress entry, bool reopen)
    {
        close(prompt_window);
        if (!create(prompt_window, L"Continue watching", 476, 216))
            return;
        progress = std::move(entry);
        heading(prompt_window, L"Continue watching");
        const auto title = widget(prompt_window, L"STATIC", progress.title.c_str(),
                                  SS_NOPREFIX | SS_WORDELLIPSIS, 0, 24, 61, 428, 43);
        const auto time = clock_duration(progress.position);
        prompt_window.secondary_label = label(prompt_window, progress.duration > 0 ?
            time + L" / " + clock_duration(progress.duration) : time, 24, 107, 428);
        prompt_window.progress_fraction = progress.duration > 0 ?
            std::clamp(static_cast<double>(progress.position) / progress.duration, 0.0, 1.0) : -1.0;
        button(prompt_window, (std::wstring(reopen ? L"Open at " : L"Continue at ") + time).c_str(), confirm, 24, 153, 220, true);
        button(prompt_window, L"Close", dismiss, 344, 153, 108);
        if (title) show(prompt_window, false);
    }

    void bookmark_form(Window& window, const Bookmark& item, bool editing)
    {
        heading(window, editing ? L"Edit bookmark" : L"Add bookmark");
        widget(window, L"STATIC", item.title.c_str(), SS_NOPREFIX | SS_WORDELLIPSIS, 0, 24, 61, 428, 42);
        window.secondary_label = label(window, L"Saved position  " + clock_duration(item.position), 24, 105, 428);
        label(window, L"Comment", 24, 145, 428);
        window.focus = input(window, item.comment.c_str(), notes, 24, 169, 428, 82, true);
        button(window, L"Save bookmark", confirm, 24, 268, 167, true);
        button(window, L"Cancel", dismiss, 344, 268, 108);
        show(window, true);
    }

    void add(const Bookmark& item)
    {
        if (create(add_window, L"Add bookmark", 476, 332))
            bookmark_form(add_window, item, false);
        else if (!add_window.hwnd)
            callbacks.bookmark_closed(false);
    }

    void refresh_library()
    {
        const auto filter = sort_key(get_text(GetDlgItem(library_window.hwnd, search)));
        HWND control = GetDlgItem(library_window.hwnd, list);
        SendMessageW(control, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(control);
        visible.clear();
        for (std::size_t i = library.size(); i > 0; --i) {
            const auto& item = library[i - 1];
            if (!filter.empty() && sort_key(item.title + L" " + item.comment).find(filter) == std::wstring::npos)
                continue;
            std::error_code ec;
            const bool available = fs::is_regular_file(fs::path(item.path), ec);
            row(control, static_cast<int>(visible.size()), {item.title, clock_duration(item.position),
                item.comment, available ? L"Available" : L"File unavailable"});
            visible.push_back(i - 1);
        }
        if (!visible.empty())
            ListView_SetItemState(control, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(control, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(control, nullptr, FALSE);
    }

    void show_bookmarks(const std::vector<Bookmark>& items)
    {
        close(edit_window);
        library = items;
        if (create(library_window, L"All bookmarks", 752, 414)) {
            label(library_window, L"Search by video title or comment", 14, 10, 710);
            library_window.focus = input(library_window, L"", search, 14, 32, 724, 25);
            table(library_window, list, 14, 68, 724, 294,
                  {{L"Video", 240}, {L"Time", 75}, {L"Comment", 275}, {L"File", 115}});
            button(library_window, L"Open video", open_item, 14, 374, 106, true);
            button(library_window, L"View or edit", edit_item, 128, 374, 106);
            button(library_window, L"Delete", remove_item, 242, 374, 86);
            button(library_window, L"Close", dismiss, 652, 374, 86);
            show(library_window, true);
        }
        if (library_window.hwnd) refresh_library();
    }

    void edit(std::size_t index)
    {
        if (!create(edit_window, L"Edit bookmark", 476, 332, library_window.hwnd))
            return;
        edited = index;
        bookmark_form(edit_window, library[index], true);
    }

    void refresh_bindings(int selected_row)
    {
        HWND control = GetDlgItem(settings_window.hwnd, bind_list);
        ListView_DeleteAllItems(control);
        for (unsigned i = 0; i < draft.bindings.size(); ++i)
            row(control, static_cast<int>(i), {wide(action_names[i]), binding_name(draft.bindings[i])});
        ListView_SetItemState(control, selected_row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    void show_settings(const Settings& value)
    {
        if (!create(settings_window, L"VLC WatchKit Settings", 486, 466))
            return;
        draft = value;
        label(settings_window, L"Discord Application ID", 14, 12, 206);
        label(settings_window, L"Large image key (optional)", 248, 12, 220);
        settings_window.focus = input(settings_window, wide(value.application_id).c_str(), application, 14, 35, 220, 24);
        input(settings_window, wide(value.large_image).c_str(), image, 248, 35, 224, 24);
        HWND check = widget(settings_window, L"BUTTON", L"Share playback and pause status", WS_TABSTOP | BS_AUTOCHECKBOX,
                            publish, 14, 66, dark ? 16 : 356, 25);
        if (dark)
            widget(settings_window, L"STATIC", L"Share playback and pause status", SS_NOTIFY, publish_label,
                   34, 70, 345, 22);
        SendMessageW(check, BM_SETCHECK, value.discord_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        label(settings_window, L"Keyboard shortcuts", 14, 106, 442);
        table(settings_window, bind_list, 14, 128, 458, 240, {{L"Action", 320}, {L"Shortcut", 118}});
        refresh_bindings(0);
        button(settings_window, L"Change shortcut", change_bind, 14, 379, 124);
        button(settings_window, L"Restore defaults", reset_binds, 146, 379, 126);
        button(settings_window, L"Save", save_settings, 282, 425, 92, true);
        button(settings_window, L"Cancel", dismiss, 382, 425, 90);
        show(settings_window, true);
    }

    void ask_binding()
    {
        const int index = selected(GetDlgItem(settings_window.hwnd, bind_list));
        if (index < 0 || !create(capture_window, L"Change shortcut", 330, 84, settings_window.hwnd))
            return;
        binding_index = static_cast<unsigned>(index);
        label(capture_window, L"Press the new key or key combination.", 14, 12, 302);
        label(capture_window, L"Press Esc to cancel.", 14, 42, 302);
        show(capture_window, true);
    }

    void assign(Binding binding)
    {
        if (!supported_binding(binding)) return;
        const auto existing = std::find(draft.bindings.begin(), draft.bindings.end(), binding);
        if (existing != draft.bindings.end() && existing != draft.bindings.begin() + binding_index) {
            MessageBoxW(capture_window.hwnd, L"That shortcut is already assigned to another VLC WatchKit action.",
                        L"Shortcut already used", MB_OK | MB_ICONINFORMATION);
            return;
        }
        draft.bindings[binding_index] = binding;
        close(capture_window);
        refresh_bindings(static_cast<int>(binding_index));
    }

    void save()
    {
        draft.application_id = utf8(get_text(GetDlgItem(settings_window.hwnd, application)));
        draft.large_image = utf8(get_text(GetDlgItem(settings_window.hwnd, image)));
        draft.discord_enabled = SendMessageW(GetDlgItem(settings_window.hwnd, publish), BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!draft.application_id.empty() && !valid_application_id(draft.application_id)) {
            MessageBoxW(settings_window.hwnd, L"Enter the numeric Application ID from the Discord Developer Portal.",
                        L"Invalid Application ID", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (callbacks.save_settings(draft))
            close(settings_window);
        else
            report_save_error(settings_window.hwnd);
    }

    static void report_save_error(HWND window)
    {
        MessageBoxW(window, L"VLC WatchKit could not save your changes. Check the local data directory.",
                    L"Save failed", MB_OK | MB_ICONERROR);
    }

    void cancel(Window& window)
    {
        close(window);
        if (window.kind == Kind::add) callbacks.bookmark_closed(false);
        if (window.kind == Kind::prompt) callbacks.prompt_closed();
    }

    void command(Window& window, int id, int notification)
    {
        if (id == IDCANCEL || id == dismiss) {
            cancel(window);
            return;
        }
        switch (window.kind) {
            case Kind::prompt:
                if (id == confirm || id == IDOK) {
                    auto entry = progress;
                    close(window);
                    callbacks.resume(std::move(entry));
                }
                break;
            case Kind::add:
                if (id == confirm || id == IDOK) {
                    callbacks.bookmark_saved(get_text(GetDlgItem(window.hwnd, notes)));
                    close(window);
                    callbacks.bookmark_closed(true);
                }
                break;
            case Kind::edit:
                if (id == confirm || id == IDOK) {
                    auto comment = get_text(GetDlgItem(window.hwnd, notes));
                    if (callbacks.edit_bookmark(edited, comment)) {
                        library[edited].comment = std::move(comment);
                        close(window);
                        refresh_library();
                    } else report_save_error(window.hwnd);
                }
                break;
            case Kind::library:
                library_command(id, notification);
                break;
            case Kind::settings:
                if (id == publish_label && notification == STN_CLICKED)
                    SendMessageW(GetDlgItem(window.hwnd, publish), BM_CLICK, 0, 0);
                if (id == save_settings || id == IDOK) save();
                if (id == change_bind) ask_binding();
                if (id == reset_binds) {
                    draft.bindings = default_bindings();
                    refresh_bindings(0);
                }
                break;
            case Kind::speed:
                if (id == confirm || id == IDOK) apply_speed();
                break;
            case Kind::capture:
            case Kind::status: break;
        }
    }

    void library_command(int id, int notification)
    {
        if (id == search && notification == EN_CHANGE) {
            if (GetDlgItem(library_window.hwnd, list)) refresh_library();
            return;
        }
        if (id != open_item && id != edit_item && id != remove_item && id != IDOK)
            return;
        const auto index = selected(GetDlgItem(library_window.hwnd, list));
        if (index < 0 || static_cast<std::size_t>(index) >= visible.size()) return;
        const auto item = visible[index];
        if (id == edit_item) {
            edit(item);
        } else if (id == remove_item) {
            if (MessageBoxW(library_window.hwnd, L"Delete this bookmark?", L"Delete bookmark",
                            MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES) {
                if (callbacks.remove_bookmark(item)) {
                    library.erase(library.begin() + item);
                    refresh_library();
                } else report_save_error(library_window.hwnd);
            }
        } else {
            std::error_code ec;
            if (!fs::is_regular_file(fs::path(library[item].path), ec)) {
                MessageBoxW(library_window.hwnd, L"The video file is unavailable. Your bookmark is still saved.",
                            L"File unavailable", MB_OK | MB_ICONINFORMATION);
            } else {
                callbacks.open_bookmark(library[item]);
                close(library_window);
            }
        }
    }

    void process(MSG& message)
    {
        const bool key = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
        if (capture_window.hwnd && GetAncestor(message.hwnd, GA_ROOT) == capture_window.hwnd && key) {
            if (message.wParam == VK_ESCAPE) close(capture_window);
            else assign(key_message(message));
            return;
        }
        for (auto* window : {&edit_window, &add_window, &library_window, &settings_window, &speed_window, &prompt_window}) {
            if (window->hwnd && GetAncestor(message.hwnd, GA_ROOT) == window->hwnd) {
                if (key && message.wParam == VK_RETURN && (window->kind == Kind::add || window->kind == Kind::edit || window->kind == Kind::speed)
                    && GetKeyState(VK_SHIFT) >= 0) {
                    command(*window, confirm, 0);
                    return;
                }
                if (IsDialogMessageW(window->hwnd, &message))
                    return;
                break;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    UiCallbacks callbacks;
    const bool dark;
    const Palette& palette;
    HINSTANCE module;
    HWND owner = nullptr;
    HBRUSH surface_brush = CreateSolidBrush(palette.surface), field_brush = CreateSolidBrush(palette.field);
    bool registered = false;
    Window prompt_window, add_window, library_window, edit_window, settings_window, capture_window, status_window, speed_window;
    HWND status_tip = nullptr;
    RECT speed_area{};
    std::wstring status_text;
    bool status_on_toolbar = false, status_compact = false;
    COLORREF status_foreground = CLR_INVALID;
    BYTE status_opacity = 255;
    Progress progress{};
    std::vector<Bookmark> library;
    std::vector<std::size_t> visible;
    std::size_t edited = 0;
    Settings draft;
    unsigned binding_index = 0;
};

Ui::Ui(UiCallbacks callbacks, bool dark) : impl_(std::make_unique<Impl>(std::move(callbacks), dark)) {}
Ui::~Ui() = default;
void Ui::owner(HWND window) { impl_->owner = window; }
void Ui::prompt(Progress progress, bool reopen) { impl_->prompt(std::move(progress), reopen); }
void Ui::hide_prompt() { impl_->close(impl_->prompt_window); }
void Ui::reposition_prompt()
{
    if (const auto window = impl_->prompt_window.hwnd; window && impl_->owner) {
        if (GetWindow(window, GW_OWNER) != impl_->owner)
            SetWindowLongPtrW(window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(impl_->owner));
        impl_->center(window, impl_->owner);
    }
}
void Ui::add_bookmark(const Bookmark& bookmark) { impl_->add(bookmark); }
void Ui::show_bookmarks(const std::vector<Bookmark>& bookmarks) { impl_->show_bookmarks(bookmarks); }
void Ui::show_settings(const Settings& settings) { impl_->show_settings(settings); }
void Ui::playback_status(HWND player, std::wstring text, bool fullscreen) { impl_->playback_status(player, std::move(text), fullscreen); }
void Ui::process(MSG& message) { impl_->process(message); }
}
