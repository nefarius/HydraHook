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
#include "ImGuiWin32Buttons.h"
#include "InitGuard.h"
#include "WindowProcHook.h"

// 
// STL
// 
#include <atomic>
#include <cstdint>
#include <mutex>
#include <wrl/client.h>

// 
// ImGui includes
// 
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_dx10.h>
#include <imgui_impl_dx11.h>
#ifdef _WIN64
#include <dxgi1_4.h>
#include <imgui_impl_dx12.h>
#endif
#include <imgui_impl_win32.h>

// Overlay visibility (F12). The window-proc hook reads this and only feeds ImGui while it is set.
static std::atomic<bool> g_ShowOverlay{ true };
static std::atomic<HWND> g_InputWindow{ nullptr };
// Buttons this hook forwarded to ImGui, and whether that caused the Win32 backend to take capture.
static std::atomic<uint32_t> g_ForwardedButtons{ 0 };
static std::atomic<bool> g_OverlayOwnsCapture{ false };
static std::atomic<bool> g_ReleasePending{ false };
static std::atomic<int> g_ReleaseAttempts{ 0 };
// Hide generation. A queued release carries the value from the hide that posted it.
static std::atomic<uint32_t> g_ReleaseGeneration{ 0 };
// New input stays out until the release for the current generation has run.
static std::atomic<bool> g_HoldInput{ false };
// Set before the final unhook release so a button-down cannot land after cleanup.
static std::atomic<bool> g_InputClosed{ false };

// Serializes ImGui context use between the render thread and the window thread.
// Re-entry on the thread that already holds it does not lock again: Present and a
// SendMessage it triggers can share a thread, and Dear ImGui is already on that stack.
static std::mutex g_ImGuiMutex;
static std::atomic<DWORD> g_ImGuiOwner{ 0 };

struct ImGuiScope
{
	ImGuiScope()
	{
		const DWORD self = ::GetCurrentThreadId();
		if (g_ImGuiOwner.load(std::memory_order_relaxed) == self)
			return;
		g_ImGuiMutex.lock();
		g_ImGuiOwner.store(self, std::memory_order_relaxed);
		locked_ = true;
	}

	~ImGuiScope()
	{
		if (!locked_)
			return;
		g_ImGuiOwner.store(0, std::memory_order_relaxed);
		g_ImGuiMutex.unlock();
	}

	ImGuiScope(const ImGuiScope&) = delete;
	ImGuiScope& operator=(const ImGuiScope&) = delete;

private:
	bool locked_ = false;
};

static UINT OverlayReleaseMessage()
{
	static UINT message = ::RegisterWindowMessageW(L"HydraHook.ImGui.ReleaseOverlayInput");
	static bool logged = false;
	if (message == 0 && !logged)
	{
		logged = true;
		HydraHookEngineLogError("Couldn't register overlay input-release message (error %lu)", ::GetLastError());
	}
	return message;
}

static void RequestOverlayRelease()
{
	// Same lock as ReleaseOverlayInput. An older cleanup must not clear the hold
	// after this hide has published it.
	ImGuiScope scope;
	g_ReleaseAttempts.store(0);
	g_HoldInput.store(true);
	g_ReleaseGeneration.fetch_add(1);
	g_ReleasePending.store(true);
}

// Posts the release for the generation observed at entry. A newer hide keeps its own pending bit.
static void DeliverPendingRelease()
{
	if (!g_ReleasePending.load())
		return;

	const uint32_t generation = g_ReleaseGeneration.load();
	const HWND window = g_InputWindow.load();
	if (window == nullptr || !::IsWindow(window))
	{
		if (g_ReleaseGeneration.load() == generation)
			g_ReleasePending.store(false);
		g_HoldInput.store(false);
		g_ForwardedButtons.store(0);
		g_OverlayOwnsCapture.store(false);
		return;
	}

	const UINT message = OverlayReleaseMessage();
	if (message == 0)
	{
		if (g_ReleaseGeneration.load() == generation)
			g_ReleasePending.store(false);
		return;
	}

	if (::PostMessageW(window, message, generation, 0))
	{
		if (g_ReleaseGeneration.load() == generation)
			g_ReleasePending.store(false);
		return;
	}

	const int attempt = g_ReleaseAttempts.fetch_add(1) + 1;
	if (attempt <= 8)
		HydraHookEngineLogError("Couldn't post overlay input release (error %lu)", ::GetLastError());
}

static void PollOverlayToggle()
{
	static std::mutex mutex;
	static bool pressed = false;

	std::lock_guard<std::mutex> lock(mutex);
	const bool down = (::GetAsyncKeyState(VK_F12) & 0x8000) != 0;
	if (!down)
	{
		pressed = false;
		return;
	}
	if (pressed)
		return;

	pressed = true;
	const bool show = !g_ShowOverlay.load();
	g_ShowOverlay.store(show);
	if (!show)
		RequestOverlayRelease();
}

static bool BeginOverlayFrame()
{
	PollOverlayToggle();
	DeliverPendingRelease();
	return g_ShowOverlay.load();
}

static HYDRAHOOK_D3D_VERSION g_GameVersion = HydraHookDirect3DVersionUnknown;

// Per-backend render-path init guards; reset on unhook so a rehook re-initializes.
static ResettableInitGuard g_d3d9InitGuard;
static ResettableInitGuard g_d3d10InitGuard;
static ResettableInitGuard g_d3d11InitGuard;
#ifdef _WIN64
static ResettableInitGuard g_d3d12InitGuard;
#endif

#ifdef _WIN64
static void D3D12_CleanupInitResources();
static void D3D12_WaitForGpu();
#endif

using Microsoft::WRL::ComPtr;

static ComPtr<ID3D10Device> g_d3d10Device;
static ComPtr<ID3D10RenderTargetView> g_d3d10Target;
static ComPtr<ID3D11Device> g_d3d11Device;
static ComPtr<ID3D11DeviceContext> g_d3d11Context;
static ComPtr<ID3D11RenderTargetView> g_d3d11Target;

template<typename Context>
struct ShaderResourceStage
{
	decltype(&Context::PSGetShaderResources) get;
	decltype(&Context::PSSetShaderResources) set;
};

