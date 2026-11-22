#include "FrameRate.h"

#include <cstdio>
#include <imgui.h>

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

	ImGui::SetNextWindowPos(ImVec2(12, 12));
	ImGui::SetNextWindowBgAlpha(0.78f);
	const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
		| ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoFocusOnAppearing;
	if (ImGui::Begin("##HydraHookFPS", nullptr, flags))
		ImGui::TextUnformatted(label);
	ImGui::End();
	ImGui::Render();
}
