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
#include "Global.h"

//
// Logging
// 
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

//
// STL
// 
#include <map>
#include <mutex>

//
// Keep track of HINSTANCE/HANDLE to engine handle association
// 
static std::map<HMODULE, PHYDRAHOOK_ENGINE> g_EngineHostInstances;


/**
 * @brief Create and initialize a HydraHook engine for a host module and start its main thread.
 *
 * Initializes a new engine instance for the specified host module, configures logging per
 * the provided engine configuration, creates the engine cancellation event, launches the
 * engine's main thread, and registers the engine in the process-wide host-instance map.
 *
 * @param HostInstance Handle to the host module (HMODULE) the engine will be associated with.
 * @param EngineConfig Pointer to an engine configuration structure that controls logging and other engine options.
 * @param Engine Optional out parameter that receives the allocated engine handle on success.
 * @return HYDRAHOOK_ERROR_NONE on success.
 * @return HYDRAHOOK_ERROR_ENGINE_ALREADY_ALLOCATED if an engine is already created for HostInstance.
 * @return HYDRAHOOK_ERROR_REFERENCE_INCREMENT_FAILED if the host DLL module handle could not be obtained.
 * @return HYDRAHOOK_ERROR_ENGINE_ALLOCATION_FAILED if engine memory allocation failed.
 * @return HYDRAHOOK_ERROR_CREATE_LOGGER_FAILED if a suitable logger could not be created or obtained.
 * @return HYDRAHOOK_ERROR_CREATE_EVENT_FAILED if the engine cancellation event could not be created.
 * @return HYDRAHOOK_ERROR_CREATE_THREAD_FAILED if the engine main thread could not be created.
 */
HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineCreate(HMODULE HostInstance, PHYDRAHOOK_ENGINE_CONFIG EngineConfig, PHYDRAHOOK_ENGINE * Engine)
{
	//
	// Check if we got initialized for this instance before
	// 
	if (g_EngineHostInstances.count(HostInstance)) {
		return HYDRAHOOK_ERROR_ENGINE_ALREADY_ALLOCATED;
	}

	//
	// Increase host DLL reference count
	// 
	HMODULE hmod;
	if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
		reinterpret_cast<LPCTSTR>(HostInstance),
		&hmod)) {
		return HYDRAHOOK_ERROR_REFERENCE_INCREMENT_FAILED;
	}

	const auto engine = static_cast<PHYDRAHOOK_ENGINE>(malloc(sizeof(HYDRAHOOK_ENGINE)));

	if (!engine) {
		return HYDRAHOOK_ERROR_ENGINE_ALLOCATION_FAILED;
	}

	//
	// Initialize engine handle
	// 
	ZeroMemory(engine, sizeof(HYDRAHOOK_ENGINE));
	engine->HostInstance = HostInstance;
	CopyMemory(&engine->EngineConfig, EngineConfig, sizeof(HYDRAHOOK_ENGINE_CONFIG));

	//
	// Set up logging: try process directory first, then DLL directory, then %TEMP%
	//
	std::string processDir = HydraHook::Core::Util::get_process_directory();
	std::string dllDir = HydraHook::Core::Util::get_module_directory(HostInstance);
	std::string tempPath = HydraHook::Core::Util::expand_environment_variables(EngineConfig->Logging.FilePath);

	auto tryCreateLogger = [](const std::string& path) -> bool {
		try {
			(void)spdlog::basic_logger_mt("HYDRAHOOK", path);
			return true;
		}
		catch (const std::exception&) {
			return false;
		}
	};

	if (!processDir.empty() && tryCreateLogger(processDir + "HydraHook.log")) { /* ok */ }
	else if (!dllDir.empty() && tryCreateLogger(dllDir + "HydraHook.log")) { /* ok */ }
	else { tryCreateLogger(tempPath); }

	auto logger = spdlog::get("HYDRAHOOK");
	if (!logger) {
		try {
			logger = spdlog::stdout_color_mt("HYDRAHOOK");
		}
		catch (const std::exception&) {
			free(engine);
			return HYDRAHOOK_ERROR_CREATE_LOGGER_FAILED;
		}
	}

#if _DEBUG
	spdlog::set_level(spdlog::level::debug);
	logger->flush_on(spdlog::level::debug);
#else
	logger->flush_on(spdlog::level::info);
