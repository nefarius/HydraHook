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

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "Capture.h"
#include "Overlay.h"
#include "Perception.h"

#include <HydraHook/Engine/HydraHookDirect3D11.h>
#include <HydraHook/Engine/HydraHookDirect3D12.h>
#include <HydraHook/Engine/HydraHookCore.h>

#include <opencv2/core.hpp>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <dxgi1_4.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <new>
#include <thread>

#ifdef _WIN64
#include <imgui_impl_dx12.h>
#endif

static constexpr UINT CAPTURE_NUM_BUFFERS = 2;
static constexpr UINT D3D12_NUM_BACK_BUFFERS = DXGI_MAX_SWAP_CHAIN_BUFFERS;
#ifdef _WIN64
static constexpr UINT D3D12_SRV_HEAP_SIZE = 64;
#endif

// All per-engine runtime state of this sample. Lives in the engine's custom context memory:
// allocated and constructed in Capture_SetupCallbacks, destroyed in Capture_PostUnhook.
struct OpenCVContext
{
	struct D3D11State
	{
		ID3D11Texture2D* staging[CAPTURE_NUM_BUFFERS] = {};
		ID3D11Query* query[CAPTURE_NUM_BUFFERS] = {};
		UINT captureWidth = 0;
		UINT captureHeight = 0;
		UINT frameCounter = 0;
		ID3D11RenderTargetView* mainRTV = nullptr;
		bool imguiInitialized = false;
	};

	struct D3D12State
	{
		bool initialized = false;
		bool skipResizeRebuild = false;
		ID3D12Device* pDevice = nullptr;
		ID3D12CommandQueue* pCommandQueue = nullptr;
		ID3D12CommandAllocator* pCommandAllocator = nullptr;
		ID3D12GraphicsCommandList* pCommandList = nullptr;
		ID3D12Fence* pFence = nullptr;
		HANDLE hFenceEvent = nullptr;
		UINT64 fenceLastSignaledValue = 0;
		ID3D12DescriptorHeap* pRtvDescHeap = nullptr;
		ID3D12Resource* mainRenderTargetResource[D3D12_NUM_BACK_BUFFERS] = {};
		D3D12_CPU_DESCRIPTOR_HANDLE mainRenderTargetDescriptor[D3D12_NUM_BACK_BUFFERS] = {};
		UINT rtvDescriptorSize = 0;
		UINT numBackBuffers = D3D12_NUM_BACK_BUFFERS;
		ID3D12Resource* readback[CAPTURE_NUM_BUFFERS] = {};
		UINT captureWidth = 0;
		UINT captureHeight = 0;
		UINT captureRowPitch = 0;
		UINT frameCounter = 0;
		UINT64 fenceValueForReadback[CAPTURE_NUM_BUFFERS] = {};
		bool imguiInitialized = false;
		ID3D12DescriptorHeap* pSrvDescHeap = nullptr;
		UINT srvDescriptorIncrement = 0;
		UINT srvDescriptorCount = 0;
	};

	// Frame hand-off from the render thread to the worker.
	struct Pending
	{
		std::condition_variable cv;
		std::mutex mutex;
		int api = 0;
		cv::Mat d3d11Frame;
		UINT width = 0;
		UINT height = 0;
		UINT64 d3d12FenceValue = 0;
		ID3D12Resource* d3d12Readback = nullptr;
		UINT d3d12RowPitch = 0;
	};

	std::mutex resultsMutex;
	PerceptionResults results;
	PerceptionState perception;

	std::atomic<bool> workerRunning{ false };
	std::atomic<bool> shutdownDone{ false };
	std::thread workerThread;

	// In-flight Present callbacks hold this across ImGui use.
	std::mutex renderMutex;

	std::atomic<bool> showOverlay{ true };
	KeyToggle overlayToggle;

	D3D11State d3d11;
	D3D12State d3d12;
	Pending pending;
};

static_assert(alignof(OpenCVContext) <= alignof(std::max_align_t),
	"malloc-backed engine context cannot satisfy this alignment");

struct RenderGate
{
	std::unique_lock<std::mutex> lock;
	bool allow;

	explicit RenderGate(OpenCVContext& ctx)
		: lock(ctx.renderMutex)
		, allow(!ctx.shutdownDone.load(std::memory_order_relaxed))
	{
	}
};

// The engine hands out a zeroed block. `state` sits in front of the context so that
// Capture_SetupCallbacks and Capture_PreUnhook can agree on who wins before OpenCVContext is
// constructed. Only `ctx` is ever constructed or destroyed; `state` is a zero-initialized atomic.
enum ContextState : int
{
	ContextUninitialized = 0, // Allocated by the engine, nothing constructed.
	ContextInitializing = 1,  // Setup is constructing the context and starting the worker.
	ContextReady = 2,         // ctx is constructed (it may have been stopped, see shutdownDone).
	ContextAborted = 3        // Unhook began before setup; nothing was constructed.
};

struct ContextBlock
{
	std::atomic<int> state;
	OpenCVContext ctx;
};

static_assert(alignof(ContextBlock) <= alignof(std::max_align_t),
	"malloc-backed engine context cannot satisfy this alignment");

static ContextBlock* BlockFromEngine(PHYDRAHOOK_ENGINE Engine)
{
	return static_cast<ContextBlock*>(HydraHookEngineGetCustomContext(Engine));
}

// The block is published before the context is constructed; callbacks must not touch it until then.
static OpenCVContext* ReadyContext(void* raw)
{
	if (!raw)
		return nullptr;
	auto* block = static_cast<ContextBlock*>(raw);
	return block->state.load(std::memory_order_acquire) == ContextReady ? &block->ctx : nullptr;
}

static OpenCVContext* ContextFromExtension(PHYDRAHOOK_EVT_PRE_EXTENSION Extension)
{
	return ReadyContext(Extension ? Extension->Context : nullptr);
}

