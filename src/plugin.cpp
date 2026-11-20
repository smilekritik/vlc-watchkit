#define MODULE_STRING "vlc_addon"
#include "discord.hpp"
#include "preview.hpp"
#include "ui.hpp"
#include "windows.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <future>
#include <thread>
#include <utility>

struct pollfd;
extern "C" int poll(pollfd*, unsigned, int);
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_input.h>
#include <vlc_playlist.h>
#include <vlc_url.h>
#include <vlc_vout_osd.h>

namespace addon {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr char menu_command[] = "vlc-addon-menu-command";
constexpr unsigned state = 1, video = 2, rate = 4, metadata = 8, position = 16;
thread_local bool adding = false;

std::string take(char* value)
{
    const std::unique_ptr<char, decltype(&std::free)> owned(value, std::free);
    return value ? value : "";
}

fs::path data_directory()
{
    wchar_t value[32768]{};
    const auto override_size = GetEnvironmentVariableW(L"VLC_ADDON_DATA_DIR", value, std::size(value));
    if (override_size && override_size < std::size(value) && fs::path(value).is_absolute())
        return fs::path(value);
    const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", value, std::size(value));
    if (!size || size >= std::size(value)) throw std::runtime_error("LOCALAPPDATA is unavailable");
    return fs::path(value) / L"VLCAddon";
}

struct InputRelease {
    void operator()(input_thread_t* value) const { if (value) vlc_object_release(value); }
};
using Input = std::unique_ptr<input_thread_t, InputRelease>;

struct PlaylistLock {
    explicit PlaylistLock(playlist_t* value) : value(value) { playlist_Lock(value); }
    ~PlaylistLock() { playlist_Unlock(value); }
    playlist_t* value;
};
struct AutoAdd {
    AutoAdd() { adding = true; }
    ~AutoAdd() { adding = false; }
};
struct Event {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Event() { if (!handle) throw std::runtime_error("Unable to create wake event"); }
    ~Event() { CloseHandle(handle); }
};

class Addon {
public:
    explicit Addon(intf_thread_t* interface);
    ~Addon();
private:
    static int menu_event(vlc_object_t*, const char*, vlc_value_t, vlc_value_t, void*);
    static int input_event(vlc_object_t*, const char*, vlc_value_t, vlc_value_t, void*);
    static int append_event(vlc_object_t*, const char*, vlc_value_t, vlc_value_t, void*);
    static int media_event(vlc_object_t*, const char*, vlc_value_t, vlc_value_t, void*);

    void detach();
    void run() noexcept;
    DWORD timeout() const;
    void tick();
    void locate_ui(bool reposition = false);
    void suggest_latest();
    void update_status();
    Ui& ui();
    void attach();
    void finish();
    void playback(unsigned flags);
    void prepare();
    void add_following();
    void play_if_idle(int id);
    void record(bool final);
    void publish();
    void update_frame_preview(bool running);
    void overlay(const std::wstring& message);
    void act(Action action);
    void change_speed(int step);
    void set_speed(int value);
    void begin_bookmark();
    void save_bookmark(std::wstring comment);
    void close_bookmark(bool saved);
    bool edit_bookmark(std::size_t index, std::wstring comment);
    bool remove_bookmark(std::size_t index);
    bool save_settings(Settings settings);
    void open_at(const std::wstring& path, MediaTime target);

    bool playing() const { return input_ && var_GetInteger(input_.get(), "state") == PLAYING_S; }
    MediaTime time() const { return input_ ? var_GetInteger(input_.get(), "time") : 0; }
    MediaTime length() const { return input_ ? var_GetInteger(input_.get(), "length") : 0; }
    int speed() const;
    int reported_speed() const;
    void apply_speed(int value);

    intf_thread_t* interface_;
    playlist_t* playlist_;
    vlc_object_t* root_;
    Event wake_;
    std::thread worker_;
    std::atomic<bool> stopping_ = false, changed_ = true, sample_ = false;
    std::atomic<int> menu_ = 0, appended_ = 0;
    std::atomic<unsigned> media_flags_ = 0;
    std::atomic<ULONGLONG> sample_at_ = 0;


