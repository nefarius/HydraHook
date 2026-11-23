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
#include "Utils/Global.h"
#include "Utils/Hook.h"
#include "FpsConfig.h"
#include "FrameRate.h"
#include "InitGuard.h"
#include "KeyToggle.h"
#include "OverlayRelease.h"
#include "WindowProcHook.h"

#include <atomic>
#include <new>
#include <chrono>
#include <clocale>
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
        return new (std::nothrow) HYDRAHOOK_ENGINE();
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

        Check(HydraHookEngineCreate(GetModuleHandleA(nullptr), nullptr, nullptr) ==
              HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE,
              "create rejects null config", __FILE__, __LINE__);
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
        PHYDRAHOOK_ENGINE nullContextEngine = MakeBareEngine();
        Check(nullContextEngine != nullptr
              && HydraHookEngineAllocCustomContext(nullContextEngine, nullptr, 8) == HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE,
              "alloc rejects null context", __FILE__, __LINE__);
        delete nullContextEngine;
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
        using SetD3D12Callbacks = void (*)(PHYDRAHOOK_ENGINE, PHYDRAHOOK_D3D12_EVENT_CALLBACKS);
        using GetD3D12Queue = ID3D12CommandQueue* (*)(IDXGISwapChain*);
        const SetD3D12Callbacks setCallbacks = &HydraHookEngineSetD3D12EventCallbacks;
        const GetD3D12Queue getQueue = &HydraHookEngineGetD3D12CommandQueue;
        Check(setCallbacks != nullptr && getQueue != nullptr, "D3D12 exports have C linkage", __FILE__, __LINE__);

        HydraHookEngineLogDebug("unit-test %d", 1);
        HydraHookEngineLogInfo("unit-test %d", 2);
        HydraHookEngineLogWarning("unit-test %d", 3);
        HydraHookEngineLogError("unit-test %d", 4);

        delete engine;

        const std::string logPath = HydraHook::Core::Util::get_process_directory() + "HydraHook.log";
        WIN32_FILE_ATTRIBUTE_DATA before{};
        const BOOL existed = GetFileAttributesExA(logPath.c_str(), GetFileExInfoStandard, &before);

        HYDRAHOOK_ENGINE_CONFIG quiet;
        HYDRAHOOK_ENGINE_CONFIG_INIT(&quiet);
        quiet.Logging.IsEnabled = FALSE;
        quiet.Logging.FilePath = nullptr;
        PHYDRAHOOK_ENGINE created = nullptr;
        const HYDRAHOOK_ERROR createdError = HydraHookEngineCreate(GetModuleHandleA(nullptr), &quiet, &created);
        Check(createdError == HYDRAHOOK_ERROR_NONE, "create with logging disabled", __FILE__, __LINE__);
        if (createdError == HYDRAHOOK_ERROR_NONE)
        {
            std::string longMessage(300, 'A');
            HydraHookEngineLogInfo("%s", longMessage.c_str());
            Check(HydraHookEngineUnhook(created) == HYDRAHOOK_ERROR_NONE, "unhook disabled logger", __FILE__, __LINE__);
            Check(HydraHookEngineDestroy(GetModuleHandleA(nullptr)) == HYDRAHOOK_ERROR_NONE,
                  "destroy disabled logger", __FILE__, __LINE__);
            HookActivityTracker::reset();
        }

        PHYDRAHOOK_ENGINE live = nullptr;
        HYDRAHOOK_ENGINE_CONFIG liveConfig;
        HYDRAHOOK_ENGINE_CONFIG_INIT(&liveConfig);
        liveConfig.Logging.IsEnabled = FALSE;
        const HMODULE self = GetModuleHandleA(nullptr);
        Check(HydraHookEngineCreate(self, &liveConfig, &live) == HYDRAHOOK_ERROR_NONE,
              "create before the worker exits", __FILE__, __LINE__);
        if (live != nullptr)
        {
            const HYDRAHOOK_ERROR firstDestroy = HydraHookEngineDestroy(self);
            if (firstDestroy == HYDRAHOOK_ERROR_UNHOOK_INCOMPLETE)
            {
                Check(HydraHookEngineUnhook(live) == HYDRAHOOK_ERROR_NONE,
                      "unhook engine retained by destroy", __FILE__, __LINE__);
                Check(HydraHookEngineDestroy(self) == HYDRAHOOK_ERROR_NONE,
                      "destroy after the retained worker exits", __FILE__, __LINE__);
            }
            else
            {
                Check(firstDestroy == HYDRAHOOK_ERROR_NONE,
                      "destroy of a worker that had already exited", __FILE__, __LINE__);
            }
            HookActivityTracker::reset();
        }

        WIN32_FILE_ATTRIBUTE_DATA after{};
        const BOOL existsAfter = GetFileAttributesExA(logPath.c_str(), GetFileExInfoStandard, &after);
        if (!existed)
        {
            Check(!existsAfter, "disabled logging creates no log file", __FILE__, __LINE__);
        }
        else
        {
            Check(existsAfter
                  && CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0,
                  "disabled logging does not rewrite the log file", __FILE__, __LINE__);
        }
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
            bool doubleApplyThrew = false;
            try
            {
                hook.apply(reinterpret_cast<size_t>(&StdTarget), &StdDetour);
            }
            catch (const HydraHook::Core::Exceptions::DetourException&)
            {
                doubleApplyThrew = true;
            }
            Check(doubleApplyThrew, "second apply is rejected", __FILE__, __LINE__);
            Check(hook.is_applied(), "rejected apply leaves the hook applied", __FILE__, __LINE__);
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
            hook.remove();
            Check(!hook.is_applied(), "remove on an unapplied hook is a no-op", __FILE__, __LINE__);
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

        {
            using StdHook = Hook<CallConvention::stdcall_t, int, int>;
            alignas(StdHook) unsigned char storage[sizeof(StdHook)];
            auto* hook = new (storage) StdHook();
            g_stdHook = hook;
            hook->apply(reinterpret_cast<size_t>(&StdTarget), &StdDetour);
            g_hookDestructorDetachSuppressed = true;
            hook->~StdHook();
            Check(StdTarget(4) == stdOriginal + 10, "suppressed destructor leaves the detour", __FILE__, __LINE__);
            g_hookDestructorDetachSuppressed = false;
            hook->remove();
            Check(StdTarget(4) == stdOriginal, "explicit remove clears a suppressed detour", __FILE__, __LINE__);
            new (storage) StdHook();
            hook->~StdHook();
            g_stdHook = nullptr;
        }
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
        Check(!expanded.empty() && expanded.back() != '\0', "expanded string has no trailing NUL", __FILE__, __LINE__);
        Check(expanded.find('\0') == std::string::npos, "expanded string size excludes the NUL", __FILE__, __LINE__);

        const std::string processDir = HydraHook::Core::Util::get_process_directory();
        Check(!processDir.empty(), "process directory", __FILE__, __LINE__);
        Check(processDir.back() == '\\' || processDir.back() == '/', "process directory separator", __FILE__, __LINE__);

        const std::string moduleDir = HydraHook::Core::Util::get_module_directory(GetModuleHandleA("kernel32.dll"));
        Check(!moduleDir.empty(), "module directory", __FILE__, __LINE__);
        Check(moduleDir.back() == '\\' || moduleDir.back() == '/', "module directory separator", __FILE__, __LINE__);

        const std::string image = HydraHook::Core::Util::process_name();
        Check(!image.empty(), "process name", __FILE__, __LINE__);
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
        Check(HydraHookCrashHandlerActiveOwnerForTest() == engine, "first install owns the snapshot", __FILE__, __LINE__);
        HydraHookCrashHandlerUninstall(engine);
        Check(HydraHookCrashHandlerActiveOwnerForTest() == engine, "second registration is promoted", __FILE__, __LINE__);
        HydraHookCrashHandlerUninstall(engine);
        Check(HydraHookCrashHandlerActiveOwnerForTest() == nullptr, "last uninstall clears the snapshot", __FILE__, __LINE__);
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

        PHYDRAHOOK_ENGINE other = MakeBareEngine();
        Check(other != nullptr, "second crash-handler engine", __FILE__, __LINE__);
        if (other)
        {
            HydraHookCrashHandlerInstall(engine);
            HydraHookCrashHandlerUninstall(other);
            Check(HydraHookCrashHandlerActiveOwnerForTest() == engine, "unregistered uninstall keeps the owner", __FILE__, __LINE__);
            HydraHookCrashHandlerInstall(other);
            Check(HydraHookCrashHandlerActiveOwnerForTest() == engine, "first engine stays active", __FILE__, __LINE__);
            HydraHookCrashHandlerUninstall(engine);
            Check(HydraHookCrashHandlerActiveOwnerForTest() == other, "surviving engine is promoted", __FILE__, __LINE__);
            HydraHookCrashHandlerUninstall(other);
            Check(HydraHookCrashHandlerActiveOwnerForTest() == nullptr, "final uninstall clears the snapshot", __FILE__, __LINE__);
            delete other;
        }

        delete engine;
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

    void TestKeyToggle()
    {
        KeyToggle key;
        Check(!key.Update(false), "idle key does not toggle", __FILE__, __LINE__);
        Check(key.Update(true), "press toggles", __FILE__, __LINE__);
        Check(!key.Update(true), "held key does not repeat", __FILE__, __LINE__);
        Check(!key.Update(false), "release does not toggle", __FILE__, __LINE__);
        Check(key.Update(true), "next press toggles again", __FILE__, __LINE__);
        key.Reset();
        Check(key.Update(true), "press after reset toggles again", __FILE__, __LINE__);

        // Two chords on one primary key. Each detector sees the same snapshot.
        KeyToggle plain;
        KeyToggle shifted;
        Check(plain.Update(true, 0, 0), "F12 press toggles the plain binding", __FILE__, __LINE__);
        Check(!shifted.Update(true, 0, kKeyModShift), "F12 press does not toggle Shift+F12", __FILE__, __LINE__);
        Check(!plain.Update(true, 0, 0), "held F12 does not repeat", __FILE__, __LINE__);
        Check(!shifted.Update(true, 0, kKeyModShift), "held F12 still does not match Shift+F12", __FILE__, __LINE__);
        Check(!plain.Update(false, 0, 0), "F12 release does not toggle", __FILE__, __LINE__);
        Check(!shifted.Update(false, 0, kKeyModShift), "F12 release does not toggle the chord", __FILE__, __LINE__);

        Check(!plain.Update(true, kKeyModShift, 0), "Shift+F12 does not toggle the plain binding", __FILE__, __LINE__);
        Check(shifted.Update(true, kKeyModShift, kKeyModShift), "Shift+F12 toggles the chord", __FILE__, __LINE__);
        Check(!plain.Update(true, 0, 0), "releasing Shift while F12 is held does not toggle F12", __FILE__, __LINE__);
        Check(!shifted.Update(true, 0, kKeyModShift), "releasing Shift while F12 is held does not toggle Shift+F12", __FILE__, __LINE__);
        Check(!plain.Update(false, 0, 0), "F12 release after Shift does not toggle", __FILE__, __LINE__);
        Check(!shifted.Update(false, 0, kKeyModShift), "F12 release after Shift does not toggle the chord", __FILE__, __LINE__);

        plain.Reset();
        shifted.Reset();
        Check(plain.Update(true, 0, 0), "F12 press after reset toggles the plain binding", __FILE__, __LINE__);
        Check(!shifted.Update(true, 0, kKeyModShift), "F12 press after reset does not arm Shift+F12", __FILE__, __LINE__);
        Check(!plain.Update(true, kKeyModShift, 0), "adding Shift does not toggle F12 again", __FILE__, __LINE__);
        Check(!shifted.Update(true, kKeyModShift, kKeyModShift), "adding Shift after F12 is down does not toggle Shift+F12", __FILE__, __LINE__);

        KeyToggle exact;
        Check(!exact.Update(true, kKeyModCtrl, 0), "Ctrl+F12 does not match F12", __FILE__, __LINE__);
        Check(!exact.Update(true, 0, 0), "releasing Ctrl while F12 is held does not toggle F12", __FILE__, __LINE__);
        exact.Reset();
        Check(exact.Update(true, 0, 0), "F12 toggles after the extra modifier is forgotten", __FILE__, __LINE__);
    }

    void TestFpsConfig()
    {
        // Defaults and round trip.
        FpsConfig defaults;
        Check(defaults.corner == FpsCorner::TopLeft && defaults.scalePercent == 100 && defaults.showCounter,
              "defaults", __FILE__, __LINE__);
        Check(defaults.counterKey == kDefaultCounterKey && defaults.counterKey.vk == VK_F12 && defaults.counterKey.modifiers == 0,
              "counter key defaults to F12", __FILE__, __LINE__);
        Check(defaults.menuKey == kDefaultMenuKey && defaults.menuKey.vk == VK_F12 && defaults.menuKey.modifiers == kKeyModShift,
              "menu key defaults to Shift+F12", __FILE__, __LINE__);

        FpsConfig written;
        written.corner = FpsCorner::BottomRight;
        written.scalePercent = 175;
        written.showCounter = false;
        written.counterKey = { kKeyModCtrl, static_cast<uint8_t>('P') };
        written.menuKey = { static_cast<uint8_t>(kKeyModAlt | kKeyModShift), VK_F1 };
        char text[512];
        const size_t length = FormatFpsConfig(written, text, sizeof(text));
        Check(length > 0, "format fits the buffer", __FILE__, __LINE__);
        char tiny[8];
        Check(FormatFpsConfig(written, tiny, sizeof(tiny)) == 0, "format reports a short buffer", __FILE__, __LINE__);
        FpsConfig parsed;
        ParseFpsConfig(text, length, parsed);
        Check(parsed == written, "format and parse round trip", __FILE__, __LINE__);

        // Locale changes must not alter the format.
        const std::string previousLocale = std::setlocale(LC_ALL, nullptr);
        std::setlocale(LC_ALL, "de-DE");
        char localized[512];
        const size_t localizedLength = FormatFpsConfig(written, localized, sizeof(localized));
        std::setlocale(LC_ALL, previousLocale.c_str());
        Check(localizedLength == length && std::memcmp(localized, text, length) == 0,
              "format ignores the C locale", __FILE__, __LINE__);

        // Tolerant parsing: comments, sections, whitespace, case, CRLF, unknown keys.
        const char messy[] = "; c\n[Section]\n  corner =  topright \r\n# x\nUnknown=1\nSCALEPERCENT=9999\nShowCounter=0\ncounterkey = shift + f12\nMenuKey=None";
        FpsConfig messyConfig;
        ParseFpsConfig(messy, sizeof(messy) - 1, messyConfig);
        Check(messyConfig.corner == FpsCorner::TopRight, "corner parsed case-insensitively", __FILE__, __LINE__);
        Check(messyConfig.scalePercent == kFpsMaxScalePercent, "scale clamped high", __FILE__, __LINE__);
        Check(!messyConfig.showCounter, "ShowCounter parsed before the hotkey lines", __FILE__, __LINE__);
        Check(messyConfig.counterKey == kDefaultMenuKey, "counter chord parsed with spaces", __FILE__, __LINE__);
        Check(messyConfig.menuKey.vk == 0 && messyConfig.menuKey.modifiers == 0, "None disables a shortcut", __FILE__, __LINE__);

        const char bad[] = "Corner=Middle\nScalePercent=abc\nShowCounter=\nScalePercent=1\nCounterKey=Shift+None\nMenuKey=0x10\nCounterKey=Ctrl+\n";
        FpsConfig badConfig;
        ParseFpsConfig(bad, sizeof(bad) - 1, badConfig);
        Check(badConfig.corner == FpsCorner::TopLeft && badConfig.showCounter, "bad values keep defaults", __FILE__, __LINE__);
        Check(badConfig.scalePercent == kFpsMinScalePercent, "scale clamped low", __FILE__, __LINE__);
        Check(badConfig.counterKey == kDefaultCounterKey && badConfig.menuKey == kDefaultMenuKey,
              "invalid hotkeys keep defaults", __FILE__, __LINE__);

        struct HotkeySample
        {
            const char* text;
            uint8_t modifiers;
            uint8_t vk;
            const char* canonical;
        };
        const HotkeySample samples[] = {
            { "F12", 0, VK_F12, "F12" },
            { "shift+f12", kKeyModShift, VK_F12, "Shift+F12" },
            { "  Ctrl + Alt + Shift + F24 ", static_cast<uint8_t>(kKeyModCtrl | kKeyModAlt | kKeyModShift), VK_F24, "Ctrl+Alt+Shift+F24" },
            { "control+a", kKeyModCtrl, 'A', "Ctrl+A" },
            { "Numpad5", 0, VK_NUMPAD5, "Num5" },
            { "Num0", 0, VK_NUMPAD0, "Num0" },
            { "0xba", 0, 0xBA, "0xBA" },
            { "0x0D", 0, VK_RETURN, "Enter" },
            { "None", 0, 0, "None" },
            { "Space", 0, VK_SPACE, "Space" },
            { "Delete", 0, VK_DELETE, "Delete" },
            { "Left", 0, VK_LEFT, "Left" },
            { "Shift+Ctrl+F1", static_cast<uint8_t>(kKeyModCtrl | kKeyModShift), VK_F1, "Ctrl+Shift+F1" },
        };
        for (const HotkeySample& sample : samples)
        {
            FpsHotkey hotkey;
            Check(ParseFpsHotkey(sample.text, sample.text + std::strlen(sample.text), hotkey), sample.text, __FILE__, __LINE__);
            Check(hotkey.modifiers == sample.modifiers && hotkey.vk == sample.vk, "parsed hotkey fields", __FILE__, __LINE__);
            char canonical[40];
            const size_t canonicalLength = FormatFpsHotkey(hotkey, canonical, sizeof(canonical));
            Check(canonicalLength == std::strlen(sample.canonical) && std::strcmp(canonical, sample.canonical) == 0,
                  "hotkey formats canonically", __FILE__, __LINE__);
        }

        const char* rejected[] = {
            "", "Shift+None", "F25", "F0", "0x00", "0x10", "0xA0", "0xFF", "0x100", "Middle", "Ctrl+", "+F12",
            "Shift+Shift+F12", "F12+Shift", "None+F12", "Ctrl+Control+A", "  "
        };
        for (const char* rejectedText : rejected)
        {
            FpsHotkey rejectedKey = kDefaultCounterKey;
            Check(!ParseFpsHotkey(rejectedText, rejectedText + std::strlen(rejectedText), rejectedKey), rejectedText, __FILE__, __LINE__);
            Check(rejectedKey == kDefaultCounterKey, "rejected hotkey leaves the output unchanged", __FILE__, __LINE__);
        }

        char tinyKey[4];
        Check(FormatFpsHotkey(kDefaultMenuKey, tinyKey, sizeof(tinyKey)) == 0, "hotkey format reports a short buffer", __FILE__, __LINE__);
        const std::string hotkeyLocale = std::setlocale(LC_ALL, nullptr);
        std::setlocale(LC_ALL, "de-DE");
        char localizedKey[40];
        const size_t localizedKeyLength = FormatFpsHotkey(written.menuKey, localizedKey, sizeof(localizedKey));
        std::setlocale(LC_ALL, hotkeyLocale.c_str());
        char stableKey[40];
        const size_t stableKeyLength = FormatFpsHotkey(written.menuKey, stableKey, sizeof(stableKey));
        Check(localizedKeyLength == stableKeyLength && localizedKeyLength > 0
              && std::memcmp(localizedKey, stableKey, localizedKeyLength) == 0,
              "hotkey format ignores the C locale", __FILE__, __LINE__);

        const char onlyCounter[] = "CounterKey=Ctrl+P\n";
        FpsConfig onlyCounterConfig;
        ParseFpsConfig(onlyCounter, sizeof(onlyCounter) - 1, onlyCounterConfig);
        Check(onlyCounterConfig.counterKey.modifiers == kKeyModCtrl && onlyCounterConfig.counterKey.vk == 'P',
              "INI can override only the counter key", __FILE__, __LINE__);
        Check(onlyCounterConfig.menuKey == kDefaultMenuKey, "omitted menu key stays Shift+F12", __FILE__, __LINE__);
        onlyCounterConfig.scalePercent = 80;
        char savedKeys[512];
        const size_t savedKeysLength = FormatFpsConfig(onlyCounterConfig, savedKeys, sizeof(savedKeys));
        FpsConfig savedKeysConfig;
        ParseFpsConfig(savedKeys, savedKeysLength, savedKeysConfig);
        Check(savedKeysLength > 0 && savedKeysConfig == onlyCounterConfig, "save keeps the INI hotkey override", __FILE__, __LINE__);

        // Corner placement.
        const FpsCornerPlacement tl = ComputeFpsCornerPlacement(FpsCorner::TopLeft, 1920, 1080, 10);
        const FpsCornerPlacement tr = ComputeFpsCornerPlacement(FpsCorner::TopRight, 1920, 1080, 10);
        const FpsCornerPlacement bl = ComputeFpsCornerPlacement(FpsCorner::BottomLeft, 1920, 1080, 10);
        const FpsCornerPlacement br = ComputeFpsCornerPlacement(FpsCorner::BottomRight, 1920, 1080, 10);
        Check(tl.x == 10 && tl.y == 10 && tl.pivotX == 0 && tl.pivotY == 0, "top left", __FILE__, __LINE__);
        Check(tr.x == 1910 && tr.y == 10 && tr.pivotX == 1 && tr.pivotY == 0, "top right", __FILE__, __LINE__);
        Check(bl.x == 10 && bl.y == 1070 && bl.pivotX == 0 && bl.pivotY == 1, "bottom left", __FILE__, __LINE__);
        Check(br.x == 1910 && br.y == 1070 && br.pivotX == 1 && br.pivotY == 1, "bottom right", __FILE__, __LINE__);

        // File placement: first writable directory wins, then the file stays where it was found.
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring root = std::wstring(temp) + L"HydraHookFpsConfig" + std::to_wstring(GetCurrentProcessId());
        const std::wstring processDir = root + L"\\process\\";
        const std::wstring dllDir = root + L"\\dll\\";
        CreateDirectoryW(root.c_str(), nullptr);
        CreateDirectoryW((root + L"\\process").c_str(), nullptr);
        CreateDirectoryW((root + L"\\dll").c_str(), nullptr);

        {
            FpsConfigStore store;
            store.Init(processDir.c_str(), dllDir.c_str(), L"HydraHook-FPS.ini");
            FpsConfig loaded;
            Check(!store.Load(loaded), "no file yet", __FILE__, __LINE__);
            Check(store.Save(written), "save succeeds", __FILE__, __LINE__);
            Check(std::wstring(store.ActivePath()) == processDir + L"HydraHook-FPS.ini", "saved to the process directory", __FILE__, __LINE__);
        }
        {
            FpsConfigStore store;
            store.Init(processDir.c_str(), dllDir.c_str(), L"HydraHook-FPS.ini");
            FpsConfig loaded;
            Check(store.Load(loaded) && loaded == written, "loaded from the process directory", __FILE__, __LINE__);
        }
        DeleteFileW((processDir + L"HydraHook-FPS.ini").c_str());
        {
            // Process directory missing: falls back to the DLL directory.
            FpsConfigStore store;
            store.Init((root + L"\\missing\\").c_str(), dllDir.c_str(), L"HydraHook-FPS.ini");
            Check(store.Save(written), "save falls back", __FILE__, __LINE__);
            Check(std::wstring(store.ActivePath()) == dllDir + L"HydraHook-FPS.ini", "saved to the DLL directory", __FILE__, __LINE__);
        }
        {
            FpsConfigStore store;
            store.Init(processDir.c_str(), dllDir.c_str(), L"HydraHook-FPS.ini");
            FpsConfig loaded;
            Check(store.Load(loaded) && loaded == written, "loaded from the DLL directory", __FILE__, __LINE__);
            FpsConfig changed = written;
            changed.scalePercent = 80;
            Check(store.Save(changed), "save after load", __FILE__, __LINE__);
            Check(std::wstring(store.ActivePath()) == dllDir + L"HydraHook-FPS.ini", "save stays where the file was found", __FILE__, __LINE__);
        }
        DeleteFileW((dllDir + L"HydraHook-FPS.ini").c_str());
        {
            // A read-only file that was loaded: save fails instead of writing a file Load would ignore.
            const std::wstring locked = processDir + L"HydraHook-FPS.ini";
            FpsConfigStore seed;
            seed.Init(processDir.c_str(), dllDir.c_str(), L"HydraHook-FPS.ini");
            seed.Save(written);
            SetFileAttributesW(locked.c_str(), FILE_ATTRIBUTE_READONLY);
            FpsConfigStore store;
            store.Init(processDir.c_str(), dllDir.c_str(), L"HydraHook-FPS.ini");
            FpsConfig loaded;
            Check(store.Load(loaded), "read-only file still loads", __FILE__, __LINE__);
            Check(!store.Save(written), "failed write is not redirected", __FILE__, __LINE__);
            Check(GetFileAttributesW((dllDir + L"HydraHook-FPS.ini").c_str()) == INVALID_FILE_ATTRIBUTES,
                  "no shadow file in the DLL directory", __FILE__, __LINE__);
            SetFileAttributesW(locked.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(locked.c_str());
        }
        RemoveDirectoryW((root + L"\\process").c_str());
        RemoveDirectoryW((root + L"\\dll").c_str());
        RemoveDirectoryW(root.c_str());
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
    std::atomic<int> g_origUnhookDrained{-1};
    std::atomic<int> g_handlerUnhookDrained{-1};
    HANDLE g_stalledHandlerEntered = nullptr;
    HANDLE g_stalledHandlerRelease = nullptr;

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
        case WM_APP + 4:
        {
            // Simulate a host WNDPROC calling PostQuitMessage / ExitProcess and blocking
            // while the engine worker thread calls UnhookAll().
            std::thread worker([]()
            {
                g_origUnhookDrained.store(WindowProcHook::Instance().UnhookAll(500) ? 1 : 0);
            });
            worker.join();
            return 0x44;
        }
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
        if (msg == WM_APP + 5)
        {
            g_handlerUnhookDrained.store(WindowProcHook::Instance().UnhookAll(500) ? 1 : 0);
            return 0x55;
        }
        if (msg == WM_APP + 6)
        {
            if (g_stalledHandlerEntered)
                SetEvent(g_stalledHandlerEntered);
            if (g_stalledHandlerRelease)
                WaitForSingleObject(g_stalledHandlerRelease, 5000);
            return 0x66;
        }
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

        // Cross-thread UnhookAll while the host original WNDPROC is blocked waiting for it
        // (reproduces PostQuitMessage/ExitProcess called from inside a host WNDPROC).
        {
            HWND w5 = MakeTestWindow();
            Check(w5 != nullptr, "orig-unhook window created", __FILE__, __LINE__);
            Check(hook.Hook(w5), "hook before orig-triggered UnhookAll", __FILE__, __LINE__);
            g_origUnhookDrained.store(-1);
            Check(SendMessageW(w5, WM_APP + 4, 0, 0) == 0x44,
                  "orig-triggered cross-thread UnhookAll does not deadlock", __FILE__, __LINE__);
            Check(g_origUnhookDrained.load() == 1,
                  "UnhookAll drains without waiting on original WNDPROC", __FILE__, __LINE__);
            Check(GetWindowLongPtrW(w5, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&TestOrigProc),
                  "original restored during orig-triggered UnhookAll", __FILE__, __LINE__);
            Check(hook.HookedCount() == 0, "no active hooks after orig-triggered UnhookAll", __FILE__, __LINE__);
            DestroyWindow(w5);
        }

        // Same-thread UnhookAll from inside MessageHandler skips the drain wait and returns false.
        {
            HWND w6 = MakeTestWindow();
            Check(w6 != nullptr, "reentrant-unhook window created", __FILE__, __LINE__);
            Check(hook.Hook(w6), "hook before handler-triggered UnhookAll", __FILE__, __LINE__);
            g_handlerUnhookDrained.store(-1);
            Check(SendMessageW(w6, WM_APP + 5, 0, 0) == 0x55, "handler-triggered UnhookAll returns", __FILE__, __LINE__);
            Check(g_handlerUnhookDrained.load() == 0,
                  "re-entrant UnhookAll from handler reports not drained", __FILE__, __LINE__);
            Check(GetWindowLongPtrW(w6, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&TestOrigProc),
                  "original restored during handler-triggered UnhookAll", __FILE__, __LINE__);
            DestroyWindow(w6);
        }

        // Bounded drain timeout when another thread is stuck inside MessageHandler.
        {
            g_stalledHandlerEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_stalledHandlerRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::atomic<HWND> stalledHwnd{nullptr};
            std::atomic<int> stalledReady{0};
            std::thread stalledOwner([&]()
            {
                HWND hw = MakeTestWindow();
                if (!hw)
                {
                    stalledReady.store(-1);
                    return;
                }
                stalledHwnd.store(hw);
                stalledReady.store(1);
                MSG m;
                while (GetMessageW(&m, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
            });
            while (stalledReady.load() == 0)
                SwitchToThread();
            if (stalledReady.load() < 0)
            {
                stalledOwner.join();
                Check(false, "stalled-handler owner window created", __FILE__, __LINE__);
            }
            else
            {
                HWND hw = stalledHwnd.load();
                Check(hook.Hook(hw), "hook before stalled-handler UnhookAll", __FILE__, __LINE__);
                std::thread sender([hw]() { SendMessageW(hw, WM_APP + 6, 0, 0); });
                Check(WaitForSingleObject(g_stalledHandlerEntered, 2000) == WAIT_OBJECT_0,
                      "stalled handler entered", __FILE__, __LINE__);
                Check(!hook.UnhookAll(50), "UnhookAll times out cleanly when handler is stalled", __FILE__, __LINE__);
                SetEvent(g_stalledHandlerRelease);
                sender.join();
                Check(hook.UnhookAll(200), "UnhookAll drains once stalled handler returns", __FILE__, __LINE__);
                SendMessageW(hw, WM_APP + 9, 0, 0);
                stalledOwner.join();
            }
            if (g_stalledHandlerEntered)
            {
                CloseHandle(g_stalledHandlerEntered);
                g_stalledHandlerEntered = nullptr;
            }
            if (g_stalledHandlerRelease)
            {
                CloseHandle(g_stalledHandlerRelease);
                g_stalledHandlerRelease = nullptr;
            }
        }

        // Deferred capture-release fallback when SendMessageTimeoutW times out and PostMessageW fails:
        // keeping the hook attached lets the next pumped message release capture on the window-owning
        // thread while forwarding that message to the host WNDPROC.
        {
            static std::atomic<bool> s_inputClosed{false};
            static std::atomic<bool> s_overlayOwnsCapture{false};
            static std::atomic<uint32_t> s_forwardedButtons{0};
            static std::atomic<DWORD> s_releaseThreadId{0};
            static std::atomic<int> s_releaseCalls{0};
            static HANDLE s_blockOwnerEvent = nullptr;
            static HANDLE s_ownerBlockedEvent = nullptr;

            s_inputClosed.store(false);
            s_overlayOwnsCapture.store(true);
            s_forwardedButtons.store(1u);
            s_releaseThreadId.store(0);
            s_releaseCalls.store(0);
            s_blockOwnerEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            s_ownerBlockedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

            std::atomic<HWND> fbHwnd{nullptr};
            std::atomic<DWORD> fbOwnerTid{0};
            std::atomic<int> fbReady{0};
            std::atomic<bool> captureAcquired{false};
            static std::atomic<bool> s_captureClearedAfterFallback{false};
            s_captureClearedAfterFallback.store(false);

            std::thread fbOwner([&]()
            {
                fbOwnerTid.store(GetCurrentThreadId());
                HWND hw = MakeTestWindow();
                if (!hw)
                {
                    fbReady.store(-1);
                    return;
                }
                SetCapture(hw);
                captureAcquired.store(GetCapture() == hw);
                fbHwnd.store(hw);
                fbReady.store(1);

                // Block briefly so cross-thread SendMessageTimeoutW times out.
                SetEvent(s_ownerBlockedEvent);
                WaitForSingleObject(s_blockOwnerEvent, 5000);

                MSG m;
                while (GetMessageW(&m, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
            });

            while (fbReady.load() == 0)
                SwitchToThread();
            if (fbReady.load() < 0)
            {
                SetEvent(s_blockOwnerEvent);
                fbOwner.join();
                Check(false, "deferred-release owner window created", __FILE__, __LINE__);
            }
            else
            {
                HWND hw = fbHwnd.load();
                hook.SetMessageHandler([](HWND hWnd, UINT, WPARAM, LPARAM) -> LRESULT
                {
                    if (ShouldFallbackReleaseOnClosedInput(s_inputClosed, s_overlayOwnsCapture, s_forwardedButtons))
                    {
                        s_releaseThreadId.store(GetCurrentThreadId());
                        const bool ownedCapture = s_overlayOwnsCapture.exchange(false);
                        s_forwardedButtons.exchange(0);
                        if (ownedCapture && GetCapture() == hWnd)
                            ReleaseCapture();
                        s_captureClearedAfterFallback.store(GetCapture() == nullptr);
                        ++s_releaseCalls;
                        return 0;
                    }
                    return 0;
                });
                Check(hook.Hook(hw), "hook installed for deferred-release fallback", __FILE__, __LINE__);
                Check(WaitForSingleObject(s_ownerBlockedEvent, 2000) == WAIT_OBJECT_0,
                      "owner thread entered blocked state", __FILE__, __LINE__);

                // Simulate UnhookWindowProc: close input, attempt SendMessageTimeoutW (times out),
                // simulate PostMessageW failure (no release message queued), keep state and hook intact.
                s_inputClosed.store(true);
                DWORD_PTR sendResult = 0;
                const LRESULT sent = SendMessageTimeoutW(
                    hw, WM_APP + 50, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 30, &sendResult);
                Check(sent == 0, "SendMessageTimeoutW times out while window thread is blocked", __FILE__, __LINE__);
                Check(ShouldFallbackReleaseOnClosedInput(s_inputClosed, s_overlayOwnsCapture, s_forwardedButtons),
                      "release obligation retained after timeout and failed PostMessageW", __FILE__, __LINE__);

                // Unblock window thread and send a normal host message without any queued release message.
                SetEvent(s_blockOwnerEvent);
                Check(SendMessageW(hw, WM_APP + 1, 0, 0) == 0x1234,
                      "host WNDPROC still receives the next message after fallback release", __FILE__, __LINE__);
                Check(s_releaseCalls.load() == 1,
                      "fallback release ran once on next pumped message", __FILE__, __LINE__);
                Check(s_releaseThreadId.load() == fbOwnerTid.load(),
                      "fallback release ran on window-owning thread", __FILE__, __LINE__);
                Check(!s_overlayOwnsCapture.load() && s_forwardedButtons.load() == 0,
                      "fallback release cleared capture and button state", __FILE__, __LINE__);
                if (captureAcquired.load())
                    Check(s_captureClearedAfterFallback.load(),
                          "fallback release cleared Win32 capture on owner thread", __FILE__, __LINE__);

                Check(hook.UnhookAll(500), "UnhookAll succeeds after fallback release", __FILE__, __LINE__);
                SendMessageW(hw, WM_APP + 9, 0, 0);
                fbOwner.join();
            }

            if (s_blockOwnerEvent)
            {
                CloseHandle(s_blockOwnerEvent);
                s_blockOwnerEvent = nullptr;
            }
            if (s_ownerBlockedEvent)
            {
                CloseHandle(s_ownerBlockedEvent);
                s_ownerBlockedEvent = nullptr;
            }
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

        // A failed apply() must abort its Detours transaction so subsequent apply() calls succeed.
        Hook<CallConvention::stdcall_t, int, int> failedHook;
        bool threw = false;
        try
        {
            failedHook.apply(static_cast<size_t>(0), &StdDetour);
        }
        catch (const HydraHook::Core::Exceptions::DetourException&)
        {
            threw = true;
        }
        Check(threw, "apply(0) throws DetourException", __FILE__, __LINE__);
        Check(!failedHook.is_applied(), "failed apply leaves hook unapplied", __FILE__, __LINE__);

        hook.apply(reinterpret_cast<size_t>(&StdTarget), &StdDetour);
        Check(StdTarget(4) == original + 10, "apply succeeds after a failed apply aborted its transaction", __FILE__, __LINE__);
        Check(hook.remove_nothrow(), "remove_nothrow after recovery apply", __FILE__, __LINE__);
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

        delete engine;
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
    TestKeyToggle();
    TestFpsConfig();
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
