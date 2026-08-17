#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <vector>

struct LiveDesktopFrame {
    RECT bounds{};
    int width = 0;
    int height = 0;
    std::uint64_t generation = 0;
    std::vector<DWORD> pixels;
};

// Captures the real Explorer desktop composition (including live-wallpaper
// child surfaces) without capturing DesktopOrganizer's separate top-level
// windows. Frames are delivered through a coalesced window message.
class LiveDesktopCapture {
public:
    LiveDesktopCapture();
    ~LiveDesktopCapture();

    LiveDesktopCapture(const LiveDesktopCapture&) = delete;
    LiveDesktopCapture& operator=(const LiveDesktopCapture&) = delete;

    bool Start(HWND desktopWindow, HWND notifyWindow, UINT notifyMessage);
    void Stop();
    bool TakeLatestFrame(LiveDesktopFrame& frame);
    bool IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
