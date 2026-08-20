#pragma once

#include "automation.h"
#include "config.h"
#include "hints.h"
#include "overlay.h"

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

namespace kbun {

class App {
public:
    App() = default;
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool Initialize(HINSTANCE instance);
    int Run();

private:
    enum class TargetFilter : std::uint8_t {
        All,
        Buttons,
        Text,
    };

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardProcedure(int code, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleKeyboard(WPARAM message, const KBDLLHOOKSTRUCT& event);

    bool CreateMessageWindow();
    bool InstallKeyboardHook();
    bool AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT location);
    void ShowSettings();
    void ShowAbout();
    void Shutdown();

    void BeginNavigation();
    void EndNavigation();
    void CancelNavigation();
    void RefreshScan();
    void HandleHintLetter(wchar_t letter, LPARAM modifiers);
    void HandleHintBack();
    void SelectElement(const ElementInfo& element, LPARAM modifiers = 0);
    void HandleScanResult(ScanResult* result);
    void HandleActivationResult(ActivationResult* result);
    void HandleCaretMessage(WPARAM wParam, LPARAM lParam);
    void SendPointerClick(bool rightButton);
    void PerformPointerClick(bool rightButton, int count);
    void LeaveCaretMode();
    void DismissSelection(bool sendCaretExit = true);
    void SetMouseCursorHidden(bool hidden);
    void ShowHintsForScan(const ScanResult& scan);
    std::vector<ElementInfo> FilterElements(const std::vector<ElementInfo>& elements) const;
    void RefreshDropdownOptions();
    void RecreateTrayIcon();
    void ApplyConfig(Config updated, bool runAtStartup);

    static App* activeApp_;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND settingsWindow_ = nullptr;
    HHOOK keyboardHook_ = nullptr;
    HANDLE singletonMutex_ = nullptr;
    HICON trayIcon_ = nullptr;
    NOTIFYICONDATAW trayData_{};
    UINT taskbarCreatedMessage_ = 0;

    Config config_;
    OverlayWindow overlay_;
    AutomationWorker automation_;
    HintNavigator hints_;

    std::optional<ScanResult> cachedScan_;
    ULONGLONG cachedAt_ = 0;
    std::uint64_t nextGeneration_ = 0;
    std::uint64_t requestedGeneration_ = 0;
    std::uint64_t pendingActivationId_ = 0;
    std::uint64_t selectedGeneration_ = 0;
    std::optional<ElementInfo> selectedElement_;
    std::optional<ElementInfo> dropdownParent_;

    std::array<bool, 256> keyDown_{};
    std::array<bool, 256> suppressed_{};
    UINT heldNavigationKey_ = 0;
    bool navigationHeld_ = false;
    std::atomic_bool overlayCapturing_{false};
    std::atomic_bool pointerArmed_{false};
    std::atomic_bool caretMode_{false};
    std::atomic_bool selectionActive_{false};
    std::atomic_bool settingsKeyCapture_{false};
    std::atomic<UINT> settingsCapturedKey_{0};
    TargetFilter targetFilter_ = TargetFilter::All;
    bool filterShortcutArmed_ = false;
    bool dropdownMode_ = false;
    int dropdownRefreshAttempts_ = 0;
    bool mouseCursorHidden_ = false;
    bool shuttingDown_ = false;
};

}  // namespace kbun
