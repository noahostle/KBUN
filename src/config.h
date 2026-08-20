#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace kbun {

constexpr std::size_t kMaximumGradientColors = 15;

enum class UiTheme : UINT {
    Dark,
    Light,
};

enum class GradientMode : UINT {
    Rainbow,
    Custom,
};

struct KeyChoice {
    UINT virtualKey;
    const wchar_t* label;
};

struct Config {
    bool enabled = true;
    UINT navigationKey = VK_CAPITAL;
    UINT leftClickKey = VK_RETURN;
    UINT rightClickKey = VK_SPACE;
    UINT fadeInMs = 90;
    UINT fadeOutMs = 75;
    bool quickTypeFilters = false;
    bool automaticDoubleClick = false;
    UiTheme theme = UiTheme::Dark;
    GradientMode gradientMode = GradientMode::Rainbow;
    std::vector<COLORREF> gradientColors{RGB(0, 122, 255), RGB(255, 45, 85)};
    COLORREF labelTextColor = RGB(255, 255, 255);
    bool highContrastLabels = false;
    UINT labelScalePercent = 100;

    static Config Load();
    bool Save() const;
};

const std::vector<KeyChoice>& NavigationKeyChoices();
const std::vector<KeyChoice>& ClickKeyChoices();
bool IsValidNavigationKey(UINT virtualKey) noexcept;
std::wstring KeyName(UINT virtualKey);
std::wstring ConfigPath();
bool IsRunAtStartupEnabled();
bool SetRunAtStartup(bool enabled);

}  // namespace kbun
