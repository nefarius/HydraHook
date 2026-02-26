# <img src="assets/HydraHook_128x128.png" align="left" /> HydraHook

API-Hooking and rendering framework for DirectX-based games.

[![Build status](https://ci.appveyor.com/api/projects/status/gi1n6yvtuhea55jl?svg=true)](https://ci.appveyor.com/project/nefarius/hydrahook)
[![Discord](https://img.shields.io/discord/346756263763378176.svg)](https://discord.nefarius.at)
[![Website](https://img.shields.io/website-up-down-green-red/https/docs.nefarius.at.svg?label=docs.nefarius.at)](https://docs.nefarius.at/)
[![GitHub followers](https://img.shields.io/github/followers/nefarius.svg?style=social&label=Follow)](https://github.com/nefarius)
[![Mastodon Follow](https://img.shields.io/mastodon/follow/109321120351128938?domain=https%3A%2F%2Ffosstodon.org%2F&style=social)](https://fosstodon.org/@Nefarius)

## About

`HydraHook` consists of a self-contained library (DLL) which exposes a minimalistic API for rendering custom content in foreign processes eliminating the need for in-depth knowledge about Direct3D and API-hooking. The most common use-case might be drawing custom overlays on top of your games. The framework takes care about pesky tasks like detecting the DirectX version the game was built for and supports runtime-hooking (no special launcher application required).

> [!CAUTION]
> Use caution when injecting HydraHook into any game protected by anti-cheat software. API hooking and DLL injection are commonly detected by anti-cheat systems and may result in permanent bans from online services. Use only with games you own and in environments where such use is permitted.

> [!NOTE]
> The authors of this project do not condone the use of HydraHook in cheating software. The project's motivation has been curiosity and education entirely.

## Supported DirectX versions

- DirectX 9.0
- DirectX 9.0 Extended (Vista+)
- DirectX 10
- DirectX 11
- DirectX 12

## How to build

### Prerequisites

- Visual Studio **2022** ([Community Edition](https://visualstudio.microsoft.com/downloads/) is free)
- [Windows SDK](https://learn.microsoft.com/en-us/windows/apps/windows-sdk/downloads)

### Build steps

1. Clone the repository and initialize submodules: `git submodule update --init --recursive`
2. Open `HydraHook.sln` in Visual Studio and build

Dependencies (spdlog, detours, imgui, directxtk) are declared in `vcpkg.json` and installed via [vcpkg](https://github.com/microsoft/vcpkg) (included as a submodule). Run `prepare-deps.bat` from a **Developer Command Prompt for VS 2022** (or x64 Native Tools Command Prompt) before the first build in Visual Studio; the build will use existing `vcpkg_installed` if present.

### Pre-built binaries

![Last updated](https://buildbot.nefarius.at/builds/HydraHook/latest/LAST_UPDATED_AT.svg)

Pre-built binaries are available from the [buildbot](https://buildbot.nefarius.at/builds/HydraHook/latest/). Note: these builds may be outdated; for the latest code, build from source.

## How to use

Inject the resulting host library (e.g. `HydraHook-ImGui.dll`) into the target process first using a DLL injection utility of your choice (you can ofc. [use mine as well](https://github.com/nefarius/Injector)). The following example loads the [imgui sample](samples/HydraHook-ImGui):

```PowerShell
.\Injector -n hl2.exe -i HydraHook-ImGui.dll
```

Just make sure your host library doesn't require any external dependencies not present in the process context or you'll get a `LoadLibrary failed` error.

## Diagnostics

The core library logs its progress and potential errors to `HydraHook.log`. It tries to write in this order: (1) the directory of the process executable, (2) the directory of the HydraHook DLL, (3) `%TEMP%` if both prior locations fail (e.g. no write permissions).

## Demos

The following demo videos show [imgui](https://github.com/ocornut/imgui) being rendered in foreign processes using different versions of DirectX. Click a thumbnail to watch the video.

### DirectX 9

Half-Life 2, 32-Bit

[![Half-Life 2 Demo](https://placehold.co/640x360/e8e8e8/999999?text=Half-Life+2+%28click+to+watch%29)](https://sharex.nefarius.at/u/hl2.mp4)

### DirectX 9 Ex

Castlevania: Lords of Shadow, 32-Bit

[![Castlevania: Lords of Shadow Demo](https://placehold.co/640x360/e8e8e8/999999?text=Castlevania+%28click+to+watch%29)](https://sharex.nefarius.at/u/CastlevaniaLoSUE.mp4)

### DirectX 10

Bioshock 2, 32-Bit

[![Bioshock 2 Demo](https://placehold.co/640x360/e8e8e8/999999?text=Bioshock+2+%28click+to+watch%29)](https://sharex.nefarius.at/u/BioShock2.mp4)

### DirectX 11

Road Redemption, 64-Bit

[![Road Redemption Demo](https://placehold.co/640x360/e8e8e8/999999?text=Road+Redemption+%28click+to+watch%29)](https://sharex.nefarius.at/u/RoadRedemption.mp4)

### DirectX 12

Supported on 64-bit builds. The ImGui sample automatically detects and hooks DX12 games.

BEHEMOTH, 64-Bit

[![BEHEMOTH Demo](https://placehold.co/640x360/e8e8e8/999999?text=BEHEMOTH+%28click+to+watch%29)](https://sharex.nefarius.at/u/BEHEMOTH.mp4)

## Sources

### Dependencies

This project uses the following libraries (via [vcpkg](https://github.com/microsoft/vcpkg)):

**Core library only:**
- [spdlog](https://github.com/gabime/spdlog) – Fast C++ logging library
- [Microsoft Detours](https://github.com/microsoft/Detours) – API hooking library

**Sample projects only:**
- [Dear ImGui](https://github.com/ocornut/imgui) – Immediate mode GUI
- [DirectXTK](https://github.com/microsoft/DirectXTK) – DirectX Toolkit
- [OpenCV](https://github.com/opencv/opencv) – Computer vision library

### References & inspiration

- [DX9-Overlay-API](https://github.com/agrippa1994/DX9-Overlay-API)
- [Creating a render target in DirectX 11](https://www.hlsl.co.uk/blog/2014/11/19/creating-a-render-target-in-directx11)
- [ion RE Library](https://github.com/scen/ionlib)
- [C# – Screen capture and overlays for Direct3D 9, 10 and 11 using API hooks](https://web.archive.org/web/20240922031432/https://spazzarama.com/2011/03/14/c-screen-capture-and-overlays-for-direct3d-9-10-and-11-using-api-hooks/)
- [HelloD3D12](https://github.com/GPUOpen-LibrariesAndSDKs/HelloD3D12)
