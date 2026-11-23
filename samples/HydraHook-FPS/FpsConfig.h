#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstddef>
#include <cstdio>

// Settings of the FPS overlay and their INI persistence. Win32 only (no CRT file I/O, no
// <filesystem>, no ImGui) so the DLL stays small and the logic is testable on its own.
// Numbers are stored as integers, so host code calling setlocale() cannot change the format.

enum class FpsCorner : int
{
	TopLeft = 0,
	TopRight,
	BottomLeft,
	BottomRight,
	Count
};

constexpr int kFpsMinScalePercent = 50;
constexpr int kFpsMaxScalePercent = 300;

struct FpsConfig
{
	FpsCorner corner = FpsCorner::TopLeft;
	int scalePercent = 100;
	bool showCounter = true;
};

inline const char* FpsCornerName(FpsCorner corner)
{
	static const char* const names[] = { "TopLeft", "TopRight", "BottomLeft", "BottomRight" };
	const int index = static_cast<int>(corner);
	return index >= 0 && index < static_cast<int>(FpsCorner::Count) ? names[index] : names[0];
}

inline int ClampFpsScalePercent(int percent)
{
	return percent < kFpsMinScalePercent ? kFpsMinScalePercent
		: percent > kFpsMaxScalePercent ? kFpsMaxScalePercent : percent;
}

inline bool operator==(const FpsConfig& a, const FpsConfig& b)
{
	return a.corner == b.corner && a.scalePercent == b.scalePercent && a.showCounter == b.showCounter;
}

namespace FpsConfigDetail
{
	inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

	inline bool EqualsNoCase(const char* begin, const char* end, const char* literal)
	{
		for (; begin != end; ++begin, ++literal)
		{
			char a = *begin, b = *literal;
			if (b == '\0')
				return false;
			if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
			if (a != b)
				return false;
		}
		return *literal == '\0';
	}

	inline bool ParseInt(const char* begin, const char* end, int& out)
	{
		if (begin == end || end - begin > 9)
			return false;
		int value = 0;
		for (; begin != end; ++begin)
		{
			if (*begin < '0' || *begin > '9')
				return false;
			value = value * 10 + (*begin - '0');
		}
		out = value;
		return true;
	}
}

// Applies every recognized "Key=Value" line to cfg. Unknown keys, bad values, comments and
// sections are skipped, so a damaged file never leaves cfg out of range.
inline void ParseFpsConfig(const char* text, size_t length, FpsConfig& cfg)
{
	using namespace FpsConfigDetail;
	const char* const textEnd = text + length;
	while (text < textEnd)
	{
		const char* lineEnd = text;
		while (lineEnd < textEnd && *lineEnd != '\n')
			++lineEnd;

		const char* begin = text;
		const char* end = lineEnd;
		text = lineEnd < textEnd ? lineEnd + 1 : textEnd;

		while (begin < end && IsSpace(*begin)) ++begin;
		while (end > begin && IsSpace(end[-1])) --end;
		if (begin == end || *begin == ';' || *begin == '#' || *begin == '[')
			continue;

		const char* eq = begin;
		while (eq < end && *eq != '=') ++eq;
		if (eq == end)
			continue;

		const char* keyEnd = eq;
		while (keyEnd > begin && IsSpace(keyEnd[-1])) --keyEnd;
		const char* value = eq + 1;
		while (value < end && IsSpace(*value)) ++value;

		int number = 0;
		if (EqualsNoCase(begin, keyEnd, "Corner"))
		{
			for (int i = 0; i < static_cast<int>(FpsCorner::Count); ++i)
			{
				if (EqualsNoCase(value, end, FpsCornerName(static_cast<FpsCorner>(i))))
					cfg.corner = static_cast<FpsCorner>(i);
			}
		}
		else if (EqualsNoCase(begin, keyEnd, "ScalePercent"))
		{
			if (ParseInt(value, end, number))
				cfg.scalePercent = ClampFpsScalePercent(number);
		}
		else if (EqualsNoCase(begin, keyEnd, "ShowCounter"))
		{
			if (ParseInt(value, end, number))
				cfg.showCounter = number != 0;
		}
	}
}

// Writes the INI text and returns its length (without terminator), or 0 when buffer is too small.
inline size_t FormatFpsConfig(const FpsConfig& cfg, char* buffer, size_t capacity)
{
	const int written = std::snprintf(buffer, capacity,
		"; HydraHook-FPS settings. Corner: TopLeft, TopRight, BottomLeft, BottomRight.\r\n"
		"Corner=%s\r\nScalePercent=%d\r\nShowCounter=%d\r\n",
		FpsCornerName(cfg.corner), ClampFpsScalePercent(cfg.scalePercent), cfg.showCounter ? 1 : 0);
	return written > 0 && static_cast<size_t>(written) < capacity ? static_cast<size_t>(written) : 0;
}

