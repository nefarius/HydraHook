#include "..\HydraHook-ImGui\dllmain.h"
#include "..\HydraHook-ImGui\KeyToggle.h"
#include "FpsConfig.h"
#include "FrameRate.h"

#include <cstdio>
#include <mutex>
#include <new>

// F12 shows or hides the counter. Shift+F12 opens the settings menu, which takes mouse and
// keyboard input only while it is open. Both shortcuts come from HydraHook-FPS.ini (see FpsConfigStore).

// Per-engine state of the FPS scene. Lives in the engine's custom context memory next to the host's.
struct OverlaySceneContext
{
	std::mutex mutex; // Guards everything below it. Always taken after the host's ImGui lock, never before.
	FpsConfig config;
	FpsConfigStore store;
	KeyToggle counterToggle;
	KeyToggle menuToggle;
	bool menuOpen = false;
	bool focusMenu = false;
	bool dirty = false;
	bool cursorDirty = false;     // One more frame is needed to apply io.MouseDrawCursor.
	ULONGLONG retryAt = 0;        // Earliest tick of the next save attempt after a failure.

	// Cached readout, refreshed by RenderScene.
	FrameRate counter;
	char label[64] = {};
};

namespace
{
	constexpr ULONGLONG kSaveRetryMs = 5000;

	void MarkDirtyLocked(OverlaySceneContext& scene)
	{
		scene.dirty = true;
		scene.retryAt = 0;
	}

	// A failed save keeps dirty and backs off, so unwritable locations do not cost file I/O per frame.
	void FlushLocked(OverlaySceneContext& scene, bool force = false)
	{
		if (!scene.dirty)
			return;
		const ULONGLONG now = ::GetTickCount64();
		if (!force && now < scene.retryAt)
			return;
		if (scene.store.Save(scene.config))
			scene.dirty = false;
		else
			scene.retryAt = now + kSaveRetryMs;
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
			char menuKey[40] = "None";
			char counterKey[40] = "None";
			char hint[96];
			FormatFpsHotkey(config.menuKey, menuKey, sizeof(menuKey));
			FormatFpsHotkey(config.counterKey, counterKey, sizeof(counterKey));
			std::snprintf(hint, sizeof(hint), "%s menu  |  %s counter", menuKey, counterKey);
			ImGui::TextUnformatted(hint);
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

size_t OverlaySceneContextSize()
{
	return sizeof(OverlaySceneContext);
}

void OverlaySceneConstruct(OverlaySceneContext* scene)
{
	new (scene) OverlaySceneContext();
}

void OverlaySceneDestroy(OverlaySceneContext* scene)
{
	scene->~OverlaySceneContext();
}

OverlayFrameState UpdateOverlayState(OverlaySceneContext* scene)
{
	FpsHotkey counterKey;
	FpsHotkey menuKey;
	{
		std::lock_guard<std::mutex> lock(scene->mutex);
		counterKey = scene->config.counterKey;
		menuKey = scene->config.menuKey;
	}

	// One snapshot, so two chords that share a primary key see the same modifiers and key state.
	const bool foreground = KeyToggle::ForegroundIsThisProcess();
	const uint8_t modifiers = foreground ? KeyToggle::CurrentModifiers() : 0;
	bool counterDown = false;
	bool menuDown = false;
	if (foreground && counterKey.vk != 0)
		counterDown = KeyToggle::IsKeyDown(counterKey.vk);
	if (foreground && menuKey.vk != 0)
		menuDown = menuKey.vk == counterKey.vk ? counterDown : KeyToggle::IsKeyDown(menuKey.vk);
	const bool toggleCounter = scene->counterToggle.Update(counterDown, modifiers, counterKey.modifiers);
	const bool toggleMenu = scene->menuToggle.Update(menuDown, modifiers, menuKey.modifiers);

	std::lock_guard<std::mutex> lock(scene->mutex);
	if (toggleCounter)
	{
		scene->config.showCounter = !scene->config.showCounter;
		MarkDirtyLocked(*scene);
	}
	if (toggleMenu)
	{
		scene->menuOpen = !scene->menuOpen;
		scene->focusMenu = scene->menuOpen;
		scene->cursorDirty = true;
	}
	if (!scene->menuOpen)
		FlushLocked(*scene);
	// Render one more frame after the menu closes so RenderScene can clear io.MouseDrawCursor.
	return { scene->config.showCounter || scene->menuOpen || scene->cursorDirty, scene->menuOpen };
}

void OnOverlayContextCreated(OverlaySceneContext* scene, ImGuiIO& io)
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

	std::lock_guard<std::mutex> lock(scene->mutex);
	scene->config = FpsConfig();
	scene->store.Init(processDir, dllDir, L"HydraHook-FPS.ini");
	if (scene->store.Load(scene->config))
		HydraHookEngineLogInfo("FPS settings loaded from %ls", scene->store.ActivePath());
	scene->menuOpen = false;
	scene->focusMenu = false;
	scene->dirty = false;
	scene->cursorDirty = false;
	scene->retryAt = 0;
	scene->counterToggle.Reset();
	scene->menuToggle.Reset();
}

void OnOverlayShutdown(OverlaySceneContext* scene)
{
	std::lock_guard<std::mutex> lock(scene->mutex);
	FlushLocked(*scene, true);
	scene->menuOpen = false;
	scene->cursorDirty = false;
	scene->counterToggle.Reset();
	scene->menuToggle.Reset();
}

// The shared ImGui host calls this once per visible frame on every backend.
void RenderScene(OverlaySceneContext* scene)
{
	if (scene->counter.Sample(FrameRate::Clock::now()))
	{
		const double frameMs = scene->counter.AverageMilliseconds();
		if (frameMs > 0.0)
			std::snprintf(scene->label, sizeof(scene->label), "%.0f FPS  |  %.1f ms", 1000.0 / frameMs, frameMs);
		else
			std::snprintf(scene->label, sizeof(scene->label), "-- FPS");
	}

	ImGuiIO& io = ImGui::GetIO();
	{
		std::lock_guard<std::mutex> lock(scene->mutex);
		FpsConfig config = scene->config;
		if (config.showCounter)
			DrawCounter(config, io, scene->label);

		// The host's game cursor is usually hidden; draw ImGui's own while the menu is open.
		io.MouseDrawCursor = scene->menuOpen;
		scene->cursorDirty = false;
		if (scene->menuOpen)
		{
			const bool focus = scene->focusMenu;
			scene->focusMenu = false;
			if (!DrawMenu(config, focus))
			{
				scene->menuOpen = false;
				scene->cursorDirty = true; // Next frame clears the cursor flag.
			}
			if (!(config == scene->config))
			{
				scene->config = config;
				MarkDirtyLocked(*scene);
			}
			// Write when no widget is held, and when the menu closes.
			if (!ImGui::IsAnyItemActive() || !scene->menuOpen)
				FlushLocked(*scene);
		}
	}
	ImGui::Render();
}