static OpenCVContext* ContextFromExtension(PHYDRAHOOK_EVT_POST_EXTENSION Extension)
{
	return ReadyContext(Extension ? Extension->Context : nullptr);
}
static bool D3D11_CreateCaptureResources(OpenCVContext& ctx, ID3D11Device* pDevice, UINT width, UINT height);
static void D3D11_ReleaseCaptureResources(OpenCVContext& ctx);
static void D3D12_CleanupOverlayResources(OpenCVContext& ctx);
static void D3D12_CleanupInitResources(OpenCVContext& ctx);
static bool D3D12_WaitForGpu(OpenCVContext& ctx, DWORD timeoutMs);
static bool D3D12_CreateOverlayResources(OpenCVContext& ctx, IDXGISwapChain* pSwapChain);
static bool D3D12_CreateCaptureResources(OpenCVContext& ctx, UINT width, UINT height, DXGI_FORMAT format);
static void D3D12_ReleaseCaptureResources(OpenCVContext& ctx);

static void EvtHydraHookD3D11PrePresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, PHYDRAHOOK_EVT_PRE_EXTENSION Extension);
static void EvtHydraHookD3D11PreResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, PHYDRAHOOK_EVT_PRE_EXTENSION Extension);
static void EvtHydraHookD3D11PostResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, PHYDRAHOOK_EVT_POST_EXTENSION Extension);
static void EvtHydraHookD3D12PrePresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, PHYDRAHOOK_EVT_PRE_EXTENSION Extension);
static void EvtHydraHookD3D12PreResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, PHYDRAHOOK_EVT_PRE_EXTENSION Extension);
static void EvtHydraHookD3D12PostResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, PHYDRAHOOK_EVT_POST_EXTENSION Extension);

static void Capture_GetResults(OpenCVContext& ctx, PerceptionResults& out)
{
	std::lock_guard<std::mutex> lock(ctx.resultsMutex);
	out = ctx.results;
}

static void WorkerThreadProc(OpenCVContext* ctx)
{
	auto& pending = ctx->pending;
#ifdef _WIN64
	auto& d3d12 = ctx->d3d12;
#endif

	while (ctx->workerRunning)
	{
		int api = 0;
		UINT64 d3d12FenceVal = 0;
		ID3D12Resource* pD3D12Readback = nullptr;
		UINT width = 0, height = 0;
		UINT rowPitch = 0;

		{
			std::unique_lock<std::mutex> lock(pending.mutex);
			pending.cv.wait(lock, [ctx, &pending] { return !ctx->workerRunning || pending.api != 0; });
			if (!ctx->workerRunning)
				break;

			api = pending.api;
			width = pending.width;
			height = pending.height;
			rowPitch = width * 4;
			if (api == 12)
				rowPitch = pending.d3d12RowPitch;
			pending.api = 0;

			if (api == 12)
			{
				d3d12FenceVal = pending.d3d12FenceValue;
				pD3D12Readback = pending.d3d12Readback;
				// Ownership of the AddRef taken by the render thread moves to this iteration.
				pending.d3d12Readback = nullptr;
			}
		}

		if (width == 0 || height == 0)
		{
			if (pD3D12Readback)
				pD3D12Readback->Release();
			continue;
		}

		cv::Mat frame;

		if (api == 11)
		{
			{
				std::lock_guard<std::mutex> lock(pending.mutex);
				if (!pending.d3d11Frame.empty())
				{
					frame = pending.d3d11Frame;
					pending.d3d11Frame.release();
				}
			}
		}
#ifdef _WIN64
		else if (api == 12 && pD3D12Readback && d3d12.pFence && d3d12.hFenceEvent)
		{
			d3d12.pFence->SetEventOnCompletion(d3d12FenceVal, d3d12.hFenceEvent);
			while (ctx->workerRunning && WaitForSingleObject(d3d12.hFenceEvent, 200) == WAIT_TIMEOUT)
				;
			if (!ctx->workerRunning)
			{
				pD3D12Readback->Release();
				continue;
			}

			const UINT rp = (rowPitch != 0) ? rowPitch : (width * 4);
			const SIZE_T readSize = (SIZE_T)height * (SIZE_T)rp;
			D3D12_RANGE readRange = { 0, readSize };
			void* pData = nullptr;
			if (SUCCEEDED(pD3D12Readback->Map(0, &readRange, &pData)))
			{
				frame.create((int)height, (int)width, CV_8UC3);
				for (UINT y = 0; y < height; y++)
				{
					const uint8_t* src = (const uint8_t*)pData + y * rp;
					uint8_t* dst = frame.ptr((int)y);
					for (UINT x = 0; x < width; x++)
					{
						dst[x * 3 + 0] = src[x * 4 + 2];
						dst[x * 3 + 1] = src[x * 4 + 1];
						dst[x * 3 + 2] = src[x * 4 + 0];
					}
				}
				pD3D12Readback->Unmap(0, nullptr);
			}
			pD3D12Readback->Release();
		}
#endif
		else if (pD3D12Readback)
		{
			pD3D12Readback->Release();
		}
		if (!frame.empty())
		{
			PerceptionResults out;
			RunPerceptionPipeline(ctx->perception, frame, out);
			{
				std::lock_guard<std::mutex> lock(ctx->resultsMutex);
				ctx->results = out;
			}
		}
	}
}

static bool Capture_TryFinalize(PHYDRAHOOK_ENGINE EngineHandle);
static bool Capture_Revive(ContextBlock& block);

