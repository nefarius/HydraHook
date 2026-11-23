/**
 * @file Engine.cpp
 * @brief HydraHook engine implementation - create, destroy, context, callbacks.
 *
 * Implements the public C API declared in HydraHookCore.h. Uses spdlog for
 * logging and maintains HMODULE-to-engine handle mapping.
 *
 * @internal
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


#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdarg.h>

//
// Public
// 
#include "HydraHook/Engine/HydraHookCore.h"
#include "HydraHook/Engine/HydraHookDirect3D9.h"
#include "HydraHook/Engine/HydraHookDirect3D10.h"
#include "HydraHook/Engine/HydraHookDirect3D11.h"
#include "HydraHook/Engine/HydraHookDirect3D12.h"
#include "HydraHook/Engine/HydraHookCoreAudio.h"

//
// Internal
// 
#include "Engine.h"
#include "CrashHandler.h"
#include "Game/Game.h"
#include "Game/Shutdown.h"
#include "Utils/Global.h"

//
// Logging
// 
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

//
// STL
// 
#include <map>
#include <mutex>
#include <new>
#include <string>

//
// Keep track of HINSTANCE/HANDLE to engine handle association
// 
static std::mutex g_EngineHostInstancesMutex;
static std::map<HMODULE, PHYDRAHOOK_ENGINE> g_EngineHostInstances;

namespace
{
	std::mutex g_hostLoggerMutex;
	std::shared_ptr<spdlog::logger> g_hostLogger;

	void ResetHostLogger()
	{
		std::lock_guard<std::mutex> lock(g_hostLoggerMutex);
		g_hostLogger.reset();
	}

	struct LifecycleGuard
	{
		explicit LifecycleGuard(PSRWLOCK lock) : lock_(lock) { AcquireSRWLockExclusive(lock_); }
		~LifecycleGuard() { ReleaseSRWLockExclusive(lock_); }
		LifecycleGuard(const LifecycleGuard&) = delete;
		LifecycleGuard& operator=(const LifecycleGuard&) = delete;

	private:
		PSRWLOCK lock_;
	};
}


HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineCreate(HMODULE HostInstance, PHYDRAHOOK_ENGINE_CONFIG EngineConfig,
                                                    PHYDRAHOOK_ENGINE* Engine)
{
	if (!EngineConfig)
	{
		return HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE;
	}

	{
		std::lock_guard<std::mutex> lock(g_EngineHostInstancesMutex);
		if (g_EngineHostInstances.count(HostInstance))
		{
			return HYDRAHOOK_ERROR_ENGINE_ALREADY_ALLOCATED;
		}
	}

	HMODULE hMod;
	if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCTSTR>(HostInstance),
		&hMod))
	{
		return HYDRAHOOK_ERROR_GET_MODULE_HANDLE_FAILED;
	}

	const auto engine = new (std::nothrow) HYDRAHOOK_ENGINE();

	if (!engine)
	{
		return HYDRAHOOK_ERROR_ENGINE_ALLOCATION_FAILED;
	}

	engine->HostInstance = HostInstance;
	engine->DllModule = hMod;
	HookActivityTracker::reset();
	CopyMemory(&engine->EngineConfig, EngineConfig, sizeof(HYDRAHOOK_ENGINE_CONFIG));

	//
	// Set up logging: disabled config uses a null sink. Otherwise try the process
	// directory, then the DLL directory, then the configured fallback path.
	//
	auto logger = spdlog::get("HYDRAHOOK");
	if (!logger)
	{
		if (!EngineConfig->Logging.IsEnabled)
		{
			try
			{
				logger = std::make_shared<spdlog::logger>(
					"HYDRAHOOK",
					std::make_shared<spdlog::sinks::null_sink_mt>());
				spdlog::register_logger(logger);
			}
			catch (const std::exception&)
			{
				delete engine;
				return HYDRAHOOK_ERROR_CREATE_LOGGER_FAILED;
			}
		}
		else
		{
			const std::string processDir = HydraHook::Core::Util::get_process_directory();
			const std::string dllDir = HydraHook::Core::Util::get_module_directory(HostInstance);
			const char* configuredPath = EngineConfig->Logging.FilePath;
			const std::string tempPath = configuredPath
				? HydraHook::Core::Util::expand_environment_variables(configuredPath)
				: std::string();

			auto tryCreateLogger = [](const std::string& path) -> bool
			{
				if (path.empty())
					return false;
				try
				{
					(void)spdlog::basic_logger_mt("HYDRAHOOK", path);
					return true;
				}
				catch (const std::exception&)
				{
					return false;
				}
			};

			if (!processDir.empty() && tryCreateLogger(processDir + "HydraHook.log"))
			{
				/* ok */
			}
			else if (!dllDir.empty() && tryCreateLogger(dllDir + "HydraHook.log"))
			{
				/* ok */
			}
			else if (!tempPath.empty())
			{
				tryCreateLogger(tempPath);
			}

			logger = spdlog::get("HYDRAHOOK");
			if (!logger)
			{
				try
				{
					logger = spdlog::stdout_color_mt("HYDRAHOOK");
				}
				catch (const std::exception&)
				{
					delete engine;
					return HYDRAHOOK_ERROR_CREATE_LOGGER_FAILED;
				}
			}
		}
	}

