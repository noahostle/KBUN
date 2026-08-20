#include "app.h"

#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <memory>
#include <string>
#include <utility>

namespace kbun {
namespace {

constexpr wchar_t kAppWindowClass[] = L"KBUN.App";
constexpr wchar_t kSettingsWindowClass[] = L"KBUN.Settings";
constexpr wchar_t kSingletonName[] = L"Local\\KBUN.Singleton";
constexpr UINT_PTR kDropdownRefreshTimer = 2;
constexpr COLORREF kSettingsAccent = RGB(0, 122, 255);
constexpr int kSettingsClientWidth = 650;
constexpr int kSettingsClientHeight = 440;

enum CommandId : UINT {
    CommandSettings = 100,
    CommandEnabled,
    CommandStartup,
    CommandQuickFilters,
    CommandAutomaticDoubleClick,
    CommandRefresh,
    CommandAbout,
    CommandExit,
};

enum SettingsControlId : int {
    SettingsEnabled = 200,
    SettingsStartup,
    SettingsQuickFilters,
    SettingsAutomaticDoubleClick,
    SettingsNavigation,
    SettingsLeftClick,
    SettingsRightClick,
    SettingsFadeIn,
    SettingsFadeOut,
    SettingsTheme,
    SettingsGradientMode,
    SettingsAddColor,
    SettingsRemoveColor,
    SettingsLabelColor,
    SettingsHighContrast,
    SettingsLabelScale,
    SettingsLabelScaleValue,
    SettingsGeneralTab,
    SettingsAppearanceTab,
    SettingsSave,
    SettingsCancel,
    SettingsTitle,
    SettingsSubtitle,
    SettingsPaletteBase = 300,
};

struct SettingsColors {
    COLORREF background;
    COLORREF field;
    COLORREF text;
    COLORREF muted;
    COLORREF border;
};

struct SettingsContext {
    HWND appWindow = nullptr;
    Config config;
    bool startup = false;
    bool capturingNavigation = false;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH fieldBrush = nullptr;
    HFONT font = nullptr;
    HFONT titleFont = nullptr;
    HFONT sectionFont = nullptr;
    int page = 0;
    std::vector<HWND> generalControls;
    std::vector<HWND> appearanceControls;
    std::array<COLORREF, 16> colorPickerValues{};
};

struct SettingsUpdate {
    Config config;
    bool startup = false;
};

int ChoiceIndex(const std::vector<KeyChoice>& choices, UINT key) {
    const auto found = std::ranges::find_if(choices, [key](const KeyChoice& choice) {
        return choice.virtualKey == key;
    });
    return found == choices.end() ? 0 : static_cast<int>(std::distance(choices.begin(), found));
}

void SetControlFont(HWND control) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

void SetControlFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND AddControl(
    HWND parent,
    const wchar_t* className,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int id) {
    const UINT dpi = GetDpiForWindow(parent);
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    HWND control = CreateWindowExW(
        0,
        className,
        text,
        WS_CHILD | WS_VISIBLE | style,
        scale(x),
        scale(y),
        scale(width),
        scale(height),
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),
        nullptr);
    if (control) SetControlFont(control);
    return control;
}

void PopulateCombo(HWND combo, const std::vector<KeyChoice>& choices, UINT selected) {
    for (const auto& choice : choices) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label));
    SendMessageW(combo, CB_SETCURSEL, ChoiceIndex(choices, selected), 0);
}

UINT ComboKey(HWND window, int id, const std::vector<KeyChoice>& choices) {
    const LRESULT selected = SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0);
    if (selected < 0 || static_cast<std::size_t>(selected) >= choices.size()) return choices.front().virtualKey;
    return choices[static_cast<std::size_t>(selected)].virtualKey;
}

SettingsColors ColorsFor(UiTheme theme) {
    if (theme == UiTheme::Light) {
        return {RGB(246, 246, 248), RGB(255, 255, 255), RGB(28, 28, 30),
                RGB(103, 103, 110), RGB(214, 214, 220)};
    }
    return {RGB(28, 28, 30), RGB(44, 44, 46), RGB(244, 244, 246),
            RGB(159, 159, 168), RGB(69, 69, 74)};
}

SettingsColors ColorsFor(const SettingsContext& context) {
    return ColorsFor(context.config.theme);
}

struct ComboListLookup {
    HWND list = nullptr;
    HWND combo = nullptr;
    DWORD processId = 0;
};

BOOL CALLBACK MatchComboChild(HWND window, LPARAM parameter) {
    auto& lookup = *reinterpret_cast<ComboListLookup*>(parameter);
    wchar_t className[32]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"ComboBox") != 0) return TRUE;

    COMBOBOXINFO info{};
    info.cbSize = sizeof(info);
    if (GetComboBoxInfo(window, &info) && info.hwndList == lookup.list) {
        lookup.combo = window;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK MatchComboTopLevel(HWND window, LPARAM parameter) {
    auto& lookup = *reinterpret_cast<ComboListLookup*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != lookup.processId) return TRUE;
    if (!MatchComboChild(window, parameter)) return FALSE;
    EnumChildWindows(window, MatchComboChild, parameter);
    return lookup.combo == nullptr;
}

HWND FindComboForList(HWND list) {
    ComboListLookup lookup;
    lookup.list = list;
    GetWindowThreadProcessId(list, &lookup.processId);
    if (lookup.processId != 0) EnumWindows(MatchComboTopLevel, reinterpret_cast<LPARAM>(&lookup));
    return lookup.combo;
}

struct ComboListAtPointLookup {
    POINT point{};
    HWND list = nullptr;
};

BOOL CALLBACK MatchComboListAtPoint(HWND window, LPARAM parameter) {
    auto& lookup = *reinterpret_cast<ComboListAtPointLookup*>(parameter);
    wchar_t className[32]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    RECT bounds{};
    if (_wcsicmp(className, L"ComboLBox") == 0 && IsWindowVisible(window) &&
        GetWindowRect(window, &bounds) && ContainsPoint(bounds, lookup.point)) {
        lookup.list = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindComboListAtPoint(POINT point) {
    ComboListAtPointLookup lookup{point};
    EnumWindows(MatchComboListAtPoint, reinterpret_cast<LPARAM>(&lookup));
    return lookup.list;
}

bool ClickNativeComboOption(const ElementInfo& element) {
    const POINT screenPoint = RectCenter(element.bounds);
    wchar_t className[32]{};
    if (IsWindow(element.ownerWindow)) {
        GetClassNameW(element.ownerWindow, className, static_cast<int>(std::size(className)));
    }

    HWND list = nullptr;
    HWND combo = nullptr;
    if (_wcsicmp(className, L"ComboBox") == 0) {
        COMBOBOXINFO info{};
        info.cbSize = sizeof(info);
        if (GetComboBoxInfo(element.ownerWindow, &info) && IsWindow(info.hwndList)) {
            combo = element.ownerWindow;
            list = info.hwndList;
        }
    } else if (_wcsicmp(className, L"ComboLBox") == 0) {
        list = element.ownerWindow;
    }
    if (!list) list = FindComboListAtPoint(screenPoint);
    if (!list) return false;
    if (!combo) combo = FindComboForList(list);

    POINT point = screenPoint;
    if (!ScreenToClient(list, &point)) return false;
    RECT client{};
    if (!GetClientRect(list, &client) || !ContainsPoint(client, point)) return false;

    const LPARAM location = MAKELPARAM(point.x, point.y);
    if (!combo) {
        const bool pressed = PostMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, location) != FALSE;
        const bool released = PostMessageW(list, WM_LBUTTONUP, 0, location) != FALSE;
        return pressed && released;
    }

    DWORD_PTR hitResult = 0;
    if (!SendMessageTimeoutW(list, LB_ITEMFROMPOINT, 0, location,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 120, &hitResult) || HIWORD(hitResult) != 0) {
        return false;
    }

    const int item = LOWORD(hitResult);
    HWND parent = GetParent(combo);
    const int controlId = GetDlgCtrlID(combo);
    bool queued = PostMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(item), 0) != FALSE;
    if (parent) {
        queued = PostMessageW(parent, WM_COMMAND,
            MAKEWPARAM(controlId, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo)) != FALSE && queued;
        queued = PostMessageW(parent, WM_COMMAND,
            MAKEWPARAM(controlId, CBN_SELENDOK), reinterpret_cast<LPARAM>(combo)) != FALSE && queued;
    }
    queued = PostMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0) != FALSE && queued;
    if (parent) {
        queued = PostMessageW(parent, WM_COMMAND,
            MAKEWPARAM(controlId, CBN_CLOSEUP), reinterpret_cast<LPARAM>(combo)) != FALSE && queued;
    }
    return queued;
}

void PopulateTextCombo(HWND combo, std::initializer_list<const wchar_t*> labels, int selected) {
    for (const wchar_t* label : labels) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    }
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

void RefreshPaletteControls(HWND window, SettingsContext& context) {
    const bool custom = context.config.gradientMode == GradientMode::Custom;
    for (std::size_t index = 0; index < kMaximumGradientColors; ++index) {
        HWND swatch = GetDlgItem(window, SettingsPaletteBase + static_cast<int>(index));
        const bool visible = context.page == 1 && index < context.config.gradientColors.size();
        ShowWindow(swatch, visible ? SW_SHOW : SW_HIDE);
        EnableWindow(swatch, custom);
        InvalidateRect(swatch, nullptr, TRUE);
    }
    EnableWindow(GetDlgItem(window, SettingsAddColor),
                 custom && context.config.gradientColors.size() < kMaximumGradientColors);
    EnableWindow(GetDlgItem(window, SettingsRemoveColor),
                 custom && context.config.gradientColors.size() > 1);
}

