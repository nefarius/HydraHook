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

// 
// Detours
// 
#include <detours/detours.h>

// 
// STL
// 
#include <mutex>

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

t_WindowProc OriginalDefWindowProc = nullptr;
t_WindowProc OriginalWindowProc = nullptr;
PHYDRAHOOK_ENGINE engine = nullptr;

/**
 * @brief DLL entry point that initializes or shuts down the HydraHook engine.
 *
 * Initialization (DLL_PROCESS_ATTACH) creates and configures the HydraHook engine
 * and registers the game-hook callback. Shutdown (DLL_PROCESS_DETACH) destroys
 * the engine and releases resources. Do not perform other work here to avoid
 * potential deadlocks (e.g., avoid heavy initialization or thread operations).
 *
 * @param hInstance Module instance handle provided by the OS.
 * @param dwReason  Reason code for the call (e.g., DLL_PROCESS_ATTACH, DLL_PROCESS_DETACH).
 * @param         Unused parameter reserved by the loader.
 * @return TRUE on success, FALSE otherwise.
 */
BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
{
	//
	// We don't need to get notified in thread attach- or detachments
	// 
	DisableThreadLibraryCalls(static_cast<HMODULE>(hInstance));

	HYDRAHOOK_ENGINE_CONFIG cfg;
	HYDRAHOOK_ENGINE_CONFIG_INIT(&cfg);

	cfg.Direct3D.HookDirect3D9 = TRUE;
	cfg.Direct3D.HookDirect3D10 = TRUE;
	cfg.Direct3D.HookDirect3D11 = TRUE;
#ifdef _WIN64
	cfg.Direct3D.HookDirect3D12 = TRUE;
#endif

	cfg.EvtHydraHookGameHooked = EvtHydraHookGameHooked;
	cfg.CrashHandler.IsEnabled = TRUE;

	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:

		//
		// Bootstrap the engine. Allocates resources, establishes hooks etc.
		// 
		(void)HydraHookEngineCreate(
			static_cast<HMODULE>(hInstance),
			&cfg,
			NULL
		);

		break;
	case DLL_PROCESS_DETACH:

		//
		// Tears down the engine. Graceful shutdown, frees resources etc.
		// 
		(void)HydraHookEngineDestroy(static_cast<HMODULE>(hInstance));

		break;
	default:
		break;
	}

	return TRUE;
}

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
	d3d10.EvtHydraHookD3D10PostResizeBuffers = EvtHydraHookD3D10PostResizeBuffers;

	HYDRAHOOK_D3D11_EVENT_CALLBACKS d3d11;
	HYDRAHOOK_D3D11_EVENT_CALLBACKS_INIT(&d3d11);
	d3d11.EvtHydraHookD3D11PrePresent = EvtHydraHookD3D11Present;
	d3d11.EvtHydraHookD3D11PreResizeBuffers = EvtHydraHookD3D11PreResizeBuffers;
	d3d11.EvtHydraHookD3D11PostResizeBuffers = EvtHydraHookD3D11PostResizeBuffers;

#ifdef _WIN64
	HYDRAHOOK_D3D12_EVENT_CALLBACKS d3d12;
	HYDRAHOOK_D3D12_EVENT_CALLBACKS_INIT(&d3d12);
	d3d12.EvtHydraHookD3D12PrePresent = EvtHydraHookD3D12Present;
	d3d12.EvtHydraHookD3D12PreResizeBuffers = EvtHydraHookD3D12PreResizeBuffers;
	d3d12.EvtHydraHookD3D12PostResizeBuffers = EvtHydraHookD3D12PostResizeBuffers;
#endif

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
 * \brief   Gets called when all core engine hooks have been released. At this stage it is save
 *          to remove our own additional hooks and shut down the hooking sub-system as well.
 *
 * \author  Benjamin "Nefarius" Hï¿½glinger
 * \date    16.06.2018
 */
