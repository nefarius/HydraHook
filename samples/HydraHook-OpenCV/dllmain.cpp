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

#include <HydraHook/Engine/HydraHookDirect3D11.h>
#include <HydraHook/Engine/HydraHookDirect3D12.h>
#include <HydraHook/Engine/HydraHookCore.h>

#include <opencv2/core.hpp>

#include <dxgi1_4.h>

#include <mutex>


static constexpr float CLEAR_COLOR[4] = { 0.0f, 0.5f, 0.5f, 1.0f }; /* Teal - distinct overlay tint */

/* D3D12 state */
static constexpr UINT D3D12_NUM_BACK_BUFFERS = 2;
static ID3D12Device* g_d3d12_pDevice = nullptr;
static ID3D12CommandQueue* g_d3d12_pCommandQueue = nullptr;
static ID3D12CommandAllocator* g_d3d12_pCommandAllocator = nullptr;
static ID3D12GraphicsCommandList* g_d3d12_pCommandList = nullptr;
static ID3D12Fence* g_d3d12_pFence = nullptr;
static HANDLE g_d3d12_hFenceEvent = nullptr;
static UINT64 g_d3d12_fenceLastSignaledValue = 0;
static ID3D12DescriptorHeap* g_d3d12_pRtvDescHeap = nullptr;
static ID3D12Resource* g_d3d12_mainRenderTargetResource[D3D12_NUM_BACK_BUFFERS] = {};
static D3D12_CPU_DESCRIPTOR_HANDLE g_d3d12_mainRenderTargetDescriptor[D3D12_NUM_BACK_BUFFERS] = {};
static UINT g_d3d12_rtvDescriptorSize = 0;
static UINT g_d3d12_numBackBuffers = D3D12_NUM_BACK_BUFFERS;

static void D3D12_CleanupOverlayResources();
static void D3D12_CleanupInitResources();
static void D3D12_WaitForGpu();
static bool D3D12_CreateOverlayResources(IDXGISwapChain* pSwapChain);

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
{
	DisableThreadLibraryCalls(static_cast<HMODULE>(hInstance));

	HYDRAHOOK_ENGINE_CONFIG cfg;
	HYDRAHOOK_ENGINE_CONFIG_INIT(&cfg);

	cfg.Direct3D.HookDirect3D11 = TRUE;
	cfg.Direct3D.HookDirect3D12 = TRUE;
	cfg.EvtHydraHookGameHooked = EvtHydraHookGameHooked;

	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		(void)HydraHookEngineCreate(
			static_cast<HMODULE>(hInstance),
			&cfg,
			NULL
		);
		break;
	case DLL_PROCESS_DETACH:
		(void)HydraHookEngineDestroy(static_cast<HMODULE>(hInstance));
		break;
	default:
		break;
	}

	return TRUE;
}

void EvtHydraHookGameHooked(
	PHYDRAHOOK_ENGINE EngineHandle,
	const HYDRAHOOK_D3D_VERSION GameVersion
)
{
	HydraHookEngineLogInfo("HydraHook-OpenCV: Loading");

	HYDRAHOOK_D3D11_EVENT_CALLBACKS d3d11;
	HYDRAHOOK_D3D11_EVENT_CALLBACKS_INIT(&d3d11);
	d3d11.EvtHydraHookD3D11PrePresent = EvtHydraHookD3D11PrePresent;
	d3d11.EvtHydraHookD3D11PreResizeBuffers = EvtHydraHookD3D11PreResizeBuffers;
	d3d11.EvtHydraHookD3D11PostResizeBuffers = EvtHydraHookD3D11PostResizeBuffers;

	HYDRAHOOK_D3D12_EVENT_CALLBACKS d3d12;
	HYDRAHOOK_D3D12_EVENT_CALLBACKS_INIT(&d3d12);
	d3d12.EvtHydraHookD3D12PrePresent = EvtHydraHookD3D12PrePresent;
	d3d12.EvtHydraHookD3D12PreResizeBuffers = EvtHydraHookD3D12PreResizeBuffers;
	d3d12.EvtHydraHookD3D12PostResizeBuffers = EvtHydraHookD3D12PostResizeBuffers;

	switch (GameVersion)
	{
	case HydraHookDirect3DVersion11:
		HydraHookEngineSetD3D11EventCallbacks(EngineHandle, &d3d11);
		break;
	case HydraHookDirect3DVersion12:
		HydraHookEngineSetD3D12EventCallbacks(EngineHandle, &d3d12);
		break;
	default:
		HydraHookEngineLogInfo("HydraHook-OpenCV: Unsupported D3D version, no callbacks registered");
		break;
	}
}

