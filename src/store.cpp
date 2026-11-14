#include "store.hpp"

#include <windows.h>
#include <algorithm>
#include <fstream>
#include <json.hpp>

namespace addon {
namespace {
using nlohmann::json;

bool read(const fs::path& file, auto decode)
{
    std::error_code ec;
    if (!fs::exists(file, ec))
        return !ec;
    try {
        std::ifstream stream(file, std::ios::binary);
        const auto data = json::parse(stream);
        if (data.at("version") != 1 && (file.filename() != L"settings.json" || data.at("version") != 2))
            return false;
        decode(data);
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

bool write(const fs::path& file, const json& data)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec)
        return false;
    const auto temporary = file.wstring() + L".tmp";
    const auto content = data.dump(2) + "\n";
    {
        std::ofstream stream(fs::path(temporary), std::ios::binary | std::ios::trunc);
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.close();
        if (!stream)
            return false;
    }
    return MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
}

Bindings default_bindings()
{
    return {{{VK_MULTIPLY, 0}, {VK_NUMPAD9, 0}, {VK_NUMPAD9, control}, {VK_PRIOR, 0}, {VK_NEXT, 0},
             {VK_HOME, 0}, {VK_ADD, 0}, {VK_SUBTRACT, 0}, {'T', 0}}};
}

bool supported_binding(Binding binding)
{
    return binding.key >= 8 && binding.key < 255 && binding.modifiers <= 7 &&
           binding.key != VK_SHIFT && binding.key != VK_CONTROL && binding.key != VK_MENU &&
           binding.key != VK_LWIN && binding.key != VK_RWIN &&
           binding.key != VK_ESCAPE && binding.key != VK_TAB && binding.key != VK_RETURN &&
           binding.key != VK_BACK;
}

std::wstring binding_name(Binding binding)
{
    std::wstring result;
    if (binding.modifiers & control) result += L"Ctrl+";
    if (binding.modifiers & alt) result += L"Alt+";
    if (binding.modifiers & shift) result += L"Shift+";
    switch (binding.key) {
        case VK_MULTIPLY: return result + L"NumPad *";
        case VK_NUMPAD9: return result + L"NumPad 9";
        case VK_ADD: return result + L"NumPad +";
        case VK_SUBTRACT: return result + L"NumPad -";
        case VK_PRIOR: return result + L"Page Up";
        case VK_NEXT: return result + L"Page Down";
        case VK_HOME: return result + L"Home";
        default: break;
    }
    if (binding.key >= VK_F1 && binding.key <= VK_F24)
        return result + L"F" + std::to_wstring(binding.key - VK_F1 + 1);
    const auto scan = MapVirtualKeyW(binding.key, MAPVK_VK_TO_VSC_EX);
    wchar_t name[64]{};
    const LONG key_data = static_cast<LONG>(((scan & 0xff) << 16) | ((scan > 0xff) ? 1 << 24 : 0));
    if (GetKeyNameTextW(key_data, name, static_cast<int>(std::size(name))) > 0)
        return result + name;
    return result + L"Key " + std::to_wstring(binding.key);
}

bool valid_application_id(std::string_view id)
{
    return id.size() >= 16 && id.size() <= 20 &&
           std::ranges::all_of(id, [](char c) { return c >= '0' && c <= '9'; });
}

Store::Store(fs::path directory) : directory_(std::move(directory))
{
    settings_readable_ = read(directory_ / L"settings.json", [this](const json& data) {
        Settings loaded;
        loaded.discord_enabled = data.value("discord_enabled", true);
        loaded.video_speed = std::clamp(data.value("video_speed", 100), min_manual_speed, max_video_speed);
        loaded.application_id = data.value("application_id", "");
        loaded.large_image = data.value("large_image", "");
        if (!loaded.application_id.empty() && !valid_application_id(loaded.application_id))
            loaded.application_id.clear();
        const auto& bindings = data.at("bindings");
        for (unsigned i = 0; i < loaded.bindings.size(); ++i) {
            const auto& value = bindings.at(i);
            Binding binding{value.at("key").get<unsigned>(), value.at("modifiers").get<unsigned>()};
            if (!supported_binding(binding) ||
                std::find(loaded.bindings.begin(), loaded.bindings.begin() + i, binding) != loaded.bindings.begin() + i)
                throw json::type_error::create(302, "Invalid shortcut", &value);
            loaded.bindings[i] = binding;
        }
        if (data.at("version") == 1) {
            constexpr std::array<Binding, 3> old{{{VK_F9, 0}, {VK_F10, 0}, {VK_F10, shift}}};
            const auto defaults = default_bindings();
            for (unsigned i = 0; i < old.size(); ++i)
                if (loaded.bindings[i] == old[i] && std::ranges::find(loaded.bindings, defaults[i]) == loaded.bindings.end())
                    loaded.bindings[i] = defaults[i];
        }
        settings = std::move(loaded);
    });
    if (!settings_readable_)
        settings.discord_enabled = false;

    bookmarks_readable_ = read(directory_ / L"bookmarks.json", [this](const json& data) {
        std::vector<Bookmark> loaded;
        for (const auto& item : data.at("bookmarks"))
            loaded.push_back({wide(item.at("path").get<std::string>()),
                              wide(item.at("title").get<std::string>()), item.at("position").get<MediaTime>(),
                              wide(item.at("comment").get<std::string>())});
        bookmarks = std::move(loaded);
    });

    progress_readable_ = read(directory_ / L"history.json", [this](const json& data) {
        std::unordered_map<std::wstring, Progress> loaded;
        for (const auto& item : data.at("videos")) {
            Progress entry{wide(item.at("path").get<std::string>()), wide(item.at("title").get<std::string>()),
                           item.at("position").get<MediaTime>(), item.at("duration").get<MediaTime>(),
                           item.at("updated").get<std::int64_t>()};
            if (!entry.path.empty() && entry.position > 0)
                loaded.insert_or_assign(entry.path, std::move(entry));
        }
        positions = std::move(loaded);
    });
    read_error_ = !settings_readable_ || !bookmarks_readable_ || !progress_readable_;
}

bool Store::save_settings() const
{
    if (!settings_readable_)
        return false;
    auto bindings = json::array();
    for (const auto& item : settings.bindings)
        bindings.push_back({{"key", item.key}, {"modifiers", item.modifiers}});
    return write(directory_ / L"settings.json", {
        {"version", 2}, {"discord_enabled", settings.discord_enabled}, {"video_speed", settings.video_speed},
        {"application_id", settings.application_id}, {"large_image", settings.large_image}, {"bindings", std::move(bindings)}
    });
}

bool Store::save_bookmarks() const
{
    if (!bookmarks_readable_)
        return false;
    auto items = json::array();
    for (const auto& item : bookmarks)
        items.push_back({{"path", utf8(item.path)}, {"title", utf8(item.title)},
                         {"position", item.position}, {"comment", utf8(item.comment)}});
    return write(directory_ / L"bookmarks.json", {{"version", 1}, {"bookmarks", std::move(items)}});
}

bool Store::save_progress() const
{
    if (!progress_readable_)
        return false;
    auto items = json::array();
    for (const auto& [path, item] : positions)
        items.push_back({{"path", utf8(path)}, {"title", utf8(item.title)}, {"position", item.position},
                         {"duration", item.duration}, {"updated", item.updated}});
    return write(directory_ / L"history.json", {{"version", 1}, {"videos", std::move(items)}});
}
}
