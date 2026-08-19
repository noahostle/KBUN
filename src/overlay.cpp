#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <string>

namespace kbun {
namespace {

constexpr wchar_t kOverlayClass[] = L"KBUN.Overlay";
constexpr wchar_t kCaretClass[] = L"KBUN.Caret";
constexpr UINT_PTR kFadeTimer = 1;
constexpr COLORREF kTransparentKey = RGB(1, 2, 3);
constexpr COLORREF kDimOutline = RGB(88, 102, 110);
constexpr COLORREF kVeilPixel = RGB(74, 78, 84);
constexpr COLORREF kLabelText = RGB(255, 255, 255);
constexpr COLORREF kLabelShadow = RGB(12, 16, 20);
constexpr BYTE kVeilAlpha = 30;
constexpr BYTE kShadowAlpha = 82;

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

COLORREF HsvColor(double hue, double saturation, double value) {
    hue -= std::floor(hue);
    const double scaled = hue * 6.0;
    const int sector = static_cast<int>(scaled);
    const double fraction = scaled - sector;
    const double p = value * (1.0 - saturation);
    const double q = value * (1.0 - saturation * fraction);
    const double t = value * (1.0 - saturation * (1.0 - fraction));

    double red = value;
    double green = t;
    double blue = p;
    switch (sector % 6) {
        case 0: red = value; green = t; blue = p; break;
        case 1: red = q; green = value; blue = p; break;
        case 2: red = p; green = value; blue = t; break;
        case 3: red = p; green = q; blue = value; break;
        case 4: red = t; green = p; blue = value; break;
        case 5: red = value; green = p; blue = q; break;
        default: break;
    }
    return RGB(
        static_cast<BYTE>(std::lround(red * 255.0)),
        static_cast<BYTE>(std::lround(green * 255.0)),
        static_cast<BYTE>(std::lround(blue * 255.0)));
}

COLORREF RainbowAt(POINT screenPoint, const RECT& virtualScreen) {
    const double x = RectWidth(virtualScreen) > 0
        ? static_cast<double>(screenPoint.x - virtualScreen.left) / RectWidth(virtualScreen)
        : 0.0;
    const double y = RectHeight(virtualScreen) > 0
        ? static_cast<double>(screenPoint.y - virtualScreen.top) / RectHeight(virtualScreen)
        : 0.0;
    return HsvColor(x * 0.86 + y * 0.14 + 0.92, 0.72, 1.0);
}

bool PixelMatches(BYTE red, BYTE green, BYTE blue, COLORREF color) {
    return red == static_cast<BYTE>(color & 0xffU) &&
           green == static_cast<BYTE>((color >> 8U) & 0xffU) &&
           blue == static_cast<BYTE>((color >> 16U) & 0xffU);
}

int CornerRadius(const RECT& bounds) {
    const int smallest = std::min(RectWidth(bounds), RectHeight(bounds));
    return std::clamp(5 + smallest / 10, 6, 24);
}

void DrawGradientLine(
    HDC dc,
    POINT start,
    POINT end,
    POINT screenOffset,
    const RECT& virtualScreen,
    int thickness) {
    const int length = std::max(std::abs(end.x - start.x), std::abs(end.y - start.y));
    const int segments = std::clamp((length + 55) / 56, 1, 20);
    HPEN pen = CreatePen(PS_SOLID, thickness, RGB(255, 255, 255));
    HGDIOBJ previousPen = SelectObject(dc, pen);
    for (int index = 0; index < segments; ++index) {
        const double from = static_cast<double>(index) / segments;
        const double to = static_cast<double>(index + 1) / segments;
        const POINT a{
            start.x + static_cast<LONG>(std::lround((end.x - start.x) * from)),
            start.y + static_cast<LONG>(std::lround((end.y - start.y) * from)),
        };
        const POINT b{
            start.x + static_cast<LONG>(std::lround((end.x - start.x) * to)),
            start.y + static_cast<LONG>(std::lround((end.y - start.y) * to)),
        };
        const POINT sample{
            screenOffset.x + (a.x + b.x) / 2,
            screenOffset.y + (a.y + b.y) / 2,
        };
        LOGBRUSH brush{BS_SOLID, RainbowAt(sample, virtualScreen), 0};
        HPEN segmentPen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_FLAT,
                                      thickness, &brush, 0, nullptr);
        SelectObject(dc, segmentPen);
        MoveToEx(dc, a.x, a.y, nullptr);
        LineTo(dc, b.x, b.y);
        SelectObject(dc, pen);
        DeleteObject(segmentPen);
    }
    SelectObject(dc, previousPen);
    DeleteObject(pen);
}

void DrawRoundedOutline(
    HDC dc,
    const RECT& bounds,
    const RECT& virtualScreen,
    bool dimmed,
    int thickness) {
    const POINT localCenter = RectCenter(bounds);
    const POINT screenCenter{localCenter.x + virtualScreen.left, localCenter.y + virtualScreen.top};
    const COLORREF accent = dimmed ? kDimOutline : RainbowAt(screenCenter, virtualScreen);
    const int radius = CornerRadius(bounds);
    HPEN pen = CreatePen(PS_SOLID, thickness, accent);
    HGDIOBJ previousPen = SelectObject(dc, pen);
    HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, radius * 2, radius * 2);
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(pen);

