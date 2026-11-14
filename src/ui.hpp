#pragma once

#include "store.hpp"
#include <functional>
#include <memory>
#include <windows.h>

namespace addon {
struct UiCallbacks {
    std::function<void(std::wstring)> bookmark_saved;
    std::function<void(bool)> bookmark_closed;
    std::function<void(Bookmark)> open_bookmark;
    std::function<bool(std::size_t, std::wstring)> edit_bookmark;
    std::function<bool(std::size_t)> remove_bookmark;
    std::function<bool(Settings)> save_settings;
    std::function<void(Progress)> resume;
    std::function<void()> prompt_closed;
    std::function<void(int)> set_speed;
};

class Ui {
public:
    explicit Ui(UiCallbacks callbacks, bool dark);
    ~Ui();
    void owner(HWND window);
    void prompt(Progress progress, bool reopen);
    void hide_prompt();
    void reposition_prompt();
    void add_bookmark(const Bookmark& bookmark);
    void show_bookmarks(const std::vector<Bookmark>& bookmarks);
    void show_settings(const Settings& settings);
    void process(MSG& message);
    void playback_status(HWND player, std::wstring text, bool fullscreen = false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
