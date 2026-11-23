# HydraHook-OpenCV

Direct3D 11 and Direct3D 12 sample that reads the back buffer asynchronously and runs an OpenCV perception pipeline.

## About

> [!CAUTION]
> This sample is still rough around the edges and has not been tested with many games. Use it on games you own.

On each Present the sample copies the back buffer into a staging resource (D3D11 `Map`, or a D3D12 readback heap) and hands the frame to a worker thread. The worker:

- detects features with ORB
- tracks them with Lucas-Kanade optical flow
- estimates motion with a RANSAC essential matrix and recovers a pose trail

An ImGui debug HUD draws the trail and feature counts. Press `F12` to toggle it. The D3D12 hook and ImGui overlay are x64-only; the x86 build enables D3D11 only. D3D11 works on x86 and x64.

## Building

1. Initialize submodules with `git submodule update --init --recursive`, then run `prepare-deps.bat` from the repository root so vcpkg installs `opencv4`. The [root build steps](../../README.md#build-steps) cover the same prerequisite.
2. Build `Release_LIB` (or `Release` of this project against the static library). The DLL is `bin\x64\HydraHook-OpenCV.dll` or `bin\x86\HydraHook-OpenCV.dll`.
3. Inject that DLL into a D3D11 or D3D12 process. Use the x64 build for D3D12.
