#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace kbun {
namespace {

constexpr wchar_t kOverlayClass[] = L"KBUN.Overlay";
constexpr UINT_PTR kFadeTimer = 1;
constexpr COLORREF kTransparentKey = RGB(1, 2, 3);
constexpr COLORREF kActionOutline = RGB(42, 210, 190);
constexpr COLORREF kTextOutline = RGB(85, 170, 255);
constexpr COLORREF kGroupOutline = RGB(255, 182, 72);
constexpr COLORREF kDimOutline = RGB(88, 102, 110);
constexpr COLORREF kLabelBackground = RGB(15, 24, 31);
constexpr COLORREF kLabelText = RGB(248, 250, 252);

RECT ToLocal(RECT screenRect, const RECT& virtualScreen) {
    OffsetRect(&screenRect, -virtualScreen.left, -virtualScreen.top);
    return screenRect;
}

RECT ClampLabel(RECT label, int width, int height) {
    if (label.left < 2) OffsetRect(&label, 2 - label.left, 0);
    if (label.right > width - 2) OffsetRect(&label, width - 2 - label.right, 0);
    if (label.top < 2) OffsetRect(&label, 0, 2 - label.top);
    if (label.bottom > height - 2) OffsetRect(&label, 0, height - 2 - label.bottom);
    return label;
}

}  // namespace

OverlayWindow::~OverlayWindow() {
    Destroy();
}

bool OverlayWindow::Create(HINSTANCE instance) {
    if (window_) return true;
    instance_ = instance;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance_;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = kOverlayClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    virtualScreen_ = RECT{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };

    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kOverlayClass,
        L"KBUN Overlay",
        WS_POPUP,
        virtualScreen_.left,
        virtualScreen_.top,
        RectWidth(virtualScreen_),
        RectHeight(virtualScreen_),
        nullptr,
        nullptr,
        instance_,
        this);
    if (!window_) return false;

    SetLayeredWindowAttributes(window_, kTransparentKey, 0, LWA_COLORKEY | LWA_ALPHA);
    return RecreateSurface();
}