static constexpr ShaderResourceStage<ID3D10Device> d3d10Stages[] = {
	{ &ID3D10Device::VSGetShaderResources, &ID3D10Device::VSSetShaderResources },
	{ &ID3D10Device::GSGetShaderResources, &ID3D10Device::GSSetShaderResources },
	{ &ID3D10Device::PSGetShaderResources, &ID3D10Device::PSSetShaderResources },
};

static constexpr ShaderResourceStage<ID3D11DeviceContext> d3d11Stages[] = {
	{ &ID3D11DeviceContext::VSGetShaderResources, &ID3D11DeviceContext::VSSetShaderResources },
	{ &ID3D11DeviceContext::HSGetShaderResources, &ID3D11DeviceContext::HSSetShaderResources },
	{ &ID3D11DeviceContext::DSGetShaderResources, &ID3D11DeviceContext::DSSetShaderResources },
	{ &ID3D11DeviceContext::GSGetShaderResources, &ID3D11DeviceContext::GSSetShaderResources },
	{ &ID3D11DeviceContext::PSGetShaderResources, &ID3D11DeviceContext::PSSetShaderResources },
	{ &ID3D11DeviceContext::CSGetShaderResources, &ID3D11DeviceContext::CSSetShaderResources },
};

// D3D10 and D3D11 may enter Present with the back buffer unbound. Bind it for
// the overlay draw and restore all game render targets before returning.
template<typename Resource, typename DepthView, typename ShaderView, typename Device, typename Context, typename Target, size_t StageCount>
static void DrawToBackBuffer(IDXGISwapChain* chain, Device* device, Context* context,
	ComPtr<Target>& target, const ShaderResourceStage<Context> (&stages)[StageCount], void (*draw)(ImDrawData*))
{
	// Key the view by the presenting chain's actual buffer without retaining the chain.
	ComPtr<Resource> buffer;
	if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer))))
		return;
	ComPtr<Resource> cachedBuffer;
	if (target)
		target->GetResource(&cachedBuffer);
	if (cachedBuffer.Get() != buffer.Get())
	{
		if (FAILED(device->CreateRenderTargetView(buffer.Get(), nullptr, target.ReleaseAndGetAddressOf())))
			return;
	}

	Target* previous[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
	ComPtr<DepthView> depth;
	context->OMGetRenderTargets(ARRAYSIZE(previous), previous, &depth);
	// Binding the back buffer for output can clear overlapping SRVs in any shader stage.
	constexpr UINT slotCount = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
	ShaderView* resources[StageCount][slotCount]{};
	for (size_t i = 0; i < StageCount; ++i)
		(context->*stages[i].get)(0, slotCount, resources[i]);
	context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
	draw(ImGui::GetDrawData());
	context->OMSetRenderTargets(ARRAYSIZE(previous), previous, depth.Get());
	for (size_t i = 0; i < StageCount; ++i)
	{
		(context->*stages[i].set)(0, slotCount, resources[i]);
		for (auto* view : resources[i])
			if (view)
				view->Release();
	}
	for (auto* view : previous)
		if (view)
			view->Release();
}

HYDRAHOOK_DEFINE_DLLMAIN(
	cfg.Direct3D.HookDirect3D9 = TRUE;
	cfg.Direct3D.HookDirect3D10 = TRUE;
	cfg.Direct3D.HookDirect3D11 = TRUE;
	cfg.Direct3D.HookDirect3D12 = sizeof(void*) == 8;
	cfg.EvtHydraHookGameHooked = EvtHydraHookGameHooked;
	cfg.EvtHydraHookGamePostUnhook = EvtHydraHookGameUnhooked;
	cfg.CrashHandler.IsEnabled = TRUE;
)

/**
 * @brief Initializes ImGui and registers Direct3D event callbacks for the detected rendering version.
 *
 * Creates a Dear ImGui context, applies default styling, prepares per-API callback structures
 * for D3D9/D3D10/D3D11 (and D3D12 on 64-bit builds) and registers the appropriate callbacks
 * with the provided HydraHook engine according to the detected GameVersion.
 *
 * @param EngineHandle Handle to the HydraHook engine used to register event callbacks.
 * @param GameVersion Detected Direct3D version for which to install the event callbacks.
 */
void EvtHydraHookGameHooked(
	PHYDRAHOOK_ENGINE EngineHandle,
	const HYDRAHOOK_D3D_VERSION GameVersion
)
{
	// Present, Present1 and resize hooks can announce the same renderer.
	if (g_GameVersion != HydraHookDirect3DVersionUnknown)
		return;

	HydraHookEngineLogInfo("Loading ImGui plugin");

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // Enable Gamepad Controls

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	//ImGui::StyleColorsClassic();

	HYDRAHOOK_D3D9_EVENT_CALLBACKS d3d9;
	HYDRAHOOK_D3D9_EVENT_CALLBACKS_INIT(&d3d9);
	d3d9.EvtHydraHookD3D9PrePresent = EvtHydraHookD3D9Present;
	d3d9.EvtHydraHookD3D9PreReset = EvtHydraHookD3D9PreReset;
	d3d9.EvtHydraHookD3D9PostReset = EvtHydraHookD3D9PostReset;
	d3d9.EvtHydraHookD3D9PrePresentEx = EvtHydraHookD3D9PresentEx;
	d3d9.EvtHydraHookD3D9PreResetEx = EvtHydraHookD3D9PreResetEx;
	d3d9.EvtHydraHookD3D9PostResetEx = EvtHydraHookD3D9PostResetEx;

	HYDRAHOOK_D3D10_EVENT_CALLBACKS d3d10;
	HYDRAHOOK_D3D10_EVENT_CALLBACKS_INIT(&d3d10);
	d3d10.EvtHydraHookD3D10PrePresent = EvtHydraHookD3D10Present;
	d3d10.EvtHydraHookD3D10PreResizeBuffers = EvtHydraHookD3D10PreResizeBuffers;

	HYDRAHOOK_D3D11_EVENT_CALLBACKS d3d11;
	HYDRAHOOK_D3D11_EVENT_CALLBACKS_INIT(&d3d11);
	d3d11.EvtHydraHookD3D11PrePresent = EvtHydraHookD3D11Present;
	d3d11.EvtHydraHookD3D11PreResizeBuffers = EvtHydraHookD3D11PreResizeBuffers;

#ifdef _WIN64 // vcpkg doesn't offer x86 builds for ImGui at the moment
	HYDRAHOOK_D3D12_EVENT_CALLBACKS d3d12;
	HYDRAHOOK_D3D12_EVENT_CALLBACKS_INIT(&d3d12);
	d3d12.EvtHydraHookD3D12PrePresent = EvtHydraHookD3D12Present;
	d3d12.EvtHydraHookD3D12PreResizeBuffers = EvtHydraHookD3D12PreResizeBuffers;
	d3d12.EvtHydraHookD3D12PostResizeBuffers = EvtHydraHookD3D12PostResizeBuffers;
#endif

	g_GameVersion = GameVersion;

	switch (GameVersion)
	{
	case HydraHookDirect3DVersion9:
		HydraHookEngineSetD3D9EventCallbacks(EngineHandle, &d3d9);
		break;
	case HydraHookDirect3DVersion10:
		HydraHookEngineSetD3D10EventCallbacks(EngineHandle, &d3d10);
		break;
	case HydraHookDirect3DVersion11:
		HydraHookEngineSetD3D11EventCallbacks(EngineHandle, &d3d11);
		break;
#ifdef _WIN64
	case HydraHookDirect3DVersion12:
		HydraHookEngineSetD3D12EventCallbacks(EngineHandle, &d3d12);
		break;
#endif
	}
}