void UpdateSettingsPage(HWND window, SettingsContext& context) {
    for (HWND control : context.generalControls) ShowWindow(control, context.page == 0 ? SW_SHOW : SW_HIDE);
    for (HWND control : context.appearanceControls) ShowWindow(control, context.page == 1 ? SW_SHOW : SW_HIDE);
    RefreshPaletteControls(window, context);
    RedrawWindow(
        window,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void SetNavigationCapture(HWND window, SettingsContext& context, bool active) {
    context.capturingNavigation = active;
    HWND control = GetDlgItem(window, SettingsNavigation);
    if (control) {
        const std::wstring label = active ? L"Press a key" : KeyName(context.config.navigationKey);
        SetWindowTextW(control, label.c_str());
        InvalidateRect(control, nullptr, TRUE);
        if (active) SetFocus(control);
    }
    SendMessageW(context.appWindow, active ? kMsgKeyCaptureBegin : kMsgKeyCaptureEnd, 0, 0);
}

void UpdateLabelScaleValue(HWND window, UINT percent) {
    const std::wstring value = std::to_wstring(percent) + L"%";
    SetWindowTextW(GetDlgItem(window, SettingsLabelScaleValue), value.c_str());
}

void BuildSettingsControls(HWND window, SettingsContext& context) {
    constexpr int labelX = 28;
    constexpr int inputX = 365;
    constexpr int inputWidth = 257;
    constexpr int rowHeight = 30;
    HWND title = AddControl(window, L"STATIC", L"KBUN", SS_LEFT, labelX, 12, 300, 32, SettingsTitle);
    SetControlFont(title, context.titleFont);
    HWND subtitle = AddControl(
        window, L"STATIC", L"Settings", SS_LEFT, labelX, 42, 300, 18, SettingsSubtitle);
    SetControlFont(subtitle, context.font);
    AddControl(window, L"BUTTON", L"General", BS_OWNERDRAW | WS_TABSTOP,
               labelX, 67, 293, 32, SettingsGeneralTab);
    AddControl(window, L"BUTTON", L"Appearance", BS_OWNERDRAW | WS_TABSTOP,
               329, 67, 293, 32, SettingsAppearanceTab);

    auto addGeneral = [&context](HWND control) {
        if (control) context.generalControls.push_back(control);
        return control;
    };
    auto addAppearance = [&context](HWND control) {
        if (control) context.appearanceControls.push_back(control);
        return control;
    };

    HWND heading = addGeneral(AddControl(window, L"STATIC", L"GENERAL", SS_LEFT,
                                         labelX, 115, 240, 18, -1));
    SetControlFont(heading, context.sectionFont);
    addGeneral(AddControl(window, L"BUTTON", L"KBUN enabled", BS_OWNERDRAW | WS_TABSTOP,
                          labelX, 139, 594, 24, SettingsEnabled));
    addGeneral(AddControl(window, L"BUTTON", L"Launch at sign in", BS_OWNERDRAW | WS_TABSTOP,
                          labelX, 168, 594, 24, SettingsStartup));
    addGeneral(AddControl(window, L"BUTTON", L"A / D type filters", BS_OWNERDRAW | WS_TABSTOP,
                          labelX, 197, 594, 24, SettingsQuickFilters));
    addGeneral(AddControl(window, L"BUTTON", L"Automatic double click", BS_OWNERDRAW | WS_TABSTOP,
                          labelX, 226, 594, 24, SettingsAutomaticDoubleClick));

    heading = addGeneral(AddControl(window, L"STATIC", L"KEYBOARD", SS_LEFT,
                                    labelX, 265, 240, 18, -1));
    SetControlFont(heading, context.sectionFont);
    addGeneral(AddControl(window, L"STATIC", L"Navigation key", SS_LEFT,
                          labelX, 293, 180, 22, -1));
    addGeneral(AddControl(
        window, L"BUTTON", KeyName(context.config.navigationKey).c_str(), BS_OWNERDRAW | WS_TABSTOP,
        inputX, 287, inputWidth, 26, SettingsNavigation));
    addGeneral(AddControl(window, L"STATIC", L"Left click key", SS_LEFT,
                          labelX, 293 + rowHeight, 180, 22, -1));
    HWND leftClick = addGeneral(AddControl(
        window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
        inputX, 287 + rowHeight, inputWidth, 180, SettingsLeftClick));
    PopulateCombo(leftClick, ClickKeyChoices(), context.config.leftClickKey);
    addGeneral(AddControl(window, L"STATIC", L"Right click key", SS_LEFT,
                          labelX, 293 + rowHeight * 2, 180, 22, -1));
    HWND rightClick = addGeneral(AddControl(
        window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
        inputX, 287 + rowHeight * 2, inputWidth, 180, SettingsRightClick));
    PopulateCombo(rightClick, ClickKeyChoices(), context.config.rightClickKey);

    addGeneral(AddControl(window, L"STATIC", L"Fade in", SS_LEFT, labelX, 386, 56, 22, -1));
    HWND fadeIn = addGeneral(AddControl(
        window, L"EDIT", std::to_wstring(context.config.fadeInMs).c_str(),
        ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
        86, 380, 64, 26, SettingsFadeIn));
    Edit_LimitText(fadeIn, 3);
    addGeneral(AddControl(window, L"STATIC", L"Fade out", SS_LEFT,
                          174, 386, 64, 22, -1));
    HWND fadeOut = addGeneral(AddControl(
        window, L"EDIT", std::to_wstring(context.config.fadeOutMs).c_str(),
        ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
        242, 380, 64, 26, SettingsFadeOut));
    Edit_LimitText(fadeOut, 3);

    heading = addAppearance(AddControl(window, L"STATIC", L"APPEARANCE", SS_LEFT,
                                       labelX, 115, 240, 18, -1));
    SetControlFont(heading, context.sectionFont);
    addAppearance(AddControl(window, L"STATIC", L"Theme", SS_LEFT, labelX, 143, 180, 22, -1));
    HWND theme = addAppearance(AddControl(
        window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
        inputX, 137, inputWidth, 150, SettingsTheme));
    PopulateTextCombo(theme, {L"Dark", L"Light"}, static_cast<int>(context.config.theme));

    addAppearance(AddControl(window, L"STATIC", L"Gradient", SS_LEFT,
                             labelX, 143 + rowHeight, 180, 22, -1));
    HWND gradient = addAppearance(AddControl(
        window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
        inputX, 137 + rowHeight, inputWidth, 150, SettingsGradientMode));
    PopulateTextCombo(
        gradient,
        {L"Rainbow", L"Custom palette"},
        static_cast<int>(context.config.gradientMode));

    heading = addAppearance(AddControl(window, L"STATIC", L"PALETTE", SS_LEFT,
                                       labelX, 211, 240, 18, -1));
    SetControlFont(heading, context.sectionFont);
    for (std::size_t index = 0; index < kMaximumGradientColors; ++index) {
        const int column = static_cast<int>(index % 15);
        const int row = static_cast<int>(index / 15);
        addAppearance(AddControl(
            window, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP,
            labelX + column * 39, 235 + row * 34, 29, 29,
            SettingsPaletteBase + static_cast<int>(index)));
    }
    addAppearance(AddControl(window, L"BUTTON", L"Add color", BS_OWNERDRAW | WS_TABSTOP,
                             labelX, 274, 104, 30, SettingsAddColor));
    addAppearance(AddControl(window, L"BUTTON", L"Remove last", BS_OWNERDRAW | WS_TABSTOP,
                             142, 274, 118, 30, SettingsRemoveColor));

    heading = addAppearance(AddControl(window, L"STATIC", L"LABELS", SS_LEFT,
                                       labelX, 318, 240, 18, -1));
    SetControlFont(heading, context.sectionFont);
    addAppearance(AddControl(window, L"STATIC", L"Hint size", SS_LEFT,
                             labelX, 345, 180, 22, -1));
    HWND labelScale = addAppearance(AddControl(
        window, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        inputX, 334, 210, 32, SettingsLabelScale));
    SendMessageW(labelScale, TBM_SETRANGE, TRUE, MAKELPARAM(70, 200));
    SendMessageW(labelScale, TBM_SETLINESIZE, 0, 5);
    SendMessageW(labelScale, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(labelScale, TBM_SETPOS, TRUE, context.config.labelScalePercent);
    addAppearance(AddControl(window, L"STATIC", L"", SS_RIGHT,
                             582, 342, 40, 22, SettingsLabelScaleValue));
    UpdateLabelScaleValue(window, context.config.labelScalePercent);
    addAppearance(AddControl(window, L"STATIC", L"Text color", SS_LEFT,
                             labelX, 379, 180, 22, -1));
    addAppearance(AddControl(window, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP,
                             inputX, 372, 48, 29, SettingsLabelColor));
    addAppearance(AddControl(window, L"BUTTON", L"Adaptive high contrast", BS_OWNERDRAW | WS_TABSTOP,
                             labelX, 406, 276, 24, SettingsHighContrast));

    AddControl(
        window, L"BUTTON", L"Save", BS_OWNERDRAW | WS_TABSTOP,
        446, 400, 84, 34, SettingsSave);
    AddControl(
        window, L"BUTTON", L"Cancel", BS_OWNERDRAW | WS_TABSTOP,
        538, 400, 84, 34, SettingsCancel);

    EnumChildWindows(window, [](HWND child, LPARAM parameter) -> BOOL {
        auto* settings = reinterpret_cast<SettingsContext*>(parameter);
        if (GetDlgCtrlID(child) != SettingsTitle) SetControlFont(child, settings->font);
        wchar_t className[32]{};
        GetClassNameW(child, className, static_cast<int>(std::size(className)));
        const bool dark = settings->config.theme == UiTheme::Dark;
        if (dark && (std::wcscmp(className, L"ComboBox") == 0 ||
                     std::wcscmp(className, L"Edit") == 0)) {
            SetWindowTheme(child, L"DarkMode_CFD", nullptr);
        } else {
            SetWindowTheme(child, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    UpdateSettingsPage(window, context);
}

void InitializeSettingsStyle(HWND window, SettingsContext& context) {
    const UINT dpi = GetDpiForWindow(window);
    const SettingsColors colors = ColorsFor(context);
    context.backgroundBrush = CreateSolidBrush(colors.background);
    context.fieldBrush = CreateSolidBrush(colors.field);
    context.font = CreateFontW(
        -MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    context.titleFont = CreateFontW(
        -MulDiv(20, static_cast<int>(dpi), 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");
    context.sectionFont = CreateFontW(
        -MulDiv(8, static_cast<int>(dpi), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");

    const BOOL dark = context.config.theme == UiTheme::Dark;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    const int backdrop = 2;
    DwmSetWindowAttribute(window, 38, &backdrop, sizeof(backdrop));
    SetWindowTheme(window, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

void RefreshSettingsTheme(HWND window, SettingsContext& context) {
    if (context.backgroundBrush) DeleteObject(context.backgroundBrush);
    if (context.fieldBrush) DeleteObject(context.fieldBrush);
    const SettingsColors colors = ColorsFor(context);
    context.backgroundBrush = CreateSolidBrush(colors.background);
    context.fieldBrush = CreateSolidBrush(colors.field);
    const BOOL dark = context.config.theme == UiTheme::Dark;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    EnumChildWindows(window, [](HWND child, LPARAM parameter) -> BOOL {
        const bool darkTheme = static_cast<UiTheme>(parameter) == UiTheme::Dark;
        wchar_t className[32]{};
        GetClassNameW(child, className, static_cast<int>(std::size(className)));
        SetWindowTheme(
            child,
            darkTheme && (std::wcscmp(className, L"ComboBox") == 0 ||
                          std::wcscmp(className, L"Edit") == 0)
                ? L"DarkMode_CFD"
                : darkTheme ? L"DarkMode_Explorer" : L"Explorer",
            nullptr);
        return TRUE;
    }, static_cast<LPARAM>(context.config.theme));
    InvalidateRect(window, nullptr, TRUE);
}

void ReleaseSettingsStyle(SettingsContext& context) {
    if (context.backgroundBrush) DeleteObject(context.backgroundBrush);
    if (context.fieldBrush) DeleteObject(context.fieldBrush);
    if (context.font) DeleteObject(context.font);
    if (context.titleFont) DeleteObject(context.titleFont);
    if (context.sectionFont) DeleteObject(context.sectionFont);
    context.backgroundBrush = nullptr;
    context.fieldBrush = nullptr;
    context.font = nullptr;
    context.titleFont = nullptr;
    context.sectionFont = nullptr;
}

COLORREF SettingsGradientColor(const Config& config, double position) {
    constexpr std::array<COLORREF, 7> rainbow{
        RGB(255, 76, 127), RGB(255, 149, 64), RGB(250, 210, 67), RGB(52, 199, 89),
        RGB(50, 173, 230), RGB(88, 86, 214), RGB(191, 90, 242),
    };
    const COLORREF* data = config.gradientMode == GradientMode::Rainbow || config.gradientColors.empty()
        ? rainbow.data()
        : config.gradientColors.data();
    const std::size_t count = config.gradientMode == GradientMode::Rainbow || config.gradientColors.empty()
        ? rainbow.size()
        : config.gradientColors.size();
    if (count == 1) return data[0];
    const double scaled = std::clamp(position, 0.0, 1.0) * static_cast<double>(count - 1);
    const std::size_t index = std::min(count - 2, static_cast<std::size_t>(scaled));
    const double blend = scaled - static_cast<double>(index);
    const auto channel = [blend](BYTE from, BYTE to) {
        return static_cast<BYTE>(std::lround(from + (to - from) * blend));
    };
    return RGB(
        channel(GetRValue(data[index]), GetRValue(data[index + 1])),
        channel(GetGValue(data[index]), GetGValue(data[index + 1])),
        channel(GetBValue(data[index]), GetBValue(data[index + 1])));
}

void PaintSettingsBackground(HWND window, SettingsContext& context) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, context.backgroundBrush);

    constexpr int segmentCount = 56;
    const int segmentWidth = std::max(1, RectWidth(client) / segmentCount);
    const int stripHeight = std::max(3, MulDiv(3, static_cast<int>(GetDpiForWindow(window)), 96));
    for (int index = 0; index < segmentCount; ++index) {
        const COLORREF color = SettingsGradientColor(
            context.config,
            static_cast<double>(index) / (segmentCount - 1));
        RECT segment{
            static_cast<LONG>(index * segmentWidth),
            0,
            index + 1 == segmentCount ? client.right : static_cast<LONG>((index + 1) * segmentWidth),
            stripHeight,
        };
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &segment, brush);
        DeleteObject(brush);
    }
    EndPaint(window, &paint);
}

void DrawSettingsToggle(const DRAWITEMSTRUCT& item) {
    RECT bounds = item.rcItem;
    const auto* context = reinterpret_cast<SettingsContext*>(
        GetWindowLongPtrW(GetParent(item.hwndItem), GWLP_USERDATA));
    bool checked = false;
    if (context) {
        switch (item.CtlID) {
            case SettingsEnabled: checked = context->config.enabled; break;
            case SettingsStartup: checked = context->startup; break;
            case SettingsQuickFilters: checked = context->config.quickTypeFilters; break;
            case SettingsAutomaticDoubleClick: checked = context->config.automaticDoubleClick; break;
            case SettingsHighContrast: checked = context->config.highContrastLabels; break;
            default: break;
        }
    }
    const SettingsColors colors = context ? ColorsFor(*context) : ColorsFor(UiTheme::Dark);
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const int dpi = GetDeviceCaps(item.hDC, LOGPIXELSY);
    const int trackWidth = MulDiv(34, dpi, 96);
    const int trackHeight = MulDiv(18, dpi, 96);
    HBRUSH background = CreateSolidBrush(colors.background);
    FillRect(item.hDC, &bounds, background);
    DeleteObject(background);
    RECT track{
        bounds.left + 1,
        bounds.top + (RectHeight(bounds) - trackHeight) / 2,
        bounds.left + 1 + trackWidth,
        bounds.top + (RectHeight(bounds) - trackHeight) / 2 + trackHeight,
    };

    const COLORREF trackColor = checked
        ? (pressed ? RGB(0, 96, 204) : kSettingsAccent)
        : (pressed ? colors.border : colors.field);
    HBRUSH trackBrush = CreateSolidBrush(trackColor);
    HPEN trackPen = CreatePen(PS_SOLID, 1, checked ? kSettingsAccent : colors.border);
    HGDIOBJ previousBrush = SelectObject(item.hDC, trackBrush);
    HGDIOBJ previousPen = SelectObject(item.hDC, trackPen);
    RoundRect(item.hDC, track.left, track.top, track.right, track.bottom, trackHeight, trackHeight);
    SelectObject(item.hDC, previousPen);
    SelectObject(item.hDC, previousBrush);
    DeleteObject(trackPen);
    DeleteObject(trackBrush);

    const int knobSize = trackHeight - MulDiv(4, dpi, 96);
    const int knobInset = MulDiv(2, dpi, 96);
    const int knobLeft = checked ? track.right - knobInset - knobSize : track.left + knobInset;
    RECT knob{
        knobLeft,
        track.top + (trackHeight - knobSize) / 2,
        knobLeft + knobSize,
        track.top + (trackHeight - knobSize) / 2 + knobSize,
    };
    HBRUSH knobBrush = CreateSolidBrush(checked ? RGB(255, 255, 255) : colors.muted);
    previousBrush = SelectObject(item.hDC, knobBrush);
    previousPen = SelectObject(item.hDC, GetStockObject(NULL_PEN));
    Ellipse(item.hDC, knob.left, knob.top, knob.right, knob.bottom);
    SelectObject(item.hDC, previousPen);
    SelectObject(item.hDC, previousBrush);
    DeleteObject(knobBrush);

    RECT textBounds = bounds;
    textBounds.left = track.right + MulDiv(10, dpi, 96);
    wchar_t label[64]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, colors.text);
    HGDIOBJ previousFont = SelectObject(
        item.hDC,
        reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0)));
    DrawTextW(item.hDC, label, -1, &textBounds,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, previousFont);

    if ((item.itemState & ODS_FOCUS) != 0) {
        RECT focus = textBounds;
        DrawFocusRect(item.hDC, &focus);
    }
}

void DrawSettingsButton(const DRAWITEMSTRUCT& item) {
    RECT bounds = item.rcItem;
    const auto* context = reinterpret_cast<SettingsContext*>(
        GetWindowLongPtrW(GetParent(item.hwndItem), GWLP_USERDATA));
    const SettingsColors colors = context ? ColorsFor(*context) : ColorsFor(UiTheme::Dark);
    const bool save = item.CtlID == SettingsSave;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const COLORREF fill = save
        ? (pressed ? RGB(0, 96, 204) : kSettingsAccent)
        : (pressed ? colors.border : colors.field);
    const COLORREF text = disabled ? colors.muted : save ? RGB(255, 255, 255) : colors.text;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, save ? kSettingsAccent : colors.border);
    HGDIOBJ previousBrush = SelectObject(item.hDC, brush);
    HGDIOBJ previousPen = SelectObject(item.hDC, pen);
    const int radius = MulDiv(9, GetDeviceCaps(item.hDC, LOGPIXELSY), 96);
    RoundRect(item.hDC, bounds.left, bounds.top, bounds.right, bounds.bottom, radius * 2, radius * 2);
    SelectObject(item.hDC, previousPen);
    SelectObject(item.hDC, previousBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    wchar_t label[64]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, text);
    HGDIOBJ previousFont = SelectObject(item.hDC, reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0)));
    DrawTextW(item.hDC, label, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, previousFont);

    if ((item.itemState & ODS_FOCUS) != 0) {
        InflateRect(&bounds, -3, -3);
        DrawFocusRect(item.hDC, &bounds);
    }
}

void DrawSettingsTab(const DRAWITEMSTRUCT& item) {
    RECT bounds = item.rcItem;
    const auto* context = reinterpret_cast<SettingsContext*>(
        GetWindowLongPtrW(GetParent(item.hwndItem), GWLP_USERDATA));
    if (!context) return;
    const SettingsColors colors = ColorsFor(*context);
    const bool active = (item.CtlID == SettingsGeneralTab && context->page == 0) ||
                        (item.CtlID == SettingsAppearanceTab && context->page == 1);
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const COLORREF fill = active ? (pressed ? RGB(0, 96, 204) : kSettingsAccent) : colors.field;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, active ? kSettingsAccent : colors.border);
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    const int radius = MulDiv(9, GetDeviceCaps(item.hDC, LOGPIXELSY), 96);
    RoundRect(item.hDC, bounds.left, bounds.top, bounds.right, bounds.bottom, radius * 2, radius * 2);
    SelectObject(item.hDC, oldPen);
    SelectObject(item.hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    wchar_t label[32]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, active ? RGB(255, 255, 255) : colors.text);
    HGDIOBJ oldFont = SelectObject(
        item.hDC,
        reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0)));
    DrawTextW(item.hDC, label, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, oldFont);
}

void DrawSettingsSwatch(const DRAWITEMSTRUCT& item) {
    RECT bounds = item.rcItem;
    const auto* context = reinterpret_cast<SettingsContext*>(
        GetWindowLongPtrW(GetParent(item.hwndItem), GWLP_USERDATA));
    if (!context) return;
    const SettingsColors colors = ColorsFor(*context);
    COLORREF color = context->config.labelTextColor;
    if (item.CtlID >= SettingsPaletteBase &&
        item.CtlID < SettingsPaletteBase + static_cast<int>(kMaximumGradientColors)) {
        const std::size_t index = static_cast<std::size_t>(item.CtlID - SettingsPaletteBase);
        if (index < context->config.gradientColors.size()) color = context->config.gradientColors[index];
    }

    HBRUSH background = CreateSolidBrush(colors.background);
    FillRect(item.hDC, &bounds, background);
    DeleteObject(background);
    InflateRect(&bounds, -2, -2);
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, colors.border);
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    const int radius = MulDiv(7, GetDeviceCaps(item.hDC, LOGPIXELSY), 96);
    RoundRect(item.hDC, bounds.left, bounds.top, bounds.right, bounds.bottom, radius * 2, radius * 2);
    SelectObject(item.hDC, oldPen);
    SelectObject(item.hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
    if ((item.itemState & ODS_FOCUS) != 0) DrawFocusRect(item.hDC, &bounds);
}

bool ReadSettings(HWND window, SettingsContext& context) {
    Config updated = context.config;
    updated.leftClickKey = ComboKey(window, SettingsLeftClick, ClickKeyChoices());
    updated.rightClickKey = ComboKey(window, SettingsRightClick, ClickKeyChoices());
    updated.labelScalePercent = std::clamp<UINT>(
        static_cast<UINT>(SendDlgItemMessageW(window, SettingsLabelScale, TBM_GETPOS, 0, 0)),
        70U,
        200U);
    BOOL validFadeIn = FALSE;
    BOOL validFadeOut = FALSE;
    const UINT fadeIn = GetDlgItemInt(window, SettingsFadeIn, &validFadeIn, FALSE);
    const UINT fadeOut = GetDlgItemInt(window, SettingsFadeOut, &validFadeOut, FALSE);
    updated.fadeInMs = std::clamp(validFadeIn ? fadeIn : 90U, 0U, 500U);
    updated.fadeOutMs = std::clamp(validFadeOut ? fadeOut : 75U, 0U, 500U);
    const LRESULT theme = SendDlgItemMessageW(window, SettingsTheme, CB_GETCURSEL, 0, 0);
    const LRESULT gradient = SendDlgItemMessageW(window, SettingsGradientMode, CB_GETCURSEL, 0, 0);
    updated.theme = theme == 1 ? UiTheme::Light : UiTheme::Dark;
    updated.gradientMode = gradient == 1 ? GradientMode::Custom : GradientMode::Rainbow;
    if (updated.gradientColors.empty()) updated.gradientColors.push_back(RGB(0, 122, 255));
    if (updated.gradientColors.size() > kMaximumGradientColors) {
        updated.gradientColors.resize(kMaximumGradientColors);
    }

    if (!IsValidNavigationKey(updated.navigationKey)) {
        MessageBoxW(
            window,
            L"Choose another navigation key. A-Z, Escape, and mouse buttons are reserved.",
            L"KBUN",
            MB_OK | MB_ICONWARNING);
        return false;
    }

    if (updated.leftClickKey == updated.rightClickKey ||
        updated.navigationKey == updated.leftClickKey ||
        updated.navigationKey == updated.rightClickKey) {
        MessageBoxW(
            window,
            L"Navigation, left click, and right click need different keys.",
            L"KBUN",
            MB_OK | MB_ICONWARNING);
        return false;
    }
    context.config = updated;
    return true;
}

bool PickSettingsColor(HWND window, SettingsContext& context, COLORREF& color) {
    CHOOSECOLORW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = window;
    picker.rgbResult = color;
    picker.lpCustColors = context.colorPickerValues.data();
    picker.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&picker)) return false;
    color = picker.rgbResult;
    return true;
}

