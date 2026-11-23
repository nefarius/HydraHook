#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

// Per-window WNDPROC hook manager (Win32 only, no ImGui dependency, C++14 compatible).
//
// Installs a dispatcher through SetWindowLongPtrW. SetWindowSubclass is not used: it must
// run on the window's owning thread, and render callbacks usually run elsewhere. The previous
// WNDPROC is kept per window and called with CallWindowProcW.
//
// The mutex is never held while a message handler or the original WNDPROC runs, so a handler
// can re-enter Hook/Unhook/UnhookAll. Retired entries stay in the map. A dispatch that user32
// has already called can still be before its in-flight increment, and erasing there would
// forward through DefWindowProcW. WM_DESTROY and WM_NCDESTROY always reach the original.
//
// The module that contains DispatchProc is pinned with GET_MODULE_HANDLE_EX_FLAG_PIN before
// that procedure is installed. A later FreeLibrary does not unmap it. If another party
// subclasses the window after us, the dispatcher stays in the chain as a pass-through
// (restoring would drop their hook).
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
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		messageHandler_ = handler;
	}

	void SetErrorLogger(ErrorLogger logger)
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		errorLogger_ = logger;
	}

	// Test hook. Runs at the start of DispatchProc, before the in-flight count is incremented.
	using BeforeDispatch = void (*)(UINT msg);
	void SetBeforeDispatch(BeforeDispatch gate)
	{
		beforeDispatch_.store(gate, std::memory_order_release);
	}

	// Returns true when the window is hooked after the call (idempotent).
	// Returns false while UnhookAll is still removing hooks; the caller can retry.
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

		std::string error;
		{
			std::lock_guard<std::recursive_mutex> lock(mutex_);
			if (removing_)
			{
				error = "WindowProcHook: hook rejected while removal is in progress";
			}
			else
			{
				const auto it = windows_.find(hWnd);
				Entry* existing = nullptr;
				if (it != windows_.end())
				{
					if (!it->second.retired && !it->second.passthrough)
						return true;
					if (it->second.passthrough || it->second.inFlight != 0)
						error = "WindowProcHook: hook rejected while a previous hook is still winding down";
					else
						existing = &it->second;
				}
				if (error.empty())
				{
					// Pin before the dispatcher is reachable. A failed pin leaves the window unchanged.
					if (!PinLocked())
					{
						error = "WindowProcHook: failed to pin the module; hook was not installed";
					}
					else
					{
						::SetLastError(0);
						const LONG_PTR current = ::GetWindowLongPtrW(hWnd, GWLP_WNDPROC);
						if (current == 0 && ::GetLastError() != 0)
						{
							char buf[96];
							std::snprintf(buf, sizeof(buf), "WindowProcHook: GetWindowLongPtrW failed (error %lu)", ::GetLastError());
							error = buf;
						}
						else if (current == reinterpret_cast<LONG_PTR>(&DispatchProc))
						{
							error = "WindowProcHook: window already carries the dispatcher without a tracked original";
						}
						else
						{
							const bool inserted = existing == nullptr;
							const WNDPROC previousOriginal = existing != nullptr ? existing->original : nullptr;
							// Publish the original before swapping, so a dispatch that arrives as part of
							// SetWindowLongPtrW can still forward. A new map insert happens first: if it throws,
							// the window procedure has not changed.
							if (inserted)
							{
								Entry entry;
								entry.original = reinterpret_cast<WNDPROC>(current);
								windows_.emplace(hWnd, entry);
							}
							else
							{
								existing->original = reinterpret_cast<WNDPROC>(current);
								existing->passthrough = false;
								existing->retired = false;
							}

							::SetLastError(0);
							const LONG_PTR prev = ::SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DispatchProc));
							auto placed = windows_.find(hWnd);
							if (prev == 0 && ::GetLastError() != 0)
							{
								if (inserted && placed != windows_.end() && placed->second.inFlight == 0)
									windows_.erase(placed);
								else if (placed != windows_.end())
								{
									placed->second.original = previousOriginal;
									placed->second.retired = true;
								}
								char buf[96];
								std::snprintf(buf, sizeof(buf), "WindowProcHook: SetWindowLongPtrW failed (error %lu)", ::GetLastError());
								error = buf;
							}
							else if (placed != windows_.end())
							{
								placed->second.original = reinterpret_cast<WNDPROC>(prev);
								placed->second.retired = false;
								placed->second.passthrough = false;
								dispatchEnabled_ = true;
								return true;
							}
						}
					}
				}
			}
		}

		LogError(error.c_str());
		return false;
	}

	// Restores the original procedure when the dispatcher is still the current one.
	// Returns false when the window was not hooked, another subclass is on top, or the restore call fails.
	bool Unhook(HWND hWnd)
	{
		std::string error;
		bool restored = false;
		{
			std::lock_guard<std::recursive_mutex> lock(mutex_);
			const auto it = windows_.find(hWnd);
			if (it == windows_.end() || it->second.retired)
				return false;

			restored = RetireLocked(hWnd, it->second, &error);
		}

		if (!error.empty())
			LogError(error.c_str());
		return restored;
	}

	// Disables handlers, restores every window we are still on top of, and waits until dispatches
	// that have entered DispatchProc have finished. The wait is skipped when the caller is itself
	// inside a dispatch (it would wait for itself). Returns false when that wait was skipped.
	bool UnhookAll()
	{
		std::string error;
		{
			std::lock_guard<std::recursive_mutex> lock(mutex_);
			removing_ = true;
			dispatchEnabled_ = false;
			for (auto& kv : windows_)
			{
				if (kv.second.retired)
					continue;
				if (!::IsWindow(kv.first))
				{
					kv.second.retired = true;
					continue;
				}
				std::string one;
				RetireLocked(kv.first, kv.second, &one);
				if (!one.empty())
				{
					if (!error.empty())
						error += '\n';
					error += one;
				}
			}
		}

		const bool drained = Depth() == 0;
		if (drained)
		{
			while (entered_.load() != 0)
				::Sleep(1);
		}

		{
			std::lock_guard<std::recursive_mutex> lock(mutex_);
			removing_ = false;
		}

		if (!error.empty())
			LogError(error.c_str());
		return drained;
	}

	bool IsHooked(HWND hWnd)
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		const auto it = windows_.find(hWnd);
		return it != windows_.end() && !it->second.passthrough && !it->second.retired;
	}

	// Live and pass-through windows. Retired entries stay in the map and are not counted.
	size_t HookedCount()
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		size_t count = 0;
		for (const auto& kv : windows_)
		{
			if (!kv.second.retired)
				++count;
		}
		return count;
	}