/**
 * \fn  void EvtHydraHookGameUnhooked()
 *
 * \brief   Gets called when all core engine hooks have been released. At this stage it is safe
 *          to remove our own additional hooks and shut down the hooking sub-system as well.
 *
 * \author  Benjamin "Nefarius" Höglinger
 * \date    16.06.2018
 */
void EvtHydraHookGameUnhooked(
	PHYDRAHOOK_ENGINE EngineHandle)
{
	(void)EngineHandle;

	HydraHookEngineLogInfo("Unhooking ImGui overlay");

	// Restore window procedures and wait for in-flight handlers before ImGui is torn down.
	// A handler that called unhook itself cannot be waited on; destroying the context under it is a use-after-free.
	if (!UnhookWindowProc())
	{
		HydraHookEngineLogError("WNDPROC unhook ran inside a message handler; leaving the ImGui context alive");
		return;
	}

	ImGuiScope imguiShutdown;

	// Shutdown ImGui backend based on detected game version (order: backend first, then Win32, then context)
	if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)
	{
		switch (g_GameVersion)
		{
		case HydraHookDirect3DVersion9:
			ImGui_ImplDX9_Shutdown();
			break;
		case HydraHookDirect3DVersion10:
			ImGui_ImplDX10_Shutdown();
			break;
		case HydraHookDirect3DVersion11:
			ImGui_ImplDX11_Shutdown();
			break;
#ifdef _WIN64
		case HydraHookDirect3DVersion12:
			D3D12_WaitForGpu();
			ImGui_ImplDX12_Shutdown();
			break;
#endif
		default:
			break;
		}
	}

	if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendPlatformUserData)
		ImGui_ImplWin32_Shutdown();
	if (ImGui::GetCurrentContext() != nullptr)
	{
		ImGui::DestroyContext();
	}

	g_d3d10Target.Reset();
	g_d3d10Device.Reset();
	g_d3d11Target.Reset();
	g_d3d11Context.Reset();
	g_d3d11Device.Reset();

#ifdef _WIN64
	// Release D3D12 overlay resources (device, queue, heaps, etc. created by overlay)
	D3D12_CleanupInitResources();
#endif

	// Allow render callbacks to re-initialize on a rehook without a DLL reload
	g_d3d9InitGuard.Reset();
	g_d3d10InitGuard.Reset();
	g_d3d11InitGuard.Reset();
#ifdef _WIN64
	g_d3d12InitGuard.Reset();
#endif

	g_ShowOverlay.store(true);
	g_GameVersion = HydraHookDirect3DVersionUnknown;
}

#pragma region D3D9(Ex)

void EvtHydraHookD3D9Present(
	LPDIRECT3DDEVICE9   pDevice,
	const RECT          *pSourceRect,
	const RECT          *pDestRect,
	HWND                hDestWindowOverride,
	const RGNDATA       *pDirtyRegion
)
{
	//
	// This section is only called once to initialize ImGui
	// 
	const bool initialized = g_d3d9InitGuard.EnsureInitialized([&]()
	{
		D3DDEVICE_CREATION_PARAMETERS params;

		const auto hr = pDevice->GetCreationParameters(&params);
		if (FAILED(hr))
		{
			HydraHookEngineLogError("Couldn't get creation parameters from device");
			return false;
		}

		ImGui_ImplWin32_Init(params.hFocusWindow);
		ImGui_ImplDX9_Init(pDevice);

		HydraHookEngineLogInfo("ImGui (DX9) initialized");

		HookWindowProc(params.hFocusWindow);

		return true;
	});

	if (!initialized)
		return;

	if (!BeginOverlayFrame())
		return;

	ImGuiScope imguiFrame;

	// Start the Dear ImGui frame
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	RenderScene();

	if (SUCCEEDED(pDevice->BeginScene()))
	{
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
		pDevice->EndScene();
	}
}

void EvtHydraHookD3D9PreReset(
	LPDIRECT3DDEVICE9       pDevice,
	D3DPRESENT_PARAMETERS   *pPresentationParameters
)
{
	ImGuiScope imguiDevice;
	if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)
		ImGui_ImplDX9_InvalidateDeviceObjects();
}

void EvtHydraHookD3D9PostReset(
	LPDIRECT3DDEVICE9       pDevice,
	D3DPRESENT_PARAMETERS   *pPresentationParameters
)
{
	ImGuiScope imguiDevice;
	if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)
		ImGui_ImplDX9_CreateDeviceObjects();
}

