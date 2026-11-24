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
#include "OverlayRelease.h"
#include "InitGuard.h"
#include "WindowProcHook.h"

// 
// STL
// 
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
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

using Microsoft::WRL::ComPtr;

#ifdef _WIN64
static constexpr UINT D3D12_NUM_BACK_BUFFERS = DXGI_MAX_SWAP_CHAIN_BUFFERS;
static constexpr UINT D3D12_NUM_FRAMES_IN_FLIGHT = 2;
static constexpr UINT D3D12_SRV_HEAP_SIZE = 64;
#endif

// All per-engine runtime state of the shared host. It lives in the engine's custom context
// memory, followed by the scene's context (see EvtHydraHookGameHooked).
struct OverlayHostContext
{
	PHYDRAHOOK_ENGINE engine = nullptr;
	OverlaySceneContext* scene = nullptr;

	// Set from the scene's OverlayFrameState::acceptInput. The window-proc hook only feeds ImGui while it is set.
	std::atomic<bool> acceptInput{ false };
	std::atomic<HWND> inputWindow{ nullptr };
	// Buttons this hook forwarded to ImGui, and whether that caused the Win32 backend to take capture.
	std::atomic<uint32_t> forwardedButtons{ 0 };
	std::atomic<bool> overlayOwnsCapture{ false };
	std::atomic<bool> releasePending{ false };
	std::atomic<int> releaseAttempts{ 0 };
	// Hide generation. A queued release carries the value from the hide that posted it.
	std::atomic<uint32_t> releaseGeneration{ 0 };
	std::atomic<uint32_t> releaseApplied{ 0 };
	// New input stays out until the release for the current generation has run.
	std::atomic<bool> holdInput{ false };
	// Set before the final unhook release so a button-down cannot land after cleanup.
	std::atomic<bool> inputClosed{ false };
	std::once_flag wndProcFirstMessage;

	// Serializes ImGui context use between the render thread and the window thread.
	// Re-entry on the thread that already holds it does not lock again: Present and a
	// SendMessage it triggers can share a thread, and Dear ImGui is already on that stack.
	std::timed_mutex imguiMutex;
	std::atomic<DWORD> imguiOwner{ 0 };

	HYDRAHOOK_D3D_VERSION gameVersion = HydraHookDirect3DVersionUnknown;

	// Per-backend render-path init guards.
	ResettableInitGuard d3d9InitGuard;
	ResettableInitGuard d3d10InitGuard;
	ResettableInitGuard d3d11InitGuard;
#ifdef _WIN64
	ResettableInitGuard d3d12InitGuard;
#endif

	ComPtr<ID3D10Device> d3d10Device;
	ComPtr<ID3D10RenderTargetView> d3d10Target;
	ComPtr<ID3D11Device> d3d11Device;
	ComPtr<ID3D11DeviceContext> d3d11Context;
	ComPtr<ID3D11RenderTargetView> d3d11Target;

#ifdef _WIN64
	struct D3D12State
	{
		ID3D12Device* pDevice = nullptr;
		ID3D12CommandQueue* pCommandQueue = nullptr;
		ID3D12CommandAllocator* pCommandAllocator = nullptr;
		ID3D12GraphicsCommandList* pCommandList = nullptr;
		ID3D12Fence* pFence = nullptr;
		HANDLE hFenceEvent = nullptr;
		UINT64 fenceLastSignaledValue = 0;
		bool fenceSignalFailed = false;
		bool overlayReleasedForResize = false;
		ID3D12DescriptorHeap* pRtvDescHeap = nullptr;
		ID3D12DescriptorHeap* pSrvDescHeap = nullptr;
		ID3D12Resource* mainRenderTargetResource[D3D12_NUM_BACK_BUFFERS] = {};
		D3D12_CPU_DESCRIPTOR_HANDLE mainRenderTargetDescriptor[D3D12_NUM_BACK_BUFFERS] = {};
		UINT rtvDescriptorSize = 0;
		UINT srvDescriptorIncrement = 0;
		UINT numBackBuffers = D3D12_NUM_BACK_BUFFERS;
		UINT srvDescriptorCount = 0;
	} d3d12;
#endif
};

static_assert(alignof(OverlayHostContext) <= alignof(std::max_align_t),
	"malloc-backed engine context cannot satisfy this alignment");

// The host has no globals. D3D11/D3D12 callbacks get the context through their extension. The
// D3D9 and D3D10 callbacks resolve the engine from the device or swap chain they receive, and
// the window-procedure handler receives the engine as hook user data. The context itself is
// looked up on every use, so it reads as null once the engine has released it.
static OverlayHostContext* HostFromEngine(PHYDRAHOOK_ENGINE engine)
{
	return engine ? static_cast<OverlayHostContext*>(HydraHookEngineGetCustomContext(engine)) : nullptr;
}

static OverlayHostContext* HostFromExtension(PHYDRAHOOK_EVT_PRE_EXTENSION Extension)
{
	return Extension ? static_cast<OverlayHostContext*>(Extension->Context) : nullptr;
}

static OverlayHostContext* HostFromExtension(PHYDRAHOOK_EVT_POST_EXTENSION Extension)
{
	return Extension ? static_cast<OverlayHostContext*>(Extension->Context) : nullptr;
}

static OverlayHostContext* HostFromD3D9Device(LPDIRECT3DDEVICE9 pDevice)
{
	PHYDRAHOOK_ENGINE engine = HydraHookEngineGetHandleFromD3D9Device(pDevice);
	if (!engine)
		engine = HydraHookEngineGetHandleFromD3D9ExDevice(reinterpret_cast<LPDIRECT3DDEVICE9EX>(pDevice));
	return HostFromEngine(engine);
}

static OverlayHostContext* HostFromD3D10SwapChain(IDXGISwapChain* pSwapChain)
{
	return HostFromEngine(HydraHookEngineGetHandleFromD3D10SwapChain(pSwapChain));
}

