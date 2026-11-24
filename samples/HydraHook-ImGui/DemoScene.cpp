/*
MIT License

Copyright (c) 2018-2026 Benjamin Höglinger-Stelzer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "dllmain.h"
#include "KeyToggle.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <new>

// Per-engine state of the demo scene. Lives in the engine's custom context memory next to the host's.
struct OverlaySceneContext
{
	// F12 shows or hides the whole overlay; while it is visible it also takes input.
	std::atomic<bool> visible{ true };
	KeyToggle f12;
	std::once_flag renderLogged;

	// Plot demo state.
	bool animate = true;
	float values[90] = {};
	int valuesOffset = 0;
	float refreshTime = 0.0f;
	float phase = 0.0f;
	int funcType = 0;
	int displayCount = 70;
	float progress = 0.0f;
	float progressDir = 1.0f;
};

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
	if (scene->f12.Poll(VK_F12))
		scene->visible.store(!scene->visible.load());
	const bool visible = scene->visible.load();
	return { visible, visible };
}

void OnOverlayContextCreated(OverlaySceneContext*, ImGuiIO&)
{
}

void OnOverlayShutdown(OverlaySceneContext* scene)
{
	scene->visible.store(true);
}

/**
 * @brief Renders the ImGui overlay/demo UI containing plots and widgets.
 *
 * Displays an ImGui window with frame-time plots, line plots and histograms (including a selectable function and sample count),
 * an "Animate" toggle, and an animated progress bar with textual label. Submits ImGui draw data by calling ImGui::Render().
 */
void RenderScene(OverlaySceneContext* scene)
{
	std::call_once(scene->renderLogged, []() { HydraHookEngineLogInfo("++ RenderScene called"); });

	ImGui::ShowMetricsWindow();

	{
		ImGui::SetNextWindowPos(ImVec2(1400, 60));
		ImGui::Begin(
			"Some plots =)",
			nullptr,
			ImGuiWindowFlags_AlwaysAutoResize
		);

		ImGui::Checkbox("Animate", &scene->animate);

		static constexpr float arr[] = { 0.6f, 0.1f, 1.0f, 0.5f, 0.92f, 0.1f, 0.2f };
		ImGui::PlotLines("Frame Times", arr, IM_ARRAYSIZE(arr));

		// Create a dummy array of contiguous float values to plot
		// Tip: If your float aren't contiguous but part of a structure, you can pass a pointer to your first float and the sizeof() of your structure in the Stride parameter.
		float* values = scene->values;
		if (!scene->animate || scene->refreshTime == 0.0f)
			scene->refreshTime = (float)ImGui::GetTime();
		while (scene->refreshTime < ImGui::GetTime()) // Create dummy data at fixed 60 hz rate for the demo
		{
			values[scene->valuesOffset] = cosf(scene->phase);
			scene->valuesOffset = (scene->valuesOffset + 1) % IM_ARRAYSIZE(scene->values);
			scene->phase += 0.10f * scene->valuesOffset;
			scene->refreshTime += 1.0f / 60.0f;
		}
		ImGui::PlotLines("Lines", values, IM_ARRAYSIZE(scene->values), scene->valuesOffset, "avg 0.0", -1.0f, 1.0f, ImVec2(0, 80));
		ImGui::PlotHistogram("Histogram", arr, IM_ARRAYSIZE(arr), 0, NULL, 0.0f, 1.0f, ImVec2(0, 80));

		// Use functions to generate output
		struct Funcs
		{
			static float Sin(void*, int i) { return sinf(i * 0.1f); }
			static float Saw(void*, int i) { return (i & 1) ? 1.0f : -1.0f; }
		};
		ImGui::Separator();
		ImGui::PushItemWidth(100); ImGui::Combo("func", &scene->funcType, "Sin\0Saw\0"); ImGui::PopItemWidth();
		ImGui::SameLine();
		ImGui::SliderInt("Sample count", &scene->displayCount, 1, 400);
		float(*func)(void*, int) = (scene->funcType == 0) ? Funcs::Sin : Funcs::Saw;
		ImGui::PlotLines("Lines", func, NULL, scene->displayCount, 0, NULL, -1.0f, 1.0f, ImVec2(0, 80));
		ImGui::PlotHistogram("Histogram", func, NULL, scene->displayCount, 0, NULL, -1.0f, 1.0f, ImVec2(0, 80));
		ImGui::Separator();

		// Animate a simple progress bar
		float& progress = scene->progress;
		float& progressDir = scene->progressDir;
		if (scene->animate)
		{
			progress += progressDir * 0.4f * ImGui::GetIO().DeltaTime;
			if (progress >= +1.1f) { progress = +1.1f; progressDir *= -1.0f; }
			if (progress <= -0.1f) { progress = -0.1f; progressDir *= -1.0f; }
		}

		// Typically we would use ImVec2(-1.0f,0.0f) to use all available width, or ImVec2(width,0.0f) for a specified width. ImVec2(0.0f,0.0f) uses ItemWidth.
		ImGui::ProgressBar(progress, ImVec2(0.0f, 0.0f));
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::Text("Progress Bar");

		float progress_saturated = (progress < 0.0f) ? 0.0f : (progress > 1.0f) ? 1.0f : progress;
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%d/%d", (int)(progress_saturated * 1753), 1753);
		ImGui::ProgressBar(progress, ImVec2(0.f, 0.f), buf);

		ImGui::End();
	}

	ImGui::Render();
}
