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

#include <Utils/Hook.h>

#include "Game.h"
#include "Shutdown.h"
#include "Utils/Global.h"
#include "Exceptions.hpp"
#include "CrashHandler.h"
using namespace HydraHook::Core::Exceptions;

//
// Hooking helper sub-systems
// 
#include <Game/Hook/Direct3D9.h>
#include <Game/Hook/Direct3D9Ex.h>
#include <Game/Hook/DXGI.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <Game/Hook/Direct3D10.h>
#include <Game/Hook/Direct3D11.h>
#include <Game/Hook/Direct3D12.h>
#include <Game/Hook/DirectInput8.h>
#include <Game/Hook/AudioRenderClientHook.h>

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

//
// STL
// 
#include <mutex>
#include <memory>
#include <unordered_map>

//
// Logging
//
#include <spdlog/spdlog.h>

#ifndef HYDRAHOOK_NO_D3D12
static std::mutex g_d3d12QueueMapMutex;
static std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*> g_d3d12SwapChainToQueue;
/// Runtime capture: device -> queue (for mid-process injection when CreateSwapChain already ran)
static std::unordered_map<ID3D12Device*, ID3D12CommandQueue*> g_d3d12DeviceToQueue;

static void D3D12_ReleaseQueueMaps()
{
	std::lock_guard<std::mutex> lock(g_d3d12QueueMapMutex);
	for (auto& kv : g_d3d12SwapChainToQueue)
	{
		if (kv.second)
			kv.second->Release();
	}
	g_d3d12SwapChainToQueue.clear();
	for (auto& kv : g_d3d12DeviceToQueue)
	{
		if (kv.second)
			kv.second->Release();
	}
	g_d3d12DeviceToQueue.clear();
}
#endif

//
// Internal flow-control hooks (file scope for PerformShutdownCleanup access)
//
static Hook<CallConvention::stdcall_t, VOID, UINT> g_exitProcessHook;
static Hook<CallConvention::stdcall_t, void, int> g_postQuitMessageHook;
static Hook<CallConvention::stdcall_t, BOOL, HMODULE> g_freeLibraryHook;

//
// Per-run worker state. File scope so hook lambdas can reach it and so every
// worker run (initial hook or rehook) starts from a clean slate.
//
static PHYDRAHOOK_ENGINE engine = nullptr;

/// Once-per-hook announcements; re-armed at the start of every worker run.
static struct HookAnnouncements
{
	ResettableOnce present9, reset9, endScene9, present9Ex, reset9Ex;
	ResettableOnce present10, resizeTarget10, resizeBuffers10;
	ResettableOnce present11, resizeTarget11, resizeBuffers11;
	ResettableOnce present12, resizeTarget12, resizeBuffers12;
	ResettableOnce present1D3D12, present1D3D11, present1D3D10;
	ResettableOnce resizeBuffers1D3D12, resizeBuffers1D3D11, resizeBuffers1D3D10;
	ResettableOnce arcGetBuffer, arcReleaseBuffer;

	void reset_all()
	{
		present9.reset(); reset9.reset(); endScene9.reset(); present9Ex.reset(); reset9Ex.reset();
		present10.reset(); resizeTarget10.reset(); resizeBuffers10.reset();
		present11.reset(); resizeTarget11.reset(); resizeBuffers11.reset();
		present12.reset(); resizeTarget12.reset(); resizeBuffers12.reset();
		present1D3D12.reset(); present1D3D11.reset(); present1D3D10.reset();
		resizeBuffers1D3D12.reset(); resizeBuffers1D3D11.reset(); resizeBuffers1D3D10.reset();
		arcGetBuffer.reset(); arcReleaseBuffer.reset();
	}
} g_announce;

void PerformShutdownCleanup(PHYDRAHOOK_ENGINE engine, ShutdownOrigin origin)
{
	if (engine->ShutdownCleanupDone.exchange(true))
		return;

	auto logChannel = "shutdown";
	auto logMessage = "Performing pre-DLL-detach clean-up tasks";
	switch (origin)
	{
	case ShutdownOrigin::ExitProcessHook:
		logChannel = "process";
		logMessage = "Host process is terminating, performing pre-DLL-detach clean-up tasks";
		break;
	case ShutdownOrigin::PostQuitMessageHook:
		logChannel = "quit";
		logMessage = "WM_QUIT was fired, performing pre-DLL-detach clean-up tasks";
		break;
	case ShutdownOrigin::FreeLibraryHook:
		logChannel = "unload";
		logMessage = "FreeLibrary called for HydraHook DLL, performing pre-unload clean-up tasks";
		break;
	case ShutdownOrigin::DllMainProcessDetach:
		logChannel = "detach";
		break;
	case ShutdownOrigin::ManualUnhook:
		logChannel = "unhook";
		logMessage = "HydraHookEngineUnhook called, removing hooks and stopping the worker";
		break;
	}

	auto logger = spdlog::get("HYDRAHOOK")->clone(logChannel);
	logger->info(logMessage);

	if (origin == ShutdownOrigin::ExitProcessHook)
	{
		g_postQuitMessageHook.remove_nothrow();
		g_freeLibraryHook.remove_nothrow();
	}
	else if (origin == ShutdownOrigin::PostQuitMessageHook)
	{
		g_exitProcessHook.remove_nothrow();
		g_freeLibraryHook.remove_nothrow();
	}
	else if (origin == ShutdownOrigin::FreeLibraryHook)
	{
		g_postQuitMessageHook.remove_nothrow();
		g_exitProcessHook.remove_nothrow();
	}
	else if (origin == ShutdownOrigin::DllMainProcessDetach)
	{
		g_postQuitMessageHook.remove_nothrow();
		g_exitProcessHook.remove_nothrow();
	}

	else if (origin == ShutdownOrigin::ManualUnhook)
	{
		// The worker removes every hook (including the flow-control ones) on its own
		// thread; touching Detours from here could race a worker still installing hooks.
		engine->SkipDllUnloadOnExit.store(true, std::memory_order_release);
	}

	if (origin != ShutdownOrigin::DllMainProcessDetach && origin != ShutdownOrigin::ManualUnhook
		&& engine->EngineConfig.EvtHydraHookGamePreExit)
	{
		engine->EngineConfig.EvtHydraHookGamePreExit(engine);
	}

	const auto ret = SetEvent(engine->EngineCancellationEvent);
	if (!ret)
	{
		logger->error("SetEvent failed: {}", GetLastError());
	}

	// wait on thread shutdown or check state
	// A manual unhook waits long enough for HookActivityTracker::drain (5000 ms)
	const DWORD waitTimeout = origin == ShutdownOrigin::DllMainProcessDetach
		                          ? 0
		                          : (origin == ShutdownOrigin::ManualUnhook ? 8000 : 3000);
	const auto result = WaitForSingleObject(engine->EngineThread, waitTimeout);

	switch (result)
	{
	case WAIT_ABANDONED:
		logger->error("Unknown state, host process might crash");
		break;
	case WAIT_OBJECT_0:
		logger->info("Thread shutdown complete");
		break;
	case WAIT_TIMEOUT:
		if (origin == ShutdownOrigin::ManualUnhook)
		{
			// Never terminate on a manual unhook: the caller keeps the engine and can retry
			logger->error("Thread hasn't finished clean-up within expected time, unhook incomplete");
		}
		else if (origin != ShutdownOrigin::DllMainProcessDetach)
		{
			TerminateThread(engine->EngineThread, 0);
			logger->error("Thread hasn't finished clean-up within expected time, terminating");
		}
		break;
	case WAIT_FAILED:
		logger->error("Unknown error, host process might crash");
		break;
	default:
		if (origin != ShutdownOrigin::DllMainProcessDetach && origin != ShutdownOrigin::ManualUnhook)
		{
			TerminateThread(engine->EngineThread, 0);
		}
		logger->error("Unexpected return value");
		break;
	}
}

// NOTE: DirectInput hooking is technically implemented but not really useful
// #define HOOK_DINPUT8

#ifdef HOOK_DINPUT8
// DInput8
Hook<CallConvention::stdcall_t, HRESULT, LPDIRECTINPUTDEVICE8> g_acquire8Hook;
Hook<CallConvention::stdcall_t, HRESULT, LPDIRECTINPUTDEVICE8, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD>
g_getDeviceData8Hook;
Hook<CallConvention::stdcall_t, HRESULT, LPDIRECTINPUTDEVICE8, LPDIDEVICEINSTANCE> g_getDeviceInfo8Hook;
Hook<CallConvention::stdcall_t, HRESULT, LPDIRECTINPUTDEVICE8, DWORD, LPVOID> g_getDeviceState8Hook;
Hook<CallConvention::stdcall_t, HRESULT, LPDIRECTINPUTDEVICE8, LPDIDEVICEOBJECTINSTANCE, DWORD, DWORD>
g_getObjectInfo8Hook;

void HookDInput8(size_t* vtable8);
#endif

