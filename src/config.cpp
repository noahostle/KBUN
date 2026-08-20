#include "config.h"

#include <shlobj.h>

#include <filesystem>
#include <sstream>
#include <string_view>

namespace kbun {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::wstring ExecutablePath() {
    std::wstring result(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, result.data(), static_cast<DWORD>(result.size()));
        if (length == 0) return {};
        if (length < result.size() - 1) {
            result.resize(length);
            return result;
        }
        result.resize(result.size() * 2);
    }
}

UINT ReadUnsigned(const wchar_t* section, const wchar_t* key, UINT fallback, const std::wstring& path) {
    return GetPrivateProfileIntW(section, key, static_cast<int>(fallback), path.c_str());
}

bool WriteUnsigned(const wchar_t* section, const wchar_t* key, UINT value, const std::wstring& path) {
    const std::wstring text = std::to_wstring(value);
    return WritePrivateProfileStringW(section, key, text.c_str(), path.c_str()) != FALSE;
}

bool IsAllowed(UINT key, const std::vector<KeyChoice>& choices) {
    return std::ranges::any_of(choices, [key](const KeyChoice& choice) {
        return choice.virtualKey == key;
    });
}

std::vector<COLORREF> ReadPalette(const std::wstring& path) {
    wchar_t value[1024]{};
    GetPrivateProfileStringW(L"overlay", L"gradient_colors", L"", value, std::size(value), path.c_str());
    std::vector<COLORREF> colors;
    std::wstringstream stream(value);
    std::wstring token;
    while (std::getline(stream, token, L',') && colors.size() < kMaximumGradientColors) {
        wchar_t* end = nullptr;
        const unsigned long color = std::wcstoul(token.c_str(), &end, 16);
        if (end != token.c_str() && *end == L'\0' && color <= 0xFFFFFFUL) {
            colors.push_back(static_cast<COLORREF>(color));
        }
    }
    return colors;
}

bool WritePalette(const std::vector<COLORREF>& colors, const std::wstring& path) {
    std::wstringstream stream;
    stream << std::hex << std::uppercase;
    for (std::size_t index = 0; index < colors.size() && index < kMaximumGradientColors; ++index) {
        if (index != 0) stream << L',';
        stream.width(6);
        stream.fill(L'0');
        stream << (colors[index] & 0xFFFFFFUL);
    }
    return WritePrivateProfileStringW(
               L"overlay", L"gradient_colors", stream.str().c_str(), path.c_str()) != FALSE;
}

}  // namespace

const std::vector<KeyChoice>& NavigationKeyChoices() {
    static const std::vector<KeyChoice> choices{
        {VK_CAPITAL, L"Caps Lock"},
        {VK_LMENU, L"Left Alt"},
        {VK_RMENU, L"Right Alt"},
        {VK_SCROLL, L"Scroll Lock"},
        {VK_F12, L"F12"},
    };
    return choices;
}

const std::vector<KeyChoice>& ClickKeyChoices() {
    static const std::vector<KeyChoice> choices{
        {VK_RETURN, L"Enter"},
        {VK_SPACE, L"Space"},
        {'F', L"F"},
        {'J', L"J"},
        {VK_OEM_1, L"Semicolon"},
    };
    return choices;
}

bool IsValidNavigationKey(UINT virtualKey) noexcept {
    if (virtualKey == 0 || virtualKey >= 256 ||
        (virtualKey >= 'A' && virtualKey <= 'Z')) {
        return false;
    }
    switch (virtualKey) {
        case VK_LBUTTON:
        case VK_RBUTTON:
        case VK_MBUTTON:
        case VK_XBUTTON1:
        case VK_XBUTTON2:
        case VK_ESCAPE:
        case VK_PROCESSKEY:
        case VK_PACKET:
            return false;
        default:
            return true;
    }
}

std::wstring KeyName(UINT virtualKey) {
    for (const auto& choice : NavigationKeyChoices()) {
        if (choice.virtualKey == virtualKey) return choice.label;
    }
    for (const auto& choice : ClickKeyChoices()) {
        if (choice.virtualKey == virtualKey) return choice.label;
    }

    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC_EX);
    if (scanCode != 0) {
        LPARAM keyData = static_cast<LPARAM>((scanCode & 0xFFU) << 16U);
        if ((scanCode & 0xFF00U) != 0) keyData |= 1 << 24;
        wchar_t name[64]{};
        if (GetKeyNameTextW(static_cast<LONG>(keyData), name, static_cast<int>(std::size(name))) > 0) {
            return name;
        }
    }
    return L"Key " + std::to_wstring(virtualKey);
}

std::wstring ConfigPath() {
    PWSTR localAppData = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData))) {
        base = localAppData;
        CoTaskMemFree(localAppData);
    } else {
        base = std::filesystem::temp_directory_path();
    }

    base /= L"KBUN";
    std::error_code error;
    std::filesystem::create_directories(base, error);
    return (base / L"config.ini").wstring();
}

