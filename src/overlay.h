#pragma once

#include "config.h"
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
    void SetAppearance(const Config& config);
    void Begin();
    void ShowHints(const std::vector<DisplayHint>& hints);
    void ShowSelection(const ElementInfo& element);
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
    void RecreateFont();
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
    GradientMode gradientMode_ = GradientMode::Rainbow;
    std::vector<COLORREF> gradientColors_{RGB(0, 122, 255), RGB(255, 45, 85)};
    COLORREF labelTextColor_ = RGB(255, 255, 255);
    bool highContrastLabels_ = false;
    UINT labelScalePercent_ = 100;
    bool shown_ = false;
};

}  // namespace kbun
