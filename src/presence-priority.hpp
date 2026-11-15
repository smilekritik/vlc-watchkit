#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <windows.h>

namespace addon {
// These names and their order are shared by WatchKit for Chrome and VLC.
// Windows releases ownership when a process or its publishing thread exits.
enum class PresenceLevel : std::size_t { browser_paused, vlc_paused, browser_playing, vlc_playing };

class PresencePriority {
public:
    PresencePriority() = default;
    PresencePriority(const PresencePriority&) = delete;
    PresencePriority& operator=(const PresencePriority&) = delete;
    ~PresencePriority() { set(std::nullopt); }

    void set(std::optional<PresenceLevel> level)
    {
        if (level_ != level) {
            if (claim_) {
                ReleaseMutex(claim_);
                CloseHandle(claim_);
                claim_ = nullptr;
            }
            level_ = level;
        }
        if (!level_ || claim_) return;
        auto handle = CreateMutexW(nullptr, FALSE, names[static_cast<std::size_t>(*level_)]);
        if (!handle) return;
        if (acquire(handle)) claim_ = handle;
        else CloseHandle(handle);
    }

    bool can_yield() const { return level_ && *level_ != PresenceLevel::vlc_playing; }

    bool blocked() const
    {
        if (!level_) return false;
        for (auto index = static_cast<std::size_t>(*level_) + 1; index < names.size(); ++index) {
            const auto handle = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, names[index]);
            if (!handle) continue;
            const bool available = acquire(handle);
            if (available) ReleaseMutex(handle);
            CloseHandle(handle);
            if (!available) return true;
        }
        return false;
    }

private:
    static bool acquire(HANDLE handle)
    {
        const auto result = WaitForSingleObject(handle, 0);
        return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }

    static constexpr std::array names{
        LR"(Local\Smilekritik.WatchKit.Presence.v1.ChromePaused)",
        LR"(Local\Smilekritik.WatchKit.Presence.v1.VlcPaused)",
        LR"(Local\Smilekritik.WatchKit.Presence.v1.ChromePlaying)",
        LR"(Local\Smilekritik.WatchKit.Presence.v1.VlcPlaying)"
    };
    std::optional<PresenceLevel> level_;
    HANDLE claim_ = nullptr;
};
}
