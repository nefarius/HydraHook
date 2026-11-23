#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <cstdint>

// Modifier bits for a chord. Matched exactly: Shift+F12 does not also match F12.
constexpr uint8_t kKeyModCtrl = 1;
constexpr uint8_t kKeyModAlt = 2;
constexpr uint8_t kKeyModShift = 4;

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

	// True once per press of the primary key while the modifier set matches exactly.
	// Releasing or adding a modifier while the primary key stays down is not a new press.
	bool Update(bool keyDown, uint8_t activeModifiers, uint8_t requiredModifiers)
	{
		const bool wasDown = down_.exchange(keyDown);
		return keyDown && !wasDown && activeModifiers == requiredModifiers;
	}

	// Polls a virtual key. Presses are ignored unless this process owns the foreground window,
	// so a key typed in another application does not toggle the overlay.
	bool Poll(int virtualKey)
	{
		return Update(ForegroundIsThisProcess() && IsKeyDown(virtualKey));
	}

	// Polls a virtual key plus an exact modifier chord. Same foreground rule as Poll(int).
	bool Poll(int virtualKey, uint8_t requiredModifiers)
	{
		if (!ForegroundIsThisProcess())
			return Update(false, 0, requiredModifiers);
		return Update(IsKeyDown(virtualKey), CurrentModifiers(), requiredModifiers);
	}

	// Forgets a held key so the next down state is a new press.
	void Reset()
	{
		down_.store(false);
	}

	static bool ForegroundIsThisProcess()
	{
		DWORD pid = 0;
		const HWND window = ::GetForegroundWindow();
		if (window == nullptr)
			return false;
		::GetWindowThreadProcessId(window, &pid);
		return pid == ::GetCurrentProcessId();
	}

	static bool IsKeyDown(int virtualKey)
	{
		return (::GetAsyncKeyState(virtualKey) & 0x8000) != 0;
	}

	// Ctrl, Alt, and Shift, including the left and right keys via the combined virtual keys.
	static uint8_t CurrentModifiers()
	{
		uint8_t modifiers = 0;
		if (IsKeyDown(VK_CONTROL))
			modifiers |= kKeyModCtrl;
		if (IsKeyDown(VK_MENU))
			modifiers |= kKeyModAlt;
		if (IsKeyDown(VK_SHIFT))
			modifiers |= kKeyModShift;
		return modifiers;
	}

private:
	std::atomic<bool> down_{ false };
};