    Store* store_ = nullptr;
    Discord* discord_ = nullptr;
    PlayerWindows* windows_ = nullptr;
    HWND owner_ = nullptr;
    std::unique_ptr<Ui> ui_;
    Input input_, bookmark_input_;
    std::optional<Bookmark> new_bookmark_;
    std::optional<Progress> prompt_;
    std::wstring path_, title_, seek_path_;
    MediaTime seek_time_ = 0, last_position_ = 0, last_length_ = 0, saved_position_ = 0;
    int append_id_ = 0, window_attempts_ = 30, last_speed_ = 0;
    bool video_ = false, prepared_ = false, rate_applied_ = false, watched_ = false;
    bool startup_checked_ = false, bookmark_playing_ = false, resume_choice_ = false;
    Clock::time_point last_sample_{};
    std::optional<PreviewImages> previews_;
    std::future<std::string> frame_upload_;
    std::string uploaded_frame_;
    std::wstring upload_endpoint_;
    Clock::time_point next_frame_{};
    std::size_t video_generation_ = 0, upload_generation_ = 0;
    std::optional<int> requested_speed_;
    std::optional<Clock::time_point> next_save_, idle_append_, discord_refresh_, find_window_ = Clock::now();
};

Addon::Addon(intf_thread_t* interface)
    : interface_(interface), playlist_(pl_Get(interface)), root_(VLC_OBJECT(interface->obj.libvlc))
{
    if (var_Create(root_, menu_command, VLC_VAR_INTEGER) != VLC_SUCCESS)
        throw std::runtime_error("Unable to create the menu command");
    var_AddCallback(root_, menu_command, menu_event, this);
    var_AddCallback(playlist_, "input-current", input_event, this);
    var_AddCallback(playlist_, "playlist-item-append", append_event, this);
    try {
        worker_ = std::thread([this] { run(); });
    } catch (...) {
        detach();
        throw;
    }
}

Addon::~Addon()
{
    detach();
    stopping_ = true;
    SetEvent(wake_.handle);
    worker_.join();
}

void Addon::detach()
{
    var_DelCallback(playlist_, "playlist-item-append", append_event, this);
    var_DelCallback(playlist_, "input-current", input_event, this);
    var_DelCallback(root_, menu_command, menu_event, this);
    var_Destroy(root_, menu_command);
}

int Addon::menu_event(vlc_object_t*, const char*, vlc_value_t, vlc_value_t value, void* data)
{
    auto& self = *static_cast<Addon*>(data);
    self.menu_ = static_cast<int>(value.i_int);
    SetEvent(self.wake_.handle);
    return VLC_SUCCESS;
}

int Addon::input_event(vlc_object_t*, const char*, vlc_value_t, vlc_value_t, void* data)
{
    auto& self = *static_cast<Addon*>(data);
    self.changed_ = true;
    SetEvent(self.wake_.handle);
    return VLC_SUCCESS;
}

int Addon::append_event(vlc_object_t*, const char*, vlc_value_t, vlc_value_t value, void* data)
{
    auto& self = *static_cast<Addon*>(data);
    const auto* item = static_cast<playlist_item_t*>(value.p_address);
    if (!adding && item && item->p_parent == self.playlist_->p_playing) {
        self.appended_ = item->i_id;
        SetEvent(self.wake_.handle);
    }
    return VLC_SUCCESS;
}

int Addon::media_event(vlc_object_t*, const char*, vlc_value_t, vlc_value_t value, void* data)
{
    auto& self = *static_cast<Addon*>(data);
    unsigned flag = 0;
    switch (value.i_int) {
        case INPUT_EVENT_STATE:
        case INPUT_EVENT_DEAD: flag = state; break;
        case INPUT_EVENT_ES:
        case INPUT_EVENT_VOUT: flag = video; break;
        case INPUT_EVENT_RATE: flag = rate; break;
        case INPUT_EVENT_ITEM_META: flag = metadata; break;
        case INPUT_EVENT_POSITION:
            if (self.sample_ && GetTickCount64() >= self.sample_at_.load()) {
                self.sample_at_ = GetTickCount64() + 1000;
                flag = position;
            }
            break;
        default: break;
    }
    if (flag) {
        self.media_flags_.fetch_or(flag);
        SetEvent(self.wake_.handle);
    }
    return VLC_SUCCESS;
}

void Addon::run() noexcept
{
    try {
        Store storage(data_directory());
        store_ = &storage;
        if (storage.has_read_errors()) msg_Warn(interface_, "WatchKit could not read one or more data files");
        Discord discord;
        discord_ = &discord;
        MSG msg{};
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        PlayerWindows windows(GetCurrentThreadId());
        windows_ = &windows;
        windows.bindings(store_->settings.bindings);
        msg_Info(interface_, "WatchKit loaded");
        try {
            while (!stopping_) {
                tick();
                for (unsigned i = 0; i < 32 && !stopping_ && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i) {
                    if (!msg.hwnd && msg.message == hotkey_message) {
                        owner_ = reinterpret_cast<HWND>(msg.lParam);
                        if (ui_) ui_->owner(owner_);
                        act(static_cast<Action>(msg.wParam));
                    } else if (!msg.hwnd && msg.message == window_changed_message) {
                        locate_ui(true);
                    } else if (ui_) {
                        ui_->process(msg);
                    } else {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                }
                if (stopping_) break;
                HANDLE handles[] = {wake_.handle, discord.event()};
                MsgWaitForMultipleObjectsEx(discord.event() ? 2 : 1, handles, timeout(), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            finish();
        } catch (const std::exception& error) {
            msg_Err(interface_, "WatchKit worker stopped: %s", error.what());
        } catch (...) {
            msg_Err(interface_, "WatchKit worker stopped");
        }
        if (input_) var_DelCallback(input_.get(), "intf-event", media_event, this);
        input_.reset();
        bookmark_input_.reset();
        ui_.reset();
        discord.update(store_->settings.application_id, {});
    } catch (const std::exception& error) {
        msg_Err(interface_, "WatchKit could not start: %s", error.what());
    } catch (...) {
        msg_Err(interface_, "WatchKit could not start");
    }
}

DWORD Addon::timeout() const
{
    auto deadline = next_save_;
    for (const auto next : {find_window_, idle_append_, discord_refresh_, discord_->deadline()})
        if (next && (!deadline || next < deadline)) deadline = next;
    if (!deadline) return INFINITE;
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(*deadline - Clock::now()).count();
    return static_cast<DWORD>(std::clamp<std::int64_t>(wait, 0, 0xfffffffe));
}

void Addon::tick()
{
    const auto now = Clock::now();
    if (changed_.exchange(false)) attach();
    if (const auto flags = media_flags_.exchange(0)) playback(flags);
    if (const auto id = appended_.exchange(0)) {
        append_id_ = id;
        idle_append_ = now + 120ms;
    }
    if (idle_append_ && now >= *idle_append_) {
        idle_append_.reset();
        play_if_idle(append_id_);
    }
    if (find_window_ && now >= *find_window_) {
        locate_ui();
        if (owner_ || --window_attempts_ <= 0) find_window_.reset();
        else find_window_ = now + 100ms;
    }
    if (const auto menu = menu_.exchange(0)) {
        locate_ui();
        if (owner_ && menu == 1) ui().show_settings(store_->settings);
        if (owner_ && menu == 2) ui().show_bookmarks(store_->bookmarks);
        if (owner_ && menu == 3 && !input_) suggest_latest();
    }
    if (next_save_ && now >= *next_save_) {
        record(false);
        next_save_ = playing() && video_ && !path_.empty() ? std::optional(now + 15s) : std::nullopt;
    }
    if (discord_refresh_ && now >= *discord_refresh_) publish();
    discord_->pump();
    if (discord_->take_acknowledgement()) msg_Dbg(interface_, "Discord accepted the Rich Presence activity");
    if (discord_->take_rejection()) msg_Warn(interface_, "Discord rejected the Rich Presence activity");
}

void Addon::locate_ui(bool reposition)
{
    const auto found = windows_->refresh();
    if (!found) return;
    reposition = reposition || found != owner_;
    owner_ = found;
    if (ui_) {
        ui_->owner(owner_);
        if (reposition) ui_->reposition_prompt();
    }
    update_status();
    if (prompt_) {
        ui().prompt(std::move(*prompt_), false);
        prompt_.reset();
    }
    if (!startup_checked_) {
        startup_checked_ = true;
        if (!input_) suggest_latest();
    }
}

void Addon::update_status()
{
    if (!owner_ || !video_ || !input_ || !watched_) {
        if (ui_) ui_->playback_status(nullptr, {});
        return;
    }
    ui().playback_status(owner_, playback_status(speed(), length(), time()), var_GetBool(playlist_, "fullscreen"));
}

Ui& Addon::ui()
{
    if (!ui_) {
        UiCallbacks callbacks{
            [this](std::wstring comment) { save_bookmark(std::move(comment)); },
            [this](bool saved) { close_bookmark(saved); },
            [this](Bookmark item) { open_at(item.path, item.position); },
            [this](std::size_t id, std::wstring comment) { return edit_bookmark(id, std::move(comment)); },
            [this](std::size_t id) { return remove_bookmark(id); },
            [this](Settings settings) { return save_settings(std::move(settings)); },
            [this](Progress item) { resume_choice_ = false; open_at(item.path, item.position); },
            [this] { resume_choice_ = false; },
            [this](int value) { set_speed(value); }
        };
        ui_ = std::make_unique<Ui>(std::move(callbacks), var_InheritBool(interface_, "qt-dark-palette"));
    }
    ui_->owner(owner_);
    return *ui_;
}

void Addon::suggest_latest()
{
    const Progress* latest = nullptr;
    for (const auto& [path, item] : store_->positions) {
        if (latest && latest->updated >= item.updated) continue;
        std::error_code error;
        if (fs::is_regular_file(fs::path(path), error)) latest = &item;
    }
    if (latest && owner_) ui().prompt(*latest, true);
}

void Addon::finish()
{
    if (!input_) return;
    record(true);
    sample_ = false;
    var_DelCallback(input_.get(), "intf-event", media_event, this);
    input_.reset();
    next_save_.reset();
    prompt_.reset();
    if (ui_) {
        ui_->hide_prompt();
        ui_->playback_status(nullptr, {});
    }
    discord_refresh_.reset();
    ++video_generation_;
    uploaded_frame_.clear();
    next_frame_ = {};
    discord_->update(store_->settings.application_id, {});
}

void Addon::attach()
{
    Input current(playlist_CurrentInput(playlist_));
    if (current.get() == input_.get()) return;
    finish();
    path_.clear();
    title_.clear();
    video_ = prepared_ = rate_applied_ = watched_ = false;
    last_position_ = last_length_ = saved_position_ = 0;
    last_speed_ = 0;
    requested_speed_.reset();
    previews_.reset();
    resume_choice_ = false;
    if (!current) return;
    input_ = std::move(current);
    auto* item = input_GetItem(input_.get());
    const auto mrl = take(input_item_GetURI(item));
    if (mrl.starts_with("file://")) path_ = wide(take(vlc_uri2path(mrl.c_str())));
    title_ = wide(take(input_item_GetName(item)));
    if (title_.empty() && !path_.empty()) title_ = fs::path(path_).filename().wstring();
    video_ = !path_.empty() && video_file(fs::path(path_));
    var_AddCallback(input_.get(), "intf-event", media_event, this);
    sample_ = store_->settings.discord_enabled && !store_->settings.application_id.empty();
    sample_at_ = 0;
    if (ui_) ui_->hide_prompt();
    playback(state | video | metadata);
}

int Addon::speed() const
{
    const auto current = reported_speed();
    if (requested_speed_) {
        const auto value = *requested_speed_;
        const auto denominator = std::clamp(static_cast<int>(INPUT_RATE_DEFAULT / (value / 100.f)),
                                            INPUT_RATE_MIN, INPUT_RATE_MAX);
        const auto rounded = static_cast<int>(std::lround(100.0 * INPUT_RATE_DEFAULT / denominator));
        if (current == value || current == rounded) return value;
    }
    return current;
}

int Addon::reported_speed() const
{
    if (!input_) return 100;
    const auto value = var_GetFloat(input_.get(), "rate");
    return value > 0 && std::isfinite(value) ? std::max(1, static_cast<int>(std::lround(value * 100.f))) : 100;
}

void Addon::playback(unsigned flags)
{
    if (!input_) return;
    const auto status = var_GetInteger(input_.get(), "state");
    const bool active = status == PLAYING_S || status == PAUSE_S;
    if (active && (flags & video)) {
        vlc_value_t track{};
        if (var_GetChecked(input_.get(), "video-es", VLC_VAR_INTEGER, &track) == VLC_SUCCESS && track.i_int >= 0)
            video_ = true;
        locate_ui();
    }
    if (active && !prepared_) {
        prepared_ = true;
        prepare();
    }
    if (active && video_ && !rate_applied_) {
        rate_applied_ = true;
        const auto preferred = store_->settings.video_speed;
        if (speed() != preferred) apply_speed(preferred);
    } else if (active && video_ && (flags & rate)) {
        const auto current = std::clamp(speed(), min_manual_speed, max_video_speed);
        if (requested_speed_ && *requested_speed_ != current) requested_speed_.reset();
        if (store_->settings.video_speed != current) {
            store_->settings.video_speed = current;
            if (!store_->save_settings()) msg_Warn(interface_, "Unable to save playback speed");
        }
    }
    watched_ = watched_ || active;
    if (status == PLAYING_S) {
        if (video_ && !path_.empty() && !next_save_) next_save_ = Clock::now() + 15s;
    } else if (next_save_) {
        next_save_.reset();
        record(false);
    }
    bool refresh = (flags & (state | video | metadata | rate)) != 0;
    if (active) {
        const auto now = Clock::now();
        const auto current = time();
        const auto duration = length();
        const auto current_speed = speed();
        if (duration != last_length_) refresh = true;
        if ((flags & position) && last_speed_ > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last_sample_).count();
            const auto expected = last_position_ + (playing() ? elapsed * current_speed / 100 : 0);
            if (std::abs(current - expected) > 2 * second) refresh = true;
        }
        last_position_ = current;
        last_length_ = duration;
        last_sample_ = now;
        last_speed_ = current_speed;
    }
    sample_ = active && (video_ || (!store_->settings.application_id.empty() && store_->settings.discord_enabled));
    if (active) update_status();
    if (refresh) publish();
}

void Addon::prepare()
{
    if (path_.empty()) return;
    if (path_ == seek_path_) {
        var_SetInteger(input_.get(), "time", seek_time_);
    } else if (video_) {
        const auto entry = store_->positions.find(path_);
        if (entry != store_->positions.end()) {
            resume_choice_ = true;
            prompt_ = entry->second;
            locate_ui();
        }
    }
    seek_path_.clear();
    if (video_) add_following();
}

void Addon::add_following()
{
    {
        PlaylistLock lock(playlist_);
        if (playlist_->p_playing->i_children != 1) return;
    }
    auto files = following_episodes(fs::path(path_));
    {
        PlaylistLock lock(playlist_);
        if (playlist_->p_playing->i_children != 1) return;
    }
    AutoAdd own;
    for (const auto& file : files) {
        auto uri = take(vlc_path2uri(utf8(file.wstring()).c_str(), nullptr));
        if (!uri.empty() && playlist_Add(playlist_, uri.c_str(), false) != VLC_SUCCESS)
            msg_Warn(interface_, "Unable to enqueue a neighboring episode");
    }
}

void Addon::play_if_idle(int id)
{
    PlaylistLock lock(playlist_);
    if (playlist_Status(playlist_) != PLAYLIST_STOPPED) return;
    auto* item = playlist_ItemGetById(playlist_, id);
    if (item && item->p_parent == playlist_->p_playing) {
        msg_Dbg(interface_, "Starting a newly enqueued item in an idle player");
        playlist_ViewPlay(playlist_, playlist_->p_playing, item);
    }
}

void Addon::record(bool final)
{
    if (!video_ || path_.empty() || !watched_ || resume_choice_) return;
    const auto duration = std::max(length(), last_length_);
    const auto current = time();
    const auto saved = final && current == 0 ? last_position_ : current;
    if (final && duration > 0 && saved >= duration - 2 * second) {
        if (store_->positions.erase(path_) && !store_->save_progress())
            msg_Warn(interface_, "Unable to save viewing history");
    } else if (saved >= second && (final || saved != saved_position_)) {
        store_->positions.insert_or_assign(path_, Progress{path_, title_, saved, duration, std::time(nullptr)});
        saved_position_ = saved;
        if (!store_->save_progress()) msg_Warn(interface_, "Unable to save viewing history");
    }
}

void Addon::update_frame_preview(bool running)
{
    const auto endpoint = wide(store_->settings.preview_upload_url);
    if (endpoint != upload_endpoint_) {
        upload_endpoint_ = endpoint;
        ++video_generation_;
        uploaded_frame_.clear();
        next_frame_ = {};
    }
    if (frame_upload_.valid() && frame_upload_.wait_for(0s) == std::future_status::ready) {
        try {
            auto url = frame_upload_.get();
            if (!url.empty() && upload_generation_ == video_generation_) uploaded_frame_ = std::move(url);
        } catch (const std::exception& error) {
            msg_Warn(interface_, "WatchKit frame upload failed: %s", error.what());
        }
    }
    if (!running || upload_endpoint_.empty() || frame_upload_.valid() || Clock::now() < next_frame_)
        return;
    if (auto* output = input_GetVout(input_.get())) {
        next_frame_ = Clock::now() + 60s;
        try {
            frame_upload_ = std::async(std::launch::async, upload_video_frame, output, upload_endpoint_);
            upload_generation_ = video_generation_;
        } catch (const std::exception& error) {
            vlc_object_release(output);
            msg_Warn(interface_, "WatchKit could not start a frame upload: %s", error.what());
        }
    }
}

void Addon::publish()
{
    const auto& settings = store_->settings;
    const bool running = playing();
    const bool paused = input_ && var_GetInteger(input_.get(), "state") == PAUSE_S;
    if (!settings.discord_enabled || settings.application_id.empty() || (!running && !paused)) {
        discord_refresh_.reset();
        discord_->update(settings.application_id, {});
        return;
    }
    if (video_) update_frame_preview(running);
    discord_refresh_ = running || (video_ && frame_upload_.valid()) ? std::optional(Clock::now() + 5s) : std::nullopt;
    auto* item = input_GetItem(input_.get());
    auto title = take(input_item_GetTitle(item));
    auto detail = take(input_item_GetArtist(item));
    if (video_) {
        const auto show = take(input_item_GetShowName(item));
        const auto season = take(input_item_GetSeason(item));
        const auto episode = take(input_item_GetEpisode(item));
        if (!show.empty()) title = show;
        if (!episode.empty())
            detail = episode_label(season, episode);
        else if (!path_.empty())
            if (const auto info = episode_from_filename(fs::path(path_)); info)
                detail = episode_label(std::to_string(info->season), std::to_string(info->number));
    } else {
        const auto album = take(input_item_GetAlbum(item));
        if (!album.empty()) detail += (detail.empty() ? "" : " \xC2\xB7 ") + album;
    }
    if (title.empty()) title = utf8(title_);
    if (title.empty()) title = video_ ? "Video" : "Audio";
    auto image = settings.large_image;
    const auto current = time();
    const auto sampled_at = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto duration = length();
    const auto current_speed = speed();
    const auto timestamps = activity_timestamps(current_speed, duration, current, sampled_at, running);
    if (video_) {
        if (!previews_) previews_ = load_preview_images(fs::path(path_));
        image = preview_image(*previews_, current, uploaded_frame_.empty() ? image : uploaded_frame_);
        if (!detail.empty()) title = activity_video_title(title, detail) + " \xC2\xB7 " + detail;
        detail = timestamps.start > 0 ? "" : utf8(activity_playback_status(current_speed, duration, current));
    } else {
        if (timestamps.start == 0 && (duration > 0 || current_speed != 100)) {
            const auto timing = utf8(activity_playback_status(current_speed, duration, current));
            detail = detail.empty() ? timing : timing + " \xC2\xB7 " + detail;
        }
    }
    const auto image_text = image.empty() ? std::string{} : utf8(playback_time_left(current_speed, duration, current));
    discord_->update(settings.application_id, Activity{
        "VLC · " + title, detail, image, paused || video_ ? 3 : 2, timestamps, image_text, paused
    });
}

void Addon::overlay(const std::wstring& message)
{
    if (!input_) return;
    if (auto* output = input_GetVout(input_.get())) {
        const auto text = utf8(message);
        vout_OSDMessage(output, 0, "%s", text.c_str());
        vlc_object_release(output);
    }
}

void Addon::act(Action action)
{
    switch (action) {
        case Action::discord:
            store_->settings.discord_enabled = !store_->settings.discord_enabled;
            if (!store_->save_settings()) {
                store_->settings.discord_enabled = false;
                overlay(L"Discord status off. Could not save this setting.");
            } else overlay(store_->settings.discord_enabled ? L"Discord status on" : L"Discord status off");
            publish();
            break;
        case Action::bookmark: begin_bookmark(); break;
        case Action::bookmarks: if (owner_) ui().show_bookmarks(store_->bookmarks); break;
        case Action::speed_up: change_speed(5); break;
        case Action::speed_down: change_speed(-5); break;
        case Action::speed_normal: set_speed(100); break;
        case Action::speed_fast: set_speed(large_speed_step(speed(), true)); break;
        case Action::speed_slow: set_speed(large_speed_step(speed(), false)); break;
        case Action::time: overlay(playback_summary(speed(), length(), time(), playing())); break;
        case Action::count: break;
    }
}

void Addon::change_speed(int step)
{
    if (!input_ || !video_) return;
    set_speed(speed_step(speed(), step));
}

void Addon::set_speed(int value)
{
    if (!input_ || !video_) return;
    apply_speed(value);
    overlay(playback_summary(value, 0, 0, false));
}

void Addon::apply_speed(int value)
{
    requested_speed_ = value;
    if (var_SetFloat(input_.get(), "rate", value / 100.f) != VLC_SUCCESS)
        requested_speed_.reset();
}

void Addon::begin_bookmark()
{
    if (!input_ || !video_ || !owner_ || path_.empty() || new_bookmark_) return;
    new_bookmark_ = Bookmark{path_, title_, time(), {}};
    bookmark_input_ = Input(playlist_CurrentInput(playlist_));
    bookmark_playing_ = bookmark_input_.get() == input_.get() && playing();
    if (bookmark_playing_) var_SetInteger(input_.get(), "state", PAUSE_S);
    ui().add_bookmark(*new_bookmark_);
}

void Addon::save_bookmark(std::wstring comment)
{
    if (!new_bookmark_) return;
    new_bookmark_->comment = std::move(comment);
    store_->bookmarks.push_back(*new_bookmark_);
    if (!store_->save_bookmarks()) {
        store_->bookmarks.pop_back();
        throw std::runtime_error("Unable to save bookmark");
    }
}

void Addon::close_bookmark(bool saved)
{
    if (bookmark_input_ && bookmark_input_.get() == input_.get()) {
        if (bookmark_playing_ && var_GetInteger(input_.get(), "state") == PAUSE_S)
            var_SetInteger(input_.get(), "state", PLAYING_S);
        if (saved && new_bookmark_)
            overlay(L"Bookmark saved  \u00B7  " + clock_duration(new_bookmark_->position));
    }
    new_bookmark_.reset();
    bookmark_input_.reset();
    bookmark_playing_ = false;
}

bool Addon::edit_bookmark(std::size_t index, std::wstring comment)
{
    if (index >= store_->bookmarks.size()) return false;
    auto previous = std::exchange(store_->bookmarks[index].comment, std::move(comment));
    if (store_->save_bookmarks()) return true;
    store_->bookmarks[index].comment = std::move(previous);
    return false;
}

bool Addon::remove_bookmark(std::size_t index)
{
    if (index >= store_->bookmarks.size()) return false;
    auto item = store_->bookmarks[index];
    store_->bookmarks.erase(store_->bookmarks.begin() + index);
    if (store_->save_bookmarks()) return true;
    store_->bookmarks.insert(store_->bookmarks.begin() + index, std::move(item));
    return false;
}

bool Addon::save_settings(Settings settings)
{
    settings.video_speed = store_->settings.video_speed;
    auto previous = std::exchange(store_->settings, std::move(settings));
    if (!store_->save_settings()) {
        store_->settings = std::move(previous);
        return false;
    }
    windows_->bindings(store_->settings.bindings);
    publish();
    return true;
}

void Addon::open_at(const std::wstring& path, MediaTime target)
{
    if (input_ && path == path_) {
        var_SetInteger(input_.get(), "time", target);
        return;
    }
    std::error_code error;
    if (!fs::is_regular_file(fs::path(path), error)) return;
    auto uri = take(vlc_path2uri(utf8(path).c_str(), nullptr));
    if (uri.empty()) return;
    seek_path_ = path;
    seek_time_ = target;
    AutoAdd own;
    if (playlist_Add(playlist_, uri.c_str(), true) != VLC_SUCCESS) seek_path_.clear();
}

int open(vlc_object_t* object) noexcept
{
    auto* interface = reinterpret_cast<intf_thread_t*>(object);
    try {
        interface->p_sys = reinterpret_cast<intf_sys_t*>(new Addon(interface));
        return VLC_SUCCESS;
    } catch (const std::exception& error) {
        msg_Err(interface, "WatchKit could not load: %s", error.what());
        return VLC_EGENERIC;
    } catch (...) {
        return VLC_EGENERIC;
    }
}

void close(vlc_object_t* object) noexcept
{
    auto* interface = reinterpret_cast<intf_thread_t*>(object);
    delete reinterpret_cast<Addon*>(interface->p_sys);
}
}
}

vlc_module_begin()
    set_shortname("WatchKit")
    set_description("Playback controls, bookmarks, and Discord Rich Presence")
    set_capability("interface", 0)
    set_callbacks(addon::open, addon::close)
vlc_module_end()