void EvtHydraHookGameUnhooked()
{
#ifdef WNDPROC_HOOK
	auto& logger = Logger::get(__func__);

	if (MH_DisableHook(MH_ALL_HOOKS) != MH_OK)
	{
		logger.fatal("Couldn't disable hooks, host process might crash");
		return;
	}

	HydraHookEngineLogInfo("Hooks disabled");

	if (MH_Uninitialize() != MH_OK)
	{
		logger.fatal("Couldn't shut down hook engine, host process might crash");
		return;
	}
#endif
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
	static auto initialized = false;
	static bool show_overlay = true;
	static std::once_flag init;

	//
	// This section is only called once to initialize ImGui
	// 
	std::call_once(init, [&](LPDIRECT3DDEVICE9 pd3dDevice)
	{
		D3DDEVICE_CREATION_PARAMETERS params;

		const auto hr = pd3dDevice->GetCreationParameters(&params);
		if (FAILED(hr))
		{
			HydraHookEngineLogError("Couldn't get creation parameters from device");
			return;
		}

		ImGui_ImplWin32_Init(params.hFocusWindow);
		ImGui_ImplDX9_Init(pd3dDevice);

		HydraHookEngineLogInfo("ImGui (DX9) initialized");

		HookWindowProc(params.hFocusWindow);

		initialized = true;

	}, pDevice);

	if (!initialized)
		return;

	TOGGLE_STATE(VK_F12, show_overlay);
	if (!show_overlay)
		return;

	// Start the Dear ImGui frame
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	RenderScene();

	ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void EvtHydraHookD3D9PreReset(
	LPDIRECT3DDEVICE9       pDevice,
	D3DPRESENT_PARAMETERS   *pPresentationParameters
)
{
	ImGui_ImplDX9_InvalidateDeviceObjects();
}

void EvtHydraHookD3D9PostReset(
	LPDIRECT3DDEVICE9       pDevice,
	D3DPRESENT_PARAMETERS   *pPresentationParameters
)
{
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
	static auto initialized = false;
	static bool show_overlay = true;
	static std::once_flag init;

	//
	// This section is only called once to initialize ImGui
	// 
	std::call_once(init, [&](LPDIRECT3DDEVICE9EX pd3dDevice)
	{
		D3DDEVICE_CREATION_PARAMETERS params;

		const auto hr = pd3dDevice->GetCreationParameters(&params);
		if (FAILED(hr))
		{
			HydraHookEngineLogError("Couldn't get creation parameters from device");
			return;
		}

		ImGui_ImplWin32_Init(params.hFocusWindow);
		ImGui_ImplDX9_Init(pd3dDevice);

		HydraHookEngineLogInfo("ImGui (DX9Ex) initialized");

		HookWindowProc(params.hFocusWindow);

		initialized = true;

	}, pDevice);

	if (!initialized)
		return;

	TOGGLE_STATE(VK_F12, show_overlay);
	if (!show_overlay)
		return;

	// Start the Dear ImGui frame
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	RenderScene();

	ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void EvtHydraHookD3D9PreResetEx(
	LPDIRECT3DDEVICE9EX     pDevice,
	D3DPRESENT_PARAMETERS   *pPresentationParameters,
	D3DDISPLAYMODEEX        *pFullscreenDisplayMode
)
{
	ImGui_ImplDX9_InvalidateDeviceObjects();
}

void EvtHydraHookD3D9PostResetEx(
	LPDIRECT3DDEVICE9EX     pDevice,
	D3DPRESENT_PARAMETERS   *pPresentationParameters,
	D3DDISPLAYMODEEX        *pFullscreenDisplayMode
)
{
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
	static auto initialized = false;
	static bool show_overlay = true;
	static std::once_flag init;

	//
	// This section is only called once to initialize ImGui
	// 
	std::call_once(init, [&](IDXGISwapChain *pChain)
	{
		HydraHookEngineLogInfo("Grabbing device and context pointers");

		ID3D10Device *pDevice;
		if (FAILED(D3D10_DEVICE_FROM_SWAPCHAIN(pChain, &pDevice)))
		{
			HydraHookEngineLogError("Couldn't get device from swapchain");
			return;
		}

		DXGI_SWAP_CHAIN_DESC sd;
		pChain->GetDesc(&sd);

		HydraHookEngineLogInfo("Initializing ImGui");

		ImGui_ImplWin32_Init(sd.OutputWindow);
		ImGui_ImplDX10_Init(pDevice);

		HydraHookEngineLogInfo("ImGui (DX10) initialized");

		HookWindowProc(sd.OutputWindow);

		initialized = true;

	}, pSwapChain);

	if (!initialized)
		return;

	TOGGLE_STATE(VK_F12, show_overlay);
	if (!show_overlay)
		return;


	// Start the Dear ImGui frame
	ImGui_ImplDX10_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	RenderScene();

	ImGui_ImplDX10_RenderDrawData(ImGui::GetDrawData());
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
	ImGui_ImplDX10_InvalidateDeviceObjects();
}

void EvtHydraHookD3D10PostResizeBuffers(
	IDXGISwapChain  *pSwapChain,
	UINT            BufferCount,
	UINT            Width,
	UINT            Height,
	DXGI_FORMAT     NewFormat,
	UINT            SwapChainFlags
)
{
	ImGui_ImplDX10_CreateDeviceObjects();
}

#pragma endregion

#pragma region D3D11

// TODO: lazy global, improve
static ID3D11RenderTargetView *g_d3d11_mainRenderTargetView = nullptr;

void EvtHydraHookD3D11Present(
	IDXGISwapChain				*pSwapChain,
	UINT						SyncInterval,
	UINT						Flags,
	PHYDRAHOOK_EVT_PRE_EXTENSION Extension
)
{
	static auto initialized = false;
	static bool show_overlay = true;
	static std::once_flag init;

	static ID3D11DeviceContext *pContext;

	//
	// This section is only called once to initialize ImGui
	// 
	std::call_once(init, [&](IDXGISwapChain *pChain)
	{
		HydraHookEngineLogInfo("Grabbing device and context pointers");

		ID3D11Device *pDevice;
		if (FAILED(D3D11_DEVICE_IMMEDIATE_CONTEXT_FROM_SWAPCHAIN(pChain, &pDevice, &pContext)))
		{
			HydraHookEngineLogError("Couldn't get device and context from swapchain");
			return;
		}

		ID3D11Texture2D* pBackBuffer;
		pChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
		pDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_d3d11_mainRenderTargetView);
		pBackBuffer->Release();

		DXGI_SWAP_CHAIN_DESC sd;
		pChain->GetDesc(&sd);

		HydraHookEngineLogInfo("Initializing ImGui");

		ImGui_ImplWin32_Init(sd.OutputWindow);
		ImGui_ImplDX11_Init(pDevice, pContext);

		HydraHookEngineLogInfo("ImGui (DX11) initialized");

		HookWindowProc(sd.OutputWindow);

		initialized = true;

	}, pSwapChain);

	if (!initialized)
		return;

	TOGGLE_STATE(VK_F12, show_overlay);
	if (!show_overlay)
		return;

	// Start the Dear ImGui frame
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	pContext->OMSetRenderTargets(1, &g_d3d11_mainRenderTargetView, NULL);

	RenderScene();

	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
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
	if (g_d3d11_mainRenderTargetView)
	{
		g_d3d11_mainRenderTargetView->Release();
		g_d3d11_mainRenderTargetView = nullptr;
	}
}

//
// Called after the original invocation of ResizeBuffers.
// 
void EvtHydraHookD3D11PostResizeBuffers(
	IDXGISwapChain					*pSwapChain,
	UINT							BufferCount,
	UINT							Width,
	UINT							Height,
	DXGI_FORMAT						NewFormat,
	UINT							SwapChainFlags,
	PHYDRAHOOK_EVT_POST_EXTENSION	Extension
)
{
	ID3D11Texture2D* pBackBuffer;
	ID3D11DeviceContext *pContext;
	ID3D11Device *pDevice;
	D3D11_DEVICE_IMMEDIATE_CONTEXT_FROM_SWAPCHAIN(pSwapChain, &pDevice, &pContext);

	pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	pDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_d3d11_mainRenderTargetView);
	pBackBuffer->Release();
}