// Claims an unconstructed block (fresh or aborted by pre-unhook), constructs the context in
// place and starts the worker. Returns false when pre-unhook got there first or setup is
// already running.
static bool Capture_StartContext(ContextBlock& block)
{
	int expected = ContextUninitialized;
	if (!block.state.compare_exchange_strong(expected, ContextInitializing, std::memory_order_acq_rel))
	{
		expected = ContextAborted;
		if (!block.state.compare_exchange_strong(expected, ContextInitializing, std::memory_order_acq_rel))
			return false;
	}

	// Placement new: the context has non-trivial members.
	new (&block.ctx) OpenCVContext();
	block.ctx.workerRunning = true;
	block.ctx.workerThread = std::thread(WorkerThreadProc, &block.ctx);
	block.state.store(ContextReady, std::memory_order_release);
	return true;
}

void Capture_SetupCallbacks(PHYDRAHOOK_ENGINE EngineHandle, HYDRAHOOK_D3D_VERSION GameVersion)
{
	HydraHookEngineLogInfo("HydraHook-OpenCV: Loading");

	if (GameVersion != HydraHookDirect3DVersion11 && GameVersion != HydraHookDirect3DVersion12)
	{
		HydraHookEngineLogInfo("HydraHook-OpenCV: Unsupported D3D version, no callbacks registered");
		return;
	}

	// Announcements for different renderers can overlap on different render threads. The lock
	// only serializes this function; it holds no state.
	static std::mutex setupMutex;
	std::lock_guard<std::mutex> setupLock(setupMutex);

	// Each renderer is announced separately and may be announced more than once, so a live
	// context is reused. A stopped one (earlier unhook) is replaced, but only once nothing can
	// still reference it.
	ContextBlock* block = BlockFromEngine(EngineHandle);
	if (!block)
	{
		PVOID memory = nullptr;
		if (HydraHookEngineAllocCustomContext(EngineHandle, &memory, sizeof(ContextBlock)) != HYDRAHOOK_ERROR_NONE || !memory)
		{
			HydraHookEngineLogError("HydraHook-OpenCV: Failed to allocate custom context");
			return;
		}
		block = static_cast<ContextBlock*>(memory);
	}

	if (block->state.load(std::memory_order_acquire) != ContextReady)
	{
		// Fresh or aborted block (pre-unhook can win the race against setup). Nothing is
		// constructed in it, and it is never freed here: another thread may still hold the pointer.
		if (!Capture_StartContext(*block))
		{
			HydraHookEngineLogWarning("HydraHook-OpenCV: unhook started during setup; capture stays inactive");
			return;
		}
	}
	else if (block->ctx.shutdownDone.load())
	{
		// Retained after an unconfirmed unhook. Callbacks are live again after a rehook and may
		// already hold this context, so it is revived in place and never destroyed here.
		if (!Capture_Revive(*block))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: previous context could not be released yet; capture stays inactive");
			return;
		}
	}

	switch (GameVersion)
	{
	case HydraHookDirect3DVersion11:
	{
		HYDRAHOOK_D3D11_EVENT_CALLBACKS d3d11;
		HYDRAHOOK_D3D11_EVENT_CALLBACKS_INIT(&d3d11);
		d3d11.EvtHydraHookD3D11PrePresent = EvtHydraHookD3D11PrePresent;
		d3d11.EvtHydraHookD3D11PreResizeBuffers = EvtHydraHookD3D11PreResizeBuffers;
		d3d11.EvtHydraHookD3D11PostResizeBuffers = EvtHydraHookD3D11PostResizeBuffers;
		HydraHookEngineSetD3D11EventCallbacks(EngineHandle, &d3d11);
		break;
	}
	case HydraHookDirect3DVersion12:
	{
		HYDRAHOOK_D3D12_EVENT_CALLBACKS d3d12;
		HYDRAHOOK_D3D12_EVENT_CALLBACKS_INIT(&d3d12);
		d3d12.EvtHydraHookD3D12PrePresent = EvtHydraHookD3D12PrePresent;
		d3d12.EvtHydraHookD3D12PreResizeBuffers = EvtHydraHookD3D12PreResizeBuffers;
		d3d12.EvtHydraHookD3D12PostResizeBuffers = EvtHydraHookD3D12PostResizeBuffers;
		HydraHookEngineSetD3D12EventCallbacks(EngineHandle, &d3d12);
		break;
	}
	default:
		break;
	}
}

// Stops the worker and makes later render callbacks skip their work. The context stays
// allocated: Present callbacks that were already running may still reference it.
void Capture_PreUnhook(PHYDRAHOOK_ENGINE EngineHandle)
{
	ContextBlock* block = BlockFromEngine(EngineHandle);
	if (!block)
		return;

	// Setup may still be between allocation and construction. Either mark the block aborted so
	// setup never starts a worker, or wait for setup to finish so there is a worker to stop.
	int state = block->state.load(std::memory_order_acquire);
	if (state == ContextUninitialized
		&& block->state.compare_exchange_strong(state, ContextAborted, std::memory_order_acq_rel))
		return;
	while (block->state.load(std::memory_order_acquire) == ContextInitializing)
		std::this_thread::yield();
	if (block->state.load(std::memory_order_acquire) != ContextReady)
		return;

	OpenCVContext* ctx = &block->ctx;
	if (ctx->shutdownDone.exchange(true))
		return;

	ctx->workerRunning = false;
	{
		std::lock_guard<std::mutex> lock(ctx->pending.mutex);
		ctx->pending.api = 0;
		ctx->pending.d3d11Frame.release();
		if (ctx->pending.d3d12Readback)
		{
			ctx->pending.d3d12Readback->Release();
			ctx->pending.d3d12Readback = nullptr;
		}
	}
	ctx->pending.cv.notify_all();
	if (ctx->workerThread.joinable())
		ctx->workerThread.join();
}