DWORD WINAPI HydraHookMainThread(LPVOID Params)
{
	auto logger = spdlog::get("HYDRAHOOK")->clone("game");
	engine = reinterpret_cast<PHYDRAHOOK_ENGINE>(Params);
	g_announce.reset_all();
	const auto& config = engine->EngineConfig;

	if (config.CrashHandler.IsEnabled)
	{
		HydraHookCrashHandlerInstallThreadSEH();
		logger->info("Per-thread SEH translator installed on engine worker thread");
	}

	logger->info("Library loaded into {}", HydraHook::Core::Util::process_name());

	logger->info("Library enabled");

	// 
	// D3D9 Hooks
	// 
#ifndef HYDRAHOOK_NO_D3D9
	static Hook<CallConvention::stdcall_t, HRESULT, LPDIRECT3DDEVICE9, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*>
		present9Hook;
	static Hook<CallConvention::stdcall_t, HRESULT, LPDIRECT3DDEVICE9, D3DPRESENT_PARAMETERS*> reset9Hook;
	static Hook<CallConvention::stdcall_t, HRESULT, LPDIRECT3DDEVICE9> endScene9Hook;

	// 
	// D3D9Ex Hooks
	// 
	static Hook<CallConvention::stdcall_t, HRESULT, LPDIRECT3DDEVICE9EX, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*,
	            DWORD> present9ExHook;
	static Hook<CallConvention::stdcall_t, HRESULT, LPDIRECT3DDEVICE9EX, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*>
		reset9ExHook;
#else
	logger->info("Direct3D 9 hooking disabled at compile time");
#endif

	//
	// DXGI Hooks (shared across D3D10/11/12; one Detours hook per vtable slot)
	//
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain*, UINT, UINT> swapChainPresentHook;
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain*, const DXGI_MODE_DESC*> swapChainResizeTargetHook;
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT>
		swapChainResizeBuffersHook;
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*>
		swapChainPresent1Hook;
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
	            IUnknown* const*> swapChainResizeBuffers1Hook;

#ifdef HYDRAHOOK_NO_D3D10
	logger->info("Direct3D 10 hooking disabled at compile time");
#endif
#ifdef HYDRAHOOK_NO_D3D11
	logger->info("Direct3D 11 hooking disabled at compile time");
#endif

	// 
	// D3D12 Queue Capture Hooks
	// 
#ifndef HYDRAHOOK_NO_D3D12
	static Hook<CallConvention::stdcall_t, HRESULT, IUnknown*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**>
		createSwapChain12Hook;
	static Hook<CallConvention::stdcall_t, HRESULT, IUnknown*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const
	            DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**> createSwapChainForHwnd12Hook;
	static Hook<CallConvention::stdcall_t, void, ID3D12CommandQueue*, UINT, ID3D12CommandList* const*>
		executeCommandLists12Hook;
#else
	logger->info("Direct3D 12 hooking disabled at compile time");
#endif

	// 
	// Core Audio Hooks
	// 
#ifndef HYDRAHOOK_NO_COREAUDIO
	static Hook<CallConvention::stdcall_t, HRESULT, IAudioRenderClient*, UINT32, BYTE**> arcGetBufferHook;
	static Hook<CallConvention::stdcall_t, HRESULT, IAudioRenderClient*, UINT32, DWORD> arcReleaseBufferHook;
#else
	logger->info("Core Audio hooking disabled at compile time");
#endif

	/*
	 * This is a bit of a gamble but ExitProcess is expected to be implicitly called
	 * _before_ the injected DLL gets unloaded (without proper call to FreeLibrary)
	 * and by hooking it we get a chance to gracefully shutdown and free resources
	 * which might otherwise become victim to a termination race condition and DLL
	 * loader-lock restrictions.
	 */
	try
	{
		if (!g_exitProcessHook.is_applied())
		g_exitProcessHook.apply((size_t)ExitProcess, [](UINT uExitCode)
		{
			PerformShutdownCleanup(engine, ShutdownOrigin::ExitProcessHook);
			g_exitProcessHook.call_orig(uExitCode);
		});
	}
	catch (DetourException& ex)
	{
		logger->error("Failed to hook ExitProcess: {}", ex.what());
	}

	/*
	 * Hooking PostQuitMessage in addition to ExitProcess should be practically
	 * more reliable since a game is expected to have at least one main window
	 * which _should_ receive the WM_QUIT message upon application shutdown.
	 */
	try
	{
		if (!g_postQuitMessageHook.is_applied())
		g_postQuitMessageHook.apply((size_t)PostQuitMessage, [](int nExitCode)
		{
			PerformShutdownCleanup(engine, ShutdownOrigin::PostQuitMessageHook);
			g_postQuitMessageHook.call_orig(nExitCode);
		});
	}
	catch (DetourException& ex)
	{
		logger->error("Failed to hook PostQuitMessage: {}", ex.what());
	}

	/*
	 * Hooking FreeLibrary allows detection of explicit host-initiated DLL unload.
	 * When the host calls FreeLibrary on our module, we perform graceful shutdown
	 * before the actual unload, avoiding loader-lock and termination race issues.
	 */
	try
	{
		if (!g_freeLibraryHook.is_applied())
		g_freeLibraryHook.apply((size_t)FreeLibrary, [](HMODULE hLibModule) -> BOOL
		{
			if (hLibModule == engine->DllModule)
			{
				engine->FreeLibraryHookActive.store(true, std::memory_order_release);
				PerformShutdownCleanup(engine, ShutdownOrigin::FreeLibraryHook);
				FreeLibraryAndExitThread(hLibModule, 0);
			}
			return g_freeLibraryHook.call_orig(hLibModule);
		});
	}
	catch (DetourException& ex)
	{
		logger->error("Failed to hook FreeLibrary: {}", ex.what());
	}

#pragma region D3D9

	/*
	 * The following section is disabled because hooking IDirect3DDevice9Ex functions
	 * should work on "vanilla" D3D9 (like Half-Life 2) equally well while also supporting
	 * both windowed and full-screen mode without modifications. Section will be left here
	 * for experiments and tests.
	 */
#ifdef D3D9_LEGACY_HOOKING

	try
	{
		AutoPtr<Direct3D9Hooking::Direct3D9> d3d(new Direct3D9Hooking::Direct3D9);

		BOOST_LOG_TRIVIAL(info) << "Hooking IDirect3DDevice9::Present"
		)
		;

		present9Hook.apply(d3d->vtable()[Direct3D9Hooking::Present], [](
		                   LPDIRECT3DDEVICE9 dev,
		                   CONST RECT* a1,
		                   CONST RECT* a2,
		                   HWND a3,
		                   CONST RGNDATA* a4
	                   ) -> HRESULT
		                   {
			                   static std::once_flag flag;
			                   std::call_once(flag, []()
			                   {
				                   Logger::get("HookDX9").information("++ IDirect3DDevice9::Present called");

				                   INVOKE_HYDRAHOOK_GAME_HOOKED(engine, HydraHookDirect3DVersion9);
			                   });

			                   INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PrePresent, dev, a1, a2, a3, a4);

			                   auto ret = present9Hook.callOrig(dev, a1, a2, a3, a4);

			                   INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PostPresent, dev, a1, a2, a3, a4);

			                   return ret;
		                   });

		BOOST_LOG_TRIVIAL(info) << "Hooking IDirect3DDevice9::Reset"
		)
		;

		reset9Hook.apply(d3d->vtable()[Direct3D9Hooking::Reset], [](
		                 LPDIRECT3DDEVICE9 dev,
		                 D3DPRESENT_PARAMETERS* pp
	                 ) -> HRESULT
		                 {
			                 static std::once_flag flag;
			                 std::call_once(flag, []()
			                 {
				                 Logger::get("HookDX9").information("++ IDirect3DDevice9::Reset called");
			                 });

			                 INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PreReset, dev, pp);

			                 auto ret = reset9Hook.callOrig(dev, pp);

			                 INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PostReset, dev, pp);

			                 return ret;
		                 });

		BOOST_LOG_TRIVIAL(info) << "Hooking IDirect3DDevice9::EndScene"
		)
		;

		endScene9Hook.apply(d3d->vtable()[Direct3D9Hooking::EndScene], [](
		                    LPDIRECT3DDEVICE9 dev
	                    ) -> HRESULT
		                    {
			                    static std::once_flag flag;
			                    std::call_once(flag, []()
			                    {
				                    Logger::get("HookDX9").information(
					                    "++ IDirect3DDevice9::EndScene called");
			                    });

			                    INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PreEndScene, dev);

			                    auto ret = endScene9Hook.callOrig(dev);

			                    INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PostEndScene, dev);

			                    return ret;
		                    });
	}
	catch (Poco::Exception& pex)
	{
		logger.error("Hooking D3D9 failed: %s", pex.displayText());
	}

#endif

