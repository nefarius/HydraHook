/**
 * @file Engine.h
 * @brief Internal engine structure and callback invocation macros.
 *
 * Defines the opaque HYDRAHOOK_ENGINE layout and macros for invoking
 * version-specific callbacks. Internal use only; not part of public API.
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

/**
 * @brief Internal engine instance structure (opaque in public API).
 */
typedef struct _HYDRAHOOK_ENGINE
{
    HMODULE HostInstance;                    /**< Host DLL module handle. */
    HMODULE DllModule;
    HYDRAHOOK_D3D_VERSION GameVersion;       /**< Detected render API version. */
    HYDRAHOOK_ENGINE_CONFIG EngineConfig;   /**< Configuration at creation. */
    HYDRAHOOK_D3D9_EVENT_CALLBACKS EventsD3D9;   /**< D3D9 callbacks. */
    HYDRAHOOK_D3D10_EVENT_CALLBACKS EventsD3D10; /**< D3D10 callbacks. */
    HYDRAHOOK_D3D11_EVENT_CALLBACKS EventsD3D11; /**< D3D11 callbacks. */
    HYDRAHOOK_D3D12_EVENT_CALLBACKS EventsD3D12; /**< D3D12 callbacks. */
    HYDRAHOOK_ARC_EVENT_CALLBACKS EventsARC;    /**< Core Audio callbacks. */
    HANDLE EngineThread;                     /**< Hook worker thread. */
    HANDLE EngineCancellationEvent;           /**< Shutdown signal. */
    PVOID CustomContext;                     /**< User-allocated context. */
    BOOL CrashHandlerInstalled;              /**< TRUE if this instance enabled the crash handler. */

    union
    {
        IDXGISwapChain* pSwapChain;      /**< D3D10/11/12 swap chain. */
        LPDIRECT3DDEVICE9 pD3D9Device;   /**< D3D9 device. */
        LPDIRECT3DDEVICE9EX pD3D9ExDevice; /**< D3D9Ex device. */
    } RenderPipeline;

    struct
    {
        IAudioRenderClient *pARC;        /**< Core Audio render client. */
    } CoreAudio;

} HYDRAHOOK_ENGINE;

/** @brief Invokes EvtHydraHookGameHooked if non-NULL. */
#define INVOKE_HYDRAHOOK_GAME_HOOKED(_engine_, _version_)    \
                                    ((_engine_)->EngineConfig.EvtHydraHookGameHooked ? \
                                    (_engine_)->EngineConfig.EvtHydraHookGameHooked(_engine_, _version_) : \
                                    (void)0)

/** @brief Invokes D3D9 callback if registered. */
#define INVOKE_D3D9_CALLBACK(_engine_, _callback_, ...)     \
                            ((_engine_)->EventsD3D9._callback_ ? \
                            (_engine_)->EventsD3D9._callback_(##__VA_ARGS__) : \
                            (void)0)

/** @brief Invokes D3D10 callback if registered. */
#define INVOKE_D3D10_CALLBACK(_engine_, _callback_, ...)     \
                             ((_engine_)->EventsD3D10._callback_ ? \
                             (_engine_)->EventsD3D10._callback_(##__VA_ARGS__) : \
                             (void)0)

/** @brief Invokes D3D11 callback if registered. */
#define INVOKE_D3D11_CALLBACK(_engine_, _callback_, ...)     \
                             ((_engine_)->EventsD3D11._callback_ ? \
                             (_engine_)->EventsD3D11._callback_(##__VA_ARGS__) : \
                             (void)0)

/** @brief Invokes D3D12 callback if registered. */
#define INVOKE_D3D12_CALLBACK(_engine_, _callback_, ...)     \
                             ((_engine_)->EventsD3D12._callback_ ? \
                             (_engine_)->EventsD3D12._callback_(##__VA_ARGS__) : \
                             (void)0)

/** @brief Invokes Core Audio callback if registered. */
#define INVOKE_ARC_CALLBACK(_engine_, _callback_, ...)     \
                             ((_engine_)->EventsARC._callback_ ? \
                             (_engine_)->EventsARC._callback_(##__VA_ARGS__) : \
                             (void)0)