// Shuts down the window hook, ImGui and every GPU resource of a stopped context. The caller
// holds renderMutex. Returns false, leaving everything allocated, when the window hook has not
// drained, the GPU is not idle or D3D12 resources remain.
static bool Capture_ReleaseResourcesLocked(OpenCVContext& ctx)
{
	const bool windowDrained = Overlay_UnhookWindowProc();
	const bool gpuIdle = D3D12_WaitForGpu(ctx, 2000);
	if (!windowDrained || !gpuIdle)
	{
		HydraHookEngineLogError(
			"HydraHook-OpenCV: leaving ImGui and GPU resources allocated (window drain %d, gpu idle %d)",
			windowDrained ? 1 : 0,
			gpuIdle ? 1 : 0);
		return false;
	}

	if (ctx.d3d11.imguiInitialized)
	{
		ImGui_ImplDX11_Shutdown();
		ctx.d3d11.imguiInitialized = false;
	}
#ifdef _WIN64
	if (ctx.d3d12.imguiInitialized)
	{
		ImGui_ImplDX12_Shutdown();
		ctx.d3d12.imguiInitialized = false;
	}
#endif
	if (ImGui::GetCurrentContext())
	{
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
	}
	ctx.d3d12.initialized = false;
	if (ctx.d3d11.mainRTV)
	{
		ctx.d3d11.mainRTV->Release();
		ctx.d3d11.mainRTV = nullptr;
	}
	D3D11_ReleaseCaptureResources(ctx);
	D3D12_CleanupInitResources(ctx);

	if (ctx.d3d12.pDevice)
	{
		HydraHookEngineLogError("HydraHook-OpenCV: D3D12 resources remain allocated; keeping context alive");
		return false;
	}
	return true;
}

// Brings a stopped, retained context back to life without destroying it: render callbacks that
// obtained the pointer after a rehook only ever wait on renderMutex and then see a clean context.
static bool Capture_Revive(ContextBlock& block)
{
	OpenCVContext& ctx = block.ctx;
	std::lock_guard<std::mutex> lock(ctx.renderMutex);
	if (!ctx.shutdownDone.load())
		return true;
	if (!Capture_ReleaseResourcesLocked(ctx))
		return false;

	ctx.workerRunning = true;
	ctx.workerThread = std::thread(WorkerThreadProc, &ctx);
	ctx.shutdownDone.store(false);
	return true;
}
// Stops the worker, then frees ImGui, GPU resources and the context. That needs all of: the
// engine's callback drain completed, the window hook drained and the GPU idle. Otherwise
// everything stays allocated (a later setup retries, and the engine frees the memory when it
// is destroyed). Returns true when no context remains.
static bool Capture_TryFinalize(PHYDRAHOOK_ENGINE EngineHandle)
{
	ContextBlock* block = BlockFromEngine(EngineHandle);
	if (!block)
		return true;

	Capture_PreUnhook(EngineHandle); // No-op when it already ran.

	const int state = block->state.load(std::memory_order_acquire);
	if (state == ContextInitializing)
		return false;
	if (state != ContextReady)
	{
		// Nothing was constructed. Setup may still be running on another thread with this
		// pointer, so the block stays allocated; the engine frees it on destroy.
		return true;
	}

	if (!HydraHookEngineAreCallbacksDrained(EngineHandle))
	{
		HydraHookEngineLogError("HydraHook-OpenCV: render callbacks did not drain; keeping context alive");
		return false;
	}

	OpenCVContext* ctx = &block->ctx;
	{
		std::lock_guard<std::mutex> lock(ctx->renderMutex);
		if (!Capture_ReleaseResourcesLocked(*ctx))
			return false;
	}

	ctx->~OpenCVContext();
	HydraHookEngineFreeCustomContext(EngineHandle);
	return true;
}

// Runs after the hooks are removed and the callbacks drained (or the drain timed out).
void Capture_PostUnhook(PHYDRAHOOK_ENGINE EngineHandle)
{
	Capture_TryFinalize(EngineHandle);
}
#pragma region D3D11

static bool D3D11_CreateCaptureResources(OpenCVContext& ctx, ID3D11Device* pDevice, UINT width, UINT height)
{
	auto& d = ctx.d3d11;

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = width;
	td.Height = height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.SampleDesc.Quality = 0;
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	D3D11_QUERY_DESC qd = {};
	qd.Query = D3D11_QUERY_EVENT;
	qd.MiscFlags = 0;

	for (UINT i = 0; i < CAPTURE_NUM_BUFFERS; i++)
	{
		if (d.staging[i])
		{
			d.staging[i]->Release();
			d.staging[i] = nullptr;
		}
		if (d.query[i])
		{
			d.query[i]->Release();
			d.query[i] = nullptr;
		}
		HRESULT hrTex = pDevice->CreateTexture2D(&td, nullptr, &d.staging[i]);
		HRESULT hrQuery = pDevice->CreateQuery(&qd, &d.query[i]);
		if (FAILED(hrTex) || !d.staging[i] || FAILED(hrQuery) || !d.query[i])
		{
			HydraHookEngineLogError("HydraHook-OpenCV: D3D11 CreateTexture2D/CreateQuery failed (hrTex=0x%08X, hrQuery=0x%08X)", (unsigned)hrTex, (unsigned)hrQuery);
			D3D11_ReleaseCaptureResources(ctx);
			return false;
		}
	}
	d.captureWidth = width;
	d.captureHeight = height;
	return true;
}

static void D3D11_ReleaseCaptureResources(OpenCVContext& ctx)
{
	auto& d = ctx.d3d11;
	for (UINT i = 0; i < CAPTURE_NUM_BUFFERS; i++)
	{
		if (d.staging[i]) { d.staging[i]->Release(); d.staging[i] = nullptr; }
		if (d.query[i]) { d.query[i]->Release(); d.query[i] = nullptr; }
	}
	d.captureWidth = 0;
	d.captureHeight = 0;
}