#ifndef HYDRAHOOK_NO_D3D9

	if (config.Direct3D.HookDirect3D9)
	{
		try
		{
			const std::unique_ptr<Direct3D9Hooking::Direct3D9Ex> d3dEx(new Direct3D9Hooking::Direct3D9Ex);

			logger->info("Hooking IDirect3DDevice9Ex::Present");

			present9Hook.apply(d3dEx->vtable()[Direct3D9Hooking::Present], [](
			                   LPDIRECT3DDEVICE9 dev,
			                   CONST RECT* a1,
			                   CONST RECT* a2,
			                   HWND a3,
			                   CONST RGNDATA* a4
		                   ) -> HRESULT
			                   {
				                   HookActivityTracker::Guard guard;

				                   if (guard.invoke)
				                   {
					                   g_announce.present9.call([&pDev = dev]()
					                   {
						                   spdlog::get("HYDRAHOOK")->clone("d3d9")->info(
							                   "++ IDirect3DDevice9Ex::Present called");

						                   engine->RenderPipeline.pD3D9Device = pDev;

						                   INVOKE_HYDRAHOOK_GAME_HOOKED(engine, HydraHookDirect3DVersion9);
					                   });

					                   INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PrePresent, dev, a1, a2, a3, a4);
				                   }

				                   const auto ret = present9Hook.call_orig(dev, a1, a2, a3, a4);

				                   if (guard.invoke)
				                   {
					                   INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PostPresent, dev, a1, a2, a3, a4);
				                   }

				                   return ret;
			                   });

			logger->info("Hooking IDirect3DDevice9Ex::Reset");

			reset9Hook.apply(d3dEx->vtable()[Direct3D9Hooking::Reset], [](
			                 LPDIRECT3DDEVICE9 dev,
			                 D3DPRESENT_PARAMETERS* pp
		                 ) -> HRESULT
			                 {
				                 HookActivityTracker::Guard guard;

				                 if (guard.invoke)
				                 {
					                 g_announce.reset9.call([]()
					                 {
						                 spdlog::get("HYDRAHOOK")->clone("d3d9")->info(
							                 "++ IDirect3DDevice9Ex::Reset called");
					                 });

					                 INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PreReset, dev, pp);
				                 }

				                 const auto ret = reset9Hook.call_orig(dev, pp);

				                 if (guard.invoke)
				                 {
					                 INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PostReset, dev, pp);
				                 }

				                 return ret;
			                 });

			logger->info("Hooking IDirect3DDevice9Ex::EndScene");

			endScene9Hook.apply(d3dEx->vtable()[Direct3D9Hooking::EndScene], [](
			                    LPDIRECT3DDEVICE9 dev
		                    ) -> HRESULT
			                    {
				                    HookActivityTracker::Guard guard;

				                    if (guard.invoke)
				                    {
					                    g_announce.endScene9.call([]()
					                    {
						                    spdlog::get("HYDRAHOOK")->clone("d3d9")->info(
							                    "++ IDirect3DDevice9Ex::EndScene called");
					                    });

					                    INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PreEndScene, dev);
				                    }

				                    const auto ret = endScene9Hook.call_orig(dev);

				                    if (guard.invoke)
				                    {
					                    INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PostEndScene, dev);
				                    }

				                    return ret;
			                    });

			logger->info("Hooking IDirect3DDevice9Ex::PresentEx");

			present9ExHook.apply(d3dEx->vtable()[Direct3D9Hooking::PresentEx], [](
			                     LPDIRECT3DDEVICE9EX dev,
			                     CONST RECT* a1,
			                     CONST RECT* a2,
			                     HWND a3,
			                     CONST RGNDATA* a4,
			                     DWORD a5
		                     ) -> HRESULT
			                     {
				                     HookActivityTracker::Guard guard;

				                     if (guard.invoke)
				                     {
					                     g_announce.present9Ex.call([&pDev = dev]()
					                     {
						                     spdlog::get("HYDRAHOOK")->clone("d3d9")->info(
							                     "++ IDirect3DDevice9Ex::PresentEx called");

						                     engine->RenderPipeline.pD3D9ExDevice = pDev;

						                     INVOKE_HYDRAHOOK_GAME_HOOKED(engine, HydraHookDirect3DVersion9);
					                     });

					                     INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PrePresentEx, dev, a1, a2, a3, a4,
					                                          a5);
				                     }

				                     const auto ret = present9ExHook.call_orig(dev, a1, a2, a3, a4, a5);

				                     if (guard.invoke)
				                     {
					                     INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PostPresentEx, dev, a1, a2, a3,
					                                          a4,
					                                          a5);
				                     }

				                     return ret;
			                     });

			logger->info("Hooking IDirect3DDevice9Ex::ResetEx");

			reset9ExHook.apply(d3dEx->vtable()[Direct3D9Hooking::ResetEx], [](
			                   LPDIRECT3DDEVICE9EX dev,
			                   D3DPRESENT_PARAMETERS* pp,
			                   D3DDISPLAYMODEEX* ppp
		                   ) -> HRESULT
			                   {
				                   HookActivityTracker::Guard guard;

				                   if (guard.invoke)
				                   {
					                   g_announce.reset9Ex.call([]()
					                   {
						                   spdlog::get("HYDRAHOOK")->clone("d3d9")->info(
							                   "++ IDirect3DDevice9Ex::ResetEx called");
					                   });

					                   INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PreResetEx, dev, pp, ppp);
				                   }

				                   const auto ret = reset9ExHook.call_orig(dev, pp, ppp);

				                   if (guard.invoke)
				                   {
					                   INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PostResetEx, dev, pp, ppp);
				                   }

				                   return ret;
			                   });
		}
		catch (DetourException& ex)
		{
			logger->error("Hooking D3D9Ex failed: {}", ex.what());
		}
		catch (ModuleNotFoundException& ex)
		{
			logger->warn("Module not found: {}", ex.what());
		}
		catch (RuntimeException& ex)
		{
			logger->error("D3D9(Ex) runtime error: {}", ex.what());
		}
	}

#endif

#pragma endregion

#pragma region DXGI / D3D10 / D3D11 / D3D12

	size_t dxgiPresentAddress = 0;
	size_t dxgiResizeTargetAddress = 0;
	size_t dxgiResizeBuffersAddress = 0;
	size_t dxgiPresent1Address = 0;
	size_t dxgiResizeBuffers1Address = 0;

	const auto captureDxgiVtable = [&](const std::vector<size_t>& vtable)
	{
		if (dxgiPresentAddress == 0 && vtable.size() > static_cast<size_t>(DXGIHooking::Present))
			dxgiPresentAddress = vtable[DXGIHooking::Present];
		if (dxgiResizeTargetAddress == 0 && vtable.size() > static_cast<size_t>(DXGIHooking::ResizeTarget))
			dxgiResizeTargetAddress = vtable[DXGIHooking::ResizeTarget];
		if (dxgiResizeBuffersAddress == 0 && vtable.size() > static_cast<size_t>(DXGIHooking::ResizeBuffers))
			dxgiResizeBuffersAddress = vtable[DXGIHooking::ResizeBuffers];
		if (dxgiPresent1Address == 0 && vtable.size() > static_cast<size_t>(
			DXGIHooking::DXGI1::DXGISwapChain1VTbl::Present1))
			dxgiPresent1Address = vtable[DXGIHooking::DXGI1::Present1];
		if (dxgiResizeBuffers1Address == 0 && vtable.size() > static_cast<size_t>(
			DXGIHooking::DXGI3::ResizeBuffers1))
			dxgiResizeBuffers1Address = vtable[DXGIHooking::DXGI3::ResizeBuffers1];
	};

#ifndef HYDRAHOOK_NO_D3D12
	size_t d3d12ExecuteCommandListsAddress = 0;

	if (config.Direct3D.HookDirect3D12)
	{
		try
		{
			const std::unique_ptr<Direct3D12Hooking::Direct3D12> d3d12(new Direct3D12Hooking::Direct3D12);
			captureDxgiVtable(d3d12->vtable());

			if (void** pQueueVtbl = d3d12->commandQueueVtable())
			{
				constexpr int ExecuteCommandListsIndex = 10;
				d3d12ExecuteCommandListsAddress = reinterpret_cast<size_t>(pQueueVtbl[ExecuteCommandListsIndex]);
			}
		}
		catch (ModuleNotFoundException& ex)
		{
			logger->warn("Module not found: {}", ex.what());
		}
		catch (RuntimeException& ex)
		{
			logger->error("D3D12 runtime error: {}", ex.what());
		}
	}
#endif

#ifndef HYDRAHOOK_NO_D3D11
	if (config.Direct3D.HookDirect3D11 && dxgiPresentAddress == 0)
	{
		try
		{
			const std::unique_ptr<Direct3D11Hooking::Direct3D11> d3d11(new Direct3D11Hooking::Direct3D11);
			captureDxgiVtable(d3d11->vtable());
		}
		catch (ModuleNotFoundException& ex)
		{
			logger->warn("Module not found: {}", ex.what());
		}
		catch (RuntimeException& ex)
		{
			logger->error("D3D11 runtime error: {}", ex.what());
		}
	}
#endif

#ifndef HYDRAHOOK_NO_D3D10
	if (config.Direct3D.HookDirect3D10 && dxgiPresentAddress == 0)
	{
		try
		{
			const std::unique_ptr<Direct3D10Hooking::Direct3D10> d3d10(new Direct3D10Hooking::Direct3D10);
			captureDxgiVtable(d3d10->vtable());
		}
		catch (ModuleNotFoundException& ex)
		{
			logger->warn("Module not found: {}", ex.what());
		}
		catch (RuntimeException& ex)
		{
			logger->error("D3D10 runtime error: {}", ex.what());
		}
	}
