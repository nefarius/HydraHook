# Samples

This folder contains sample libraries which demonstrate how to render custom content in foreign processes using HydraHook.

## Available samples

| Sample | Description |
|--------|-------------|
| [HydraHook-FPS](HydraHook-FPS/) | Compact counter showing average FPS and frame time, refreshed twice per second. F12 shows or hides it; Shift+F12 opens an ImGui menu for corner, scale, and visibility. Both shortcuts are configurable in `HydraHook-FPS.ini` |
| [HydraHook-ImGui](HydraHook-ImGui/) | ImGui overlay for DirectX 9, 10, 11, and DirectX 12 (x64 only). Mouse and keyboard go through a per-window procedure hook with pre-exit capture release and bounded shutdown/unhook waits; F12 shows and hides the overlay |
| [HydraHook-DirectXTK](HydraHook-DirectXTK/) | DirectX 11 text rendering using DirectXTK SpriteFont |
| [HydraHook-OpenCV](HydraHook-OpenCV/) | OpenCV perception pipeline with an ImGui overlay. D3D11 is x86 and x64. The D3D12 hook and ImGui overlay are x64 only |