void OverlayWindow::Destroy() {
    if (window_) {
        KillTimer(window_, kFadeTimer);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    ReleaseSurface();
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
}

void OverlayWindow::SetFadeDurations(UINT fadeInMs, UINT fadeOutMs) {
    fadeInMs_ = fadeInMs;
    fadeOutMs_ = fadeOutMs;
}

bool OverlayWindow::RecreateSurface() {
    ReleaseSurface();
    virtualScreen_ = RECT{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };

    HDC screen = GetDC(nullptr);
    memoryDc_ = CreateCompatibleDC(screen);
    bitmap_ = CreateCompatibleBitmap(screen, RectWidth(virtualScreen_), RectHeight(virtualScreen_));
    ReleaseDC(nullptr, screen);
    if (!memoryDc_ || !bitmap_) return false;
    previousBitmap_ = SelectObject(memoryDc_, bitmap_);

    if (!font_) {
        const UINT dpi = GetDpiForSystem();
        font_ = CreateFontW(
            -MulDiv(11, static_cast<int>(dpi), 72),
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
    }
    return true;
}

void OverlayWindow::ReleaseSurface() {
    if (memoryDc_ && previousBitmap_) {
        SelectObject(memoryDc_, previousBitmap_);
        previousBitmap_ = nullptr;
    }
    if (bitmap_) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    if (memoryDc_) {
        DeleteDC(memoryDc_);
        memoryDc_ = nullptr;
    }
}

void OverlayWindow::Begin() {
    if (!window_) return;
    const RECT current{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
    if (!EqualRect(&current, &virtualScreen_)) RecreateSurface();

    if (memoryDc_) {
        HBRUSH clear = CreateSolidBrush(kTransparentKey);
        RECT surface{0, 0, RectWidth(virtualScreen_), RectHeight(virtualScreen_)};
        FillRect(memoryDc_, &surface, clear);
        DeleteObject(clear);
    }

    shown_ = true;
    SetWindowPos(
        window_,
        HWND_TOPMOST,
        virtualScreen_.left,
        virtualScreen_.top,
        RectWidth(virtualScreen_),
        RectHeight(virtualScreen_),
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, FALSE);
    StartFade(255, fadeInMs_);
}

void OverlayWindow::ShowHints(const std::vector<DisplayHint>& hints) {
    if (!window_ || !memoryDc_) return;
    Render(hints);
    InvalidateRect(window_, nullptr, FALSE);
    UpdateWindow(window_);
}

void OverlayWindow::FadeOut() {
    if (!window_ || !shown_) return;
    StartFade(0, fadeOutMs_);
}

void OverlayWindow::HideImmediately() {
    if (!window_) return;
    KillTimer(window_, kFadeTimer);
    alpha_ = 0;
    fadeTargetAlpha_ = 0;
    ApplyAlpha();
    ShowWindow(window_, SW_HIDE);
    shown_ = false;
}

void OverlayWindow::StartFade(BYTE target, UINT durationMs) {
    fadeStartAlpha_ = alpha_;
    fadeTargetAlpha_ = target;
    fadeDurationMs_ = durationMs;
    fadeStartedAt_ = GetTickCount64();
    if (durationMs == 0 || alpha_ == target) {
        alpha_ = target;
        ApplyAlpha();
        if (target == 0) {
            ShowWindow(window_, SW_HIDE);
            shown_ = false;
        }
        KillTimer(window_, kFadeTimer);
        return;
    }
    SetTimer(window_, kFadeTimer, 15, nullptr);
}

void OverlayWindow::TickFade() {
    const ULONGLONG elapsed = GetTickCount64() - fadeStartedAt_;
    const double progress = fadeDurationMs_ == 0
        ? 1.0
        : std::clamp(static_cast<double>(elapsed) / fadeDurationMs_, 0.0, 1.0);
    const double eased = 1.0 - std::pow(1.0 - progress, 3.0);
    alpha_ = static_cast<BYTE>(std::lround(
        static_cast<double>(fadeStartAlpha_) +
        (static_cast<double>(fadeTargetAlpha_) - fadeStartAlpha_) * eased));
    ApplyAlpha();

    if (progress >= 1.0) {
        alpha_ = fadeTargetAlpha_;
        ApplyAlpha();
        KillTimer(window_, kFadeTimer);
        if (alpha_ == 0) {
            ShowWindow(window_, SW_HIDE);
            shown_ = false;
        }
    }
}

void OverlayWindow::ApplyAlpha() {
    if (window_) SetLayeredWindowAttributes(window_, kTransparentKey, alpha_, LWA_COLORKEY | LWA_ALPHA);
}

void OverlayWindow::Render(const std::vector<DisplayHint>& hints) {
    const int surfaceWidth = RectWidth(virtualScreen_);
    const int surfaceHeight = RectHeight(virtualScreen_);
    RECT surface{0, 0, surfaceWidth, surfaceHeight};
    HBRUSH clear = CreateSolidBrush(kTransparentKey);
    FillRect(memoryDc_, &surface, clear);
    DeleteObject(clear);

    const HGDIOBJ previousFont = SelectObject(memoryDc_, font_);
    SetBkMode(memoryDc_, TRANSPARENT);

    for (const DisplayHint& hint : hints) {
        RECT bounds = ToLocal(hint.bounds, virtualScreen_);
        RECT clipped{};
        if (!IntersectRect(&clipped, &bounds, &surface)) continue;

        COLORREF outline = kActionOutline;
        if (!hint.prefixMatch) {
            outline = kDimOutline;
        } else if (hint.isGroup) {
            outline = kGroupOutline;
        } else if (hint.role != ElementRole::Action) {
            outline = kTextOutline;
        }

        HPEN pen = CreatePen(PS_SOLID, hint.isGroup ? 3 : 2, outline);
        HGDIOBJ oldPen = SelectObject(memoryDc_, pen);
        HGDIOBJ oldBrush = SelectObject(memoryDc_, GetStockObject(HOLLOW_BRUSH));
        Rectangle(memoryDc_, bounds.left, bounds.top, bounds.right, bounds.bottom);
        SelectObject(memoryDc_, oldBrush);
        SelectObject(memoryDc_, oldPen);
        DeleteObject(pen);

        SIZE textSize{};
        GetTextExtentPoint32W(
            memoryDc_,
            hint.code.c_str(),
            static_cast<int>(hint.code.size()),
            &textSize);
        constexpr int horizontalPadding = 5;
        constexpr int verticalPadding = 2;
        const int labelWidth = textSize.cx + horizontalPadding * 2;
        const int labelHeight = textSize.cy + verticalPadding * 2;

        RECT label{};
        if (hint.isGroup || hint.role != ElementRole::Action) {
            const POINT center = RectCenter(bounds);
            label = RECT{
                center.x - labelWidth / 2,
                center.y - labelHeight / 2,
                center.x - labelWidth / 2 + labelWidth,
                center.y - labelHeight / 2 + labelHeight,
            };
        } else {
            label = RECT{
                bounds.left + (RectWidth(bounds) - labelWidth) / 2,
                bounds.bottom + 3,
                bounds.left + (RectWidth(bounds) - labelWidth) / 2 + labelWidth,
                bounds.bottom + 3 + labelHeight,
            };
            if (label.bottom > surfaceHeight - 2) {
                OffsetRect(&label, 0, bounds.top - 3 - labelHeight - label.top);
            }
        }
        label = ClampLabel(label, surfaceWidth, surfaceHeight);

        HBRUSH background = CreateSolidBrush(kLabelBackground);
        FillRect(memoryDc_, &label, background);
        DeleteObject(background);
        FrameRect(memoryDc_, &label, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

        SetTextColor(memoryDc_, hint.prefixMatch ? kLabelText : RGB(160, 168, 174));
        DrawTextW(
            memoryDc_,
            hint.code.c_str(),
            static_cast<int>(hint.code.size()),
            &label,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    SelectObject(memoryDc_, previousFont);
}

void OverlayWindow::Paint() {
    PAINTSTRUCT paint{};
    HDC destination = BeginPaint(window_, &paint);
    if (memoryDc_) {
        BitBlt(
            destination,
            0,
            0,
            RectWidth(virtualScreen_),
            RectHeight(virtualScreen_),
            memoryDc_,
            0,
            0,
            SRCCOPY);
    }
    EndPaint(window_, &paint);
}

LRESULT CALLBACK OverlayWindow::WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    OverlayWindow* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OverlayWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT OverlayWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wParam == kFadeTimer) TickFade();
            return 0;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
    }
}

}  // namespace kbun
