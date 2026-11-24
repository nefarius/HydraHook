/**
 * @file HydraHookDirect3D10.h
 * @brief Direct3D 10 hook callbacks and helpers.
 *
 * Defines event callbacks for Present, ResizeTarget, and ResizeBuffers.
 * Use HydraHookEngineSetD3D10EventCallbacks to register.
 *
 * D3D10, D3D11, and D3D12 share unified DXGI swap-chain hooks (`Present`/`Present1`,
 * `ResizeTarget`, `ResizeBuffers`/`ResizeBuffers1`) that dispatch per call to the
 * callback table matching the swap chain's enabled device runtime.
 *
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


#ifndef HydraHookDirect3D10_h__
#define HydraHookDirect3D10_h__

#ifndef HYDRAHOOK_NO_D3D10

#include <dxgi.h>
#include <d3d10_1.h>

/** @brief Callback for IDXGISwapChain::Present (pre/post). */
typedef
_Function_class_(EVT_HYDRAHOOK_D3D10_PRESENT)
VOID
EVT_HYDRAHOOK_D3D10_PRESENT(
    IDXGISwapChain  *pSwapChain,
    UINT            SyncInterval,
    UINT            Flags
);

typedef EVT_HYDRAHOOK_D3D10_PRESENT *PFN_HYDRAHOOK_D3D10_PRESENT;

/** @brief Callback for IDXGISwapChain::ResizeTarget (pre/post). */
typedef
_Function_class_(EVT_HYDRAHOOK_D3D10_RESIZE_TARGET)
VOID
EVT_HYDRAHOOK_D3D10_RESIZE_TARGET(
    IDXGISwapChain          *pSwapChain,
    const DXGI_MODE_DESC    *pNewTargetParameters
);

typedef EVT_HYDRAHOOK_D3D10_RESIZE_TARGET *PFN_HYDRAHOOK_D3D10_RESIZE_TARGET;

/** @brief Callback for IDXGISwapChain::ResizeBuffers (pre/post). */
typedef
_Function_class_(EVT_HYDRAHOOK_D3D10_RESIZE_BUFFERS)
VOID
EVT_HYDRAHOOK_D3D10_RESIZE_BUFFERS(
    IDXGISwapChain  *pSwapChain,
    UINT            BufferCount,
    UINT            Width,
    UINT            Height,
    DXGI_FORMAT     NewFormat,
    UINT            SwapChainFlags
);

typedef EVT_HYDRAHOOK_D3D10_RESIZE_BUFFERS *PFN_HYDRAHOOK_D3D10_RESIZE_BUFFERS;

/**
 * @brief Direct3D 10 event callback collection.
 */
typedef struct _HYDRAHOOK_D3D10_EVENT_CALLBACKS
{
    PFN_HYDRAHOOK_D3D10_PRESENT          EvtHydraHookD3D10PrePresent;
    PFN_HYDRAHOOK_D3D10_PRESENT          EvtHydraHookD3D10PostPresent;

    PFN_HYDRAHOOK_D3D10_RESIZE_TARGET    EvtHydraHookD3D10PreResizeTarget;
    PFN_HYDRAHOOK_D3D10_RESIZE_TARGET    EvtHydraHookD3D10PostResizeTarget;

    PFN_HYDRAHOOK_D3D10_RESIZE_BUFFERS   EvtHydraHookD3D10PreResizeBuffers;
    PFN_HYDRAHOOK_D3D10_RESIZE_BUFFERS   EvtHydraHookD3D10PostResizeBuffers;

} HYDRAHOOK_D3D10_EVENT_CALLBACKS, *PHYDRAHOOK_D3D10_EVENT_CALLBACKS;

/**
 * @brief Retrieves ID3D10Device from IDXGISwapChain.
 * @param[in] pSwapChain Valid swap chain.
 * @param[out] ppDevice Receives device pointer.
 * @return S_OK on success.
 */
HRESULT
FORCEINLINE
D3D10_DEVICE_FROM_SWAPCHAIN(
    IDXGISwapChain *pSwapChain,
    ID3D10Device **ppDevice
)
{
    return pSwapChain->GetDevice(__uuidof(ID3D10Device), (PVOID*)ppDevice);
}

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Resolves the engine handle from the swap chain passed to a Direct3D 10 callback.
 *
 * Direct3D 10 callbacks carry no extension, so this is the way to reach
 * HydraHookEngineGetCustomContext() from them. The engine tracks the swap chain of the most
 * recent D3D10 Present or Present1 it dispatched. When that does not match (the game owns
 * several swap chains) and the module hosts exactly one engine, that engine is returned.
 *
 * @param[in] SwapChain Swap chain received by the callback.
 * @return Engine handle, or NULL if @p SwapChain is NULL or not tracked by any engine.
 */
HYDRAHOOK_API PHYDRAHOOK_ENGINE HydraHookEngineGetHandleFromD3D10SwapChain(
    _In_
    IDXGISwapChain *SwapChain
);

#ifdef __cplusplus
}
#endif

/**
 * @brief Initializes Direct3D 10 callback collection (zeros all pointers).
 * @param[out] Callbacks Callback structure to initialize.
 */
VOID FORCEINLINE HYDRAHOOK_D3D10_EVENT_CALLBACKS_INIT(
    _Out_ PHYDRAHOOK_D3D10_EVENT_CALLBACKS Callbacks
)
{
    ZeroMemory(Callbacks, sizeof(HYDRAHOOK_D3D10_EVENT_CALLBACKS));
}

#endif

#endif // HydraHookDirect3D10_h__