#pragma endregion

#pragma region D3D12

#ifdef _WIN64

static constexpr UINT D3D12_NUM_BACK_BUFFERS = 3;
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
}

/**
 * @brief Blocks the CPU until the GPU has completed work up to the last signaled fence value.
 *
 * Waits on the fence event for g_d3d12_fenceLastSignaledValue (signaled by the caller before invoking).
 */
static void D3D12_WaitForGpu()
{
	const UINT64 fence = g_d3d12_fenceLastSignaledValue;
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
	(void)SyncInterval;
	(void)Flags;
	(void)Extension;

	static auto initialized = false;
	static bool show_overlay = true;

	if (!initialized)
	{
		HydraHookEngineLogInfo("Grabbing D3D12 device and command queue from swapchain");

		if (FAILED(D3D12_DEVICE_FROM_SWAPCHAIN(pSwapChain, &g_d3d12_pDevice)))
		{
			HydraHookEngineLogError("Couldn't get D3D12 device from swapchain");
			return;
		}

		g_d3d12_pCommandQueue = HydraHookEngineGetD3D12CommandQueue(pSwapChain);
		if (!g_d3d12_pCommandQueue)
		{
			HydraHookEngineLogInfo("D3D12 command queue not yet captured (mid-process injection); will retry next frame");
			D3D12_CleanupInitResources();
			return;
		}

		if (FAILED(g_d3d12_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_d3d12_pCommandAllocator))))
		{
			HydraHookEngineLogError("Couldn't create D3D12 command allocator");
			D3D12_CleanupInitResources();
			return;
		}

		if (FAILED(g_d3d12_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_d3d12_pCommandAllocator, nullptr, IID_PPV_ARGS(&g_d3d12_pCommandList))) ||
			FAILED(g_d3d12_pCommandList->Close()))
		{
			HydraHookEngineLogError("Couldn't create D3D12 command list");
			D3D12_CleanupInitResources();
			return;
		}

		if (FAILED(g_d3d12_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_d3d12_pFence))))
		{
			HydraHookEngineLogError("Couldn't create D3D12 fence");
			D3D12_CleanupInitResources();
			return;
		}

		g_d3d12_hFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!g_d3d12_hFenceEvent)
		{
			HydraHookEngineLogError("Couldn't create fence event");
			D3D12_CleanupInitResources();
			return;
		}

		D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
		srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvDesc.NumDescriptors = D3D12_SRV_HEAP_SIZE;
		srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(g_d3d12_pDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_d3d12_pSrvDescHeap))))
		{
			HydraHookEngineLogError("Couldn't create D3D12 SRV descriptor heap");
			D3D12_CleanupInitResources();
			return;
		}
		g_d3d12_srvDescriptorIncrement = g_d3d12_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		if (!D3D12_CreateOverlayResources(pSwapChain))
		{
			D3D12_CleanupInitResources();
			return;
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
			return;
		}

		ImGui_ImplWin32_Init(sd.OutputWindow);
		HydraHookEngineLogInfo("ImGui (DX12) initialized");
		HookWindowProc(sd.OutputWindow);
		initialized = true;
	}

	if (!initialized)
		return;

	TOGGLE_STATE(VK_F12, show_overlay);
	if (!show_overlay)
		return;

	UINT backBufferIdx = 0;
	IDXGISwapChain3* pSwapChain3 = nullptr;
	if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&pSwapChain3))))
	{
		backBufferIdx = pSwapChain3->GetCurrentBackBufferIndex();
		pSwapChain3->Release();
	}
	if (backBufferIdx >= g_d3d12_numBackBuffers)
		backBufferIdx = 0;

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

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
	g_d3d12_pCommandQueue->Signal(g_d3d12_pFence, ++g_d3d12_fenceLastSignaledValue);
	D3D12_WaitForGpu();
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

	ImGui_ImplDX12_InvalidateDeviceObjects();
	D3D12_WaitForGpu();
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