LRESULT CALLBACK SettingsWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* context = reinterpret_cast<SettingsContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        context = static_cast<SettingsContext*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }

    switch (message) {
        case kMsgKeyCaptureInput: {
            const UINT key = static_cast<UINT>(wParam);
            if (!context || !context->capturingNavigation) return 0;
            if (!IsValidNavigationKey(key)) {
                MessageBeep(MB_ICONWARNING);
                SetWindowTextW(
                    GetDlgItem(window, SettingsNavigation),
                    key >= 'A' && key <= 'Z' ? L"A-Z reserved" : L"Reserved key");
                InvalidateRect(GetDlgItem(window, SettingsNavigation), nullptr, TRUE);
                return 0;
            }
            context->config.navigationKey = key;
            SetNavigationCapture(window, *context, false);
            return 0;
        }
        case WM_CREATE:
            InitializeSettingsStyle(window, *context);
            BuildSettingsControls(window, *context);
            return 0;
        case WM_PAINT:
            PaintSettingsBackground(window, *context);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_ACTIVATE:
            if (context && LOWORD(wParam) == WA_INACTIVE && context->capturingNavigation) {
                SetNavigationCapture(window, *context, false);
            }
            break;
        case WM_HSCROLL:
            if (context && reinterpret_cast<HWND>(lParam) == GetDlgItem(window, SettingsLabelScale)) {
                context->config.labelScalePercent = std::clamp<UINT>(
                    static_cast<UINT>(SendDlgItemMessageW(window, SettingsLabelScale, TBM_GETPOS, 0, 0)),
                    70U,
                    200U);
                UpdateLabelScaleValue(window, context->config.labelScalePercent);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            const SettingsColors colors = ColorsFor(*context);
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            SetTextColor(dc, id == SettingsTitle ? colors.text : colors.muted);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            const SettingsColors colors = ColorsFor(*context);
            SetTextColor(dc, colors.text);
            SetBkColor(dc, colors.field);
            return reinterpret_cast<LRESULT>(context->fieldBrush);
        }
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ColorsFor(*context).text);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(context->backgroundBrush);
        }
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (wParam == SettingsSave || wParam == SettingsCancel ||
                wParam == SettingsAddColor || wParam == SettingsRemoveColor ||
                wParam == SettingsNavigation) {
                DrawSettingsButton(item);
                return TRUE;
            }
            if (wParam == SettingsGeneralTab || wParam == SettingsAppearanceTab) {
                DrawSettingsTab(item);
                return TRUE;
            }
            if (wParam == SettingsEnabled || wParam == SettingsStartup ||
                wParam == SettingsQuickFilters || wParam == SettingsAutomaticDoubleClick ||
                wParam == SettingsHighContrast) {
                DrawSettingsToggle(item);
                return TRUE;
            }
            if (wParam == SettingsLabelColor ||
                (wParam >= SettingsPaletteBase &&
                 wParam < SettingsPaletteBase + kMaximumGradientColors)) {
                DrawSettingsSwatch(item);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == SettingsNavigation && HIWORD(wParam) == BN_CLICKED) {
                SetNavigationCapture(window, *context, !context->capturingNavigation);
                return 0;
            }
            if ((LOWORD(wParam) == SettingsEnabled || LOWORD(wParam) == SettingsStartup ||
                 LOWORD(wParam) == SettingsQuickFilters ||
                 LOWORD(wParam) == SettingsAutomaticDoubleClick ||
                 LOWORD(wParam) == SettingsHighContrast) &&
                HIWORD(wParam) == BN_CLICKED) {
                HWND toggle = reinterpret_cast<HWND>(lParam);
                switch (LOWORD(wParam)) {
                    case SettingsEnabled: context->config.enabled = !context->config.enabled; break;
                    case SettingsStartup: context->startup = !context->startup; break;
                    case SettingsQuickFilters:
                        context->config.quickTypeFilters = !context->config.quickTypeFilters;
                        break;
                    case SettingsAutomaticDoubleClick:
                        context->config.automaticDoubleClick = !context->config.automaticDoubleClick;
                        break;
                    case SettingsHighContrast:
                        context->config.highContrastLabels = !context->config.highContrastLabels;
                        break;
                    default: break;
                }
                InvalidateRect(toggle, nullptr, TRUE);
                return 0;
            }
            if ((LOWORD(wParam) == SettingsGeneralTab || LOWORD(wParam) == SettingsAppearanceTab) &&
                HIWORD(wParam) == BN_CLICKED) {
                context->page = LOWORD(wParam) == SettingsGeneralTab ? 0 : 1;
                UpdateSettingsPage(window, *context);
                return 0;
            }
            if (LOWORD(wParam) == SettingsTheme && HIWORD(wParam) == CBN_SELCHANGE) {
                context->config.theme = SendDlgItemMessageW(window, SettingsTheme, CB_GETCURSEL, 0, 0) == 1
                    ? UiTheme::Light
                    : UiTheme::Dark;
                RefreshSettingsTheme(window, *context);
                return 0;
            }
            if (LOWORD(wParam) == SettingsGradientMode && HIWORD(wParam) == CBN_SELCHANGE) {
                context->config.gradientMode =
                    SendDlgItemMessageW(window, SettingsGradientMode, CB_GETCURSEL, 0, 0) == 1
                    ? GradientMode::Custom
                    : GradientMode::Rainbow;
                RefreshPaletteControls(window, *context);
                InvalidateRect(window, nullptr, TRUE);
                return 0;
            }
            if (LOWORD(wParam) >= SettingsPaletteBase &&
                LOWORD(wParam) < SettingsPaletteBase + kMaximumGradientColors &&
                HIWORD(wParam) == BN_CLICKED) {
                const std::size_t index = static_cast<std::size_t>(LOWORD(wParam) - SettingsPaletteBase);
                if (index < context->config.gradientColors.size() &&
                    PickSettingsColor(window, *context, context->config.gradientColors[index])) {
                    InvalidateRect(reinterpret_cast<HWND>(lParam), nullptr, TRUE);
                    InvalidateRect(window, nullptr, TRUE);
                }
                return 0;
            }
            if (LOWORD(wParam) == SettingsAddColor && HIWORD(wParam) == BN_CLICKED) {
                if (context->config.gradientColors.size() < kMaximumGradientColors) {
                    COLORREF color = context->config.gradientColors.empty()
                        ? kSettingsAccent
                        : context->config.gradientColors.back();
                    if (PickSettingsColor(window, *context, color)) {
                        context->config.gradientColors.push_back(color);
                        RefreshPaletteControls(window, *context);
                        InvalidateRect(window, nullptr, TRUE);
                    }
                }
                return 0;
            }
            if (LOWORD(wParam) == SettingsRemoveColor && HIWORD(wParam) == BN_CLICKED) {
                if (context->config.gradientColors.size() > 1) {
                    context->config.gradientColors.pop_back();
                    RefreshPaletteControls(window, *context);
                    InvalidateRect(window, nullptr, TRUE);
                }
                return 0;
            }
            if (LOWORD(wParam) == SettingsLabelColor && HIWORD(wParam) == BN_CLICKED) {
                if (PickSettingsColor(window, *context, context->config.labelTextColor)) {
                    InvalidateRect(reinterpret_cast<HWND>(lParam), nullptr, TRUE);
                }
                return 0;
            }
            if (LOWORD(wParam) == SettingsSave) {
                if (!ReadSettings(window, *context)) return 0;
                auto* update = new SettingsUpdate{context->config, context->startup};
                if (!PostMessageW(context->appWindow, kMsgSettingsSaved, 0, reinterpret_cast<LPARAM>(update))) {
                    delete update;
                }
                DestroyWindow(window);
                return 0;
            }
            if (LOWORD(wParam) == SettingsCancel) {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_NCDESTROY:
            if (context) {
                if (context->capturingNavigation) SetNavigationCapture(window, *context, false);
                PostMessageW(context->appWindow, kMsgSettingsSaved, 1, 0);
                ReleaseSettingsStyle(*context);
                delete context;
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HICON CreateTrayIcon(const Config& config) {
    constexpr int size = 32;
    HDC screen = GetDC(nullptr);
    HDC colorDc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HGDIOBJ oldColor = SelectObject(colorDc, color);
    HBRUSH outside = CreateSolidBrush(RGB(0, 0, 0));
    RECT bounds{0, 0, size, size};
    FillRect(colorDc, &bounds, outside);
    DeleteObject(outside);
    const bool light = config.theme == UiTheme::Light;
    const COLORREF backgroundColor = light ? RGB(250, 250, 252) : RGB(28, 28, 30);
    const COLORREF foregroundColor = light ? RGB(30, 30, 34) : RGB(250, 250, 252);
    const COLORREF borderColor = light ? RGB(200, 200, 206) : RGB(76, 76, 82);
    HBRUSH background = CreateSolidBrush(backgroundColor);
    HPEN baseBorder = CreatePen(PS_SOLID, 1, borderColor);
    HGDIOBJ oldPen = SelectObject(colorDc, baseBorder);
    HGDIOBJ oldBrush = SelectObject(colorDc, background);
    RoundRect(colorDc, 3, 3, size - 3, size - 3, 10, 10);
    SelectObject(colorDc, oldBrush);
    SelectObject(colorDc, oldPen);
    DeleteObject(background);
    DeleteObject(baseBorder);

    HPEN accentTop = CreatePen(PS_SOLID, 2, SettingsGradientColor(config, 0.18));
    oldPen = SelectObject(colorDc, accentTop);
    MoveToEx(colorDc, 8, 4, nullptr);
    LineTo(colorDc, 24, 4);
    SelectObject(colorDc, oldPen);
    DeleteObject(accentTop);
    HPEN accentSide = CreatePen(PS_SOLID, 2, SettingsGradientColor(config, 0.78));
    oldPen = SelectObject(colorDc, accentSide);
    MoveToEx(colorDc, 28, 9, nullptr);
    LineTo(colorDc, 28, 23);
    SelectObject(colorDc, oldPen);
    DeleteObject(accentSide);

    LOGBRUSH glyphBrush{BS_SOLID, foregroundColor, 0};
    HPEN glyph = ExtCreatePen(
        PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
        3,
        &glyphBrush,
        0,
        nullptr);
    oldPen = SelectObject(colorDc, glyph);
    MoveToEx(colorDc, 11, 9, nullptr);
    LineTo(colorDc, 11, 23);
    MoveToEx(colorDc, 12, 16, nullptr);
    LineTo(colorDc, 21, 9);
    MoveToEx(colorDc, 13, 16, nullptr);
    LineTo(colorDc, 22, 23);
    SelectObject(colorDc, oldPen);
    DeleteObject(glyph);
    SelectObject(colorDc, oldColor);
    DeleteDC(colorDc);
    ReleaseDC(nullptr, screen);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HDC maskDc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldMask = SelectObject(maskDc, mask);
    PatBlt(maskDc, 0, 0, size, size, WHITENESS);
    HBRUSH maskBrush = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    HPEN maskPen = static_cast<HPEN>(GetStockObject(BLACK_PEN));
    HGDIOBJ previousMaskBrush = SelectObject(maskDc, maskBrush);
    HGDIOBJ previousMaskPen = SelectObject(maskDc, maskPen);
    RoundRect(maskDc, 3, 3, size - 3, size - 3, 10, 10);
    SelectObject(maskDc, previousMaskPen);
    SelectObject(maskDc, previousMaskBrush);
    SelectObject(maskDc, oldMask);
    DeleteDC(maskDc);

    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = color;
    iconInfo.hbmMask = mask;
    HICON icon = CreateIconIndirect(&iconInfo);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

bool IsKeyDown(UINT virtualKey) {
    return (GetAsyncKeyState(static_cast<int>(virtualKey)) & 0x8000) != 0;
}

bool IsAltKey(UINT virtualKey) {
    return virtualKey == VK_MENU || virtualKey == VK_LMENU || virtualKey == VK_RMENU;
}

int AxisGap(LONG firstStart, LONG firstEnd, LONG secondStart, LONG secondEnd) {
    if (firstEnd < secondStart) return secondStart - firstEnd;
    if (secondEnd < firstStart) return firstStart - secondEnd;
    return 0;
}

UINT NormalizeVirtualKey(const KBDLLHOOKSTRUCT& event) {
    if (event.vkCode == VK_MENU) return (event.flags & LLKHF_EXTENDED) ? VK_RMENU : VK_LMENU;
    if (event.vkCode == VK_CONTROL) return (event.flags & LLKHF_EXTENDED) ? VK_RCONTROL : VK_LCONTROL;
    if (event.vkCode == VK_SHIFT) {
        return MapVirtualKeyW(event.scanCode, MAPVK_VSC_TO_VK_EX);
    }
    return event.vkCode;
}

std::optional<CaretAction> CaretActionForKey(UINT virtualKey) {
    switch (virtualKey) {
        case VK_LEFT: return CaretAction::Left;
        case VK_RIGHT: return CaretAction::Right;
        case VK_UP: return CaretAction::Up;
        case VK_DOWN: return CaretAction::Down;
        case VK_HOME: return CaretAction::Home;
        case VK_END: return CaretAction::End;
        case VK_PRIOR: return CaretAction::PageUp;
        case VK_NEXT: return CaretAction::PageDown;
        default: return std::nullopt;
    }
}

}  // namespace

App* App::activeApp_ = nullptr;

App::~App() {
    Shutdown();
}

bool App::Initialize(HINSTANCE instance) {
    instance_ = instance;
    INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&commonControls);
    singletonMutex_ = CreateMutexW(nullptr, FALSE, kSingletonName);
    if (!singletonMutex_ || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singletonMutex_) {
            CloseHandle(singletonMutex_);
            singletonMutex_ = nullptr;
        }
        MessageBoxW(nullptr, L"KBUN is already running in the notification area.", L"KBUN", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    config_ = Config::Load();
    trayIcon_ = CreateTrayIcon(config_);
    if (!CreateMessageWindow()) return false;
    if (!overlay_.Create(instance_)) return false;
    overlay_.SetFadeDurations(config_.fadeInMs, config_.fadeOutMs);
    overlay_.SetAppearance(config_);
    if (!automation_.Start(window_)) return false;
    if (!InstallKeyboardHook()) return false;
    if (!AddTrayIcon()) return false;
    return true;
}

int App::Run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (settingsWindow_ && IsDialogMessageW(settingsWindow_, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

bool App::CreateMessageWindow() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance_;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = kAppWindowClass;
    windowClass.hIcon = trayIcon_;
    windowClass.hIconSm = trayIcon_;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kAppWindowClass,
        L"KBUN",
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance_,
        this);
    if (!window_) return false;
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    return true;
}

bool App::InstallKeyboardHook() {
    activeApp_ = this;
    keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProcedure, instance_, 0);
    if (!keyboardHook_) activeApp_ = nullptr;
    return keyboardHook_ != nullptr;
}

bool App::AddTrayIcon() {
    if (!trayIcon_) trayIcon_ = CreateTrayIcon(config_);
    trayData_ = {};
    trayData_.cbSize = sizeof(trayData_);
    trayData_.hWnd = window_;
    trayData_.uID = kTrayIconId;
    trayData_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    trayData_.uCallbackMessage = kMsgTray;
    trayData_.hIcon = trayIcon_ ? trayIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(trayData_.szTip, L"KBUN keyboard navigation");
    if (!Shell_NotifyIconW(NIM_ADD, &trayData_)) return false;
    trayData_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &trayData_);
    return true;
}

