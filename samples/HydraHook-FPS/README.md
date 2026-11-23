# HydraHook-FPS

A small overlay shows average FPS and milliseconds per frame, with a settings menu for position, size, and visibility. It supports Direct3D 9, 9Ex, 10, and 11 on Win32 and x64, plus Direct3D 12 on x64.

## Build and use

1. From a Visual Studio 2026 Developer Command Prompt in the repository root, run `prepare-deps.bat x64` or `prepare-deps.bat Win32` to match the application.
2. Open `HydraHook.slnx`, select `Release_LIB` and the matching platform, then build the **HydraHook-FPS** project. This configuration links HydraHook into the sample DLL.
3. Load `bin\x64\HydraHook-FPS.dll` or `bin\x86\HydraHook-FPS.dll` using the same workflow as the other samples.

## Keys

| Key | Action |
|-----|--------|
| **F12** | Show or hide the counter |
| **Shift+F12** | Open or close the settings menu |

These are the defaults. `CounterKey` and `MenuKey` in `HydraHook-FPS.ini` replace them. The menu shows the shortcuts that are active; it does not edit them.

A shortcut fires when its primary key goes down and the modifier set matches exactly. `Shift+F12` does not also toggle the counter. Releasing Shift while F12 is still held does not toggle the counter, and pressing Shift after F12 is already down does not open the menu. `Ctrl+F12` matches neither default.

Keys are read only while the host process owns the foreground window. `None` disables a shortcut.

## Settings menu

- Mouse and keyboard work as in the [ImGui sample](../HydraHook-ImGui/): messages reach ImGui through the per-window `WNDPROC` hook, and keyboard navigation (arrows, Tab, Space, Enter) is enabled. The menu opens focused. ImGui draws its own cursor while the menu is open.
- Input is forwarded to ImGui only while the menu is open. Closing it (Shift+F12 or the close button) releases held buttons the same way hiding the ImGui sample does.
- ImGui does not block messages from the host window, so the game still sees clicks and keys while the menu is open.
- **Show counter**: same as the counter shortcut (F12 by default).
- **Position**: top left, top right, bottom left, bottom right.
- **Scale**: 50% to 300%, with `-`/`+` (5% steps, hold to repeat) or `Reset`. There is no slider: it adds about 85 KB (x64) of ImGui text-input code.

## Settings file

Settings are stored in `HydraHook-FPS.ini`, placed like `HydraHook.log`:

- Read: the process executable's directory first, then the DLL's directory.
- Write: back to the file that was read; if that write fails (for example the file is read-only), the save fails and is retried every 5 seconds instead of writing a file the next load would ignore. When none exists, the process directory is tried first and the DLL directory is the fallback if it is not writable.
- Changes are written when no menu widget is held, when the menu closes, after the counter shortcut, and on unhook. A save writes the shortcuts that were loaded, so edit keys in the INI before the overlay hooks.
- The file is read when the overlay hooks. A shortcut edit takes effect on the next hook.

```ini
Corner=TopLeft
ScalePercent=100
ShowCounter=1
CounterKey=F12
MenuKey=Shift+F12
```

`CounterKey` and `MenuKey` syntax:

- Modifiers: `Ctrl` or `Control`, `Alt`, `Shift`. Any order, joined to the key with `+`.
- Key: `F1`–`F24`, `A`–`Z`, `0`–`9`, `Num0`–`Num9` (`Numpad0`–`Numpad9` is the same), or one of `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Pause`, `ScrollLock`, `PrintScreen`, `Tab`, `Space`, `Backspace`, `Enter`, `Escape`, `Left`, `Right`, `Up`, `Down`, `NumMultiply`, `NumAdd`, `NumSubtract`, `NumDecimal`, `NumDivide`.
- A virtual-key code `0x01`–`0xFE`, or `None` alone to disable that shortcut.
- Names are case-insensitive. Spaces around `+` are ignored.
- Rejected values stay at the previous setting: an empty piece, an unknown name, `Shift+None`, a repeated modifier, and modifier virtual keys (`0x10` plus the left/right Shift, Ctrl, and Alt codes).

Unknown keys and other invalid values are ignored. `ScalePercent` is clamped to 50-300. Values are integers and key names are ASCII, so the host's locale does not affect the file. The format is in [FpsConfig.h](FpsConfig.h).

## Reading the counter

At 60 FPS the readout is approximately `60 FPS  |  16.7 ms`. Both numbers come from the same measurement. The counter divides elapsed wall-clock time by the number of frame intervals, then takes the reciprocal for FPS. A long frame contributes its full duration to the average.

The label refreshes at most twice per second and shows `-- FPS` during warm-up. A gap of two seconds or more starts a fresh measurement. The update interval and pause threshold are in [FrameRate.h](FrameRate.h). The sample uses the [ImGui sample's host](../HydraHook-ImGui/dllmain.cpp) for Direct3D rendering; the scene hooks it calls are in [FpsOverlay.cpp](FpsOverlay.cpp).

## Size

`Release` builds use `/O1`, string pooling, and `/Gw`. The menu uses only core ImGui widgets and the INI code uses Win32 file calls, with no `<filesystem>` or iostreams.
