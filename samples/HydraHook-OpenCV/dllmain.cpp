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

#include "Capture.h"
#include <HydraHook/Engine/HydraHookCore.h>

static void EvtHydraHookGameHooked(
	PHYDRAHOOK_ENGINE EngineHandle,
	const HYDRAHOOK_D3D_VERSION GameVersion
)
{
	Capture_SetupCallbacks(EngineHandle, GameVersion);
}

static void EvtHydraHookGamePreUnhook(PHYDRAHOOK_ENGINE EngineHandle)
{
	(void)EngineHandle;
	Capture_Shutdown();
}

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(static_cast<HMODULE>(hInstance));
		{
			HYDRAHOOK_ENGINE_CONFIG cfg;
			HYDRAHOOK_ENGINE_CONFIG_INIT(&cfg);
			cfg.Direct3D.HookDirect3D11 = TRUE;
			cfg.Direct3D.HookDirect3D12 = TRUE;
			cfg.EvtHydraHookGameHooked = EvtHydraHookGameHooked;
			cfg.EvtHydraHookGamePreUnhook = EvtHydraHookGamePreUnhook;
			cfg.CrashHandler.IsEnabled = TRUE;
			(void)HydraHookEngineCreate(
				static_cast<HMODULE>(hInstance),
				&cfg,
				NULL
			);
		}
		break;
	case DLL_PROCESS_DETACH:
		Capture_Shutdown();
		(void)HydraHookEngineDestroy(static_cast<HMODULE>(hInstance));
		break;
	default:
		break;
	}

	return TRUE;
}
