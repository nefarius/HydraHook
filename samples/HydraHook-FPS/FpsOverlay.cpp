#include "..\HydraHook-ImGui\dllmain.h"
#include "..\HydraHook-ImGui\KeyToggle.h"
#include "FpsConfig.h"
#include "FrameRate.h"

#include <cstdio>
#include <mutex>

// F12 shows or hides the counter. F11 opens the settings menu, which takes mouse and keyboard
// input only while it is open. Settings live in HydraHook-FPS.ini (see FpsConfigStore).

namespace
{
	std::mutex g_mutex; // Guards everything below. Always taken after the host's ImGui lock, never before.
	FpsConfig g_config;
	FpsConfigStore g_store;
	bool g_menuOpen = false;
	bool g_focusMenu = false;
	bool g_dirty = false;
	bool g_cursorDirty = false;     // One more frame is needed to apply io.MouseDrawCursor.
	ULONGLONG g_retryAt = 0;        // Earliest tick of the next save attempt after a failure.
	constexpr ULONGLONG kSaveRetryMs = 5000;

	void MarkDirtyLocked()
	{
		g_dirty = true;
		g_retryAt = 0;
	}

	// A failed save keeps g_dirty and backs off, so unwritable locations do not cost file I/O per frame.
	void FlushLocked(bool force = false)
	{
		if (!g_dirty)
			return;
		const ULONGLONG now = ::GetTickCount64();
		if (!force && now < g_retryAt)
			return;
		if (g_store.Save(g_config))
			g_dirty = false;
		else
			g_retryAt = now + kSaveRetryMs;
	}

	void DrawCounter(const FpsConfig& config, const ImGuiIO& io, const char* label)
	{
		const float scale = config.scalePercent / 100.0f;
		const FpsCornerPlacement place = ComputeFpsCornerPlacement(config.corner, io.DisplaySize.x, io.DisplaySize.y, 12.0f * scale);
		ImGui::SetNextWindowPos(ImVec2(place.x, place.y), ImGuiCond_Always, ImVec2(place.pivotX, place.pivotY));
		ImGui::SetNextWindowBgAlpha(0.78f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 8.0f * scale));
		const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
			| ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
		if (ImGui::Begin("##HydraHookFPS", nullptr, flags))
		{
			ImGui::SetWindowFontScale(scale);
			ImGui::TextUnformatted(label);
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}

	// Returns false when the user closed the window.
	bool DrawMenu(FpsConfig& config, bool focus)
	{
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (focus)
			ImGui::SetNextWindowFocus();

		bool open = true;
		if (ImGui::Begin("HydraHook FPS##config", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::TextUnformatted("F11 menu  |  F12 counter");
			ImGui::Separator();
			ImGui::Checkbox("Show counter", &config.showCounter);

			int corner = static_cast<int>(config.corner);
			ImGui::TextUnformatted("Position");
			ImGui::RadioButton("Top left", &corner, 0);
			ImGui::SameLine();
			ImGui::RadioButton("Top right", &corner, 1);
			ImGui::RadioButton("Bottom left", &corner, 2);
			ImGui::SameLine();
			ImGui::RadioButton("Bottom right", &corner, 3);
			config.corner = static_cast<FpsCorner>(corner);

			// Buttons instead of a slider: SliderInt pulls in ImGui's text-input code (~85 KB on x64).
			ImGui::Text("Scale %d%%", config.scalePercent);
			ImGui::SameLine();
			ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
			if (ImGui::Button("-"))
				config.scalePercent -= 5;
			ImGui::SameLine();
			if (ImGui::Button("+"))
				config.scalePercent += 5;
			ImGui::PopItemFlag();
			ImGui::SameLine();
			if (ImGui::Button("Reset"))
				config.scalePercent = 100;
			config.scalePercent = ClampFpsScalePercent(config.scalePercent);
		}
		ImGui::End();
		return open;
	}
}

OverlayFrameState UpdateOverlayState()
{
	static KeyToggle f11, f12;
	const bool toggleCounter = f12.Poll(VK_F12);
	const bool toggleMenu = f11.Poll(VK_F11);

	std::lock_guard<std::mutex> lock(g_mutex);
	if (toggleCounter)
	{
		g_config.showCounter = !g_config.showCounter;
		MarkDirtyLocked();
	}
	if (toggleMenu)
	{
		g_menuOpen = !g_menuOpen;
		g_focusMenu = g_menuOpen;
		g_cursorDirty = true;
	}
	if (!g_menuOpen)
		FlushLocked();
	// Render one more frame after the menu closes so RenderScene can clear io.MouseDrawCursor.
	return { g_config.showCounter || g_menuOpen || g_cursorDirty, g_menuOpen };
}

void OnOverlayContextCreated(ImGuiIO& io)
{
	io.IniFilename = nullptr; // No stray imgui.ini in the game directory.
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	wchar_t processDir[MAX_PATH] = {};
	wchar_t dllDir[MAX_PATH] = {};
	FpsDirectoryOfModule(nullptr, processDir);
	HMODULE self = nullptr;
	if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&OnOverlayContextCreated), &self))
		FpsDirectoryOfModule(self, dllDir);

	std::lock_guard<std::mutex> lock(g_mutex);
	g_config = FpsConfig();
	g_store.Init(processDir, dllDir, L"HydraHook-FPS.ini");
	if (g_store.Load(g_config))
		HydraHookEngineLogInfo("FPS settings loaded from %ls", g_store.ActivePath());
	g_menuOpen = false;
	g_focusMenu = false;
	g_dirty = false;
	g_cursorDirty = false;
	g_retryAt = 0;
}

void OnOverlayShutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	FlushLocked(true);
	g_menuOpen = false;
	g_cursorDirty = false;
}

// The shared ImGui host calls this once per visible frame on every backend.
void RenderScene()
{
	static FrameRate counter;
	static char label[64];
	if (counter.Sample(FrameRate::Clock::now()))
	{
		const double frameMs = counter.AverageMilliseconds();
		if (frameMs > 0.0)
			std::snprintf(label, sizeof(label), "%.0f FPS  |  %.1f ms", 1000.0 / frameMs, frameMs);
		else
			std::snprintf(label, sizeof(label), "-- FPS");
	}

	ImGuiIO& io = ImGui::GetIO();
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		FpsConfig config = g_config;
		if (config.showCounter)
			DrawCounter(config, io, label);

		// The host's game cursor is usually hidden; draw ImGui's own while the menu is open.
		io.MouseDrawCursor = g_menuOpen;
		g_cursorDirty = false;
		if (g_menuOpen)
		{
			const bool focus = g_focusMenu;
			g_focusMenu = false;
			if (!DrawMenu(config, focus))
			{
				g_menuOpen = false;
				g_cursorDirty = true; // Next frame clears the cursor flag.
			}
			if (!(config == g_config))
			{
				g_config = config;
				MarkDirtyLocked();
			}
			// Write when no widget is held, and when the menu closes.
			if (!ImGui::IsAnyItemActive() || !g_menuOpen)
				FlushLocked();
		}
	}
	ImGui::Render();
}
