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

void PerformShutdownCleanup(PHYDRAHOOK_ENGINE engine, ShutdownOrigin origin)
{
	const char* logChannel = "shutdown";
	const char* logMessage = "Performing pre-DLL-detach clean-up tasks";
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
	case ShutdownOrigin::DllMainProcessDetach:
		logChannel = "detach";
		break;
	}

	auto logger = spdlog::get("HYDRAHOOK")->clone(logChannel);
	logger->info(logMessage);

	if (origin == ShutdownOrigin::ExitProcessHook)
		g_postQuitMessageHook.remove();
	else if (origin == ShutdownOrigin::PostQuitMessageHook)
		g_exitProcessHook.remove();
	else if (origin == ShutdownOrigin::DllMainProcessDetach)
	{
		g_postQuitMessageHook.remove_nothrow();
		g_exitProcessHook.remove_nothrow();
	}

	if (origin != ShutdownOrigin::DllMainProcessDetach && engine->EngineConfig.EvtHydraHookGamePreExit)
	{
		engine->EngineConfig.EvtHydraHookGamePreExit(engine);
	}

	const auto ret = SetEvent(engine->EngineCancellationEvent);
	if (!ret)
	{
		logger->error("SetEvent failed: {}", GetLastError());
	}

	// wait on thread shutdown or check state
	const DWORD waitTimeout = (origin == ShutdownOrigin::DllMainProcessDetach) ? 0 : 3000;
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
//#ifndef _DEBUG
		if (origin != ShutdownOrigin::DllMainProcessDetach)
		{
			TerminateThread(engine->EngineThread, 0);
			logger->error("Thread hasn't finished clean-up within expected time, terminating");
		}
//#endif
		break;
	case WAIT_FAILED:
		logger->error("Unknown error, host process might crash");
		break;
	default:
		if (origin != ShutdownOrigin::DllMainProcessDetach)
		{
			TerminateThread(engine->EngineThread, 0);
		}
		logger->error("Unexpected return value, terminating");
		break;
	}

	// decrease ref-count we incremented on engine initialization
	if (engine->HostModule)
		FreeLibrary(engine->HostModule);
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

/**
 * @brief Entry point for the HydraHook engine worker thread that initializes, installs,
 *        and manages all runtime hooks for supported subsystems (D3D9/10/11/12, Core Audio,
 *        DirectInput8) and handles graceful shutdown.
 *
 * The thread sets up per-API hooks, captures runtime state required by the render/audio
 * pipelines, wires pre/post extension callbacks, waits for a cancellation event, then
 * uninstalls hooks and exits the host DLL thread.
 *
 * @param Params Pointer to a PHYDRAHOOK_ENGINE instance (passed as LPVOID). The function
 *               interprets this parameter as the engine context used for configuration,
 *               event callbacks, and shared state.
 * @return DWORD Thread exit code. Note: the function ends the thread via FreeLibraryAndExitThread,
 *               so it does not return to its caller in the usual way.
 */