static void EvtHydraHookD3D11PrePresent(
	IDXGISwapChain* pSwapChain,
	UINT SyncInterval,
	UINT Flags,
	PHYDRAHOOK_EVT_PRE_EXTENSION Extension
)
{
	(void)SyncInterval;
	(void)Flags;

	OpenCVContext* pCtx = ContextFromExtension(Extension);
	if (!pCtx)
		return;
	OpenCVContext& ctx = *pCtx;
	auto& d = ctx.d3d11;

	RenderGate gate(ctx);
	if (!gate.allow)
		return;

	ID3D11Device* pDevice = nullptr;
	ID3D11DeviceContext* pContext = nullptr;
	if (FAILED(D3D11_DEVICE_IMMEDIATE_CONTEXT_FROM_SWAPCHAIN(pSwapChain, &pDevice, &pContext)))
	{
		HydraHookEngineLogError("HydraHook-OpenCV: Couldn't get D3D11 device/context from swapchain");
		return;
	}

	ID3D11Texture2D* pBackBuffer = nullptr;
	if (FAILED(D3D11_BACKBUFFER_FROM_SWAPCHAIN(pSwapChain, &pBackBuffer)))
	{
		pDevice->Release();
		pContext->Release();
		return;
	}

	DXGI_SWAP_CHAIN_DESC sd = {};
	pSwapChain->GetDesc(&sd);
	const UINT width = sd.BufferDesc.Width;
	const UINT height = sd.BufferDesc.Height;

	if (!d.imguiInitialized)
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::StyleColorsDark();
		ImGui_ImplWin32_Init(sd.OutputWindow);
		ImGui_ImplDX11_Init(pDevice, pContext);
		Overlay_HookWindowProc(sd.OutputWindow);
		d.imguiInitialized = true;
		HydraHookEngineLogInfo("HydraHook-OpenCV: ImGui D3D11 initialized");
	}

	if (d.captureWidth != width || d.captureHeight != height)
	{
		if (!D3D11_CreateCaptureResources(ctx, pDevice, width, height))
		{
			pBackBuffer->Release();
			pDevice->Release();
			pContext->Release();
			return;
		}
	}

	if (d.mainRTV)
	{
		d.mainRTV->Release();
		d.mainRTV = nullptr;
	}
	HRESULT hr = pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &d.mainRTV);
	if (FAILED(hr) || !d.mainRTV)
	{
		pBackBuffer->Release();
		pDevice->Release();
		pContext->Release();
		return;
	}

	const UINT bufIdx = d.frameCounter % CAPTURE_NUM_BUFFERS;
	pContext->CopyResource(d.staging[bufIdx], pBackBuffer);
	pContext->End(d.query[bufIdx]);

	if (d.frameCounter >= 1)
	{
		const UINT prevIdx = (d.frameCounter - 1) % CAPTURE_NUM_BUFFERS;
		/* Non-blocking: GetData returns S_FALSE if GPU not ready. Blocking would freeze the display. */
		if (pContext->GetData(d.query[prevIdx], nullptr, 0, 0) == S_OK)
		{
			D3D11_MAPPED_SUBRESOURCE mapped = {};
			if (SUCCEEDED(pContext->Map(d.staging[prevIdx], 0, D3D11_MAP_READ, 0, &mapped)))
			{
				cv::Mat frame;
				frame.create((int)height, (int)width, CV_8UC3);
				const UINT rp = mapped.RowPitch;
				for (UINT y = 0; y < height; y++)
				{
					const uint8_t* src = (const uint8_t*)mapped.pData + y * rp;
					uint8_t* dst = frame.ptr((int)y);
					for (UINT x = 0; x < width; x++)
					{
						dst[x * 3 + 0] = src[x * 4 + 2];
						dst[x * 3 + 1] = src[x * 4 + 1];
						dst[x * 3 + 2] = src[x * 4 + 0];
					}
				}
				pContext->Unmap(d.staging[prevIdx], 0);
				{
					std::lock_guard<std::mutex> lock(ctx.pending.mutex);
					ctx.pending.d3d11Frame = frame;
					ctx.pending.api = 11;
					ctx.pending.width = d.captureWidth;
					ctx.pending.height = d.captureHeight;
				}
				ctx.pending.cv.notify_one();
			}
		}
	}
	d.frameCounter++;

	pBackBuffer->Release();

	pContext->OMSetRenderTargets(1, &d.mainRTV, nullptr);

	bool showOverlay = ctx.showOverlay;
	Overlay_ToggleState(ctx.overlayToggle, VK_F12, showOverlay);
	ctx.showOverlay = showOverlay;

	if (ctx.showOverlay)
	{
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		PerceptionResults res;
		Capture_GetResults(ctx, res);
		Overlay_Render((float)width, (float)height, res);
		Overlay_DrawDebugHUD(res);

		ImGui::Render();
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}

	pContext->Release();
	pDevice->Release();
}

static void EvtHydraHookD3D11PreResizeBuffers(
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

	OpenCVContext* pCtx = ContextFromExtension(Extension);
	if (!pCtx)
		return;
	OpenCVContext& ctx = *pCtx;

	RenderGate gate(ctx);
	if (!gate.allow)
		return;

	if (ctx.d3d11.mainRTV)
	{
		ctx.d3d11.mainRTV->Release();
		ctx.d3d11.mainRTV = nullptr;
	}
	D3D11_ReleaseCaptureResources(ctx);
}

static void EvtHydraHookD3D11PostResizeBuffers(
	IDXGISwapChain* pSwapChain,
	UINT BufferCount,
	UINT Width,
	UINT Height,
	DXGI_FORMAT NewFormat,
	UINT SwapChainFlags,
	PHYDRAHOOK_EVT_POST_EXTENSION Extension
)
{
	(void)pSwapChain;
	(void)BufferCount;
	(void)Width;
	(void)Height;
	(void)NewFormat;
	(void)SwapChainFlags;
	(void)Extension;
}

#pragma endregion

#pragma region D3D12