static constexpr size_t AlignUp(size_t value, size_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

struct ImGuiScope
{
	explicit ImGuiScope(OverlayHostContext& ctx) : ctx_(ctx)
	{
		const DWORD self = ::GetCurrentThreadId();
		if (ctx_.imguiOwner.load(std::memory_order_relaxed) == self)
		{
			acquired_ = true;
			return;
		}
		ctx_.imguiMutex.lock();
		ctx_.imguiOwner.store(self, std::memory_order_relaxed);
		locked_ = true;
		acquired_ = true;
	}

	ImGuiScope(OverlayHostContext& ctx, std::chrono::milliseconds timeout) : ctx_(ctx)
	{
		const DWORD self = ::GetCurrentThreadId();
		if (ctx_.imguiOwner.load(std::memory_order_relaxed) == self)
		{
			acquired_ = true;
			return;
		}
		if (ctx_.imguiMutex.try_lock_for(timeout))
		{
			ctx_.imguiOwner.store(self, std::memory_order_relaxed);
			locked_ = true;
			acquired_ = true;
		}
	}

	~ImGuiScope()
	{
		if (!locked_)
			return;
		ctx_.imguiOwner.store(0, std::memory_order_relaxed);
		ctx_.imguiMutex.unlock();
	}

	bool owns_lock() const { return acquired_; }

	ImGuiScope(const ImGuiScope&) = delete;
	ImGuiScope& operator=(const ImGuiScope&) = delete;

private:
	OverlayHostContext& ctx_;
	bool locked_ = false;
	bool acquired_ = false;
};

static bool UnhookWindowProc(OverlayHostContext& ctx);
static void HookWindowProc(OverlayHostContext& ctx, HWND hWnd);

#ifdef _WIN64
static void D3D12_CleanupOverlayResources(OverlayHostContext& ctx);
static void D3D12_CleanupInitResources(OverlayHostContext& ctx);
static bool D3D12_WaitForGpu(OverlayHostContext& ctx, DWORD timeoutMs = INFINITE);
#endif

// Process-wide registration of a window message. The value only caches an OS atom.
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

static void RequestOverlayRelease(OverlayHostContext& ctx)
{
	// Same lock as ReleaseOverlayInput. An older cleanup must not clear the hold
	// after this hide has published it.
	ImGuiScope scope(ctx);
	ctx.releaseAttempts.store(0);
	ctx.holdInput.store(true);
	ctx.releaseGeneration.fetch_add(1);
	ctx.releasePending.store(true);
}

// Posts the release for the generation observed at entry. A newer hide keeps its own pending bit.
static void DeliverPendingRelease(OverlayHostContext& ctx)
{
	if (!ctx.releasePending.load())
		return;

	const uint32_t generation = ctx.releaseGeneration.load();
	const HWND window = ctx.inputWindow.load();
	if (window == nullptr || !::IsWindow(window))
	{
		ImGuiScope scope(ctx);
		if (ClearPendingIfCurrent(ctx.releaseGeneration, ctx.releasePending, generation))
		{
			ctx.holdInput.store(false);
			ctx.forwardedButtons.store(0);
			ctx.overlayOwnsCapture.store(false);
		}
		return;
	}

	const UINT message = OverlayReleaseMessage();
	if (message == 0)
	{
		ImGuiScope scope(ctx);
		ClearPendingIfCurrent(ctx.releaseGeneration, ctx.releasePending, generation);
		return;
	}

	if (::PostMessageW(window, message, generation, 0))
	{
		// Same lock as RequestOverlayRelease. The generation check and the clear
		// must not straddle a newer hide, or that hide is never posted and input stays held.
		ImGuiScope scope(ctx);
		ClearPendingIfCurrent(ctx.releaseGeneration, ctx.releasePending, generation);
		return;
	}

	const int attempt = ctx.releaseAttempts.fetch_add(1) + 1;
	if (attempt <= 8)
		HydraHookEngineLogError("Couldn't post overlay input release (error %lu)", ::GetLastError());
}

// Asks the scene what to do this frame. Losing input acceptance queues the same release as hiding.
// Returns whether the scene should render.
static bool BeginOverlayFrame(OverlayHostContext& ctx)
{
	const OverlayFrameState state = UpdateOverlayState(ctx.scene);
	if (ctx.acceptInput.exchange(state.acceptInput) && !state.acceptInput)
		RequestOverlayRelease(ctx);
	DeliverPendingRelease(ctx);
	return state.render;
}

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
	cfg.EvtHydraHookGamePreExit = EvtHydraHookGamePreExit;
	cfg.CrashHandler.IsEnabled = TRUE;
)

/**
 * @brief Allocates the engine-bound context, initializes ImGui and registers Direct3D event callbacks.
 *
 * Allocates one custom-context block that holds the host state followed by the scene's state,
 * creates a Dear ImGui context, applies default styling, prepares per-API callback structures
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
	// Present, Present1 and resize hooks can announce the same renderer. A context kept alive
	// after an incomplete unhook also lands here and must not be replaced.
	if (HydraHookEngineGetCustomContext(EngineHandle) != nullptr)
		return;

	HydraHookEngineLogInfo("Loading ImGui plugin");

	constexpr size_t hostSize = AlignUp(sizeof(OverlayHostContext), alignof(std::max_align_t));
	PVOID memory = nullptr;
	if (HydraHookEngineAllocCustomContext(EngineHandle, &memory, hostSize + OverlaySceneContextSize())
		!= HYDRAHOOK_ERROR_NONE || !memory)
	{
		HydraHookEngineLogError("Failed to allocate custom context for the ImGui host");
		return;
	}

	// Placement new: both contexts have non-trivial members.
	auto* ctx = new (memory) OverlayHostContext();
	ctx->engine = EngineHandle;
	ctx->scene = reinterpret_cast<OverlaySceneContext*>(static_cast<uint8_t*>(memory) + hostSize);
	OverlaySceneConstruct(ctx->scene);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	OnOverlayContextCreated(ctx->scene, io);

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

#ifdef _WIN64 // D3D12 ImGui overlay is x64-only by choice (vcpkg.json disables the dx12 binding on x86)
	HYDRAHOOK_D3D12_EVENT_CALLBACKS d3d12;
	HYDRAHOOK_D3D12_EVENT_CALLBACKS_INIT(&d3d12);
	d3d12.EvtHydraHookD3D12PrePresent = EvtHydraHookD3D12Present;
	d3d12.EvtHydraHookD3D12PreResizeBuffers = EvtHydraHookD3D12PreResizeBuffers;
	d3d12.EvtHydraHookD3D12PostResizeBuffers = EvtHydraHookD3D12PostResizeBuffers;
#endif

	ctx->gameVersion = GameVersion;

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
 *          The engine context is only released when nothing can still reference it.
 *
 * \author  Benjamin "Nefarius" Höglinger
 * \date    16.06.2018
 */
