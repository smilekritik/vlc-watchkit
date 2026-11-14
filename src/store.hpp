#pragma once

#include "model.hpp"
#include <array>
#include <string>
#include <unordered_map>

namespace addon {
enum class Action : unsigned {
    discord, bookmark, bookmarks, speed_up, speed_down, speed_normal, speed_fast, speed_slow, time, count
};
struct Binding {
    unsigned key{};
    unsigned modifiers{};
    bool operator==(const Binding&) const = default;
};
inline constexpr unsigned shift = 1, control = 2, alt = 4;
using Bindings = std::array<Binding, static_cast<unsigned>(Action::count)>;
inline constexpr std::array action_names{
    "Toggle Discord status", "Add a bookmark", "Show all bookmarks", "Increase speed by 0.05x",
    "Decrease speed by 0.05x", "Reset speed to 1.00x", "Increase speed (large step)",
    "Decrease speed (large step)", "Show playback time"
};
Bindings default_bindings();
bool supported_binding(Binding binding);
std::wstring binding_name(Binding binding);

struct Settings {
    bool discord_enabled = true;
    int video_speed = 100;
    std::string application_id;
    std::string large_image;
    std::string preview_upload_url;
    Bindings bindings = default_bindings();
};
bool valid_application_id(std::string_view id);

struct Bookmark {
    std::wstring path;
    std::wstring title;
    MediaTime position;
    std::wstring comment;
};

struct Progress {
    std::wstring path;
    std::wstring title;
    MediaTime position;
    MediaTime duration;
    std::int64_t updated;
};

class Store {
public:
    explicit Store(fs::path directory);
    Settings settings;
    std::vector<Bookmark> bookmarks;
    std::unordered_map<std::wstring, Progress> positions;
    bool save_settings() const;
    bool save_bookmarks() const;
    bool save_progress() const;
    [[nodiscard]] bool has_read_errors() const { return read_error_; }
private:
    fs::path directory_;
    bool read_error_ = false;
    bool settings_readable_ = true;
    bool bookmarks_readable_ = true;
    bool progress_readable_ = true;
};
}
