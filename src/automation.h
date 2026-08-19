#pragma once

#include "common.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace kbun {

class AutomationWorker {
public:
    AutomationWorker() = default;
    ~AutomationWorker();

    AutomationWorker(const AutomationWorker&) = delete;
    AutomationWorker& operator=(const AutomationWorker&) = delete;

    bool Start(HWND replyWindow);
    void Stop();

    void RequestScan(std::uint64_t generation);
    void CancelScan(std::uint64_t generation);
    void Activate(std::uint64_t generation, std::uint64_t elementId);
    void SendCaretInput(CaretInput input);

private:
    enum class CommandType {
        Scan,
        Activate,
        Caret,
        Stop,
    };

    struct Command {
        CommandType type = CommandType::Stop;
        std::uint64_t generation = 0;
        std::uint64_t elementId = 0;
        CaretInput caret{};
    };

    void Push(Command command);
    void ThreadMain();

    HWND replyWindow_ = nullptr;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Command> commands_;
    bool stopping_ = false;
    std::atomic<std::uint64_t> latestScan_{0};
};

}  // namespace kbun