#endif

#ifndef HYDRAHOOK_NO_D3D12
	if (config.Direct3D.HookDirect3D12)
	{
		try
		{
			// Installed after all temporary probe swap chains above have been destroyed
			// so our own dummy swap chain is never captured into g_d3d12SwapChainToQueue.
			IDXGIFactory2* pFactory = nullptr;
			HMODULE hModDXGI = GetModuleHandleW(L"dxgi.dll");
			HRESULT hrFactory = E_FAIL;
			if (hModDXGI)
			{
				auto pCreateDXGIFactory1 = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
					GetProcAddress(hModDXGI, "CreateDXGIFactory1"));
				if (pCreateDXGIFactory1)
					hrFactory = pCreateDXGIFactory1(IID_PPV_ARGS(&pFactory));
			}
			if (SUCCEEDED(hrFactory) && pFactory)
			{
				void** pFactoryVtbl = *reinterpret_cast<void***>(pFactory);
				constexpr int CreateSwapChainIndex = 10;
				constexpr int CreateSwapChainForHwndIndex = 14;
				const auto createSwapChainAddr = reinterpret_cast<size_t>(pFactoryVtbl[CreateSwapChainIndex]);
				const auto createSwapChainForHwndAddr = reinterpret_cast<size_t>(
					pFactoryVtbl[CreateSwapChainForHwndIndex]);
				pFactory->Release();
				pFactory = nullptr;

				if (!createSwapChain12Hook.is_applied())
				createSwapChain12Hook.apply(createSwapChainAddr, [](
				                            IUnknown* pFactoryThis,
				                            IUnknown* pDevice,
				                            DXGI_SWAP_CHAIN_DESC* pDesc,
				                            IDXGISwapChain** ppSwapChain
			                            ) -> HRESULT
				                            {
					                            HookActivityTracker::Guard guard;

					                            const auto ret = createSwapChain12Hook.call_orig(
						                            pFactoryThis, pDevice, pDesc, ppSwapChain);
					                            if (guard.invoke && SUCCEEDED(ret) && ppSwapChain && *ppSwapChain &&
						                            pDevice)
					                            {
						                            ID3D12CommandQueue* pQueue = nullptr;
						                            if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue))))
						                            {
							                            std::lock_guard<std::mutex> lock(g_d3d12QueueMapMutex);
							                            auto it = g_d3d12SwapChainToQueue.find(*ppSwapChain);
							                            if (it != g_d3d12SwapChainToQueue.end() && it->second)
								                            it->second->Release();
							                            g_d3d12SwapChainToQueue[*ppSwapChain] = pQueue;
						                            }
					                            }
					                            return ret;
				                            });

				if (!createSwapChainForHwnd12Hook.is_applied())
				createSwapChainForHwnd12Hook.apply(createSwapChainForHwndAddr,
				                                   [](
				                                   IUnknown* pFactoryThis,
				                                   IUnknown* pDevice,
				                                   HWND hWnd,
				                                   const DXGI_SWAP_CHAIN_DESC1* pDesc,
				                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
				                                   IDXGIOutput* pRestrictToOutput,
				                                   IDXGISwapChain1** ppSwapChain
			                                   ) -> HRESULT
				                                   {
					                                   HookActivityTracker::Guard guard;

					                                   const auto ret = createSwapChainForHwnd12Hook.call_orig(
						                                   pFactoryThis, pDevice, hWnd, pDesc, pFullscreenDesc,
						                                   pRestrictToOutput, ppSwapChain);
					                                   if (guard.invoke && SUCCEEDED(ret) && ppSwapChain && *ppSwapChain
						                                   && pDevice)
					                                   {
						                                   ID3D12CommandQueue* pQueue = nullptr;
						                                   if (SUCCEEDED(
							                                   pDevice->QueryInterface(IID_PPV_ARGS(&pQueue))))
						                                   {
							                                   std::lock_guard<std::mutex> lock(g_d3d12QueueMapMutex);
							                                   auto it = g_d3d12SwapChainToQueue.find(*ppSwapChain);
							                                   if (it != g_d3d12SwapChainToQueue.end() && it->second)
								                                   it->second->Release();
							                                   g_d3d12SwapChainToQueue[*ppSwapChain] = pQueue;
						                                   }
					                                   }
					                                   return ret;
				                                   });

				logger->info("Hooking IDXGIFactory::CreateSwapChain/CreateSwapChainForHwnd for D3D12 queue capture");
			}

			if (d3d12ExecuteCommandListsAddress != 0 && !executeCommandLists12Hook.is_applied())
			{
				executeCommandLists12Hook.apply(d3d12ExecuteCommandListsAddress, [](
				                                ID3D12CommandQueue* pQueue,
				                                UINT NumCommandLists,
				                                ID3D12CommandList* const* ppCommandLists
			                                ) -> void
				                                {
					                                HookActivityTracker::Guard guard;

					                                if (guard.invoke && pQueue
						                                && pQueue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
					                                {
						                                ID3D12Device* pDevice = nullptr;
						                                if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&pDevice))) &&
							                                pDevice)
						                                {
							                                std::lock_guard<std::mutex> lock(g_d3d12QueueMapMutex);
							                                auto it = g_d3d12DeviceToQueue.find(pDevice);
							                                if (it == g_d3d12DeviceToQueue.end() || it->second != pQueue)
							                                {
								                                if (it != g_d3d12DeviceToQueue.end() && it->second)
									                                it->second->Release();
								                                pQueue->AddRef();
								                                g_d3d12DeviceToQueue[pDevice] = pQueue;
							                                }
							                                pDevice->Release();
						                                }
					                                }
					                                executeCommandLists12Hook.call_orig(
						                                pQueue, NumCommandLists, ppCommandLists);
				                                });
				logger->info("Hooking ID3D12CommandQueue::ExecuteCommandLists for runtime queue capture");
			}
		}
		catch (DetourException& ex)
		{
			logger->error("Hooking D3D12 queue capture failed: {}", ex.what());
		}
	}
#endif

	// D3D10, D3D11, and D3D12 share the same DXGI CDXGISwapChain implementation.
	// Install at most one Detours hook per vtable slot and dispatch by swap chain device;
	// stacking multiple Detours hooks on the same target corrupts the prologue if detached out of order.
	try
	{
		if (dxgiPresentAddress != 0 && !swapChainPresentHook.is_applied())
		{
			logger->info("Hooking IDXGISwapChain::Present");

			swapChainPresentHook.apply(dxgiPresentAddress, [](
			                           IDXGISwapChain* chain,
			                           UINT SyncInterval,
			                           UINT Flags
		                           ) -> HRESULT
			                           {
				                           HookActivityTracker::Guard guard;

				                           if (!guard.invoke)
					                           return swapChainPresentHook.call_orig(chain, SyncInterval, Flags);

#ifndef HYDRAHOOK_NO_D3D12
				                           if (engine->EngineConfig.Direct3D.HookDirect3D12)
				                           {
					                           ID3D12Device* pD12Device = nullptr;
					                           if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD12Device))) && pD12Device)
					                           {
						                           pD12Device->Release();

						                           g_announce.present12.call([&pChain = chain]()
						                           {
							                           spdlog::get("HYDRAHOOK")->clone("d3d12")->info(
								                           "++ IDXGISwapChain::Present called");

							                           engine->RenderPipeline.pSwapChain = pChain;

							                           INVOKE_HYDRAHOOK_GAME_HOOKED(engine, HydraHookDirect3DVersion12);
						                           });

						                           HYDRAHOOK_EVT_PRE_EXTENSION pre;
						                           HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
							                           &pre, engine, engine->CustomContext);

						                           INVOKE_D3D12_CALLBACK(engine, EvtHydraHookD3D12PrePresent, chain,
						                                                 SyncInterval, Flags, &pre);

						                           const auto ret = swapChainPresentHook.call_orig(
							                           chain, SyncInterval, Flags);

						                           HYDRAHOOK_EVT_POST_EXTENSION post;
						                           HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                           &post, engine, engine->CustomContext);

						                           INVOKE_D3D12_CALLBACK(engine, EvtHydraHookD3D12PostPresent, chain,
						                                                 SyncInterval, Flags, &post);

						                           return ret;
					                           }
				                           }
#endif