void EvtHydraHookD3D9PresentEx(
	LPDIRECT3DDEVICE9EX     pDevice,
	const RECT              *pSourceRect,
	const RECT              *pDestRect,
	HWND                    hDestWindowOverride,
	const RGNDATA           *pDirtyRegion,
	DWORD                   dwFlags
)
{
	if (!(dwFlags & D3DPRESENT_DONOTFLIP))
		EvtHydraHookD3D9Present(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

void EvtHydraHookD3D9PreResetEx(
	LPDIRECT3DDEVICE9EX     pDevice,
	D3DPRESENT_PARAMETERS   *pPresentationParameters,
	D3DDISPLAYMODEEX        *pFullscreenDisplayMode
)
{
	ImGuiScope imguiDevice;
	if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)
		ImGui_ImplDX9_InvalidateDeviceObjects();
}

void EvtHydraHookD3D9PostResetEx(
	LPDIRECT3DDEVICE9EX     pDevice,
	D3DPRESENT_PARAMETERS   *pPresentationParameters,
	D3DDISPLAYMODEEX        *pFullscreenDisplayMode
)
{
	ImGuiScope imguiDevice;
	if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)
		ImGui_ImplDX9_CreateDeviceObjects();
}

#pragma endregion

#pragma region D3D10

void EvtHydraHookD3D10Present(
	IDXGISwapChain  *pSwapChain,
	UINT            SyncInterval,
	UINT            Flags
)
{
	if (Flags & DXGI_PRESENT_TEST)
		return;

	//
	// This section is only called once to initialize ImGui
	// 
	const bool initialized = g_d3d10InitGuard.EnsureInitialized([&]()
	{
		HydraHookEngineLogInfo("Grabbing device and context pointers");

		if (FAILED(D3D10_DEVICE_FROM_SWAPCHAIN(pSwapChain, &g_d3d10Device)))
		{
			HydraHookEngineLogError("Couldn't get device from swapchain");
			return false;
		}

		DXGI_SWAP_CHAIN_DESC sd;
		pSwapChain->GetDesc(&sd);

		HydraHookEngineLogInfo("Initializing ImGui");

		ImGui_ImplWin32_Init(sd.OutputWindow);
		ImGui_ImplDX10_Init(g_d3d10Device.Get());

		HydraHookEngineLogInfo("ImGui (DX10) initialized");

		HookWindowProc(sd.OutputWindow);

		return true;
	});

	if (!initialized)
		return;

	if (!BeginOverlayFrame())
		return;

	ImGuiScope imguiFrame;


	// Start the Dear ImGui frame
	ImGui_ImplDX10_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	RenderScene();

	DrawToBackBuffer<ID3D10Resource, ID3D10DepthStencilView, ID3D10ShaderResourceView>(pSwapChain, g_d3d10Device.Get(),
		g_d3d10Device.Get(), g_d3d10Target, d3d10Stages, ImGui_ImplDX10_RenderDrawData);
}

void EvtHydraHookD3D10PreResizeBuffers(
	IDXGISwapChain  *pSwapChain,
	UINT            BufferCount,
	UINT            Width,
	UINT            Height,
	DXGI_FORMAT     NewFormat,
	UINT            SwapChainFlags
)
{
	g_d3d10Target.Reset();
}

#pragma endregion

#pragma region D3D11

void EvtHydraHookD3D11Present(
	IDXGISwapChain				*pSwapChain,
	UINT						SyncInterval,
	UINT						Flags,
	PHYDRAHOOK_EVT_PRE_EXTENSION Extension
)
{
	if (Flags & DXGI_PRESENT_TEST)
		return;

	//
	// This section is only called once to initialize ImGui
	// 
	const bool initialized = g_d3d11InitGuard.EnsureInitialized([&]()
	{
		HydraHookEngineLogInfo("Grabbing device and context pointers");

		if (FAILED(D3D11_DEVICE_IMMEDIATE_CONTEXT_FROM_SWAPCHAIN(pSwapChain, &g_d3d11Device, &g_d3d11Context)))
		{
			HydraHookEngineLogError("Couldn't get device and context from swapchain");
			return false;
		}

		DXGI_SWAP_CHAIN_DESC sd;
		pSwapChain->GetDesc(&sd);

		HydraHookEngineLogInfo("Initializing ImGui");

		ImGui_ImplWin32_Init(sd.OutputWindow);
		ImGui_ImplDX11_Init(g_d3d11Device.Get(), g_d3d11Context.Get());

		HydraHookEngineLogInfo("ImGui (DX11) initialized");

		HookWindowProc(sd.OutputWindow);

		return true;
	});

	if (!initialized)
		return;

	if (!BeginOverlayFrame())
		return;

	ImGuiScope imguiFrame;

	// Start the Dear ImGui frame
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	RenderScene();

	DrawToBackBuffer<ID3D11Resource, ID3D11DepthStencilView, ID3D11ShaderResourceView>(pSwapChain, g_d3d11Device.Get(),
		g_d3d11Context.Get(), g_d3d11Target, d3d11Stages, ImGui_ImplDX11_RenderDrawData);
}

//
// Called prior to the original invocation of ResizeBuffers.
// 
void EvtHydraHookD3D11PreResizeBuffers(
	IDXGISwapChain				*pSwapChain,
	UINT						BufferCount,
	UINT						Width,
	UINT						Height,
	DXGI_FORMAT					NewFormat,
	UINT						SwapChainFlags,
	PHYDRAHOOK_EVT_PRE_EXTENSION Extension
)
{
	g_d3d11Target.Reset();
}

#pragma endregion

#pragma region D3D12

#ifdef _WIN64

static constexpr UINT D3D12_NUM_BACK_BUFFERS = DXGI_MAX_SWAP_CHAIN_BUFFERS;
static constexpr UINT D3D12_NUM_FRAMES_IN_FLIGHT = 2;
static constexpr UINT D3D12_SRV_HEAP_SIZE = 64;