void App::RecreateTrayIcon() {
    HICON replacement = CreateTrayIcon(config_);
    if (!replacement) return;
    HICON previous = trayIcon_;
    trayIcon_ = replacement;
    if (trayData_.hWnd) {
        trayData_.uFlags = NIF_ICON;
        trayData_.hIcon = trayIcon_;
        Shell_NotifyIconW(NIM_MODIFY, &trayData_);
        trayData_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    }
    if (previous) DestroyIcon(previous);
}

void App::RemoveTrayIcon() {
    if (trayData_.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &trayData_);
        trayData_.hWnd = nullptr;
    }
}

void App::ShowTrayMenu(POINT location) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CommandSettings, L"Settings...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (config_.enabled ? MF_CHECKED : 0), CommandEnabled, L"Enabled");
    AppendMenuW(menu, MF_STRING | (IsRunAtStartupEnabled() ? MF_CHECKED : 0), CommandStartup, L"Run when I sign in");
    AppendMenuW(menu, MF_STRING | (config_.quickTypeFilters ? MF_CHECKED : 0),
                CommandQuickFilters, L"A/D type filters");
    AppendMenuW(menu, MF_STRING | (config_.automaticDoubleClick ? MF_CHECKED : 0),
                CommandAutomaticDoubleClick, L"Automatic double click");
    AppendMenuW(menu, MF_STRING, CommandRefresh, L"Refresh targets");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CommandAbout, L"About KBUN");
    AppendMenuW(menu, MF_STRING, CommandExit, L"Exit");

    SetForegroundWindow(window_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, location.x, location.y, 0, window_, nullptr);
    DestroyMenu(menu);
}

