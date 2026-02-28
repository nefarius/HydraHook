/**
 * @file HydraHookCore.h
 * @brief Core engine API for HydraHook - Direct3D and Core Audio hooking library.
 *
 * This header defines the main engine interface, configuration structures,
 * error codes, and lifecycle functions. Intended to be called from DllMain()
 * at DLL_PROCESS_ATTACH and DLL_PROCESS_DETACH.
 *
 * @par Windows Integration
 * Uses Microsoft SAL annotations (_In_, _Out_, etc.) for static analysis.
 * Compatible with Visual Studio IntelliSense and Doxygen documentation.
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


#ifndef HydraHookCore_h__
#define HydraHookCore_h__


#ifdef HYDRAHOOK_DYNAMIC
#ifdef HYDRAHOOK_EXPORTS
#define HYDRAHOOK_API __declspec(dllexport)
#else
#define HYDRAHOOK_API __declspec(dllimport)
#endif
#else
#define HYDRAHOOK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief Error codes returned by HydraHook engine functions.
     */
    typedef enum _HYDRAHOOK_ERRORS
    {
        HYDRAHOOK_ERROR_NONE = 0x20000000,                      /**< Success. */
        HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE = 0xE0000001,     /**< Engine handle is NULL or invalid. */
        HYDRAHOOK_ERROR_CREATE_THREAD_FAILED = 0xE0000002,      /**< CreateThread failed for hook worker. */
        HYDRAHOOK_ERROR_ENGINE_ALLOCATION_FAILED = 0xE0000003,   /**< Failed to allocate engine structure. */
        HYDRAHOOK_ERROR_ENGINE_ALREADY_ALLOCATED = 0xE0000004,  /**< Engine already created for this HMODULE. */
        HYDRAHOOK_ERROR_INVALID_HMODULE_HANDLE = 0xE0000005,    /**< HMODULE not associated with an engine. */
        HYDRAHOOK_ERROR_REFERENCE_INCREMENT_FAILED = 0xE0000006, /**< GetModuleHandleEx failed. */
        HYDRAHOOK_ERROR_CONTEXT_ALLOCATION_FAILED = 0xE0000007, /**< Custom context allocation failed. */
        HYDRAHOOK_ERROR_CREATE_EVENT_FAILED = 0xE0000008,       /**< CreateEvent failed for cancellation. */
        HYDRAHOOK_ERROR_CREATE_LOGGER_FAILED = 0xE0000009,      /**< Failed to create fallback logger. */

    } HYDRAHOOK_ERROR;

    /**
     * @brief Bitmask of detected Direct3D API versions.
     */
    typedef enum _HYDRAHOOK_D3D_VERSION {
        HydraHookDirect3DVersionUnknown = 0,  /**< No Direct3D detected. */
        HydraHookDirect3DVersion9 = 1 << 0,   /**< Direct3D 9 or 9Ex. */
        HydraHookDirect3DVersion10 = 1 << 1,  /**< Direct3D 10. */
        HydraHookDirect3DVersion11 = 1 << 2,  /**< Direct3D 11. */
        HydraHookDirect3DVersion12 = 1 << 3   /**< Direct3D 12. */
    } HYDRAHOOK_D3D_VERSION, *PHYDRAHOOK_D3D_VERSION;

    /** @brief Opaque handle to the HydraHook engine instance. */
    typedef struct _HYDRAHOOK_ENGINE *PHYDRAHOOK_ENGINE;

    typedef struct _HYDRAHOOK_D3D9_EVENT_CALLBACKS *PHYDRAHOOK_D3D9_EVENT_CALLBACKS;   /**< D3D9 callbacks. */
    typedef struct _HYDRAHOOK_D3D10_EVENT_CALLBACKS *PHYDRAHOOK_D3D10_EVENT_CALLBACKS; /**< D3D10 callbacks. */
    typedef struct _HYDRAHOOK_D3D11_EVENT_CALLBACKS *PHYDRAHOOK_D3D11_EVENT_CALLBACKS; /**< D3D11 callbacks. */
    typedef struct _HYDRAHOOK_D3D12_EVENT_CALLBACKS *PHYDRAHOOK_D3D12_EVENT_CALLBACKS; /**< D3D12 callbacks. */
    typedef struct _HYDRAHOOK_ARC_EVENT_CALLBACKS *PHYDRAHOOK_ARC_EVENT_CALLBACKS;    /**< Core Audio callbacks. */

    /**
     * @brief Extension data passed to pre-hook event callbacks (D3D11/12, Core Audio).
     */
    typedef struct _HYDRAHOOK_EVT_PRE_EXTENSION
    {
        PHYDRAHOOK_ENGINE    Engine;  /**< Engine handle for API calls. */
        PVOID               Context; /**< Custom context, or NULL if not allocated. */

    } HYDRAHOOK_EVT_PRE_EXTENSION, *PHYDRAHOOK_EVT_PRE_EXTENSION;

    /**
     * @brief Initializes a pre-extension structure for event callbacks.
     * @param[in,out] Extension The extension structure to initialize.
     * @param[in] Engine The engine handle.
     * @param[in] Context Optional custom context pointer.
     */
    VOID FORCEINLINE HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
        PHYDRAHOOK_EVT_PRE_EXTENSION Extension,
        PHYDRAHOOK_ENGINE Engine,
        PVOID Context
    )
    {
        ZeroMemory(Extension, sizeof(HYDRAHOOK_EVT_PRE_EXTENSION));

        Extension->Engine = Engine;
        Extension->Context = Context;
    }

    /**
     * @brief Extension data passed to post-hook event callbacks (D3D11/12, Core Audio).
     */
    typedef struct _HYDRAHOOK_EVT_POST_EXTENSION
    {
        PHYDRAHOOK_ENGINE    Engine;  /**< Engine handle for API calls. */
        PVOID               Context; /**< Custom context, or NULL if not allocated. */

    } HYDRAHOOK_EVT_POST_EXTENSION, *PHYDRAHOOK_EVT_POST_EXTENSION;

    /**
     * @brief Initializes a post-extension structure for event callbacks.
     * @param[in,out] Extension The extension structure to initialize.
     * @param[in] Engine The engine handle.
     * @param[in] Context Optional custom context pointer.
     */
    VOID FORCEINLINE HYDRAHOOK_EVT_POST_EXTENSION_INIT(
        PHYDRAHOOK_EVT_POST_EXTENSION Extension,
        PHYDRAHOOK_ENGINE Engine,
        PVOID Context
    )
    {
        ZeroMemory(Extension, sizeof(HYDRAHOOK_EVT_POST_EXTENSION));

        Extension->Engine = Engine;
        Extension->Context = Context;
    }

    /**
     * @brief Minidump verbosity levels for the crash handler.
     */
    typedef enum _HYDRAHOOK_DUMP_TYPE {
        HydraHookDumpTypeMinimal = 0,  /**< Threads + stacks only (small). */
        HydraHookDumpTypeNormal  = 1,  /**< + data segments, handles, unloaded modules. */
        HydraHookDumpTypeFull    = 2   /**< Full process memory (large). */
    } HYDRAHOOK_DUMP_TYPE;

    /**
     * @brief Crash handler callback invoked before a minidump is written.
     * @return TRUE to proceed with dump file creation, FALSE to skip it.
     */
    typedef
        _Function_class_(EVT_HYDRAHOOK_CRASH_HANDLER)
        BOOL
        EVT_HYDRAHOOK_CRASH_HANDLER(
            PHYDRAHOOK_ENGINE EngineHandle,
            DWORD ExceptionCode,
            struct _EXCEPTION_POINTERS* ExceptionInfo
        );

    typedef EVT_HYDRAHOOK_CRASH_HANDLER *PFN_HYDRAHOOK_CRASH_HANDLER;

    /** @brief Callback invoked when a render API has been hooked successfully. */
    typedef
        _Function_class_(EVT_HYDRAHOOK_GAME_HOOKED)
        VOID
        EVT_HYDRAHOOK_GAME_HOOKED(
            PHYDRAHOOK_ENGINE EngineHandle,
            const HYDRAHOOK_D3D_VERSION GameVersion
        );

    typedef EVT_HYDRAHOOK_GAME_HOOKED *PFN_HYDRAHOOK_GAME_HOOKED;

    /** @brief Callback invoked before or after unhooking the render API. */
    typedef
        _Function_class_(EVT_HYDRAHOOK_GAME_UNHOOKED)
        VOID
        EVT_HYDRAHOOK_GAME_UNHOOKED(
            PHYDRAHOOK_ENGINE EngineHandle
        );

    typedef EVT_HYDRAHOOK_GAME_UNHOOKED *PFN_HYDRAHOOK_GAME_UNHOOKED;

    /** @brief Callback invoked when host process shutdown is detected. */
    typedef
        _Function_class_(EVT_HYDRAHOOK_GAME_EXIT)
        VOID
        EVT_HYDRAHOOK_GAME_EXIT(
            PHYDRAHOOK_ENGINE EngineHandle
        );

    typedef EVT_HYDRAHOOK_GAME_EXIT *PFN_HYDRAHOOK_GAME_EXIT;

    /**
     * @brief Engine configuration passed to HydraHookEngineCreate.
     */
    typedef struct _HYDRAHOOK_ENGINE_CONFIG
    {
        PFN_HYDRAHOOK_GAME_HOOKED EvtHydraHookGameHooked;      /**< Invoked when render API is hooked. */
        PFN_HYDRAHOOK_GAME_UNHOOKED EvtHydraHookGamePreUnhook; /**< Invoked before unhooking. */
        PFN_HYDRAHOOK_GAME_UNHOOKED EvtHydraHookGamePostUnhook;/**< Invoked after unhooking. */
        PFN_HYDRAHOOK_GAME_EXIT EvtHydraHookGamePreExit;       /**< Invoked on process shutdown. */

        struct
        {
            BOOL HookDirect3D9;   /**< Enable Direct3D 9/9Ex hooking. */
            BOOL HookDirect3D10;  /**< Enable Direct3D 10 hooking. */
            BOOL HookDirect3D11;  /**< Enable Direct3D 11 hooking. */
            BOOL HookDirect3D12;  /**< Enable Direct3D 12 hooking. */
        } Direct3D;

        struct
        {
            BOOL HookCoreAudio;   /**< Enable Core Audio (IAudioRenderClient) hooking. */
        } CoreAudio;

        struct
        {
            BOOL IsEnabled;       /**< TRUE to enable logging. */
            PCSTR FilePath;      /**< Fallback log path (e.g. %TEMP%\\HydraHook.log); used if process/DLL dirs fail. */
        } Logging;

        struct
        {
            BOOL IsEnabled;                          /**< TRUE to enable crash handler (opt-in). */
            PCSTR DumpDirectoryPath;                 /**< Directory for dump files; NULL = use log file directory. */
            HYDRAHOOK_DUMP_TYPE DumpType;            /**< Minidump verbosity (default: HydraHookDumpTypeNormal). */
            PFN_HYDRAHOOK_CRASH_HANDLER EvtCrashHandler; /**< Optional pre-dump callback; return FALSE to skip dump. */
        } CrashHandler;

    } HYDRAHOOK_ENGINE_CONFIG, *PHYDRAHOOK_ENGINE_CONFIG;

    /**
     * Initialize an HYDRAHOOK_ENGINE_CONFIG structure with library defaults (logging enabled; fallback log path "%TEMP%\\HydraHook.log").
     * @param[in,out] EngineConfig Pointer to the HYDRAHOOK_ENGINE_CONFIG to initialize. Must point to a writable structure.
     */
    VOID FORCEINLINE HYDRAHOOK_ENGINE_CONFIG_INIT(
        PHYDRAHOOK_ENGINE_CONFIG EngineConfig
    )
    {
        ZeroMemory(EngineConfig, sizeof(HYDRAHOOK_ENGINE_CONFIG));

        EngineConfig->Logging.IsEnabled = TRUE;
        EngineConfig->Logging.FilePath = "%TEMP%\\HydraHook.log";
    }

    /**
     * @brief Creates and initializes the HydraHook engine.
     *
     * Spawns a worker thread that detects and hooks the host process's render pipeline
     * (Direct3D 9/10/11/12) and optionally Core Audio. Call from DllMain() at DLL_PROCESS_ATTACH.
     *
     * @param[in] HostInstance HMODULE of the injecting DLL (typically from DllMain).
     * @param[in] EngineConfig Configuration and event callbacks.
     * @param[out] Engine Optional; receives the engine handle on success.
     * @retval HYDRAHOOK_ERROR_NONE Success.
     * @retval HYDRAHOOK_ERROR_ENGINE_ALREADY_ALLOCATED Engine already exists for this HMODULE.
     * @retval HYDRAHOOK_ERROR_REFERENCE_INCREMENT_FAILED GetModuleHandleEx failed.
     * @retval HYDRAHOOK_ERROR_ENGINE_ALLOCATION_FAILED malloc failed.
     * @retval HYDRAHOOK_ERROR_CREATE_EVENT_FAILED CreateEvent failed.
     * @retval HYDRAHOOK_ERROR_CREATE_THREAD_FAILED CreateThread failed.
     * @retval HYDRAHOOK_ERROR_CREATE_LOGGER_FAILED Fallback logger creation failed.
     */
    HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineCreate(
        _In_ HMODULE HostInstance,
        _In_ PHYDRAHOOK_ENGINE_CONFIG EngineConfig,
        _Out_opt_ PHYDRAHOOK_ENGINE* Engine
    );

    /**
     * @brief Destroys the engine and frees all resources.
     *
     * Unhooks the render pipeline and invokes shutdown callbacks. Call from DllMain()
     * at DLL_PROCESS_DETACH.
     *
     * @param[in] HostInstance HMODULE passed to HydraHookEngineCreate.
     * @retval HYDRAHOOK_ERROR_NONE Success.
     * @retval HYDRAHOOK_ERROR_INVALID_HMODULE_HANDLE No engine for this HMODULE.
     */
    HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineDestroy(
        _In_ HMODULE HostInstance
    );

    /**
     * @brief Allocates custom context memory accessible from all event callbacks.
     *
     * Replaces any previously allocated context. Use HydraHookEngineGetCustomContext
     * from callbacks to retrieve the pointer.
     *
     * @param[in] Engine Valid engine handle.
     * @param[out] Context Receives pointer to allocated memory.
     * @param[in] ContextSize Size in bytes.
     * @retval HYDRAHOOK_ERROR_NONE Success.
     * @retval HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE Engine is NULL.
     * @retval HYDRAHOOK_ERROR_CONTEXT_ALLOCATION_FAILED malloc failed.
     */
    HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineAllocCustomContext(
        _In_
        PHYDRAHOOK_ENGINE Engine,
        _Out_
        PVOID* Context,
        _In_
        size_t ContextSize
    );

    /**
     * @brief Frees custom context memory for the engine.
     * @param[in] Engine Valid engine handle.
     * @retval HYDRAHOOK_ERROR_NONE Success.
     * @retval HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE Engine is NULL.
     */
    HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineFreeCustomContext(
        _In_
        PHYDRAHOOK_ENGINE Engine
    );

    /**
     * @brief Returns pointer to custom context memory, or NULL if none allocated.
     * @param[in] Engine Valid engine handle.
     * @return Pointer to context, or NULL.
     */
    HYDRAHOOK_API PVOID HydraHookEngineGetCustomContext(
        _In_
        PHYDRAHOOK_ENGINE Engine
    );

