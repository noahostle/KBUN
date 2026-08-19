#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace kbun {

constexpr wchar_t kAppName[] = L"KBUN";
constexpr UINT kTrayIconId = 1;
constexpr UINT kMsgTray = WM_APP + 1;
constexpr UINT kMsgNavDown = WM_APP + 2;
constexpr UINT kMsgNavUp = WM_APP + 3;
constexpr UINT kMsgHintKey = WM_APP + 4;
constexpr UINT kMsgHintBack = WM_APP + 5;
constexpr UINT kMsgHintCancel = WM_APP + 6;
constexpr UINT kMsgPointerClick = WM_APP + 7;
constexpr UINT kMsgCaretKey = WM_APP + 8;
constexpr UINT kMsgScanComplete = WM_APP + 9;
constexpr UINT kMsgActivationComplete = WM_APP + 10;
constexpr UINT kMsgSettingsSaved = WM_APP + 11;
constexpr UINT kMsgCaretVisual = WM_APP + 12;

enum class ElementRole : std::uint8_t {
    Action,
    EditableText,
    ReadOnlyText,
};

struct ElementInfo {
    std::uint64_t id = 0;
    RECT bounds{};
    ElementRole role = ElementRole::Action;
    std::uint64_t sectionId = 0;
    RECT sectionBounds{};
    bool drawOutline = true;
    std::wstring name;
};

struct ScanResult {
    std::uint64_t generation = 0;
    HWND foreground = nullptr;
    DWORD elapsedMs = 0;
    std::vector<ElementInfo> elements;
};

enum class ActivationMode : std::uint8_t {
    Pointer,
    Editable,
    Caret,
};

struct ActivationResult {
    std::uint64_t generation = 0;
    std::uint64_t elementId = 0;
    ActivationMode mode = ActivationMode::Pointer;
    bool succeeded = false;
};

enum class CaretAction : std::uint8_t {
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    SelectAll,
    Copy,
    Exit,
};

struct CaretInput {
    CaretAction action = CaretAction::Exit;
    bool extend = false;
    bool control = false;
};

struct CaretVisualResult {
    bool visible = false;
    RECT bounds{};
};

inline int RectWidth(const RECT& rect) noexcept {
    return std::max(0L, rect.right - rect.left);
}

inline int RectHeight(const RECT& rect) noexcept {
    return std::max(0L, rect.bottom - rect.top);
}

inline std::int64_t RectArea(const RECT& rect) noexcept {
    return static_cast<std::int64_t>(RectWidth(rect)) * RectHeight(rect);
}

inline POINT RectCenter(const RECT& rect) noexcept {
    return POINT{rect.left + RectWidth(rect) / 2, rect.top + RectHeight(rect) / 2};
}

inline bool ContainsPoint(const RECT& rect, POINT point) noexcept {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

inline RECT UnionRects(const RECT& left, const RECT& right) noexcept {
    if (RectArea(left) == 0) return right;
    if (RectArea(right) == 0) return left;
    return RECT{
        std::min(left.left, right.left),
        std::min(left.top, right.top),
        std::max(left.right, right.right),
        std::max(left.bottom, right.bottom),
    };
}

}  // namespace kbun