/**
 * @brief Installs hooks for window-procedure dispatch to intercept input and forward to ImGui.
 *
 * Creates and enables hooks for DefWindowProcW, DefWindowProcA, and the specified window's
 * GWLP_WNDPROC, and stores the original function pointers in `OriginalDefWindowProc` and
 * `OriginalWindowProc` for later unhooking or forwarding.
 *
 * @note This function has effect only when `WNDPROC_HOOK` is enabled at build time.
 *
 * @param hWnd Handle to the target window whose window procedure will be hooked.
 */
void HookWindowProc(HWND hWnd)
{
#ifdef WNDPROC_HOOK

	auto& logger = Logger::get(__func__);

	MH_STATUS ret;

	if ((ret = MH_CreateHook(
		&DefWindowProcW,
		&DetourDefWindowProc,
		reinterpret_cast<LPVOID*>(&OriginalDefWindowProc))
		) != MH_OK)
	{
		HydraHookEngineLogError("Couldn't create hook for DefWindowProcW: %lu", static_cast<ULONG>(ret));
		return;
	}

	if (ret == MH_OK && MH_EnableHook(&DefWindowProcW) != MH_OK)
	{
		HydraHookEngineLogError("Couldn't enable DefWindowProcW hook");
	}

	if ((ret = MH_CreateHook(
		&DefWindowProcA,
		&DetourDefWindowProc,
		reinterpret_cast<LPVOID*>(&OriginalDefWindowProc))
		) != MH_OK)
	{
		HydraHookEngineLogError("Couldn't create hook for DefWindowProcA: %lu", static_cast<ULONG>(ret));
		return;
	}

	if (ret == MH_OK && MH_EnableHook(&DefWindowProcA) != MH_OK)
	{
		HydraHookEngineLogError("Couldn't enable DefWindowProcW hook");
	}

	auto lptrWndProc = reinterpret_cast<t_WindowProc>(GetWindowLongPtr(hWnd, GWLP_WNDPROC));

	if (MH_CreateHook(lptrWndProc, &DetourWindowProc, reinterpret_cast<LPVOID*>(&OriginalWindowProc)) != MH_OK)
	{
		logger.warning("Couldn't create hook for GWLP_WNDPROC");
		return;
	}

	if (MH_EnableHook(lptrWndProc) != MH_OK)
	{
		HydraHookEngineLogError("Couldn't enable GWLP_WNDPROC hook");
	}
#endif
}

