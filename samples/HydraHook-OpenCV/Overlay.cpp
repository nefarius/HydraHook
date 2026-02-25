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

#include "Overlay.h"
#include <unordered_map>
#include <imgui.h>
#include <imgui_impl_win32.h>

#ifndef IMGUI_IMPL_API
#define IMGUI_IMPL_API
#endif
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

typedef LRESULT(CALLBACK* WndProc_t)(HWND, UINT, WPARAM, LPARAM);
static WndProc_t g_originalWndProc = nullptr;
static HWND g_hookedWindow = nullptr;

static LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return 0;
	return g_originalWndProc ? CallWindowProcW(g_originalWndProc, hWnd, msg, wParam, lParam) : DefWindowProcW(hWnd, msg, wParam, lParam);
}

void Overlay_HookWindowProc(HWND hWnd)
{
	if (!hWnd || g_originalWndProc)
		return;
	WNDPROC current = (WNDPROC)GetWindowLongPtrW(hWnd, GWLP_WNDPROC);
	if (current == OverlayWndProc)
		return;
	g_originalWndProc = (WndProc_t)current;
	g_hookedWindow = hWnd;
	SetWindowLongPtrW(hWnd, GWLP_WNDPROC, (LONG_PTR)OverlayWndProc);
}

void Overlay_UnhookWindowProc(void)
{
	if (g_hookedWindow && g_originalWndProc)
	{
		SetWindowLongPtrW(g_hookedWindow, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
		g_originalWndProc = nullptr;
		g_hookedWindow = nullptr;
	}
}

void Overlay_ToggleState(int key, bool& toggle)
{
	static std::unordered_map<int, bool> pressedPast;
	static std::unordered_map<int, bool> pressedNow;
	bool& past = pressedPast[key];
	bool& now = pressedNow[key];
	if (GetAsyncKeyState(key) & 0x8000)
		now = true;
	else
	{
		past = false;
		now = false;
	}
	if (!past && now)
	{
		toggle = !toggle;
		past = true;
	}
}

void Overlay_Render(float displayWidth, float displayHeight, const PerceptionResults& res)
{
	ImDrawList* draw = ImGui::GetBackgroundDrawList();
	if (!draw)
		return;

	if (!res.valid || res.currPts.empty())
		return;

	const ImU32 colPoint = IM_COL32(0, 255, 0, 255);
	const ImU32 colVector = IM_COL32(255, 200, 0, 200);
	const ImU32 colTrail = IM_COL32(255, 100, 255, 200);

	for (const auto& pt : res.currPts)
		draw->AddCircle(ImVec2(pt.x, pt.y), 3.0f, colPoint, 0, 2.0f);

	for (size_t i = 0; i < res.prevPts.size() && i < res.currPts.size(); i++)
		draw->AddLine(ImVec2(res.prevPts[i].x, res.prevPts[i].y),
			ImVec2(res.currPts[i].x, res.currPts[i].y), colVector, 1.5f);

	if (res.poseTrail.size() >= 2)
	{
		float scale = 50.0f;
		float ox = displayWidth * 0.5f;
		float oy = displayHeight * 0.8f;
		for (size_t i = 1; i < res.poseTrail.size(); i++)
		{
			const auto& a = res.poseTrail[i - 1];
			const auto& b = res.poseTrail[i];
			draw->AddLine(
				ImVec2(ox + a[0] * scale, oy - a[2] * scale),
				ImVec2(ox + b[0] * scale, oy - b[2] * scale),
				colTrail, 2.0f);
		}
	}
}

void Overlay_DrawDebugHUD(const PerceptionResults& res)
{
	ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
	ImGui::Begin("Perception HUD", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
	ImGui::Text("Features: %d", res.featureCount);
	ImGui::Text("Inliers: %d", res.inliers);
	if (res.R.data)
	{
		ImGui::Text("R: [%.2f %.2f %.2f]", res.R.at<double>(0, 0), res.R.at<double>(0, 1), res.R.at<double>(0, 2));
		if (res.t.data)
			ImGui::Text("t: [%.3f %.3f %.3f]", res.t.at<double>(0), res.t.at<double>(1), res.t.at<double>(2));
	}
	ImGui::Text("Trail: %zu pts", res.poseTrail.size());
	ImGui::End();
}