#ifndef HYDRAHOOK_NO_D3D11
				                           if (engine->EngineConfig.Direct3D.HookDirect3D11)
				                           {
					                           ID3D11Device* pD11Device = nullptr;
					                           if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD11Device))) && pD11Device)
					                           {
						                           pD11Device->Release();

						                           g_announce.present11.call([&pChain = chain]()
						                           {
							                           spdlog::get("HYDRAHOOK")->clone("d3d11")->info(
								                           "++ IDXGISwapChain::Present called");

							                           engine->RenderPipeline.pSwapChain = pChain;

							                           INVOKE_HYDRAHOOK_GAME_HOOKED(engine, HydraHookDirect3DVersion11);
						                           });

						                           HYDRAHOOK_EVT_PRE_EXTENSION pre;
						                           HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
							                           &pre, engine, engine->CustomContext);

						                           INVOKE_D3D11_CALLBACK(
							                           engine,
							                           EvtHydraHookD3D11PrePresent,
							                           chain,
							                           SyncInterval,
							                           Flags,
							                           &pre
						                           );

						                           const auto ret = swapChainPresentHook.call_orig(
							                           chain, SyncInterval, Flags);

						                           HYDRAHOOK_EVT_POST_EXTENSION post;
						                           HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                           &post, engine, engine->CustomContext);

						                           INVOKE_D3D11_CALLBACK(
							                           engine,
							                           EvtHydraHookD3D11PostPresent,
							                           chain,
							                           SyncInterval,
							                           Flags,
							                           &post
						                           );

						                           return ret;
					                           }
				                           }
#endif

#ifndef HYDRAHOOK_NO_D3D10
				                           if (engine->EngineConfig.Direct3D.HookDirect3D10)
				                           {
					                           ID3D10Device* pD10Device = nullptr;
					                           if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD10Device))) && pD10Device)
					                           {
						                           pD10Device->Release();

						                           g_announce.present10.call([&pChain = chain]()
						                           {
							                           spdlog::get("HYDRAHOOK")->clone("d3d10")->info(
								                           "++ IDXGISwapChain::Present called");

							                           engine->RenderPipeline.pSwapChain = pChain;

							                           INVOKE_HYDRAHOOK_GAME_HOOKED(engine, HydraHookDirect3DVersion10);
						                           });

						                           INVOKE_D3D10_CALLBACK(engine, EvtHydraHookD3D10PrePresent, chain,
						                                                 SyncInterval, Flags);

						                           const auto ret = swapChainPresentHook.call_orig(
							                           chain, SyncInterval, Flags);

						                           INVOKE_D3D10_CALLBACK(engine, EvtHydraHookD3D10PostPresent, chain,
						                                                 SyncInterval, Flags);

						                           return ret;
					                           }
				                           }
#endif

				                           return swapChainPresentHook.call_orig(chain, SyncInterval, Flags);
			                           });
		}

		if (dxgiResizeTargetAddress != 0 && !swapChainResizeTargetHook.is_applied())
		{
			logger->info("Hooking IDXGISwapChain::ResizeTarget");

			swapChainResizeTargetHook.apply(dxgiResizeTargetAddress, [](
			                                IDXGISwapChain* chain,
			                                const DXGI_MODE_DESC* pNewTargetParameters
		                                ) -> HRESULT
			                                {
				                                HookActivityTracker::Guard guard;

				                                if (!guard.invoke)
					                                return swapChainResizeTargetHook.call_orig(
						                                chain, pNewTargetParameters);

#ifndef HYDRAHOOK_NO_D3D12
				                                if (engine->EngineConfig.Direct3D.HookDirect3D12)
				                                {
					                                ID3D12Device* pD12Device = nullptr;
					                                if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD12Device))) &&
						                                pD12Device)
					                                {
						                                pD12Device->Release();

						                                g_announce.resizeTarget12.call([]()
						                                {
							                                spdlog::get("HYDRAHOOK")->clone("d3d12")->info(
								                                "++ IDXGISwapChain::ResizeTarget called");
						                                });

						                                HYDRAHOOK_EVT_PRE_EXTENSION pre;
						                                HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
							                                &pre, engine, engine->CustomContext);

						                                INVOKE_D3D12_CALLBACK(
							                                engine, EvtHydraHookD3D12PreResizeTarget, chain,
							                                pNewTargetParameters, &pre);

						                                const auto ret = swapChainResizeTargetHook.call_orig(
							                                chain, pNewTargetParameters);

						                                HYDRAHOOK_EVT_POST_EXTENSION post;
						                                HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                                &post, engine, engine->CustomContext);

						                                INVOKE_D3D12_CALLBACK(
							                                engine, EvtHydraHookD3D12PostResizeTarget, chain,
							                                pNewTargetParameters, &post);

						                                return ret;
					                                }
				                                }
#endif

#ifndef HYDRAHOOK_NO_D3D11
				                                if (engine->EngineConfig.Direct3D.HookDirect3D11)
				                                {
					                                ID3D11Device* pD11Device = nullptr;
					                                if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD11Device))) &&
						                                pD11Device)
					                                {
						                                pD11Device->Release();

						                                g_announce.resizeTarget11.call([]()
						                                {
							                                spdlog::get("HYDRAHOOK")->clone("d3d11")->info(
								                                "++ IDXGISwapChain::ResizeTarget called");
						                                });

						                                HYDRAHOOK_EVT_PRE_EXTENSION pre;
						                                HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
							                                &pre, engine, engine->CustomContext);

						                                INVOKE_D3D11_CALLBACK(
							                                engine,
							                                EvtHydraHookD3D11PreResizeTarget,
							                                chain,
							                                pNewTargetParameters,
							                                &pre
						                                );

						                                const auto ret = swapChainResizeTargetHook.call_orig(
							                                chain, pNewTargetParameters);

						                                HYDRAHOOK_EVT_POST_EXTENSION post;
						                                HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                                &post, engine, engine->CustomContext);

						                                INVOKE_D3D11_CALLBACK(
							                                engine,
							                                EvtHydraHookD3D11PostResizeTarget,
							                                chain,
							                                pNewTargetParameters,
							                                &post
						                                );

						                                return ret;
					                                }
				                                }
#endif

#ifndef HYDRAHOOK_NO_D3D10
				                                if (engine->EngineConfig.Direct3D.HookDirect3D10)
				                                {
					                                ID3D10Device* pD10Device = nullptr;
					                                if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD10Device))) &&
						                                pD10Device)
					                                {
						                                pD10Device->Release();

						                                g_announce.resizeTarget10.call([]()
						                                {
							                                spdlog::get("HYDRAHOOK")->clone("d3d10")->info(
								                                "++ IDXGISwapChain::ResizeTarget called");
						                                });

						                                INVOKE_D3D10_CALLBACK(
							                                engine, EvtHydraHookD3D10PreResizeTarget, chain,
							                                pNewTargetParameters);

						                                const auto ret = swapChainResizeTargetHook.call_orig(
							                                chain, pNewTargetParameters);

						                                INVOKE_D3D10_CALLBACK(
							                                engine, EvtHydraHookD3D10PostResizeTarget, chain,
							                                pNewTargetParameters);

						                                return ret;
					                                }
				                                }
#endif

				                                return swapChainResizeTargetHook.call_orig(chain, pNewTargetParameters);
			                                });
		}

		if (dxgiResizeBuffersAddress != 0 && !swapChainResizeBuffersHook.is_applied())
		{
			logger->info("Hooking IDXGISwapChain::ResizeBuffers");

			swapChainResizeBuffersHook.apply(dxgiResizeBuffersAddress, [](
			                                 IDXGISwapChain* chain,
			                                 UINT BufferCount,
			                                 UINT Width,
			                                 UINT Height,
			                                 DXGI_FORMAT NewFormat,
			                                 UINT SwapChainFlags
		                                 ) -> HRESULT
			                                 {
				                                 HookActivityTracker::Guard guard;

				                                 if (!guard.invoke)
					                                 return swapChainResizeBuffersHook.call_orig(
						                                 chain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

#ifndef HYDRAHOOK_NO_D3D12
				                                 if (engine->EngineConfig.Direct3D.HookDirect3D12)
				                                 {
					                                 ID3D12Device* pD12Device = nullptr;
					                                 if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD12Device))) &&
						                                 pD12Device)
					                                 {
						                                 pD12Device->Release();

						                                 g_announce.resizeBuffers12.call([]()
						                                 {
							                                 spdlog::get("HYDRAHOOK")->clone("d3d12")->info(
								                                 "++ IDXGISwapChain::ResizeBuffers called");
						                                 });

						                                 HYDRAHOOK_EVT_PRE_EXTENSION pre;
						                                 HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
							                                 &pre, engine, engine->CustomContext);

						                                 INVOKE_D3D12_CALLBACK(
							                                 engine, EvtHydraHookD3D12PreResizeBuffers, chain,
							                                 BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                 &pre);

						                                 const auto ret = swapChainResizeBuffersHook.call_orig(
							                                 chain, BufferCount, Width, Height, NewFormat,
							                                 SwapChainFlags);

						                                 HYDRAHOOK_EVT_POST_EXTENSION post;
						                                 HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                                 &post, engine, engine->CustomContext);

						                                 INVOKE_D3D12_CALLBACK(
							                                 engine, EvtHydraHookD3D12PostResizeBuffers, chain,
							                                 BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                 &post);

						                                 return ret;
					                                 }
				                                 }
#endif