static ID3D12Device* g_d3d12_pDevice = nullptr;
static ID3D12CommandQueue* g_d3d12_pCommandQueue = nullptr;
static ID3D12CommandAllocator* g_d3d12_pCommandAllocator = nullptr;
static ID3D12GraphicsCommandList* g_d3d12_pCommandList = nullptr;
static ID3D12Fence* g_d3d12_pFence = nullptr;
static HANDLE g_d3d12_hFenceEvent = nullptr;
static UINT64 g_d3d12_fenceLastSignaledValue = 0;
static ID3D12DescriptorHeap* g_d3d12_pRtvDescHeap = nullptr;
static ID3D12DescriptorHeap* g_d3d12_pSrvDescHeap = nullptr;
static ID3D12Resource* g_d3d12_mainRenderTargetResource[D3D12_NUM_BACK_BUFFERS] = {};
static D3D12_CPU_DESCRIPTOR_HANDLE g_d3d12_mainRenderTargetDescriptor[D3D12_NUM_BACK_BUFFERS] = {};
static UINT g_d3d12_rtvDescriptorSize = 0;
static UINT g_d3d12_srvDescriptorIncrement = 0;
static UINT g_d3d12_numBackBuffers = D3D12_NUM_BACK_BUFFERS;
static UINT g_d3d12_srvDescriptorCount = 0;

/**
 * @brief Allocates the next available shader-visible SRV descriptor from the internal DX12 descriptor heap.
 *
 * Produces CPU and GPU descriptor handles for a new SRV and advances g_d3d12_srvDescriptorCount.
 * When the heap is exhausted (g_d3d12_srvDescriptorCount >= D3D12_SRV_HEAP_SIZE), the function
 * zeroes *out_cpu and *out_gpu and returns; callers receive zeroed handles on allocation failure.
 *
 * @param out_cpu Pointer to receive the CPU descriptor handle for the allocated SRV.
 * @param out_gpu Pointer to receive the GPU descriptor handle for the allocated SRV.
 */
static void D3D12_SrvDescriptorAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
{
	(void)info;
	if (g_d3d12_srvDescriptorCount >= D3D12_SRV_HEAP_SIZE)
	{
		*out_cpu = {};
		*out_gpu = {};
		return;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_d3d12_pSrvDescHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_d3d12_pSrvDescHeap->GetGPUDescriptorHandleForHeapStart();
	cpu.ptr += g_d3d12_srvDescriptorCount * g_d3d12_srvDescriptorIncrement;
	gpu.ptr += g_d3d12_srvDescriptorCount * g_d3d12_srvDescriptorIncrement;
	g_d3d12_srvDescriptorCount++;
	*out_cpu = cpu;
	*out_gpu = gpu;
}

/**
 * @brief Releases a previously allocated SRV descriptor back to the ImGui DX12 descriptor allocator.
 *
 * This function is provided to return an SRV descriptor (CPU/GPU handles) to the allocator associated
 * with the given ImGui_ImplDX12_InitInfo. In the current implementation this operation is a no-op.
 *
 * @param info Pointer to the ImGui DX12 initialization info that owns the descriptor heap.
 * @param cpu_handle CPU descriptor handle of the SRV to release.
 * @param gpu_handle GPU descriptor handle of the SRV to release.
 */
static void D3D12_SrvDescriptorFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle)
{
	(void)info;
	(void)cpu_handle;
	(void)gpu_handle;
}

static void D3D12_CleanupOverlayResources();

/**
 * @brief Creates RTV descriptor heap and render target views for the swap chain's back buffers.
 *
 * Initializes and stores render-target resources and descriptors used by the overlay.
 * On failure, logs an error and returns false; caller must call D3D12_CleanupInitResources.
 *
 * @return true on success, false on failure.
 */
static bool D3D12_CreateOverlayResources(IDXGISwapChain* pSwapChain)
{
	ID3D12Resource* pBackBuffer = nullptr;
	if (FAILED(D3D12_BACKBUFFER_FROM_SWAPCHAIN(pSwapChain, &pBackBuffer, 0)))
	{
		HydraHookEngineLogError("Couldn't get back buffer from swapchain");
		return false;
	}

	DXGI_SWAP_CHAIN_DESC sd;
	pSwapChain->GetDesc(&sd);
	g_d3d12_numBackBuffers = sd.BufferCount;
	if (g_d3d12_numBackBuffers > D3D12_NUM_BACK_BUFFERS)
		g_d3d12_numBackBuffers = D3D12_NUM_BACK_BUFFERS;

	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
	rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDesc.NumDescriptors = g_d3d12_numBackBuffers;
	rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvDesc.NodeMask = 1;
	if (FAILED(g_d3d12_pDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_d3d12_pRtvDescHeap))))
	{
		pBackBuffer->Release();
		HydraHookEngineLogError("Couldn't create RTV descriptor heap");
		return false;
	}

	g_d3d12_rtvDescriptorSize = g_d3d12_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_d3d12_pRtvDescHeap->GetCPUDescriptorHandleForHeapStart();

	for (UINT i = 0; i < g_d3d12_numBackBuffers; i++)
	{
		ID3D12Resource* pBuffer = nullptr;
		if (FAILED(pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBuffer))))
		{
			D3D12_CleanupOverlayResources();
			pBackBuffer->Release();
			HydraHookEngineLogError("Couldn't get swap chain buffer %u", i);
			return false;
		}
		g_d3d12_pDevice->CreateRenderTargetView(pBuffer, nullptr, rtvHandle);
		g_d3d12_mainRenderTargetResource[i] = pBuffer;
		g_d3d12_mainRenderTargetDescriptor[i] = rtvHandle;
		rtvHandle.ptr += g_d3d12_rtvDescriptorSize;
	}
	pBackBuffer->Release();
	return true;
}

/**
 * @brief Releases and clears D3D12 overlay render target resources and the RTV descriptor heap.
 *
 * Frees any back-buffer resources stored in g_d3d12_mainRenderTargetResource (up to
 * g_d3d12_numBackBuffers) and releases the g_d3d12_pRtvDescHeap, setting the freed pointers to nullptr.
 */
static void D3D12_CleanupOverlayResources()
{
	for (UINT i = 0; i < g_d3d12_numBackBuffers; i++)
	{
		if (g_d3d12_mainRenderTargetResource[i])
		{
			g_d3d12_mainRenderTargetResource[i]->Release();
			g_d3d12_mainRenderTargetResource[i] = nullptr;
		}
		g_d3d12_mainRenderTargetDescriptor[i] = {};
	}
	if (g_d3d12_pRtvDescHeap)
	{
		g_d3d12_pRtvDescHeap->Release();
		g_d3d12_pRtvDescHeap = nullptr;
	}
}

