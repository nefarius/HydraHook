#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "../HydraHook-ImGui/KeyToggle.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

// Settings of the FPS overlay and their INI persistence. Win32 only (no CRT file I/O, no
// <filesystem>, no ImGui) so the DLL stays small and the logic is testable on its own.
// Numbers and hotkey names are ASCII, so host code calling setlocale() cannot change the format.

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

struct FpsHotkey
{
	uint8_t modifiers = 0;
	uint8_t vk = 0;
};

inline constexpr FpsHotkey kDefaultCounterKey{ 0, VK_F12 };
inline constexpr FpsHotkey kDefaultMenuKey{ kKeyModShift, VK_F12 };

struct FpsConfig
{
	FpsCorner corner = FpsCorner::TopLeft;
	int scalePercent = 100;
	bool showCounter = true;
	FpsHotkey counterKey = kDefaultCounterKey;
	FpsHotkey menuKey = kDefaultMenuKey;
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

inline bool operator==(const FpsHotkey& a, const FpsHotkey& b)
{
	return a.modifiers == b.modifiers && a.vk == b.vk;
}

inline bool operator==(const FpsConfig& a, const FpsConfig& b)
{
	return a.corner == b.corner && a.scalePercent == b.scalePercent && a.showCounter == b.showCounter
		&& a.counterKey == b.counterKey && a.menuKey == b.menuKey;
}

namespace FpsConfigDetail
{
	struct VkName
	{
		const char* name;
		uint8_t vk;
	};

	// Canonical names. Numpad digits and hex virtual keys are parsed separately.
	inline const VkName kNamedKeys[] = {
		{ "Backspace", VK_BACK },
		{ "Tab", VK_TAB },
		{ "Enter", VK_RETURN },
		{ "Pause", VK_PAUSE },
		{ "Escape", VK_ESCAPE },
		{ "Space", VK_SPACE },
		{ "PageUp", VK_PRIOR },
		{ "PageDown", VK_NEXT },
		{ "End", VK_END },
		{ "Home", VK_HOME },
		{ "Left", VK_LEFT },
		{ "Up", VK_UP },
		{ "Right", VK_RIGHT },
		{ "Down", VK_DOWN },
		{ "PrintScreen", VK_SNAPSHOT },
		{ "Insert", VK_INSERT },
		{ "Delete", VK_DELETE },
		{ "NumMultiply", VK_MULTIPLY },
		{ "NumAdd", VK_ADD },
		{ "NumSubtract", VK_SUBTRACT },
		{ "NumDecimal", VK_DECIMAL },
		{ "NumDivide", VK_DIVIDE },
		{ "ScrollLock", VK_SCROLL },
	};

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

	inline bool IsModifierVirtualKey(int vk)
	{
		switch (vk)
		{
		case VK_SHIFT:
		case VK_LSHIFT:
		case VK_RSHIFT:
		case VK_CONTROL:
		case VK_LCONTROL:
		case VK_RCONTROL:
		case VK_MENU:
		case VK_LMENU:
		case VK_RMENU:
			return true;
		default:
			return false;
		}
	}

	inline bool ParseModifierToken(const char* begin, const char* end, uint8_t& modifier)
	{
		if (EqualsNoCase(begin, end, "Ctrl") || EqualsNoCase(begin, end, "Control"))
			modifier = kKeyModCtrl;
		else if (EqualsNoCase(begin, end, "Alt"))
			modifier = kKeyModAlt;
		else if (EqualsNoCase(begin, end, "Shift"))
			modifier = kKeyModShift;
		else
			return false;
		return true;
	}

	inline bool ParseHexVirtualKey(const char* begin, const char* end, int& vk)
	{
		const auto length = end - begin;
		if (length < 3 || length > 4 || begin[0] != '0' || (begin[1] != 'x' && begin[1] != 'X'))
			return false;
		int value = 0;
		for (const char* cursor = begin + 2; cursor != end; ++cursor)
		{
			int digit = 0;
			if (*cursor >= '0' && *cursor <= '9')
				digit = *cursor - '0';
			else if (*cursor >= 'a' && *cursor <= 'f')
				digit = *cursor - 'a' + 10;
			else if (*cursor >= 'A' && *cursor <= 'F')
				digit = *cursor - 'A' + 10;
			else
				return false;
			value = value * 16 + digit;
		}
		if (value < 0x01 || value > 0xFE || IsModifierVirtualKey(value))
			return false;
		vk = value;
		return true;
	}

