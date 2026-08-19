#include "app.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>

namespace kbun {
namespace {

constexpr wchar_t kAppWindowClass[] = L"KBUN.App";
constexpr wchar_t kSettingsWindowClass[] = L"KBUN.Settings";
constexpr wchar_t kSingletonName[] = L"Local\\KBUN.Singleton";

enum CommandId : UINT {
    CommandSettings = 100,
    CommandEnabled,
    CommandStartup,
    CommandRefresh,
    CommandAbout,
    CommandExit,
};

enum SettingsControlId : int {
    SettingsEnabled = 200,
    SettingsNavigation,
    SettingsLeftClick,
    SettingsRightClick,
    SettingsFadeIn,
    SettingsFadeOut,
    SettingsStartup,
    SettingsSave,
    SettingsCancel,
};

struct SettingsContext {
    HWND appWindow = nullptr;
    Config config;
    bool startup = false;
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
    HWND control = CreateWindowExW(
        0,
        className,
        text,
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        width,
        height,
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

void BuildSettingsControls(HWND window, SettingsContext& context) {
    constexpr int labelX = 20;
    constexpr int inputX = 165;
    constexpr int inputWidth = 190;
    constexpr int rowHeight = 30;
    int y = 18;

    HWND enabled = AddControl(
        window, L"BUTTON", L"KBUN enabled", BS_AUTOCHECKBOX | WS_TABSTOP,
        labelX, y, 335, 24, SettingsEnabled);
    Button_SetCheck(enabled, context.config.enabled ? BST_CHECKED : BST_UNCHECKED);
    y += rowHeight + 8;

    AddControl(window, L"STATIC", L"Navigation key", SS_LEFT, labelX, y + 4, 130, 22, -1);
    HWND navigation = AddControl(
        window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
        inputX, y, inputWidth, 220, SettingsNavigation);
    PopulateCombo(navigation, NavigationKeyChoices(), context.config.navigationKey);
    y += rowHeight;

    AddControl(window, L"STATIC", L"Left click key", SS_LEFT, labelX, y + 4, 130, 22, -1);
    HWND leftClick = AddControl(
        window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
        inputX, y, inputWidth, 220, SettingsLeftClick);
    PopulateCombo(leftClick, ClickKeyChoices(), context.config.leftClickKey);
    y += rowHeight;

    AddControl(window, L"STATIC", L"Right click key", SS_LEFT, labelX, y + 4, 130, 22, -1);
    HWND rightClick = AddControl(
        window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
        inputX, y, inputWidth, 220, SettingsRightClick);
    PopulateCombo(rightClick, ClickKeyChoices(), context.config.rightClickKey);
    y += rowHeight;

    AddControl(window, L"STATIC", L"Fade in (ms)", SS_LEFT, labelX, y + 4, 130, 22, -1);
    HWND fadeIn = AddControl(
        window, L"EDIT", std::to_wstring(context.config.fadeInMs).c_str(),
        ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
        inputX, y, 82, 24, SettingsFadeIn);
    Edit_LimitText(fadeIn, 3);
    y += rowHeight;

    AddControl(window, L"STATIC", L"Fade out (ms)", SS_LEFT, labelX, y + 4, 130, 22, -1);
    HWND fadeOut = AddControl(
        window, L"EDIT", std::to_wstring(context.config.fadeOutMs).c_str(),
        ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
        inputX, y, 82, 24, SettingsFadeOut);
    Edit_LimitText(fadeOut, 3);
    y += rowHeight + 5;

    HWND startup = AddControl(
        window, L"BUTTON", L"Run when I sign in", BS_AUTOCHECKBOX | WS_TABSTOP,
        labelX, y, 335, 24, SettingsStartup);
    Button_SetCheck(startup, context.startup ? BST_CHECKED : BST_UNCHECKED);

    AddControl(
        window, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP,
        195, 280, 76, 28, SettingsSave);
    AddControl(
        window, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP,
        279, 280, 76, 28, SettingsCancel);
}

bool ReadSettings(HWND window, SettingsContext& context) {
    Config updated = context.config;
    updated.enabled = Button_GetCheck(GetDlgItem(window, SettingsEnabled)) == BST_CHECKED;
    updated.navigationKey = ComboKey(window, SettingsNavigation, NavigationKeyChoices());
    updated.leftClickKey = ComboKey(window, SettingsLeftClick, ClickKeyChoices());
    updated.rightClickKey = ComboKey(window, SettingsRightClick, ClickKeyChoices());
    BOOL validFadeIn = FALSE;
    BOOL validFadeOut = FALSE;
    const UINT fadeIn = GetDlgItemInt(window, SettingsFadeIn, &validFadeIn, FALSE);
    const UINT fadeOut = GetDlgItemInt(window, SettingsFadeOut, &validFadeOut, FALSE);
    updated.fadeInMs = std::clamp(validFadeIn ? fadeIn : 90U, 0U, 500U);
    updated.fadeOutMs = std::clamp(validFadeOut ? fadeOut : 75U, 0U, 500U);

    if (updated.leftClickKey == updated.rightClickKey) {
        MessageBoxW(window, L"Left click and right click need different keys.", L"KBUN", MB_OK | MB_ICONWARNING);
        return false;
    }
    context.config = updated;
    context.startup = Button_GetCheck(GetDlgItem(window, SettingsStartup)) == BST_CHECKED;
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
        case WM_CREATE:
            BuildSettingsControls(window, *context);
            return 0;
        case WM_COMMAND:
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
                PostMessageW(context->appWindow, kMsgSettingsSaved, 1, 0);
                delete context;
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HICON CreateTrayIcon() {
    constexpr int size = 32;
    HDC screen = GetDC(nullptr);
    HDC colorDc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HGDIOBJ oldColor = SelectObject(colorDc, color);
    HBRUSH background = CreateSolidBrush(RGB(18, 29, 36));
    RECT bounds{0, 0, size, size};
    FillRect(colorDc, &bounds, background);
    DeleteObject(background);
    HPEN border = CreatePen(PS_SOLID, 2, RGB(42, 210, 190));
    HGDIOBJ oldPen = SelectObject(colorDc, border);
    HGDIOBJ oldBrush = SelectObject(colorDc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(colorDc, 1, 1, size - 1, size - 1);
    SelectObject(colorDc, oldBrush);
    SelectObject(colorDc, oldPen);
    DeleteObject(border);
    HFONT font = CreateFontW(-21, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(colorDc, font);
    SetBkMode(colorDc, TRANSPARENT);
    SetTextColor(colorDc, RGB(248, 250, 252));
    DrawTextW(colorDc, L"K", 1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(colorDc, oldFont);
    DeleteObject(font);
    SelectObject(colorDc, oldColor);
    DeleteDC(colorDc);
    ReleaseDC(nullptr, screen);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HDC maskDc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldMask = SelectObject(maskDc, mask);
    PatBlt(maskDc, 0, 0, size, size, BLACKNESS);
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
    if (!CreateMessageWindow()) return false;
    if (!overlay_.Create(instance_)) return false;
    overlay_.SetFadeDurations(config_.fadeInMs, config_.fadeOutMs);
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
    if (!trayIcon_) trayIcon_ = CreateTrayIcon();
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
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&windowClass);

    auto* context = new SettingsContext{window_, config_, IsRunAtStartupEnabled()};
    const int width = 395;
    const int height = 355;
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + (GetSystemMetrics(SM_CXVIRTUALSCREEN) - width) / 2;
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN) + (GetSystemMetrics(SM_CYVIRTUALSCREEN) - height) / 2;
    settingsWindow_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW,
        kSettingsWindowClass,
        L"KBUN Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x,
        y,
        width,
        height,
        window_,
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
    pointerArmed_.store(false, std::memory_order_release);
    LeaveCaretMode();

    overlayCapturing_.store(true, std::memory_order_release);
    requestedGeneration_ = ++nextGeneration_;
    hints_.Clear();
    overlay_.Begin();

    const HWND foreground = GetForegroundWindow();
    if (cachedScan_ && cachedScan_->foreground == foreground && GetTickCount64() - cachedAt_ <= 1500) {
        hints_.Reset(cachedScan_->generation, cachedScan_->elements);
        overlay_.ShowHints(hints_.Display());
    }
    automation_.RequestScan(requestedGeneration_);
}

void App::EndNavigation() {
    if (!overlayCapturing_.exchange(false, std::memory_order_acq_rel)) return;
    automation_.CancelScan(requestedGeneration_);
    overlay_.FadeOut();
}

void App::CancelNavigation() {
    EndNavigation();
    hints_.Clear();
}

void App::RefreshScan() {
    cachedScan_.reset();
    if (!overlayCapturing_.load(std::memory_order_acquire)) return;
    requestedGeneration_ = ++nextGeneration_;
    automation_.RequestScan(requestedGeneration_);
}

void App::HandleHintLetter(wchar_t letter) {
    if (!overlayCapturing_.load(std::memory_order_acquire) || hints_.Empty()) return;
    const HintOutcome outcome = hints_.Input(letter);
    if (outcome.type == HintOutcome::Type::Selected) {
        SelectElement(outcome.element);
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

void App::SelectElement(const ElementInfo& element) {
    overlayCapturing_.store(false, std::memory_order_release);
    automation_.CancelScan(requestedGeneration_);
    overlay_.FadeOut();
    const POINT center = RectCenter(element.bounds);
    SetCursorPos(center.x, center.y);

    pendingActivationId_ = element.id;
    if (element.role == ElementRole::Action) {
        pointerArmed_.store(true, std::memory_order_release);
        caretMode_.store(false, std::memory_order_release);
        pendingActivationId_ = 0;
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
    hints_.Reset(result->generation, std::move(result->elements));
    overlay_.ShowHints(hints_.Display());
}

void App::HandleActivationResult(ActivationResult* rawResult) {
    std::unique_ptr<ActivationResult> result(rawResult);
    if (!result || result->elementId != pendingActivationId_) return;
    pendingActivationId_ = 0;

    if (!result->succeeded) {
        pointerArmed_.store(true, std::memory_order_release);
        return;
    }
    if (result->mode == ActivationMode::Caret) {
        caretMode_.store(true, std::memory_order_release);
    } else {
        caretMode_.store(false, std::memory_order_release);
    }
}

void App::HandleCaretMessage(WPARAM wParam, LPARAM lParam) {
    CaretInput input;
    input.action = static_cast<CaretAction>(wParam);
    input.extend = (lParam & 1) != 0;
    input.control = (lParam & 2) != 0;
    if (input.action == CaretAction::Exit) caretMode_.store(false, std::memory_order_release);
    automation_.SendCaretInput(input);
}

void App::SendPointerClick(bool rightButton) {
    if (!pointerArmed_.exchange(false, std::memory_order_acq_rel)) return;
    std::array<INPUT, 2> input{};
    input[0].type = INPUT_MOUSE;
    input[1].type = INPUT_MOUSE;
    input[0].mi.dwFlags = rightButton ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    input[1].mi.dwFlags = rightButton ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    SendInput(static_cast<UINT>(input.size()), input.data(), sizeof(INPUT));
}

void App::LeaveCaretMode() {
    if (!caretMode_.exchange(false, std::memory_order_acq_rel)) return;
    automation_.SendCaretInput(CaretInput{CaretAction::Exit});
}

void App::ApplyConfig(Config updated, bool runAtStartup) {
    SetRunAtStartup(runAtStartup);
    config_ = updated;
    config_.Save();
    overlay_.SetFadeDurations(config_.fadeInMs, config_.fadeOutMs);
    if (!config_.enabled) {
        CancelNavigation();
        pointerArmed_.store(false, std::memory_order_release);
        LeaveCaretMode();
    }
}

void App::Shutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    overlayCapturing_.store(false, std::memory_order_release);
    pointerArmed_.store(false, std::memory_order_release);
    caretMode_.store(false, std::memory_order_release);

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
        case kMsgHintKey: HandleHintLetter(static_cast<wchar_t>(wParam)); return 0;
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
        case kMsgSettingsSaved:
            if (wParam == 1) {
                settingsWindow_ = nullptr;
            } else {
                std::unique_ptr<SettingsUpdate> update(reinterpret_cast<SettingsUpdate*>(lParam));
                if (update) ApplyConfig(update->config, update->startup);
            }
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
                    PostMessageW(window_, kMsgHintKey, key, 0);
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
    if (pointerArmed_.load(std::memory_order_acquire) && key == VK_ESCAPE) {
        suppressed_[key] = down;
        if (down && !wasDown) pointerArmed_.store(false, std::memory_order_release);
        return 1;
    }

    return CallNextHookEx(keyboardHook_, HC_ACTION, message, reinterpret_cast<LPARAM>(&event));
}

}  // namespace kbun
