#include <Windows.h>
#include <d3d11.h>

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

    Check(state->hooked != 0, "game hooked callback");
    Check(state->version == HydraHookDirect3DVersion11, "hooked version is D3D11");
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
            ExitProcess(1);
        }
        CloseHandle(unloadThread);
    }
    Check(GetModuleHandleW(L"HydraHookTestHost.dll") == nullptr, "host unloaded");

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