DWORD WINAPI HydraHookMainThread(LPVOID Params)
{
	auto logger = spdlog::get("HYDRAHOOK")->clone("game");
	static auto engine = reinterpret_cast<PHYDRAHOOK_ENGINE>(Params);
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
	// D3D10 Hooks
	// 
#ifndef HYDRAHOOK_NO_D3D10
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain*, UINT, UINT> swapChainPresent10Hook;
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain*, const DXGI_MODE_DESC*> swapChainResizeTarget10Hook;
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT>
		swapChainResizeBuffers10Hook;
#else
	logger->info("Direct3D 10 hooking disabled at compile time");
#endif

	// 
	// D3D11 Hooks
	// 
#ifndef HYDRAHOOK_NO_D3D11
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain*, UINT, UINT> swapChainPresent11Hook;
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain*, const DXGI_MODE_DESC*> swapChainResizeTarget11Hook;
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT>
		swapChainResizeBuffers11Hook;
#else
	logger->info("Direct3D 11 hooking disabled at compile time");
#endif

	//
	// DXGI1+ Hooks (shared across D3D10/11/12)
	//
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*>
		swapChainPresent1Hook;
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
	            IUnknown* const*> swapChainResizeBuffers1Hook;

	// 
	// D3D12 Hooks
	// 
#ifndef HYDRAHOOK_NO_D3D12
	static Hook<CallConvention::stdcall_t, HRESULT, IUnknown*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**>
		createSwapChain12Hook;
	static Hook<CallConvention::stdcall_t, HRESULT, IUnknown*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const
	            DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**> createSwapChainForHwnd12Hook;
	static Hook<CallConvention::stdcall_t, void, ID3D12CommandQueue*, UINT, ID3D12CommandList* const*>
		executeCommandLists12Hook;
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain*, UINT, UINT> swapChainPresent12Hook;
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain*, const DXGI_MODE_DESC*> swapChainResizeTarget12Hook;
	static Hook<CallConvention::stdcall_t, HRESULT, IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT>
		swapChainResizeBuffers12Hook;
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
	g_exitProcessHook.apply((size_t)ExitProcess, [](UINT uExitCode)
		{
			PerformShutdownCleanup(engine, ShutdownOrigin::ExitProcessHook);
			// Call native API. After this it becomes unsafe to use any remaining library resources!
			g_exitProcessHook.call_orig(uExitCode);
		});

	/*
	 * Hooking PostQuitMessage in addition to ExitProcess should be practically
	 * more reliable since a game is expected to have at least one main window
	 * which _should_ receive the WM_QUIT message upon application shutdown.
	 */
	g_postQuitMessageHook.apply((size_t)PostQuitMessage, [](int nExitCode)
		{
			PerformShutdownCleanup(engine, ShutdownOrigin::PostQuitMessageHook);
			g_postQuitMessageHook.call_orig(nExitCode);
		});


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
				                   static std::once_flag flag;
				                   std::call_once(flag, [&pDev = dev]()
				                   {
					                   spdlog::get("HYDRAHOOK")->clone("d3d9")->info(
						                   "++ IDirect3DDevice9Ex::Present called");

					                   engine->RenderPipeline.pD3D9Device = pDev;

					                   INVOKE_HYDRAHOOK_GAME_HOOKED(engine, HydraHookDirect3DVersion9);
				                   });

				                   INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PrePresent, dev, a1, a2, a3, a4);

				                   const auto ret = present9Hook.call_orig(dev, a1, a2, a3, a4);

				                   INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PostPresent, dev, a1, a2, a3, a4);

				                   return ret;
			                   });

			logger->info("Hooking IDirect3DDevice9Ex::Reset");

			reset9Hook.apply(d3dEx->vtable()[Direct3D9Hooking::Reset], [](
			                 LPDIRECT3DDEVICE9 dev,
			                 D3DPRESENT_PARAMETERS* pp
		                 ) -> HRESULT
			                 {
				                 static std::once_flag flag;
				                 std::call_once(flag, []()
				                 {
					                 spdlog::get("HYDRAHOOK")->clone("d3d9")->info(
						                 "++ IDirect3DDevice9Ex::Reset called");
				                 });

				                 INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PreReset, dev, pp);

				                 const auto ret = reset9Hook.call_orig(dev, pp);

				                 INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PostReset, dev, pp);

				                 return ret;
			                 });

			logger->info("Hooking IDirect3DDevice9Ex::EndScene");

			endScene9Hook.apply(d3dEx->vtable()[Direct3D9Hooking::EndScene], [](
			                    LPDIRECT3DDEVICE9 dev
		                    ) -> HRESULT
			                    {
				                    static std::once_flag flag;
				                    std::call_once(flag, []()
				                    {
					                    spdlog::get("HYDRAHOOK")->clone("d3d9")->info(
						                    "++ IDirect3DDevice9Ex::EndScene called");
				                    });

				                    INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PreEndScene, dev);

				                    const auto ret = endScene9Hook.call_orig(dev);

				                    INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PostEndScene, dev);

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
				                     static std::once_flag flag;
				                     std::call_once(flag, [&pDev = dev]()
				                     {
					                     spdlog::get("HYDRAHOOK")->clone("d3d9")->info(
						                     "++ IDirect3DDevice9Ex::PresentEx called");

					                     engine->RenderPipeline.pD3D9ExDevice = pDev;

					                     INVOKE_HYDRAHOOK_GAME_HOOKED(engine, HydraHookDirect3DVersion9);
				                     });

				                     INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PrePresentEx, dev, a1, a2, a3, a4,
				                                          a5);

				                     const auto ret = present9ExHook.call_orig(dev, a1, a2, a3, a4, a5);

				                     INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PostPresentEx, dev, a1, a2, a3, a4,
				                                          a5);

				                     return ret;
			                     });

			logger->info("Hooking IDirect3DDevice9Ex::ResetEx");

			reset9ExHook.apply(d3dEx->vtable()[Direct3D9Hooking::ResetEx], [](
			                   LPDIRECT3DDEVICE9EX dev,
			                   D3DPRESENT_PARAMETERS* pp,
			                   D3DDISPLAYMODEEX* ppp
		                   ) -> HRESULT
			                   {
				                   static std::once_flag flag;
				                   std::call_once(flag, []()
				                   {
					                   spdlog::get("HYDRAHOOK")->clone("d3d9")->info(
						                   "++ IDirect3DDevice9Ex::ResetEx called");
				                   });

				                   INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PreResetEx, dev, pp, ppp);

				                   const auto ret = reset9ExHook.call_orig(dev, pp, ppp);

				                   INVOKE_D3D9_CALLBACK(engine, EvtHydraHookD3D9PostResetEx, dev, pp, ppp);

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

#pragma region D3D10

	static HYDRAHOOK_D3D_VERSION deviceVersion = HydraHookDirect3DVersionUnknown;
	size_t dxgiPresentAddress = 0;
	size_t dxgiPresent1Address = 0;
	size_t dxgiResizeBuffers1Address = 0;

#ifndef HYDRAHOOK_NO_D3D10

	if (config.Direct3D.HookDirect3D10)
	{
		try
		{
			const std::unique_ptr<Direct3D10Hooking::Direct3D10> d3d10(new Direct3D10Hooking::Direct3D10);
			auto vtable = d3d10->vtable();

			logger->info("Hooking IDXGISwapChain::Present");

			swapChainPresent10Hook.apply(vtable[DXGIHooking::Present], [](
			                             IDXGISwapChain* chain,
			                             UINT SyncInterval,
			                             UINT Flags
		                             ) -> HRESULT
			                             {
				                             static std::once_flag flag;
				                             std::call_once(flag, [&pChain = chain]()
				                             {
					                             auto l = spdlog::get("HYDRAHOOK")->clone("d3d10");
					                             l->info("++ IDXGISwapChain::Present called");

					                             ID3D10Device* pp10Device = nullptr;
					                             ID3D11Device* pp11Device = nullptr;

					                             auto ret = pChain->GetDevice(
						                             __uuidof(ID3D10Device), reinterpret_cast<PVOID*>(&pp10Device));

					                             if (SUCCEEDED(ret))
					                             {
						                             l->debug("ID3D10Device object acquired");
						                             deviceVersion = HydraHookDirect3DVersion10;
						                             INVOKE_HYDRAHOOK_GAME_HOOKED(engine, deviceVersion);
						                             return;
					                             }

					                             ret = pChain->GetDevice(
						                             __uuidof(ID3D11Device), reinterpret_cast<PVOID*>(&pp11Device));

					                             if (SUCCEEDED(ret))
					                             {
						                             l->debug("ID3D11Device object acquired");
						                             deviceVersion = HydraHookDirect3DVersion11;
						                             INVOKE_HYDRAHOOK_GAME_HOOKED(engine, deviceVersion);
						                             return;
					                             }

					                             l->error("Could not fetch device pointer");
				                             });

				                             HYDRAHOOK_EVT_PRE_EXTENSION pre;
				                             HYDRAHOOK_EVT_PRE_EXTENSION_INIT(&pre, engine, engine->CustomContext);
				                             HYDRAHOOK_EVT_POST_EXTENSION post;
				                             HYDRAHOOK_EVT_POST_EXTENSION_INIT(&post, engine, engine->CustomContext);

				                             if (deviceVersion == HydraHookDirect3DVersion10)
				                             {
					                             INVOKE_D3D10_CALLBACK(engine, EvtHydraHookD3D10PrePresent, chain,
					                                                   SyncInterval, Flags);
				                             }

				                             if (deviceVersion == HydraHookDirect3DVersion11)
				                             {
					                             INVOKE_D3D11_CALLBACK(engine, EvtHydraHookD3D11PrePresent, chain,
					                                                   SyncInterval, Flags, &pre);
				                             }

				                             const auto ret = swapChainPresent10Hook.call_orig(
					                             chain, SyncInterval, Flags);

				                             if (deviceVersion == HydraHookDirect3DVersion10)
				                             {
					                             INVOKE_D3D10_CALLBACK(engine, EvtHydraHookD3D10PostPresent, chain,
					                                                   SyncInterval, Flags);
				                             }

				                             if (deviceVersion == HydraHookDirect3DVersion11)
				                             {
					                             INVOKE_D3D11_CALLBACK(engine, EvtHydraHookD3D11PostPresent, chain,
					                                                   SyncInterval, Flags, &post);
				                             }

				                             return ret;
			                             });
			dxgiPresentAddress = vtable[DXGIHooking::Present];
			if (vtable.size() > static_cast<size_t>(DXGIHooking::DXGI1::DXGISwapChain1VTbl::Present1))
				dxgiPresent1Address = vtable[DXGIHooking::DXGI1::Present1];
			if (vtable.size() > static_cast<size_t>(DXGIHooking::DXGI3::ResizeBuffers1))
				dxgiResizeBuffers1Address = vtable[DXGIHooking::DXGI3::ResizeBuffers1];

			logger->info("Hooking IDXGISwapChain::ResizeTarget");

			swapChainResizeTarget10Hook.apply(vtable[DXGIHooking::ResizeTarget], [](
			                                  IDXGISwapChain* chain,
			                                  const DXGI_MODE_DESC* pNewTargetParameters
		                                  ) -> HRESULT
			                                  {
				                                  static std::once_flag flag;
				                                  std::call_once(flag, []()
				                                  {
					                                  spdlog::get("HYDRAHOOK")->clone("d3d10")->info(
						                                  "++ IDXGISwapChain::ResizeTarget called");
				                                  });

				                                  HYDRAHOOK_EVT_PRE_EXTENSION pre;
				                                  HYDRAHOOK_EVT_PRE_EXTENSION_INIT(&pre, engine, engine->CustomContext);
				                                  HYDRAHOOK_EVT_POST_EXTENSION post;
				                                  HYDRAHOOK_EVT_POST_EXTENSION_INIT(
					                                  &post, engine, engine->CustomContext);

				                                  if (deviceVersion == HydraHookDirect3DVersion10)
				                                  {
					                                  INVOKE_D3D10_CALLBACK(
						                                  engine, EvtHydraHookD3D10PreResizeTarget, chain,
						                                  pNewTargetParameters);
				                                  }

				                                  if (deviceVersion == HydraHookDirect3DVersion11)
				                                  {
					                                  INVOKE_D3D11_CALLBACK(
						                                  engine,
						                                  EvtHydraHookD3D11PreResizeTarget,
						                                  chain,
						                                  pNewTargetParameters,
						                                  &pre
					                                  );
				                                  }

				                                  const auto ret = swapChainResizeTarget10Hook.call_orig(
					                                  chain, pNewTargetParameters);

				                                  if (deviceVersion == HydraHookDirect3DVersion10)
				                                  {
					                                  INVOKE_D3D10_CALLBACK(
						                                  engine, EvtHydraHookD3D10PostResizeTarget, chain,
						                                  pNewTargetParameters);
				                                  }

				                                  if (deviceVersion == HydraHookDirect3DVersion11)
				                                  {
					                                  INVOKE_D3D11_CALLBACK(
						                                  engine,
						                                  EvtHydraHookD3D11PostResizeTarget,
						                                  chain,
						                                  pNewTargetParameters,
						                                  &post
					                                  );
				                                  }

				                                  return ret;
			                                  });

			logger->info("Hooking IDXGISwapChain::ResizeBuffers");

			swapChainResizeBuffers10Hook.apply(vtable[DXGIHooking::ResizeBuffers], [](
			                                   IDXGISwapChain* chain,
			                                   UINT BufferCount,
			                                   UINT Width,
			                                   UINT Height,
			                                   DXGI_FORMAT NewFormat,
			                                   UINT SwapChainFlags
		                                   ) -> HRESULT
			                                   {
				                                   static std::once_flag flag;
				                                   std::call_once(flag, []()
				                                   {
					                                   spdlog::get("HYDRAHOOK")->clone("d3d10")->info(
						                                   "++ IDXGISwapChain::ResizeBuffers called");
				                                   });

				                                   HYDRAHOOK_EVT_PRE_EXTENSION pre;
				                                   HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
					                                   &pre, engine, engine->CustomContext);
				                                   HYDRAHOOK_EVT_POST_EXTENSION post;
				                                   HYDRAHOOK_EVT_POST_EXTENSION_INIT(
					                                   &post, engine, engine->CustomContext);

				                                   if (deviceVersion == HydraHookDirect3DVersion10)
				                                   {
					                                   INVOKE_D3D10_CALLBACK(
						                                   engine, EvtHydraHookD3D10PreResizeBuffers, chain,
						                                   BufferCount, Width, Height, NewFormat, SwapChainFlags);
				                                   }

				                                   if (deviceVersion == HydraHookDirect3DVersion11)
				                                   {
					                                   INVOKE_D3D11_CALLBACK(
						                                   engine, EvtHydraHookD3D11PreResizeBuffers, chain,
						                                   BufferCount, Width, Height, NewFormat, SwapChainFlags, &pre);
				                                   }

				                                   const auto ret = swapChainResizeBuffers10Hook.call_orig(chain,
					                                   BufferCount, Width, Height, NewFormat, SwapChainFlags);

				                                   if (deviceVersion == HydraHookDirect3DVersion10)
				                                   {
					                                   INVOKE_D3D10_CALLBACK(
						                                   engine, EvtHydraHookD3D10PostResizeBuffers, chain,
						                                   BufferCount, Width, Height, NewFormat, SwapChainFlags);
				                                   }

				                                   if (deviceVersion == HydraHookDirect3DVersion11)
				                                   {
					                                   INVOKE_D3D11_CALLBACK(
						                                   engine, EvtHydraHookD3D11PostResizeBuffers, chain,
						                                   BufferCount, Width, Height, NewFormat, SwapChainFlags, &post);
				                                   }

				                                   return ret;
			                                   });
		}
		catch (DetourException& ex)
		{
			logger->error("Hooking D3D10 failed: {}", ex.what());
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

#pragma endregion

#pragma region D3D11

#ifndef HYDRAHOOK_NO_D3D11

	if (config.Direct3D.HookDirect3D11)
	{
		try
		{
			const std::unique_ptr<Direct3D11Hooking::Direct3D11> d3d11(new Direct3D11Hooking::Direct3D11);
			auto vtable = d3d11->vtable();
			const size_t d3d11PresentAddress = vtable[DXGIHooking::Present];

			if (dxgiPresent1Address == 0 && vtable.size() > static_cast<size_t>(
				DXGIHooking::DXGI1::DXGISwapChain1VTbl::Present1))
				dxgiPresent1Address = vtable[DXGIHooking::DXGI1::Present1];
			if (dxgiResizeBuffers1Address == 0 && vtable.size() > static_cast<size_t>(
				DXGIHooking::DXGI3::ResizeBuffers1))
				dxgiResizeBuffers1Address = vtable[DXGIHooking::DXGI3::ResizeBuffers1];

			// D3D10 and D3D11 share the same DXGI swap chain implementation. Applying both would
			// create a duplicate hook chain; the D3D10 hook already handles both via device detection.
			if (dxgiPresentAddress != 0 && d3d11PresentAddress == dxgiPresentAddress)
			{
				logger->info("Skipping D3D11 DXGI hooks (same vtable as D3D10; D3D10 hook handles both)");
			}
			else
			{
				logger->info("Hooking IDXGISwapChain::Present");

				swapChainPresent11Hook.apply(vtable[DXGIHooking::Present], [](
				                             IDXGISwapChain* chain,
				                             UINT SyncInterval,
				                             UINT Flags
			                             ) -> HRESULT
				                             {
					                             ID3D11Device* pD11Device = nullptr;
					                             if (FAILED(chain->GetDevice(IID_PPV_ARGS(&pD11Device))))
					                             {
						                             if (pD11Device)
						                             {
							                             pD11Device->Release();
						                             }
						                             return swapChainPresent11Hook.
							                             call_orig(chain, SyncInterval, Flags);
					                             }
					                             if (pD11Device)
					                             {
						                             pD11Device->Release();
					                             }

					                             static std::once_flag flag;
					                             std::call_once(flag, [&pChain = chain]()
					                             {
						                             spdlog::get("HYDRAHOOK")->clone("d3d11")->info(
							                             "++ IDXGISwapChain::Present called");

						                             engine->RenderPipeline.pSwapChain = pChain;

						                             INVOKE_HYDRAHOOK_GAME_HOOKED(engine, HydraHookDirect3DVersion11);
					                             });

					                             HYDRAHOOK_EVT_PRE_EXTENSION pre;
					                             HYDRAHOOK_EVT_PRE_EXTENSION_INIT(&pre, engine, engine->CustomContext);
					                             HYDRAHOOK_EVT_POST_EXTENSION post;
					                             HYDRAHOOK_EVT_POST_EXTENSION_INIT(
						                             &post, engine, engine->CustomContext);

					                             INVOKE_D3D11_CALLBACK(
						                             engine,
						                             EvtHydraHookD3D11PrePresent,
						                             chain,
						                             SyncInterval,
						                             Flags,
						                             &pre
					                             );

					                             const auto ret = swapChainPresent11Hook.call_orig(
						                             chain, SyncInterval, Flags);

					                             INVOKE_D3D11_CALLBACK(
						                             engine,
						                             EvtHydraHookD3D11PostPresent,
						                             chain,
						                             SyncInterval,
						                             Flags,
						                             &post
					                             );

					                             return ret;
				                             });

				logger->info("Hooking IDXGISwapChain::ResizeTarget");

				swapChainResizeTarget11Hook.apply(vtable[DXGIHooking::ResizeTarget], [](
				                                  IDXGISwapChain* chain,
				                                  const DXGI_MODE_DESC* pNewTargetParameters
			                                  ) -> HRESULT
				                                  {
					                                  ID3D11Device* pD11Device = nullptr;
					                                  if (FAILED(chain->GetDevice(IID_PPV_ARGS(&pD11Device))))
					                                  {
						                                  if (pD11Device)
						                                  {
							                                  pD11Device->Release();
						                                  }
						                                  return swapChainResizeTarget11Hook.call_orig(
							                                  chain, pNewTargetParameters);
					                                  }
					                                  if (pD11Device)
					                                  {
						                                  pD11Device->Release();
					                                  }

					                                  static std::once_flag flag;
					                                  std::call_once(flag, []()
					                                  {
						                                  spdlog::get("HYDRAHOOK")->clone("d3d11")->info(
							                                  "++ IDXGISwapChain::ResizeTarget called");
					                                  });

					                                  HYDRAHOOK_EVT_PRE_EXTENSION pre;
					                                  HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
						                                  &pre, engine, engine->CustomContext);
					                                  HYDRAHOOK_EVT_POST_EXTENSION post;
					                                  HYDRAHOOK_EVT_POST_EXTENSION_INIT(
						                                  &post, engine, engine->CustomContext);

					                                  INVOKE_D3D11_CALLBACK(
						                                  engine,
						                                  EvtHydraHookD3D11PreResizeTarget,
						                                  chain,
						                                  pNewTargetParameters,
						                                  &pre
					                                  );

					                                  const auto ret = swapChainResizeTarget11Hook.call_orig(
						                                  chain, pNewTargetParameters);

					                                  INVOKE_D3D11_CALLBACK(
						                                  engine,
						                                  EvtHydraHookD3D11PostResizeTarget,
						                                  chain,
						                                  pNewTargetParameters,
						                                  &post
					                                  );

					                                  return ret;
				                                  });

				logger->info("Hooking IDXGISwapChain::ResizeBuffers");

				swapChainResizeBuffers11Hook.apply(vtable[DXGIHooking::ResizeBuffers], [](
				                                   IDXGISwapChain* chain,
				                                   UINT BufferCount,
				                                   UINT Width,
				                                   UINT Height,
				                                   DXGI_FORMAT NewFormat,
				                                   UINT SwapChainFlags
			                                   ) -> HRESULT
				                                   {
					                                   ID3D11Device* pD11Device = nullptr;
					                                   if (FAILED(chain->GetDevice(IID_PPV_ARGS(&pD11Device))))
					                                   {
						                                   if (pD11Device)
						                                   {
							                                   pD11Device->Release();
						                                   }
						                                   return swapChainResizeBuffers11Hook.call_orig(chain,
							                                   BufferCount, Width, Height, NewFormat, SwapChainFlags);
					                                   }
					                                   if (pD11Device)
					                                   {
						                                   pD11Device->Release();
					                                   }

					                                   static std::once_flag flag;
					                                   std::call_once(flag, []()
					                                   {
						                                   spdlog::get("HYDRAHOOK")->clone("d3d11")->info(
							                                   "++ IDXGISwapChain::ResizeBuffers called");
					                                   });

					                                   HYDRAHOOK_EVT_PRE_EXTENSION pre;
					                                   HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
						                                   &pre, engine, engine->CustomContext);
					                                   HYDRAHOOK_EVT_POST_EXTENSION post;
					                                   HYDRAHOOK_EVT_POST_EXTENSION_INIT(
						                                   &post, engine, engine->CustomContext);

					                                   INVOKE_D3D11_CALLBACK(
						                                   engine, EvtHydraHookD3D11PreResizeBuffers, chain,
						                                   BufferCount, Width, Height, NewFormat, SwapChainFlags, &pre);

					                                   const auto ret = swapChainResizeBuffers11Hook.call_orig(chain,
						                                   BufferCount, Width, Height, NewFormat, SwapChainFlags);

					                                   INVOKE_D3D11_CALLBACK(
						                                   engine, EvtHydraHookD3D11PostResizeBuffers, chain,
						                                   BufferCount, Width, Height, NewFormat, SwapChainFlags, &post);

					                                   return ret;
				                                   });
			}
		}
		catch (DetourException& ex)
		{
			logger->error("Hooking D3D11 failed: {}", ex.what());
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

#pragma endregion

#pragma region D3D12

#ifndef HYDRAHOOK_NO_D3D12

	if (config.Direct3D.HookDirect3D12)
	{
		try
		{
			IDXGIFactory2* pFactory = nullptr;
			HMODULE hModDXGI = LoadLibraryW(L"dxgi.dll");
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

				createSwapChain12Hook.apply(reinterpret_cast<size_t>(pFactoryVtbl[CreateSwapChainIndex]), [](
				                            IUnknown* pFactoryThis,
				                            IUnknown* pDevice,
				                            DXGI_SWAP_CHAIN_DESC* pDesc,
				                            IDXGISwapChain** ppSwapChain
			                            ) -> HRESULT
				                            {
					                            const auto ret = createSwapChain12Hook.call_orig(
						                            pFactoryThis, pDevice, pDesc, ppSwapChain);
					                            if (SUCCEEDED(ret) && ppSwapChain && *ppSwapChain && pDevice)
					                            {
						                            ID3D12CommandQueue* pQueue = nullptr;
						                            if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue))))
						                            {
							                            std::lock_guard<std::mutex> lock(g_d3d12QueueMapMutex);
							                            g_d3d12SwapChainToQueue[*ppSwapChain] = pQueue;
						                            }
					                            }
					                            return ret;
				                            });

				createSwapChainForHwnd12Hook.apply(reinterpret_cast<size_t>(pFactoryVtbl[CreateSwapChainForHwndIndex]),
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
					                                   const auto ret = createSwapChainForHwnd12Hook.call_orig(
						                                   pFactoryThis, pDevice, hWnd, pDesc, pFullscreenDesc,
						                                   pRestrictToOutput, ppSwapChain);
					                                   if (SUCCEEDED(ret) && ppSwapChain && *ppSwapChain && pDevice)
					                                   {
						                                   ID3D12CommandQueue* pQueue = nullptr;
						                                   if (SUCCEEDED(
							                                   pDevice->QueryInterface(IID_PPV_ARGS(&pQueue))))
						                                   {
							                                   std::lock_guard<std::mutex> lock(g_d3d12QueueMapMutex);
							                                   g_d3d12SwapChainToQueue[*ppSwapChain] = pQueue;
						                                   }
					                                   }
					                                   return ret;
				                                   });

				logger->info("Hooking IDXGIFactory::CreateSwapChain/CreateSwapChainForHwnd for D3D12 queue capture");
				pFactory->Release();
			}

			const std::unique_ptr<Direct3D12Hooking::Direct3D12> d3d12(new Direct3D12Hooking::Direct3D12);
			auto vtable = d3d12->vtable();

			// Hook ExecuteCommandLists to capture the game's queue at runtime (supports mid-process injection)
			void** pQueueVtbl = d3d12->commandQueueVtable();
			if (pQueueVtbl)
			{
				constexpr int ExecuteCommandListsIndex = 10;
				executeCommandLists12Hook.apply(reinterpret_cast<size_t>(pQueueVtbl[ExecuteCommandListsIndex]), [](
				                                ID3D12CommandQueue* pQueue,
				                                UINT NumCommandLists,
				                                ID3D12CommandList* const* ppCommandLists
			                                ) -> void
				                                {
					                                if (pQueue)
					                                {
						                                ID3D12Device* pDevice = nullptr;
						                                if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&pDevice))) &&
							                                pDevice)
						                                {
							                                std::lock_guard<std::mutex> lock(g_d3d12QueueMapMutex);
							                                auto it = g_d3d12DeviceToQueue.find(pDevice);
							                                if (it != g_d3d12DeviceToQueue.end() && it->second)
								                                it->second->Release();
							                                pQueue->AddRef();
							                                g_d3d12DeviceToQueue[pDevice] = pQueue;
							                                pDevice->Release();
						                                }
					                                }
					                                executeCommandLists12Hook.call_orig(
						                                pQueue, NumCommandLists, ppCommandLists);
				                                });
				logger->info("Hooking ID3D12CommandQueue::ExecuteCommandLists for runtime queue capture");
			}

			logger->info("Hooking IDXGISwapChain::Present");

			swapChainPresent12Hook.apply(vtable[DXGIHooking::Present], [](
			                             IDXGISwapChain* chain,
			                             UINT SyncInterval,
			                             UINT Flags
		                             ) -> HRESULT
			                             {
				                             ID3D12Device* pD12Device = nullptr;
				                             if (FAILED(chain->GetDevice(IID_PPV_ARGS(&pD12Device))))
				                             {
					                             if (pD12Device)
					                             {
						                             pD12Device->Release();
					                             }
					                             return swapChainPresent12Hook.call_orig(chain, SyncInterval, Flags);
				                             }
				                             if (pD12Device)
				                             {
					                             pD12Device->Release();
				                             }

				                             static std::once_flag flag;
				                             std::call_once(flag, [&pChain = chain]()
				                             {
					                             spdlog::get("HYDRAHOOK")->clone("d3d12")->info(
						                             "++ IDXGISwapChain::Present called");

					                             engine->RenderPipeline.pSwapChain = pChain;

					                             INVOKE_HYDRAHOOK_GAME_HOOKED(engine, HydraHookDirect3DVersion12);
				                             });

				                             HYDRAHOOK_EVT_PRE_EXTENSION pre;
				                             HYDRAHOOK_EVT_PRE_EXTENSION_INIT(&pre, engine, engine->CustomContext);
				                             HYDRAHOOK_EVT_POST_EXTENSION post;
				                             HYDRAHOOK_EVT_POST_EXTENSION_INIT(&post, engine, engine->CustomContext);

				                             INVOKE_D3D12_CALLBACK(engine, EvtHydraHookD3D12PrePresent, chain,
				                                                   SyncInterval, Flags, &pre);

				                             const auto ret = swapChainPresent12Hook.call_orig(
					                             chain, SyncInterval, Flags);

				                             INVOKE_D3D12_CALLBACK(engine, EvtHydraHookD3D12PostPresent, chain,
				                                                   SyncInterval, Flags, &post);

				                             return ret;
			                             });

			if (vtable.size() > static_cast<size_t>(DXGIHooking::DXGI1::DXGISwapChain1VTbl::Present1))
				dxgiPresent1Address = vtable[DXGIHooking::DXGI1::Present1];
			if (vtable.size() > static_cast<size_t>(DXGIHooking::DXGI3::ResizeBuffers1))
				dxgiResizeBuffers1Address = vtable[DXGIHooking::DXGI3::ResizeBuffers1];

			logger->info("Hooking IDXGISwapChain::ResizeTarget");

			swapChainResizeTarget12Hook.apply(vtable[DXGIHooking::ResizeTarget], [](
			                                  IDXGISwapChain* chain,
			                                  const DXGI_MODE_DESC* pNewTargetParameters
		                                  ) -> HRESULT
			                                  {
				                                  ID3D12Device* pD12Device = nullptr;
				                                  if (FAILED(chain->GetDevice(IID_PPV_ARGS(&pD12Device))))
				                                  {
					                                  if (pD12Device)
					                                  {
						                                  pD12Device->Release();
					                                  }
					                                  return swapChainResizeTarget12Hook.call_orig(
						                                  chain, pNewTargetParameters);
				                                  }
				                                  if (pD12Device)
				                                  {
					                                  pD12Device->Release();
				                                  }

				                                  static std::once_flag flag;
				                                  std::call_once(flag, []()
				                                  {
					                                  spdlog::get("HYDRAHOOK")->clone("d3d12")->info(
						                                  "++ IDXGISwapChain::ResizeTarget called");
				                                  });

				                                  HYDRAHOOK_EVT_PRE_EXTENSION pre;
				                                  HYDRAHOOK_EVT_PRE_EXTENSION_INIT(&pre, engine, engine->CustomContext);
				                                  HYDRAHOOK_EVT_POST_EXTENSION post;
				                                  HYDRAHOOK_EVT_POST_EXTENSION_INIT(
					                                  &post, engine, engine->CustomContext);

				                                  INVOKE_D3D12_CALLBACK(engine, EvtHydraHookD3D12PreResizeTarget, chain,
				                                                        pNewTargetParameters, &pre);

				                                  const auto ret = swapChainResizeTarget12Hook.call_orig(
					                                  chain, pNewTargetParameters);

				                                  INVOKE_D3D12_CALLBACK(engine, EvtHydraHookD3D12PostResizeTarget,
				                                                        chain, pNewTargetParameters, &post);

				                                  return ret;
			                                  });

			logger->info("Hooking IDXGISwapChain::ResizeBuffers");

			swapChainResizeBuffers12Hook.apply(vtable[DXGIHooking::ResizeBuffers], [](
			                                   IDXGISwapChain* chain,
			                                   UINT BufferCount,
			                                   UINT Width,
			                                   UINT Height,
			                                   DXGI_FORMAT NewFormat,
			                                   UINT SwapChainFlags
		                                   ) -> HRESULT
			                                   {
				                                   ID3D12Device* pD12Device = nullptr;
				                                   if (FAILED(chain->GetDevice(IID_PPV_ARGS(&pD12Device))))
				                                   {
					                                   if (pD12Device)
					                                   {
						                                   pD12Device->Release();
					                                   }
					                                   return swapChainResizeBuffers12Hook.call_orig(chain,
						                                   BufferCount, Width, Height, NewFormat, SwapChainFlags);
				                                   }
				                                   if (pD12Device)
				                                   {
					                                   pD12Device->Release();
				                                   }

				                                   static std::once_flag flag;
				                                   std::call_once(flag, []()
				                                   {
					                                   spdlog::get("HYDRAHOOK")->clone("d3d12")->info(
						                                   "++ IDXGISwapChain::ResizeBuffers called");
				                                   });

				                                   HYDRAHOOK_EVT_PRE_EXTENSION pre;
				                                   HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
					                                   &pre, engine, engine->CustomContext);
				                                   HYDRAHOOK_EVT_POST_EXTENSION post;
				                                   HYDRAHOOK_EVT_POST_EXTENSION_INIT(
					                                   &post, engine, engine->CustomContext);

				                                   INVOKE_D3D12_CALLBACK(
					                                   engine, EvtHydraHookD3D12PreResizeBuffers, chain,
					                                   BufferCount, Width, Height, NewFormat, SwapChainFlags, &pre);

				                                   const auto ret = swapChainResizeBuffers12Hook.call_orig(chain,
					                                   BufferCount, Width, Height, NewFormat, SwapChainFlags);

				                                   INVOKE_D3D12_CALLBACK(
					                                   engine, EvtHydraHookD3D12PostResizeBuffers, chain,
					                                   BufferCount, Width, Height, NewFormat, SwapChainFlags, &post);

				                                   return ret;
			                                   });
		}
		catch (DetourException& ex)
		{
			logger->error("Hooking D3D12 failed: {}", ex.what());
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

#pragma endregion

#pragma region DXGI1+ (Present1 / ResizeBuffers1)

	try
	{
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
				                            ID3D12Device* pD12Device = nullptr;
				                            ID3D11Device* pD11Device = nullptr;
				                            ID3D10Device* pD10Device = nullptr;

				                            if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD12Device))) && pD12Device)
				                            {
					                            pD12Device->Release();

					                            if (engine->EngineConfig.Direct3D.HookDirect3D12)
					                            {
						                            static std::once_flag flag;
						                            std::call_once(flag, [&pChain = chain]()
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
						                            HYDRAHOOK_EVT_POST_EXTENSION post;
						                            HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                            &post, engine, engine->CustomContext);

						                            INVOKE_D3D12_CALLBACK(
							                            engine, EvtHydraHookD3D12PrePresent, chain, SyncInterval,
							                            PresentFlags, &pre);

						                            const auto ret = swapChainPresent1Hook.call_orig(
							                            chain, SyncInterval, PresentFlags, pPresentParameters);

						                            INVOKE_D3D12_CALLBACK(
							                            engine, EvtHydraHookD3D12PostPresent, chain, SyncInterval,
							                            PresentFlags, &post);

						                            return ret;
					                            }

					                            return swapChainPresent1Hook.call_orig(
						                            chain, SyncInterval, PresentFlags, pPresentParameters);
				                            }

				                            if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD11Device))) && pD11Device)
				                            {
					                            pD11Device->Release();

					                            if (engine->EngineConfig.Direct3D.HookDirect3D11)
					                            {
						                            static std::once_flag flag;
						                            std::call_once(flag, [&pChain = chain]()
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
						                            HYDRAHOOK_EVT_POST_EXTENSION post;
						                            HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                            &post, engine, engine->CustomContext);

						                            INVOKE_D3D11_CALLBACK(
							                            engine, EvtHydraHookD3D11PrePresent, chain, SyncInterval,
							                            PresentFlags, &pre);

						                            const auto ret = swapChainPresent1Hook.call_orig(
							                            chain, SyncInterval, PresentFlags, pPresentParameters);

						                            INVOKE_D3D11_CALLBACK(
							                            engine, EvtHydraHookD3D11PostPresent, chain, SyncInterval,
							                            PresentFlags, &post);

						                            return ret;
					                            }

					                            return swapChainPresent1Hook.call_orig(
						                            chain, SyncInterval, PresentFlags, pPresentParameters);
				                            }

				                            if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD10Device))) && pD10Device)
				                            {
					                            pD10Device->Release();

					                            if (engine->EngineConfig.Direct3D.HookDirect3D10)
					                            {
						                            static std::once_flag flag;
						                            std::call_once(flag, []()
						                            {
							                            spdlog::get("HYDRAHOOK")->clone("d3d10")->info(
								                            "++ IDXGISwapChain1::Present1 called (D3D10)");

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

					                            return swapChainPresent1Hook.call_orig(
						                            chain, SyncInterval, PresentFlags, pPresentParameters);
				                            }

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
				                                  ID3D12Device* pD12Device = nullptr;
				                                  ID3D11Device* pD11Device = nullptr;
				                                  ID3D10Device* pD10Device = nullptr;

				                                  if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD12Device))) &&
					                                  pD12Device)
				                                  {
					                                  pD12Device->Release();

					                                  if (engine->EngineConfig.Direct3D.HookDirect3D12)
					                                  {
						                                  static std::once_flag flag;
						                                  std::call_once(flag, []()
						                                  {
							                                  spdlog::get("HYDRAHOOK")->clone("d3d12")->info(
								                                  "++ IDXGISwapChain3::ResizeBuffers1 called (D3D12)");
						                                  });

						                                  HYDRAHOOK_EVT_PRE_EXTENSION pre;
						                                  HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
							                                  &pre, engine, engine->CustomContext);
						                                  HYDRAHOOK_EVT_POST_EXTENSION post;
						                                  HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                                  &post, engine, engine->CustomContext);

						                                  INVOKE_D3D12_CALLBACK(
							                                  engine, EvtHydraHookD3D12PreResizeBuffers, chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                  &pre);

						                                  const auto ret = swapChainResizeBuffers1Hook.call_orig(chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                  pCreationNodeMask, ppPresentQueue);

						                                  INVOKE_D3D12_CALLBACK(
							                                  engine, EvtHydraHookD3D12PostResizeBuffers, chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                  &post);

						                                  return ret;
					                                  }

					                                  return swapChainResizeBuffers1Hook.call_orig(chain,
						                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
						                                  pCreationNodeMask, ppPresentQueue);
				                                  }

				                                  if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD11Device))) &&
					                                  pD11Device)
				                                  {
					                                  pD11Device->Release();

					                                  if (engine->EngineConfig.Direct3D.HookDirect3D11)
					                                  {
						                                  static std::once_flag flag;
						                                  std::call_once(flag, []()
						                                  {
							                                  spdlog::get("HYDRAHOOK")->clone("d3d11")->info(
								                                  "++ IDXGISwapChain3::ResizeBuffers1 called (D3D11)");
						                                  });

						                                  HYDRAHOOK_EVT_PRE_EXTENSION pre;
						                                  HYDRAHOOK_EVT_PRE_EXTENSION_INIT(
							                                  &pre, engine, engine->CustomContext);
						                                  HYDRAHOOK_EVT_POST_EXTENSION post;
						                                  HYDRAHOOK_EVT_POST_EXTENSION_INIT(
							                                  &post, engine, engine->CustomContext);

						                                  INVOKE_D3D11_CALLBACK(
							                                  engine, EvtHydraHookD3D11PreResizeBuffers, chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                  &pre);

						                                  const auto ret = swapChainResizeBuffers1Hook.call_orig(chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                  pCreationNodeMask, ppPresentQueue);

						                                  INVOKE_D3D11_CALLBACK(
							                                  engine, EvtHydraHookD3D11PostResizeBuffers, chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                  &post);

						                                  return ret;
					                                  }

					                                  return swapChainResizeBuffers1Hook.call_orig(chain,
						                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
						                                  pCreationNodeMask, ppPresentQueue);
				                                  }

				                                  if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&pD10Device))) &&
					                                  pD10Device)
				                                  {
					                                  pD10Device->Release();

					                                  if (engine->EngineConfig.Direct3D.HookDirect3D10)
					                                  {
						                                  static std::once_flag flag;
						                                  std::call_once(flag, []()
						                                  {
							                                  spdlog::get("HYDRAHOOK")->clone("d3d10")->info(
								                                  "++ IDXGISwapChain3::ResizeBuffers1 called (D3D10)");
						                                  });

						                                  INVOKE_D3D10_CALLBACK(
							                                  engine, EvtHydraHookD3D10PreResizeBuffers, chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags);

						                                  const auto ret = swapChainResizeBuffers1Hook.call_orig(chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
							                                  pCreationNodeMask, ppPresentQueue);

						                                  INVOKE_D3D10_CALLBACK(
							                                  engine, EvtHydraHookD3D10PostResizeBuffers, chain,
							                                  BufferCount, Width, Height, NewFormat, SwapChainFlags);

						                                  return ret;
					                                  }

					                                  return swapChainResizeBuffers1Hook.call_orig(chain,
						                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
						                                  pCreationNodeMask, ppPresentQueue);
				                                  }

				                                  return swapChainResizeBuffers1Hook.call_orig(chain,
					                                  BufferCount, Width, Height, NewFormat, SwapChainFlags,
					                                  pCreationNodeMask, ppPresentQueue);
			                                  });
		}
	}
	catch (DetourException& ex)
	{
		logger->error("Hooking DXGI1+ failed: {}", ex.what());
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
				                       static std::once_flag flag;
				                       std::call_once(flag, [&pClient = client]()
				                       {
					                       spdlog::get("HYDRAHOOK")->clone("arc")->info(
						                       "++ IAudioRenderClient::GetBuffer called");

					                       engine->CoreAudio.pARC = pClient;
				                       });

				                       HYDRAHOOK_EVT_PRE_EXTENSION pre;
				                       HYDRAHOOK_EVT_PRE_EXTENSION_INIT(&pre, engine, engine->CustomContext);
				                       HYDRAHOOK_EVT_POST_EXTENSION post;
				                       HYDRAHOOK_EVT_POST_EXTENSION_INIT(&post, engine, engine->CustomContext);

				                       INVOKE_ARC_CALLBACK(engine, EvtHydraHookARCPreGetBuffer, client,
				                                           NumFramesRequested, ppData, &pre);

				                       const auto ret = arcGetBufferHook.call_orig(client, NumFramesRequested, ppData);

				                       INVOKE_ARC_CALLBACK(engine, EvtHydraHookARCPostGetBuffer, client,
				                                           NumFramesRequested, ppData, &post);

				                       return ret;
			                       });

			logger->info("Hooking IAudioRenderClient::ReleaseBuffer");

			arcReleaseBufferHook.apply(arc->vtable()[CoreAudioHooking::ReleaseBuffer], [](
			                           IAudioRenderClient* client,
			                           UINT32 NumFramesWritten,
			                           DWORD dwFlags
		                           ) -> HRESULT
			                           {
				                           static std::once_flag flag;
				                           std::call_once(flag, []()
				                           {
					                           spdlog::get("HYDRAHOOK")->clone("arc")->info(
						                           "++ IAudioRenderClient::ReleaseBuffer called");
				                           });

				                           HYDRAHOOK_EVT_PRE_EXTENSION pre;
				                           HYDRAHOOK_EVT_PRE_EXTENSION_INIT(&pre, engine, engine->CustomContext);
				                           HYDRAHOOK_EVT_POST_EXTENSION post;
				                           HYDRAHOOK_EVT_POST_EXTENSION_INIT(&post, engine, engine->CustomContext);

				                           INVOKE_ARC_CALLBACK(engine, EvtHydraHookARCPreReleaseBuffer, client,
				                                               NumFramesWritten, dwFlags, &pre);

				                           const auto ret = arcReleaseBufferHook.call_orig(
					                           client, NumFramesWritten, dwFlags);

				                           INVOKE_ARC_CALLBACK(engine, EvtHydraHookARCPostReleaseBuffer, client,
				                                               NumFramesWritten, dwFlags, &post);

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
	// Notify host that we are about to release all render pipeline hooks
	// 
	if (engine->EngineConfig.EvtHydraHookGamePreUnhook)
	{
		engine->EngineConfig.EvtHydraHookGamePreUnhook(engine);
	}

	try
	{
#ifndef HYDRAHOOK_NO_D3D9
		present9Hook.remove();
		reset9Hook.remove();
		endScene9Hook.remove();
		present9ExHook.remove();
		reset9ExHook.remove();
#endif

#ifndef HYDRAHOOK_NO_D3D10
		swapChainPresent10Hook.remove();
		swapChainResizeTarget10Hook.remove();
		swapChainResizeBuffers10Hook.remove();
#endif

#ifndef HYDRAHOOK_NO_D3D11
		swapChainPresent11Hook.remove();
		swapChainResizeTarget11Hook.remove();
		swapChainResizeBuffers11Hook.remove();
#endif

		swapChainPresent1Hook.remove();
		swapChainResizeBuffers1Hook.remove();

#ifndef HYDRAHOOK_NO_D3D12
		createSwapChain12Hook.remove();
		createSwapChainForHwnd12Hook.remove();
		executeCommandLists12Hook.remove();
		swapChainPresent12Hook.remove();
		swapChainResizeTarget12Hook.remove();
		swapChainResizeBuffers12Hook.remove();
		D3D12_ReleaseQueueMaps();
#endif

#ifndef HYDRAHOOK_NO_COREAUDIO
		arcGetBufferHook.remove();
		arcReleaseBufferHook.remove();
#endif

		logger->info("Hooks disabled");
	}
	catch (DetourException& pex)
	{
		logger->error("Unhooking failed: {}", pex.what());
	}

	//
	// Notify host that we released all render pipeline hooks
	// 
	if (engine->EngineConfig.EvtHydraHookGamePostUnhook)
	{
		engine->EngineConfig.EvtHydraHookGamePostUnhook(engine);
	}

	logger->info("Exiting worker thread");

	//
	// Decrease host DLL reference count and exit thread
	// 
	FreeLibraryAndExitThread(engine->HostInstance, 0);
}

/**
 * @brief Retrieves the ID3D12CommandQueue associated with a DXGI swap chain.
 *
 * Looks up a command queue first from the swap-chain-to-queue mapping populated at swap-chain creation,
 * and if not found, falls back to a device-to-queue mapping captured at runtime via ExecuteCommandLists.
 *
 * @param pSwapChain The swap chain to query; may be null.
 * @return ID3D12CommandQueue* The associated command queue with its reference count incremented via `AddRef`, or `nullptr` if none is available or `pSwapChain` is null.
 */
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