void App::ShowSettings() {
    if (settingsWindow_ && IsWindow(settingsWindow_)) {
        ShowWindow(settingsWindow_, SW_SHOWNORMAL);
        SetForegroundWindow(settingsWindow_);
        return;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance_;
    windowClass.lpfnWndProc = SettingsWindowProcedure;
    windowClass.lpszClassName = kSettingsWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = trayIcon_;
    windowClass.hIconSm = trayIcon_;
    windowClass.hbrBackground = nullptr;
    RegisterClassExW(&windowClass);

    auto* context = new SettingsContext{window_, config_, IsRunAtStartupEnabled()};
    const UINT dpi = GetDpiForSystem();
    constexpr DWORD settingsStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    constexpr DWORD settingsExStyle = WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW;
    RECT frame{
        0,
        0,
        MulDiv(kSettingsClientWidth, static_cast<int>(dpi), 96),
        MulDiv(kSettingsClientHeight, static_cast<int>(dpi), 96),
    };
    AdjustWindowRectExForDpi(&frame, settingsStyle, FALSE, settingsExStyle, dpi);
    const int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int width = std::min(RectWidth(frame), virtualWidth);
    const int height = std::min(RectHeight(frame), virtualHeight);
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + (virtualWidth - width) / 2;
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN) + (virtualHeight - height) / 2;
    settingsWindow_ = CreateWindowExW(
        settingsExStyle,
        kSettingsWindowClass,
        L"KBUN Settings",
        settingsStyle,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance_,
        context);
    if (!settingsWindow_) {
        delete context;
        return;
    }
    ShowWindow(settingsWindow_, SW_SHOWNORMAL);
    SetForegroundWindow(settingsWindow_);
}

