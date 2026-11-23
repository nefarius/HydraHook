#include <Windows.h>

#include "HydraHook/Engine/HydraHookCore.h"
#include "HydraHook/Engine/HydraHookCoreAudio.h"
#include "HydraHook/Engine/HydraHookDirect3D9.h"
#include "HydraHook/Engine/HydraHookDirect3D10.h"
#include "HydraHook/Engine/HydraHookDirect3D11.h"
#include "HydraHook/Engine/HydraHookDirect3D12.h"

#include "CrashHandler.h"
#include "Engine.h"
#include "Exceptions.hpp"
#include "LdrLock.h"
#include "Utils/Global.h"
#include "Utils/Hook.h"
#include "FrameRate.h"
#include "InitGuard.h"
#include "OverlayRelease.h"
#include "WindowProcHook.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* expression, const char* file, int line)
    {
        if (condition)
            return;

        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        ++g_failures;
    }

#define CHECK_ZERO_INIT(type, init)                                                                 \
    do                                                                                              \
    {                                                                                               \
        type value;                                                                                 \
        std::memset(&value, 0xAB, sizeof(value));                                                   \
        init(&value);                                                                               \
        type zero;                                                                                  \
        std::memset(&zero, 0, sizeof(zero));                                                        \
        Check(std::memcmp(&value, &zero, sizeof(type)) == 0, #init " zeroes the structure",         \
              __FILE__, __LINE__);                                                                  \
    } while (0)

    PHYDRAHOOK_ENGINE MakeBareEngine()
    {
        auto* engine = static_cast<PHYDRAHOOK_ENGINE>(std::malloc(sizeof(HYDRAHOOK_ENGINE)));
        if (!engine)
            return nullptr;

        std::memset(engine, 0, sizeof(HYDRAHOOK_ENGINE));
        engine->ShutdownCleanupDone.store(false);
        engine->FreeLibraryHookActive.store(false);
        return engine;
    }

    __declspec(noinline) int __stdcall StdTarget(int value)
    {
        volatile int sink = value;
        sink += 3;
        sink += 5;
        sink += 7;
        return sink;
    }

    __declspec(noinline) int __cdecl CdeclTarget(int value)
    {
        volatile int sink = value;
        sink += 11;
        sink += 13;
        return sink;
    }

    Hook<CallConvention::stdcall_t, int, int>* g_stdHook = nullptr;
    Hook<CallConvention::cdecl_t, int, int>* g_cdeclHook = nullptr;

    __declspec(noinline) int __stdcall StdDetour(int value)
    {
        return g_stdHook->call_orig(value) + 10;
    }

    __declspec(noinline) int __cdecl CdeclDetour(int value)
    {
        return g_cdeclHook->call_orig(value) + 20;
    }

    void TestInitHelpers()
    {
        HYDRAHOOK_ENGINE_CONFIG config;
        std::memset(&config, 0xAB, sizeof(config));
        HYDRAHOOK_ENGINE_CONFIG_INIT(&config);
        Check(config.Logging.IsEnabled == TRUE, "logging enabled by default", __FILE__, __LINE__);
        Check(config.Logging.FilePath != nullptr &&
              std::strcmp(config.Logging.FilePath, "%TEMP%\\HydraHook.log") == 0,
              "default log path", __FILE__, __LINE__);
        Check(config.CrashHandler.IsEnabled == FALSE, "crash handler opt-in", __FILE__, __LINE__);
        Check(config.CrashHandler.DumpDirectoryPath == nullptr, "dump path default", __FILE__, __LINE__);
        Check(config.CrashHandler.DumpType == HydraHookDumpTypeNormal, "dump type default", __FILE__, __LINE__);
        Check(config.CrashHandler.EvtCrashHandler == nullptr, "crash callback default", __FILE__, __LINE__);
        Check(config.Direct3D.HookDirect3D9 == FALSE, "d3d9 default off", __FILE__, __LINE__);
        Check(config.Direct3D.HookDirect3D10 == FALSE, "d3d10 default off", __FILE__, __LINE__);
        Check(config.Direct3D.HookDirect3D11 == FALSE, "d3d11 default off", __FILE__, __LINE__);
        Check(config.Direct3D.HookDirect3D12 == FALSE, "d3d12 default off", __FILE__, __LINE__);
        Check(config.EvtHydraHookGameHooked == nullptr, "game hooked default", __FILE__, __LINE__);

        auto* engine = reinterpret_cast<PHYDRAHOOK_ENGINE>(static_cast<uintptr_t>(0x10));
        auto* context = reinterpret_cast<void*>(static_cast<uintptr_t>(0x20));

        HYDRAHOOK_EVT_PRE_EXTENSION pre;
        std::memset(&pre, 0xAB, sizeof(pre));
        HYDRAHOOK_EVT_PRE_EXTENSION_INIT(&pre, engine, context);
        Check(pre.Engine == engine && pre.Context == context, "pre extension init", __FILE__, __LINE__);

        HYDRAHOOK_EVT_POST_EXTENSION post;
        std::memset(&post, 0xAB, sizeof(post));
        HYDRAHOOK_EVT_POST_EXTENSION_INIT(&post, engine, context);
        Check(post.Engine == engine && post.Context == context, "post extension init", __FILE__, __LINE__);

        CHECK_ZERO_INIT(HYDRAHOOK_D3D9_EVENT_CALLBACKS, HYDRAHOOK_D3D9_EVENT_CALLBACKS_INIT);
        CHECK_ZERO_INIT(HYDRAHOOK_D3D10_EVENT_CALLBACKS, HYDRAHOOK_D3D10_EVENT_CALLBACKS_INIT);
        CHECK_ZERO_INIT(HYDRAHOOK_D3D11_EVENT_CALLBACKS, HYDRAHOOK_D3D11_EVENT_CALLBACKS_INIT);
        CHECK_ZERO_INIT(HYDRAHOOK_D3D12_EVENT_CALLBACKS, HYDRAHOOK_D3D12_EVENT_CALLBACKS_INIT);
        CHECK_ZERO_INIT(HYDRAHOOK_ARC_EVENT_CALLBACKS, HYDRAHOOK_ARC_EVENT_CALLBACKS_INIT);
    }

    void TestEngineHandles()
    {
        HYDRAHOOK_ENGINE_CONFIG config;
        HYDRAHOOK_ENGINE_CONFIG_INIT(&config);
        config.Logging.IsEnabled = FALSE;

        Check(HydraHookEngineCreate(nullptr, &config, nullptr) == HYDRAHOOK_ERROR_GET_MODULE_HANDLE_FAILED,
              "create rejects null module", __FILE__, __LINE__);
        Check(HydraHookEngineCreate(reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x1)), &config, nullptr) ==
              HYDRAHOOK_ERROR_GET_MODULE_HANDLE_FAILED,
              "create rejects unknown module", __FILE__, __LINE__);
        Check(HydraHookEngineDestroy(nullptr) == HYDRAHOOK_ERROR_INVALID_HMODULE_HANDLE,
              "destroy rejects null module", __FILE__, __LINE__);
        Check(HydraHookEngineDestroy(reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x1))) ==
              HYDRAHOOK_ERROR_INVALID_HMODULE_HANDLE,
              "destroy rejects unknown module", __FILE__, __LINE__);

        Check(HydraHookEngineAllocCustomContext(nullptr, nullptr, 8) == HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE,
              "alloc rejects null engine", __FILE__, __LINE__);
        Check(HydraHookEngineFreeCustomContext(nullptr) == HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE,
              "free rejects null engine", __FILE__, __LINE__);
        Check(HydraHookEngineGetCustomContext(nullptr) == nullptr, "get rejects null engine", __FILE__, __LINE__);

        PHYDRAHOOK_ENGINE engine = MakeBareEngine();
        Check(engine != nullptr, "bare engine allocated", __FILE__, __LINE__);
        if (!engine)
            return;

        void* first = nullptr;
        Check(HydraHookEngineAllocCustomContext(engine, &first, 16) == HYDRAHOOK_ERROR_NONE,
              "alloc context", __FILE__, __LINE__);
        Check(first != nullptr && first == HydraHookEngineGetCustomContext(engine), "context stored", __FILE__, __LINE__);
        auto* bytes = static_cast<unsigned char*>(first);
        bool zeroed = true;
        for (int i = 0; i < 16; ++i)
            zeroed = zeroed && bytes[i] == 0;
        Check(zeroed, "context zero-initialized", __FILE__, __LINE__);
        bytes[0] = 0x5A;

        void* second = nullptr;
        Check(HydraHookEngineAllocCustomContext(engine, &second, 8) == HYDRAHOOK_ERROR_NONE,
              "replacement alloc", __FILE__, __LINE__);
        Check(second != nullptr && second == HydraHookEngineGetCustomContext(engine),
              "replacement stored", __FILE__, __LINE__);
        Check(static_cast<unsigned char*>(second)[0] == 0, "replacement zero-initialized", __FILE__, __LINE__);

        Check(HydraHookEngineFreeCustomContext(engine) == HYDRAHOOK_ERROR_NONE, "free context", __FILE__, __LINE__);
        Check(HydraHookEngineGetCustomContext(engine) == nullptr, "context cleared", __FILE__, __LINE__);
        Check(HydraHookEngineFreeCustomContext(engine) == HYDRAHOOK_ERROR_NONE, "free is idempotent", __FILE__, __LINE__);

        HYDRAHOOK_D3D9_EVENT_CALLBACKS d3d9;
        HYDRAHOOK_D3D9_EVENT_CALLBACKS_INIT(&d3d9);
        HYDRAHOOK_D3D10_EVENT_CALLBACKS d3d10;
        HYDRAHOOK_D3D10_EVENT_CALLBACKS_INIT(&d3d10);
        HYDRAHOOK_D3D11_EVENT_CALLBACKS d3d11;
        HYDRAHOOK_D3D11_EVENT_CALLBACKS_INIT(&d3d11);
        HYDRAHOOK_D3D12_EVENT_CALLBACKS d3d12;
        HYDRAHOOK_D3D12_EVENT_CALLBACKS_INIT(&d3d12);
        HYDRAHOOK_ARC_EVENT_CALLBACKS arc;
        HYDRAHOOK_ARC_EVENT_CALLBACKS_INIT(&arc);

        HydraHookEngineSetD3D9EventCallbacks(nullptr, &d3d9);
        HydraHookEngineSetD3D10EventCallbacks(nullptr, &d3d10);
        HydraHookEngineSetD3D11EventCallbacks(nullptr, &d3d11);
        HydraHookEngineSetD3D12EventCallbacks(nullptr, &d3d12);
        HydraHookEngineSetARCEventCallbacks(nullptr, &arc);

        HydraHookEngineSetD3D11EventCallbacks(engine, &d3d11);
        Check(engine->EventsD3D11.EvtHydraHookD3D11PrePresent == nullptr, "null callback stored", __FILE__, __LINE__);

        Check(HydraHookEngineGetHandleFromD3D9Device(nullptr) == nullptr, "null d3d9 device", __FILE__, __LINE__);
        Check(HydraHookEngineGetHandleFromD3D9ExDevice(nullptr) == nullptr, "null d3d9ex device", __FILE__, __LINE__);
        Check(HydraHookEngineGetD3D12CommandQueue(nullptr) == nullptr, "null d3d12 queue", __FILE__, __LINE__);

        HydraHookEngineLogDebug("unit-test %d", 1);
        HydraHookEngineLogInfo("unit-test %d", 2);
        HydraHookEngineLogWarning("unit-test %d", 3);
        HydraHookEngineLogError("unit-test %d", 4);

        std::free(engine);
    }

    void TestHooks()
    {
        const int stdOriginal = StdTarget(4);
        {
            Hook<CallConvention::stdcall_t, int, int> hook;
            g_stdHook = &hook;
            Check(!hook.is_applied(), "stdcall hook starts unapplied", __FILE__, __LINE__);
            hook.apply(reinterpret_cast<size_t>(&StdTarget), &StdDetour);
            Check(hook.is_applied(), "stdcall hook applied", __FILE__, __LINE__);
            Check(StdTarget(4) == stdOriginal + 10, "stdcall detour invoked", __FILE__, __LINE__);
            Check(hook.call_orig(4) == stdOriginal, "stdcall call_orig", __FILE__, __LINE__);
            Check(hook.remove_nothrow(), "stdcall remove_nothrow", __FILE__, __LINE__);
            Check(!hook.is_applied(), "stdcall hook removed", __FILE__, __LINE__);
            Check(hook.remove_nothrow(), "stdcall remove_nothrow is idempotent", __FILE__, __LINE__);
            Check(StdTarget(4) == stdOriginal, "stdcall target restored", __FILE__, __LINE__);
            g_stdHook = nullptr;
        }

        const int cdeclOriginal = CdeclTarget(4);
        {
            Hook<CallConvention::cdecl_t, int, int> hook;
            g_cdeclHook = &hook;
            hook.apply(reinterpret_cast<size_t>(&CdeclTarget), &CdeclDetour);
            Check(hook.is_applied(), "cdecl hook applied", __FILE__, __LINE__);
            Check(CdeclTarget(4) == cdeclOriginal + 20, "cdecl detour invoked", __FILE__, __LINE__);
            Check(hook.call_orig(4) == cdeclOriginal, "cdecl call_orig", __FILE__, __LINE__);
            hook.remove();
            Check(!hook.is_applied(), "cdecl hook removed", __FILE__, __LINE__);
            g_cdeclHook = nullptr;
        }

        {
            Hook<CallConvention::stdcall_t, int, int> hook;
            g_stdHook = &hook;
            hook.apply(reinterpret_cast<size_t>(&StdTarget), &StdDetour);
            Check(StdTarget(1) == StdTarget(1), "detour stable", __FILE__, __LINE__);
            g_stdHook = nullptr;
        }
        Check(StdTarget(4) == stdOriginal, "RAII teardown restored stdcall target", __FILE__, __LINE__);
    }

    void TestActivityTracker()
    {
        Check(HookActivityTracker::s_active.load() == 0, "tracker starts idle", __FILE__, __LINE__);
        Check(HookActivityTracker::drain(0), "drain is immediate when idle", __FILE__, __LINE__);

        {
            HookActivityTracker::Guard outer;
            Check(outer.invoke, "guard invokes before shutdown", __FILE__, __LINE__);
            Check(HookActivityTracker::s_active.load() == 1, "one guard held", __FILE__, __LINE__);
            {
                HookActivityTracker::Guard inner;
                Check(HookActivityTracker::s_active.load() == 2, "nested guards", __FILE__, __LINE__);
            }
            Check(HookActivityTracker::s_active.load() == 1, "inner guard released", __FILE__, __LINE__);
        }
        Check(HookActivityTracker::s_active.load() == 0, "guards released", __FILE__, __LINE__);

        std::atomic<bool> release{false};
        std::atomic<bool> entered{false};
        std::thread held([&]()
        {
            HookActivityTracker::Guard guard;
            entered.store(true);
            while (!release.load())
                SwitchToThread();
        });

        while (!entered.load())
            SwitchToThread();

        Check(!HookActivityTracker::drain(50), "drain times out while a guard is held", __FILE__, __LINE__);
        release.store(true);
        held.join();

        std::atomic<bool> waiting{false};
        std::thread worker([&]()
        {
            HookActivityTracker::Guard guard;
            waiting.store(true);
            Sleep(200);
        });
        while (!waiting.load())
            SwitchToThread();
        Check(HookActivityTracker::drain(2000), "drain waits for the worker thread", __FILE__, __LINE__);
        worker.join();
        Check(HookActivityTracker::s_active.load() == 0, "worker released the guard", __FILE__, __LINE__);
    }

    void TestUtilities()
    {
        const std::string expanded = HydraHook::Core::Util::expand_environment_variables("%SystemRoot%");
        Check(expanded.find("Windows") != std::string::npos || expanded.find("WINDOWS") != std::string::npos,
              "SystemRoot expanded", __FILE__, __LINE__);

        const std::string processDir = HydraHook::Core::Util::get_process_directory();
        Check(!processDir.empty(), "process directory", __FILE__, __LINE__);
        Check(processDir.back() == '\\' || processDir.back() == '/', "process directory separator", __FILE__, __LINE__);

        const std::string moduleDir = HydraHook::Core::Util::get_module_directory(GetModuleHandleA("kernel32.dll"));
        Check(!moduleDir.empty(), "module directory", __FILE__, __LINE__);
        Check(moduleDir.back() == '\\' || moduleDir.back() == '/', "module directory separator", __FILE__, __LINE__);

        const std::string image = HydraHook::Core::Util::process_name();
        Check(!image.empty(), "process name", __FILE__, __LINE__);

        Check(!HydraHook::Core::Util::IsLoaderLockHeld(), "loader lock not held", __FILE__, __LINE__);
    }

    void TestExceptions()
    {
        const HydraHook::Core::Exceptions::DXAPIException dx("device failed", E_FAIL);
        Check(dx.hresult() == E_FAIL, "HRESULT preserved", __FILE__, __LINE__);
        Check(std::strstr(dx.what(), "device failed") != nullptr, "DXAPI message preserved", __FILE__, __LINE__);

        const HydraHook::Core::Exceptions::SehException seh(EXCEPTION_ACCESS_VIOLATION, nullptr);
        Check(seh.code() == EXCEPTION_ACCESS_VIOLATION, "SEH code preserved", __FILE__, __LINE__);
        Check(seh.exception_pointers() == nullptr, "SEH pointers preserved", __FILE__, __LINE__);
    }

    void TestCrashHandler()
    {
        PHYDRAHOOK_ENGINE engine = MakeBareEngine();
        Check(engine != nullptr, "crash-handler engine", __FILE__, __LINE__);
        if (!engine)
            return;

        HydraHookCrashHandlerInstall(nullptr);
        HydraHookCrashHandlerUninstall(engine);

        HydraHookCrashHandlerInstall(engine);
        HydraHookCrashHandlerInstall(engine);
        HydraHookCrashHandlerUninstall(engine);
        HydraHookCrashHandlerUninstall(engine);
        HydraHookCrashHandlerUninstall(engine);

        HydraHookCrashHandlerInstall(engine);
        HydraHookCrashHandlerUninstall(engine);

        HydraHookCrashHandlerInstallThreadSEH();
        bool caught = false;
        try
        {
            RaiseException(0xE0001234, 0, 0, nullptr);
        }
        catch (const HydraHook::Core::Exceptions::SehException& ex)
        {
            caught = ex.code() == 0xE0001234;
        }
        Check(caught, "SEH translator raises SehException", __FILE__, __LINE__);

        std::free(engine);
    }

    void TestFrameRate()
    {
        FrameRate frameRate;
        const auto start = FrameRate::Clock::time_point{};

        Check(frameRate.Sample(start), "first sample warms up", __FILE__, __LINE__);
        Check(frameRate.AverageMilliseconds() == 0.0, "warm-up has no average", __FILE__, __LINE__);
        Check(!frameRate.Sample(start + std::chrono::milliseconds(100)), "inside the window", __FILE__, __LINE__);
        Check(!frameRate.Sample(start + std::chrono::milliseconds(499)), "just under 500 ms", __FILE__, __LINE__);
        Check(frameRate.Sample(start + std::chrono::milliseconds(500)), "500 ms window elapsed", __FILE__, __LINE__);
        Check(frameRate.AverageMilliseconds() > 166.0 && frameRate.AverageMilliseconds() < 167.0,
              "average is elapsed/intervals", __FILE__, __LINE__);

        Check(frameRate.Sample(start + std::chrono::milliseconds(500) + std::chrono::seconds(2)),
              "two second pause resets", __FILE__, __LINE__);
        Check(frameRate.AverageMilliseconds() == 0.0, "reset clears the average", __FILE__, __LINE__);
    }

    void TestResettableInitGuard()
    {
        {
            ResettableInitGuard guard;
            int calls = 0;
            auto init = [&]() { ++calls; return true; };
            Check(guard.EnsureInitialized(init), "init succeeds", __FILE__, __LINE__);
            Check(guard.EnsureInitialized(init), "init stays latched", __FILE__, __LINE__);
            Check(calls == 1, "init runs once", __FILE__, __LINE__);
            guard.Reset();
            Check(guard.EnsureInitialized(init), "init runs after reset", __FILE__, __LINE__);
            Check(calls == 2, "reset allows re-init", __FILE__, __LINE__);
        }

        {
            ResettableInitGuard guard;
            int calls = 0;
            bool succeed = false;
            auto init = [&]() { ++calls; return succeed; };
            Check(!guard.EnsureInitialized(init), "failure reported", __FILE__, __LINE__);
            succeed = true;
            Check(!guard.EnsureInitialized(init), "failure latched like call_once", __FILE__, __LINE__);
            Check(calls == 1, "no retry without reset", __FILE__, __LINE__);
            guard.Reset();
            Check(guard.EnsureInitialized(init), "success after reset", __FILE__, __LINE__);
            Check(calls == 2, "reset re-executed init", __FILE__, __LINE__);
        }

        {
            ResettableInitGuard guard;
            int calls = 0;
            int failuresLeft = 2;
            auto init = [&]() { ++calls; return failuresLeft-- <= 0; };
            Check(!guard.EnsureInitialized(init, true), "retry mode fails 1", __FILE__, __LINE__);
            Check(!guard.EnsureInitialized(init, true), "retry mode fails 2", __FILE__, __LINE__);
            Check(guard.EnsureInitialized(init, true), "retry mode succeeds", __FILE__, __LINE__);
            Check(guard.EnsureInitialized(init, true), "retry mode latches", __FILE__, __LINE__);
            Check(calls == 3, "retry mode ran until success", __FILE__, __LINE__);
            guard.Reset();
            failuresLeft = 0;
            Check(guard.EnsureInitialized(init, true), "retry mode re-inits after reset", __FILE__, __LINE__);
            Check(calls == 4, "reset re-executed retry-mode init", __FILE__, __LINE__);
        }

        {
            ResettableInitGuard guard;
            std::atomic<int> calls{0};
            std::atomic<bool> go{false};
            std::thread threads[8];
            for (auto& t : threads)
            {
                t = std::thread([&]()
                {
                    while (!go.load())
                        SwitchToThread();
                    guard.EnsureInitialized([&]() { ++calls; Sleep(20); return true; });
                });
            }
            go.store(true);
            for (auto& t : threads)
                t.join();
            Check(calls.load() == 1, "concurrent callers init exactly once", __FILE__, __LINE__);
        }
    }

    std::atomic<int> g_origMessages{0};
    std::atomic<int> g_origDestroy{0};
    std::atomic<int> g_origNcDestroy{0};
    std::atomic<int> g_handlerCalls{0};
    std::atomic<int> g_errorCalls{0};
    std::atomic<LRESULT> g_reentrantResult{-1};
    std::atomic<int> g_beforeDispatchPhase{0};

    void TestBeforeDispatch(UINT msg)
    {
        if (msg != WM_APP + 1 || g_beforeDispatchPhase.load() != 1)
            return;
        g_beforeDispatchPhase.store(2);
        while (g_beforeDispatchPhase.load() == 2)
            SwitchToThread();
    }

    LRESULT CALLBACK TestOrigProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        ++g_origMessages;
        switch (msg)
        {
        case WM_APP + 1:
            return 0x1234;
        case WM_APP + 9:
            DestroyWindow(hWnd);
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            ++g_origDestroy;
            break;
        case WM_NCDESTROY:
            ++g_origNcDestroy;
            break;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    LRESULT CALLBACK TestOuterProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    WNDPROC g_outerPrevious = nullptr;

    LRESULT CALLBACK TestOuterProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        return CallWindowProcW(g_outerPrevious, hWnd, msg, wParam, lParam);
    }

    LRESULT TestHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        (void)wParam;
        (void)lParam;
        ++g_handlerCalls;
        if (msg == WM_APP + 2)
            return 0x77;
        if (msg == WM_APP + 3)
            g_reentrantResult = SendMessageW(hWnd, WM_APP + 1, 0, 0);
        return 0;
    }

    void TestErrorLogger(const char*)
    {
        ++g_errorCalls;
    }

    HWND MakeTestWindow()
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = TestOrigProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"HydraHookWndProcTest";
            RegisterClassW(&wc);
            registered = true;
        }
        return CreateWindowExW(0, L"HydraHookWndProcTest", L"t", WS_OVERLAPPED, 0, 0, 16, 16,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }

    void TestWindowProcHook()
    {
        auto& hook = WindowProcHook::Instance();
        hook.SetErrorLogger(TestErrorLogger);
        hook.SetMessageHandler(TestHandler);

        // Input validation
        Check(!hook.Hook(nullptr), "hook rejects null window", __FILE__, __LINE__);
        Check(g_errorCalls.load() == 1, "null window logged", __FILE__, __LINE__);
        HWND dead = MakeTestWindow();
        DestroyWindow(dead);
        Check(!hook.Hook(dead), "hook rejects destroyed window", __FILE__, __LINE__);
        Check(g_errorCalls.load() == 2, "destroyed window logged", __FILE__, __LINE__);

        // Interception and forwarding
        HWND w1 = MakeTestWindow();
        Check(w1 != nullptr, "test window created", __FILE__, __LINE__);
        Check(hook.Hook(w1), "hook window", __FILE__, __LINE__);
        Check(hook.Hook(w1), "duplicate hook is idempotent", __FILE__, __LINE__);
        Check(hook.HookedCount() == 1 && hook.IsHooked(w1), "single tracked window", __FILE__, __LINE__);
        Check(GetWindowLongPtrW(w1, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&TestOrigProc),
              "dispatcher installed", __FILE__, __LINE__);

        g_handlerCalls = 0;
        g_origMessages = 0;
        Check(SendMessageW(w1, WM_APP + 1, 0, 0) == 0x1234, "unhandled message forwarded to original", __FILE__, __LINE__);
        Check(g_handlerCalls.load() == 1 && g_origMessages.load() == 1, "handler then original", __FILE__, __LINE__);
        g_origMessages = 0;
        Check(SendMessageW(w1, WM_APP + 2, 0, 0) == 0x77, "handler result short-circuits", __FILE__, __LINE__);
        Check(g_origMessages.load() == 0, "original skipped when handled", __FILE__, __LINE__);

        // Re-entrant SendMessage from the handler
        g_reentrantResult = -1;
        SendMessageW(w1, WM_APP + 3, 0, 0);
        Check(g_reentrantResult.load() == 0x1234, "re-entrant send does not deadlock", __FILE__, __LINE__);

        // Independent windows, selective unhook
        HWND w2 = MakeTestWindow();
        Check(hook.Hook(w2), "hook second window", __FILE__, __LINE__);
        Check(hook.HookedCount() == 2, "two tracked windows", __FILE__, __LINE__);
        Check(hook.Unhook(w1), "unhook restores", __FILE__, __LINE__);
        Check(GetWindowLongPtrW(w1, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&TestOrigProc),
              "original restored", __FILE__, __LINE__);
        Check(!hook.IsHooked(w1) && hook.IsHooked(w2), "second window untouched", __FILE__, __LINE__);
        g_handlerCalls = 0;
        SendMessageW(w1, WM_APP + 1, 0, 0);
        Check(g_handlerCalls.load() == 0, "unhooked window bypasses handler", __FILE__, __LINE__);

        // Destroy messages reach the original and clean up
        g_origDestroy = 0;
        g_origNcDestroy = 0;
        DestroyWindow(w2);
        Check(g_origDestroy.load() == 1 && g_origNcDestroy.load() == 1, "destroy messages forwarded", __FILE__, __LINE__);
        Check(hook.HookedCount() == 0, "no active hook after destroy", __FILE__, __LINE__);
        DestroyWindow(w1);

        // Re-hook after UnhookAll
        HWND w3 = MakeTestWindow();
        Check(hook.Hook(w3), "hook before UnhookAll", __FILE__, __LINE__);
        hook.UnhookAll();
        Check(hook.HookedCount() == 0, "UnhookAll leaves no active hook", __FILE__, __LINE__);
        Check(GetWindowLongPtrW(w3, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&TestOrigProc),
              "UnhookAll restores", __FILE__, __LINE__);
        Check(hook.Hook(w3), "re-hook after UnhookAll", __FILE__, __LINE__);
        g_handlerCalls = 0;
        SendMessageW(w3, WM_APP + 1, 0, 0);
        Check(g_handlerCalls.load() == 1, "handler active after re-hook", __FILE__, __LINE__);
        hook.UnhookAll();
        DestroyWindow(w3);

        // Another party subclasses after us: do not clobber it
        HWND w4 = MakeTestWindow();
        Check(hook.Hook(w4), "hook before external subclass", __FILE__, __LINE__);
        g_outerPrevious = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(w4, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&TestOuterProc)));
        Check(!hook.Unhook(w4), "unhook refuses to drop outer subclass", __FILE__, __LINE__);
        Check(GetWindowLongPtrW(w4, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&TestOuterProc),
              "outer subclass kept", __FILE__, __LINE__);
        g_handlerCalls = 0;
        Check(SendMessageW(w4, WM_APP + 1, 0, 0) == 0x1234, "pass-through still reaches original", __FILE__, __LINE__);
        Check(g_handlerCalls.load() == 0, "pass-through skips handler", __FILE__, __LINE__);
        g_origDestroy = 0;
        g_origNcDestroy = 0;
        DestroyWindow(w4);
        Check(g_origDestroy.load() == 1 && g_origNcDestroy.load() == 1, "chained destroy forwarded", __FILE__, __LINE__);
        Check(hook.HookedCount() == 0, "no active hook after chained destroy", __FILE__, __LINE__);

        // Unhook while DispatchProc is entered but has not incremented its in-flight count.
        // The original procedure must still be invoked. DefWindowProcW returns 0 for WM_APP.
        {
            std::atomic<HWND> shared{nullptr};
            std::atomic<int> ownerReady{0};
            std::thread owner([&]()
            {
                HWND hw = MakeTestWindow();
                if (!hw)
                {
                    ownerReady.store(-1);
                    return;
                }
                shared.store(hw);
                ownerReady.store(1);
                MSG m;
                while (GetMessageW(&m, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
            });
            while (ownerReady.load() == 0)
                SwitchToThread();
            if (ownerReady.load() < 0)
            {
                owner.join();
                Check(false, "preamble owner window created", __FILE__, __LINE__);
            }
            else
            {
                HWND hw = shared.load();
                const bool hooked = hook.Hook(hw);
                Check(hooked, "hook before preamble unhook", __FILE__, __LINE__);
                if (!hooked)
                {
                    SendMessageW(hw, WM_APP + 9, 0, 0);
                    owner.join();
                }
                else
                {
                    hook.SetBeforeDispatch(TestBeforeDispatch);
                    g_beforeDispatchPhase.store(0);
                    g_origMessages = 0;
                    std::atomic<LRESULT> delivered{-1};
                    std::thread sender([&]()
                    {
                        g_beforeDispatchPhase.store(1);
                        delivered.store(SendMessageW(hw, WM_APP + 1, 0, 0));
                    });
                    while (g_beforeDispatchPhase.load() != 2)
                        SwitchToThread();
                    Check(hook.Unhook(hw), "unhook during dispatch preamble", __FILE__, __LINE__);
                    g_beforeDispatchPhase.store(3);
                    sender.join();
                    hook.SetBeforeDispatch(nullptr);
                    Check(delivered.load() == 0x1234, "preamble dispatch still reaches the original", __FILE__, __LINE__);
                    Check(g_origMessages.load() >= 1, "original procedure ran after preamble unhook", __FILE__, __LINE__);
                    SendMessageW(hw, WM_APP + 9, 0, 0);
                    owner.join();
                }
            }
        }

        // Cross-thread: the owner thread pumps while this thread hooks, sends, and unhooks.
        // Readiness is separate from the HWND so a failed CreateWindow cannot spin forever.
        std::atomic<HWND> shared{nullptr};
        std::atomic<int> ownerReady{0};
        std::thread owner([&]()
        {
            HWND hw = MakeTestWindow();
            if (!hw)
            {
                ownerReady.store(-1);
                return;
            }
            shared.store(hw);
            ownerReady.store(1);
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        });
        while (ownerReady.load() == 0)
            SwitchToThread();
        if (ownerReady.load() < 0)
        {
            owner.join();
            Check(false, "owner window created", __FILE__, __LINE__);
        }
        else
        {
        HWND shw = shared.load();

        std::atomic<bool> stop{false};
        std::atomic<int> sent{0};
        std::thread sender([&]()
        {
            while (!stop.load())
            {
                if (SendMessageW(shw, WM_APP + 1, 0, 0) == 0x1234)
                    ++sent;
            }
        });
        int installs = 0;
        int observed = 0;
        for (int i = 0; i < 100; ++i)
        {
            if (!hook.Hook(shw))
            {
                hook.UnhookAll();
                continue;
            }
            ++installs;
            const int before = g_handlerCalls.load();
            const LRESULT delivered = SendMessageW(shw, WM_APP + 1, 0, 0);
            if (delivered == 0x1234 && g_handlerCalls.load() > before)
                ++observed;
            hook.UnhookAll();
        }
        stop.store(true);
        sender.join();
        Check(installs > 0, "at least one cross-thread hook installed", __FILE__, __LINE__);
        Check(observed > 0, "handler saw a message while the hook was installed", __FILE__, __LINE__);
        Check(sent.load() > 0, "messages delivered during hook churn", __FILE__, __LINE__);
        Check(GetWindowLongPtrW(shw, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&TestOrigProc),
              "original restored after churn", __FILE__, __LINE__);
        SendMessageW(shw, WM_APP + 9, 0, 0);
        owner.join();
        Check(hook.HookedCount() == 0, "no active hooks remain", __FILE__, __LINE__);
        }

        hook.SetMessageHandler(nullptr);
        hook.SetErrorLogger(nullptr);
        hook.SetBeforeDispatch(nullptr);
    }

    void TestOverlayReleaseClaim()
    {
        std::atomic<uint32_t> current{1};
        std::atomic<uint32_t> applied{0};

        Check(ClaimOverlayRelease(current, applied, 1, false), "first release claims the hide", __FILE__, __LINE__);
        // Reopening does not change the generation. A delayed duplicate must not claim again.
        Check(!ClaimOverlayRelease(current, applied, 1, false), "duplicate release after reopen is ignored", __FILE__, __LINE__);

        current.store(2);
        Check(!ClaimOverlayRelease(current, applied, 1, false), "stale hide is ignored", __FILE__, __LINE__);
        Check(ClaimOverlayRelease(current, applied, 2, false), "newer hide claims", __FILE__, __LINE__);
        Check(!ClaimOverlayRelease(current, applied, 2, false), "newer hide is also idempotent", __FILE__, __LINE__);

        Check(ClaimOverlayRelease(current, applied, 0, true), "unhook force still runs", __FILE__, __LINE__);
        Check(!ClaimOverlayRelease(current, applied, 2, false), "force consumes the current generation", __FILE__, __LINE__);

        std::mutex gate;
        std::atomic<bool> pending{true};
        {
            std::lock_guard<std::mutex> lock(gate);
            current.store(3);
            pending.store(true);
        }
        {
            std::lock_guard<std::mutex> lock(gate);
            Check(!ClearPendingIfCurrent(current, pending, 2), "older post does not clear a newer hide", __FILE__, __LINE__);
        }
        Check(pending.load(), "newer hide stays pending", __FILE__, __LINE__);
        {
            std::lock_guard<std::mutex> lock(gate);
            Check(ClearPendingIfCurrent(current, pending, 3), "matching post clears pending", __FILE__, __LINE__);
        }
        Check(!pending.load(), "matching generation consumed the request", __FILE__, __LINE__);
    }

    void TestShutdownGuard()
    {
        HookActivityTracker::shutdown();
        {
            HookActivityTracker::Guard guard;
            Check(!guard.invoke, "shutdown suppresses guard.invoke", __FILE__, __LINE__);
        }

        HookActivityTracker::reset();
        HookActivityTracker::Guard rearmed;
        Check(rearmed.invoke, "reset re-enables guard.invoke", __FILE__, __LINE__);
    }

    void TestResettableOnce()
    {
        ResettableOnce once;
        int calls = 0;
        once.call([&]() { ++calls; });
        once.call([&]() { ++calls; });
        Check(calls == 1, "announcement runs once", __FILE__, __LINE__);
        once.reset();
        once.call([&]() { ++calls; });
        Check(calls == 2, "reset re-arms the announcement", __FILE__, __LINE__);

        ResettableOnce shared;
        std::atomic<int> concurrent{0};
        std::atomic<bool> go{false};
        std::thread threads[8];
        for (auto& t : threads)
        {
            t = std::thread([&]()
            {
                while (!go.load())
                    SwitchToThread();
                shared.call([&]() { ++concurrent; Sleep(20); });
            });
        }
        go.store(true);
        for (auto& t : threads)
            t.join();
        Check(concurrent.load() == 1, "concurrent callers announce exactly once", __FILE__, __LINE__);
    }

    void TestHookReapply()
    {
        const int original = StdTarget(4);
        Hook<CallConvention::stdcall_t, int, int> hook;
        g_stdHook = &hook;
        for (int cycle = 0; cycle < 2; ++cycle)
        {
            hook.apply(reinterpret_cast<size_t>(&StdTarget), &StdDetour);
            Check(StdTarget(4) == original + 10, "detour active on each cycle", __FILE__, __LINE__);
            Check(hook.remove_nothrow(), "remove_nothrow on each cycle", __FILE__, __LINE__);
            Check(StdTarget(4) == original, "target restored on each cycle", __FILE__, __LINE__);
        }
        g_stdHook = nullptr;
    }

    void TestUnhookRehookHandles()
    {
        Check(HydraHookEngineUnhook(nullptr) == HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE,
              "unhook rejects null engine", __FILE__, __LINE__);
        Check(HydraHookEngineRehook(nullptr) == HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE,
              "rehook rejects null engine", __FILE__, __LINE__);
        Check(HYDRAHOOK_ERROR_UNHOOK_INCOMPLETE != HYDRAHOOK_ERROR_NONE
              && HYDRAHOOK_ERROR_UNHOOK_INCOMPLETE != HYDRAHOOK_ERROR_NO_LOADER_LOCK,
              "unhook incomplete has a distinct error code", __FILE__, __LINE__);

        PHYDRAHOOK_ENGINE engine = MakeBareEngine();
        Check(engine != nullptr, "unhook engine allocated", __FILE__, __LINE__);
        if (!engine)
            return;

        Check(HydraHookEngineUnhook(engine) == HYDRAHOOK_ERROR_NONE,
              "unhook without a worker is a no-op", __FILE__, __LINE__);

        HYDRAHOOK_D3D11_EVENT_CALLBACKS d3d11;
        HYDRAHOOK_D3D11_EVENT_CALLBACKS_INIT(&d3d11);
        d3d11.EvtHydraHookD3D11PrePresent = reinterpret_cast<PFN_HYDRAHOOK_D3D11_PRE_PRESENT>(
            static_cast<uintptr_t>(0x10));
        HydraHookEngineSetD3D11EventCallbacks(engine, &d3d11);
        Check(engine->SavedEventsD3D11.EvtHydraHookD3D11PrePresent == d3d11.EvtHydraHookD3D11PrePresent,
              "registered callbacks are saved for rehook", __FILE__, __LINE__);

        std::free(engine);
    }
}

int main()
{
    TestInitHelpers();
    TestEngineHandles();
    TestHooks();
    TestActivityTracker();
    TestUtilities();
    TestExceptions();
    TestCrashHandler();
    TestFrameRate();
    TestResettableInitGuard();
    TestWindowProcHook();
    TestOverlayReleaseClaim();
    TestShutdownGuard();
    TestResettableOnce();
    TestHookReapply();
    TestUnhookRehookHandles();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
        return 1;
    }

    std::puts("HydraHookTests passed");
    return 0;
}