#endif

	if (EngineConfig->Logging.IsEnabled) {
		spdlog::set_default_logger(logger);
	}

	logger = spdlog::get("HYDRAHOOK")->clone("api");

	//
	// Install crash handler if enabled
	//
	if (EngineConfig->CrashHandler.IsEnabled) {
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
		NULL // Named unique event
	);

	if (engine->EngineCancellationEvent == INVALID_HANDLE_VALUE
		|| engine->EngineCancellationEvent == NULL) {
		logger->error("Failed to create the Engine Cancellation Event: {}", GetLastError());
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

	if (!engine->EngineThread) {
		logger->error("Could not create main thread, library unusable");
		return HYDRAHOOK_ERROR_CREATE_THREAD_FAILED;
	}

	logger->info("Main thread created successfully");

	if (Engine)
	{
		*Engine = engine;
	}

	g_EngineHostInstances[HostInstance] = engine;

	return HYDRAHOOK_ERROR_NONE;
}

HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineDestroy(HMODULE HostInstance)
{
	if (!g_EngineHostInstances.count(HostInstance)) {
		return HYDRAHOOK_ERROR_INVALID_HMODULE_HANDLE;
	}

	const auto& engine = g_EngineHostInstances[HostInstance];
	auto logger = spdlog::get("HYDRAHOOK")->clone("api");

	logger->info("Freeing remaining resources");

	if (engine->CrashHandlerInstalled) {
		HydraHookCrashHandlerUninstall();
		engine->CrashHandlerInstalled = FALSE;
	}

	CloseHandle(engine->EngineCancellationEvent);
	CloseHandle(engine->EngineThread);

	const auto it = g_EngineHostInstances.find(HostInstance);
	g_EngineHostInstances.erase(it);

	logger->info("Engine shutdown complete");
	logger->flush();

	return HYDRAHOOK_ERROR_NONE;
}

HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineAllocCustomContext(PHYDRAHOOK_ENGINE Engine, PVOID* Context, size_t ContextSize)
{
	if (!Engine) {
		return HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE;
	}

	if (Engine->CustomContext) {
		HydraHookEngineFreeCustomContext(Engine);
	}

	Engine->CustomContext = malloc(ContextSize);

	if (!Engine->CustomContext) {
		return HYDRAHOOK_ERROR_CONTEXT_ALLOCATION_FAILED;
	}

	ZeroMemory(Engine->CustomContext, ContextSize);
	*Context = Engine->CustomContext;

	return HYDRAHOOK_ERROR_NONE;
}

HYDRAHOOK_API HYDRAHOOK_ERROR HydraHookEngineFreeCustomContext(PHYDRAHOOK_ENGINE Engine)
{
	if (!Engine) {
		return HYDRAHOOK_ERROR_INVALID_ENGINE_HANDLE;
	}

	if (Engine->CustomContext) {
		free(Engine->CustomContext);
	}

	return HYDRAHOOK_ERROR_NONE;
}

HYDRAHOOK_API PVOID HydraHookEngineGetCustomContext(PHYDRAHOOK_ENGINE Engine)
{
	if (!Engine) {
		return nullptr;
	}

	return Engine->CustomContext;
}

#ifndef HYDRAHOOK_NO_D3D9

HYDRAHOOK_API VOID HydraHookEngineSetD3D9EventCallbacks(PHYDRAHOOK_ENGINE Engine, PHYDRAHOOK_D3D9_EVENT_CALLBACKS Callbacks)
{
	if (Engine) {
		Engine->EventsD3D9 = *Callbacks;
	}
}

HYDRAHOOK_API PHYDRAHOOK_ENGINE HydraHookEngineGetHandleFromD3D9Device(LPDIRECT3DDEVICE9 Device)
{
	for (const auto& kv : g_EngineHostInstances)
	{
		const auto& engine = kv.second;

		if (engine->RenderPipeline.pD3D9Device == Device) {
			return engine;
		}
	}

	return nullptr;
}

HYDRAHOOK_API PHYDRAHOOK_ENGINE HydraHookEngineGetHandleFromD3D9ExDevice(LPDIRECT3DDEVICE9EX Device)
{
	for (const auto& kv : g_EngineHostInstances)
	{
		const auto& engine = kv.second;

		if (engine->RenderPipeline.pD3D9ExDevice == Device) {
			return engine;
		}
	}

	return nullptr;
}

#endif

#ifndef HYDRAHOOK_NO_D3D10

HYDRAHOOK_API VOID HydraHookEngineSetD3D10EventCallbacks(PHYDRAHOOK_ENGINE Engine, PHYDRAHOOK_D3D10_EVENT_CALLBACKS Callbacks)
{
	if (Engine) {
		Engine->EventsD3D10 = *Callbacks;
	}
}

#endif

#ifndef HYDRAHOOK_NO_D3D11