LRESULT WINAPI DetourDefWindowProc(
	_In_ HWND hWnd,
	_In_ UINT Msg,
	_In_ WPARAM wParam,
	_In_ LPARAM lParam
)
{
	static std::once_flag flag;
	std::call_once(flag, []() { HydraHookEngineLogInfo("++ DetourDefWindowProc called"); });

	ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);

	return OriginalDefWindowProc(hWnd, Msg, wParam, lParam);
}

LRESULT WINAPI DetourWindowProc(
	_In_ HWND hWnd,
	_In_ UINT Msg,
	_In_ WPARAM wParam,
	_In_ LPARAM lParam
)
{
	static std::once_flag flag;
	std::call_once(flag, []() { HydraHookEngineLogInfo("++ DetourWindowProc called"); });

	ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);

	return OriginalWindowProc(hWnd, Msg, wParam, lParam);
}

#pragma endregion

#pragma region Main content rendering

/**
 * @brief Renders the ImGui overlay/demo UI containing plots and widgets.
 *
 * Displays an ImGui window with frame-time plots, line plots and histograms (including a selectable function and sample count),
 * an "Animate" toggle, and an animated progress bar with textual label. Submits ImGui draw data by calling ImGui::Render().
 */
void RenderScene()
{
	static std::once_flag flag;
	std::call_once(flag, []() { HydraHookEngineLogInfo("++ RenderScene called"); });

	ImGui::ShowMetricsWindow();

	{
		ImGui::SetNextWindowPos(ImVec2(1400, 60));
		ImGui::Begin(
			"Some plots =)",
			nullptr,
			ImGuiWindowFlags_AlwaysAutoResize
		);

		static bool animate = true;
		ImGui::Checkbox("Animate", &animate);

		static float arr[] = { 0.6f, 0.1f, 1.0f, 0.5f, 0.92f, 0.1f, 0.2f };
		ImGui::PlotLines("Frame Times", arr, IM_ARRAYSIZE(arr));

		// Create a dummy array of contiguous float values to plot
		// Tip: If your float aren't contiguous but part of a structure, you can pass a pointer to your first float and the sizeof() of your structure in the Stride parameter.
		static float values[90] = { 0 };
		static int values_offset = 0;
		static float refresh_time = 0.0f;
		if (!animate || refresh_time == 0.0f)
			refresh_time = (float)ImGui::GetTime();
		while (refresh_time < ImGui::GetTime()) // Create dummy data at fixed 60 hz rate for the demo
		{
			static float phase = 0.0f;
			values[values_offset] = cosf(phase);
			values_offset = (values_offset + 1) % IM_ARRAYSIZE(values);
			phase += 0.10f*values_offset;
			refresh_time += 1.0f / 60.0f;
		}
		ImGui::PlotLines("Lines", values, IM_ARRAYSIZE(values), values_offset, "avg 0.0", -1.0f, 1.0f, ImVec2(0, 80));
		ImGui::PlotHistogram("Histogram", arr, IM_ARRAYSIZE(arr), 0, NULL, 0.0f, 1.0f, ImVec2(0, 80));

		// Use functions to generate output
		// FIXME: This is rather awkward because current plot API only pass in indices. We probably want an API passing floats and user provide sample rate/count.
		struct Funcs
		{
			static float Sin(void*, int i) { return sinf(i * 0.1f); }
			static float Saw(void*, int i) { return (i & 1) ? 1.0f : -1.0f; }
		};
		static int func_type = 0, display_count = 70;
		ImGui::Separator();
		ImGui::PushItemWidth(100); ImGui::Combo("func", &func_type, "Sin\0Saw\0"); ImGui::PopItemWidth();
		ImGui::SameLine();
		ImGui::SliderInt("Sample count", &display_count, 1, 400);
		float(*func)(void*, int) = (func_type == 0) ? Funcs::Sin : Funcs::Saw;
		ImGui::PlotLines("Lines", func, NULL, display_count, 0, NULL, -1.0f, 1.0f, ImVec2(0, 80));
		ImGui::PlotHistogram("Histogram", func, NULL, display_count, 0, NULL, -1.0f, 1.0f, ImVec2(0, 80));
		ImGui::Separator();

		// Animate a simple progress bar
		static float progress = 0.0f, progress_dir = 1.0f;
		if (animate)
		{
			progress += progress_dir * 0.4f * ImGui::GetIO().DeltaTime;
			if (progress >= +1.1f) { progress = +1.1f; progress_dir *= -1.0f; }
			if (progress <= -0.1f) { progress = -0.1f; progress_dir *= -1.0f; }
		}

		// Typically we would use ImVec2(-1.0f,0.0f) to use all available width, or ImVec2(width,0.0f) for a specified width. ImVec2(0.0f,0.0f) uses ItemWidth.
		ImGui::ProgressBar(progress, ImVec2(0.0f, 0.0f));
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::Text("Progress Bar");

		float progress_saturated = (progress < 0.0f) ? 0.0f : (progress > 1.0f) ? 1.0f : progress;
		char buf[32];
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
		sprintf(buf, "%d/%d", (int)(progress_saturated * 1753), 1753);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
		ImGui::ProgressBar(progress, ImVec2(0.f, 0.f), buf);

		ImGui::End();
	}

	ImGui::Render();
}

