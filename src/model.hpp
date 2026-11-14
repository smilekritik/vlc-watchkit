#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace addon {
namespace fs = std::filesystem;
using MediaTime = std::int64_t;
inline constexpr MediaTime second = 1'000'000;

struct Episode {
    std::wstring series;
    int season;
    int number;
};

bool video_file(const fs::path& file);
std::string episode_label(std::string_view season, std::string_view episode);
std::string activity_video_title(std::string_view title, std::string_view label);
std::optional<Episode> episode_from_filename(const fs::path& file);
std::vector<fs::path> following_episodes(const fs::path& current);
// Hundredths of a multiplier, rounded from VLC 3.0.24's native rate bounds.
inline constexpr int min_manual_speed = 3;
inline constexpr int max_video_speed = 3125;
inline constexpr int min_step_speed = 5;
int speed_step(int current, int step);
int large_speed_step(int current, bool faster);
std::optional<int> parse_speed(std::wstring_view text);
using PreviewImages = std::map<MediaTime, std::string>;
PreviewImages load_preview_images(const fs::path& file);
std::string preview_image(const PreviewImages& images, MediaTime position, std::string_view fallback);
MediaTime remaining_playback_time(int rate, MediaTime length, MediaTime position);
std::wstring playback_status(int rate, MediaTime length, MediaTime position);
std::wstring activity_playback_status(int rate, MediaTime length, MediaTime position);
std::wstring clock_duration(MediaTime time);
std::wstring playback_summary(int rate, MediaTime length, MediaTime position, bool playing);
std::string utf8(std::wstring_view value);
std::wstring wide(std::string_view value);
}
