#include "model.hpp"
#include "store.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <windows.h>

using namespace addon;
namespace {
void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void touch(const fs::path& path) { std::ofstream(path).put('x'); }

struct Fixture {
    fs::path path = fs::temp_directory_path() / (L"vlc-addon-unit-" + std::to_wstring(GetCurrentProcessId()));
    Fixture() { fs::create_directory(path); }
    ~Fixture()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void episodes(const fs::path& directory)
{
    auto current = directory / L"[Group] Series.Name.S01E03.1080p.mkv";
    auto fourth = directory / L"series-name S01E04.mp4";
    touch(current);
    touch(fourth);
    touch(directory / L"Series_Name_S01E06.mkv");
    touch(directory / L"Other Series S01E05.mkv");
    touch(directory / L"Series Name S02E05.mkv");
    touch(directory / L"Series Name S01E05.srt");
    check(following_episodes(current) == std::vector{fourth}, "episode order and first gap");
    touch(directory / L"Series Name S01E04 (alternative).mkv");
    check(following_episodes(current).empty(), "stop on ambiguous episode");
    check(episode_label("01", "04") == "S1 Ep.4", "compact season and episode in Discord");
    check(activity_video_title("MobLand S01E04", "S1 Ep.4") == "MobLand", "remove duplicate episode code");
    check(activity_video_title("MobLand S01E04 - Episode title", "S1 Ep.4") == "MobLand - Episode title",
          "preserve the episode title");
    check(activity_video_title("MobLand S01E04", "S1 Ep.5") == "MobLand S01E04",
          "preserve episode codes that differ from the label");
    check(episode_label("", "04") == "Ep.4", "episode without season metadata");
    check(!episode_from_filename(L"Series S01E03-E04.mkv"), "ignore multi-episode video");
    check(!episode_from_filename(L"Movie 2024 1080p.mkv"), "ignore standalone films");
}

void persistence(const fs::path& directory)
{
    const auto video = directory / L"deleted.mp4";
    Store original(directory);
    original.settings.discord_enabled = false;
    original.settings.video_speed = 125;
    original.settings.bindings[0] = {VK_F11, control};
    original.bookmarks.push_back({video.wstring(), L"Example \u2603", 22 * second, L"Remember this \u0416"});
    original.positions.emplace(video.wstring(), Progress{video.wstring(), L"Example", 7 * second, 30 * second, 1});
    check(original.save_settings() && original.save_bookmarks() && original.save_progress(), "save local data");

    Store restored(directory);
    check(!restored.has_read_errors(), "read settings and history");
    check(!restored.settings.discord_enabled && restored.settings.video_speed == 125, "restore speed and Discord toggle");
    check(restored.settings.bindings[0] == Binding{VK_F11, control}, "restore custom shortcut");
    check(restored.positions.at(video.wstring()).position == 7 * second, "restore position");
    check(restored.bookmarks.front().comment == L"Remember this \u0416", "retain Unicode bookmark without a file");
    check(!fs::exists(video), "bookmark fixture refers to a missing file");

    std::ofstream(directory / L"settings.json") << "{ broken";
    Store malformed(directory);
    check(malformed.has_read_errors() && !malformed.settings.discord_enabled, "fail closed on corrupt settings");
    check(!malformed.save_settings(), "retain corrupt settings for recovery");
}

void rich_presence(const fs::path& directory)
{
    check(activity_playback_status(125, 22 * 60 * second, 8 * 60 * second) ==
              L"08:00 / 22:00 \u00b7 Speed 1.25\u00d7",
          "share the media clock and non-default speed");
    check(activity_playback_status(150, 186 * second, 107 * second) ==
              L"01:47 / 03:06 \u00b7 Speed 1.5\u00d7",
          "keep the actual duration at 1.5x");
    check(activity_playback_status(1000, 22 * 60 * second, 8 * 60 * second) ==
              L"08:00 / 22:00 \u00b7 Speed 10\u00d7",
          "drop trailing decimal zeros for integer speeds");
    check(activity_playback_status(100, 3601 * second, 2 * second) ==
              L"0:00:02 / 1:00:01",
          "keep hour fields and hide normal speed");
    check(playback_status(125, 22 * 60 * second, 8 * 60 * second) == L"1.25\u00d7  \u00B7  11:12 left",
          "keep the speed and remaining time in VLC's controls");
    check(remaining_playback_time(125, 22 * 60 * second, 8 * 60 * second) == 672 * second,
          "adjust the local countdown for playback speed");
    check(remaining_playback_time(125, 3 * second, 4 * second) == 0, "clamp completed videos");
    check(remaining_playback_time(125, 0, 3 * second) == 0, "omit countdown for unknown duration");
    check(playback_time_left(150, 186 * second, 107 * second) == L"00:53 left",
          "use real time left at the chosen speed in the artwork tooltip");
    check(playback_time_left(100, 0, 107 * second).empty(), "omit the artwork tooltip for unknown duration");

    constexpr MediaTime sample = 1'800'000'000 * second;
    constexpr MediaTime length = 186 * second;
    const auto progress = activity_timestamps(100, length, 107 * second, sample, true);
    check(progress.start == 1'799'999'893 && progress.end == 1'800'000'079, "use the media clock as the Discord progress range");
    const auto next = activity_timestamps(100, length, 112 * second, sample + 5 * second, true);
    check(next.start == progress.start && next.end == progress.end, "keep the same range during steady playback");
    const auto seek = activity_timestamps(100, length, 150 * second, sample, true);
    check(seek.start == 1'799'999'850 && seek.end == 1'800'000'036, "move the range when the player seeks");
    check(activity_timestamps(150, length, 107 * second, sample, true).end == 0 &&
          activity_timestamps(100, length, 107 * second, sample, false).end == 0 &&
          activity_timestamps(100, 0, 107 * second, sample, true).end == 0,
          "omit the moving progress range at non-default speed, on pause, and without a duration");

    auto file = directory / L"Episode.mp4";
    std::ofstream(directory / L"Episode.mp4.discord-previews.txt")
        << "0 opening\n1 https://example.test/minute-1.jpg\n2 file:///private.jpg\n3 local-frame\n";
    auto images = load_preview_images(file);
    check(preview_image(images, 59 * second, "vlc") == "opening", "select opening image");
    check(preview_image(images, 60 * second, "vlc") == "https://example.test/minute-1.jpg", "rotate at one minute");
    check(preview_image(images, 120 * second, "vlc") == "vlc", "fall back when no allowed preview exists");
    check(preview_image(images, 180 * second, "vlc") == "local-frame", "use registered Discord asset");

    Store data(directory);
    data.settings.bindings[0] = {VK_F9, 0};
    data.settings.bindings[1] = {VK_F10, 0};
    data.settings.bindings[2] = {VK_F12, control};
    check(data.save_settings(), "write shortcut fixture");
    const auto config = directory / L"settings.json";
    std::string saved;
    {
        std::ifstream stream(config);
        saved.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    const auto version = saved.find("\"version\": 2");
    check(version != std::string::npos, "use current settings version");
    saved[version + 11] = '1';
    std::ofstream(config) << saved;
    Store upgraded(directory);
    check(!upgraded.has_read_errors() && upgraded.settings.bindings[0] == Binding{VK_MULTIPLY, 0} &&
          upgraded.settings.bindings[1] == Binding{VK_NUMPAD9, 0} &&
          upgraded.settings.bindings[2] == Binding{VK_F12, control}, "migrate old default hotkeys and keep custom ones");
}

void controls()
{
    check(speed_step(120, 5) == 125 && speed_step(6, -5) == 5, "fine speed steps");
    check(speed_step(1600, 5) == 1605 && speed_step(3124, 5) == 3125, "fine steps above 4x");
    check(large_speed_step(125, true) == 175 && large_speed_step(175, false) == 125, "large additive steps");
    check(large_speed_step(100, false) == 50 && large_speed_step(50, false) == 25 &&
          large_speed_step(25, false) == 12 && large_speed_step(6, false) == 5, "halving steps and floor");
    check(large_speed_step(400, true) == 800 && large_speed_step(800, true) == 1600 &&
          large_speed_step(1600, false) == 800 && large_speed_step(800, false) == 400 &&
          large_speed_step(1600, true) == 3125, "doubling steps and VLC ceiling");
    check(parse_speed(L"16") == 1600 && parse_speed(L"0,05\u00d7") == 5 &&
          parse_speed(L"31.25") == 3125 && !parse_speed(L"0") && !parse_speed(L"32"), "custom VLC speeds");
    check(clock_duration(3661 * second) == L"1:01:01", "format a long timestamp");
    check(playback_summary(125, 45 * second, 5 * second, false).find(L"00:32 remaining") != std::wstring::npos,
          "remaining time accounts for current speed");
    check(default_bindings()[6].key == VK_ADD && default_bindings()[7].key == VK_SUBTRACT, "use numeric keypad");
    check(binding_name({VK_ADD, 0}) == L"NumPad +", "show keypad name");
    check(valid_application_id("123456789012345678") && !valid_application_id("12345"), "validate Discord ID");
}
}

int main()
{
    try {
        Fixture fixture;
        episodes(fixture.path);
        rich_presence(fixture.path);
        persistence(fixture.path);
        controls();
        std::cout << "Episode selection, persistence, shortcuts, speed and time checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
