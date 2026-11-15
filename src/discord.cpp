#include "discord.hpp"

#include <algorithm>
#include <cstring>
#include <utility>
#include <json.hpp>

namespace addon {
namespace {
using nlohmann::json;
using namespace std::chrono_literals;

std::string clean(std::string text)
{
    std::erase_if(text, [](unsigned char c) { return c < 32; });
    if (text.size() > 128) {
        std::size_t size = 128;
        while (size && (static_cast<unsigned char>(text[size]) & 0xc0) == 0x80)
            --size;
        text.resize(size);
    }
    if (text.size() == 1) text += ' ';
    return text;
}
}

Discord::~Discord()
{
    disconnect();
    if (event_) CloseHandle(event_);
}

void Discord::disconnect()
{
    if (pipe_ != INVALID_HANDLE_VALUE) {
        if (operation_ != Operation::none) {
            CancelIoEx(pipe_, &io_);
            DWORD ignored = 0;
            GetOverlappedResult(pipe_, &io_, &ignored, TRUE);
        }
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    operation_ = Operation::none;
    ready_ = false;
    published_.clear();
    incoming_.clear();
    pong_.clear();
    deadline_.reset();
    send_after_.reset();
    if (event_) ResetEvent(event_);
}

void Discord::update(std::string id, std::optional<Activity> value, bool paused)
{
    activity_id_ = std::move(id);
    activity_ = std::move(value);
    std::optional<PresenceLevel> level;
    if (activity_ && !activity_id_.empty() && event_)
        level = paused ? PresenceLevel::vlc_paused : PresenceLevel::vlc_playing;
    priority_.set(level);
    sync_priority(true);
}

void Discord::sync_priority(bool updated)
{
    const auto was_yielding = yielding_;
    yielding_ = priority_.blocked();
    priority_check_ = priority_.can_yield() ? std::optional(Clock::now() + 500ms) : std::nullopt;
    if (updated || was_yielding != yielding_) {
        if (yielding_) set_activity(activity_id_, std::nullopt);
        else set_activity(activity_id_, activity_);
    }
}

void Discord::set_activity(std::string id, const std::optional<Activity>& value)
{
    if (id_ != id) {
        disconnect();
        id_ = std::move(id);
        retry_ = 0;
    }
    if (!value || id_.empty() || !event_) {
        if (pipe_ != INVALID_HANDLE_VALUE && ready_ && !published_.empty()) {
            // Best effort clear before closing. Discord also removes the activity on disconnect.
            if (operation_ == Operation::read) {
                CancelIoEx(pipe_, &io_);
                DWORD ignored = 0;
                GetOverlappedResult(pipe_, &io_, &ignored, TRUE);
                operation_ = Operation::none;
            }
            if (operation_ == Operation::none) {
                write(1, json{{"cmd", "SET_ACTIVITY"}, {"nonce", std::to_string(++nonce_)},
                              {"args", {{"pid", GetCurrentProcessId()}, {"activity", nullptr}}}}.dump());
            }
        }
        wanted_.clear();
        disconnect();
        retry_ = 0;
        return;
    }
    json activity{{"type", value->type}, {"details", clean(value->title)}};
    if (!value->detail.empty()) activity["state"] = clean(value->detail);
    if (!value->image.empty()) activity["assets"] = {{"large_image", value->image}};
    if (value->end > 0) activity["timestamps"] = {{"end", value->end}};
    auto payload = activity.dump(-1, ' ', false, json::error_handler_t::replace);
    if (wanted_ == payload && (deadline_ || pipe_ != INVALID_HANDLE_VALUE))
        return;
    wanted_ = std::move(payload);
    if (pipe_ == INVALID_HANDLE_VALUE && !deadline_)
        connect();
    else if (ready_)
        publish();
}

void Discord::connect()
{
    if (wanted_.empty() || id_.empty())
        return;
    for (wchar_t slot = L'0'; slot <= L'9'; ++slot) {
        const auto name = std::wstring(LR"(\\.\pipe\discord-ipc-)") + slot;
        pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE)
            break;
    }
    if (pipe_ == INVALID_HANDLE_VALUE) {
        failed();
        return;
    }
    deadline_ = Clock::now() + 4s;
    write(0, json{{"v", 1}, {"client_id", id_}}.dump());
}

void Discord::write(int opcode, std::string payload)
{
    const std::uint32_t header[] = {static_cast<std::uint32_t>(opcode), static_cast<std::uint32_t>(payload.size())};
    outgoing_.assign(reinterpret_cast<const char*>(header), sizeof(header));
    outgoing_ += payload;
    io_ = {};
    io_.hEvent = event_;
    ResetEvent(event_);
    operation_ = Operation::write;
    DWORD transferred = 0;
    if (WriteFile(pipe_, outgoing_.data(), static_cast<DWORD>(outgoing_.size()), &transferred, &io_))
        SetEvent(event_);
    else if (GetLastError() != ERROR_IO_PENDING) {
        operation_ = Operation::none;
        failed();
    }
}

void Discord::read()
{
    io_ = {};
    io_.hEvent = event_;
    ResetEvent(event_);
    operation_ = Operation::read;
    DWORD transferred = 0;
    if (ReadFile(pipe_, buffer_, sizeof(buffer_), &transferred, &io_))
        SetEvent(event_);
    else if (GetLastError() != ERROR_IO_PENDING) {
        operation_ = Operation::none;
        failed();
    }
}

void Discord::process_frames()
{
    while (incoming_.size() >= 8) {
        std::uint32_t header[2]{};
        std::memcpy(header, incoming_.data(), sizeof(header));
        const auto [opcode, length] = header;
        if (opcode == 2 || length > 65536) {
            failed();
            return;
        }
        if (incoming_.size() < 8 + length)
            return;
        auto payload = incoming_.substr(8, length);
        incoming_.erase(0, 8 + length);
        if (opcode == 3) {
            pong_ = std::move(payload);
        } else if (opcode == 1) {
            const auto reply = json::parse(payload, nullptr, false);
            if (reply.is_object() && reply.contains("evt") && reply["evt"] == "READY") {
                ready_ = true;
                deadline_.reset();
                retry_ = 0;
            } else if (reply.is_object() && reply.value("cmd", "") == "SET_ACTIVITY") {
                if (reply.value("evt", nlohmann::json(nullptr)) == "ERROR") rejected_ = true;
                else acknowledged_ = true;
            }
        }
    }
}

void Discord::publish()
{
    if (!ready_ || published_ == wanted_ || pipe_ == INVALID_HANDLE_VALUE)
        return;
    if (operation_ == Operation::write)
        return;
    const auto now = Clock::now();
    // Discord limits activity updates to five in a rolling twenty-second window.
    constexpr auto window = 20s + 100ms;
    while (!recent_updates_.empty() && now >= recent_updates_.front() + window)
        recent_updates_.pop_front();
    if (recent_updates_.size() >= 5) {
        send_after_ = recent_updates_.front() + window;
        return;
    }
    send_after_.reset();
    if (operation_ == Operation::read) {
        CancelIoEx(pipe_, &io_);
        DWORD transferred = 0;
        if (GetOverlappedResult(pipe_, &io_, &transferred, TRUE) && transferred) {
            incoming_.append(buffer_, transferred);
            process_frames();
        }
        operation_ = Operation::none;
        if (pipe_ == INVALID_HANDLE_VALUE)
            return;
    }
    recent_updates_.push_back(now);
    published_ = wanted_;
    write(1, json{{"cmd", "SET_ACTIVITY"}, {"nonce", std::to_string(++nonce_)},
                  {"args", {{"pid", GetCurrentProcessId()}, {"activity", json::parse(wanted_)}}}}.dump());
}

void Discord::complete(DWORD transferred)
{
    const auto previous = operation_;
    operation_ = Operation::none;
    if (!transferred || (previous == Operation::write && transferred != outgoing_.size())) {
        failed();
        return;
    }
    if (previous == Operation::read) {
        incoming_.append(buffer_, transferred);
        process_frames();
    }
    if (pipe_ == INVALID_HANDLE_VALUE)
        return;
    if (!pong_.empty()) {
        auto payload = std::move(pong_);
        pong_.clear();
        write(4, std::move(payload));
    } else {
        publish();
        if (operation_ == Operation::none && pipe_ != INVALID_HANDLE_VALUE)
            read();
    }
}

void Discord::failed()
{
    disconnect();
    if (!wanted_.empty()) {
        deadline_ = Clock::now() + std::chrono::seconds(std::min(30u, 1u << std::min(retry_, 5u)));
        ++retry_;
    }
}

bool Discord::take_acknowledgement()
{
    return std::exchange(acknowledged_, false);
}

bool Discord::take_rejection()
{
    return std::exchange(rejected_, false);
}

std::optional<Discord::Clock::time_point> Discord::deadline() const
{
    auto next = deadline_;
    for (const auto deadline : {send_after_, priority_check_})
        if (deadline && (!next || *deadline < *next)) next = deadline;
    return next;
}

void Discord::pump()
{
    if (priority_check_ && Clock::now() >= *priority_check_) sync_priority(false);
    if (event_ && operation_ != Operation::none && WaitForSingleObject(event_, 0) == WAIT_OBJECT_0) {
        DWORD transferred = 0;
        if (GetOverlappedResult(pipe_, &io_, &transferred, FALSE))
            complete(transferred);
        else if (GetLastError() != ERROR_IO_INCOMPLETE) {
            operation_ = Operation::none;
            failed();
        }
    }
    if (send_after_ && Clock::now() >= *send_after_) {
        send_after_.reset();
        publish();
    }
    if (deadline_ && Clock::now() >= *deadline_) {
        if (pipe_ == INVALID_HANDLE_VALUE)
            connect();
        else
            failed();
    }
}
}