#if _DEBUG
	spdlog::set_level(spdlog::level::debug);
	logger->flush_on(spdlog::level::debug);
#else
	logger->flush_on(spdlog::level::info);
#endif

	if (EngineConfig->Logging.IsEnabled)
	{
		spdlog::set_default_logger(logger);
	}

	logger = spdlog::get("HYDRAHOOK")->clone("api");

	//
	// Install crash handler if enabled
	//
	if (EngineConfig->CrashHandler.IsEnabled)
	{
		HydraHookCrashHandlerInstall(engine);
		engine->CrashHandlerInstalled = TRUE;
	}

	//
	// Event to notify engine thread about termination
	// 
	engine->EngineCancellationEvent = CreateEvent(
		nullptr,
		FALSE, // Auto-reset event
		FALSE, // Initial state non-signaled
		nullptr // Unnamed event
	);

	if (engine->EngineCancellationEvent == nullptr)
	{
		logger->error("Failed to create the Engine Cancellation Event: {}", GetLastError());
		if (engine->CrashHandlerInstalled)
		{
			HydraHookCrashHandlerUninstall(engine);
			engine->CrashHandlerInstalled = FALSE;
		}
		delete engine;
		return HYDRAHOOK_ERROR_CREATE_EVENT_FAILED;
	}

	logger->info("HydraHook engine initialized, attempting to launch main thread");

	//
	// Kickstart hooking the rendering pipeline
	// 
	engine->EngineThread = CreateThread(
		nullptr,
		0,
		reinterpret_cast<LPTHREAD_START_ROUTINE>(HydraHookMainThread),
		engine,
		0,
		nullptr
	);

	if (!engine->EngineThread)
	{
		logger->error("Could not create main thread, library unusable");
		if (engine->CrashHandlerInstalled)
		{
			HydraHookCrashHandlerUninstall(engine);
			engine->CrashHandlerInstalled = FALSE;
		}
		CloseHandle(engine->EngineCancellationEvent);
		delete engine;
		return HYDRAHOOK_ERROR_CREATE_THREAD_FAILED;
	}

	logger->info("Main thread created successfully");

	if (Engine)
	{
		*Engine = engine;
	}

	{
		std::lock_guard<std::mutex> lock(g_EngineHostInstancesMutex);
		if (g_EngineHostInstances.count(HostInstance))
		{
			SetEvent(engine->EngineCancellationEvent);
			WaitForSingleObject(engine->EngineThread, 6500);
			if (engine->CrashHandlerInstalled)
			{
				HydraHookCrashHandlerUninstall(engine);
			}
			CloseHandle(engine->EngineCancellationEvent);
			CloseHandle(engine->EngineThread);
			delete engine;
			return HYDRAHOOK_ERROR_ENGINE_ALREADY_ALLOCATED;
		}
		g_EngineHostInstances[HostInstance] = engine;
	}

	return HYDRAHOOK_ERROR_NONE;
}

HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineDestroy(HMODULE HostInstance)
{
	PHYDRAHOOK_ENGINE engine = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_EngineHostInstancesMutex);
		const auto it = g_EngineHostInstances.find(HostInstance);
		if (it == g_EngineHostInstances.end())
		{
			return HYDRAHOOK_ERROR_INVALID_HMODULE_HANDLE;
		}
		engine = it->second;
	}

	auto base = spdlog::get("HYDRAHOOK");
	auto logger = base ? base->clone("api") : nullptr;

	{
		LifecycleGuard lifecycle(&engine->LifecycleLock);
		PerformShutdownCleanup(engine, ShutdownOrigin::DllMainProcessDetach);
	}

	if (logger)
		logger->info("Freeing remaining resources");

	if (engine->CustomContext)
	{
		free(engine->CustomContext);
		engine->CustomContext = nullptr;
	}

	if (engine->CrashHandlerInstalled)
	{
		HydraHookCrashHandlerUninstall(engine);
		engine->CrashHandlerInstalled = FALSE;
	}

	if (engine->EngineCancellationEvent)
	{
		CloseHandle(engine->EngineCancellationEvent);
	}
	if (engine->EngineThread)
	{
		CloseHandle(engine->EngineThread);
	}

	bool lastEngine = false;
	{
		std::lock_guard<std::mutex> lock(g_EngineHostInstancesMutex);
		g_EngineHostInstances.erase(HostInstance);
		lastEngine = g_EngineHostInstances.empty();
	}

	if (logger)
	{
		logger->info("Engine shutdown complete");
		logger->flush();
	}

	delete engine;

	if (lastEngine)
	{
		ResetHostLogger();
		spdlog::drop("HYDRAHOOK");
	}

	return HYDRAHOOK_ERROR_NONE;
}

extern "C" HYDRAHOOK_D3D_VERSION HydraHookEngineGameVersionForTest(PHYDRAHOOK_ENGINE engine)
{
	return engine != nullptr ? engine->GameVersion : HydraHookDirect3DVersionUnknown;
}

HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineUnhook(PHYDRAHOOK_ENGINE Engine)
{
	if (!Engine)
	{
		return HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE;
	}

	LifecycleGuard lifecycle(&Engine->LifecycleLock);

	if (!Engine->EngineThread)
	{
		return HYDRAHOOK_ERROR_NONE;
	}

	// The worker cannot wait for itself
	if (GetThreadId(Engine->EngineThread) == GetCurrentThreadId())
	{
		return HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE;
	}

	// Keep the DLL loaded even if another shutdown path already claimed cleanup
	Engine->SkipDllUnloadOnExit.store(true, std::memory_order_release);

	PerformShutdownCleanup(Engine, ShutdownOrigin::ManualUnhook);

	// PerformShutdownCleanup returns early when another path owns cleanup, and
	// never terminates the worker on a manual unhook; confirm the worker is gone
	// before releasing its handle or any engine state.
	if (WaitForSingleObject(Engine->EngineThread, 8000) != WAIT_OBJECT_0)
	{
		return HYDRAHOOK_ERROR_UNHOOK_INCOMPLETE;
	}

	// The worker exits even if a callback outlives the drain timeout; keep the
	// engine until those callbacks have returned.
	if (Engine->DrainIncomplete.load(std::memory_order_acquire))
	{
		if (!HookActivityTracker::drain(1000))
		{
			return HYDRAHOOK_ERROR_UNHOOK_INCOMPLETE;
		}

		ZeroMemory(&Engine->EventsD3D9, sizeof(Engine->EventsD3D9));
		ZeroMemory(&Engine->EventsD3D10, sizeof(Engine->EventsD3D10));
		ZeroMemory(&Engine->EventsD3D11, sizeof(Engine->EventsD3D11));
		ZeroMemory(&Engine->EventsD3D12, sizeof(Engine->EventsD3D12));
		ZeroMemory(&Engine->EventsARC, sizeof(Engine->EventsARC));
		Engine->DrainIncomplete.store(false, std::memory_order_release);
	}

	CloseHandle(Engine->EngineThread);
	Engine->EngineThread = nullptr;

	ZeroMemory(&Engine->RenderPipeline, sizeof(Engine->RenderPipeline));
	ZeroMemory(&Engine->CoreAudio, sizeof(Engine->CoreAudio));
	Engine->GameVersion = HydraHookDirect3DVersionUnknown;

	return HYDRAHOOK_ERROR_NONE;
}

HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineRehook(PHYDRAHOOK_ENGINE Engine)
{
	if (!Engine)
	{
		return HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE;
	}

	LifecycleGuard lifecycle(&Engine->LifecycleLock);

	if (Engine->EngineThread)
	{
		if (WaitForSingleObject(Engine->EngineThread, 0) == WAIT_TIMEOUT)
		{
			return HYDRAHOOK_ERROR_NONE;
		}

		if (Engine->DrainIncomplete.load(std::memory_order_acquire)
			&& !HookActivityTracker::drain(0))
		{
			return HYDRAHOOK_ERROR_UNHOOK_INCOMPLETE;
		}

		CloseHandle(Engine->EngineThread);
		Engine->EngineThread = nullptr;
	}

	Engine->DrainIncomplete.store(false, std::memory_order_release);

	HookActivityTracker::reset();
	Engine->ShutdownCleanupDone.store(false);
	Engine->FreeLibraryHookActive.store(false);
	Engine->SkipDllUnloadOnExit.store(false);
	ResetEvent(Engine->EngineCancellationEvent);

	Engine->EventsD3D9 = Engine->SavedEventsD3D9;
	Engine->EventsD3D10 = Engine->SavedEventsD3D10;
	Engine->EventsD3D11 = Engine->SavedEventsD3D11;
	Engine->EventsD3D12 = Engine->SavedEventsD3D12;
	Engine->EventsARC = Engine->SavedEventsARC;

	Engine->EngineThread = CreateThread(
		nullptr,
		0,
		reinterpret_cast<LPTHREAD_START_ROUTINE>(HydraHookMainThread),
		Engine,
		0,
		nullptr
	);

	if (!Engine->EngineThread)
	{
		Engine->ShutdownCleanupDone.store(true);
		return HYDRAHOOK_ERROR_CREATE_THREAD_FAILED;
	}

	return HYDRAHOOK_ERROR_NONE;
}

HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineAllocCustomContext(PHYDRAHOOK_ENGINE Engine, PVOID* Context,
                                                                size_t ContextSize)
{
	if (!Engine || !Context)
	{
		return HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE;
	}

	if (Engine->CustomContext)
	{
		HydraHookEngineFreeCustomContext(Engine);
	}

	Engine->CustomContext = malloc(ContextSize);

	if (!Engine->CustomContext)
	{
		return HYDRAHOOK_ERROR_CONTEXT_ALLOCATION_FAILED;
	}

	ZeroMemory(Engine->CustomContext, ContextSize);
	*Context = Engine->CustomContext;

	return HYDRAHOOK_ERROR_NONE;
}

HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineFreeCustomContext(PHYDRAHOOK_ENGINE Engine)
{
	if (!Engine)
	{
		return HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE;
	}

	if (Engine->CustomContext)
	{
		free(Engine->CustomContext);
		Engine->CustomContext = NULL;
	}

	return HYDRAHOOK_ERROR_NONE;
}

HYDRAHOOK_API PVOID HydraHookEngineGetCustomContext(PHYDRAHOOK_ENGINE Engine)
{
	if (!Engine)
	{
		return nullptr;
	}

	return Engine->CustomContext;
}

#ifndef HYDRAHOOK_NO_D3D9

HYDRAHOOK_API VOID HydraHookEngineSetD3D9EventCallbacks(PHYDRAHOOK_ENGINE Engine,
                                                        PHYDRAHOOK_D3D9_EVENT_CALLBACKS Callbacks)
{
	if (Engine && Callbacks)
	{
		Engine->EventsD3D9 = *Callbacks;
		Engine->SavedEventsD3D9 = *Callbacks;
	}
}

HYDRAHOOK_API PHYDRAHOOK_ENGINE HydraHookEngineGetHandleFromD3D9Device(LPDIRECT3DDEVICE9 Device)
{
	std::lock_guard<std::mutex> lock(g_EngineHostInstancesMutex);
	for (const auto& kv : g_EngineHostInstances)
	{
		const auto& engine = kv.second;

		if (engine->RenderPipeline.pD3D9Device == Device)
		{
			return engine;
		}
	}

	return nullptr;
}

HYDRAHOOK_API PHYDRAHOOK_ENGINE HydraHookEngineGetHandleFromD3D9ExDevice(LPDIRECT3DDEVICE9EX Device)
{
	std::lock_guard<std::mutex> lock(g_EngineHostInstancesMutex);
	for (const auto& kv : g_EngineHostInstances)
	{
		const auto& engine = kv.second;

		if (engine->RenderPipeline.pD3D9ExDevice == Device)
		{
			return engine;
		}
	}

	return nullptr;
}

#endif

#ifndef HYDRAHOOK_NO_D3D10

HYDRAHOOK_API VOID HydraHookEngineSetD3D10EventCallbacks(PHYDRAHOOK_ENGINE Engine,
                                                         PHYDRAHOOK_D3D10_EVENT_CALLBACKS Callbacks)
{
	if (Engine && Callbacks)
	{
		Engine->EventsD3D10 = *Callbacks;
		Engine->SavedEventsD3D10 = *Callbacks;
	}
}

#endif

#ifndef HYDRAHOOK_NO_D3D11

