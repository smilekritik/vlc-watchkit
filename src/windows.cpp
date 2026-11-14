#include "windows.hpp"

#include <oleacc.h>

namespace addon {
namespace {
SRWLOCK key_lock = SRWLOCK_INIT;
Bindings shortcuts = default_bindings();
DWORD receiver = 0;

bool player_window(HWND window, bool visible = true)
{
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || (visible && !IsWindowVisible(window)))
        return false;
    wchar_t name[128]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return wcscmp(name, L"VLCAddonWindow") != 0 &&
        (wcsstr(name, L"Qt") || wcsstr(name, L"VLC") || wcsstr(name, L"vlc"));
}

LRESULT CALLBACK read_key(int code, WPARAM removed, LPARAM param)
{
    if (code != HC_ACTION || removed != PM_REMOVE)
        return CallNextHookEx(nullptr, code, removed, param);
    auto& message = *reinterpret_cast<MSG*>(param);
    if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN)
        return CallNextHookEx(nullptr, code, removed, param);
    const auto window = GetAncestor(message.hwnd, GA_ROOT);
    if (!player_window(window))
        return CallNextHookEx(nullptr, code, removed, param);
    const Binding pressed{static_cast<unsigned>(message.wParam), (GetKeyState(VK_SHIFT) < 0 ? shift : 0) |
        (GetKeyState(VK_CONTROL) < 0 ? control : 0) | (GetKeyState(VK_MENU) < 0 ? alt : 0)};
    AcquireSRWLockShared(&key_lock);
    for (unsigned i = 0; receiver && i < shortcuts.size(); ++i) {
        if (shortcuts[i] != pressed) continue;
        const auto action = static_cast<Action>(i);
        const bool repeated = (message.lParam & (1L << 30)) != 0;
        if (!repeated || (action >= Action::speed_up && action <= Action::speed_slow))
            PostThreadMessageW(receiver, hotkey_message, i, reinterpret_cast<LPARAM>(window));
        message.message = WM_NULL;
        break;
    }
    ReleaseSRWLockShared(&key_lock);
    return CallNextHookEx(nullptr, code, removed, param);
}

bool controller_candidate(HWND window, HWND player)
{
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != player)
        return false;
    if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0)
        return false;

    // VLC's floating controller is a frameless Qt tool owned by its main window.
    wchar_t name[64]{};
    if (!GetClassNameW(window, name, static_cast<int>(std::size(name))) ||
        wcscmp(name, L"Qt5QWindowToolSaveBits") != 0)
        return false;
    RECT rect{};
    if (!GetClientRect(window, &rect)) return false;
    const auto dpi = GetDpiForWindow(window);
    return rect.right >= MulDiv(360, dpi, 96) &&
        rect.bottom >= MulDiv(42, dpi, 96) && rect.bottom <= MulDiv(130, dpi, 96);
}

bool enabled_button_at(POINT point)
{
    IAccessible* accessible = nullptr;
    VARIANT child{};
    if (FAILED(AccessibleObjectFromPoint(point, &accessible, &child)) || !accessible)
        return false;

    VARIANT role{}, state{};
    LONG left = 0, top = 0, width = 0, height = 0;
    const bool button = SUCCEEDED(accessible->get_accRole(child, &role)) && role.vt == VT_I4 &&
        (role.lVal == ROLE_SYSTEM_PUSHBUTTON || role.lVal == ROLE_SYSTEM_CHECKBUTTON ||
         role.lVal == ROLE_SYSTEM_BUTTONDROPDOWN || role.lVal == ROLE_SYSTEM_BUTTONMENU) &&
        SUCCEEDED(accessible->get_accState(child, &state)) && state.vt == VT_I4 &&
        !(state.lVal & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) &&
        SUCCEEDED(accessible->accLocation(&left, &top, &width, &height, child)) &&
        width > 0 && height > 0 && point.x >= left && point.x - left < width &&
        point.y >= top && point.y - top < height;
    VariantClear(&state);
    VariantClear(&role);
    VariantClear(&child);
    accessible->Release();
    return button;
}