void EvtHydraHookGameUnhooked(
	PHYDRAHOOK_ENGINE EngineHandle)
{
	auto* ctx = static_cast<OverlayHostContext*>(HydraHookEngineGetCustomContext(EngineHandle));
	if (!ctx)
		return;

	HydraHookEngineLogInfo("Unhooking ImGui overlay");

	// Restore window procedures and wait for in-flight handlers before ImGui is torn down.
	// A handler that called unhook itself, stalled past the timeout, or could not yet process
	// the capture-release message cannot be waited on; destroying the context under it is a
	// use-after-free, so only drop our own render targets when GPU completion is confirmed.
	if (!UnhookWindowProc(*ctx))
	{
		HydraHookEngineLogError("WNDPROC unhook did not drain in-flight message handlers; leaving the ImGui context alive");
		// Render targets are only dropped once no render callback can still use them.
		if (!HydraHookEngineAreCallbacksDrained(EngineHandle))
			return;
		ctx->d3d10Target.Reset();
		ctx->d3d11Target.Reset();
#ifdef _WIN64
		if (D3D12_WaitForGpu(*ctx, 1000))
			D3D12_CleanupOverlayResources(*ctx);
		else
			HydraHookEngineLogError("Skipping D3D12 overlay resource release on unhook because GPU wait did not complete");
#endif
		return;
	}

	// A render callback that outlived the engine's drain timeout may still hold the context.
	// Leave ImGui and the context allocated; the engine releases the memory when it is destroyed.
	if (!HydraHookEngineAreCallbacksDrained(EngineHandle))
	{
		HydraHookEngineLogError("Render callbacks did not drain; leaving the ImGui context alive");
		return;
	}

	{
		ImGuiScope imguiShutdown(*ctx);

		// Shutdown ImGui backend based on detected game version (order: backend first, then Win32, then context)
		if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)
		{
			switch (ctx->gameVersion)
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
				if (!D3D12_WaitForGpu(*ctx, 1000))
				{
					HydraHookEngineLogError("Skipping D3D12 ImGui shutdown, context destruction, and resource release on unhook because GPU wait did not complete");
					ctx->acceptInput.store(false);
					return;
				}
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

		ctx->d3d10Target.Reset();
		ctx->d3d10Device.Reset();
		ctx->d3d11Target.Reset();
		ctx->d3d11Context.Reset();
		ctx->d3d11Device.Reset();

#ifdef _WIN64
		// Release D3D12 overlay resources (device, queue, heaps, etc. created by overlay)
		D3D12_CleanupInitResources(*ctx);
#endif

		ctx->acceptInput.store(false);
		OnOverlayShutdown(ctx->scene);
	}

	// Window procedures are restored and drained and the render hooks are gone: nothing
	// resolves the context anymore. A rehook announces the renderer again and allocates anew.
	OverlaySceneDestroy(ctx->scene);
	ctx->~OverlayHostContext();
	HydraHookEngineFreeCustomContext(EngineHandle);
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
	OverlayHostContext* pCtx = HostFromD3D9Device(pDevice);
	if (!pCtx)
		return;
	OverlayHostContext& ctx = *pCtx;

	//
	// This section is only called once to initialize ImGui
	// 
	const bool initialized = ctx.d3d9InitGuard.EnsureInitialized([&]()
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

		HookWindowProc(ctx, params.hFocusWindow);

		return true;
	});

	if (!initialized)
		return;

	if (!BeginOverlayFrame(ctx))
		return;

	ImGuiScope imguiFrame(ctx);

	// Start the Dear ImGui frame
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	RenderScene(ctx.scene);

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
	OverlayHostContext* ctx = HostFromD3D9Device(pDevice);
	if (!ctx)
		return;
	ImGuiScope imguiDevice(*ctx);
	if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)
		ImGui_ImplDX9_InvalidateDeviceObjects();
}

void EvtHydraHookD3D9PostReset(
	LPDIRECT3DDEVICE9       pDevice,
	D3DPRESENT_PARAMETERS   *pPresentationParameters
)
{
	OverlayHostContext* ctx = HostFromD3D9Device(pDevice);
	if (!ctx)
		return;
	ImGuiScope imguiDevice(*ctx);
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
	OverlayHostContext* ctx = HostFromD3D9Device(pDevice);
	if (!ctx)
		return;
	ImGuiScope imguiDevice(*ctx);
	if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)
		ImGui_ImplDX9_InvalidateDeviceObjects();
}

