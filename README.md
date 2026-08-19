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
2. Hold Caps Lock. Major sections with more than three targets receive one-letter hints. Other targets receive two-letter hints.
3. Type a section letter to drill in, then a one-letter target. Backspace returns to the previous level.
4. For action controls, press Enter for left click or Space for right click after choosing the target.
5. In read-only text, use arrows, Home, End, Page Up, Page Down, and Shift to select. Ctrl+C copies the selected UI Automation text and Escape leaves caret mode.

Escape also cancels a selected action before it is clicked. The tray tooltip reports the most recent semantic target count and scan time.

Open the tray menu for key bindings, enable/disable, startup registration, refresh, and exit.

## Accessibility boundary

UI Automation is the Windows semantic accessibility API. KBUN can only expose controls that an application publishes through that API. Custom-drawn controls with no accessibility provider cannot be discovered reliably without computer vision. Likewise, Windows prevents a normal process from controlling elevated applications; run KBUN at the same integrity level as the target when that is required.