LRESULT CALLBACK update_cursor(int code, WPARAM wp, LPARAM lp)
{
    const auto next = CallNextHookEx(nullptr, code, wp, lp);
    if (code != HC_ACTION)
        return next;

    const auto& message = *reinterpret_cast<const CWPRETSTRUCT*>(lp);
    if (message.message != WM_SETCURSOR || LOWORD(message.lParam) != HTCLIENT ||
        HIWORD(message.lParam) != WM_MOUSEMOVE)
        return next;

    // Query Qt on its own GUI thread. MSAA may send a message back to that window.
    thread_local bool querying = false;
    if (querying)
        return next;
    wchar_t name[64]{};
    if (!GetClassNameW(message.hwnd, name, static_cast<int>(std::size(name))) ||
        wcsncmp(name, L"Qt", 2) != 0)
        return next;

    POINT point{};
    if (!GetCursorPos(&point))
        return next;
    querying = true;
    const bool button = enabled_button_at(point);
    querying = false;
    const auto hand = LoadCursorW(nullptr, IDC_HAND);
    thread_local bool set_hand = false;
    if (button) {
        SetCursor(hand);
    } else if (set_hand && GetCursor() == hand) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
    set_hand = button;
    return next;
}

void CALLBACK window_event(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD)
{
    if (!hwnd || object != OBJID_WINDOW || child != CHILDID_SELF ||
        (event != EVENT_SYSTEM_FOREGROUND &&
         (GetAncestor(hwnd, GA_ROOT) != hwnd || !player_window(hwnd, false))))
        return;
    AcquireSRWLockShared(&key_lock);
    if (receiver) PostThreadMessageW(receiver, window_changed_message, 0, 0);
    ReleaseSRWLockShared(&key_lock);
}

BOOL CALLBACK list_windows(HWND window, LPARAM data)
{
    if (player_window(window, false))
        reinterpret_cast<std::vector<HWND>*>(data)->push_back(window);
    return TRUE;
}
}

HWND fullscreen_controls(HWND player)
{
    struct Search {
        HWND player;
        HWND controls = nullptr;
    } search{player};
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto& result = *reinterpret_cast<Search*>(data);
        if (!controller_candidate(window, result.player))
            return TRUE;
        result.controls = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.controls;
}

PlayerWindows::PlayerWindows(DWORD target)
{
    AcquireSRWLockExclusive(&key_lock);
    receiver = target;
    ReleaseSRWLockExclusive(&key_lock);
    foreground_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, window_event,
                                 GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    movement_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, window_event,
                               GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    visibility_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, window_event,
                                 GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
}

PlayerWindows::~PlayerWindows()
{
    AcquireSRWLockExclusive(&key_lock);
    receiver = 0;
    ReleaseSRWLockExclusive(&key_lock);
    if (visibility_) UnhookWinEvent(visibility_);
    if (movement_) UnhookWinEvent(movement_);
    if (foreground_) UnhookWinEvent(foreground_);
    for (const auto& [thread, hooks] : hooks_) {
        if (hooks.cursor) UnhookWindowsHookEx(hooks.cursor);
        if (hooks.keyboard) UnhookWindowsHookEx(hooks.keyboard);
    }
}

void PlayerWindows::bindings(const Bindings& value)
{
    AcquireSRWLockExclusive(&key_lock);
    shortcuts = value;
    ReleaseSRWLockExclusive(&key_lock);
}

HWND PlayerWindows::refresh()
{
    std::vector<HWND> windows;
    EnumWindows(list_windows, reinterpret_cast<LPARAM>(&windows));
    for (const auto window : windows) {
        const auto thread = GetWindowThreadProcessId(window, nullptr);
        if (!thread || hooks_.contains(thread))
            continue;
        Hooks hooks{SetWindowsHookExW(WH_GETMESSAGE, read_key, nullptr, thread),
                    SetWindowsHookExW(WH_CALLWNDPROCRET, update_cursor, nullptr, thread)};
        if (hooks.keyboard || hooks.cursor)
            hooks_.emplace(thread, hooks);
    }
    for (auto it = hooks_.begin(); it != hooks_.end();) {
        bool live = false;
        for (const auto window : windows) {
            if (GetWindowThreadProcessId(window, nullptr) == it->first) live = true;
        }
        if (!live) {
            if (it->second.cursor) UnhookWindowsHookEx(it->second.cursor);
            if (it->second.keyboard) UnhookWindowsHookEx(it->second.keyboard);
            it = hooks_.erase(it);
        } else ++it;
    }
    const auto foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (player_window(foreground)) {
        const auto parent = GetWindow(foreground, GW_OWNER);
        if (!controller_candidate(foreground, parent))
            owner_ = foreground;
    }
    if (!player_window(owner_))
        for (const auto window : windows)
            if (player_window(window) && !controller_candidate(window, GetWindow(window, GW_OWNER))) { owner_ = window; break; }
    return player_window(owner_) ? owner_ : nullptr;
}
}