void EvtHydraHookD3D9PostResetEx(
	LPDIRECT3DDEVICE9EX     pDevice,
	D3DPRESENT_PARAMETERS   *pPresentationParameters,
	D3DDISPLAYMODEEX        *pFullscreenDisplayMode
)
{
	OverlayHostContext* ctx = HostFromD3D9Device(pDevice);
	if (!ctx)
		return;
	ImGuiScope imguiDevice(*ctx);
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

	OverlayHostContext* pCtx = HostFromD3D10SwapChain(pSwapChain);
	if (!pCtx)
		return;
	OverlayHostContext& ctx = *pCtx;

	//
	// This section is only called once to initialize ImGui
	// 
	const bool initialized = ctx.d3d10InitGuard.EnsureInitialized([&]()
	{
		HydraHookEngineLogInfo("Grabbing device and context pointers");

		if (FAILED(D3D10_DEVICE_FROM_SWAPCHAIN(pSwapChain, &ctx.d3d10Device)))
		{
			HydraHookEngineLogError("Couldn't get device from swapchain");
			return false;
		}

		DXGI_SWAP_CHAIN_DESC sd;
		pSwapChain->GetDesc(&sd);

		HydraHookEngineLogInfo("Initializing ImGui");

		ImGui_ImplWin32_Init(sd.OutputWindow);
		ImGui_ImplDX10_Init(ctx.d3d10Device.Get());

		HydraHookEngineLogInfo("ImGui (DX10) initialized");

		HookWindowProc(ctx, sd.OutputWindow);

		return true;
	});

	if (!initialized)
		return;

	if (!BeginOverlayFrame(ctx))
		return;

	ImGuiScope imguiFrame(ctx);


	// Start the Dear ImGui frame
	ImGui_ImplDX10_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	RenderScene(ctx.scene);

	DrawToBackBuffer<ID3D10Resource, ID3D10DepthStencilView, ID3D10ShaderResourceView>(pSwapChain, ctx.d3d10Device.Get(),
		ctx.d3d10Device.Get(), ctx.d3d10Target, d3d10Stages, ImGui_ImplDX10_RenderDrawData);
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
	if (OverlayHostContext* ctx = HostFromD3D10SwapChain(pSwapChain))
		ctx->d3d10Target.Reset();
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

	OverlayHostContext* pCtx = HostFromExtension(Extension);
	if (!pCtx)
		return;
	OverlayHostContext& ctx = *pCtx;

	//
	// This section is only called once to initialize ImGui
	// 
	const bool initialized = ctx.d3d11InitGuard.EnsureInitialized([&]()
	{
		HydraHookEngineLogInfo("Grabbing device and context pointers");

		if (FAILED(D3D11_DEVICE_IMMEDIATE_CONTEXT_FROM_SWAPCHAIN(pSwapChain, &ctx.d3d11Device, &ctx.d3d11Context)))
		{
			HydraHookEngineLogError("Couldn't get device and context from swapchain");
			return false;
		}

		DXGI_SWAP_CHAIN_DESC sd;
		pSwapChain->GetDesc(&sd);

		HydraHookEngineLogInfo("Initializing ImGui");

		ImGui_ImplWin32_Init(sd.OutputWindow);
		ImGui_ImplDX11_Init(ctx.d3d11Device.Get(), ctx.d3d11Context.Get());

		HydraHookEngineLogInfo("ImGui (DX11) initialized");

		HookWindowProc(ctx, sd.OutputWindow);

		return true;
	});

	if (!initialized)
		return;

	if (!BeginOverlayFrame(ctx))
		return;

	ImGuiScope imguiFrame(ctx);

	// Start the Dear ImGui frame
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	RenderScene(ctx.scene);

	DrawToBackBuffer<ID3D11Resource, ID3D11DepthStencilView, ID3D11ShaderResourceView>(pSwapChain, ctx.d3d11Device.Get(),
		ctx.d3d11Context.Get(), ctx.d3d11Target, d3d11Stages, ImGui_ImplDX11_RenderDrawData);
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
	if (OverlayHostContext* ctx = HostFromExtension(Extension))
		ctx->d3d11Target.Reset();
}

#pragma endregion

#pragma region D3D12

#ifdef _WIN64

/**
 * @brief Allocates the next available shader-visible SRV descriptor from the internal DX12 descriptor heap.
 *
 * Produces CPU and GPU descriptor handles for a new SRV and advances the context's descriptor count.
 * When the heap is exhausted (count >= D3D12_SRV_HEAP_SIZE), the function
 * zeroes *out_cpu and *out_gpu and returns; callers receive zeroed handles on allocation failure.
 * The context arrives through ImGui_ImplDX12_InitInfo::UserData.
 *
 * @param info ImGui DX12 init info whose UserData points to the OverlayHostContext.
 * @param out_cpu Pointer to receive the CPU descriptor handle for the allocated SRV.
 * @param out_gpu Pointer to receive the GPU descriptor handle for the allocated SRV.
 */
