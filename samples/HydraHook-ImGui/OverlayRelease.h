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
