# <img src="assets/HydraHook_128x128.png" align="left" /> HydraHook

API-Hooking and rendering framework for DirectX-based games.

[![Build status](https://github.com/nefarius/HydraHook/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/nefarius/HydraHook/actions/workflows/build.yml)
[![Documentation](https://github.com/nefarius/HydraHook/actions/workflows/docs.yml/badge.svg?branch=master)](https://github.com/nefarius/HydraHook/actions/workflows/docs.yml)
[![GitHub All Releases](https://img.shields.io/github/downloads/nefarius/HydraHook/total)](https://github.com/nefarius/HydraHook/releases)
[![Discord](https://img.shields.io/discord/346756263763378176.svg)](https://discord.nefarius.at)
[![Website](https://img.shields.io/website-up-down-green-red/https/docs.nefarius.at.svg?label=docs.nefarius.at)](https://docs.nefarius.at/)
[![GitHub followers](https://img.shields.io/github/followers/nefarius.svg?style=social&label=Follow)](https://github.com/nefarius)
[![Mastodon Follow](https://img.shields.io/mastodon/follow/109321120351128938?domain=https%3A%2F%2Ffosstodon.org%2F&style=social)](https://fosstodon.org/@Nefarius)

## About

`HydraHook` consists of a self-contained library (DLL) which exposes a minimalistic API for rendering custom content in foreign processes eliminating the need for in-depth knowledge about Direct3D and API-hooking. The most common use-case might be drawing custom overlays on top of your games. The framework takes care about pesky tasks like detecting the DirectX version the game was built for.

> [!CAUTION]
> Use caution when injecting HydraHook into any game protected by anti-cheat software. API hooking and DLL injection are commonly detected by anti-cheat systems and may result in permanent bans from online services. Use only with games you own and in environments where such use is permitted.

> [!NOTE]
> The authors of this project do not condone the use of HydraHook in cheating software. The project's motivation has been curiosity and education entirely.

## Features

- **Runtime injection and ejection** — No launcher required. Inject into any running DirectX process and eject via `FreeLibrary` when done. Supports mid-process injection (e.g. D3D12 games already running). Graceful shutdown via `ExitProcess` / `PostQuitMessage` / `FreeLibrary` hooks avoids loader-lock and unload races.

- **Unhook and rehook without unloading** — `HydraHookEngineUnhook` removes all hooks and keeps the DLL loaded; `HydraHookEngineRehook` reinstalls them and fires `EvtHydraHookGameHooked` again. Registered callback tables survive the cycle. Do not call either from `DllMain`, a hook callback, or the worker thread.

- **Performance-focused** — Lock-free hot path for hook callbacks (atomic counters only; no mutex, no kernel transition). Shutdown uses loader-lock-safe `remove_nothrow` to avoid deadlocks during process exit.

- **No external dependencies at runtime** — Everything needed is statically linked in. Just drop the DLL into the target process; no redistributables or runtime packages required.
- **Core Audio** — Optional `IAudioRenderClient` buffer hooks alongside the Direct3D pipeline.

## Supported platforms

- Windows 10 and Windows 11
- x86 and x64
- Direct3D 12 hooking is built for x86 and x64. The D3D12 ImGui overlay is x64-only
- ARM and UWP are not supported

## Supported DirectX versions

- DirectX 9.0
- DirectX 9.0 Extended (Vista+)
- DirectX 10
- DirectX 11
- DirectX 12

## How to build

### Prerequisites

- Visual Studio 2022 or newer with the **v143** toolset ([Community Edition](https://visualstudio.microsoft.com/downloads/) is free)
- [Windows SDK](https://learn.microsoft.com/en-us/windows/apps/windows-sdk/downloads)

### Build steps

1. Clone the repository and initialize submodules: `git submodule update --init --recursive`
2. From the repository root, run `prepare-deps.bat` (both x86 and x64), `prepare-deps.bat x64`, or `prepare-deps.bat Win32`. With no arguments the script locates Visual Studio via `vswhere` and sets up the compiler itself.
3. Open `HydraHook.sln` and build `Release_LIB` (static library, samples, and tests) or `Release` (standalone `HydraHook.dll`)

Dependencies (`spdlog`, `detours`, `imgui`, `directxtk`, `opencv4`) are declared in `vcpkg.json` and installed via [vcpkg](https://github.com/microsoft/vcpkg) (included as a submodule). The manifest baseline is pinned in `vcpkg-configuration.json`. GitHub Actions builds core/tests, the ImGui/FPS/DirectXTK samples, and the OpenCV dependencies/sample in separate jobs and caches each slice's vcpkg archives per architecture. Publishing those packages to GitHub Packages requires the `VCPKG_NUGET_TOKEN` secret, a PAT with `read:packages` and `write:packages`. The workflow token cannot create packages under a personal account.

### Tests

`Release_LIB` builds the C++ tests for the selected platform. Executables are written to `bin\x64` or `bin\x86`:

- `HydraHookTests.exe` — public API helpers, engine handle checks, the Detours wrapper, shutdown tracking, crash-handler refcounting, the FPS sampler, and the ImGui sample's `ResettableInitGuard` (once-only init, reset, retry-on-failure, concurrent callers) and `WindowProcHook` (validation, forwarding, destroy cleanup, external subclass chaining, re-entrancy, cross-thread hook churn). `HookActivityTracker::reset`, `ResettableOnce`, `Hook` re-apply, and the `HydraHookEngineUnhook`/`HydraHookEngineRehook` handle checks are covered too. No test exercises a real game hook/unhook/rehook cycle; the WARP smoke test covers a synthetic D3D11 one.
- `HydraHookWarpSmokeTests.exe` — headless D3D11 WARP device. It loads `HydraHookTestHost.dll` from the same directory checks Present / ResizeBuffers callbacks, then runs a full `HydraHookEngineUnhook` / `HydraHookEngineRehook` cycle on the same DLL

```text
bin\x64\HydraHookTests.exe
bin\x64\HydraHookWarpSmokeTests.exe
```

### API reference

The reference covers the public C API and the engine sources. It is not required to build the DLL.

- [Doxygen](https://www.doxygen.nl/) 1.18.0
- Theme: [Doxygen Awesome](https://github.com/jothepro/doxygen-awesome-css) 2.5.0 (MIT), vendored in `docs/doxygen-awesome/`

```text
doxygen Doxyfile
```

Open `docs/html/index.html`. Generated HTML is not committed.

Published reference: <https://nefarius.github.io/HydraHook/>

Pull requests upload that HTML as an artifact. A push to `master` publishes the same build to GitHub Pages.

### Releases

A `vX.Y.Z` tag (for example `v4.2.0`) builds, tests, signs, and publishes a stable GitHub Release. A `vX.Y.Z-preN` tag (for example `v4.2.0-pre1`, where `N` is one or more digits) publishes a prerelease. Rerunning a tag that already has a published release returns that release to draft before the archives are replaced, and publishes it again only after both uploads succeed. Signing requires the `SIGN_RELAY_CI_TOKEN` repository secret. `SIGN_RELAY_SERVER` is optional and defaults to `https://signrelay.api.nefarius.systems/`.

### Pre-built binaries

Signed x86 and x64 archives are published on [GitHub Releases](https://github.com/nefarius/HydraHook/releases/latest). Each archive contains `HydraHook.dll` and the sample host DLLs. Inject them with [Injector](https://github.com/nefarius/Injector) or [HydraHookManager](https://github.com/nefarius/HydraHookManager).

## How to use

For a ready-to-use FPS counter, build the [HydraHook-FPS sample](samples/HydraHook-FPS/).

Inject the resulting host library (e.g. `HydraHook-ImGui.dll`) into the target process with [Injector](https://github.com/nefarius/Injector) or [HydraHookManager](https://github.com/nefarius/HydraHookManager). The following example loads the [imgui sample](samples/HydraHook-ImGui):

```PowerShell
.\Injector -n hl2.exe -i HydraHook-ImGui.dll
```

Just make sure your host library doesn't require any external dependencies not present in the process context or you'll get a `LoadLibrary failed` error.

## Diagnostics

The core library logs its progress and potential errors to `HydraHook.log`. It tries to write in this order: (1) the directory of the process executable, (2) the directory of the HydraHook DLL, (3) `%TEMP%` if both prior locations fail (e.g. no write permissions).

## Support

Report defects and compatibility problems on the [issue tracker](https://github.com/nefarius/HydraHook/issues). Usage questions belong on [Discord](https://discord.nefarius.at). Anti-cheat enforcement is outside the scope of this project; see the caution above.

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

Core Direct3D 12 hooking is built for x86 and x64. The ImGui sample's D3D12 overlay is x64-only.

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
- [Doxygen Awesome](https://github.com/jothepro/doxygen-awesome-css) 2.5.0 — HTML theme (MIT). Stylesheets and license: `docs/doxygen-awesome/`

## License

MIT. See [LICENSE](LICENSE).