/**
 * @brief Releases all D3D12 init resources (device, queue, allocator, list, fence, heaps).
 * Call before early return from the initialization block to avoid leaks.
 */
static void D3D12_CleanupInitResources()
{
	D3D12_CleanupOverlayResources();
	if (g_d3d12_pSrvDescHeap) { g_d3d12_pSrvDescHeap->Release(); g_d3d12_pSrvDescHeap = nullptr; }
	if (g_d3d12_hFenceEvent) { CloseHandle(g_d3d12_hFenceEvent); g_d3d12_hFenceEvent = nullptr; }
	if (g_d3d12_pFence) { g_d3d12_pFence->Release(); g_d3d12_pFence = nullptr; }
	if (g_d3d12_pCommandList) { g_d3d12_pCommandList->Release(); g_d3d12_pCommandList = nullptr; }
	if (g_d3d12_pCommandAllocator) { g_d3d12_pCommandAllocator->Release(); g_d3d12_pCommandAllocator = nullptr; }
	if (g_d3d12_pCommandQueue) { g_d3d12_pCommandQueue->Release(); g_d3d12_pCommandQueue = nullptr; }
	if (g_d3d12_pDevice) { g_d3d12_pDevice->Release(); g_d3d12_pDevice = nullptr; }

	// Scalar state must not leak into the next init (new fence starts at 0, heap is recreated)
	g_d3d12_fenceLastSignaledValue = 0;
	g_d3d12_srvDescriptorCount = 0;
	g_d3d12_rtvDescriptorSize = 0;
	g_d3d12_srvDescriptorIncrement = 0;
	g_d3d12_numBackBuffers = D3D12_NUM_BACK_BUFFERS;
}

/**
 * @brief Blocks the CPU until the GPU has completed work up to the last signaled fence value.
 *
 * Waits on the fence event for g_d3d12_fenceLastSignaledValue (signaled by the caller before invoking).
 */
static void D3D12_WaitForGpu()
{
	if (!g_d3d12_pFence || !g_d3d12_hFenceEvent)
		return;
	const UINT64 fence = g_d3d12_fenceLastSignaledValue;
	if (g_d3d12_pFence->GetCompletedValue() >= fence)
		return;
	g_d3d12_pFence->SetEventOnCompletion(fence, g_d3d12_hFenceEvent);
	WaitForSingleObject(g_d3d12_hFenceEvent, INFINITE);
}

/**
 * @brief Renders an ImGui overlay into the provided D3D12 swap chain during Present.
 *
 * Initializes D3D12/ImGui on first invocation (device, command allocator/list, fence,
 * descriptor heaps, and overlay render targets) and on each subsequent invocation
 * records and executes GPU commands to transition the current back buffer, render
 * the ImGui frame, and transition the buffer back for presentation while synchronizing
 * with the GPU.
 *
 * @param pSwapChain The DXGI swap chain to render the overlay into.
 * @param SyncInterval Ignored by this hook.
 * @param Flags Ignored by this hook.
 * @param Extension Unused hook extension parameter.
 */
void EvtHydraHookD3D12Present(
	IDXGISwapChain* pSwapChain,
	UINT SyncInterval,
	UINT Flags,
	PHYDRAHOOK_EVT_PRE_EXTENSION Extension
)
{
	if (Flags & DXGI_PRESENT_TEST)
		return;

	(void)SyncInterval;
	(void)Extension;

	// Retries on failure: the command queue may not be captured yet (mid-process injection)
	const bool initialized = g_d3d12InitGuard.EnsureInitialized([&]() -> bool
	{
		ImGuiScope imguiInit;
		HydraHookEngineLogInfo("Grabbing D3D12 device and command queue from swapchain");

		if (FAILED(D3D12_DEVICE_FROM_SWAPCHAIN(pSwapChain, &g_d3d12_pDevice)))
		{
			HydraHookEngineLogError("Couldn't get D3D12 device from swapchain");
			return false;
		}

		g_d3d12_pCommandQueue = HydraHookEngineGetD3D12CommandQueue(pSwapChain);
		if (!g_d3d12_pCommandQueue)
		{
			HydraHookEngineLogInfo("D3D12 command queue not yet captured (mid-process injection); will retry next frame");
			D3D12_CleanupInitResources();
			return false;
		}

		if (FAILED(g_d3d12_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_d3d12_pCommandAllocator))))
		{
			HydraHookEngineLogError("Couldn't create D3D12 command allocator");
			D3D12_CleanupInitResources();
			return false;
		}

		if (FAILED(g_d3d12_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_d3d12_pCommandAllocator, nullptr, IID_PPV_ARGS(&g_d3d12_pCommandList))) ||
			FAILED(g_d3d12_pCommandList->Close()))
		{
			HydraHookEngineLogError("Couldn't create D3D12 command list");
			D3D12_CleanupInitResources();
			return false;
		}

		if (FAILED(g_d3d12_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_d3d12_pFence))))
		{
			HydraHookEngineLogError("Couldn't create D3D12 fence");
			D3D12_CleanupInitResources();
			return false;
		}

		g_d3d12_hFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!g_d3d12_hFenceEvent)
		{
			HydraHookEngineLogError("Couldn't create fence event");
			D3D12_CleanupInitResources();
			return false;
		}

		D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
		srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvDesc.NumDescriptors = D3D12_SRV_HEAP_SIZE;
		srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(g_d3d12_pDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_d3d12_pSrvDescHeap))))
		{
			HydraHookEngineLogError("Couldn't create D3D12 SRV descriptor heap");
			D3D12_CleanupInitResources();
			return false;
		}
		g_d3d12_srvDescriptorIncrement = g_d3d12_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		if (!D3D12_CreateOverlayResources(pSwapChain))
		{
			D3D12_CleanupInitResources();
			return false;
		}

		DXGI_SWAP_CHAIN_DESC sd;
		pSwapChain->GetDesc(&sd);

		ImGui_ImplDX12_InitInfo init_info = {};
		init_info.Device = g_d3d12_pDevice;
		init_info.CommandQueue = g_d3d12_pCommandQueue;
		init_info.NumFramesInFlight = D3D12_NUM_FRAMES_IN_FLIGHT;
		init_info.RTVFormat = sd.BufferDesc.Format;
		init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
		init_info.SrvDescriptorHeap = g_d3d12_pSrvDescHeap;
		init_info.SrvDescriptorAllocFn = D3D12_SrvDescriptorAlloc;
		init_info.SrvDescriptorFreeFn = D3D12_SrvDescriptorFree;

		if (!ImGui_ImplDX12_Init(&init_info))
		{
			HydraHookEngineLogError("ImGui_ImplDX12_Init failed");
			D3D12_CleanupInitResources();
			return false;
		}

		ImGui_ImplWin32_Init(sd.OutputWindow);
		HydraHookEngineLogInfo("ImGui (DX12) initialized");
		HookWindowProc(sd.OutputWindow);
		return true;
	}, true);

	if (!initialized)
		return;

	if (!BeginOverlayFrame())
		return;

	ImGuiScope imguiFrame;

	UINT backBufferIdx = 0;
	IDXGISwapChain3* pSwapChain3 = nullptr;
	if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&pSwapChain3))))
	{
		backBufferIdx = pSwapChain3->GetCurrentBackBufferIndex();
		pSwapChain3->Release();
	}
	if (backBufferIdx >= g_d3d12_numBackBuffers || !g_d3d12_mainRenderTargetResource[backBufferIdx])
		return;

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	// Only wait when reusing an allocator still in flight. Present can overlap GPU work.
	D3D12_WaitForGpu();
	g_d3d12_pCommandAllocator->Reset();
	g_d3d12_pCommandList->Reset(g_d3d12_pCommandAllocator, nullptr);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = g_d3d12_mainRenderTargetResource[backBufferIdx];
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	g_d3d12_pCommandList->ResourceBarrier(1, &barrier);

	g_d3d12_pCommandList->OMSetRenderTargets(1, &g_d3d12_mainRenderTargetDescriptor[backBufferIdx], FALSE, nullptr);
	g_d3d12_pCommandList->SetDescriptorHeaps(1, &g_d3d12_pSrvDescHeap);

	RenderScene();

	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_d3d12_pCommandList);

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	g_d3d12_pCommandList->ResourceBarrier(1, &barrier);
	g_d3d12_pCommandList->Close();

	g_d3d12_pCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_d3d12_pCommandList);
	const UINT64 nextFence = g_d3d12_fenceLastSignaledValue + 1;
	if (SUCCEEDED(g_d3d12_pCommandQueue->Signal(g_d3d12_pFence, nextFence)))
		g_d3d12_fenceLastSignaledValue = nextFence;
}

