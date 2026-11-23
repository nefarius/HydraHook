#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <imgui.h>

#include <cstdint>

// Dear ImGui 1.91.9 keeps a private MouseButtonsDown mask in the Win32 backend.
// The button-up path clears one bit and then calls ReleaseCapture when the mask
// hits zero and this window has capture. That releases capture the host already
// owned, and Windows delivers WM_CAPTURECHANGED before the caller can take it back.
//
// The field layout matches backends/imgui_impl_win32.cpp ImGui_ImplWin32_Data in
// v1.91.9. There is no public setter. Bump this assert when the backend layout changes.
struct ImGuiWin32Backend19190
{
	HWND hWnd;
	HWND MouseHwnd;
	int MouseTrackedArea;
	int MouseButtonsDown;
};

inline ImGuiWin32Backend19190* ImGuiWin32BackendState()
{
	static_assert(IMGUI_VERSION_NUM == 19190, "Win32 backend button-mask layout is specific to Dear ImGui 1.91.9");
	if (ImGui::GetCurrentContext() == nullptr)
		return nullptr;
	return static_cast<ImGuiWin32Backend19190*>(ImGui::GetIO().BackendPlatformUserData);
}

inline int ImGuiWin32ButtonsDown()
{
	const ImGuiWin32Backend19190* backend = ImGuiWin32BackendState();
	return backend != nullptr ? backend->MouseButtonsDown : 0;
}

// Clears forwarded button bits and the matching ImGui mouse events.
// Releases capture only when releaseCapture is set, the mask is then zero, and this window has it.
inline void ClearImGuiWin32Buttons(HWND hWnd, std::uint32_t buttons, bool releaseCapture)
{
	ImGuiWin32Backend19190* backend = ImGuiWin32BackendState();
	if (backend == nullptr)
		return;

	ImGuiIO& io = ImGui::GetIO();
	for (int i = 0; i < 5; ++i)
	{
		if ((buttons & (1u << i)) == 0)
			continue;
		if ((backend->MouseButtonsDown & (1 << i)) != 0)
			io.AddMouseButtonEvent(i, false);
		backend->MouseButtonsDown &= ~(1 << i);
	}
	if (releaseCapture && backend->MouseButtonsDown == 0 && ::GetCapture() == hWnd)
		::ReleaseCapture();
}
