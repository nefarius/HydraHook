#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <HydraHook/Engine/HydraHookCore.h>

#include <cstdio>
#include <cstring>

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

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;

        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    LRESULT CALLBACK SmokeWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    HMODULE LoadHost()
    {
        wchar_t path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return nullptr;

        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash == nullptr)
            return nullptr;

        if (wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"HydraHookTestHost.dll") != 0)
            return nullptr;

        return LoadLibraryW(path);
    }

    DWORD WINAPI UnloadHost(LPVOID parameter)
    {
        FreeLibrary(static_cast<HMODULE>(parameter));
        return 0;
    }
}

int main()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = SmokeWndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"HydraHookWarpSmoke";
    if (RegisterClassW(&windowClass) == 0)
    {
        std::fprintf(stderr, "FAIL: RegisterClassW (%lu)\n", GetLastError());
        return 1;
    }

    HWND window = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"HydraHookWarpSmoke",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        64,
        64,
        nullptr,
        nullptr,
        instance,
        nullptr);
    Check(window != nullptr, "hidden window");
    if (window == nullptr)
        return 1;

    // Load d3d12.dll alongside d3d11.dll before the host initializes so the
    // engine probes both APIs against the shared DXGI swap chain vtable.
    const HMODULE d3d12Module = LoadLibraryW(L"d3d12.dll");

    const HMODULE host = LoadHost();
    Check(host != nullptr, "HydraHookTestHost.dll loaded");
    if (host == nullptr)
    {
        std::fprintf(stderr, "LoadLibrary error %lu\n", GetLastError());
        DestroyWindow(window);
        return 1;
    }

    using GetStateFn = HydraHookTestState* (*)();
    auto* getState = reinterpret_cast<GetStateFn>(GetProcAddress(host, "HydraHookTest_GetState"));
    Check(getState != nullptr, "HydraHookTest_GetState exported");
    HydraHookTestState* state = getState != nullptr ? getState() : nullptr;
    Check(state != nullptr, "test state");
    if (state == nullptr)
    {
        FreeLibrary(host);
        DestroyWindow(window);
        return 1;
    }

    DXGI_SWAP_CHAIN_DESC description = {};
    description.BufferCount = 1;
    description.BufferDesc.Width = 64;
    description.BufferDesc.Height = 64;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    const HRESULT created = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &description,
        &swapChain,
        &device,
        nullptr,
        &context);
    Check(SUCCEEDED(created), "WARP device and swap chain");
    if (FAILED(created))
        std::fprintf(stderr, "D3D11CreateDeviceAndSwapChain HRESULT 0x%08lX\n", static_cast<unsigned long>(created));

    const ULONGLONG deadline = GetTickCount64() + 20000;
    HRESULT presentResult = E_FAIL;
    if (swapChain != nullptr)
    {
        while (GetTickCount64() < deadline && state->hooked == 0)
        {
            presentResult = swapChain->Present(0, 0);
            Sleep(50);
        }
    }

    Check(state->hooked == 1, "game hooked callback");
    Check(state->version == HydraHookDirect3DVersion11, "hooked version is D3D11");
    using GameVersionFn = HYDRAHOOK_D3D_VERSION (*)();
    auto* gameVersion = reinterpret_cast<GameVersionFn>(GetProcAddress(host, "HydraHookTest_GameVersion"));
    Check(gameVersion != nullptr && gameVersion() == HydraHookDirect3DVersion11, "engine GameVersion is D3D11");
    if (swapChain != nullptr)
    {
        IDXGISwapChain1* swapChain1 = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain1))) && swapChain1 != nullptr)
        {
            DXGI_PRESENT_PARAMETERS presentParameters{};
            const LONG hookedBeforePresent1 = state->hooked;
            const HRESULT present1 = swapChain1->Present1(0, 0, &presentParameters);
            Check(SUCCEEDED(present1), "Present1");
            Check(state->hooked == hookedBeforePresent1, "Present1 does not announce a second hook");
            swapChain1->Release();
        }
    }
    Check(state->presentPre > 0, "pre-Present fired");
    Check(state->presentPost > 0, "post-Present fired");

    if (swapChain != nullptr)
    {
        while (GetTickCount64() < deadline && (state->resizePre == 0 || state->resizePost == 0))
        {
            const HRESULT resized = swapChain->ResizeBuffers(0, 64, 64, DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(resized))
            {
                std::fprintf(stderr, "ResizeBuffers HRESULT 0x%08lX\n", static_cast<unsigned long>(resized));
                Check(false, "ResizeBuffers");
                break;
            }
            if (state->resizePre == 0 || state->resizePost == 0)
                Sleep(50);
        }
    }
    Check(state->resizePre > 0, "pre-ResizeBuffers fired");
    Check(state->resizePost > 0, "post-ResizeBuffers fired");

    // Full hook, unhook, rehook cycle in the same loaded DLL
    using LifecycleFn = HYDRAHOOK_ERROR (*)();
    auto* unhook = reinterpret_cast<LifecycleFn>(GetProcAddress(host, "HydraHookTest_Unhook"));
    auto* rehook = reinterpret_cast<LifecycleFn>(GetProcAddress(host, "HydraHookTest_Rehook"));
    Check(unhook != nullptr && rehook != nullptr, "unhook/rehook exported");
    if (swapChain != nullptr && unhook != nullptr && rehook != nullptr)
    {
        Check(unhook() == HYDRAHOOK_ERROR_NONE, "unhook succeeds");
        Check(state->preUnhook == 1, "pre-unhook fired once");
        Check(state->postUnhook == 1, "post-unhook fired once");
        Check(GetModuleHandleW(L"HydraHookTestHost.dll") == host, "host stays loaded after unhook");
        Check(unhook() == HYDRAHOOK_ERROR_NONE, "second unhook is a no-op");
        Check(state->preUnhook == 1, "second unhook does not repeat callbacks");

        const LONG presentPreBefore = state->presentPre;
        const LONG presentPostBefore = state->presentPost;
        const LONG resizePreBefore = state->resizePre;
        const LONG resizePostBefore = state->resizePost;
        for (int i = 0; i < 5; ++i)
            Check(SUCCEEDED(swapChain->Present(0, 0)), "Present while unhooked");
        Check(SUCCEEDED(swapChain->ResizeBuffers(0, 64, 64, DXGI_FORMAT_UNKNOWN, 0)), "ResizeBuffers while unhooked");
        Check(state->presentPre == presentPreBefore && state->presentPost == presentPostBefore,
              "no Present callbacks while unhooked");
        Check(state->resizePre == resizePreBefore && state->resizePost == resizePostBefore,
              "no ResizeBuffers callbacks while unhooked");

        Check(rehook() == HYDRAHOOK_ERROR_NONE, "rehook succeeds");
        const ULONGLONG rehookDeadline = GetTickCount64() + 20000;
        while (GetTickCount64() < rehookDeadline && state->hooked < 2)
        {
            swapChain->Present(0, 0);
            Sleep(50);
        }
        Check(state->hooked == 2, "game hooked callback fires again after rehook");
        Check(state->version == HydraHookDirect3DVersion11, "rehooked version is D3D11");
        Check(state->presentPre > presentPreBefore, "pre-Present fires after rehook");
        Check(state->presentPost > presentPostBefore, "post-Present fires after rehook");

        while (GetTickCount64() < rehookDeadline
            && (state->resizePre == resizePreBefore || state->resizePost == resizePostBefore))
        {
            if (FAILED(swapChain->ResizeBuffers(0, 64, 64, DXGI_FORMAT_UNKNOWN, 0)))
            {
                Check(false, "ResizeBuffers after rehook");
                break;
            }
            Sleep(50);
        }
        Check(state->resizePre > resizePreBefore, "pre-ResizeBuffers fires after rehook");
        Check(state->resizePost > resizePostBefore, "post-ResizeBuffers fires after rehook");
    }

    if (swapChain != nullptr)
        swapChain->Release();
    if (context != nullptr)
        context->Release();
    if (device != nullptr)
        device->Release();
    DestroyWindow(window);

    const HANDLE unloadThread = CreateThread(nullptr, 0, UnloadHost, host, 0, nullptr);
    Check(unloadThread != nullptr, "unload thread");
    if (unloadThread != nullptr)
    {
        const DWORD waited = WaitForSingleObject(unloadThread, 20000);
        if (waited != WAIT_OBJECT_0)
        {
            std::fprintf(stderr, "FreeLibrary wait timed out (WaitForSingleObject returned %lu)\n",
                         static_cast<unsigned long>(waited));
            TerminateProcess(GetCurrentProcess(), 1);
        }
        CloseHandle(unloadThread);
    }
    Check(GetModuleHandleW(L"HydraHookTestHost.dll") == nullptr, "host unloaded");

    if (d3d12Module != nullptr)
        FreeLibrary(d3d12Module);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d smoke assertion(s) failed (last Present 0x%08lX)\n",
                     g_failures,
                     static_cast<unsigned long>(presentResult));
        return 1;
    }

    std::puts("HydraHookWarpSmokeTests passed");
    return 0;
}
