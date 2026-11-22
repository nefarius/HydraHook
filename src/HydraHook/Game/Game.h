/**
 * @file Game.h
 * @brief Main hook worker thread entry point.
 *
 * HydraHookMainThread runs in a dedicated thread, probing for Direct3D
 * and Core Audio APIs, then installing hooks and dispatching callbacks.
 *
 * @internal
 * @copyright MIT License (c) 2018-2026 Benjamin Höglinger-Stelzer
 */

/*
MIT License

Copyright (c) 2018-2026 Benjamin Höglinger-Stelzer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

#include <Windows.h>

struct IDXGISwapChain;
struct ID3D12CommandQueue;

/**
 * @brief Returns the D3D12 command queue associated with a swap chain.
 *
 * Checks the swap-chain map filled by the CreateSwapChain hook, then the
 * device map filled by the ExecuteCommandLists hook.
 *
 * @param pSwapChain The swap chain to query. May be null.
 * @return The command queue with AddRef, or nullptr if it is not captured or @p pSwapChain is null.
 */
ID3D12CommandQueue* GetD3D12CommandQueueForSwapChain(IDXGISwapChain* pSwapChain);

/**
 * @brief Main hook worker thread; probes and hooks render and audio APIs.
 *
 * Installs the configured hooks, waits for cancellation, then uninstalls them.
 * The worker does not own the unload. When the host calls FreeLibrary, that
 * hook runs cleanup and its caller exits with FreeLibraryAndExitThread, while
 * the worker exits with ExitThread. On any other shutdown the worker exits
 * with FreeLibraryAndExitThread after cleanup.
 *
 * @param Params PHYDRAHOOK_ENGINE cast to LPVOID.
 * @return Thread exit code. The thread does not return to its caller.
 */
DWORD WINAPI HydraHookMainThread(LPVOID Params);
