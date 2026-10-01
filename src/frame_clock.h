#pragma once

#include <windows.h>

#include <atomic>
#include <thread>

namespace cp {

class FrameClock {
public:
    FrameClock() = default;
    ~FrameClock();
    FrameClock(const FrameClock&) = delete;
    FrameClock& operator=(const FrameClock&) = delete;

    bool Start(HWND target, UINT message);
    void Stop();
    void SetActive(bool active);
    bool Active() const { return active_.load(); }
    bool Running() const { return wake_ != nullptr; }
    void SetMonitor(HMONITOR monitor) { monitor_.store(monitor); }
    void FrameHandled() { pending_.store(false); }

private:
    void Run();

    std::thread thread_;
    HANDLE wake_ = nullptr;
    HWND target_ = nullptr;
    UINT message_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> pending_{false};
    std::atomic<HMONITOR> monitor_{nullptr};
};

}
