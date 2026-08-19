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
    void ShowCaret(const RECT& bounds);
    void HideCaret();
    void FadeOut();
    void HideImmediately();

    [[nodiscard]] HWND Handle() const noexcept { return window_; }
    [[nodiscard]] bool IsShown() const noexcept { return shown_; }

private:
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK CaretWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleCaretMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool RecreateSurface();
    void ReleaseSurface();
    void PrepareSurfaceAlpha();
    void Render(const std::vector<DisplayHint>& hints);
    void Paint();
    void PaintCaret();
    void StartFade(BYTE target, UINT durationMs);
    void TickFade();
    void ApplyAlpha();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND caretWindow_ = nullptr;
    RECT virtualScreen_{};
    HDC memoryDc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previousBitmap_ = nullptr;
    void* bitmapPixels_ = nullptr;
    HFONT font_ = nullptr;
    COLORREF caretColor_ = RGB(255, 255, 255);
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