void App::ShowAbout() {
    const std::wstring text =
        L"KBUN 0.1\n\nHold " + KeyName(config_.navigationKey) +
        L" to navigate Windows using semantic UI Automation targets.\n\n"
        L"No screenshots or computer vision are used.";
    MessageBoxW(settingsWindow_ ? settingsWindow_ : window_, text.c_str(), L"About KBUN", MB_OK | MB_ICONINFORMATION);
}

void App::BeginNavigation() {
    if (!config_.enabled || shuttingDown_) return;
    DismissSelection();
    SetMouseCursorHidden(true);
    targetFilter_ = TargetFilter::All;
    filterShortcutArmed_ = config_.quickTypeFilters;
    dropdownMode_ = false;
    dropdownParent_.reset();
    dropdownRefreshAttempts_ = 0;

    overlayCapturing_.store(true, std::memory_order_release);
    requestedGeneration_ = ++nextGeneration_;
    hints_.Clear();
    overlay_.Begin();

    const HWND foreground = GetForegroundWindow();
    if (cachedScan_ && cachedScan_->foreground == foreground && GetTickCount64() - cachedAt_ <= 1500) {
        ShowHintsForScan(*cachedScan_);
    }
    automation_.RequestScan(requestedGeneration_);
}

void App::EndNavigation() {
    if (dropdownMode_) return;
    if (!overlayCapturing_.exchange(false, std::memory_order_acq_rel)) return;
    automation_.CancelScan(requestedGeneration_);
    overlay_.FadeOut();
    SetMouseCursorHidden(false);
}

void App::CancelNavigation() {
    const bool hadSelection = selectionActive_.load(std::memory_order_acquire);
    dropdownMode_ = false;
    dropdownParent_.reset();
    KillTimer(window_, kDropdownRefreshTimer);
    EndNavigation();
    hints_.Clear();
    if (hadSelection) {
        DismissSelection();
    } else {
        SetMouseCursorHidden(false);
    }
}

void App::RefreshScan() {
    cachedScan_.reset();
    if (!overlayCapturing_.load(std::memory_order_acquire)) return;
    requestedGeneration_ = ++nextGeneration_;
    automation_.RequestScan(requestedGeneration_);
}

void App::HandleHintLetter(wchar_t letter, LPARAM modifiers) {
    if (!overlayCapturing_.load(std::memory_order_acquire)) return;
    letter = static_cast<wchar_t>(std::towupper(letter));
    if (filterShortcutArmed_ && (letter == L'A' || letter == L'D')) {
        targetFilter_ = letter == L'A' ? TargetFilter::Buttons : TargetFilter::Text;
        filterShortcutArmed_ = false;
        if (cachedScan_) ShowHintsForScan(*cachedScan_);
        return;
    }
    filterShortcutArmed_ = false;
    if (hints_.Empty()) return;
    const HintOutcome outcome = hints_.Input(letter);
    if (outcome.type == HintOutcome::Type::Selected) {
        SelectElement(outcome.element, modifiers);
    } else if (outcome.type == HintOutcome::Type::Changed) {
        overlay_.ShowHints(hints_.Display());
    }
}

void App::HandleHintBack() {
    if (!overlayCapturing_.load(std::memory_order_acquire)) return;
    if (hints_.Back()) {
        overlay_.ShowHints(hints_.Display());
    } else {
        CancelNavigation();
    }
}

