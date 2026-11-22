#pragma once

#include <chrono>

class FrameRate
{
public:
	using Clock = std::chrono::steady_clock;

	// Returns true when the cached readout should be refreshed.
	bool Sample(Clock::time_point now)
	{
		if (!started || now - lastFrame >= std::chrono::seconds(2))
		{
			started = true;
			lastFrame = windowStart = now;
			intervals = 0;
			averageMilliseconds = 0.0;
			return true;
		}

		lastFrame = now;
		++intervals;
		const auto elapsed = now - windowStart;
		if (elapsed < std::chrono::milliseconds(500))
			return false;

		averageMilliseconds = std::chrono::duration<double, std::milli>(elapsed).count() / intervals;
		windowStart = now;
		intervals = 0;
		return true;
	}

	double AverageMilliseconds() const { return averageMilliseconds; }

private:
	bool started = false;
	Clock::time_point windowStart{};
	Clock::time_point lastFrame{};
	unsigned int intervals = 0;
	double averageMilliseconds = 0.0;
};
