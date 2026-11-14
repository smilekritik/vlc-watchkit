#pragma once

#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <windows.h>

namespace addon {
struct Activity {
    std::string title, detail, image;
    int type = 3;
    std::int64_t end = 0;
};

class Discord {
public:
    using Clock = std::chrono::steady_clock;
    Discord() = default;
    ~Discord();
    void update(std::string id, std::optional<Activity> activity);
    void pump();
    bool take_acknowledgement();
    bool take_rejection();
    HANDLE event() const { return event_; }
    std::optional<Clock::time_point> deadline() const;
private:
    enum class Operation { none, read, write };
    void disconnect();
    void connect();
    void write(int opcode, std::string payload);
    void read();
    void complete(DWORD transferred);
    void process_frames();
    void publish();
    void failed();

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED io_{};
    Operation operation_ = Operation::none;
    std::string id_, wanted_, published_, outgoing_, incoming_, pong_;
    char buffer_[4096]{};
    bool ready_ = false, acknowledged_ = false, rejected_ = false;
    unsigned nonce_ = 0;
    unsigned retry_ = 0;
    std::optional<Clock::time_point> deadline_, send_after_;
    std::deque<Clock::time_point> recent_updates_;
};
}