#ifndef HYDRAHOOK_NO_D3D11
				                                 if (engine->EngineConfig.Direct3D.HookDirect3D11)
				                                 {
					                                 ID3D11Device* pD11Device = nullptr;
					                                 if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD11Device))) &&
						                                 pD11Device)
					                                 {
						                                 pD11Device->Release();

						                                 g_announce.resizeBuffers11.call([]()
						                                 {
							                                 spdlog::get("HYDRAHOOK")->clone("d3d11")->info(
								                                 "++ IDXGISwapChain::ResizeBuffers called");
						                                 });

						                                 HYDRAHOOK_EVT_PRE_EXTENSION pre;
						                                 HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
							                                 &pre, engine, engine->CustomContext);

						                                 INVOKE_D3D11_CALLBACK(
							                                 engine, EvtHydraHookD3D11PreResizeBuffers, chain,
							                                 BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                 &pre);

						                                 const auto ret = swapChainResizeBuffersHook.call_orig(
							                                 chain, BufferCount, Width, Height, NewFormat,
							                                 SwapChainFlags);

						                                 HYDRAHOOK_EVT_POST_EXTENSION post;
						                                 HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                                 &post, engine, engine->CustomContext);

						                                 INVOKE_D3D11_CALLBACK(
							                                 engine, EvtHydraHookD3D11PostResizeBuffers, chain,
							                                 BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                 &post);

						                                 return ret;
					                                 }
				                                 }
#endif

#ifndef HYDRAHOOK_NO_D3D10
				                                 if (engine->EngineConfig.Direct3D.HookDirect3D10)
				                                 {
					                                 ID3D10Device* pD10Device = nullptr;
					                                 if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD10Device))) &&
						                                 pD10Device)
					                                 {
						                                 pD10Device->Release();

						                                 g_announce.resizeBuffers10.call([]()
						                                 {
							                                 spdlog::get("HYDRAHOOK")->clone("d3d10")->info(
								                                 "++ IDXGISwapChain::ResizeBuffers called");
						                                 });

						                                 INVOKE_D3D10_CALLBACK(
							                                 engine, EvtHydraHookD3D10PreResizeBuffers, chain,
							                                 BufferCount, Width, Height, NewFormat, SwapChainFlags);

						                                 const auto ret = swapChainResizeBuffersHook.call_orig(
							                                 chain, BufferCount, Width, Height, NewFormat,
							                                 SwapChainFlags);

						                                 INVOKE_D3D10_CALLBACK(
							                                 engine, EvtHydraHookD3D10PostResizeBuffers, chain,
							                                 BufferCount, Width, Height, NewFormat, SwapChainFlags);

						                                 return ret;
					                                 }
				                                 }
#endif

				                                 return swapChainResizeBuffersHook.call_orig(
					                                 chain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
			                                 });
		}

		if (dxgiPresent1Address != 0 && !swapChainPresent1Hook.is_applied())
		{
			logger->info("Hooking IDXGISwapChain1::Present1");

			swapChainPresent1Hook.apply(dxgiPresent1Address, [](
			                            IDXGISwapChain1* chain,
			                            UINT SyncInterval,
			                            UINT PresentFlags,
			                            const DXGI_PRESENT_PARAMETERS* pPresentParameters
		                            ) -> HRESULT
			                            {
				                            HookActivityTracker::Guard guard;

				                            if (!guard.invoke)
					                            return swapChainPresent1Hook.call_orig(
						                            chain, SyncInterval, PresentFlags, pPresentParameters);

#ifndef HYDRAHOOK_NO_D3D12
				                            if (engine->EngineConfig.Direct3D.HookDirect3D12)
				                            {
					                            ID3D12Device* pD12Device = nullptr;
					                            if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD12Device))) &&
						                            pD12Device)
					                            {
						                            pD12Device->Release();

						                            g_announce.present1D3D12.call([&pChain = chain]()
						                            {
							                            spdlog::get("HYDRAHOOK")->clone("d3d12")->info(
								                            "++ IDXGISwapChain1::Present1 called (D3D12)");

							                            engine->RenderPipeline.pSwapChain = pChain;

							                            INVOKE_HYDRAHOOK_GAME_HOOKED(
								                            engine, HydraHookDirect3DVersion12);
						                            });

						                            HYDRAHOOK_EVT_PRE_EXTENSION pre;
						                            HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
							                            &pre, engine, engine->CustomContext);

						                            INVOKE_D3D12_CALLBACK(
							                            engine, EvtHydraHookD3D12PrePresent, chain, SyncInterval,
							                            PresentFlags, &pre);

						                            const auto ret = swapChainPresent1Hook.call_orig(
							                            chain, SyncInterval, PresentFlags, pPresentParameters);

						                            HYDRAHOOK_EVT_POST_EXTENSION post;
						                            HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                            &post, engine, engine->CustomContext);

						                            INVOKE_D3D12_CALLBACK(
							                            engine, EvtHydraHookD3D12PostPresent, chain, SyncInterval,
							                            PresentFlags, &post);

						                            return ret;
					                            }
				                            }
#endif

#ifndef HYDRAHOOK_NO_D3D11
				                            if (engine->EngineConfig.Direct3D.HookDirect3D11)
				                            {
					                            ID3D11Device* pD11Device = nullptr;
					                            if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD11Device))) &&
						                            pD11Device)
					                            {
						                            pD11Device->Release();

						                            g_announce.present1D3D11.call([&pChain = chain]()
						                            {
							                            spdlog::get("HYDRAHOOK")->clone("d3d11")->info(
								                            "++ IDXGISwapChain1::Present1 called (D3D11)");

							                            engine->RenderPipeline.pSwapChain = pChain;

							                            INVOKE_HYDRAHOOK_GAME_HOOKED(
								                            engine, HydraHookDirect3DVersion11);
						                            });

						                            HYDRAHOOK_EVT_PRE_EXTENSION pre;
						                            HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
							                            &pre, engine, engine->CustomContext);

						                            INVOKE_D3D11_CALLBACK(
							                            engine, EvtHydraHookD3D11PrePresent, chain, SyncInterval,
							                            PresentFlags, &pre);

						                            const auto ret = swapChainPresent1Hook.call_orig(
							                            chain, SyncInterval, PresentFlags, pPresentParameters);

						                            HYDRAHOOK_EVT_POST_EXTENSION post;
						                            HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                            &post, engine, engine->CustomContext);

						                            INVOKE_D3D11_CALLBACK(
							                            engine, EvtHydraHookD3D11PostPresent, chain, SyncInterval,
							                            PresentFlags, &post);

						                            return ret;
					                            }
				                            }
#endif

#ifndef HYDRAHOOK_NO_D3D10
				                            if (engine->EngineConfig.Direct3D.HookDirect3D10)
				                            {
					                            ID3D10Device* pD10Device = nullptr;
					                            if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD10Device))) &&
						                            pD10Device)
					                            {
						                            pD10Device->Release();

						                            g_announce.present1D3D10.call([&pChain = chain]()
						                            {
							                            spdlog::get("HYDRAHOOK")->clone("d3d10")->info(
								                            "++ IDXGISwapChain1::Present1 called (D3D10)");

							                            engine->RenderPipeline.pSwapChain = pChain;

							                            INVOKE_HYDRAHOOK_GAME_HOOKED(
								                            engine, HydraHookDirect3DVersion10);
						                            });

						                            INVOKE_D3D10_CALLBACK(
							                            engine, EvtHydraHookD3D10PrePresent, chain, SyncInterval,
							                            PresentFlags);

						                            const auto ret = swapChainPresent1Hook.call_orig(
							                            chain, SyncInterval, PresentFlags, pPresentParameters);

						                            INVOKE_D3D10_CALLBACK(
							                            engine, EvtHydraHookD3D10PostPresent, chain, SyncInterval,
							                            PresentFlags);

						                            return ret;
					                            }
				                            }
#endif

				                            return swapChainPresent1Hook.call_orig(
					                            chain, SyncInterval, PresentFlags, pPresentParameters);
			                            });
		}

		if (dxgiResizeBuffers1Address != 0 && !swapChainResizeBuffers1Hook.is_applied())
		{
			logger->info("Hooking IDXGISwapChain3::ResizeBuffers1");

			swapChainResizeBuffers1Hook.apply(dxgiResizeBuffers1Address, [](
			                                  IDXGISwapChain3* chain,
			                                  UINT BufferCount,
			                                  UINT Width,
			                                  UINT Height,
			                                  DXGI_FORMAT NewFormat,
			                                  UINT SwapChainFlags,
			                                  const UINT* pCreationNodeMask,
			                                  IUnknown* const* ppPresentQueue
		                                  ) -> HRESULT
			                                  {
				                                  HookActivityTracker::Guard guard;

				                                  if (!guard.invoke)
					                                  return swapChainResizeBuffers1Hook.call_orig(
						                                  chain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
						                                  pCreationNodeMask, ppPresentQueue);

#ifndef HYDRAHOOK_NO_D3D12
				                                  if (engine->EngineConfig.Direct3D.HookDirect3D12)
				                                  {
					                                  ID3D12Device* pD12Device = nullptr;
					                                  if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD12Device))) &&
						                                  pD12Device)
					                                  {
						                                  pD12Device->Release();

						                                  g_announce.resizeBuffers1D3D12.call([]()
						                                  {
							                                  spdlog::get("HYDRAHOOK")->clone("d3d12")->info(
								                                  "++ IDXGISwapChain3::ResizeBuffers1 called (D3D12)");
						                                  });

						                                  HYDRAHOOK_EVT_PRE_EXTENSION pre;
						                                  HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
							                                  &pre, engine, engine->CustomContext);

						                                  INVOKE_D3D12_CALLBACK(
							                                  engine, EvtHydraHookD3D12PreResizeBuffers, chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                  &pre);

						                                  const auto ret = swapChainResizeBuffers1Hook.call_orig(
							                                  chain, BufferCount, Width, Height, NewFormat,
							                                  SwapChainFlags, pCreationNodeMask, ppPresentQueue);

						                                  HYDRAHOOK_EVT_POST_EXTENSION post;
						                                  HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                                  &post, engine, engine->CustomContext);

						                                  INVOKE_D3D12_CALLBACK(
							                                  engine, EvtHydraHookD3D12PostResizeBuffers, chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                  &post);

						                                  return ret;
					                                  }
				                                  }
