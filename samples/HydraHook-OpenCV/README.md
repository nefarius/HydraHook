# HydraHook-OpenCV

Skeleton sample demonstrating OpenCV integration with DirectX 11 and DirectX 12 hooks.

## About

> [!CAUTION]
> This sample app is still rough around the edges and hasn't been tested thoroughly with many games yet. Use with caution (or help finishing it) when testing!

This sample wires up HydraHook's D3D11 and D3D12 Present hooks with minimal OpenCV usage. It clears the back buffer to a teal color as a visual proof that the overlay is rendering. OpenCV is consumed via vcpkg.

## Building

1. Run `prepare-deps.bat` from the repository root (from a **Developer Command Prompt for VS 2022** or x64 Native Tools Command Prompt) to install vcpkg dependencies, including OpenCV.
2. Build the solution. The output DLL is placed in `bin/$(Configuration)/$(PlatformShortName)/` (Debug) or `bin/$(PlatformShortName)/` (Release).
3. Inject the DLL into a D3D11 or D3D12 game to verify the hooks fire and the teal overlay appears.
