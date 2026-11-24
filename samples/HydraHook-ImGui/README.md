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
- On unhook the original procedure is restored and in-flight message handlers are drained with a bounded wait (original host procedures are not waited on, so a host `WNDPROC` can trigger shutdown without deadlocking). During process shutdown, `EvtHydraHookGamePreExit` uses a bounded lock attempt to release any overlay capture on the initiating window thread before the shutdown hook waits on the worker thread; cross-thread unhook skips messaging the window thread when no capture or button state is held and uses `SendMessageTimeoutW` otherwise (deferring release via `PostMessageW` and keeping the delivery path attached if the window thread times out). D3D12 unhook uses a bounded GPU fence wait and skips releasing D3D12 resources if completion cannot be confirmed. The first successful hook pins this DLL for the rest of the process (`GET_MODULE_HANDLE_EX_FLAG_PIN`), so a later `FreeLibrary` does not unmap a thread still inside the dispatcher. If another module subclassed the window afterward, the hook stays in the chain as a pass-through.

## Shared host

`dllmain.cpp` is also compiled into [HydraHook-FPS](../HydraHook-FPS/). A sample links one scene file that defines `struct OverlaySceneContext` and implements `OverlaySceneContextSize()`, `OverlaySceneConstruct()`, `OverlaySceneDestroy()`, `UpdateOverlayState()`, `OnOverlayContextCreated()`, `OnOverlayShutdown()`, and `RenderScene()` (declared in `dllmain.h`). Scene functions take a pointer to that context and must not use mutable globals or function-local statics.

Per-engine state lives in the engine's custom context memory (`HydraHookEngineAllocCustomContext`). The host allocates one block holding its own `OverlayHostContext` followed by the scene context, placement-constructs both, and destroys and frees them in `EvtHydraHookGamePostUnhook`. D3D11 and D3D12 callbacks read the context from `Extension->Context`. D3D9, D3D10 and the `WNDPROC` handler call `HydraHookEngineGetCustomContext()` on an engine they resolve first: D3D9 with `HydraHookEngineGetHandleFromD3D9Device` / `...D3D9ExDevice`, D3D10 with `HydraHookEngineGetHandleFromD3D10SwapChain`, and the handler from the engine handle passed as `WindowProcHook` user data. Per-engine runtime state lives in `OverlayHostContext`; the host keeps no mutable globals. The context is freed only when the window-procedure drain, the D3D12 GPU wait and the engine's callback drain (`HydraHookEngineAreCallbacksDrained`) are all confirmed. Otherwise it stays allocated until `HydraHookEngineDestroy` releases the memory, without running destructors.

Known gap: if the window thread does not answer the release message within 250 ms, the `WNDPROC` hook stays attached for the deferred release and keeps the engine handle as user data. Destroying the engine before that thread resumes leaves a dangling handle. `UpdateOverlayState()` returns whether to render and whether to accept input; losing input acceptance queues the release described above. This sample's `DemoScene.cpp` ties both to `F12`. `KeyToggle.h` provides the hotkey edge detector, which ignores presses while another process owns the foreground window.

## Limitations

> [!NOTE]
> Gamepad navigation is not enabled. The Direct3D 12 overlay is x64-only; Win32 builds do not register the D3D12 callbacks.