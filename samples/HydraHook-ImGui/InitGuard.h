#pragma once

#include <atomic>
#include <mutex>

// Resettable replacement for std::once_flag + "initialized" flag pairs.
// Allows render callbacks to re-run their initialization after an unhook/rehook cycle.
// C++14 compatible.
class ResettableInitGuard
{
public:
	// Runs fn (returns bool) once. Returns true when initialization succeeded.
	// On failure the result is latched (like std::call_once) unless retryOnFailure
	// is set, in which case the next call retries.
	template <typename Fn>
	bool EnsureInitialized(Fn&& fn, bool retryOnFailure = false)
	{
		if (attempted_.load(std::memory_order_acquire))
			return initialized_.load(std::memory_order_acquire);

		std::lock_guard<std::mutex> lock(mutex_);
		if (attempted_.load(std::memory_order_acquire))
			return initialized_.load(std::memory_order_acquire);

		const bool ok = fn();
		initialized_.store(ok, std::memory_order_relaxed);
		if (ok || !retryOnFailure)
			attempted_.store(true, std::memory_order_release);
		return ok;
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		initialized_.store(false, std::memory_order_relaxed);
		attempted_.store(false, std::memory_order_release);
	}

private:
	std::mutex mutex_;
	std::atomic<bool> attempted_{ false };
	std::atomic<bool> initialized_{ false };
};
