# HydraHook-DirectXTK

DirectX 11 sample demonstrating text rendering in foreign processes.

## About

This sample shows how to render text using [DirectXTK](https://github.com/microsoft/DirectXTK) (SpriteFont and SpriteBatch), consumed via vcpkg. It draws an overlay in D3D11 Present hooks.

## Building

1. Run `prepare-deps.bat` from a **Developer Command Prompt for VS 2026** (or x64 Native Tools Command Prompt) to install vcpkg dependencies (including DirectXTK and MakeSpriteFont).
2. Build the solution. A pre-build step generates `Arial.spritefont` from the system Arial font; the output is placed next to the DLL in the output directory.
3. Deploy the DLL and `Arial.spritefont` together when injecting into a target process.
