# HydraHook-ImGui

ImGui overlay sample for DirectX 9, 10, 11, and 12 games.

## About

Demonstrates how to render [Dear ImGui](https://github.com/ocornut/imgui) interfaces in foreign processes. This sample supports multiple DirectX versions and automatically detects the game's renderer.

## Usage

Build the project, then inject `HydraHook-ImGui.dll` into a target process using a DLL injector:

```powershell
.\Injector -n hl2.exe -i HydraHook-ImGui.dll
```

Replace `hl2.exe` with your target process name.

## Input

- Mouse and keyboard messages reach ImGui through a per-window `WNDPROC` hook (`WindowProcHook.h`). It uses `SetWindowLongPtrW` and forwards to the original procedure with `CallWindowProcW`.
- `F12` toggles the overlay. While it is hidden, no messages are passed to ImGui.
- ImGui sees every message first. If it returns non-zero, the host window does not receive that message. The sample does not consult `WantCaptureMouse` / `WantCaptureKeyboard`.
- `WM_DESTROY` and `WM_NCDESTROY` always reach the host procedure.
- On unhook the original procedure is restored. If another module subclassed the window afterwards, the hook stays in the chain as a pass-through until the window is destroyed.

## Limitations

> [!NOTE]
> Gamepad navigation is not enabled.