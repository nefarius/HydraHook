#include <Windows.h>

#include "HydraHook/Engine/HydraHookCore.h"
#include "HydraHook/Engine/HydraHookDirect3D11.h"

extern "C" HYDRAHOOK_D3D_VERSION HydraHookEngineGameVersionForTest(PHYDRAHOOK_ENGINE engine);

struct HydraHookTestState
{
    volatile LONG hooked;
    volatile LONG version;
    volatile LONG presentPre;
    volatile LONG presentPost;
    volatile LONG resizePre;
    volatile LONG resizePost;
    volatile LONG preUnhook;
    volatile LONG postUnhook;
};

static HydraHookTestState g_state = {};
static PHYDRAHOOK_ENGINE g_engine = nullptr;

extern "C" __declspec(dllexport) HydraHookTestState* HydraHookTest_GetState(void)
{
    return &g_state;
}

extern "C" __declspec(dllexport) HYDRAHOOK_ERROR HydraHookTest_Unhook(void)
{
    return HydraHookEngineUnhook(g_engine);
}

extern "C" __declspec(dllexport) HYDRAHOOK_ERROR HydraHookTest_Rehook(void)
{
    return HydraHookEngineRehook(g_engine);
}

extern "C" __declspec(dllexport) HYDRAHOOK_D3D_VERSION HydraHookTest_GameVersion(void)
{
    return HydraHookEngineGameVersionForTest(g_engine);
}

static VOID EvtGameHooked(PHYDRAHOOK_ENGINE, const HYDRAHOOK_D3D_VERSION version)
{
    InterlockedExchange(&g_state.version, static_cast<LONG>(version));
    InterlockedIncrement(&g_state.hooked);
}

static VOID EvtPreUnhook(PHYDRAHOOK_ENGINE)
{
    InterlockedIncrement(&g_state.preUnhook);
}

static VOID EvtPostUnhook(PHYDRAHOOK_ENGINE)
{
    InterlockedIncrement(&g_state.postUnhook);
}

static VOID EvtPrePresent(IDXGISwapChain*, UINT, UINT, PHYDRAHOOK_EVT_PRE_EXTENSION)
{
    InterlockedIncrement(&g_state.presentPre);
}

static VOID EvtPostPresent(IDXGISwapChain*, UINT, UINT, PHYDRAHOOK_EVT_POST_EXTENSION)
{
    InterlockedIncrement(&g_state.presentPost);
}

static VOID EvtPreResizeBuffers(
    IDXGISwapChain*,
    UINT,
    UINT,
    UINT,
    DXGI_FORMAT,
    UINT,
    PHYDRAHOOK_EVT_PRE_EXTENSION)
{
    InterlockedIncrement(&g_state.resizePre);
}

static VOID EvtPostResizeBuffers(
    IDXGISwapChain*,
    UINT,
    UINT,
    UINT,
    DXGI_FORMAT,
    UINT,
    PHYDRAHOOK_EVT_POST_EXTENSION)
{
    InterlockedIncrement(&g_state.resizePost);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(instance);
        {
            HYDRAHOOK_ENGINE_CONFIG config;
            HYDRAHOOK_ENGINE_CONFIG_INIT(&config);
            config.Logging.IsEnabled = TRUE;
            config.CrashHandler.IsEnabled = FALSE;
            config.Direct3D.HookDirect3D10 = TRUE;
            config.Direct3D.HookDirect3D11 = TRUE;
            config.Direct3D.HookDirect3D12 = TRUE;
            config.EvtHydraHookGameHooked = EvtGameHooked;
            config.EvtHydraHookGamePreUnhook = EvtPreUnhook;
            config.EvtHydraHookGamePostUnhook = EvtPostUnhook;

            PHYDRAHOOK_ENGINE engine = nullptr;
            if (HydraHookEngineCreate(instance, &config, &engine) == HYDRAHOOK_ERROR_NONE && engine != nullptr)
            {
                g_engine = engine;
                HYDRAHOOK_D3D11_EVENT_CALLBACKS callbacks;
                HYDRAHOOK_D3D11_EVENT_CALLBACKS_INIT(&callbacks);
                callbacks.EvtHydraHookD3D11PrePresent = EvtPrePresent;
                callbacks.EvtHydraHookD3D11PostPresent = EvtPostPresent;
                callbacks.EvtHydraHookD3D11PreResizeBuffers = EvtPreResizeBuffers;
                callbacks.EvtHydraHookD3D11PostResizeBuffers = EvtPostResizeBuffers;
                HydraHookEngineSetD3D11EventCallbacks(engine, &callbacks);
            }
        }
        break;
    case DLL_PROCESS_DETACH:
        HydraHookEngineDestroy(instance);
        break;
    default:
        break;
    }

    return TRUE;
}
