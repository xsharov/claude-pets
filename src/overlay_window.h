#pragma once

#include "geometry.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace cp {

class Renderer;

using DisplayId = uintptr_t;

struct DisplayInfo {
    DisplayId id = 0;
    RectI work;
    RectI movable;
    float scale = 1.0f;
};

DisplayInfo DisplayFromPoint(PointI point);
DisplayInfo PrimaryDisplay();
std::optional<DisplayInfo> FindDisplay(const std::wstring& key);
std::wstring DisplayKey(DisplayId display);
PointI CursorPosition();
bool PrimaryButtonDown();
bool AltKeyDown();
bool ReducedMotionEnabled();

enum class CursorShape : uint8_t { Arrow, Hand, Move };

class OverlayWindowEvents {
public:
    virtual void OnPointerMove(PointI client, bool primaryDown) = 0;
    virtual void OnPointerLeave() = 0;
    virtual void OnPrimaryDown(PointI client) = 0;
    virtual void OnPrimaryUp(PointI client) = 0;
    virtual void OnSecondaryUp(PointI client) = 0;
    virtual void OnWheel(int delta) = 0;
    virtual void OnCaptureLost() = 0;
    virtual CursorShape CursorAt(PointI client) = 0;
    virtual bool Interactive(PointI client) = 0;
    virtual void OnFrame() = 0;
    virtual void OnTimer(uint32_t id) = 0;
    virtual void OnDisplayChanged() = 0;
    virtual void OnReducedMotionChanged() = 0;
    virtual void Paint() = 0;

protected:
    ~OverlayWindowEvents() = default;
};

class OverlayWindow {
public:
    OverlayWindow();
    ~OverlayWindow();
    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    bool Create(OverlayWindowEvents& events, Renderer& renderer);
    void Destroy();
    bool Created() const;

    void Show();
    void Hide();
    void EnsureTopmost();
    void Move(PointI origin);
    bool Present(const RectI& bounds, float scale, const RectI* dirty);

    void SetTimer(uint32_t id, uint32_t delayMs);
    void KillTimer(uint32_t id);
    void TrackPointerLeave();
    void Capture();
    void ReleaseCapture();
    PointI ScreenToClient(PointI point) const;
    PointI ClientToScreen(PointI point) const;

    void SetFrameClockActive(bool active);
    bool FrameClockActive() const;
    bool FrameClockRunning() const;
    void SetFrameClockDisplay(DisplayId display);

#if defined(_WIN32)
    HWND Handle() const;
    void SetMessageHook(std::function<bool(UINT, WPARAM, LPARAM, LRESULT&)> hook);
#else
    void* NativeWindow() const;
#endif

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}