struct FpsCornerPlacement
{
	float x, y;           // Anchor point inside the display.
	float pivotX, pivotY; // ImGui window pivot that places the window inward from that point.
};

inline FpsCornerPlacement ComputeFpsCornerPlacement(FpsCorner corner, float displayWidth, float displayHeight, float margin)
{
	const bool right = corner == FpsCorner::TopRight || corner == FpsCorner::BottomRight;
	const bool bottom = corner == FpsCorner::BottomLeft || corner == FpsCorner::BottomRight;
	return {
		right ? displayWidth - margin : margin,
		bottom ? displayHeight - margin : margin,
		right ? 1.0f : 0.0f,
		bottom ? 1.0f : 0.0f
	};
}

// Directory of a module (null: the process executable) including the trailing backslash.
inline bool FpsDirectoryOfModule(HMODULE module, wchar_t (&directory)[MAX_PATH])
{
	const DWORD length = ::GetModuleFileNameW(module, directory, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
		return false;
	DWORD cut = length;
	while (cut > 0 && directory[cut - 1] != L'\\' && directory[cut - 1] != L'/')
		--cut;
	if (cut == 0)
		return false;
	directory[cut] = L'\0';
	return true;
}

// Finds and writes the INI file the same way the engine places HydraHook.log: the process
// directory first, then the DLL directory. The file is read from whichever holds it and
// written back there only (a failed write is reported, not redirected). Without one, the
// first directory that accepts a write wins.
class FpsConfigStore
{
public:
	static constexpr int kCandidates = 2;

	// Directories end in a backslash. Either may be empty.
	void Init(const wchar_t* processDirectory, const wchar_t* dllDirectory, const wchar_t* fileName)
	{
		Build(paths_[0], processDirectory, fileName);
		Build(paths_[1], dllDirectory, fileName);
		active_ = -1;
	}

	// Returns true when a file was found and read. cfg keeps its values otherwise.
	bool Load(FpsConfig& cfg)
	{
		for (int i = 0; i < kCandidates; ++i)
		{
			char text[1024];
			DWORD read = 0;
			if (!paths_[i][0] || !ReadAll(paths_[i], text, sizeof(text), read))
				continue;
			ParseFpsConfig(text, read, cfg);
			active_ = i;
			return true;
		}
		return false;
	}

	bool Save(const FpsConfig& cfg)
	{
		char text[512];
		const size_t length = FormatFpsConfig(cfg, text, sizeof(text));
		if (length == 0)
			return false;
		// A file that was loaded must be updated in place. Writing elsewhere would be ignored
		// by the next Load, which prefers the earlier directory.
		if (active_ >= 0)
			return WriteAll(paths_[active_], text, static_cast<DWORD>(length));
		for (int i = 0; i < kCandidates; ++i)
		{
			if (paths_[i][0] && WriteAll(paths_[i], text, static_cast<DWORD>(length)))
			{
				active_ = i;
				return true;
			}
		}
		return false;
	}

	// Path of the file that was last read or written; empty before either succeeded.
	const wchar_t* ActivePath() const { return active_ >= 0 ? paths_[active_] : L""; }

private:
	static void Build(wchar_t (&out)[MAX_PATH], const wchar_t* directory, const wchar_t* fileName)
	{
		out[0] = L'\0';
		if (directory == nullptr || directory[0] == L'\0')
			return;
		if (::lstrlenW(directory) + ::lstrlenW(fileName) >= MAX_PATH)
			return;
		::lstrcpynW(out, directory, MAX_PATH);
		::lstrcatW(out, fileName);
	}

	static bool ReadAll(const wchar_t* path, char* buffer, DWORD capacity, DWORD& read)
	{
		const HANDLE file = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		const BOOL ok = ::ReadFile(file, buffer, capacity, &read, nullptr);
		::CloseHandle(file);
		return ok != FALSE;
	}

	static bool WriteAll(const wchar_t* path, const char* data, DWORD length)
	{
		const HANDLE file = ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		DWORD written = 0;
		const BOOL ok = ::WriteFile(file, data, length, &written, nullptr);
		::CloseHandle(file);
		return ok != FALSE && written == length;
	}

	wchar_t paths_[kCandidates][MAX_PATH]{};
	int active_ = -1;
};