void App::SelectElement(const ElementInfo& element, LPARAM modifiers) {
    automation_.CancelScan(requestedGeneration_);
    overlay_.HideCaret();
    selectedGeneration_ = hints_.Generation();
    const POINT center = RectCenter(element.bounds);
    SetCursorPos(center.x, center.y);
    SetMouseCursorHidden(true);

    if (element.role == ElementRole::DropDown) {
        selectedElement_ = element;
        dropdownParent_ = element;
        selectionActive_.store(true, std::memory_order_release);
        pointerArmed_.store(false, std::memory_order_release);
        caretMode_.store(false, std::memory_order_release);
        overlay_.ShowSelection(element);
        dropdownMode_ = true;
        dropdownRefreshAttempts_ = 0;
        overlayCapturing_.store(true, std::memory_order_release);
        hints_.Clear();
        pendingActivationId_ = element.id;
        automation_.Activate(selectedGeneration_, element.id);
        SetTimer(window_, kDropdownRefreshTimer, 85, nullptr);
        return;
    }

    if (element.role == ElementRole::Option && dropdownMode_) {
        if (!ClickNativeComboOption(element)) PerformPointerClick(false, 1);
        dropdownMode_ = false;
        overlayCapturing_.store(false, std::memory_order_release);
        KillTimer(window_, kDropdownRefreshTimer);
        hints_.Clear();
        if (dropdownParent_) {
            selectedElement_ = dropdownParent_;
            overlay_.ShowSelection(*dropdownParent_);
        } else {
            selectedElement_ = element;
            overlay_.ShowSelection(element);
        }
        selectionActive_.store(true, std::memory_order_release);
        dropdownParent_.reset();
        return;
    }

    overlayCapturing_.store(false, std::memory_order_release);
    selectedElement_ = element;
    selectionActive_.store(true, std::memory_order_release);
    overlay_.ShowSelection(element);

    pendingActivationId_ = element.id;
    if (IsButtonRole(element.role)) {
        caretMode_.store(false, std::memory_order_release);
        pendingActivationId_ = 0;
        if (config_.automaticDoubleClick) {
            const bool shift = (modifiers & 1) != 0;
            const bool alt = (modifiers & 2) != 0;
            PerformPointerClick(shift, shift || alt ? 1 : 2);
            pointerArmed_.store(false, std::memory_order_release);
        } else {
            pointerArmed_.store(true, std::memory_order_release);
        }
    } else {
        pointerArmed_.store(false, std::memory_order_release);
        automation_.Activate(hints_.Generation(), element.id);
    }
}

void App::HandleScanResult(ScanResult* rawResult) {
    std::unique_ptr<ScanResult> result(rawResult);
    if (!result) return;
    const std::wstring status = L"KBUN - " + std::to_wstring(result->elements.size()) +
        L" targets - " + std::to_wstring(result->elapsedMs) + L" ms";
    SetWindowTextW(window_, status.c_str());
    if (trayData_.hWnd) {
        trayData_.uFlags = NIF_TIP;
        wcsncpy_s(trayData_.szTip, status.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &trayData_);
        trayData_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    }
    cachedAt_ = GetTickCount64();
    cachedScan_ = *result;

    if (!overlayCapturing_.load(std::memory_order_acquire) || result->generation != requestedGeneration_) return;
    ShowHintsForScan(*result);
    if (dropdownMode_ && hints_.Empty()) {
        if (dropdownParent_) overlay_.ShowSelection(*dropdownParent_);
        if (++dropdownRefreshAttempts_ < 3) SetTimer(window_, kDropdownRefreshTimer, 90, nullptr);
    }
}

void App::HandleActivationResult(ActivationResult* rawResult) {
    std::unique_ptr<ActivationResult> result(rawResult);
    if (!result || result->elementId != pendingActivationId_) return;
    pendingActivationId_ = 0;

    if (dropdownMode_ && dropdownParent_ && result->elementId == dropdownParent_->id) {
        if (!result->succeeded) PerformPointerClick(false, 1);
        return;
    }

    if (!result->succeeded) {
        overlay_.HideCaret();
        pointerArmed_.store(true, std::memory_order_release);
        return;
    }
    if (result->mode == ActivationMode::Caret) {
        caretMode_.store(true, std::memory_order_release);
    } else {
        caretMode_.store(false, std::memory_order_release);
        overlay_.HideCaret();
    }
}

void App::HandleCaretMessage(WPARAM wParam, LPARAM lParam) {
    CaretInput input;
    input.action = static_cast<CaretAction>(wParam);
    input.extend = (lParam & 1) != 0;
    input.control = (lParam & 2) != 0;
    if (input.action == CaretAction::Exit) {
        caretMode_.store(false, std::memory_order_release);
        overlay_.HideCaret();
        automation_.SendCaretInput(input);
        DismissSelection(false);
        return;
    }
    automation_.SendCaretInput(input);
}

void App::SendPointerClick(bool rightButton) {
    if (!pointerArmed_.exchange(false, std::memory_order_acq_rel)) return;
    PerformPointerClick(rightButton, 1);
}

void App::PerformPointerClick(bool rightButton, int count) {
    count = std::clamp(count, 1, 2);
    std::array<INPUT, 2> input{};
    input[0].type = INPUT_MOUSE;
    input[1].type = INPUT_MOUSE;
    input[0].mi.dwFlags = rightButton ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    input[1].mi.dwFlags = rightButton ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    for (int click = 0; click < count; ++click) {
        SendInput(static_cast<UINT>(input.size()), input.data(), sizeof(INPUT));
        if (click + 1 < count) Sleep(35);
    }
}

void App::LeaveCaretMode() {
    overlay_.HideCaret();
    if (!caretMode_.exchange(false, std::memory_order_acq_rel)) return;
    automation_.SendCaretInput(CaretInput{CaretAction::Exit});
}

void App::DismissSelection(bool sendCaretExit) {
    pointerArmed_.store(false, std::memory_order_release);
    const bool hadCaret = caretMode_.exchange(false, std::memory_order_acq_rel);
    const bool hadPendingActivation = pendingActivationId_ != 0;
    pendingActivationId_ = 0;
    if (sendCaretExit && (hadCaret || hadPendingActivation)) {
        automation_.SendCaretInput(CaretInput{CaretAction::Exit});
    }
    overlay_.HideCaret();
    selectedElement_.reset();
    selectedGeneration_ = 0;
    selectionActive_.store(false, std::memory_order_release);
    if (!overlayCapturing_.load(std::memory_order_acquire)) overlay_.FadeOut();
    SetMouseCursorHidden(false);
}

void App::SetMouseCursorHidden(bool hidden) {
    if (mouseCursorHidden_ == hidden) return;
    mouseCursorHidden_ = hidden;
    if (hidden) {
        for (int attempt = 0; attempt < 16 && ShowCursor(FALSE) >= 0; ++attempt) {}
    } else {
        for (int attempt = 0; attempt < 16 && ShowCursor(TRUE) < 0; ++attempt) {}
    }
}

std::vector<ElementInfo> App::FilterElements(const std::vector<ElementInfo>& elements) const {
    std::vector<ElementInfo> filtered;
    filtered.reserve(elements.size());
    if (dropdownMode_ && dropdownParent_) {
        std::vector<const ElementInfo*> options;
        for (const ElementInfo& element : elements) {
            if (element.role == ElementRole::Option) options.push_back(&element);
        }
        if (options.empty()) return filtered;

        const POINT parentCenter = RectCenter(dropdownParent_->bounds);
        const auto nearest = *std::ranges::min_element(options, [parentCenter](const auto* left, const auto* right) {
            const POINT a = RectCenter(left->bounds);
            const POINT b = RectCenter(right->bounds);
            return std::abs(a.x - parentCenter.x) + std::abs(a.y - parentCenter.y) <
                   std::abs(b.x - parentCenter.x) + std::abs(b.y - parentCenter.y);
        });
        for (const ElementInfo* option : options) {
            const bool sameSection = nearest->sectionId != 0 && option->sectionId == nearest->sectionId;
            const int horizontalGap = AxisGap(
                option->bounds.left, option->bounds.right, nearest->bounds.left, nearest->bounds.right);
            const int verticalGap = AxisGap(
                option->bounds.top, option->bounds.bottom, nearest->bounds.top, nearest->bounds.bottom);
            if (option->ownerWindow == nearest->ownerWindow &&
                (sameSection || (horizontalGap <= 160 && verticalGap <= 720))) {
                filtered.push_back(*option);
            }
        }
        return filtered;
    }

    for (const ElementInfo& element : elements) {
        if (targetFilter_ == TargetFilter::Buttons && !IsButtonRole(element.role)) continue;
        if (targetFilter_ == TargetFilter::Text && !IsTextRole(element.role)) continue;
        filtered.push_back(element);
    }
    return filtered;
}

void App::ShowHintsForScan(const ScanResult& scan) {
    HintOptions options;
    options.singleLetter = dropdownMode_;
    if (filterShortcutArmed_) options.reservedTopLetters = L"AD";
    hints_.Reset(scan.generation, FilterElements(scan.elements), std::move(options));
    overlay_.ShowHints(hints_.Display());
}

void App::RefreshDropdownOptions() {
    KillTimer(window_, kDropdownRefreshTimer);
    if (!dropdownMode_ || !overlayCapturing_.load(std::memory_order_acquire)) return;
    requestedGeneration_ = ++nextGeneration_;
    automation_.RequestScan(requestedGeneration_);
}

void App::ApplyConfig(Config updated, bool runAtStartup) {
    SetRunAtStartup(runAtStartup);
    config_ = updated;
    config_.Save();
    overlay_.SetFadeDurations(config_.fadeInMs, config_.fadeOutMs);
    overlay_.SetAppearance(config_);
    RecreateTrayIcon();
    if (!config_.enabled) {
        CancelNavigation();
        pointerArmed_.store(false, std::memory_order_release);
        DismissSelection();
    }
}

void App::Shutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    overlayCapturing_.store(false, std::memory_order_release);
    pointerArmed_.store(false, std::memory_order_release);
    caretMode_.store(false, std::memory_order_release);
    selectionActive_.store(false, std::memory_order_release);
    settingsKeyCapture_.store(false, std::memory_order_release);
    settingsCapturedKey_.store(0, std::memory_order_release);
    SetMouseCursorHidden(false);

    RemoveTrayIcon();
    if (keyboardHook_) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    activeApp_ = nullptr;
    automation_.Stop();
    overlay_.Destroy();
    if (settingsWindow_ && IsWindow(settingsWindow_)) DestroyWindow(settingsWindow_);
    settingsWindow_ = nullptr;
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    window_ = nullptr;
    if (trayIcon_) {
        DestroyIcon(trayIcon_);
        trayIcon_ = nullptr;
    }
    if (singletonMutex_) {
        CloseHandle(singletonMutex_);
        singletonMutex_ = nullptr;
    }
}

