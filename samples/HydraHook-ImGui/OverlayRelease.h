#pragma once

#include <atomic>
#include <cstdint>

// One cleanup per hide generation. A second posted message for the same generation
// is ignored, including after the overlay is shown again without a new hide.
// generation 0 is reserved for unhook and is not recorded as a hide.
// The caller serializes this with the input-release lock.
inline bool ClaimOverlayRelease(std::atomic<std::uint32_t>& current, std::atomic<std::uint32_t>& applied,
	std::uint32_t generation, bool force)
{
	if (force)
	{
		applied.store(current.load());
		return true;
	}
	if (generation == 0 || generation != current.load())
		return false;

	std::uint32_t expected = applied.load();
	if (expected == generation)
		return false;
	return applied.compare_exchange_strong(expected, generation);
}

// Caller holds the same lock as the hide that publishes a new generation.
// An older post must not clear the pending bit after that publish.
inline bool ClearPendingIfCurrent(const std::atomic<std::uint32_t>& current, std::atomic<bool>& pending,
	std::uint32_t generation)
{
	if (current.load() != generation)
		return false;
	pending.store(false);
	return true;
}

// True when unhook has closed overlay input while capture or forwarded buttons are still
// held (e.g. SendMessageTimeoutW timed out and PostMessageW failed), so the window thread
// must release them directly on the next pumped message.
inline bool ShouldFallbackReleaseOnClosedInput(const std::atomic<bool>& inputClosed,
	const std::atomic<bool>& overlayOwnsCapture, const std::atomic<std::uint32_t>& forwardedButtons)
{
	return inputClosed.load() && (overlayOwnsCapture.load() || forwardedButtons.load() != 0);
}