static void D3D12_SrvDescriptorAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
{
	auto* ctx = static_cast<OverlayHostContext*>(info->UserData);
	if (!ctx || ctx->d3d12.srvDescriptorCount >= D3D12_SRV_HEAP_SIZE)
	{
		*out_cpu = {};
		*out_gpu = {};
		return;
	}
	auto& d = ctx->d3d12;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = d.pSrvDescHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = d.pSrvDescHeap->GetGPUDescriptorHandleForHeapStart();
	cpu.ptr += d.srvDescriptorCount * d.srvDescriptorIncrement;
	gpu.ptr += d.srvDescriptorCount * d.srvDescriptorIncrement;
	d.srvDescriptorCount++;
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

/**
 * @brief Creates RTV descriptor heap and render target views for the swap chain's back buffers.
 *
 * Initializes and stores render-target resources and descriptors used by the overlay.
 * On failure, logs an error and returns false; caller must call D3D12_CleanupInitResources.
 *
 * @return true on success, false on failure.
 */
static bool D3D12_CreateOverlayResources(OverlayHostContext& ctx, IDXGISwapChain* pSwapChain)
{
	auto& d = ctx.d3d12;
	ID3D12Resource* pBackBuffer = nullptr;
	if (FAILED(D3D12_BACKBUFFER_FROM_SWAPCHAIN(pSwapChain, &pBackBuffer, 0)))
	{
		HydraHookEngineLogError("Couldn't get back buffer from swapchain");
		return false;
	}

	DXGI_SWAP_CHAIN_DESC sd;
	pSwapChain->GetDesc(&sd);
	d.numBackBuffers = sd.BufferCount;
	if (d.numBackBuffers > D3D12_NUM_BACK_BUFFERS)
		d.numBackBuffers = D3D12_NUM_BACK_BUFFERS;

	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
	rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDesc.NumDescriptors = d.numBackBuffers;
	rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvDesc.NodeMask = 1;
	if (FAILED(d.pDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&d.pRtvDescHeap))))
	{
		pBackBuffer->Release();
		HydraHookEngineLogError("Couldn't create RTV descriptor heap");
		return false;
	}

	d.rtvDescriptorSize = d.pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = d.pRtvDescHeap->GetCPUDescriptorHandleForHeapStart();

	for (UINT i = 0; i < d.numBackBuffers; i++)
	{
		ID3D12Resource* pBuffer = nullptr;
		if (FAILED(pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBuffer))))
		{
			D3D12_CleanupOverlayResources(ctx);
			pBackBuffer->Release();
			HydraHookEngineLogError("Couldn't get swap chain buffer %u", i);
			return false;
		}
		d.pDevice->CreateRenderTargetView(pBuffer, nullptr, rtvHandle);
		d.mainRenderTargetResource[i] = pBuffer;
		d.mainRenderTargetDescriptor[i] = rtvHandle;
		rtvHandle.ptr += d.rtvDescriptorSize;
	}
	pBackBuffer->Release();
	return true;
}

/**
 * @brief Releases and clears D3D12 overlay render target resources and the RTV descriptor heap.
 *
 * Frees any back-buffer resources stored in the context (up to numBackBuffers) and releases
 * the RTV descriptor heap, setting the freed pointers to nullptr.
 */
static void D3D12_CleanupOverlayResources(OverlayHostContext& ctx)
{
	auto& d = ctx.d3d12;
	for (UINT i = 0; i < d.numBackBuffers; i++)
	{
		if (d.mainRenderTargetResource[i])
		{
			d.mainRenderTargetResource[i]->Release();
			d.mainRenderTargetResource[i] = nullptr;
		}
		d.mainRenderTargetDescriptor[i] = {};
	}
	if (d.pRtvDescHeap)
	{
		d.pRtvDescHeap->Release();
		d.pRtvDescHeap = nullptr;
	}
}

/**
 * @brief Releases all D3D12 init resources (device, queue, allocator, list, fence, heaps).
 * Call before early return from the initialization block to avoid leaks.
 */
static void D3D12_CleanupInitResources(OverlayHostContext& ctx)
{
	auto& d = ctx.d3d12;
	D3D12_CleanupOverlayResources(ctx);
	if (d.pSrvDescHeap) { d.pSrvDescHeap->Release(); d.pSrvDescHeap = nullptr; }
	if (d.hFenceEvent) { CloseHandle(d.hFenceEvent); d.hFenceEvent = nullptr; }
	if (d.pFence) { d.pFence->Release(); d.pFence = nullptr; }
	if (d.pCommandList) { d.pCommandList->Release(); d.pCommandList = nullptr; }
	if (d.pCommandAllocator) { d.pCommandAllocator->Release(); d.pCommandAllocator = nullptr; }
	if (d.pCommandQueue) { d.pCommandQueue->Release(); d.pCommandQueue = nullptr; }
	if (d.pDevice) { d.pDevice->Release(); d.pDevice = nullptr; }

	// Scalar state must not leak into the next init (new fence starts at 0, heap is recreated)
	d.fenceLastSignaledValue = 0;
	d.fenceSignalFailed = false;
	d.overlayReleasedForResize = false;
	d.srvDescriptorCount = 0;
	d.rtvDescriptorSize = 0;
	d.srvDescriptorIncrement = 0;
	d.numBackBuffers = D3D12_NUM_BACK_BUFFERS;
}

/**
 * @brief Blocks the CPU until the GPU has completed work up to the last signaled fence value.
 *
 * Waits on the fence event for the context's last signaled fence value (signaled by the caller before invoking).
 *
 * @param timeoutMs Maximum time to wait in milliseconds (defaults to INFINITE).
 * @return true if GPU completion up to the last signaled fence is confirmed (or no fence work was pending);
 *         false if the device was removed, event registration failed, or waiting did not complete.
 */