Config Config::Load() {
    Config config;
    const std::wstring path = ConfigPath();
    config.enabled = ReadUnsigned(L"general", L"enabled", config.enabled, path) != 0;
    config.navigationKey = ReadUnsigned(L"keys", L"navigation", config.navigationKey, path);
    config.leftClickKey = ReadUnsigned(L"keys", L"left_click", config.leftClickKey, path);
    config.rightClickKey = ReadUnsigned(L"keys", L"right_click", config.rightClickKey, path);
    config.fadeInMs = std::clamp(ReadUnsigned(L"overlay", L"fade_in_ms", config.fadeInMs, path), 0U, 500U);
    config.fadeOutMs = std::clamp(ReadUnsigned(L"overlay", L"fade_out_ms", config.fadeOutMs, path), 0U, 500U);
    config.quickTypeFilters = ReadUnsigned(
        L"navigation", L"quick_type_filters", config.quickTypeFilters, path) != 0;
    config.automaticDoubleClick = ReadUnsigned(
        L"clicks", L"automatic_double_click", config.automaticDoubleClick, path) != 0;
    config.theme = ReadUnsigned(L"appearance", L"theme", 0, path) == 1
        ? UiTheme::Light
        : UiTheme::Dark;
    config.gradientMode = ReadUnsigned(L"overlay", L"gradient_mode", 0, path) == 1
        ? GradientMode::Custom
        : GradientMode::Rainbow;
    config.labelTextColor = static_cast<COLORREF>(
        ReadUnsigned(L"overlay", L"label_text_color", config.labelTextColor, path) & 0xFFFFFFU);
    config.highContrastLabels = ReadUnsigned(
        L"overlay", L"high_contrast_labels", config.highContrastLabels, path) != 0;
    config.labelScalePercent = std::clamp(
        ReadUnsigned(L"overlay", L"label_scale_percent", config.labelScalePercent, path),
        70U,
        200U);
    std::vector<COLORREF> colors = ReadPalette(path);
    if (!colors.empty()) config.gradientColors = std::move(colors);

    if (!IsValidNavigationKey(config.navigationKey)) config.navigationKey = VK_CAPITAL;
    if (!IsAllowed(config.leftClickKey, ClickKeyChoices())) config.leftClickKey = VK_RETURN;
    if (!IsAllowed(config.rightClickKey, ClickKeyChoices())) config.rightClickKey = VK_SPACE;
    if (config.leftClickKey == config.rightClickKey) config.rightClickKey = VK_SPACE;
    return config;
}

bool Config::Save() const {
    const std::wstring path = ConfigPath();
    bool ok = true;
    ok &= WriteUnsigned(L"general", L"enabled", enabled ? 1U : 0U, path);
    ok &= WriteUnsigned(L"keys", L"navigation", navigationKey, path);
    ok &= WriteUnsigned(L"keys", L"left_click", leftClickKey, path);
    ok &= WriteUnsigned(L"keys", L"right_click", rightClickKey, path);
    ok &= WriteUnsigned(L"overlay", L"fade_in_ms", fadeInMs, path);
    ok &= WriteUnsigned(L"overlay", L"fade_out_ms", fadeOutMs, path);
    ok &= WriteUnsigned(L"navigation", L"quick_type_filters", quickTypeFilters ? 1U : 0U, path);
    ok &= WriteUnsigned(L"clicks", L"automatic_double_click", automaticDoubleClick ? 1U : 0U, path);
    ok &= WriteUnsigned(L"appearance", L"theme", static_cast<UINT>(theme), path);
    ok &= WriteUnsigned(L"overlay", L"gradient_mode", static_cast<UINT>(gradientMode), path);
    ok &= WriteUnsigned(L"overlay", L"label_text_color", labelTextColor & 0xFFFFFFU, path);
    ok &= WriteUnsigned(L"overlay", L"high_contrast_labels", highContrastLabels ? 1U : 0U, path);
    ok &= WriteUnsigned(
        L"overlay", L"label_scale_percent", std::clamp(labelScalePercent, 70U, 200U), path);
    ok &= WritePalette(gradientColors, path);
    return ok;
}

bool IsRunAtStartupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;

    wchar_t value[4096]{};
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    const LSTATUS status = RegQueryValueExW(
        key, L"KBUN", nullptr, &type, reinterpret_cast<BYTE*>(value), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) return false;

    const std::wstring expected = L"\"" + ExecutablePath() + L"\"";
    return std::wstring_view(value) == expected;
}

bool SetRunAtStartup(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }

    LSTATUS status = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = L"\"" + ExecutablePath() + L"\"";
        status = RegSetValueExW(
            key,
            L"KBUN",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, L"KBUN");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

}  // namespace kbun