    if (dimmed) return;
    const POINT screenOffset{virtualScreen.left, virtualScreen.top};
    if (RectWidth(bounds) > radius * 2 + 8) {
        DrawGradientLine(dc, {bounds.left + radius, bounds.top}, {bounds.right - radius, bounds.top},
                         screenOffset, virtualScreen, thickness);
        DrawGradientLine(dc, {bounds.left + radius, bounds.bottom - 1}, {bounds.right - radius, bounds.bottom - 1},
                         screenOffset, virtualScreen, thickness);
    }
    if (RectHeight(bounds) > radius * 2 + 8) {
        DrawGradientLine(dc, {bounds.left, bounds.top + radius}, {bounds.left, bounds.bottom - radius},
                         screenOffset, virtualScreen, thickness);
        DrawGradientLine(dc, {bounds.right - 1, bounds.top + radius}, {bounds.right - 1, bounds.bottom - radius},
                         screenOffset, virtualScreen, thickness);
    }
}

void DrawBadgeGradient(
    HDC dc,
    const RECT& bounds,
    const RECT& virtualScreen,
    bool dimmed,
    int radius) {
    HRGN clip = CreateRoundRectRgn(
        bounds.left,
        bounds.top,
        bounds.right + 1,
        bounds.bottom + 1,
        radius * 2,
        radius * 2);
    const int saved = SaveDC(dc);
    SelectClipRgn(dc, clip);
    HGDIOBJ previousPen = SelectObject(dc, GetStockObject(DC_PEN));

    const int width = std::max(1, RectWidth(bounds));
    for (int x = 0; x < width; ++x) {
        const double progress = width == 1 ? 0.5 : static_cast<double>(x) / (width - 1);
        const int sampleOffset = static_cast<int>(std::lround((progress - 0.5) * 150.0));
        const POINT sample{
            bounds.left + x + virtualScreen.left + sampleOffset,
            (bounds.top + bounds.bottom) / 2 + virtualScreen.top,
        };
        const COLORREF color = dimmed ? kDimOutline : RainbowAt(sample, virtualScreen);
        SetDCPenColor(dc, color);
        MoveToEx(dc, bounds.left + x, bounds.top, nullptr);
        LineTo(dc, bounds.left + x, bounds.bottom);
    }

    SelectObject(dc, previousPen);
    RestoreDC(dc, saved);
    DeleteObject(clip);
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

    WNDCLASSEXW caretClass = windowClass;
    caretClass.lpfnWndProc = CaretWindowProcedure;
    caretClass.lpszClassName = kCaretClass;
    if (!RegisterClassExW(&caretClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

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

    caretWindow_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kCaretClass,
        L"KBUN Caret",
        WS_POPUP,
        0,
        0,
        7,
        20,
        nullptr,
        nullptr,
        instance_,
        this);
    if (!caretWindow_) return false;
    SetLayeredWindowAttributes(caretWindow_, kTransparentKey, 238, LWA_COLORKEY | LWA_ALPHA);
    return RecreateSurface();
}

void OverlayWindow::Destroy() {
    if (caretWindow_) {
        DestroyWindow(caretWindow_);
        caretWindow_ = nullptr;
    }
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
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = RectWidth(virtualScreen_);
    bitmapInfo.bmiHeader.biHeight = -RectHeight(virtualScreen_);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(
        screen,
        &bitmapInfo,
        DIB_RGB_COLORS,
        &bitmapPixels_,
        nullptr,
        0);
    ReleaseDC(nullptr, screen);
    if (!memoryDc_ || !bitmap_ || !bitmapPixels_) return false;
    previousBitmap_ = SelectObject(memoryDc_, bitmap_);

    if (!font_) {
        const UINT dpi = GetDpiForSystem();
        font_ = CreateFontW(
            -MulDiv(9, static_cast<int>(dpi), 72),
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
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI Variable Text");
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
    bitmapPixels_ = nullptr;
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

    if (memoryDc_ && bitmapPixels_) {
        GdiFlush();
        const std::size_t bytes = static_cast<std::size_t>(RectWidth(virtualScreen_)) *
            static_cast<std::size_t>(RectHeight(virtualScreen_)) * 4U;
        std::memset(bitmapPixels_, 0, bytes);
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
    ApplyAlpha();
}

void OverlayWindow::ShowCaret(const RECT& bounds) {
    if (!caretWindow_ || RectHeight(bounds) <= 0) return;
    const int height = std::clamp(RectHeight(bounds), 12, 80);
    const int x = bounds.left - 3;
    const int y = bounds.top;
    caretColor_ = RainbowAt({bounds.left, bounds.top + height / 2}, virtualScreen_);
    SetWindowPos(caretWindow_, HWND_TOPMOST, x, y, 7, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(caretWindow_, nullptr, FALSE);
    UpdateWindow(caretWindow_);
}

void OverlayWindow::HideCaret() {
    if (caretWindow_) ShowWindow(caretWindow_, SW_HIDE);
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
    if (!window_ || !memoryDc_ || !bitmap_) return;
    HDC screen = GetDC(nullptr);
    POINT destination{virtualScreen_.left, virtualScreen_.top};
    POINT source{0, 0};
    SIZE size{RectWidth(virtualScreen_), RectHeight(virtualScreen_)};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha_, AC_SRC_ALPHA};
    UpdateLayeredWindow(
        window_,
        screen,
        &destination,
        &size,
        memoryDc_,
        &source,
        0,
        &blend,
        ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void OverlayWindow::PrepareSurfaceAlpha() {
    if (!bitmapPixels_) return;
    GdiFlush();

    const int pixelCount = RectWidth(virtualScreen_) * RectHeight(virtualScreen_);
    auto* pixels = static_cast<BYTE*>(bitmapPixels_);
    for (int index = 0; index < pixelCount; ++index) {
        BYTE* pixel = pixels + index * 4;
        const BYTE blue = pixel[0];
        const BYTE green = pixel[1];
        const BYTE red = pixel[2];

        BYTE pixelAlpha = 255;
        if ((red | green | blue) == 0) continue;
        if (PixelMatches(red, green, blue, kVeilPixel)) {
            pixelAlpha = kVeilAlpha;
        } else if (PixelMatches(red, green, blue, kLabelShadow)) {
            pixelAlpha = kShadowAlpha;
        }

        pixel[0] = static_cast<BYTE>((static_cast<unsigned>(blue) * pixelAlpha + 127) / 255);
        pixel[1] = static_cast<BYTE>((static_cast<unsigned>(green) * pixelAlpha + 127) / 255);
        pixel[2] = static_cast<BYTE>((static_cast<unsigned>(red) * pixelAlpha + 127) / 255);
        pixel[3] = pixelAlpha;
    }
}

void OverlayWindow::Render(const std::vector<DisplayHint>& hints) {
    const int surfaceWidth = RectWidth(virtualScreen_);
    const int surfaceHeight = RectHeight(virtualScreen_);
    RECT surface{0, 0, surfaceWidth, surfaceHeight};
    GdiFlush();
    const std::size_t bytes = static_cast<std::size_t>(surfaceWidth) *
        static_cast<std::size_t>(surfaceHeight) * 4U;
    std::memset(bitmapPixels_, 0, bytes);

    const HGDIOBJ previousFont = SelectObject(memoryDc_, font_);
    SetBkMode(memoryDc_, TRANSPARENT);

    HBRUSH veilBrush = CreateSolidBrush(kVeilPixel);
    HGDIOBJ previousPen = SelectObject(memoryDc_, GetStockObject(NULL_PEN));
    HGDIOBJ previousBrush = SelectObject(memoryDc_, veilBrush);
    for (const DisplayHint& hint : hints) {
        if (!hint.drawOutline) continue;
        RECT bounds = ToLocal(hint.bounds, virtualScreen_);
        RECT clipped{};
        if (!IntersectRect(&clipped, &bounds, &surface)) continue;
        const int radius = CornerRadius(bounds);
        RoundRect(memoryDc_, bounds.left, bounds.top, bounds.right, bounds.bottom, radius * 2, radius * 2);
    }
    SelectObject(memoryDc_, previousBrush);
    SelectObject(memoryDc_, previousPen);
    DeleteObject(veilBrush);

    for (const DisplayHint& hint : hints) {
        RECT bounds = ToLocal(hint.bounds, virtualScreen_);
        RECT clipped{};
        if (!IntersectRect(&clipped, &bounds, &surface)) continue;

        if (hint.drawOutline) {
            DrawRoundedOutline(
                memoryDc_,
                bounds,
                virtualScreen_,
                !hint.prefixMatch,
                hint.isGroup ? 4 : 3);
        }

        std::wstring code = hint.code;
        std::ranges::transform(code, code.begin(), [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });

        SIZE textSize{};
        GetTextExtentPoint32W(
            memoryDc_,
            code.c_str(),
            static_cast<int>(code.size()),
            &textSize);
        const bool textTarget = !hint.isGroup && hint.role != ElementRole::Action;
        constexpr int horizontalPadding = 5;
        constexpr int verticalPadding = 3;
        constexpr int iconGap = 4;
        constexpr int iconWidth = 7;
        const int labelHeight = std::max(22, static_cast<int>(textSize.cy) + verticalPadding * 2);
        const int labelWidth = std::max(labelHeight, static_cast<int>(textSize.cx) + horizontalPadding * 2);

        const POINT center = RectCenter(bounds);
        RECT label{
            center.x - labelWidth / 2,
            center.y - labelHeight / 2,
            center.x - labelWidth / 2 + labelWidth,
            center.y - labelHeight / 2 + labelHeight,
        };
        label = ClampLabel(label, surfaceWidth, surfaceHeight);
        if (textTarget && label.right + iconGap + iconWidth > surfaceWidth - 2) {
            OffsetRect(&label, surfaceWidth - 2 - (label.right + iconGap + iconWidth), 0);
        }

        const POINT labelScreenCenter{center.x + virtualScreen_.left, center.y + virtualScreen_.top};
        const COLORREF accent = hint.prefixMatch ? RainbowAt(labelScreenCenter, virtualScreen_) : kDimOutline;
        RECT shadow = label;
        OffsetRect(&shadow, 1, 2);
        HBRUSH shadowBrush = CreateSolidBrush(kLabelShadow);
        previousPen = SelectObject(memoryDc_, GetStockObject(NULL_PEN));
        previousBrush = SelectObject(memoryDc_, shadowBrush);
        constexpr int badgeRadius = 5;
        RoundRect(memoryDc_, shadow.left, shadow.top, shadow.right, shadow.bottom,
                  badgeRadius * 2, badgeRadius * 2);
        SelectObject(memoryDc_, previousBrush);
        DeleteObject(shadowBrush);

        DrawBadgeGradient(memoryDc_, label, virtualScreen_, !hint.prefixMatch, badgeRadius);
        HPEN badgeBorder = CreatePen(PS_SOLID, 1, accent);
        previousPen = SelectObject(memoryDc_, badgeBorder);
        previousBrush = SelectObject(memoryDc_, GetStockObject(HOLLOW_BRUSH));
        RoundRect(memoryDc_, label.left, label.top, label.right, label.bottom,
                  badgeRadius * 2, badgeRadius * 2);
        SelectObject(memoryDc_, previousBrush);
        SelectObject(memoryDc_, previousPen);
        DeleteObject(badgeBorder);

        RECT textBounds = label;
        SetTextColor(memoryDc_, kLabelText);
        DrawTextW(
            memoryDc_,
            code.c_str(),
            static_cast<int>(code.size()),
            &textBounds,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (textTarget) {
            const int iconLeft = label.right + iconGap;
            const int iconCenter = iconLeft + iconWidth / 2;
            const int iconTop = label.top + 4;
            const int iconBottom = label.bottom - 4;
            HPEN iconHalo = CreatePen(PS_SOLID, 3, kLabelShadow);
            HGDIOBJ oldPen = SelectObject(memoryDc_, iconHalo);
            MoveToEx(memoryDc_, iconCenter, iconTop, nullptr);
            LineTo(memoryDc_, iconCenter, iconBottom + 1);
            MoveToEx(memoryDc_, iconCenter - 2, iconTop, nullptr);
            LineTo(memoryDc_, iconCenter + 3, iconTop);
            MoveToEx(memoryDc_, iconCenter - 2, iconBottom, nullptr);
            LineTo(memoryDc_, iconCenter + 3, iconBottom);
            SelectObject(memoryDc_, oldPen);
            DeleteObject(iconHalo);

            HPEN iconPen = CreatePen(PS_SOLID, 1, kLabelText);
            oldPen = SelectObject(memoryDc_, iconPen);
            MoveToEx(memoryDc_, iconCenter, iconTop, nullptr);
            LineTo(memoryDc_, iconCenter, iconBottom + 1);
            MoveToEx(memoryDc_, iconCenter - 2, iconTop, nullptr);
            LineTo(memoryDc_, iconCenter + 3, iconTop);
            MoveToEx(memoryDc_, iconCenter - 2, iconBottom, nullptr);
            LineTo(memoryDc_, iconCenter + 3, iconBottom);
            SelectObject(memoryDc_, oldPen);
            DeleteObject(iconPen);
        }
    }

    SelectObject(memoryDc_, previousFont);
    PrepareSurfaceAlpha();
}

void OverlayWindow::PaintCaret() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(caretWindow_, &paint);
    RECT client{};
    GetClientRect(caretWindow_, &client);
    HBRUSH clear = CreateSolidBrush(kTransparentKey);
    FillRect(dc, &client, clear);
    DeleteObject(clear);

    HPEN shadow = CreatePen(PS_SOLID, 3, kLabelShadow);
    HGDIOBJ previousPen = SelectObject(dc, shadow);
    const int center = RectWidth(client) / 2;
    MoveToEx(dc, center + 1, 1, nullptr);
    LineTo(dc, center + 1, client.bottom - 1);
    SelectObject(dc, previousPen);
    DeleteObject(shadow);

    HPEN accent = CreatePen(PS_SOLID, 2, caretColor_);
    previousPen = SelectObject(dc, accent);
    MoveToEx(dc, center, 1, nullptr);
    LineTo(dc, center, client.bottom - 1);
    MoveToEx(dc, center - 2, 1, nullptr);
    LineTo(dc, center + 3, 1);
    MoveToEx(dc, center - 2, client.bottom - 2, nullptr);
    LineTo(dc, center + 3, client.bottom - 2);
    SelectObject(dc, previousPen);
    DeleteObject(accent);
    EndPaint(caretWindow_, &paint);
}

void OverlayWindow::Paint() {
    PAINTSTRUCT paint{};
    BeginPaint(window_, &paint);
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

LRESULT CALLBACK OverlayWindow::CaretWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    OverlayWindow* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OverlayWindow*>(create->lpCreateParams);
        self->caretWindow_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleCaretMessage(message, wParam, lParam)
                : DefWindowProcW(window, message, wParam, lParam);
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

LRESULT OverlayWindow::HandleCaretMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            PaintCaret();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        default:
            return DefWindowProcW(caretWindow_, message, wParam, lParam);
    }
}

}  // namespace kbun