/**
 * @brief Handle a Direct3D12 pre-resize-buffers event by invalidating ImGui device objects and cleaning overlay resources.
 *
 * Invalidates ImGui DX12 device objects, releases any D3D12 overlay resources created for the current swap chain, and resets the internal SRV descriptor allocation counter to zero.
 *
 * @param pSwapChain Pointer to the swap chain that will be resized.
 * @param BufferCount Number of buffers in the swap chain after resize.
 * @param Width New back buffer width.
 * @param Height New back buffer height.
 * @param NewFormat New DXGI format for the back buffers.
 * @param SwapChainFlags Flags that affect swap chain behavior.
 * @param Extension Reserved extension data provided by the hook framework.
 */
void EvtHydraHookD3D12PreResizeBuffers(
	IDXGISwapChain* pSwapChain,
	UINT BufferCount,
	UINT Width,
	UINT Height,
	DXGI_FORMAT NewFormat,
	UINT SwapChainFlags,
	PHYDRAHOOK_EVT_PRE_EXTENSION Extension
)
{
	(void)pSwapChain;
	(void)BufferCount;
	(void)Width;
	(void)Height;
	(void)NewFormat;
	(void)SwapChainFlags;
	(void)Extension;

	ImGuiScope imguiDevice;
	if (!ImGui::GetCurrentContext() || !ImGui::GetIO().BackendRendererUserData)
		return;

	D3D12_WaitForGpu();
	ImGui_ImplDX12_InvalidateDeviceObjects();
	D3D12_CleanupOverlayResources();
	g_d3d12_srvDescriptorCount = 0;
}

/**
 * @brief Recreates D3D12 overlay resources and ImGui DX12 device objects after a swap-chain resize.
 *
 * Uses the provided swap chain to (re)create render-target descriptors and other overlay resources, then
 * recreates ImGui DX12 device objects so the overlay can render with the new swap-chain configuration.
 *
 * @param pSwapChain Swap chain used to recreate overlay resources.
 */
void EvtHydraHookD3D12PostResizeBuffers(
	IDXGISwapChain* pSwapChain,
	UINT BufferCount,
	UINT Width,
	UINT Height,
	DXGI_FORMAT NewFormat,
	UINT SwapChainFlags,
	PHYDRAHOOK_EVT_POST_EXTENSION Extension
)
{
	(void)BufferCount;
	(void)Width;
	(void)Height;
	(void)NewFormat;
	(void)SwapChainFlags;
	(void)Extension;

	ImGuiScope imguiDevice;
	if (!ImGui::GetCurrentContext() || !ImGui::GetIO().BackendRendererUserData)
		return;

	if (!D3D12_CreateOverlayResources(pSwapChain))
	{
		HydraHookEngineLogError("D3D12_CreateOverlayResources failed after resize");
		return;
	}
	ImGui_ImplDX12_CreateDeviceObjects();
}

#endif

#pragma endregion

#pragma region WNDPROC Hooking

static int OverlayButton(UINT msg, WPARAM wParam, bool* down)
{
	*down = false;
	switch (msg)
	{
	case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: *down = true; return 0;
	case WM_LBUTTONUP: return 0;
	case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: *down = true; return 1;
	case WM_RBUTTONUP: return 1;
	case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: *down = true; return 2;
	case WM_MBUTTONUP: return 2;
	case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
		*down = true;
		return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4;
	case WM_XBUTTONUP:
		return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4;
	default:
		return -1;
	}
}

