#include "frame_clock.h"

#include "util.h"

#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

namespace cp {

namespace {

using Microsoft::WRL::ComPtr;

constexpr double kFallbackPeriodMs = 1000.0 / 60.0;
constexpr double kShortestWaitMs = 1.0;
constexpr double kRetryDelayMs = 1000.0;

double RefreshPeriodMs(HMONITOR monitor) {
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info)) {
        return kFallbackPeriodMs;
    }
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayFrequency <= 1) {
        return kFallbackPeriodMs;
    }
    return 1000.0 / static_cast<double>(mode.dmDisplayFrequency);
}

ComPtr<IDXGIOutput> FindOutput(IDXGIFactory1* factory, HMONITOR monitor) {
    ComPtr<IDXGIOutput> fallback;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc = {};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) {
                continue;
            }
            if (desc.Monitor == monitor) {
                return output;
            }
            if (fallback == nullptr) {
                fallback = output;
            }
        }
    }
    return fallback;
}

}

FrameClock::~FrameClock() {
    Stop();
}

bool FrameClock::Start(HWND target, UINT message) {
    if (thread_.joinable()) {
        return true;
    }
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wake_ == nullptr) {
        return false;
    }
    target_ = target;
    message_ = message;
    stop_.store(false);
    pending_.store(false);
    thread_ = std::thread([this] { Run(); });
    return true;
}

void FrameClock::Stop() {
    if (thread_.joinable()) {
        stop_.store(true);
        SetEvent(wake_);
        thread_.join();
    }
    if (wake_ != nullptr) {
        CloseHandle(wake_);
        wake_ = nullptr;
    }
    active_.store(false);
}

void FrameClock::SetActive(bool active) {
    if (active_.exchange(active) != active && active && wake_ != nullptr) {
        SetEvent(wake_);
    }
}

void FrameClock::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIOutput> output;
    HMONITOR outputMonitor = nullptr;
    double periodMs = kFallbackPeriodMs;
    double retryAtMs = 0.0;
    while (!stop_.load()) {
        if (!active_.load()) {
            WaitForSingleObject(wake_, INFINITE);
            continue;
        }
        const HMONITOR wanted = monitor_.load();
        const double start = MonotonicMs();
        if ((output == nullptr || wanted != outputMonitor) && start >= retryAtMs) {
            if (factory == nullptr || !factory->IsCurrent()) {
                factory.Reset();
                CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            }
            output = factory != nullptr ? FindOutput(factory.Get(), wanted) : nullptr;
            outputMonitor = wanted;
            periodMs = RefreshPeriodMs(wanted);
            if (output == nullptr) {
                retryAtMs = start + kRetryDelayMs;
            }
        }
        const double waitStart = MonotonicMs();
        const bool waited = output != nullptr && SUCCEEDED(output->WaitForVBlank());
        if (!waited && output != nullptr) {
            output.Reset();
            retryAtMs = waitStart + kRetryDelayMs;
        }
        if (!waited || MonotonicMs() - waitStart < kShortestWaitMs) {
            Sleep(static_cast<DWORD>(std::max(1.0, std::floor(periodMs))));
        }
        if (stop_.load() || !active_.load()) {
            continue;
        }
        if (!pending_.exchange(true)) {
            PostMessageW(target_, message_, 0, 0);
        }
    }
}

}