HYDRAHOOK_API VOID HydraHookEngineSetD3D11EventCallbacks(PHYDRAHOOK_ENGINE Engine,
                                                         PHYDRAHOOK_D3D11_EVENT_CALLBACKS Callbacks)
{
	if (Engine && Callbacks)
	{
		Engine->EventsD3D11 = *Callbacks;
		Engine->SavedEventsD3D11 = *Callbacks;
	}
}

#endif

#ifndef HYDRAHOOK_NO_D3D12

HYDRAHOOK_API VOID HydraHookEngineSetD3D12EventCallbacks(PHYDRAHOOK_ENGINE Engine,
                                                         PHYDRAHOOK_D3D12_EVENT_CALLBACKS Callbacks)
{
	if (Engine && Callbacks)
	{
		Engine->EventsD3D12 = *Callbacks;
		Engine->SavedEventsD3D12 = *Callbacks;
	}
}

HYDRAHOOK_API ID3D12CommandQueue* HydraHookEngineGetD3D12CommandQueue(IDXGISwapChain* pSwapChain)
{
	return GetD3D12CommandQueueForSwapChain(pSwapChain);
}

#endif

#ifndef HYDRAHOOK_NO_COREAUDIO

HYDRAHOOK_API VOID HydraHookEngineSetARCEventCallbacks(PHYDRAHOOK_ENGINE Engine,
                                                       PHYDRAHOOK_ARC_EVENT_CALLBACKS Callbacks)
{
	if (Engine && Callbacks)
	{
		Engine->EventsARC = *Callbacks;
		Engine->SavedEventsARC = *Callbacks;
	}
}

#endif

/**
 * @brief Returns a cached host-scoped logger clone for the process-wide "HYDRAHOOK" logger.
 *
 * The returned logger is created once on first call and reused for subsequent calls. If the
 * process-wide "HYDRAHOOK" logger is not available, this function returns `nullptr`.
 *
 * @return std::shared_ptr<spdlog::logger> Shared pointer to the cloned "host" logger, or `nullptr` if the base logger does not exist.
 */
static std::shared_ptr<spdlog::logger> GetHostLogger()
{
	std::lock_guard<std::mutex> lock(g_hostLoggerMutex);
	if (g_hostLogger)
	{
		return g_hostLogger;
	}
	auto base = spdlog::get("HYDRAHOOK");
	if (base)
	{
		g_hostLogger = base->clone("host");
		return g_hostLogger;
	}
	return nullptr;
}

/**
 * @brief Format a printf-style message and emit it to the host logger at the specified level.
 *
 * Formats the provided `Format` string with `args` into a temporary buffer and logs the result
 * via the host-scoped logger if one exists and is enabled for `level`. If no logger is available
 * or the level is disabled, the function returns without side effects.
 *
 * @param level Log severity level to use when emitting the message.
 * @param Format printf-style format string describing the message.
 * @param args   Variadic argument list corresponding to `Format`.
 */
static void HydraHookEngineLogImpl(spdlog::level::level_enum level, LPCSTR Format, va_list args)
{
	auto logger = GetHostLogger();
	if (!logger || !logger->should_log(level) || !Format)
	{
		return;
	}

	va_list measure;
	va_copy(measure, args);
	const int needed = vsnprintf(nullptr, 0, Format, measure);
	va_end(measure);
	if (needed < 0)
	{
		return;
	}

	std::string message(static_cast<size_t>(needed) + 1, '\0');
	vsnprintf(message.data(), message.size(), Format, args);
	message.resize(static_cast<size_t>(needed));
	logger->log(level, message);
}

_Use_decl_annotations_
HYDRAHOOK_API VOID HydraHookEngineLogDebug(LPCSTR Format, ...)
{
	va_list args;
	va_start(args, Format);
	HydraHookEngineLogImpl(spdlog::level::debug, Format, args);
	va_end(args);
}

_Use_decl_annotations_
HYDRAHOOK_API VOID HydraHookEngineLogInfo(LPCSTR Format, ...)
{
	va_list args;
	va_start(args, Format);
	HydraHookEngineLogImpl(spdlog::level::info, Format, args);
	va_end(args);
}

_Use_decl_annotations_
HYDRAHOOK_API VOID HydraHookEngineLogWarning(LPCSTR Format, ...)
{
	va_list args;
	va_start(args, Format);
	HydraHookEngineLogImpl(spdlog::level::warn, Format, args);
	va_end(args);
}

_Use_decl_annotations_
HYDRAHOOK_API VOID HydraHookEngineLogError(LPCSTR Format, ...)
{
	va_list args;
	va_start(args, Format);
	HydraHookEngineLogImpl(spdlog::level::err, Format, args);
	va_end(args);
}