LRESULT CALLBACK App::WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<App*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == taskbarCreatedMessage_ && taskbarCreatedMessage_ != 0) {
        trayData_.hWnd = nullptr;
        AddTrayIcon();
        return 0;
    }

    switch (message) {
        case kMsgNavDown: BeginNavigation(); return 0;
        case kMsgNavUp: EndNavigation(); return 0;
        case kMsgHintKey: HandleHintLetter(static_cast<wchar_t>(wParam), lParam); return 0;
        case kMsgHintBack: HandleHintBack(); return 0;
        case kMsgHintCancel: CancelNavigation(); return 0;
        case kMsgPointerClick: SendPointerClick(wParam != 0); return 0;
        case kMsgCaretKey: HandleCaretMessage(wParam, lParam); return 0;
        case kMsgScanComplete:
            HandleScanResult(reinterpret_cast<ScanResult*>(lParam));
            return 0;
        case kMsgActivationComplete:
            HandleActivationResult(reinterpret_cast<ActivationResult*>(lParam));
            return 0;
        case kMsgCaretVisual: {
            std::unique_ptr<CaretVisualResult> result(reinterpret_cast<CaretVisualResult*>(lParam));
            const bool currentReadOnlySelection =
                result && selectedElement_ && selectionActive_.load(std::memory_order_acquire) &&
                selectedElement_->role == ElementRole::ReadOnlyText &&
                result->generation == selectedGeneration_ &&
                result->elementId == selectedElement_->id;
            if (currentReadOnlySelection && result->visible) {
                caretMode_.store(true, std::memory_order_release);
                overlay_.ShowCaret(result->bounds);
            } else if (currentReadOnlySelection ||
                       !selectionActive_.load(std::memory_order_acquire)) {
                overlay_.HideCaret();
            }
            return 0;
        }
        case kMsgSelectionCancel:
            if (overlayCapturing_.load(std::memory_order_acquire)) {
                CancelNavigation();
            } else {
                DismissSelection();
            }
            return 0;
        case kMsgSettingsSaved:
            if (wParam == 1) {
                settingsWindow_ = nullptr;
            } else {
                std::unique_ptr<SettingsUpdate> update(reinterpret_cast<SettingsUpdate*>(lParam));
                if (update) ApplyConfig(update->config, update->startup);
            }
            return 0;
        case kMsgKeyCaptureBegin:
            settingsCapturedKey_.store(0, std::memory_order_release);
            settingsKeyCapture_.store(true, std::memory_order_release);
            return 0;
        case kMsgKeyCaptureInput:
            if (settingsWindow_ && IsWindow(settingsWindow_)) {
                PostMessageW(settingsWindow_, kMsgKeyCaptureInput, wParam, lParam);
            }
            return 0;
        case kMsgKeyCaptureEnd:
            settingsKeyCapture_.store(false, std::memory_order_release);
            settingsCapturedKey_.store(0, std::memory_order_release);
            return 0;
        case kMsgTray: {
            const UINT event = LOWORD(lParam);
            if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
                POINT location{};
                GetCursorPos(&location);
                ShowTrayMenu(location);
            } else if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP) {
                ShowSettings();
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case CommandSettings: ShowSettings(); break;
                case CommandEnabled:
                    config_.enabled = !config_.enabled;
                    ApplyConfig(config_, IsRunAtStartupEnabled());
                    break;
                case CommandStartup: SetRunAtStartup(!IsRunAtStartupEnabled()); break;
                case CommandQuickFilters:
                    config_.quickTypeFilters = !config_.quickTypeFilters;
                    ApplyConfig(config_, IsRunAtStartupEnabled());
                    break;
                case CommandAutomaticDoubleClick:
                    config_.automaticDoubleClick = !config_.automaticDoubleClick;
                    ApplyConfig(config_, IsRunAtStartupEnabled());
                    break;
                case CommandRefresh: RefreshScan(); break;
                case CommandAbout: ShowAbout(); break;
                case CommandExit: PostMessageW(window_, WM_CLOSE, 0, 0); break;
                default: break;
            }
            return 0;
        case WM_CLOSE:
            Shutdown();
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            return 0;
        case WM_TIMER:
            if (wParam == kDropdownRefreshTimer) {
                RefreshDropdownOptions();
                return 0;
            }
            return DefWindowProcW(window_, message, wParam, lParam);
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
    }
}

LRESULT CALLBACK App::KeyboardProcedure(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || !activeApp_) return CallNextHookEx(nullptr, code, wParam, lParam);
    const auto* event = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    return activeApp_->HandleKeyboard(wParam, *event);
}

LRESULT App::HandleKeyboard(WPARAM message, const KBDLLHOOKSTRUCT& event) {
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!down && !up) return CallNextHookEx(keyboardHook_, HC_ACTION, message, reinterpret_cast<LPARAM>(&event));

    const UINT key = NormalizeVirtualKey(event);
    if (key >= keyDown_.size()) return CallNextHookEx(keyboardHook_, HC_ACTION, message, reinterpret_cast<LPARAM>(&event));
    const bool wasDown = keyDown_[key];
    keyDown_[key] = down;

    if (settingsKeyCapture_.load(std::memory_order_acquire)) {
        const UINT captured = settingsCapturedKey_.load(std::memory_order_acquire);
        suppressed_[key] = down;
        if (down && !wasDown && captured == 0) {
            settingsCapturedKey_.store(key, std::memory_order_release);
            PostMessageW(window_, kMsgKeyCaptureInput, key, 0);
        } else if (up && key == captured) {
            settingsCapturedKey_.store(0, std::memory_order_release);
        }
        return 1;
    }

    if (navigationHeld_ && key == heldNavigationKey_) {
        if (up) {
            navigationHeld_ = false;
            heldNavigationKey_ = 0;
            PostMessageW(window_, kMsgNavUp, 0, 0);
            suppressed_[key] = false;
        }
        return 1;
    }
    if (config_.enabled && key == config_.navigationKey) {
        if (down && !wasDown) {
            navigationHeld_ = true;
            heldNavigationKey_ = key;
            suppressed_[key] = true;
            PostMessageW(window_, kMsgNavDown, 0, 0);
        }
        if (up) suppressed_[key] = false;
        return 1;
    }

    if (up && suppressed_[key]) {
        suppressed_[key] = false;
        return 1;
    }

    if (overlayCapturing_.load(std::memory_order_acquire)) {
        const bool hintLetter = key >= 'A' && key <= 'Z';
        if (hintLetter || key == VK_BACK || key == VK_ESCAPE) {
            suppressed_[key] = down;
            if (down && !wasDown) {
                if (hintLetter) {
                    const bool shift = IsKeyDown(VK_SHIFT) || IsKeyDown(VK_LSHIFT) || IsKeyDown(VK_RSHIFT);
                    bool alt = IsKeyDown(VK_MENU) || IsKeyDown(VK_LMENU) || IsKeyDown(VK_RMENU);
                    if (IsAltKey(heldNavigationKey_)) {
                        alt = (heldNavigationKey_ != VK_LMENU && IsKeyDown(VK_LMENU)) ||
                              (heldNavigationKey_ != VK_RMENU && IsKeyDown(VK_RMENU));
                    }
                    const LPARAM modifiers = (shift ? 1 : 0) | (alt ? 2 : 0);
                    PostMessageW(window_, kMsgHintKey, key, modifiers);
                } else if (key == VK_BACK) {
                    PostMessageW(window_, kMsgHintBack, 0, 0);
                } else {
                    PostMessageW(window_, kMsgHintCancel, 0, 0);
                }
            }
            return 1;
        }
    }

    if (caretMode_.load(std::memory_order_acquire)) {
        std::optional<CaretAction> action = CaretActionForKey(key);
        const bool control = IsKeyDown(VK_CONTROL) || IsKeyDown(VK_LCONTROL) || IsKeyDown(VK_RCONTROL);
        if (control && key == 'A') action = CaretAction::SelectAll;
        if (control && key == 'C') action = CaretAction::Copy;
        if (key == VK_ESCAPE) action = CaretAction::Exit;
        if (action) {
            suppressed_[key] = down;
            const bool repeatable = key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN ||
                                    key == VK_HOME || key == VK_END || key == VK_PRIOR || key == VK_NEXT;
            if (down && (!wasDown || repeatable)) {
                const bool extend = IsKeyDown(VK_SHIFT) || IsKeyDown(VK_LSHIFT) || IsKeyDown(VK_RSHIFT);
                const LPARAM flags = (extend ? 1 : 0) | (control ? 2 : 0);
                PostMessageW(window_, kMsgCaretKey, static_cast<WPARAM>(*action), flags);
                if (*action == CaretAction::Exit) caretMode_.store(false, std::memory_order_release);
            }
            return 1;
        }
    }

    if (pointerArmed_.load(std::memory_order_acquire) &&
        (key == config_.leftClickKey || key == config_.rightClickKey)) {
        suppressed_[key] = down;
        if (down && !wasDown) {
            PostMessageW(window_, kMsgPointerClick, key == config_.rightClickKey ? 1 : 0, 0);
        }
        return 1;
    }
    if (selectionActive_.load(std::memory_order_acquire) && key == VK_ESCAPE) {
        suppressed_[key] = down;
        if (down && !wasDown) PostMessageW(window_, kMsgSelectionCancel, 0, 0);
        return 1;
    }

    if (selectionActive_.load(std::memory_order_acquire) &&
        ShouldDismissSelectionForKey(key, down, wasDown, (event.flags & LLKHF_INJECTED) != 0)) {
        pointerArmed_.store(false, std::memory_order_release);
        PostMessageW(window_, kMsgSelectionCancel, 0, 0);
    }

    return CallNextHookEx(keyboardHook_, HC_ACTION, message, reinterpret_cast<LPARAM>(&event));
}

}  // namespace kbun