#endif

#ifndef HYDRAHOOK_NO_D3D11
				                                  if (engine->EngineConfig.Direct3D.HookDirect3D11)
				                                  {
					                                  ID3D11Device* pD11Device = nullptr;
					                                  if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD11Device))) &&
						                                  pD11Device)
					                                  {
						                                  pD11Device->Release();

						                                  g_announce.resizeBuffers1D3D11.call([]()
						                                  {
							                                  spdlog::get("HYDRAHOOK")->clone("d3d11")->info(
								                                  "++ IDXGISwapChain3::ResizeBuffers1 called (D3D11)");
						                                  });

						                                  HYDRAHOOK_EVT_PRE_EXTENSION pre;
						                                  HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
							                                  &pre, engine, engine->CustomContext);

						                                  INVOKE_D3D11_CALLBACK(
							                                  engine, EvtHydraHookD3D11PreResizeBuffers, chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                  &pre);

						                                  const auto ret = swapChainResizeBuffers1Hook.call_orig(
							                                  chain, BufferCount, Width, Height, NewFormat,
							                                  SwapChainFlags, pCreationNodeMask, ppPresentQueue);

						                                  HYDRAHOOK_EVT_POST_EXTENSION post;
						                                  HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                                  &post, engine, engine->CustomContext);

						                                  INVOKE_D3D11_CALLBACK(
							                                  engine, EvtHydraHookD3D11PostResizeBuffers, chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                  &post);

						                                  return ret;
					                                  }
				                                  }
#endif

#ifndef HYDRAHOOK_NO_D3D10
				                                  if (engine->EngineConfig.Direct3D.HookDirect3D10)
				                                  {
					                                  ID3D10Device* pD10Device = nullptr;
					                                  if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD10Device))) &&
						                                  pD10Device)
					                                  {
						                                  pD10Device->Release();

						                                  g_announce.resizeBuffers1D3D10.call([]()
						                                  {
							                                  spdlog::get("HYDRAHOOK")->clone("d3d10")->info(
								                                  "++ IDXGISwapChain3::ResizeBuffers1 called (D3D10)");
						                                  });

						                                  INVOKE_D3D10_CALLBACK(
							                                  engine, EvtHydraHookD3D10PreResizeBuffers, chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags);

						                                  const auto ret = swapChainResizeBuffers1Hook.call_orig(
							                                  chain, BufferCount, Width, Height, NewFormat,
							                                  SwapChainFlags, pCreationNodeMask, ppPresentQueue);

						                                  INVOKE_D3D10_CALLBACK(
							                                  engine, EvtHydraHookD3D10PostResizeBuffers, chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags);

						                                  return ret;
					                                  }
				                                  }
#endif

				                                  return swapChainResizeBuffers1Hook.call_orig(
					                                  chain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
					                                  pCreationNodeMask, ppPresentQueue);
			                                  });
		}
	}
	catch (DetourException& ex)
	{
		logger->error("Hooking DXGI failed: {}", ex.what());
	}

#pragma endregion

#pragma region Core Audio

#ifndef HYDRAHOOK_NO_COREAUDIO

	if (config.CoreAudio.HookCoreAudio)
	{
		try
		{
			const std::unique_ptr<CoreAudioHooking::AudioRenderClientHook> arc(
				new CoreAudioHooking::AudioRenderClientHook);

			logger->info("Hooking IAudioRenderClient::GetBuffer");

			arcGetBufferHook.apply(arc->vtable()[CoreAudioHooking::GetBuffer], [](
			                       IAudioRenderClient* client,
			                       UINT32 NumFramesRequested,
			                       BYTE** ppData
		                       ) -> HRESULT
			                       {
				                       HookActivityTracker::Guard guard;

				                       if (guard.invoke)
				                       {
					                       g_announce.arcGetBuffer.call([&pClient = client]()
					                       {
						                       spdlog::get("HYDRAHOOK")->clone("arc")->info(
							                       "++ IAudioRenderClient::GetBuffer called");

						                       engine->CoreAudio.pARC = pClient;
					                       });

					                       HYDRAHOOK_EVT_PRE_EXTENSION pre;
					                       HYDRAHOOK_EVT_PRE_EXTENSION_INIT(&pre, engine, engine->CustomContext);

					                       INVOKE_ARC_CALLBACK(engine, EvtHydraHookARCPreGetBuffer, client,
					                                           NumFramesRequested, ppData, &pre);
				                       }

				                       const auto ret = arcGetBufferHook.call_orig(client, NumFramesRequested, ppData);

				                       if (guard.invoke)
				                       {
					                       HYDRAHOOK_EVT_POST_EXTENSION post;
					                       HYDRAHOOK_EVT_POST_EXTENSION_INIT(&post, engine, engine->CustomContext);

					                       INVOKE_ARC_CALLBACK(engine, EvtHydraHookARCPostGetBuffer, client,
					                                           NumFramesRequested, ppData, &post);
				                       }

				                       return ret;
			                       });

			logger->info("Hooking IAudioRenderClient::ReleaseBuffer");

			arcReleaseBufferHook.apply(arc->vtable()[CoreAudioHooking::ReleaseBuffer], [](
			                           IAudioRenderClient* client,
			                           UINT32 NumFramesWritten,
			                           DWORD dwFlags
		                           ) -> HRESULT
			                           {
				                           HookActivityTracker::Guard guard;

				                           if (guard.invoke)
				                           {
					                           g_announce.arcReleaseBuffer.call([]()
					                           {
						                           spdlog::get("HYDRAHOOK")->clone("arc")->info(
							                           "++ IAudioRenderClient::ReleaseBuffer called");
					                           });

					                           HYDRAHOOK_EVT_PRE_EXTENSION pre;
					                           HYDRAHOOK_EVT_PRE_EXTENSION_INIT(&pre, engine, engine->CustomContext);

					                           INVOKE_ARC_CALLBACK(engine, EvtHydraHookARCPreReleaseBuffer, client,
					                                               NumFramesWritten, dwFlags, &pre);
				                           }

				                           const auto ret = arcReleaseBufferHook.call_orig(
					                           client, NumFramesWritten, dwFlags);

				                           if (guard.invoke)
				                           {
					                           HYDRAHOOK_EVT_POST_EXTENSION post;
					                           HYDRAHOOK_EVT_POST_EXTENSION_INIT(&post, engine, engine->CustomContext);

					                           INVOKE_ARC_CALLBACK(engine, EvtHydraHookARCPostReleaseBuffer, client,
					                                               NumFramesWritten, dwFlags, &post);
				                           }

				                           return ret;
			                           });
		}
		catch (DetourException& ex)
		{
			logger->error("Hooking Core Audio (ARC) failed: {}", ex.what());
		}
		catch (ARCException& ex)
		{
			logger->error("Initializing Core Audio (ARC) failed: {} (HRESULT {})",
			              ex.what(), ex.hresult());
		}
		catch (RuntimeException& ex)
		{
			logger->error("Core Audio (ARC) runtime error: {}", ex.what());
		}
	}

#endif

#pragma endregion

#ifdef HOOK_DINPUT8
	//
	// TODO: legacy, fix me up!
	// 
	if (engine->Configuration->getBool("DInput8.enabled", false))
	{
		bool dinput8_available;
		size_t vtable8[DirectInput8Hooking::DirectInput8::VTableElements] = {0};

		// Dinput8
		{
			DirectInput8Hooking::DirectInput8 di8;
			dinput8_available = di8.GetVTable(vtable8);

			if (!dinput8_available)
			{
				logger.warning("Could not get VTable for DirectInput8");
			}
		}

		if (dinput8_available)
		{
			BOOST_LOG_TRIVIAL(info) << "Game uses DirectInput8"
			)
			;
			HookDInput8(vtable8);
		}
	}