static void D3D12_CleanupOverlayResources(OpenCVContext& ctx)
{
	auto& d = ctx.d3d12;
	for (UINT i = 0; i < d.numBackBuffers; i++)
	{
		if (d.mainRenderTargetResource[i])
		{
			d.mainRenderTargetResource[i]->Release();
			d.mainRenderTargetResource[i] = nullptr;
		}
	}
	if (d.pRtvDescHeap)
	{
		d.pRtvDescHeap->Release();
		d.pRtvDescHeap = nullptr;
	}
}

static void D3D12_ReleaseCaptureResources(OpenCVContext& ctx)
{
	auto& d = ctx.d3d12;
	for (UINT i = 0; i < CAPTURE_NUM_BUFFERS; i++)
	{
		if (d.readback[i])
		{
			d.readback[i]->Release();
			d.readback[i] = nullptr;
		}
	}
	d.captureWidth = 0;
	d.captureHeight = 0;
}

static void D3D12_ReleaseDeferredResizeResources(OpenCVContext& ctx)
{
#ifdef _WIN64
	if (ctx.d3d12.imguiInitialized)
	{
		ImGui_ImplDX12_InvalidateDeviceObjects();
		ctx.d3d12.imguiInitialized = false;
	}
	ctx.d3d12.srvDescriptorCount = 0;
#endif
	D3D12_CleanupOverlayResources(ctx);
	D3D12_ReleaseCaptureResources(ctx);
}

static bool D3D12_CreateCaptureResources(OpenCVContext& ctx, UINT width, UINT height, DXGI_FORMAT format)
{
	auto& d = ctx.d3d12;
	D3D12_ReleaseCaptureResources(ctx);
	if (!d.pDevice)
		return false;

	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Alignment = 0;
	texDesc.Width = width;
	texDesc.Height = height;
	texDesc.DepthOrArraySize = 1;
	texDesc.MipLevels = 1;
	texDesc.Format = format;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT64 rowSize = 0;
	UINT64 totalBytes = 0;
	d.pDevice->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, &rowSize, &totalBytes);

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_READBACK;
	heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

	D3D12_RESOURCE_DESC resDesc = {};
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resDesc.Alignment = 0;
	resDesc.Width = totalBytes;
	resDesc.Height = 1;
	resDesc.DepthOrArraySize = 1;
	resDesc.MipLevels = 1;
	resDesc.Format = DXGI_FORMAT_UNKNOWN;
	resDesc.SampleDesc.Count = 1;
	resDesc.SampleDesc.Quality = 0;
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	for (UINT i = 0; i < CAPTURE_NUM_BUFFERS; i++)
	{
		HRESULT hr = d.pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&d.readback[i]));
		if (FAILED(hr) || !d.readback[i])
		{
			HydraHookEngineLogError("HydraHook-OpenCV: D3D12 CreateCommittedResource readback[%u] failed (hr=0x%08X)", (unsigned)i, (unsigned)hr);
			D3D12_ReleaseCaptureResources(ctx);
			return false;
		}
	}

	d.captureWidth = width;
	d.captureHeight = height;
	d.captureRowPitch = (UINT)footprint.Footprint.RowPitch;
	// Fresh readbacks: the first Present of this generation copies without publishing.
	d.frameCounter = 0;
	return true;
}

static bool D3D12_WaitForGpu(OpenCVContext& ctx, DWORD timeoutMs)
{
	auto& d = ctx.d3d12;
	if (!d.pFence || !d.hFenceEvent || d.fenceLastSignaledValue == 0)
		return true;

	const UINT64 fence = d.fenceLastSignaledValue;
	const UINT64 completed = d.pFence->GetCompletedValue();
	if (completed == UINT64_MAX)
		return false;
	if (completed >= fence)
		return true;
	if (FAILED(d.pFence->SetEventOnCompletion(fence, d.hFenceEvent)))
	{
		const UINT64 afterFail = d.pFence->GetCompletedValue();
		return afterFail != UINT64_MAX && afterFail >= fence;
	}

	const DWORD wait = WaitForSingleObject(d.hFenceEvent, timeoutMs);
	const UINT64 after = d.pFence->GetCompletedValue();
	return wait == WAIT_OBJECT_0 && after != UINT64_MAX && after >= fence;
}

static void D3D12_CleanupInitResources(OpenCVContext& ctx)
{
	auto& d = ctx.d3d12;
	if (!D3D12_WaitForGpu(ctx, 2000))
	{
		HydraHookEngineLogError("HydraHook-OpenCV: GPU wait timed out; D3D12 resources left allocated");
		return;
	}
	D3D12_CleanupOverlayResources(ctx);
	D3D12_ReleaseCaptureResources(ctx);
	if (d.pSrvDescHeap) { d.pSrvDescHeap->Release(); d.pSrvDescHeap = nullptr; }
	if (d.hFenceEvent) { CloseHandle(d.hFenceEvent); d.hFenceEvent = nullptr; }
	if (d.pFence) { d.pFence->Release(); d.pFence = nullptr; }
	// A replacement fence starts at 0; a stale value would make the next cleanup wait forever.
	d.fenceLastSignaledValue = 0;
	if (d.pCommandList) { d.pCommandList->Release(); d.pCommandList = nullptr; }
	if (d.pCommandAllocator) { d.pCommandAllocator->Release(); d.pCommandAllocator = nullptr; }
	if (d.pCommandQueue) { d.pCommandQueue->Release(); d.pCommandQueue = nullptr; }
	if (d.pDevice) { d.pDevice->Release(); d.pDevice = nullptr; }
}