#ifndef HYDRAHOOK_NO_D3D9

    /**
     * @brief Registers Direct3D 9/9Ex render pipeline callbacks.
     * @param[in] Engine Valid engine handle.
     * @param[in] Callbacks Callback collection; NULL pointers are ignored.
     */
    HYDRAHOOK_API VOID HydraHookEngineSetD3D9EventCallbacks(
        _In_
        PHYDRAHOOK_ENGINE Engine,
        _In_
        PHYDRAHOOK_D3D9_EVENT_CALLBACKS Callbacks
    );

#endif

#ifndef HYDRAHOOK_NO_D3D10

    /**
     * @brief Registers Direct3D 10 render pipeline callbacks.
     * @param[in] Engine Valid engine handle.
     * @param[in] Callbacks Callback collection; NULL pointers are ignored.
     */
    HYDRAHOOK_API VOID HydraHookEngineSetD3D10EventCallbacks(
        _In_
        PHYDRAHOOK_ENGINE Engine,
        _In_
        PHYDRAHOOK_D3D10_EVENT_CALLBACKS Callbacks
    );

#endif

#ifndef HYDRAHOOK_NO_D3D11

    /**
     * @brief Registers Direct3D 11 render pipeline callbacks.
     * @param[in] Engine Valid engine handle.
     * @param[in] Callbacks Callback collection; NULL pointers are ignored.
     */
    HYDRAHOOK_API VOID HydraHookEngineSetD3D11EventCallbacks(
        _In_
        PHYDRAHOOK_ENGINE Engine,
        _In_
        PHYDRAHOOK_D3D11_EVENT_CALLBACKS Callbacks
    );