#pragma region D3D11

void EvtHydraHookD3D11PrePresent(
	IDXGISwapChain* pSwapChain,
	UINT SyncInterval,
	UINT Flags,
	PHYDRAHOOK_EVT_PRE_EXTENSION Extension
)
{
	(void)SyncInterval;
	(void)Flags;
	(void)Extension;

	/* Verify OpenCV links */
	cv::Mat testMat(1, 1, CV_8UC1);
	(void)testMat;

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

	ID3D11RenderTargetView* pRTV = nullptr;
	HRESULT hr = pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRTV);
	pBackBuffer->Release();
	if (FAILED(hr) || !pRTV)
	{
		pDevice->Release();
		pContext->Release();
		return;
	}

	pContext->OMSetRenderTargets(1, &pRTV, nullptr);
	pContext->ClearRenderTargetView(pRTV, CLEAR_COLOR);

	static std::once_flag d3d11Logged;
	std::call_once(d3d11Logged, []() {
		HydraHookEngineLogInfo("HydraHook-OpenCV: D3D11 Present hook active (clear to teal)");
	});

	pRTV->Release();
	pContext->Release();
	pDevice->Release();
}

void EvtHydraHookD3D11PreResizeBuffers(
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
}

void EvtHydraHookD3D11PostResizeBuffers(
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

static void D3D12_CleanupOverlayResources()
{
	for (UINT i = 0; i < g_d3d12_numBackBuffers; i++)
	{
		if (g_d3d12_mainRenderTargetResource[i])
		{
			g_d3d12_mainRenderTargetResource[i]->Release();
			g_d3d12_mainRenderTargetResource[i] = nullptr;
		}
	}
	if (g_d3d12_pRtvDescHeap)
	{
		g_d3d12_pRtvDescHeap->Release();
		g_d3d12_pRtvDescHeap = nullptr;
	}
}

static void D3D12_CleanupInitResources()
{
	D3D12_CleanupOverlayResources();
	if (g_d3d12_hFenceEvent) { CloseHandle(g_d3d12_hFenceEvent); g_d3d12_hFenceEvent = nullptr; }
	if (g_d3d12_pFence) { g_d3d12_pFence->Release(); g_d3d12_pFence = nullptr; }
	if (g_d3d12_pCommandList) { g_d3d12_pCommandList->Release(); g_d3d12_pCommandList = nullptr; }
	if (g_d3d12_pCommandAllocator) { g_d3d12_pCommandAllocator->Release(); g_d3d12_pCommandAllocator = nullptr; }
	if (g_d3d12_pCommandQueue) { g_d3d12_pCommandQueue->Release(); g_d3d12_pCommandQueue = nullptr; }
	if (g_d3d12_pDevice) { g_d3d12_pDevice->Release(); g_d3d12_pDevice = nullptr; }
}

static void D3D12_WaitForGpu()
{
	const UINT64 fence = g_d3d12_fenceLastSignaledValue;
	g_d3d12_pFence->SetEventOnCompletion(fence, g_d3d12_hFenceEvent);
	WaitForSingleObject(g_d3d12_hFenceEvent, INFINITE);
}

static bool D3D12_CreateOverlayResources(IDXGISwapChain* pSwapChain)
{
	ID3D12Resource* pBackBuffer = nullptr;
	if (FAILED(D3D12_BACKBUFFER_FROM_SWAPCHAIN(pSwapChain, &pBackBuffer, 0)))
	{
		HydraHookEngineLogError("HydraHook-OpenCV: Couldn't get D3D12 back buffer from swapchain");
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
		HydraHookEngineLogError("HydraHook-OpenCV: Couldn't create D3D12 RTV descriptor heap");
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
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't get swap chain buffer %u", i);
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

void EvtHydraHookD3D12PrePresent(
	IDXGISwapChain* pSwapChain,
	UINT SyncInterval,
	UINT Flags,
	PHYDRAHOOK_EVT_PRE_EXTENSION Extension
)
{
	(void)SyncInterval;
	(void)Flags;
	(void)Extension;

	static bool initialized = false;

	if (!initialized)
	{
		HydraHookEngineLogInfo("HydraHook-OpenCV: Grabbing D3D12 device and command queue from swapchain");

		if (FAILED(D3D12_DEVICE_FROM_SWAPCHAIN(pSwapChain, &g_d3d12_pDevice)))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't get D3D12 device from swapchain");
			return;
		}

		g_d3d12_pCommandQueue = HydraHookEngineGetD3D12CommandQueue(pSwapChain);
		if (!g_d3d12_pCommandQueue)
		{
			HydraHookEngineLogInfo("HydraHook-OpenCV: D3D12 command queue not yet captured (mid-process injection); will retry next frame");
			D3D12_CleanupInitResources();
			return;
		}

		if (FAILED(g_d3d12_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_d3d12_pCommandAllocator))))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't create D3D12 command allocator");
			D3D12_CleanupInitResources();
			return;
		}

		if (FAILED(g_d3d12_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_d3d12_pCommandAllocator, nullptr, IID_PPV_ARGS(&g_d3d12_pCommandList))) ||
			FAILED(g_d3d12_pCommandList->Close()))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't create D3D12 command list");
			D3D12_CleanupInitResources();
			return;
		}

		if (FAILED(g_d3d12_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_d3d12_pFence))))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't create D3D12 fence");
			D3D12_CleanupInitResources();
			return;
		}

		g_d3d12_hFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!g_d3d12_hFenceEvent)
		{
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't create fence event");
			D3D12_CleanupInitResources();
			return;
		}

		if (!D3D12_CreateOverlayResources(pSwapChain))
		{
			D3D12_CleanupInitResources();
			return;
		}

		HydraHookEngineLogInfo("HydraHook-OpenCV: D3D12 initialized");
		initialized = true;
	}

	if (!initialized)
		return;

	/* Verify OpenCV links */
	cv::Mat testMat(1, 1, CV_8UC1);
	(void)testMat;

	UINT backBufferIdx = 0;
	IDXGISwapChain3* pSwapChain3 = nullptr;
	if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&pSwapChain3))))
	{
		backBufferIdx = pSwapChain3->GetCurrentBackBufferIndex();
		pSwapChain3->Release();
	}
	if (backBufferIdx >= g_d3d12_numBackBuffers)
		backBufferIdx = 0;

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
	g_d3d12_pCommandList->ClearRenderTargetView(g_d3d12_mainRenderTargetDescriptor[backBufferIdx], CLEAR_COLOR, 0, nullptr);

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	g_d3d12_pCommandList->ResourceBarrier(1, &barrier);
	g_d3d12_pCommandList->Close();

	g_d3d12_pCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_d3d12_pCommandList);
	g_d3d12_pCommandQueue->Signal(g_d3d12_pFence, ++g_d3d12_fenceLastSignaledValue);
	D3D12_WaitForGpu();

	static std::once_flag d3d12Logged;
	std::call_once(d3d12Logged, []() {
		HydraHookEngineLogInfo("HydraHook-OpenCV: D3D12 Present hook active (clear to teal)");
	});
}

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

	D3D12_WaitForGpu();
	D3D12_CleanupOverlayResources();
}

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

	if (!D3D12_CreateOverlayResources(pSwapChain))
		HydraHookEngineLogError("HydraHook-OpenCV: D3D12_CreateOverlayResources failed after resize");
}

#pragma endregion