#endif

	logger->info("Library initialized successfully");

	//
	// Wait until cancellation requested
	// 
	const auto result = WaitForSingleObject(engine->EngineCancellationEvent, INFINITE);
	logger->info("Shutting down hooks... (result: {}, error: {})", result, GetLastError());
	switch (result)
	{
	case WAIT_ABANDONED:
		logger->info("Shutting down hooks... Unknown state, host process might crash");
		break;
	case WAIT_OBJECT_0:
		logger->info("Shutting down hooks... Thread shutdown complete");
		break;
	case WAIT_TIMEOUT:
		logger->info("Shutting down hooks... Thread hasn't finished clean-up within expected time, terminating");
		break;
	case WAIT_FAILED:
		logger->info("Shutting down hooks... Unknown error, host process might crash");
		break;
	default:
		logger->info("Shutting down hooks... Unexpected return value, terminating");
		break;
	}
	//
	// Signal shutdown: new hook invocations will skip callbacks
	//
	HookActivityTracker::shutdown();
	logger->info("Shutdown flag set, new hook invocations will skip callbacks");

	//
	// Notify host that we are about to release all render pipeline hooks
	// 
	if (engine->EngineConfig.EvtHydraHookGamePreUnhook)
	{
		engine->EngineConfig.EvtHydraHookGamePreUnhook(engine);
	}

	//
	// Remove all hooks -- after this no new threads can enter any lambda
	//
	bool allRemoved = true;
	const auto rm = [&allRemoved](auto& hook)
	{
		if (!hook.remove_nothrow())
			allRemoved = false;
	};
	{
		// Detach in reverse (LIFO) installation order as defense-in-depth so any
		// stacked Detours trampolines unwind cleanly.
#ifndef HYDRAHOOK_NO_COREAUDIO
		rm(arcReleaseBufferHook);
		rm(arcGetBufferHook);
#endif

		rm(swapChainResizeBuffers1Hook);
		rm(swapChainPresent1Hook);
		rm(swapChainResizeBuffersHook);
		rm(swapChainResizeTargetHook);
		rm(swapChainPresentHook);

#ifndef HYDRAHOOK_NO_D3D12
		rm(executeCommandLists12Hook);
		rm(createSwapChainForHwnd12Hook);
		rm(createSwapChain12Hook);
		D3D12_ReleaseQueueMaps();
#endif

#ifndef HYDRAHOOK_NO_D3D9
		rm(reset9ExHook);
		rm(present9ExHook);
		rm(endScene9Hook);
		rm(reset9Hook);
		rm(present9Hook);
#endif
	}

	//
	// A manual unhook keeps the DLL loaded, so the flow-control hooks must go too
	//
	if (engine->SkipDllUnloadOnExit.load(std::memory_order_acquire)
		&& !engine->FreeLibraryHookActive.load(std::memory_order_acquire))
	{
		rm(g_exitProcessHook);
		rm(g_postQuitMessageHook);
		rm(g_freeLibraryHook);
	}

	if (allRemoved)
		logger->info("Hooks disabled");
	else
		logger->error("Unhooking failed for at least one hook");

	//
	// Wait for all in-flight hook lambdas to finish before touching
	// callback tables or unloading the DLL
	//
	if (!HookActivityTracker::drain())
	{
		logger->error("Timed out waiting for in-flight callbacks to drain");
		engine->DrainIncomplete.store(true, std::memory_order_release);
	}
	else
	{
		logger->info("All in-flight hook callbacks drained");
		engine->DrainIncomplete.store(false, std::memory_order_release);

		ZeroMemory(&engine->EventsD3D9, sizeof(engine->EventsD3D9));
		ZeroMemory(&engine->EventsD3D10, sizeof(engine->EventsD3D10));
		ZeroMemory(&engine->EventsD3D11, sizeof(engine->EventsD3D11));
		ZeroMemory(&engine->EventsD3D12, sizeof(engine->EventsD3D12));
		ZeroMemory(&engine->EventsARC, sizeof(engine->EventsARC));
	}

	//
	// Notify host that we released all render pipeline hooks
	// 
	if (engine->EngineConfig.EvtHydraHookGamePostUnhook)
	{
		engine->EngineConfig.EvtHydraHookGamePostUnhook(engine);
	}

	logger->info("Exiting worker thread");

	if (engine->FreeLibraryHookActive.load(std::memory_order_acquire)
		|| engine->SkipDllUnloadOnExit.load(std::memory_order_acquire))
	{
		ExitThread(0);
	}
	else
	{
		FreeLibraryAndExitThread(engine->HostInstance, 0);
	}
}

ID3D12CommandQueue* GetD3D12CommandQueueForSwapChain(IDXGISwapChain* pSwapChain)
{
#ifndef HYDRAHOOK_NO_D3D12
	if (!pSwapChain)
		return nullptr;

	std::lock_guard<std::mutex> lock(g_d3d12QueueMapMutex);
	// 1. Early injection: captured from CreateSwapChain
	auto it = g_d3d12SwapChainToQueue.find(pSwapChain);
	if (it != g_d3d12SwapChainToQueue.end() && it->second)
	{
		it->second->AddRef();
		return it->second;
	}
	// 2. Mid-process injection: captured from ExecuteCommandLists at runtime
	ID3D12Device* pDevice = nullptr;
	if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&pDevice))) && pDevice)
	{
		auto devIt = g_d3d12DeviceToQueue.find(pDevice);
		pDevice->Release();
		if (devIt != g_d3d12DeviceToQueue.end() && devIt->second)
		{
			devIt->second->AddRef();
			return devIt->second;
		}
	}
#endif
	return nullptr;
}

#ifdef HOOK_DINPUT8
/**
 * @brief Installs hooks for key IDirectInputDevice8 methods on the provided vtable.
 *
 * Attaches wrappers for Acquire, GetDeviceData, GetDeviceInfo, GetDeviceState, and GetObjectInfo that perform a one-time informational log when each method is first invoked and then invoke the original implementation.
 *
 * @param vtable8 Pointer to the IDirectInputDevice8 virtual function table (array of function pointers) where hooks will be installed.
 */
void HookDInput8(size_t* vtable8)
{
	auto& logger = Logger::get(__func__);
	BOOST_LOG_TRIVIAL(info) << "Hooking IDirectInputDevice8::Acquire"
	)
	;

	g_acquire8Hook.apply(vtable8[DirectInput8Hooking::Acquire], [](LPDIRECTINPUTDEVICE8 dev) -> HRESULT
	{
		static std::once_flag flag;
		std::call_once(flag, []()
		{
			Logger::get("HookDInput8").information("++ IDirectInputDevice8::Acquire called");
		});

		return g_acquire8Hook.callOrig(dev);
	});

	BOOST_LOG_TRIVIAL(info) << "Hooking IDirectInputDevice8::GetDeviceData"
	)
	;

	g_getDeviceData8Hook.apply(vtable8[DirectInput8Hooking::GetDeviceData],
	                           [](LPDIRECTINPUTDEVICE8 dev, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod,
	                              LPDWORD pdwInOut, DWORD dwFlags) -> HRESULT
	                           {
		                           static std::once_flag flag;
		                           std::call_once(flag, []()
		                           {
			                           Logger::get("HookDInput8").information(
				                           "++ IDirectInputDevice8::Acquire called");
		                           });

		                           return g_getDeviceData8Hook.callOrig(dev, cbObjectData, rgdod, pdwInOut, dwFlags);
	                           });

	BOOST_LOG_TRIVIAL(info) << "Hooking IDirectInputDevice8::GetDeviceInfo"
	)
	;

	g_getDeviceInfo8Hook.apply(vtable8[DirectInput8Hooking::GetDeviceInfo],
	                           [](LPDIRECTINPUTDEVICE8 dev, LPDIDEVICEINSTANCE pdidi) -> HRESULT
	                           {
		                           static std::once_flag flag;
		                           std::call_once(flag, []()
		                           {
			                           Logger::get("HookDInput8").information(
				                           "++ IDirectInputDevice8::GetDeviceInfo called");
		                           });

		                           return g_getDeviceInfo8Hook.callOrig(dev, pdidi);
	                           });

	BOOST_LOG_TRIVIAL(info) << "Hooking IDirectInputDevice8::GetDeviceState"
	)
	;

	g_getDeviceState8Hook.apply(vtable8[DirectInput8Hooking::GetDeviceState],
	                            [](LPDIRECTINPUTDEVICE8 dev, DWORD cbData, LPVOID lpvData) -> HRESULT
	                            {
		                            static std::once_flag flag;
		                            std::call_once(flag, []()
		                            {
			                            Logger::get("HookDInput8").information(
				                            "++ IDirectInputDevice8::GetDeviceState called");
		                            });

		                            return g_getDeviceState8Hook.callOrig(dev, cbData, lpvData);
	                            });

	BOOST_LOG_TRIVIAL(info) << "Hooking IDirectInputDevice8::GetObjectInfo"
	)
	;

	g_getObjectInfo8Hook.apply(vtable8[DirectInput8Hooking::GetObjectInfo],
	                           [](LPDIRECTINPUTDEVICE8 dev, LPDIDEVICEOBJECTINSTANCE pdidoi, DWORD dwObj,
	                              DWORD dwHow) -> HRESULT
	                           {
		                           static std::once_flag flag;
		                           std::call_once(flag, []()
		                           {
			                           Logger::get("HookDInput8").information(
				                           "++ IDirectInputDevice8::GetObjectInfo called");
		                           });

		                           return g_getObjectInfo8Hook.callOrig(dev, pdidoi, dwObj, dwHow);
	                           });
}
#endif
