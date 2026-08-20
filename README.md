# KBUN

KBUN is a native Windows keyboard navigation utility. Hold the navigation key (Caps Lock by default) to reveal semantic UI Automation targets, type their hint, then use Enter for a left click or Space for a right click. Editable text controls receive a caret at the start; read-only text controls enter a small UI Automation caret-browsing mode.

KBUN does not use screenshots or computer vision. It discovers controls through Microsoft UI Automation and deliberately excludes the Windows taskbar.

## Build

Requirements: Windows 10 or 11 and the Visual Studio 2022 Desktop development with C++ workload.

```powershell
.\build.ps1 -Configuration Release -RunTests
```

The executable is written to `build\KBUN.exe`. CMake is also supported:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Use

1. Start `KBUN.exe`; its icon appears in the notification area.
2. Hold Caps Lock. Major sections with more than three targets receive one-letter hints. Other targets receive two-letter hints. Overlapping targets are folded into an existing section when its total remains below 26; otherwise the section is flattened.
3. Type a section letter to drill in, then a one-letter target. Backspace returns to the previous level.
4. For action controls, press Enter for left click or Space for right click after choosing the target. The selected outline remains visible until Escape or another keyboard input, while the normal mouse pointer stays hidden.
5. Drop-downs open as soon as they are selected and keep the overlay active with one-letter hints for their visible options.
6. In read-only text, use arrows, Home, End, Page Up, and Page Down. Hold Left Shift while moving to select strictly within that text field. Ctrl+C copies the selection and Escape leaves caret mode.

Escape also cancels a selected action before it is clicked. The tray tooltip reports the most recent semantic target count and scan time.

Open Settings from the tray menu to configure key bindings, Windows startup, hint font and badge scale, dark or light appearance, the rainbow preset or a custom gradient of up to 15 colors, label text color, and adaptive high contrast. Click the navigation-key field and press any non-letter keyboard key to bind it; A-Z and Escape remain reserved, and Caps Lock is the fresh-install default. KBUN intercepts the configured key while it is held so its ordinary system action does not fire. Optional quick filters reserve `A` for buttons and hyperlinks and `D` for editable or read-only text immediately after the overlay opens.

Automatic click mode double-clicks a button as soon as its hint is entered. Hold Shift while entering the hint for a right click, or Alt for a single left click. Scroll bars are never assigned hints.

KBUN scans all visible windows on the virtual desktop. The full-desktop path is retained because it remains responsive and preserves direct cross-window navigation.

## Accessibility boundary

UI Automation is the Windows semantic accessibility API. KBUN can only expose controls that an application publishes through that API. Custom-drawn controls with no accessibility provider cannot be discovered reliably without computer vision. Likewise, Windows prevents a normal process from controlling elevated applications; run KBUN at the same integrity level as the target when that is required.
