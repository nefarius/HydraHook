# HydraHook-FPS

A small, read-only overlay in the upper-left corner shows average FPS and milliseconds per frame. It supports Direct3D 9, 9Ex, 10, and 11 on Win32 and x64, plus Direct3D 12 on x64.

## Build and use

1. From a Visual Studio 2022 Developer Command Prompt in the repository root, run `prepare-deps.bat x64` or `prepare-deps.bat Win32` to match the application.
2. Open `HydraHook.sln`, select `Release_LIB` and the matching platform, then build the **HydraHook-FPS** project. This configuration links HydraHook into the sample DLL.
3. Load `bin\x64\HydraHook-FPS.dll` or `bin\x86\HydraHook-FPS.dll` using the same workflow as the other samples. Press **F12** to hide or show the counter.

## Reading the counter

At 60 FPS the readout is approximately `60 FPS  |  16.7 ms`. Both numbers come from the same measurement. The counter divides elapsed wall-clock time by the number of frame intervals, then takes the reciprocal for FPS. A long frame contributes its full duration to the average.

The label refreshes at most twice per second and shows `-- FPS` during warm-up. A gap of two seconds or more starts a fresh measurement. The update interval and pause threshold are in [FrameRate.h](FrameRate.h). The sample uses the [ImGui sample's host](../HydraHook-ImGui/dllmain.cpp) for Direct3D rendering and the F12 toggle.