	inline bool ParsePrimaryKey(const char* begin, const char* end, int& vk)
	{
		const auto length = end - begin;
		if (length == 1)
		{
			char letter = *begin;
			if (letter >= 'a' && letter <= 'z')
				letter = static_cast<char>(letter - 'a' + 'A');
			if ((letter >= 'A' && letter <= 'Z') || (letter >= '0' && letter <= '9'))
			{
				vk = static_cast<unsigned char>(letter);
				return true;
			}
			return false;
		}

		if (length >= 2 && length <= 3 && (*begin == 'f' || *begin == 'F') && begin[1] >= '1' && begin[1] <= '9')
		{
			int number = begin[1] - '0';
			if (length == 3)
			{
				if (begin[2] < '0' || begin[2] > '9')
					return false;
				number = number * 10 + (begin[2] - '0');
			}
			if (number < 1 || number > 24)
				return false;
			vk = VK_F1 + (number - 1);
			return true;
		}

		const char* digits = nullptr;
		const char* prefixes[] = { "Numpad", "Num" };
		for (const char* prefix : prefixes)
		{
			const char* cursor = begin;
			const char* literal = prefix;
			for (; *literal != '\0'; ++cursor, ++literal)
			{
				if (cursor == end)
				{
					cursor = nullptr;
					break;
				}
				char a = *cursor, b = *literal;
				if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
				if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
				if (a != b)
				{
					cursor = nullptr;
					break;
				}
			}
			if (cursor != nullptr && cursor + 1 == end && *cursor >= '0' && *cursor <= '9')
			{
				digits = cursor;
				break;
			}
		}
		if (digits != nullptr)
		{
			vk = VK_NUMPAD0 + (*digits - '0');
			return true;
		}

		for (const VkName& named : kNamedKeys)
		{
			if (EqualsNoCase(begin, end, named.name))
			{
				vk = named.vk;
				return true;
			}
		}

		return ParseHexVirtualKey(begin, end, vk);
	}

