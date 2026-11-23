#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>

// Per-window WNDPROC hook manager (Win32 only, no ImGui dependency, C++14 compatible).
//
// Installs a dispatcher through SetWindowLongPtrW (not SetWindowSubclass, which must be
// called on the window's owning thread; render callbacks usually run elsewhere), keeps the
// previous WNDPROC per window and forwards with CallWindowProcW.
//
// - Hook/Unhook/UnhookAll are serialized by a mutex. The mutex is never held while a
//   message handler or the original WNDPROC runs, so re-entrant SendMessage cannot deadlock.
// - WM_DESTROY / WM_NCDESTROY always reach the original procedure.
// - If another party subclassed the window after us, our procedure stays in the chain as a
//   pass-through (restoring would drop their hook) until the window is destroyed.
class WindowProcHook
{
public:
	using MessageHandler = LRESULT(*)(HWND, UINT, WPARAM, LPARAM);
	using ErrorLogger = void(*)(const char*);

	static WindowProcHook& Instance()
	{
		static WindowProcHook instance;
		return instance;
	}

	void SetMessageHandler(MessageHandler handler)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		messageHandler_ = handler;
	}

	void SetErrorLogger(ErrorLogger logger)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		errorLogger_ = logger;
	}

	// Returns true when the window is hooked after the call (idempotent).
	bool Hook(HWND hWnd)
	{
		if (hWnd == nullptr || !::IsWindow(hWnd))
		{
			LogError("WindowProcHook: invalid window handle");
			return false;
		}

		DWORD pid = 0;
		::GetWindowThreadProcessId(hWnd, &pid);
		if (pid != ::GetCurrentProcessId())
		{
			LogError("WindowProcHook: window belongs to another process");
			return false;
		}

		std::lock_guard<std::mutex> lock(mutex_);

		const auto it = windows_.find(hWnd);
		if (it != windows_.end())
		{
			it->second.passthrough = false;
			dispatchEnabled_.store(true);
			return true;
		}

		::SetLastError(0);
		const LONG_PTR prev = ::SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DispatchProc));
		if (prev == 0)
		{
			char buf[96];
			std::snprintf(buf, sizeof(buf), "WindowProcHook: SetWindowLongPtrW failed (error %lu)", ::GetLastError());
			LogErrorLocked(buf);
			return false;
		}
		if (prev == reinterpret_cast<LONG_PTR>(&DispatchProc))
		{
			// Already ours but untracked; cannot know the original, leave as is.
			LogErrorLocked("WindowProcHook: window already carries the dispatcher without a tracked original");
			return false;
		}

		Entry entry;
		entry.original = reinterpret_cast<WNDPROC>(prev);
		windows_[hWnd] = entry;
		dispatchEnabled_.store(true);
		return true;
	}

	// Restores the original procedure when we are still on top of the chain.
	// Otherwise the dispatcher stays installed as pass-through. Returns true if restored.
	bool Unhook(HWND hWnd)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto it = windows_.find(hWnd);
		if (it == windows_.end())
			return false;

		if (RestoreLocked(hWnd, it->second))
		{
			windows_.erase(it);
			return true;
		}

		it->second.passthrough = true;
		LogErrorLocked("WindowProcHook: window was subclassed after us; leaving pass-through in place");
		return false;
	}

	void UnhookAll()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			dispatchEnabled_.store(false);
			for (auto& kv : windows_)
			{
				if (!::IsWindow(kv.first))
				{
					kv.second.restored = true; // gone; nothing to restore
					continue;
				}
				if (RestoreLocked(kv.first, kv.second))
					kv.second.restored = true;
				else
				{
					kv.second.passthrough = true;
					LogErrorLocked("WindowProcHook: window was subclassed after us; leaving pass-through in place");
				}
			}
		}

		// Let in-flight dispatches finish before the caller tears down what the handler uses.
		// Skip when called from inside a dispatch on this thread (it would wait on itself).
		if (Depth() == 0)
		{
			const DWORD start = ::GetTickCount();
			while (inFlight_.load() != 0 && ::GetTickCount() - start < 500)
				::Sleep(1);
		}

		std::lock_guard<std::mutex> lock(mutex_);
		for (auto it = windows_.begin(); it != windows_.end();)
		{
			if (it->second.restored)
				it = windows_.erase(it);
			else
				++it;
		}
	}

	bool IsHooked(HWND hWnd)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto it = windows_.find(hWnd);
		return it != windows_.end() && !it->second.passthrough;
	}

	size_t HookedCount()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return windows_.size();
	}

private:
	struct Entry
	{
		WNDPROC original = nullptr;
		bool passthrough = false;
		bool restored = false;
	};

	WindowProcHook() = default;

	static uint32_t& Depth()
	{
		static thread_local uint32_t depth = 0;
		return depth;
	}

	struct DispatchScope
	{
		explicit DispatchScope(std::atomic<uint32_t>& counter) : counter_(counter)
		{
			++counter_;
			++Depth();
		}
		~DispatchScope()
		{
			--Depth();
			--counter_;
		}
		std::atomic<uint32_t>& counter_;
	};

	// Caller holds mutex_. Only restores when the dispatcher is the current procedure.
	static bool RestoreLocked(HWND hWnd, const Entry& entry)
	{
		const LONG_PTR current = ::GetWindowLongPtrW(hWnd, GWLP_WNDPROC);
		if (current != reinterpret_cast<LONG_PTR>(&DispatchProc))
			return false;
		::SetLastError(0);
		::SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(entry.original));
		return true;
	}

	void LogError(const char* message)
	{
		ErrorLogger logger;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			logger = errorLogger_;
		}
		if (logger)
			logger(message);
	}

	void LogErrorLocked(const char* message)
	{
		if (errorLogger_)
			errorLogger_(message);
	}

	static LRESULT CALLBACK DispatchProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		WindowProcHook& self = Instance();
		DispatchScope scope(self.inFlight_);

		WNDPROC original = nullptr;
		MessageHandler handler = nullptr;
		bool found = false;
		{
			std::lock_guard<std::mutex> lock(self.mutex_);
			const auto it = self.windows_.find(hWnd);
			if (it != self.windows_.end())
			{
				found = true;
				original = it->second.original;
				if (self.dispatchEnabled_.load() && !it->second.passthrough)
					handler = self.messageHandler_;

				if (msg == WM_DESTROY || msg == WM_NCDESTROY)
				{
					handler = nullptr;
					if (RestoreLocked(hWnd, it->second))
						self.windows_.erase(it);
					else if (msg == WM_NCDESTROY)
						self.windows_.erase(it);
				}
			}
		}

		if (!found)
			return ::DefWindowProcW(hWnd, msg, wParam, lParam);

		if (handler)
		{
			const LRESULT handled = handler(hWnd, msg, wParam, lParam);
			if (handled != 0)
				return handled;
		}

		return original ? ::CallWindowProcW(original, hWnd, msg, wParam, lParam)
			: ::DefWindowProcW(hWnd, msg, wParam, lParam);
	}

	std::mutex mutex_;
	std::unordered_map<HWND, Entry> windows_;
	MessageHandler messageHandler_ = nullptr;
	ErrorLogger errorLogger_ = nullptr;
	std::atomic<bool> dispatchEnabled_{ true };
	std::atomic<uint32_t> inFlight_{ 0 };
};
