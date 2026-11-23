/**
 * @file HydraHookDirect3D12.h
 * @brief Direct3D 12 hook callbacks.
 *
 * Defines event callbacks for Present, ResizeTarget, and ResizeBuffers with
 * HYDRAHOOK_EVT_PRE_EXTENSION / HYDRAHOOK_EVT_POST_EXTENSION for engine access.
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


#ifndef HydraHookDirect3D12_h__
#define HydraHookDirect3D12_h__

#ifndef HYDRAHOOK_NO_D3D12

#include "HydraHookCore.h"
#include <dxgi.h>
#include <d3d12.h>


/** @brief Pre-Present callback; receives extension with engine handle. */
typedef
_Function_class_(EVT_HYDRAHOOK_D3D12_PRE_PRESENT)
VOID
EVT_HYDRAHOOK_D3D12_PRE_PRESENT(
    IDXGISwapChain                  *pSwapChain,
    UINT                            SyncInterval,
    UINT                            Flags,
    PHYDRAHOOK_EVT_PRE_EXTENSION     Extension
);

typedef EVT_HYDRAHOOK_D3D12_PRE_PRESENT *PFN_HYDRAHOOK_D3D12_PRE_PRESENT;

/** @brief Post-Present callback; receives extension with engine handle. */
typedef
_Function_class_(EVT_HYDRAHOOK_D3D12_POST_PRESENT)
VOID
EVT_HYDRAHOOK_D3D12_POST_PRESENT(
    IDXGISwapChain                  *pSwapChain,
    UINT                            SyncInterval,
    UINT                            Flags,
    PHYDRAHOOK_EVT_POST_EXTENSION    Extension
);

typedef EVT_HYDRAHOOK_D3D12_POST_PRESENT *PFN_HYDRAHOOK_D3D12_POST_PRESENT;

/** @brief Pre-ResizeTarget callback. */
typedef
_Function_class_(EVT_HYDRAHOOK_D3D12_PRE_RESIZE_TARGET)
VOID
EVT_HYDRAHOOK_D3D12_PRE_RESIZE_TARGET(
    IDXGISwapChain                  *pSwapChain,
    const DXGI_MODE_DESC            *pNewTargetParameters,
    PHYDRAHOOK_EVT_PRE_EXTENSION     Extension
);

typedef EVT_HYDRAHOOK_D3D12_PRE_RESIZE_TARGET *PFN_HYDRAHOOK_D3D12_PRE_RESIZE_TARGET;

/** @brief Post-ResizeTarget callback. */
typedef
_Function_class_(EVT_HYDRAHOOK_D3D12_POST_RESIZE_TARGET)
VOID
EVT_HYDRAHOOK_D3D12_POST_RESIZE_TARGET(
    IDXGISwapChain                  *pSwapChain,
    const DXGI_MODE_DESC            *pNewTargetParameters,
    PHYDRAHOOK_EVT_POST_EXTENSION    Extension
);

typedef EVT_HYDRAHOOK_D3D12_POST_RESIZE_TARGET *PFN_HYDRAHOOK_D3D12_POST_RESIZE_TARGET;

/** @brief Pre-ResizeBuffers callback. */
typedef
_Function_class_(EVT_HYDRAHOOK_D3D12_PRE_RESIZE_BUFFERS)
VOID
EVT_HYDRAHOOK_D3D12_PRE_RESIZE_BUFFERS(
    IDXGISwapChain                  *pSwapChain,
    UINT                            BufferCount,
    UINT                            Width,
    UINT                            Height,
    DXGI_FORMAT                     NewFormat,
    UINT                            SwapChainFlags,
    PHYDRAHOOK_EVT_PRE_EXTENSION     Extension
);

typedef EVT_HYDRAHOOK_D3D12_PRE_RESIZE_BUFFERS *PFN_HYDRAHOOK_D3D12_PRE_RESIZE_BUFFERS;

/** @brief Post-ResizeBuffers callback. */
typedef
_Function_class_(EVT_HYDRAHOOK_D3D12_POST_RESIZE_BUFFERS)
VOID
EVT_HYDRAHOOK_D3D12_POST_RESIZE_BUFFERS(
    IDXGISwapChain                  *pSwapChain,
    UINT                            BufferCount,
    UINT                            Width,
    UINT                            Height,
    DXGI_FORMAT                     NewFormat,
    UINT                            SwapChainFlags,
    PHYDRAHOOK_EVT_POST_EXTENSION    Extension
);