static bool D3D12_WaitForGpu(OverlayHostContext& ctx, DWORD timeoutMs)
{
	auto& d = ctx.d3d12;
	if (!d.pFence || !d.hFenceEvent)
		return !d.fenceSignalFailed;
	if (d.fenceSignalFailed)
	{
		if (!d.pCommandQueue)
			return false;
		const UINT64 retryFence = d.fenceLastSignaledValue + 1;
		const HRESULT retryHr = d.pCommandQueue->Signal(d.pFence, retryFence);
		if (FAILED(retryHr))
		{
			HydraHookEngineLogError("Retry ID3D12CommandQueue::Signal failed (0x%08X); GPU completion remains unconfirmed",
				static_cast<unsigned int>(retryHr));
			return false;
		}
		d.fenceLastSignaledValue = retryFence;
		d.fenceSignalFailed = false;
	}
	const UINT64 fence = d.fenceLastSignaledValue;
	if (fence == 0)
		return true;
	const UINT64 completed = d.pFence->GetCompletedValue();
	if (completed == UINT64_MAX)
		return false;
	if (completed >= fence)
		return true;
	const HRESULT hr = d.pFence->SetEventOnCompletion(fence, d.hFenceEvent);
	if (FAILED(hr))
	{
		const UINT64 after = d.pFence->GetCompletedValue();
		if (after != UINT64_MAX && after >= fence)
			return true;
		HydraHookEngineLogError("ID3D12Fence::SetEventOnCompletion failed (0x%08X)", static_cast<unsigned int>(hr));
		return false;
	}
	const DWORD waitResult = WaitForSingleObject(d.hFenceEvent, timeoutMs);
	const UINT64 after = d.pFence->GetCompletedValue();
	if (after != UINT64_MAX && after >= fence)
		return true;
	HydraHookEngineLogError("D3D12 GPU fence wait did not complete (result %lu)", waitResult);
	return false;
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
 * @param Extension Hook extension carrying the engine-bound host context.
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

	OverlayHostContext* pCtx = HostFromExtension(Extension);
	if (!pCtx)
		return;
	OverlayHostContext& ctx = *pCtx;
	auto& d = ctx.d3d12;

	// Retries on failure: the command queue may not be captured yet (mid-process injection)
	const bool initialized = ctx.d3d12InitGuard.EnsureInitialized([&]() -> bool
	{
		ImGuiScope imguiInit(ctx);
		HydraHookEngineLogInfo("Grabbing D3D12 device and command queue from swapchain");

		if (FAILED(D3D12_DEVICE_FROM_SWAPCHAIN(pSwapChain, &d.pDevice)))
		{
			HydraHookEngineLogError("Couldn't get D3D12 device from swapchain");
			return false;
		}

		d.pCommandQueue = HydraHookEngineGetD3D12CommandQueue(pSwapChain);
		if (!d.pCommandQueue)
		{
			HydraHookEngineLogInfo("D3D12 command queue not yet captured (mid-process injection); will retry next frame");
			D3D12_CleanupInitResources(ctx);
			return false;
		}

		if (FAILED(d.pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d.pCommandAllocator))))
		{
			HydraHookEngineLogError("Couldn't create D3D12 command allocator");
			D3D12_CleanupInitResources(ctx);
			return false;
		}

		if (FAILED(d.pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.pCommandAllocator, nullptr, IID_PPV_ARGS(&d.pCommandList))) ||
			FAILED(d.pCommandList->Close()))
		{
			HydraHookEngineLogError("Couldn't create D3D12 command list");
			D3D12_CleanupInitResources(ctx);
			return false;
		}

		if (FAILED(d.pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.pFence))))
		{
			HydraHookEngineLogError("Couldn't create D3D12 fence");
			D3D12_CleanupInitResources(ctx);
			return false;
		}

		d.hFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!d.hFenceEvent)
		{
			HydraHookEngineLogError("Couldn't create fence event");
			D3D12_CleanupInitResources(ctx);
			return false;
		}

		D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
		srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvDesc.NumDescriptors = D3D12_SRV_HEAP_SIZE;
		srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(d.pDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&d.pSrvDescHeap))))
		{
			HydraHookEngineLogError("Couldn't create D3D12 SRV descriptor heap");
			D3D12_CleanupInitResources(ctx);
			return false;
		}
		d.srvDescriptorIncrement = d.pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		if (!D3D12_CreateOverlayResources(ctx, pSwapChain))
		{
			D3D12_CleanupInitResources(ctx);
			return false;
		}

		DXGI_SWAP_CHAIN_DESC sd;
		pSwapChain->GetDesc(&sd);

		ImGui_ImplDX12_InitInfo init_info = {};
		init_info.Device = d.pDevice;
		init_info.CommandQueue = d.pCommandQueue;
		init_info.NumFramesInFlight = D3D12_NUM_FRAMES_IN_FLIGHT;
		init_info.RTVFormat = sd.BufferDesc.Format;
		init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
		init_info.UserData = &ctx;
		init_info.SrvDescriptorHeap = d.pSrvDescHeap;
		init_info.SrvDescriptorAllocFn = D3D12_SrvDescriptorAlloc;
		init_info.SrvDescriptorFreeFn = D3D12_SrvDescriptorFree;

		if (!ImGui_ImplDX12_Init(&init_info))
		{
			HydraHookEngineLogError("ImGui_ImplDX12_Init failed");
			D3D12_CleanupInitResources(ctx);
			return false;
		}

		ImGui_ImplWin32_Init(sd.OutputWindow);
		HydraHookEngineLogInfo("ImGui (DX12) initialized");
		HookWindowProc(ctx, sd.OutputWindow);
		return true;
	}, true);

	if (!initialized)
		return;

	if (!BeginOverlayFrame(ctx))
		return;

	ImGuiScope imguiFrame(ctx);

	UINT backBufferIdx = 0;
	IDXGISwapChain3* pSwapChain3 = nullptr;
	if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&pSwapChain3))))
	{
		backBufferIdx = pSwapChain3->GetCurrentBackBufferIndex();
		pSwapChain3->Release();
	}
	if (backBufferIdx >= d.numBackBuffers || !d.mainRenderTargetResource[backBufferIdx])
		return;

	// Only wait when reusing an allocator still in flight. Present can overlap GPU work.
	if (!D3D12_WaitForGpu(ctx))
		return;

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	if (FAILED(d.pCommandAllocator->Reset()) ||
		FAILED(d.pCommandList->Reset(d.pCommandAllocator, nullptr)))
	{
		ImGui::EndFrame();
		return;
	}

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = d.mainRenderTargetResource[backBufferIdx];
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	d.pCommandList->ResourceBarrier(1, &barrier);

	d.pCommandList->OMSetRenderTargets(1, &d.mainRenderTargetDescriptor[backBufferIdx], FALSE, nullptr);
	d.pCommandList->SetDescriptorHeaps(1, &d.pSrvDescHeap);

	RenderScene(ctx.scene);

	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), d.pCommandList);

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	d.pCommandList->ResourceBarrier(1, &barrier);
	d.pCommandList->Close();

	d.pCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&d.pCommandList);
	const UINT64 nextFence = d.fenceLastSignaledValue + 1;
	const HRESULT signalHr = d.pCommandQueue->Signal(d.pFence, nextFence);
	if (SUCCEEDED(signalHr))
	{
		d.fenceLastSignaledValue = nextFence;
		d.fenceSignalFailed = false;
	}
	else
	{
		d.fenceSignalFailed = true;
		HydraHookEngineLogError("ID3D12CommandQueue::Signal failed (0x%08X); marking GPU completion unconfirmed",
			static_cast<unsigned int>(signalHr));
	}
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
 * @param Extension Hook extension carrying the engine-bound host context.
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

	OverlayHostContext* pCtx = HostFromExtension(Extension);
	if (!pCtx)
		return;
	OverlayHostContext& ctx = *pCtx;

	ImGuiScope imguiDevice(ctx);
	ctx.d3d12.overlayReleasedForResize = false;
	if (!ImGui::GetCurrentContext() || !ImGui::GetIO().BackendRendererUserData)
		return;

	if (!D3D12_WaitForGpu(ctx))
	{
		HydraHookEngineLogError("Skipping D3D12 overlay resize cleanup because GPU wait did not complete");
		return;
	}
	ImGui_ImplDX12_InvalidateDeviceObjects();
	D3D12_CleanupOverlayResources(ctx);
	ctx.d3d12.srvDescriptorCount = 0;
	ctx.d3d12.overlayReleasedForResize = true;
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

	OverlayHostContext* pCtx = HostFromExtension(Extension);
	if (!pCtx)
		return;
	OverlayHostContext& ctx = *pCtx;

	ImGuiScope imguiDevice(ctx);
	if (!ctx.d3d12.overlayReleasedForResize)
		return;
	ctx.d3d12.overlayReleasedForResize = false;

	if (!ImGui::GetCurrentContext() || !ImGui::GetIO().BackendRendererUserData)
		return;

	if (!D3D12_CreateOverlayResources(ctx, pSwapChain))
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

