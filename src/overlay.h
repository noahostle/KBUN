#pragma once

#include "hints.h"

#include <windows.h>

#include <cstdint>
#include <vector>

namespace kbun {

class OverlayWindow {
public:
    OverlayWindow() = default;
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    bool Create(HINSTANCE instance);
    void Destroy();

    void SetFadeDurations(UINT fadeInMs, UINT fadeOutMs);
    void Begin();
    void ShowHints(const std::vector<DisplayHint>& hints);
    void FadeOut();
    void HideImmediately();

    [[nodiscard]] HWND Handle() const noexcept { return window_; }
    [[nodiscard]] bool IsShown() const noexcept { return shown_; }

private:
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool RecreateSurface();
    void ReleaseSurface();
    void Render(const std::vector<DisplayHint>& hints);
    void Paint();
    void StartFade(BYTE target, UINT durationMs);
    void TickFade();
    void ApplyAlpha();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    RECT virtualScreen_{};
    HDC memoryDc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previousBitmap_ = nullptr;
    HFONT font_ = nullptr;
    BYTE alpha_ = 0;
    BYTE fadeStartAlpha_ = 0;
    BYTE fadeTargetAlpha_ = 0;
    UINT fadeDurationMs_ = 0;
    ULONGLONG fadeStartedAt_ = 0;
    UINT fadeInMs_ = 90;
    UINT fadeOutMs_ = 75;
    bool shown_ = false;
};

}  // namespace kbun