static bool D3D12_CreateOverlayResources(OpenCVContext& ctx, IDXGISwapChain* pSwapChain)
{
	auto& d = ctx.d3d12;
	// A resize can reach a revived context before the next Present has created the device.
	if (!d.pDevice)
		return false;
	ID3D12Resource* pBackBuffer = nullptr;
	if (FAILED(D3D12_BACKBUFFER_FROM_SWAPCHAIN(pSwapChain, &pBackBuffer, 0)))
	{
		HydraHookEngineLogError("HydraHook-OpenCV: Couldn't get D3D12 back buffer from swapchain");
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
		HydraHookEngineLogError("HydraHook-OpenCV: Couldn't create D3D12 RTV descriptor heap");
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
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't get swap chain buffer %u", i);
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

#ifdef _WIN64
static void D3D12_SrvDescriptorAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
{
	auto* ctx = static_cast<OpenCVContext*>(info->UserData);
	if (!ctx)
	{
		*out_cpu = {};
		*out_gpu = {};
		return;
	}
	auto& d = ctx->d3d12;
	if (d.srvDescriptorCount >= D3D12_SRV_HEAP_SIZE)
	{
		HydraHookEngineLogError("HydraHook-OpenCV: D3D12_SrvDescriptorAlloc descriptor exhaustion (count=%u, heap_size=%u)", (unsigned)d.srvDescriptorCount, (unsigned)D3D12_SRV_HEAP_SIZE);
		*out_cpu = {};
		*out_gpu = {};
		return;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = d.pSrvDescHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = d.pSrvDescHeap->GetGPUDescriptorHandleForHeapStart();
	cpu.ptr += d.srvDescriptorCount * d.srvDescriptorIncrement;
	gpu.ptr += d.srvDescriptorCount * d.srvDescriptorIncrement;
	d.srvDescriptorCount++;
	*out_cpu = cpu;
	*out_gpu = gpu;
}

static void D3D12_SrvDescriptorFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle)
{
	(void)info;
	(void)cpu_handle;
	(void)gpu_handle;
}
#endif

static void EvtHydraHookD3D12PrePresent(
	IDXGISwapChain* pSwapChain,
	UINT SyncInterval,
	UINT Flags,
	PHYDRAHOOK_EVT_PRE_EXTENSION Extension
)
{
	(void)SyncInterval;
	(void)Flags;

	OpenCVContext* pCtx = ContextFromExtension(Extension);
	if (!pCtx)
		return;
	OpenCVContext& ctx = *pCtx;
	auto& d = ctx.d3d12;

	RenderGate gate(ctx);
	if (!gate.allow)
		return;

	if (!d.initialized)
	{
		HydraHookEngineLogInfo("HydraHook-OpenCV: Grabbing D3D12 device and command queue from swapchain");

		if (FAILED(D3D12_DEVICE_FROM_SWAPCHAIN(pSwapChain, &d.pDevice)))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't get D3D12 device from swapchain");
			return;
		}

		d.pCommandQueue = HydraHookEngineGetD3D12CommandQueue(pSwapChain);
		if (!d.pCommandQueue)
		{
			HydraHookEngineLogInfo("HydraHook-OpenCV: D3D12 command queue not yet captured (mid-process injection); will retry next frame");
			D3D12_CleanupInitResources(ctx);
			return;
		}

		if (FAILED(d.pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d.pCommandAllocator))))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't create D3D12 command allocator");
			D3D12_CleanupInitResources(ctx);
			return;
		}

		if (FAILED(d.pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.pCommandAllocator, nullptr, IID_PPV_ARGS(&d.pCommandList))) ||
			FAILED(d.pCommandList->Close()))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't create D3D12 command list");
			D3D12_CleanupInitResources(ctx);
			return;
		}

		if (FAILED(d.pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.pFence))))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't create D3D12 fence");
			D3D12_CleanupInitResources(ctx);
			return;
		}

		d.hFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!d.hFenceEvent)
		{
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't create fence event");
			D3D12_CleanupInitResources(ctx);
			return;
		}

		if (!D3D12_CreateOverlayResources(ctx, pSwapChain))
		{
			D3D12_CleanupInitResources(ctx);
			return;
		}

#ifdef _WIN64
		D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
		srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvDesc.NumDescriptors = D3D12_SRV_HEAP_SIZE;
		srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(d.pDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&d.pSrvDescHeap))))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't create D3D12 SRV descriptor heap");
			D3D12_CleanupInitResources(ctx);
			return;
		}
		d.srvDescriptorIncrement = d.pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
#endif

		HydraHookEngineLogInfo("HydraHook-OpenCV: D3D12 initialized");
		d.initialized = true;
	}

	if (!d.initialized)
		return;

	DXGI_SWAP_CHAIN_DESC sd = {};
	pSwapChain->GetDesc(&sd);
	const UINT width = sd.BufferDesc.Width;
	const UINT height = sd.BufferDesc.Height;

	if (d.skipResizeRebuild)
	{
		const UINT64 completed = d.pFence ? d.pFence->GetCompletedValue() : UINT64_MAX;
		if (completed == UINT64_MAX || completed < d.fenceLastSignaledValue)
			return;

		D3D12_ReleaseDeferredResizeResources(ctx);
		if (!D3D12_CreateOverlayResources(ctx, pSwapChain))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: D3D12_CreateOverlayResources failed after deferred resize");
			return;
		}
#ifdef _WIN64
		if (d.pDevice && d.pSrvDescHeap)
		{
			ImGui_ImplDX12_CreateDeviceObjects();
			d.imguiInitialized = true;
		}
#endif
		d.skipResizeRebuild = false;
	}

	if (d.captureWidth != width || d.captureHeight != height)
	{
		if (!D3D12_CreateCaptureResources(ctx, width, height, sd.BufferDesc.Format))
			return;
	}

