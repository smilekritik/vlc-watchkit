#include "model.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <cwchar>
#include <ctime>
#include <fstream>
#include <map>
#include <regex>

namespace addon {
namespace {
std::wstring lowercase(std::wstring value)
{
    if (!value.empty())
        CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

std::wstring series_key(std::wstring name)
{
    static const std::wregex group(LR"(^\s*\[[^\]]+\]\s*)");
    static const std::wregex separators(LR"([\s._\-]+)");
    name = std::regex_replace(name, group, L"");
    name = std::regex_replace(name, separators, L" ");
    auto start = name.find_first_not_of(L' ');
    if (start == std::wstring::npos)
        return {};
    return lowercase(name.substr(start, name.find_last_not_of(L' ') - start + 1));
}
}

bool video_file(const fs::path& file)
{
    constexpr std::array<std::wstring_view, 23> extensions{
        L".avi", L".divx", L".flv", L".m2ts", L".m4v", L".mkv", L".mov", L".mp4",
        L".mpeg", L".mpg", L".mts", L".ogm", L".ogv", L".rm", L".rmvb", L".ts",
        L".vob", L".webm", L".wmv", L".3gp", L".asf", L".mxf", L".f4v"
    };
    return std::ranges::find(extensions, lowercase(file.extension().wstring())) != extensions.end();
}

std::string episode_label(std::string_view season, std::string_view episode)
{
    if (episode.empty()) return {};
    const auto compact_number = [](std::string_view value) {
        if (!std::ranges::all_of(value, [](char digit) { return digit >= '0' && digit <= '9'; }))
            return value;
        while (value.size() > 1 && value.front() == '0')
            value.remove_prefix(1);
        return value;
    };
    std::string label;
    if (!season.empty()) {
        label = "S";
        label += compact_number(season);
        label += ' ';
    }
    label += "Ep.";
    label += compact_number(episode);
    return label;
}

static bool match_episode_title(const std::wstring& title, std::wsmatch& match)
{
    static const std::wregex numbered(
        LR"(^(.+?)[\s._\-]*[sS]([0-9]{1,2})[\s._\-]*[eE]([0-9]{1,3})(?=$|[\s._\[\]()\-])(.*)$)");
    static const std::wregex multi_episode(LR"(^\s*[-._ ]*[eE]?[0-9]{1,3}(?:$|[-._ ]))");
    return std::regex_match(title, match, numbered) && !std::regex_search(match[4].str(), multi_episode);
}

std::string activity_video_title(std::string_view title, std::string_view label)
{
    const auto name = wide(title);
    std::wsmatch match;
    if (!label.empty() && match_episode_title(name, match) &&
        label == episode_label(utf8(match[2].str()), utf8(match[3].str())))
        return utf8(match[1].str() + match[4].str());
    return std::string(title);
}

std::optional<Episode> episode_from_filename(const fs::path& file)
{
    if (!video_file(file))
        return {};
    const auto name = file.stem().wstring();
    std::wsmatch match;
    if (!match_episode_title(name, match))
        return {};
    auto series = series_key(match[1].str());
    if (series.empty())
        return {};
    return Episode{std::move(series), std::stoi(match[2].str()), std::stoi(match[3].str())};
}

std::vector<fs::path> following_episodes(const fs::path& current)
{
    const auto episode = episode_from_filename(current);
    if (!episode)
        return {};
    std::map<int, std::vector<fs::path>> candidates;
    std::error_code error;
    for (fs::directory_iterator item(current.parent_path(), error), end;
         !error && item != end; item.increment(error)) {
        std::error_code item_error;
        if (!item->is_regular_file(item_error))
            continue;
        const auto match = episode_from_filename(item->path());
        if (match && match->series == episode->series && match->season == episode->season)
            candidates[match->number].push_back(item->path());
    }
    if (error || candidates[episode->number].size() != 1)
        return {};
    std::vector<fs::path> result;
    for (int number = episode->number + 1; candidates.contains(number); ++number) {
        const auto& matches = candidates.at(number);
        if (matches.size() != 1)
            break;
        result.push_back(matches.front());
    }
    return result;
}

int speed_step(int current, int step)
{
    return std::clamp(current + step, min_step_speed, max_video_speed);
}

int large_speed_step(int current, bool faster)
{
    if (faster)
        return current >= 400 ? speed_step(current, current) : speed_step(current, 50);
    if (current < 100 || current >= 800)
        return std::max(min_step_speed, current / 2);
    return speed_step(current, -50);
}

std::optional<int> parse_speed(std::wstring_view text)
{
    std::wstring value(text);
    std::replace(value.begin(), value.end(), L',', L'.');
    const auto end = value.find_last_not_of(L" \t\u00d7xX");
    if (end != std::wstring::npos) value.resize(end + 1);
    wchar_t* next = nullptr;
    const auto rate = std::wcstod(value.c_str(), &next);
    if (next == value.c_str() || *next || !(rate >= 0.03 && rate <= max_video_speed / 100.0))
        return std::nullopt;
    return static_cast<int>(rate * 100 + 0.5);
}

static std::wstring format_clock(MediaTime time, int hour_digits)
{
    const auto seconds = std::max<MediaTime>(0, time) / second;
    wchar_t buffer[32]{};
    if (hour_digits > 0 || seconds >= 3600)
        std::swprintf(buffer, std::size(buffer), L"%0*lld:%02lld:%02lld",
                     hour_digits, seconds / 3600, seconds / 60 % 60, seconds % 60);
    else
        std::swprintf(buffer, std::size(buffer), L"%02lld:%02lld", seconds / 60, seconds % 60);
    return buffer;
}

std::wstring clock_duration(MediaTime time)
{
    return format_clock(time, 0);
}

PreviewImages load_preview_images(const fs::path& file)
{
    PreviewImages images;
    if (file.empty()) return images;
    std::ifstream input(fs::path(file.wstring() + L".discord-previews.txt"));
    std::string line;
    for (unsigned i = 0; i < 1440 && std::getline(input, line); ++i) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto separator = line.find(' ');
        if (separator == 0 || separator > 4) continue;
        const auto number = line.substr(0, separator);
        if (!std::ranges::all_of(number, [](char c) { return c >= '0' && c <= '9'; })) continue;
        const auto image = line.substr(separator + 1);
        if (image.empty() || image.size() > 512 ||
            !std::ranges::all_of(image, [](unsigned char c) { return c > 32 && c < 127; })) continue;
        const bool asset_key = std::ranges::all_of(image, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
        if (asset_key || (image.starts_with("https://") && image.size() > 8))
            images.insert_or_assign(std::stoll(number), image);
    }
    return images;
}

std::string preview_image(const PreviewImages& images, MediaTime position, std::string_view fallback)
{
    const auto match = images.find(std::max<MediaTime>(0, position) / (60 * second));
    return match == images.end() ? std::string(fallback) : match->second;
}

MediaTime remaining_playback_time(int rate, MediaTime length, MediaTime position)
{
    if (length <= 0 || rate <= 0) return 0;
    return (length - std::clamp(position, MediaTime{0}, length)) * 100 / rate;
}

static std::wstring format_speed(int rate)
{
    wchar_t speed[24]{};
    std::swprintf(speed, std::size(speed), L"%d.%02d\u00d7", rate / 100, rate % 100);
    return speed;
}

std::wstring playback_time_left(int rate, MediaTime length, MediaTime position)
{
    if (length <= 0 || rate <= 0) return {};
    const auto remaining = remaining_playback_time(rate, length, position);
    return clock_duration((remaining + second - 1) / second * second) + L" left";
}

std::wstring playback_status(int rate, MediaTime length, MediaTime position)
{
    auto result = format_speed(rate);
    const auto left = playback_time_left(rate, length, position);
    if (!left.empty()) result += L"  \u00b7  " + left;
    return result;
}

static std::wstring activity_clock(MediaTime time, MediaTime maximum)
{
    const auto hours = maximum / second / 3600;
    return format_clock(time, hours > 0 ? static_cast<int>(std::to_wstring(hours).size()) : 0);
}

std::wstring activity_playback_status(int rate, MediaTime length, MediaTime position)
{
    const auto elapsed = length > 0 ? std::min(position, length) : position;
    auto summary = activity_clock(elapsed, length);
    if (length > 0) summary += L" / " + activity_clock(length, length);
    if (rate != 100) {
        auto speed = format_speed(rate);
        if (rate % 100 == 0) speed.erase(speed.size() - 4, 3);
        else if (rate % 10 == 0) speed.erase(speed.size() - 2, 1);
        summary += L" \u00b7 Speed " + speed;
    }
    return summary;
}

ActivityTimestamps activity_timestamps(int rate, MediaTime length, MediaTime position, MediaTime sampled_at, bool playing)
{
    if (!playing || rate != 100 || length < second || position < 0 || position >= length || sampled_at <= position)
        return {};
    const auto start = (sampled_at - position) / second;
    const auto duration = length / second + (length % second >= second / 2);
    return start > 0 ? ActivityTimestamps{start, start + duration} : ActivityTimestamps{};
}

std::wstring playback_summary(int rate, MediaTime length, MediaTime position, bool playing)
{
    auto summary = format_speed(rate);
    if (length <= 0 || rate <= 0)
        return summary;
    const auto remaining = remaining_playback_time(rate, length, position);
    summary += L"  \u00B7  " + clock_duration(remaining) + L" remaining";
    if (playing) {
        const auto end = std::time(nullptr) + remaining / second;
        std::tm local{};
        wchar_t clock[16]{};
        if (localtime_s(&local, &end) == 0 && std::wcsftime(clock, std::size(clock), L"%H:%M", &local))
            summary += std::wstring(L"  \u00B7  Ends at ") + clock;
    }
    return summary;
}

std::string utf8(std::wstring_view value)
{
    if (value.empty())
        return {};
    const auto length = static_cast<int>(value.size());
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(std::string_view value)
{
    if (value.empty())
        return {};
    const auto length = static_cast<int>(value.size());
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(), size);
    return result;
}
}