HYDRAHOOK_API VOID HydraHookEngineSetD3D11EventCallbacks(PHYDRAHOOK_ENGINE Engine, PHYDRAHOOK_D3D11_EVENT_CALLBACKS Callbacks)
{
	if (Engine) {
		Engine->EventsD3D11 = *Callbacks;
	}
}

#endif

#ifndef HYDRAHOOK_NO_D3D12

/**
 * @brief Sets the engine's Direct3D12 event callback table.
 *
 * Copies the provided D3D12 event callbacks into the engine's D3D12 callback slot.
 *
 * @param Engine Pointer to the engine whose D3D12 callbacks will be updated. If `nullptr`, the function does nothing.
 * @param Callbacks Pointer to the callback table to copy from.
 */
HYDRAHOOK_API VOID HydraHookEngineSetD3D12EventCallbacks(PHYDRAHOOK_ENGINE Engine, PHYDRAHOOK_D3D12_EVENT_CALLBACKS Callbacks)
{
	if (Engine) {
		Engine->EventsD3D12 = *Callbacks;
	}
}

/**
 * @brief Retrieves the ID3D12CommandQueue associated with a given DXGI swap chain.
 *
 * @param pSwapChain Pointer to the IDXGISwapChain whose associated command queue is requested; may be nullptr.
 * @return ID3D12CommandQueue* Pointer to the associated ID3D12CommandQueue, or nullptr if no queue is found.
 */
HYDRAHOOK_API ID3D12CommandQueue* HydraHookEngineGetD3D12CommandQueue(IDXGISwapChain* pSwapChain)
{
	return GetD3D12CommandQueueForSwapChain(pSwapChain);
}

#endif

#ifndef HYDRAHOOK_NO_COREAUDIO

HYDRAHOOK_API VOID HydraHookEngineSetARCEventCallbacks(PHYDRAHOOK_ENGINE Engine, PHYDRAHOOK_ARC_EVENT_CALLBACKS Callbacks)
{
	if (Engine) {
		Engine->EventsARC = *Callbacks;
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
	static std::mutex mtx;
	static std::shared_ptr<spdlog::logger> logger;

	std::lock_guard<std::mutex> lock(mtx);
	if (logger) {
		return logger;
	}
	auto base = spdlog::get("HYDRAHOOK");
	if (base) {
		logger = base->clone("host");
		return logger;
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
	if (!logger || !logger->should_log(level)) {
		return;
	}
	char buf[256];
	vsnprintf(buf, sizeof(buf), Format, args);
	logger->log(level, buf);
}

/**
 * @brief Logs a formatted message at the debug level to the host logger.
 *
 * @param Format Printf-style format string.
 * @param ... Arguments matching the format specifiers in `Format`.
 */
_Use_decl_annotations_
HYDRAHOOK_API VOID HydraHookEngineLogDebug(LPCSTR Format, ...)
{
	va_list args;
	va_start(args, Format);
	HydraHookEngineLogImpl(spdlog::level::debug, Format, args);
	va_end(args);
}

/**
 * @brief Logs a formatted informational message to the host-scoped logger.
 *
 * Formats the provided printf-style string and emits it at the info level via the host logger.
 * If the host logger is not available or not enabled for info-level messages, no output is produced.
 *
 * @param Format printf-style format string.
 * @param ... Values referenced by `Format`.
 */
_Use_decl_annotations_
HYDRAHOOK_API VOID HydraHookEngineLogInfo(LPCSTR Format, ...)
{
	va_list args;
	va_start(args, Format);
	HydraHookEngineLogImpl(spdlog::level::info, Format, args);
	va_end(args);
}

/**
 * @brief Logs a formatted warning message to the host logger.
 *
 * Formats the provided printf-style message and emits it at the warning level
 * using the host-scoped logger.
 *
 * @param Format printf-style format string followed by corresponding arguments.
 */
_Use_decl_annotations_
HYDRAHOOK_API VOID HydraHookEngineLogWarning(LPCSTR Format, ...)
{
	va_list args;
	va_start(args, Format);
	HydraHookEngineLogImpl(spdlog::level::warn, Format, args);
	va_end(args);
}

/**
 * @brief Log an error-level message to the host logger using a printf-style format.
 *
 * Formats the provided arguments according to `Format` and emits the result at error severity
 * via the host-scoped logger.
 *
 * @param Format printf-style format string.
 * @param ... Arguments referenced by the format string.
 */
_Use_decl_annotations_
HYDRAHOOK_API VOID HydraHookEngineLogError(LPCSTR Format, ...)
{
	va_list args;
	va_start(args, Format);
	HydraHookEngineLogImpl(spdlog::level::err, Format, args);
	va_end(args);
}