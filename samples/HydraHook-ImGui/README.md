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

## Limitations

> [!NOTE]
> Mouse, keyboard and gamepad support is currently not implemented.