static void ReleaseOverlayInput(OverlayHostContext& ctx, HWND hWnd, uint32_t generation, bool force)
{
	if (!force && generation != ctx.releaseGeneration.load())
		return;

	ImGuiScope scope(ctx);
	// A duplicate post of this hide must not clear input admitted after the first release.
	if (!ClaimOverlayRelease(ctx.releaseGeneration, ctx.releaseApplied, generation, force))
		return;

	if (!ImGui::GetCurrentContext() || !ImGui::GetIO().BackendPlatformUserData)
	{
		ctx.forwardedButtons.store(0);
		ctx.overlayOwnsCapture.store(false);
	}
	else
	{
		const bool ownedCapture = ctx.overlayOwnsCapture.exchange(false);
		const uint32_t buttons = ctx.forwardedButtons.exchange(0);
		// Button-up would ReleaseCapture when the mask hits zero. Clear the private mask
		// directly so a host-owned capture is left in place and the host sees no WM_CAPTURECHANGED.
		ClearImGuiWin32Buttons(hWnd, buttons, ownedCapture);

		ImGuiIO& io = ImGui::GetIO();
		io.ClearEventsQueue();
		io.ClearInputMouse();
		io.ClearInputKeys();
	}

	if (force || ctx.releaseGeneration.load() == generation)
	{
		ctx.releasePending.store(false);
		ctx.holdInput.store(false);
	}
}