private:
	enum class RestoreResult
	{
		Restored,
		BlockedBySubclass,
		Failed
	};

	struct Entry
	{
		WNDPROC original = nullptr;
		uint32_t inFlight = 0;
		bool passthrough = false;
		bool retired = false;
	};

	WindowProcHook() = default;

	static uint32_t& Depth()
	{
		static thread_local uint32_t depth = 0;
		return depth;
	}

	struct DispatchScope
	{
		explicit DispatchScope(std::atomic<uint32_t>& entered) : entered_(entered)
		{
			++entered_;
			++Depth();
		}
		~DispatchScope()
		{
			--Depth();
		}
		std::atomic<uint32_t>& entered_;
	};

	// Decrements the dispatch counters on every exit, including when logging throws.
	struct DispatchExit
	{
		WindowProcHook& self;
		HWND hwnd;
		bool inFlight = false;

		~DispatchExit()
		{
			std::lock_guard<std::recursive_mutex> lock(self.mutex_);
			if (inFlight)
			{
				const auto it = self.windows_.find(hwnd);
				if (it != self.windows_.end() && it->second.inFlight > 0)
					--it->second.inFlight;
			}
			if (self.entered_.load() > 0)
				--self.entered_;
		}
	};

	static RestoreResult RestoreLocked(HWND hWnd, const Entry& entry, DWORD* winError)
	{
		if (winError != nullptr)
			*winError = 0;
		const LONG_PTR current = ::GetWindowLongPtrW(hWnd, GWLP_WNDPROC);
		if (current != reinterpret_cast<LONG_PTR>(&DispatchProc))
			return RestoreResult::BlockedBySubclass;

		::SetLastError(0);
		const LONG_PTR previous = ::SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(entry.original));
		if (previous == 0 && ::GetLastError() != 0)
		{
			if (winError != nullptr)
				*winError = ::GetLastError();
			return RestoreResult::Failed;
		}
		return RestoreResult::Restored;
	}

	// Caller holds mutex_. Restored entries stay until no dispatch still needs them.
	bool RetireLocked(HWND hWnd, Entry& entry, std::string* error)
	{
		const RestoreResult result = RestoreLocked(hWnd, entry, nullptr);
		if (result == RestoreResult::Restored)
		{
			entry.retired = true;
			return true;
		}

		// The dispatcher is still reachable. Stop calling the handler, keep the original
		// procedure for forwarding, and keep this module mapped.
		entry.passthrough = true;
		if (!PinLocked() && error != nullptr)
		{
			*error = "WindowProcHook: dispatcher remains installed and pinning the module failed";
			return false;
		}
		if (error != nullptr)
		{
			*error = result == RestoreResult::BlockedBySubclass
				? "WindowProcHook: window was subclassed after us; leaving pass-through in place and pinning the module"
				: "WindowProcHook: SetWindowLongPtrW failed while restoring; dispatcher remains installed and the module is pinned";
		}
		return false;
	}

	// Caller holds mutex_. GET_MODULE_HANDLE_EX_FLAG_PIN keeps DispatchProc mapped for the
	// process lifetime, including after a later FreeLibrary of the extra reference.
	bool PinLocked()
	{
		if (pinned_)
			return true;
		HMODULE module = nullptr;
		if (!::GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCWSTR>(&DispatchProc), &module))
			return false;
		pinned_ = true;
		return true;
	}

	void LogError(const char* message)
	{
		if (message == nullptr || message[0] == '\0')
			return;
		ErrorLogger logger;
		{
			std::lock_guard<std::recursive_mutex> lock(mutex_);
			logger = errorLogger_;
		}
		if (logger)
			logger(message);
	}

	static LRESULT CALLBACK DispatchProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		WindowProcHook& self = Instance();
		if (const BeforeDispatch gate = self.beforeDispatch_.load(std::memory_order_acquire))
			gate(msg);
		DispatchScope scope(self.entered_);
		DispatchExit exit{ self, hWnd };

		WNDPROC original = nullptr;
		MessageHandler handler = nullptr;
		bool found = false;
		char destroyError[160] = {};
		{
			std::lock_guard<std::recursive_mutex> lock(self.mutex_);
			const auto it = self.windows_.find(hWnd);
			if (it != self.windows_.end())
			{
				found = true;
				original = it->second.original;
				++it->second.inFlight;
				exit.inFlight = true;
				const bool destroy = msg == WM_DESTROY || msg == WM_NCDESTROY;
				if (!destroy && self.dispatchEnabled_ && !it->second.passthrough && !it->second.retired)
					handler = self.messageHandler_;
				if (destroy)
				{
					DWORD winError = 0;
					const RestoreResult result = RestoreLocked(hWnd, it->second, &winError);
					if (result == RestoreResult::Restored || msg == WM_NCDESTROY)
						it->second.retired = true;
					else
					{
						it->second.passthrough = true;
						if (!self.PinLocked())
						{
							std::snprintf(destroyError, sizeof(destroyError),
								"WindowProcHook: dispatcher remains installed during destruction and pinning the module failed");
						}
					}
					if (result == RestoreResult::Failed)
					{
						std::snprintf(destroyError, sizeof(destroyError),
							"WindowProcHook: SetWindowLongPtrW failed during window destruction (error %lu)", winError);
					}
				}
			}
		}
		if (destroyError[0] != '\0')
			self.LogError(destroyError);

		LRESULT result;
		if (!found)
			result = ::DefWindowProcW(hWnd, msg, wParam, lParam);
		else if (handler)
		{
			const LRESULT handled = handler(hWnd, msg, wParam, lParam);
			result = handled != 0
				? handled
				: (original ? ::CallWindowProcW(original, hWnd, msg, wParam, lParam)
					: ::DefWindowProcW(hWnd, msg, wParam, lParam));
		}
		else
		{
			result = original ? ::CallWindowProcW(original, hWnd, msg, wParam, lParam)
				: ::DefWindowProcW(hWnd, msg, wParam, lParam);
		}

		return result;
	}

	std::recursive_mutex mutex_;
	std::unordered_map<HWND, Entry> windows_;
	MessageHandler messageHandler_ = nullptr;
	ErrorLogger errorLogger_ = nullptr;
	bool dispatchEnabled_ = false;
	bool removing_ = false;
	bool pinned_ = false;
	std::atomic<uint32_t> entered_{ 0 };
	std::atomic<BeforeDispatch> beforeDispatch_{ nullptr };
};