	inline bool FormatVirtualKeyName(uint8_t vk, char* buffer, size_t capacity)
	{
		if (vk >= 'A' && vk <= 'Z')
		{
			if (capacity < 2)
				return false;
			buffer[0] = static_cast<char>(vk);
			buffer[1] = '\0';
			return true;
		}
		if (vk >= '0' && vk <= '9')
		{
			if (capacity < 2)
				return false;
			buffer[0] = static_cast<char>(vk);
			buffer[1] = '\0';
			return true;
		}
		if (vk >= VK_F1 && vk <= VK_F24)
		{
			const int written = std::snprintf(buffer, capacity, "F%d", vk - VK_F1 + 1);
			return written > 0 && static_cast<size_t>(written) < capacity;
		}
		if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
		{
			const int written = std::snprintf(buffer, capacity, "Num%d", vk - VK_NUMPAD0);
			return written > 0 && static_cast<size_t>(written) < capacity;
		}
		for (const VkName& named : kNamedKeys)
		{
			if (named.vk == vk)
			{
				const int written = std::snprintf(buffer, capacity, "%s", named.name);
				return written > 0 && static_cast<size_t>(written) < capacity;
			}
		}
		const int written = std::snprintf(buffer, capacity, "0x%02X", vk);
		return written > 0 && static_cast<size_t>(written) < capacity;
	}
}

// "Ctrl+Alt+Shift+F12", "F12", "A", "Num0", "0xBA", or "None". Modifiers may appear in any
// order. "None" is only valid alone. Invalid text leaves out unchanged and returns false.
inline bool ParseFpsHotkey(const char* begin, const char* end, FpsHotkey& out)
{
	using namespace FpsConfigDetail;
	uint8_t modifiers = 0;
	int key = -1;
	const char* cursor = begin;
	while (cursor <= end)
	{
		const char* plus = cursor;
		while (plus < end && *plus != '+')
			++plus;

		const char* token = cursor;
		const char* tokenEnd = plus;
		while (token < tokenEnd && IsSpace(*token)) ++token;
		while (tokenEnd > token && IsSpace(tokenEnd[-1])) --tokenEnd;
		if (token == tokenEnd)
			return false;

		const bool last = plus == end;
		uint8_t modifier = 0;
		int vk = 0;
		if (EqualsNoCase(token, tokenEnd, "None"))
		{
			if (!last || modifiers != 0 || key != -1)
				return false;
			key = 0;
		}
		else if (ParseModifierToken(token, tokenEnd, modifier))
		{
			if (last || (modifiers & modifier) != 0)
				return false;
			modifiers |= modifier;
		}
		else if (ParsePrimaryKey(token, tokenEnd, vk))
		{
			if (!last || key != -1)
				return false;
			key = vk;
		}
		else
		{
			return false;
		}

		if (last)
			break;
		cursor = plus + 1;
	}

	if (key < 0)
		return false;
	out.modifiers = key == 0 ? 0 : modifiers;
	out.vk = static_cast<uint8_t>(key);
	return true;
}

// Writes the canonical name (Ctrl+Alt+Shift+Key, or None) and returns its length without the
// terminator, or 0 when buffer is too small.
inline size_t FormatFpsHotkey(const FpsHotkey& key, char* buffer, size_t capacity)
{
	using namespace FpsConfigDetail;
	if (buffer == nullptr || capacity == 0)
		return 0;
	if (key.vk == 0)
	{
		const int written = std::snprintf(buffer, capacity, "None");
		return written > 0 && static_cast<size_t>(written) < capacity ? static_cast<size_t>(written) : 0;
	}

	char name[16];
	if (!FormatVirtualKeyName(key.vk, name, sizeof(name)))
		return 0;
	const int written = std::snprintf(buffer, capacity, "%s%s%s%s",
		(key.modifiers & kKeyModCtrl) != 0 ? "Ctrl+" : "",
		(key.modifiers & kKeyModAlt) != 0 ? "Alt+" : "",
		(key.modifiers & kKeyModShift) != 0 ? "Shift+" : "",
		name);
	return written > 0 && static_cast<size_t>(written) < capacity ? static_cast<size_t>(written) : 0;
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
		else if (EqualsNoCase(begin, keyEnd, "CounterKey") || EqualsNoCase(begin, keyEnd, "MenuKey"))
		{
			FpsHotkey hotkey;
			if (ParseFpsHotkey(value, end, hotkey))
			{
				if (EqualsNoCase(begin, keyEnd, "CounterKey"))
					cfg.counterKey = hotkey;
				else
					cfg.menuKey = hotkey;
			}
		}
	}
}

// Writes the INI text and returns its length (without terminator), or 0 when buffer is too small.
inline size_t FormatFpsConfig(const FpsConfig& cfg, char* buffer, size_t capacity)
{
	char counter[40];
	char menu[40];
	if (FormatFpsHotkey(cfg.counterKey, counter, sizeof(counter)) == 0
		|| FormatFpsHotkey(cfg.menuKey, menu, sizeof(menu)) == 0)
		return 0;
	const int written = std::snprintf(buffer, capacity,
		"; HydraHook-FPS settings. Corner: TopLeft, TopRight, BottomLeft, BottomRight.\r\n"
		"; Keys: Ctrl, Alt, Shift plus a key (F12, A, Num0, 0xBA), or None.\r\n"
		"Corner=%s\r\nScalePercent=%d\r\nShowCounter=%d\r\nCounterKey=%s\r\nMenuKey=%s\r\n",
		FpsCornerName(cfg.corner), ClampFpsScalePercent(cfg.scalePercent), cfg.showCounter ? 1 : 0,
		counter, menu);
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
			char text[2048];
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
