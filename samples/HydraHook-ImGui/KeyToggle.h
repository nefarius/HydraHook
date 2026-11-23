#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>

// Rising-edge detector for a hotkey. Safe to call from several render threads.
class KeyToggle
{
public:
	// True once per press: the key is down now and was up at the previous call.
	bool Update(bool down)
	{
		const bool wasDown = down_.exchange(down);
		return down && !wasDown;
	}

	// Polls a virtual key. Presses are ignored unless this process owns the foreground window,
	// so a key typed in another application does not toggle the overlay.
	bool Poll(int virtualKey)
	{
		return Update(ForegroundIsThisProcess() && (::GetAsyncKeyState(virtualKey) & 0x8000) != 0);
	}

private:
	static bool ForegroundIsThisProcess()
	{
		DWORD pid = 0;
		const HWND window = ::GetForegroundWindow();
		if (window == nullptr)
			return false;
		::GetWindowThreadProcessId(window, &pid);
		return pid == ::GetCurrentProcessId();
	}

	std::atomic<bool> down_{ false };
};
