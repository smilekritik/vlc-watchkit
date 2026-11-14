#pragma once
#include "store.hpp"
#include <windows.h>
#include <unordered_map>

namespace addon {
inline constexpr UINT hotkey_message = WM_APP + 0x44;
inline constexpr UINT window_changed_message = WM_APP + 0x45;

HWND fullscreen_controls(HWND player);

class PlayerWindows {
public:
    explicit PlayerWindows(DWORD receiver);
    ~PlayerWindows();
    void bindings(const Bindings& value);
    HWND refresh();
private:
    HWND owner_ = nullptr;
    HWINEVENTHOOK foreground_ = nullptr, movement_ = nullptr, visibility_ = nullptr;
    struct Hooks {
        HHOOK keyboard;
        HHOOK cursor;
    };
    std::unordered_map<DWORD, Hooks> hooks_;
};
}