static void ReleaseOverlayInput(HWND hWnd, uint32_t generation, bool force)
{
	if (!force && generation != g_ReleaseGeneration.load())
		return;

	ImGuiScope scope;
	if (!force && generation != g_ReleaseGeneration.load())
		return;

	if (!ImGui::GetCurrentContext() || !ImGui::GetIO().BackendPlatformUserData)
	{
		g_ForwardedButtons.store(0);
		g_OverlayOwnsCapture.store(false);
	}
	else
	{
		const bool ownedCapture = g_OverlayOwnsCapture.exchange(false);
		const uint32_t buttons = g_ForwardedButtons.exchange(0);
		// Button-up would ReleaseCapture when the mask hits zero. Clear the private mask
		// directly so a host-owned capture is left in place and the host sees no WM_CAPTURECHANGED.
		ClearImGuiWin32Buttons(hWnd, buttons, ownedCapture);

		ImGuiIO& io = ImGui::GetIO();
		io.ClearEventsQueue();
		io.ClearInputMouse();
		io.ClearInputKeys();
	}

	if (force || g_ReleaseGeneration.load() == generation)
	{
		g_ReleasePending.store(false);
		g_HoldInput.store(false);
	}
}

static LRESULT ImGuiMessageHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static std::once_flag flag;
	std::call_once(flag, []() { HydraHookEngineLogInfo("++ WndProc hook received first message"); });

	const UINT releaseMessage = OverlayReleaseMessage();
	if (releaseMessage != 0 && msg == releaseMessage)
	{
		const uint32_t generation = static_cast<uint32_t>(wParam);
		// wParam 0 is the unhook path and always runs. Hide posts start at 1.
		ReleaseOverlayInput(hWnd, generation, generation == 0);
		return 0;
	}

	// Capture moved to another window. 1.91.9 ignores WM_CAPTURECHANGED, so clear the tracked
	// buttons in the backend mask without releasing or taking capture for the host.
	if (msg == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lParam) != hWnd)
	{
		const uint32_t buttons = g_ForwardedButtons.exchange(0);
		g_OverlayOwnsCapture.store(false);
		ImGuiScope scope;
		ClearImGuiWin32Buttons(hWnd, buttons, false);
		return 0;
	}

	// Hidden, waiting for a queued release, or tearing down: do not hand new input to ImGui.
	if (g_HoldInput.load() || g_InputClosed.load() || !g_ShowOverlay.load())
		return 0;

	ImGuiScope scope;
	if (g_HoldInput.load() || g_InputClosed.load() || !g_ShowOverlay.load()
		|| !ImGui::GetCurrentContext() || !ImGui::GetIO().BackendPlatformUserData)
		return 0;

	bool down = false;
	const int button = OverlayButton(msg, wParam, &down);
	const HWND capturedBefore = button >= 0 ? ::GetCapture() : nullptr;
	const bool ownedCapture = g_OverlayOwnsCapture.load();
	// A host-owned capture must not go through the backend's button-up path: that path
	// calls ReleaseCapture when the mask hits zero and the host observes WM_CAPTURECHANGED.
	const bool keepHostCapture = button >= 0 && !down && !ownedCapture && capturedBefore == hWnd;
	const LRESULT handled = keepHostCapture
		? 0
		: ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
	if (keepHostCapture)
		ClearImGuiWin32Buttons(hWnd, 1u << button, false);

	if (button >= 0)
	{
		if (down)
		{
			g_ForwardedButtons.fetch_or(1u << button);
			if (capturedBefore == nullptr && ::GetCapture() == hWnd)
				g_OverlayOwnsCapture.store(true);
		}
		else
		{
			const uint32_t left = g_ForwardedButtons.fetch_and(~(1u << button)) & ~(1u << button);
			if (left == 0)
				g_OverlayOwnsCapture.store(false);
		}
	}

	return handled;
}

static void WindowProcHookLogError(const char* message)
{
	HydraHookEngineLogError("%s", message);
}

/**
 * @brief Hooks the window procedure of the given window to forward input to ImGui.
 *
 * Validates the handle, stores the original WNDPROC per window and forwards unhandled
 * messages with CallWindowProcW. See WindowProcHook.h for threading and teardown rules.
 *
 * @param hWnd Handle to the target window whose window procedure will be hooked.
 */
void HookWindowProc(HWND hWnd)
{
	if (OverlayReleaseMessage() == 0)
	{
		HydraHookEngineLogError("Couldn't install WNDPROC hook, overlay will not receive input");
		return;
	}

	g_InputClosed.store(false);
	g_HoldInput.store(false);
	auto& hook = WindowProcHook::Instance();
	hook.SetErrorLogger(WindowProcHookLogError);
	hook.SetMessageHandler(ImGuiMessageHandler);

	if (hook.Hook(hWnd))
	{
		g_InputWindow.store(hWnd);
		HydraHookEngineLogInfo("WNDPROC hook installed");
	}
	else
		HydraHookEngineLogError("Couldn't install WNDPROC hook, overlay will not receive input");
}

/**
 * @brief Restores hooked window procedures and waits for in-flight handlers to finish.
 *
 * @return false when called from inside a hooked message. The caller must not destroy ImGui
 *         until that handler has returned.
 */
bool UnhookWindowProc()
{
	g_ReleasePending.store(false);
	g_HoldInput.store(true);
	{
		// Block new button-downs before the release runs. Drop the lock before SendMessage
		// so the window thread can enter the handler.
		ImGuiScope scope;
		g_InputClosed.store(true);
	}
	const HWND window = g_InputWindow.exchange(nullptr);
	const UINT releaseMessage = OverlayReleaseMessage();
	if (window != nullptr && releaseMessage != 0 && ::IsWindow(window))
	{
		const DWORD owner = ::GetWindowThreadProcessId(window, nullptr);
		if (owner == ::GetCurrentThreadId())
			ReleaseOverlayInput(window, 0, true);
		else
			::SendMessageW(window, releaseMessage, 0, 0);
	}
	else
	{
		g_ForwardedButtons.store(0);
		g_OverlayOwnsCapture.store(false);
		g_HoldInput.store(false);
	}

	const bool drained = WindowProcHook::Instance().UnhookAll();
	if (drained)
		HydraHookEngineLogInfo("WNDPROC hooks removed");
	return drained;
}

#pragma endregion