#ifdef _WIN64
	if (!d.imguiInitialized)
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::StyleColorsDark();
		ImGui_ImplWin32_Init(sd.OutputWindow);

		ImGui_ImplDX12_InitInfo initInfo = {};
		initInfo.Device = d.pDevice;
		initInfo.CommandQueue = d.pCommandQueue;
		initInfo.NumFramesInFlight = 2;
		initInfo.RTVFormat = sd.BufferDesc.Format;
		initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
		initInfo.UserData = &ctx;
		initInfo.SrvDescriptorHeap = d.pSrvDescHeap;
		initInfo.SrvDescriptorAllocFn = D3D12_SrvDescriptorAlloc;
		initInfo.SrvDescriptorFreeFn = D3D12_SrvDescriptorFree;

		if (!ImGui_ImplDX12_Init(&initInfo))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: ImGui_ImplDX12_Init failed");
			return;
		}
		Overlay_HookWindowProc(sd.OutputWindow);
		d.imguiInitialized = true;
		HydraHookEngineLogInfo("HydraHook-OpenCV: ImGui D3D12 initialized");
	}
#endif

	UINT backBufferIdx = 0;
	IDXGISwapChain3* pSwapChain3 = nullptr;
	if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&pSwapChain3))))
	{
		backBufferIdx = pSwapChain3->GetCurrentBackBufferIndex();
		pSwapChain3->Release();
	}
	if (backBufferIdx >= d.numBackBuffers)
		backBufferIdx = 0;

	ID3D12Resource* pBackBufferRes = d.mainRenderTargetResource[backBufferIdx];

	d.pCommandAllocator->Reset();
	d.pCommandList->Reset(d.pCommandAllocator, nullptr);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = pBackBufferRes;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	d.pCommandList->ResourceBarrier(1, &barrier);

	const UINT bufIdx = d.frameCounter % CAPTURE_NUM_BUFFERS;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT numRows = 0;
	UINT64 rowSize = 0;
	UINT64 totalBytes = 0;
	const D3D12_RESOURCE_DESC backBufferDesc = pBackBufferRes->GetDesc();
	d.pDevice->GetCopyableFootprints(&backBufferDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &totalBytes);

	D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
	srcLoc.pResource = pBackBufferRes;
	srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	srcLoc.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
	dstLoc.pResource = d.readback[bufIdx];
	dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dstLoc.PlacedFootprint = footprint;

	d.pCommandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	d.pCommandList->ResourceBarrier(1, &barrier);

	d.pCommandList->OMSetRenderTargets(1, &d.mainRenderTargetDescriptor[backBufferIdx], FALSE, nullptr);

	bool showOverlay = ctx.showOverlay;
	Overlay_ToggleState(ctx.overlayToggle, VK_F12, showOverlay);
	ctx.showOverlay = showOverlay;

	if (ctx.showOverlay)
	{
#ifdef _WIN64
		d.pCommandList->SetDescriptorHeaps(1, &d.pSrvDescHeap);
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		PerceptionResults res;
		Capture_GetResults(ctx, res);
		Overlay_Render((float)width, (float)height, res);
		Overlay_DrawDebugHUD(res);

		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), d.pCommandList);
#endif
	}

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	d.pCommandList->ResourceBarrier(1, &barrier);
	d.pCommandList->Close();

	d.pCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&d.pCommandList);
	d.fenceLastSignaledValue++;
	d.pCommandQueue->Signal(d.pFence, d.fenceLastSignaledValue);

	if (d.frameCounter >= 1)
	{
		const UINT prevIdx = (d.frameCounter - 1) % CAPTURE_NUM_BUFFERS;
		{
			std::lock_guard<std::mutex> lock(ctx.pending.mutex);
			// Pre-unhook clears the slot under this lock after stopping the worker. Publishing
			// after that point would leave a reference nobody releases.
			if (ctx.workerRunning)
			{
				// A readback the worker has not picked up yet is superseded; drop its reference.
				if (ctx.pending.d3d12Readback)
					ctx.pending.d3d12Readback->Release();
				ctx.pending.api = 12;
				ctx.pending.d3d12FenceValue = d.fenceValueForReadback[prevIdx];
				ctx.pending.d3d12Readback = d.readback[prevIdx];
				if (ctx.pending.d3d12Readback) ctx.pending.d3d12Readback->AddRef();
				ctx.pending.d3d12RowPitch = d.captureRowPitch;
				ctx.pending.width = d.captureWidth;
				ctx.pending.height = d.captureHeight;
			}
		}
		ctx.pending.cv.notify_one();
	}
	d.fenceValueForReadback[bufIdx] = d.fenceLastSignaledValue;
	d.frameCounter++;
}

static void EvtHydraHookD3D12PreResizeBuffers(
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

	OpenCVContext* pCtx = ContextFromExtension(Extension);
	if (!pCtx)
		return;
	OpenCVContext& ctx = *pCtx;

	RenderGate gate(ctx);
	if (!gate.allow)
		return;

	if (!D3D12_WaitForGpu(ctx, 2000))
	{
		ctx.d3d12.skipResizeRebuild = true;
		HydraHookEngineLogError("HydraHook-OpenCV: resize GPU wait failed; overlay resources left allocated");
		return;
	}
	ctx.d3d12.skipResizeRebuild = false;
	D3D12_ReleaseDeferredResizeResources(ctx);
}

static void EvtHydraHookD3D12PostResizeBuffers(
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

	OpenCVContext* pCtx = ContextFromExtension(Extension);
	if (!pCtx)
		return;
	OpenCVContext& ctx = *pCtx;
	auto& d = ctx.d3d12;

	RenderGate gate(ctx);
	if (!gate.allow || d.skipResizeRebuild)
		return;

	if (!D3D12_CreateOverlayResources(ctx, pSwapChain))
		HydraHookEngineLogError("HydraHook-OpenCV: D3D12_CreateOverlayResources failed after resize");
#ifdef _WIN64
	if (d.imguiInitialized == false && d.pDevice && d.pSrvDescHeap)
	{
		ImGui_ImplDX12_CreateDeviceObjects();
		d.imguiInitialized = true;
	}
#endif
}

#pragma endregion
