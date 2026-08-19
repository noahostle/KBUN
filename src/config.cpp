#include "config.h"

#include <shlobj.h>

#include <filesystem>
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

std::wstring KeyName(UINT virtualKey) {
    for (const auto& choice : NavigationKeyChoices()) {
        if (choice.virtualKey == virtualKey) return choice.label;
    }
    for (const auto& choice : ClickKeyChoices()) {
        if (choice.virtualKey == virtualKey) return choice.label;
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

    if (!IsAllowed(config.navigationKey, NavigationKeyChoices())) config.navigationKey = VK_CAPITAL;
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