#pragma endregion

#pragma region ImGui-specific (taken from their examples unmodified)

bool ImGui_ImplWin32_UpdateMouseCursor()
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
		return false;

	ImGuiMouseCursor imgui_cursor = io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
	if (imgui_cursor == ImGuiMouseCursor_None)
	{
		// Hide OS mouse cursor if imgui is drawing it or if it wants no cursor
		::SetCursor(NULL);
	}
	else
	{
		// Hardware cursor type
		LPTSTR win32_cursor = IDC_ARROW;
		switch (imgui_cursor)
		{
		case ImGuiMouseCursor_Arrow:        win32_cursor = IDC_ARROW; break;
		case ImGuiMouseCursor_TextInput:    win32_cursor = IDC_IBEAM; break;
		case ImGuiMouseCursor_ResizeAll:    win32_cursor = IDC_SIZEALL; break;
		case ImGuiMouseCursor_ResizeEW:     win32_cursor = IDC_SIZEWE; break;
		case ImGuiMouseCursor_ResizeNS:     win32_cursor = IDC_SIZENS; break;
		case ImGuiMouseCursor_ResizeNESW:   win32_cursor = IDC_SIZENESW; break;
		case ImGuiMouseCursor_ResizeNWSE:   win32_cursor = IDC_SIZENWSE; break;
		}
		::SetCursor(::LoadCursor(NULL, win32_cursor));
	}
	return true;
}

#pragma endregion