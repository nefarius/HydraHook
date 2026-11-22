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

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <imgui.h>

#include <HydraHook/Engine/HydraHookCore.h>
#include <HydraHook/Engine/HydraHookDirect3D9.h>
#include <HydraHook/Engine/HydraHookDirect3D10.h>
#include <HydraHook/Engine/HydraHookDirect3D11.h>
#ifdef _WIN64
#include <HydraHook/Engine/HydraHookDirect3D12.h>
#endif

typedef LRESULT(WINAPI *t_WindowProc)(
	_In_ HWND hWnd,
	_In_ UINT Msg,
	_In_ WPARAM wParam,
	_In_ LPARAM lParam
	);

LRESULT WINAPI DetourDefWindowProc(
    _In_ HWND hWnd,
    _In_ UINT Msg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
);

LRESULT WINAPI DetourWindowProc(
    _In_ HWND hWnd,
    _In_ UINT Msg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
);

void HookWindowProc(HWND hWnd);
void RenderScene();

bool ImGui_ImplWin32_UpdateMouseCursor();
IMGUI_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

EVT_HYDRAHOOK_GAME_HOOKED EvtHydraHookGameHooked;
EVT_HYDRAHOOK_GAME_UNHOOKED EvtHydraHookGameUnhooked;

EVT_HYDRAHOOK_D3D9_PRESENT EvtHydraHookD3D9Present;
EVT_HYDRAHOOK_D3D9_RESET EvtHydraHookD3D9PreReset;
EVT_HYDRAHOOK_D3D9_RESET EvtHydraHookD3D9PostReset;
EVT_HYDRAHOOK_D3D9_PRESENT_EX EvtHydraHookD3D9PresentEx;
EVT_HYDRAHOOK_D3D9_RESET_EX EvtHydraHookD3D9PreResetEx;
EVT_HYDRAHOOK_D3D9_RESET_EX EvtHydraHookD3D9PostResetEx;

EVT_HYDRAHOOK_D3D10_PRESENT EvtHydraHookD3D10Present;
EVT_HYDRAHOOK_D3D10_RESIZE_BUFFERS EvtHydraHookD3D10PreResizeBuffers;

EVT_HYDRAHOOK_D3D11_PRE_PRESENT EvtHydraHookD3D11Present;
EVT_HYDRAHOOK_D3D11_PRE_RESIZE_BUFFERS EvtHydraHookD3D11PreResizeBuffers;

#ifdef _WIN64
EVT_HYDRAHOOK_D3D12_PRE_PRESENT EvtHydraHookD3D12Present;
EVT_HYDRAHOOK_D3D12_PRE_RESIZE_BUFFERS EvtHydraHookD3D12PreResizeBuffers;
EVT_HYDRAHOOK_D3D12_POST_RESIZE_BUFFERS EvtHydraHookD3D12PostResizeBuffers;
#endif

/**
 * \fn	TOGGLE_STATE(int key, bool& toggle)
 *
 * \brief	Overly complicated key toggle helper
 *
 * \author	Benjamin "Nefarius" Höglinger-Stelzer
 * \date	7/09/2018
 *
 * \param 		  	key   	The key.
 * \param [in,out]	toggle	The value to toggle.
 */
VOID
FORCEINLINE
TOGGLE_STATE(int key, bool& toggle)
{
	static auto pressedPast = false, pressedNow = false;

	if (GetAsyncKeyState(key) & 0x8000)
	{
		pressedNow = true;
	}
	else
	{
		pressedPast = false;
		pressedNow = false;
	}

	if (!pressedPast && pressedNow)
	{
		toggle = !toggle;

		pressedPast = true;
	}
}