typedef EVT_HYDRAHOOK_D3D12_POST_RESIZE_BUFFERS *PFN_HYDRAHOOK_D3D12_POST_RESIZE_BUFFERS;

/**
 * Obtain the D3D12 device associated with a swap chain.
 * @param[in] pSwapChain Swap chain to query for its device.
 * @param[out] ppDevice Receives the ID3D12Device pointer; the returned interface is AddRef'd on success and must be Release'd by the caller.
 * @return `S_OK` on success; otherwise an HRESULT error code.
 */
HRESULT
FORCEINLINE
D3D12_DEVICE_FROM_SWAPCHAIN(
    IDXGISwapChain          *pSwapChain,
    ID3D12Device            **ppDevice
)
{
    return pSwapChain->GetDevice(__uuidof(ID3D12Device), (PVOID*)ppDevice);
}

/**
 * @brief Retrieves back buffer resource from swap chain.
 * @param[in] pSwapChain Valid swap chain.
 * @param[out] ppBackBuffer Receives ID3D12Resource pointer.
 * @param[in] uBuffer Buffer index (default 0).
 * @return S_OK on success.
 */
HRESULT
FORCEINLINE
D3D12_BACKBUFFER_FROM_SWAPCHAIN(
    IDXGISwapChain          *pSwapChain,
    ID3D12Resource           **ppBackBuffer,
    UINT                    uBuffer = 0
)
{
    return pSwapChain->GetBuffer(uBuffer, __uuidof(ID3D12Resource), (PVOID*)ppBackBuffer);
}

/**
 * @brief Direct3D 12 event callback collection.
 */
typedef struct _HYDRAHOOK_D3D12_EVENT_CALLBACKS
{
    PFN_HYDRAHOOK_D3D12_PRE_PRESENT          EvtHydraHookD3D12PrePresent;
    PFN_HYDRAHOOK_D3D12_POST_PRESENT         EvtHydraHookD3D12PostPresent;

    PFN_HYDRAHOOK_D3D12_PRE_RESIZE_TARGET    EvtHydraHookD3D12PreResizeTarget;
    PFN_HYDRAHOOK_D3D12_POST_RESIZE_TARGET   EvtHydraHookD3D12PostResizeTarget;

    PFN_HYDRAHOOK_D3D12_PRE_RESIZE_BUFFERS   EvtHydraHookD3D12PreResizeBuffers;
    PFN_HYDRAHOOK_D3D12_POST_RESIZE_BUFFERS  EvtHydraHookD3D12PostResizeBuffers;

} HYDRAHOOK_D3D12_EVENT_CALLBACKS, *PHYDRAHOOK_D3D12_EVENT_CALLBACKS;

/**
 * Initialize a HYDRAHOOK_D3D12_EVENT_CALLBACKS structure by zeroing its memory.
 *
 * @param[out] Callbacks Pointer to the callback structure to initialize; all callback members will be set to NULL.
 */
VOID FORCEINLINE HYDRAHOOK_D3D12_EVENT_CALLBACKS_INIT(
    _Out_ PHYDRAHOOK_D3D12_EVENT_CALLBACKS Callbacks
)
{
    ZeroMemory(Callbacks, sizeof(HYDRAHOOK_D3D12_EVENT_CALLBACKS));
}

/**
 * @brief Registers Direct3D 12 render pipeline callbacks.
 * @param[in] Engine Valid engine handle.
 * @param[in] Callbacks Callback collection; NULL pointers are ignored.
 */
HYDRAHOOK_API VOID HydraHookEngineSetD3D12EventCallbacks(
    _In_ PHYDRAHOOK_ENGINE Engine,
    _In_ PHYDRAHOOK_D3D12_EVENT_CALLBACKS Callbacks
);

/**
 * @brief Returns the D3D12 command queue for a swap chain (for overlay rendering).
 * Supports mid-process injection: the queue is captured at runtime via ExecuteCommandLists.
 * @param[in] pSwapChain The swap chain (e.g. from Present callback).
 * @return The command queue with AddRef, or NULL if not yet captured. Caller must Release.
 */
HYDRAHOOK_API ID3D12CommandQueue* HydraHookEngineGetD3D12CommandQueue(
    _In_ IDXGISwapChain* pSwapChain
);

#endif

#endif // HydraHookDirect3D12_h__