// The hook passes the engine handle registered in HookWindowProc as user data.
static LRESULT ImGuiMessageHandler(void* userData, HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	OverlayHostContext* pCtx = HostFromEngine(static_cast<PHYDRAHOOK_ENGINE>(userData));
	if (!pCtx)
		return 0;
	OverlayHostContext& ctx = *pCtx;

	std::call_once(ctx.wndProcFirstMessage, []() { HydraHookEngineLogInfo("++ WndProc hook received first message"); });

	const UINT releaseMessage = OverlayReleaseMessage();
	if (releaseMessage != 0 && msg == releaseMessage)
	{
		const uint32_t generation = static_cast<uint32_t>(wParam);
		// wParam 0 is the unhook path and always runs. Hide posts start at 1.
		ReleaseOverlayInput(ctx, hWnd, generation, generation == 0);
		return 0;
	}

	// Capture moved to another window. 1.91.9 ignores WM_CAPTURECHANGED, so clear the tracked
	// buttons in the backend mask without releasing or taking capture for the host.
	if (msg == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lParam) != hWnd)
	{
		const uint32_t buttons = ctx.forwardedButtons.exchange(0);
		ctx.overlayOwnsCapture.store(false);
		ImGuiScope scope(ctx);
		ClearImGuiWin32Buttons(hWnd, buttons, false);
		return 0;
	}

	// If unhook closed input while capture or forwarded buttons were still held (for example,
	// SendMessageTimeoutW timed out and PostMessageW also failed), release directly on the
	// window-owning thread as soon as any message is pumped, then forward the message normally.
	if (ShouldFallbackReleaseOnClosedInput(ctx.inputClosed, ctx.overlayOwnsCapture, ctx.forwardedButtons))
	{
		ReleaseOverlayInput(ctx, hWnd, 0, true);
		return 0;
	}

	// Hidden, waiting for a queued release, or tearing down: do not hand new input to ImGui.
	if (ctx.holdInput.load() || ctx.inputClosed.load() || !ctx.acceptInput.load())
		return 0;

	ImGuiScope scope(ctx);
	if (ctx.holdInput.load() || ctx.inputClosed.load() || !ctx.acceptInput.load()
		|| !ImGui::GetCurrentContext() || !ImGui::GetIO().BackendPlatformUserData)
		return 0;

	bool down = false;
	const int button = OverlayButton(msg, wParam, &down);
	const HWND capturedBefore = button >= 0 ? ::GetCapture() : nullptr;
	const bool ownedCapture = ctx.overlayOwnsCapture.load();
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
			ctx.forwardedButtons.fetch_or(1u << button);
			if (capturedBefore == nullptr && ::GetCapture() == hWnd)
				ctx.overlayOwnsCapture.store(true);
		}
		else
		{
			const uint32_t left = ctx.forwardedButtons.fetch_and(~(1u << button)) & ~(1u << button);
			if (left == 0)
				ctx.overlayOwnsCapture.store(false);
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
 * @param ctx Host context that records the input window and resets the input state.
 * @param hWnd Handle to the target window whose window procedure will be hooked.
 */
static void HookWindowProc(OverlayHostContext& ctx, HWND hWnd)
{
	if (OverlayReleaseMessage() == 0)
	{
		HydraHookEngineLogError("Couldn't install WNDPROC hook, overlay will not receive input");
		return;
	}

	ctx.inputClosed.store(false);
	ctx.holdInput.store(false);
	auto& hook = WindowProcHook::Instance();
	hook.SetErrorLogger(WindowProcHookLogError);
	hook.SetMessageHandler(ImGuiMessageHandler, ctx.engine);

	if (hook.Hook(hWnd))
	{
		ctx.inputWindow.store(hWnd);
		HydraHookEngineLogInfo("WNDPROC hook installed");
	}
	else
		HydraHookEngineLogError("Couldn't install WNDPROC hook, overlay will not receive input");
}

void EvtHydraHookGamePreExit(
	PHYDRAHOOK_ENGINE EngineHandle)
{
	auto* pCtx = static_cast<OverlayHostContext*>(HydraHookEngineGetCustomContext(EngineHandle));
	if (!pCtx)
		return;
	OverlayHostContext& ctx = *pCtx;

	// Runs on the thread initiating shutdown (for PostQuitMessage, usually the window thread)
	// before PerformShutdownCleanup blocks waiting on the worker thread. Close overlay input and
	// release any capture here so UnhookWindowProc does not need a cross-thread SendMessage.
	ctx.releasePending.store(false);
	ctx.holdInput.store(true);
	ctx.inputClosed.store(true);

	// Bound mutex acquisition so a stalled render thread holding the ImGui lock cannot hang exit.
	ImGuiScope scope(ctx, std::chrono::milliseconds(50));
	if (!scope.owns_lock())
	{
		ctx.holdInput.store(false);
		HydraHookEngineLogWarning("Pre-exit could not acquire ImGui lock within 50 ms; deferring input cleanup");
		return;
	}

	const bool needsWindowRelease = ctx.overlayOwnsCapture.load() || ctx.forwardedButtons.load() != 0;
	if (!needsWindowRelease)
	{
		ctx.holdInput.store(false);
		return;
	}

	const HWND window = ctx.inputWindow.load();
	if (window != nullptr && ::IsWindow(window)
		&& ::GetWindowThreadProcessId(window, nullptr) == ::GetCurrentThreadId())
	{
		// Holding ImGuiScope here guarantees ReleaseOverlayInput re-enters without blocking.
		ReleaseOverlayInput(ctx, window, 0, true);
	}
}

/**
 * @brief Restores hooked window procedures and waits for in-flight handlers to finish.
 *
 * @return false when called from inside a hooked message handler, when capture release could
 *         not be delivered synchronously, or when draining timed out. The caller must not
 *         destroy ImGui or the host context while a handler or deferred release may still run.
 */
static bool UnhookWindowProc(OverlayHostContext& ctx)
{
	ctx.releasePending.store(false);
	ctx.holdInput.store(true);
	bool needsWindowRelease = false;
	{
		// Block new button-downs before checking whether the window thread still needs to release
		// capture or held buttons. Drop the lock before messaging so the window thread can enter.
		ImGuiScope scope(ctx);
		ctx.inputClosed.store(true);
		needsWindowRelease = ctx.overlayOwnsCapture.load() || ctx.forwardedButtons.load() != 0;
	}
	const HWND window = ctx.inputWindow.load();
	const UINT releaseMessage = OverlayReleaseMessage();
	bool releaseDelivered = true;
	if (needsWindowRelease && window != nullptr && releaseMessage != 0 && ::IsWindow(window))
	{
		const DWORD owner = ::GetWindowThreadProcessId(window, nullptr);
		if (owner == ::GetCurrentThreadId())
		{
			ReleaseOverlayInput(ctx, window, 0, true);
		}
		else
		{
			DWORD_PTR sendResult = 0;
			if (!::SendMessageTimeoutW(window, releaseMessage, 0, 0,
				SMTO_ABORTIFHUNG | SMTO_NORMAL, 250, &sendResult))
			{
				releaseDelivered = false;
				HydraHookEngineLogWarning("Timed out releasing overlay input on window thread (error %lu); deferring via PostMessageW", ::GetLastError());
				// Retain the release obligation and keep the WNDPROC hook attached so the
				// window-owning thread releases capture on the posted message (or on the next
				// pumped message if PostMessageW fails).
				if (!::PostMessageW(window, releaseMessage, 0, 0))
				{
					HydraHookEngineLogWarning("PostMessageW for deferred overlay input release failed (error %lu); release will run on the next window message", ::GetLastError());
				}
			}
		}
	}

	if (!releaseDelivered)
	{
		ctx.holdInput.store(false);
		return false;
	}

	ctx.inputWindow.store(nullptr);
	ctx.forwardedButtons.store(0);
	ctx.overlayOwnsCapture.store(false);
	ctx.holdInput.store(false);

	const bool drained = WindowProcHook::Instance().UnhookAll();
	if (drained)
		HydraHookEngineLogInfo("WNDPROC hooks removed");
	return drained;
}

#pragma endregion