#endif

#ifndef HYDRAHOOK_NO_COREAUDIO

    /**
     * @brief Registers Core Audio (IAudioRenderClient) event callbacks.
     * @param[in] Engine Valid engine handle.
     * @param[in] Callbacks Callback collection; NULL pointers are ignored.
     */
    HYDRAHOOK_API VOID HydraHookEngineSetARCEventCallbacks(
        _In_
        PHYDRAHOOK_ENGINE Engine,
        _In_
        PHYDRAHOOK_ARC_EVENT_CALLBACKS Callbacks
    );

#endif

    /**
     * @brief Logs a debug message (printf-style format).
     * @param[in] Format printf format string.
     * @param[in] ... Format arguments.
     */
    HYDRAHOOK_API VOID HydraHookEngineLogDebug(
        _In_
        LPCSTR Format,
        _In_opt_
        ...
    );

    /**
     * @brief Logs an informational message (printf-style format).
     * @param[in] Format printf format string.
     * @param[in] ... Format arguments.
     */
    HYDRAHOOK_API VOID HydraHookEngineLogInfo(
        _In_
        LPCSTR Format,
        _In_opt_
        ...
    );

    /**
     * @brief Logs a warning message (printf-style format).
     * @param[in] Format printf format string.
     * @param[in] ... Format arguments.
     */
    HYDRAHOOK_API VOID HydraHookEngineLogWarning(
        _In_
        LPCSTR Format,
        _In_opt_
        ...
    );

    /**
     * @brief Logs an error message (printf-style format).
     * @param[in] Format printf format string.
     * @param[in] ... Format arguments.
     */
    HYDRAHOOK_API VOID HydraHookEngineLogError(
        _In_
        LPCSTR Format,
        _In_opt_
        ...
    );

#ifdef __cplusplus
}
#endif

#endif // HydraHookCore_h__