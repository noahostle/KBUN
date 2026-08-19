#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace kbun {

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

    static Config Load();
    bool Save() const;
};

const std::vector<KeyChoice>& NavigationKeyChoices();
const std::vector<KeyChoice>& ClickKeyChoices();
std::wstring KeyName(UINT virtualKey);
std::wstring ConfigPath();
bool IsRunAtStartupEnabled();
bool SetRunAtStartup(bool enabled);

}  // namespace kbun

