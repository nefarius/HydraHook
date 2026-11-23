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
- `F12` toggles the overlay. While it is hidden, new messages are not passed to ImGui. Hiding posts a release to the window thread and keeps new input out until that release has run, including if `F12` shows the overlay again first. The release drops held overlay buttons and keys. If the host already owned mouse capture, the Win32 backend's button mask is cleared without `ReleaseCapture` or `WM_CAPTURECHANGED`. `ReleaseCapture` runs only when this hook's backend took capture. If the release message cannot be registered, the overlay stays render-only.
- ImGui sees every message first. If it returns non-zero, the host window does not receive that message. The sample does not consult `WantCaptureMouse` / `WantCaptureKeyboard`.
- ImGui calls from the window thread and from Present share one lock.
- `WM_DESTROY` and `WM_NCDESTROY` always reach the host procedure.
- On unhook the original procedure is restored. The first successful hook pins this DLL for the rest of the process (`GET_MODULE_HANDLE_EX_FLAG_PIN`), so a later `FreeLibrary` does not unmap a thread still inside the dispatcher. If another module subclassed the window afterwards, the hook stays in the chain as a pass-through.

## Limitations

> [!NOTE]
> Gamepad navigation is not enabled. The Direct3D 12 overlay is x64-only; Win32 builds do not register the D3D12 callbacks.