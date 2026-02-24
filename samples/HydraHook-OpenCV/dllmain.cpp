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
#include <opencv2/features2d.hpp>
#include <opencv2/video/tracking.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <dxgi1_4.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>


/* ---------------------------------------------------------------------------
 * Shared perception results (mutex-protected)
 * --------------------------------------------------------------------------- */
struct PerceptionResults
{
	std::vector<cv::Point2f> prevPts;
	std::vector<cv::Point2f> currPts;
	cv::Mat R;
	cv::Mat t;
	std::vector<cv::Vec3f> poseTrail;
	bool valid = false;
	int featureCount = 0;
	int inliers = 0;
};

static std::mutex g_resultsMutex;
static PerceptionResults g_results;
static std::atomic<bool> g_workerRunning{ true };
static std::thread* g_workerThread = nullptr;

/* D3D11 capture state */
static constexpr UINT CAPTURE_NUM_BUFFERS = 2;
static ID3D11Texture2D* g_d3d11_staging[CAPTURE_NUM_BUFFERS] = {};
static ID3D11Query* g_d3d11_query[CAPTURE_NUM_BUFFERS] = {};
static UINT g_d3d11_captureWidth = 0;
static UINT g_d3d11_captureHeight = 0;
static UINT g_d3d11_frameCounter = 0;

/* D3D11 overlay state */
static ID3D11RenderTargetView* g_d3d11_mainRTV = nullptr;
static bool g_d3d11_imguiInitialized = false;

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

/* D3D12 capture state */
static ID3D12Resource* g_d3d12_readback[CAPTURE_NUM_BUFFERS] = {};
static UINT g_d3d12_captureWidth = 0;
static UINT g_d3d12_captureHeight = 0;
static UINT g_d3d12_captureRowPitch = 0;
static UINT g_d3d12_frameCounter = 0;
static UINT64 g_d3d12_fenceValueForReadback[CAPTURE_NUM_BUFFERS] = {};

/* D3D12 overlay / ImGui */
static bool g_d3d12_imguiInitialized = false;
#ifdef _WIN64
static ID3D12DescriptorHeap* g_d3d12_pSrvDescHeap = nullptr;
static UINT g_d3d12_srvDescriptorIncrement = 0;
static UINT g_d3d12_srvDescriptorCount = 0;
#endif

/* Worker thread sync */
static std::condition_variable g_workerCv;
static std::mutex g_workerMutex;
static int g_pendingApi = 0; /* 11 or 12 */
static ID3D11Query* g_pendingD3D11Query = nullptr;
static ID3D11Texture2D* g_pendingD3D11Staging = nullptr;
static UINT g_pendingWidth = 0;
static UINT g_pendingHeight = 0;
static UINT64 g_pendingD3D12FenceValue = 0;
static ID3D12Resource* g_pendingD3D12Readback = nullptr;
static UINT g_pendingD3D12RowPitch = 0;

/* Overlay toggle */
static bool g_showOverlay = true;

/* Forward declarations */
static void EvtHydraHookGameHooked(PHYDRAHOOK_ENGINE EngineHandle, const HYDRAHOOK_D3D_VERSION GameVersion);
static void EvtHydraHookD3D11PrePresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, PHYDRAHOOK_EVT_PRE_EXTENSION Extension);
static void EvtHydraHookD3D11PreResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, PHYDRAHOOK_EVT_PRE_EXTENSION Extension);
static void EvtHydraHookD3D11PostResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, PHYDRAHOOK_EVT_POST_EXTENSION Extension);
static void EvtHydraHookD3D12PrePresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, PHYDRAHOOK_EVT_PRE_EXTENSION Extension);
static void EvtHydraHookD3D12PreResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, PHYDRAHOOK_EVT_PRE_EXTENSION Extension);
static void EvtHydraHookD3D12PostResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, PHYDRAHOOK_EVT_POST_EXTENSION Extension);

static void D3D11_CreateCaptureResources(ID3D11Device* pDevice, UINT width, UINT height);
static void D3D11_ReleaseCaptureResources();
static void D3D12_CleanupOverlayResources();
static void D3D12_CleanupInitResources();
static bool D3D12_CreateOverlayResources(IDXGISwapChain* pSwapChain);
static void D3D12_CreateCaptureResources(UINT width, UINT height, DXGI_FORMAT format);
static void D3D12_ReleaseCaptureResources();

static void RenderOverlay(float displayWidth, float displayHeight);
static void TOGGLE_STATE(int key, bool& toggle);

typedef LRESULT(CALLBACK* WndProc_t)(HWND, UINT, WPARAM, LPARAM);
static WndProc_t g_originalWndProc = nullptr;

#ifndef IMGUI_IMPL_API
#define IMGUI_IMPL_API
#endif
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return 0;
	return g_originalWndProc ? CallWindowProcW(g_originalWndProc, hWnd, msg, wParam, lParam) : DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void HookWindowProc(HWND hWnd)
{
	if (!hWnd || g_originalWndProc)
		return;
	WNDPROC current = (WNDPROC)GetWindowLongPtrW(hWnd, GWLP_WNDPROC);
	if (current == OverlayWndProc)
		return;
	g_originalWndProc = (WndProc_t)current;
	SetWindowLongPtrW(hWnd, GWLP_WNDPROC, (LONG_PTR)OverlayWndProc);
}

#ifdef _WIN64
#include <imgui_impl_dx12.h>
#endif

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
		g_workerRunning = false;
		g_workerCv.notify_all();
		if (g_workerThread)
		{
			if (g_workerThread->joinable())
				g_workerThread->join();
			delete g_workerThread;
			g_workerThread = nullptr;
		}
		(void)HydraHookEngineDestroy(static_cast<HMODULE>(hInstance));
		break;
	default:
		break;
	}

	return TRUE;
}

static void TOGGLE_STATE(int key, bool& toggle)
{
	static bool pressedPast = false, pressedNow = false;
	if (GetAsyncKeyState(key) & 0x8000)
		pressedNow = true;
	else
	{
		pressedPast = false;
		pressedNow = false;
	}
	if (!pressedPast && pressedNow)
	{
		toggle = !toggle;
		pressedPast = true;
	}
}

/* ---------------------------------------------------------------------------
 * OpenCV perception pipeline (runs in worker thread)
 * --------------------------------------------------------------------------- */
static void RunPerceptionPipeline(cv::Mat& frame)
{
	const int minFeatures = 8;
	const int maxPoseTrailLen = 100;

	static cv::Mat prevGray, currGray;
	static std::vector<cv::Point2f> prevPts, currPts;
	static cv::Ptr<cv::ORB> orb = cv::ORB::create(500);
	static std::vector<cv::Vec3f> poseTrail;
	static bool needReinit = true;

	cv::Mat gray;
	if (frame.channels() == 3)
		cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
	else
		gray = frame;

	currGray = gray;
	PerceptionResults out;
	out.valid = false;

	if (needReinit || prevPts.size() < (size_t)minFeatures)
	{
		std::vector<cv::KeyPoint> kps;
		cv::Mat desc;
		orb->detectAndCompute(currGray, cv::noArray(), kps, desc);
		prevPts.clear();
		for (const auto& kp : kps)
			prevPts.push_back(kp.pt);
		prevGray = currGray.clone();
		needReinit = false;
		out.prevPts = prevPts;
		out.currPts = prevPts;
		out.featureCount = (int)prevPts.size();
		out.valid = prevPts.size() >= (size_t)minFeatures;
	}
	else
	{
		std::vector<uchar> status;
		std::vector<float> err;
		cv::calcOpticalFlowPyrLK(prevGray, currGray, prevPts, currPts, status, err,
			cv::Size(21, 21), 3,
			cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 30, 0.01));

		std::vector<cv::Point2f> goodPrev, goodCurr;
		for (size_t i = 0; i < status.size(); i++)
		{
			if (status[i])
			{
				goodPrev.push_back(prevPts[i]);
				goodCurr.push_back(currPts[i]);
			}
		}

		if (goodPrev.size() < (size_t)minFeatures)
		{
			needReinit = true;
			out.prevPts = prevPts;
			out.currPts = currPts;
			out.featureCount = (int)currPts.size();
			out.valid = false;
		}
		else
		{
			const int w = frame.cols;
			const int h = frame.rows;
			double fx = (double)w;
			double fy = (double)w;
			double cx = w / 2.0;
			double cy = h / 2.0;
			cv::Mat K = (cv::Mat_<double>(3, 3) << fx, 0, cx, 0, fy, cy, 0, 0, 1);

			cv::Mat E, R, t;
			std::vector<int> inlierMask;
			E = cv::findEssentialMat(goodPrev, goodCurr, K, cv::RANSAC, 0.999, 1.0, inlierMask);

			int inliers = 0;
			for (int m : inlierMask)
				if (m) inliers++;

			if (inliers >= minFeatures)
			{
				std::vector<cv::Point2f> inlierPrev, inlierCurr;
				for (size_t i = 0; i < inlierMask.size(); i++)
				{
					if (inlierMask[i])
					{
						inlierPrev.push_back(goodPrev[i]);
						inlierCurr.push_back(goodCurr[i]);
					}
				}
				cv::recoverPose(E, inlierPrev, inlierCurr, K, R, t);

				poseTrail.push_back(cv::Vec3f((float)t.at<double>(0), (float)t.at<double>(1), (float)t.at<double>(2)));
				if (poseTrail.size() > (size_t)maxPoseTrailLen)
					poseTrail.erase(poseTrail.begin());

				out.R = R;
				out.t = t;
				out.poseTrail = poseTrail;
				out.inliers = inliers;
			}

			prevPts = currPts;
			prevGray = currGray.clone();
			out.prevPts = goodPrev;
			out.currPts = goodCurr;
			out.featureCount = (int)goodCurr.size();
			out.valid = true;
		}
	}

	{
		std::lock_guard<std::mutex> lock(g_resultsMutex);
		g_results = out;
	}
}

/* ---------------------------------------------------------------------------
 * Worker thread: wait for capture, map, convert to cv::Mat, run pipeline
 * --------------------------------------------------------------------------- */
static void WorkerThreadProc()
{
	while (g_workerRunning)
	{
		int api = 0;
		ID3D11Query* pD3D11Query = nullptr;
		ID3D11Texture2D* pD3D11Staging = nullptr;
		UINT64 d3d12FenceVal = 0;
		ID3D12Resource* pD3D12Readback = nullptr;
		UINT width = 0, height = 0;
		UINT rowPitch = 0;

		{
			std::unique_lock<std::mutex> lock(g_workerMutex);
			g_workerCv.wait(lock, [] { return !g_workerRunning || g_pendingApi != 0; });
			if (!g_workerRunning)
				break;

			api = g_pendingApi;
			width = g_pendingWidth;
			height = g_pendingHeight;
			rowPitch = width * 4;
			if (api == 12)
				rowPitch = g_pendingD3D12RowPitch;
			g_pendingApi = 0;

			if (api == 11)
			{
				pD3D11Query = g_pendingD3D11Query;
				pD3D11Staging = g_pendingD3D11Staging;
			}
			else if (api == 12)
			{
				d3d12FenceVal = g_pendingD3D12FenceValue;
				pD3D12Readback = g_pendingD3D12Readback;
			}
		}

		if (width == 0 || height == 0)
			continue;

		cv::Mat frame;

		if (api == 11 && pD3D11Query && pD3D11Staging)
		{
			ID3D11Device* pDev = nullptr;
			ID3D11DeviceContext* pCtx = nullptr;
			pD3D11Staging->GetDevice(&pDev);
			if (pDev)
				pDev->GetImmediateContext(&pCtx);
			if (pCtx)
			{
				while (pCtx->GetData(pD3D11Query, nullptr, 0, 0) != S_OK)
				{
					if (!g_workerRunning)
						break;
					Sleep(0);
				}
			}
			if (!g_workerRunning)
				break;

			D3D11_MAPPED_SUBRESOURCE mapped = {};
			if (pDev && pCtx && SUCCEEDED(pCtx->Map(pD3D11Staging, 0, D3D11_MAP_READ, 0, &mapped)))
			{
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
				pCtx->Unmap(pD3D11Staging, 0);
			}
			if (pCtx) pCtx->Release();
			if (pDev) pDev->Release();
		}
#ifdef _WIN64
		else if (api == 12 && pD3D12Readback && g_d3d12_pFence && g_d3d12_hFenceEvent)
		{
			g_d3d12_pFence->SetEventOnCompletion(d3d12FenceVal, g_d3d12_hFenceEvent);
			WaitForSingleObject(g_d3d12_hFenceEvent, INFINITE);

			D3D12_RANGE readRange = { 0, 0 };
			void* pData = nullptr;
			if (SUCCEEDED(pD3D12Readback->Map(0, &readRange, &pData)))
			{
				const UINT rp = (rowPitch != 0) ? rowPitch : (width * 4);
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
		}
#endif
		if (!frame.empty())
			RunPerceptionPipeline(frame);
	}
}

static void RenderOverlay(float displayWidth, float displayHeight)
{
	ImDrawList* draw = ImGui::GetBackgroundDrawList();
	if (!draw)
		return;

	PerceptionResults res;
	{
		std::lock_guard<std::mutex> lock(g_resultsMutex);
		res = g_results;
	}

	if (!res.valid || res.currPts.empty())
		return;

	const ImU32 colPoint = IM_COL32(0, 255, 0, 255);
	const ImU32 colVector = IM_COL32(255, 200, 0, 200);
	const ImU32 colTrail = IM_COL32(255, 100, 255, 200);

	for (const auto& pt : res.currPts)
		draw->AddCircle(ImVec2(pt.x, pt.y), 3.0f, colPoint, 0, 2.0f);

	for (size_t i = 0; i < res.prevPts.size() && i < res.currPts.size(); i++)
		draw->AddLine(ImVec2(res.prevPts[i].x, res.prevPts[i].y),
			ImVec2(res.currPts[i].x, res.currPts[i].y), colVector, 1.5f);

	if (res.poseTrail.size() >= 2)
	{
		float scale = 50.0f;
		float ox = displayWidth * 0.5f;
		float oy = displayHeight * 0.8f;
		for (size_t i = 1; i < res.poseTrail.size(); i++)
		{
			const auto& a = res.poseTrail[i - 1];
			const auto& b = res.poseTrail[i];
			draw->AddLine(
				ImVec2(ox + a[0] * scale, oy - a[2] * scale),
				ImVec2(ox + b[0] * scale, oy - b[2] * scale),
				colTrail, 2.0f);
		}
	}
}

static void EvtHydraHookGameHooked(
	PHYDRAHOOK_ENGINE EngineHandle,
	const HYDRAHOOK_D3D_VERSION GameVersion
)
{
	HydraHookEngineLogInfo("HydraHook-OpenCV: Loading");

	static std::once_flag workerOnce;
	std::call_once(workerOnce, []() {
		g_workerThread = new std::thread(WorkerThreadProc);
	});

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

static void D3D11_CreateCaptureResources(ID3D11Device* pDevice, UINT width, UINT height)
{
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
		if (g_d3d11_staging[i])
		{
			g_d3d11_staging[i]->Release();
			g_d3d11_staging[i] = nullptr;
		}
		if (g_d3d11_query[i])
		{
			g_d3d11_query[i]->Release();
			g_d3d11_query[i] = nullptr;
		}
		pDevice->CreateTexture2D(&td, nullptr, &g_d3d11_staging[i]);
		pDevice->CreateQuery(&qd, &g_d3d11_query[i]);
	}
	g_d3d11_captureWidth = width;
	g_d3d11_captureHeight = height;
}

static void D3D11_ReleaseCaptureResources()
{
	for (UINT i = 0; i < CAPTURE_NUM_BUFFERS; i++)
	{
		if (g_d3d11_staging[i]) { g_d3d11_staging[i]->Release(); g_d3d11_staging[i] = nullptr; }
		if (g_d3d11_query[i]) { g_d3d11_query[i]->Release(); g_d3d11_query[i] = nullptr; }
	}
	g_d3d11_captureWidth = 0;
	g_d3d11_captureHeight = 0;
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
	(void)Extension;

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

	if (!g_d3d11_imguiInitialized)
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::StyleColorsDark();
		ImGui_ImplWin32_Init(sd.OutputWindow);
		ImGui_ImplDX11_Init(pDevice, pContext);
		HookWindowProc(sd.OutputWindow);
		g_d3d11_imguiInitialized = true;
		HydraHookEngineLogInfo("HydraHook-OpenCV: ImGui D3D11 initialized");
	}

	if (g_d3d11_captureWidth != width || g_d3d11_captureHeight != height)
		D3D11_CreateCaptureResources(pDevice, width, height);

	if (!g_d3d11_mainRTV)
	{
		HRESULT hr = pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_d3d11_mainRTV);
		if (FAILED(hr) || !g_d3d11_mainRTV)
		{
			pBackBuffer->Release();
			pDevice->Release();
			pContext->Release();
			return;
		}
	}

	/* Queue capture (non-blocking) */
	const UINT bufIdx = g_d3d11_frameCounter % CAPTURE_NUM_BUFFERS;
	pContext->CopyResource(g_d3d11_staging[bufIdx], pBackBuffer);
	pContext->End(g_d3d11_query[bufIdx]);

	if (g_d3d11_frameCounter >= 1)
	{
		const UINT prevIdx = (g_d3d11_frameCounter - 1) % CAPTURE_NUM_BUFFERS;
		{
			std::lock_guard<std::mutex> lock(g_workerMutex);
			g_pendingApi = 11;
			g_pendingD3D11Query = g_d3d11_query[prevIdx];
			g_pendingD3D11Staging = g_d3d11_staging[prevIdx];
			g_pendingWidth = g_d3d11_captureWidth;
			g_pendingHeight = g_d3d11_captureHeight;
		}
		g_workerCv.notify_one();
	}
	g_d3d11_frameCounter++;

	pBackBuffer->Release();

	/* Overlay: no clear - draw on top of game */
	pContext->OMSetRenderTargets(1, &g_d3d11_mainRTV, nullptr);

	TOGGLE_STATE(VK_F12, g_showOverlay);
	if (g_showOverlay)
	{
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		RenderOverlay((float)width, (float)height);

		ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
		ImGui::Begin("Perception HUD", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
		{
			std::lock_guard<std::mutex> lock(g_resultsMutex);
			ImGui::Text("Features: %d", g_results.featureCount);
			ImGui::Text("Inliers: %d", g_results.inliers);
			if (g_results.R.data)
			{
				ImGui::Text("R: [%.2f %.2f %.2f]", g_results.R.at<double>(0, 0), g_results.R.at<double>(0, 1), g_results.R.at<double>(0, 2));
				if (g_results.t.data)
					ImGui::Text("t: [%.3f %.3f %.3f]", g_results.t.at<double>(0), g_results.t.at<double>(1), g_results.t.at<double>(2));
			}
			ImGui::Text("Trail: %zu pts", g_results.poseTrail.size());
		}
		ImGui::End();

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
	(void)Extension;

	if (g_d3d11_mainRTV)
	{
		g_d3d11_mainRTV->Release();
		g_d3d11_mainRTV = nullptr;
	}
	D3D11_ReleaseCaptureResources();
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

static void D3D12_ReleaseCaptureResources()
{
	for (UINT i = 0; i < CAPTURE_NUM_BUFFERS; i++)
	{
		if (g_d3d12_readback[i])
		{
			g_d3d12_readback[i]->Release();
			g_d3d12_readback[i] = nullptr;
		}
	}
	g_d3d12_captureWidth = 0;
	g_d3d12_captureHeight = 0;
}

static void D3D12_CreateCaptureResources(UINT width, UINT height, DXGI_FORMAT format)
{
	D3D12_ReleaseCaptureResources();
	if (!g_d3d12_pDevice)
		return;

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
	g_d3d12_pDevice->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, &rowSize, &totalBytes);

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
		g_d3d12_pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_d3d12_readback[i]));

	g_d3d12_captureWidth = width;
	g_d3d12_captureHeight = height;
	g_d3d12_captureRowPitch = (UINT)footprint.Footprint.RowPitch;
}

static void D3D12_CleanupInitResources()
{
	D3D12_CleanupOverlayResources();
	D3D12_ReleaseCaptureResources();
#ifdef _WIN64
	if (g_d3d12_pSrvDescHeap) { g_d3d12_pSrvDescHeap->Release(); g_d3d12_pSrvDescHeap = nullptr; }
#endif
	if (g_d3d12_hFenceEvent) { CloseHandle(g_d3d12_hFenceEvent); g_d3d12_hFenceEvent = nullptr; }
	if (g_d3d12_pFence) { g_d3d12_pFence->Release(); g_d3d12_pFence = nullptr; }
	if (g_d3d12_pCommandList) { g_d3d12_pCommandList->Release(); g_d3d12_pCommandList = nullptr; }
	if (g_d3d12_pCommandAllocator) { g_d3d12_pCommandAllocator->Release(); g_d3d12_pCommandAllocator = nullptr; }
	if (g_d3d12_pCommandQueue) { g_d3d12_pCommandQueue->Release(); g_d3d12_pCommandQueue = nullptr; }
	if (g_d3d12_pDevice) { g_d3d12_pDevice->Release(); g_d3d12_pDevice = nullptr; }
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

#ifdef _WIN64
static constexpr UINT D3D12_SRV_HEAP_SIZE = 64;

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

#ifdef _WIN64
		D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
		srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvDesc.NumDescriptors = D3D12_SRV_HEAP_SIZE;
		srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(g_d3d12_pDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_d3d12_pSrvDescHeap))))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: Couldn't create D3D12 SRV descriptor heap");
			D3D12_CleanupInitResources();
			return;
		}
		g_d3d12_srvDescriptorIncrement = g_d3d12_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
#endif

		HydraHookEngineLogInfo("HydraHook-OpenCV: D3D12 initialized");
		initialized = true;
	}

	if (!initialized)
		return;

	DXGI_SWAP_CHAIN_DESC sd = {};
	pSwapChain->GetDesc(&sd);
	const UINT width = sd.BufferDesc.Width;
	const UINT height = sd.BufferDesc.Height;

	if (g_d3d12_captureWidth != width || g_d3d12_captureHeight != height)
		D3D12_CreateCaptureResources(width, height, sd.BufferDesc.Format);

#ifdef _WIN64
	if (!g_d3d12_imguiInitialized)
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::StyleColorsDark();
		ImGui_ImplWin32_Init(sd.OutputWindow);

		ImGui_ImplDX12_InitInfo initInfo = {};
		initInfo.Device = g_d3d12_pDevice;
		initInfo.CommandQueue = g_d3d12_pCommandQueue;
		initInfo.NumFramesInFlight = 2;
		initInfo.RTVFormat = sd.BufferDesc.Format;
		initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
		initInfo.SrvDescriptorHeap = g_d3d12_pSrvDescHeap;
		initInfo.SrvDescriptorAllocFn = D3D12_SrvDescriptorAlloc;
		initInfo.SrvDescriptorFreeFn = D3D12_SrvDescriptorFree;

		if (!ImGui_ImplDX12_Init(&initInfo))
		{
			HydraHookEngineLogError("HydraHook-OpenCV: ImGui_ImplDX12_Init failed");
			return;
		}
		HookWindowProc(sd.OutputWindow);
		g_d3d12_imguiInitialized = true;
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
	if (backBufferIdx >= g_d3d12_numBackBuffers)
		backBufferIdx = 0;

	ID3D12Resource* pBackBufferRes = g_d3d12_mainRenderTargetResource[backBufferIdx];

	g_d3d12_pCommandAllocator->Reset();
	g_d3d12_pCommandList->Reset(g_d3d12_pCommandAllocator, nullptr);

	/* 1. Transition to COPY_SOURCE, copy to readback */
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = pBackBufferRes;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	g_d3d12_pCommandList->ResourceBarrier(1, &barrier);

	const UINT bufIdx = g_d3d12_frameCounter % CAPTURE_NUM_BUFFERS;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT numRows = 0;
	UINT64 rowSize = 0;
	UINT64 totalBytes = 0;
	g_d3d12_pDevice->GetCopyableFootprints(&pBackBufferRes->GetDesc(), 0, 1, 0, &footprint, &numRows, &rowSize, &totalBytes);

	D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
	srcLoc.pResource = pBackBufferRes;
	srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	srcLoc.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
	dstLoc.pResource = g_d3d12_readback[bufIdx];
	dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dstLoc.PlacedFootprint = footprint;

	g_d3d12_pCommandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

	/* 2. Transition to RENDER_TARGET for overlay */
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	g_d3d12_pCommandList->ResourceBarrier(1, &barrier);

	g_d3d12_pCommandList->OMSetRenderTargets(1, &g_d3d12_mainRenderTargetDescriptor[backBufferIdx], FALSE, nullptr);

	/* 3. Draw overlay (no clear) */
	TOGGLE_STATE(VK_F12, g_showOverlay);
	if (g_showOverlay)
	{
#ifdef _WIN64
		g_d3d12_pCommandList->SetDescriptorHeaps(1, &g_d3d12_pSrvDescHeap);
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		RenderOverlay((float)width, (float)height);

		ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
		ImGui::Begin("Perception HUD", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
		{
			std::lock_guard<std::mutex> lock(g_resultsMutex);
			ImGui::Text("Features: %d", g_results.featureCount);
			ImGui::Text("Inliers: %d", g_results.inliers);
			if (g_results.R.data)
			{
				ImGui::Text("R: [%.2f %.2f %.2f]", g_results.R.at<double>(0, 0), g_results.R.at<double>(0, 1), g_results.R.at<double>(0, 2));
				if (g_results.t.data)
					ImGui::Text("t: [%.3f %.3f %.3f]", g_results.t.at<double>(0), g_results.t.at<double>(1), g_results.t.at<double>(2));
			}
			ImGui::Text("Trail: %zu pts", g_results.poseTrail.size());
		}
		ImGui::End();

		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_d3d12_pCommandList);
#endif
	}

	/* 4. Transition to PRESENT */
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	g_d3d12_pCommandList->ResourceBarrier(1, &barrier);
	g_d3d12_pCommandList->Close();

	g_d3d12_pCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_d3d12_pCommandList);
	g_d3d12_fenceLastSignaledValue++;
	g_d3d12_pCommandQueue->Signal(g_d3d12_pFence, g_d3d12_fenceLastSignaledValue);

	/* Signal worker with previous frame's readback - do NOT wait for GPU here */
	if (g_d3d12_frameCounter >= 1)
	{
		const UINT prevIdx = (g_d3d12_frameCounter - 1) % CAPTURE_NUM_BUFFERS;
		{
			std::lock_guard<std::mutex> lock(g_workerMutex);
			g_pendingApi = 12;
			g_pendingD3D12FenceValue = g_d3d12_fenceValueForReadback[prevIdx];
			g_pendingD3D12Readback = g_d3d12_readback[prevIdx];
			g_pendingD3D12RowPitch = g_d3d12_captureRowPitch;
			g_pendingWidth = g_d3d12_captureWidth;
			g_pendingHeight = g_d3d12_captureHeight;
		}
		g_workerCv.notify_one();
	}
	g_d3d12_fenceValueForReadback[bufIdx] = g_d3d12_fenceLastSignaledValue;
	g_d3d12_frameCounter++;
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
	(void)Extension;

	/* Wait for GPU before releasing resources */
	if (g_d3d12_pFence && g_d3d12_hFenceEvent)
	{
		g_d3d12_pFence->SetEventOnCompletion(g_d3d12_fenceLastSignaledValue, g_d3d12_hFenceEvent);
		WaitForSingleObject(g_d3d12_hFenceEvent, INFINITE);
	}
#ifdef _WIN64
	if (g_d3d12_imguiInitialized)
	{
		ImGui_ImplDX12_InvalidateDeviceObjects();
		g_d3d12_imguiInitialized = false;
	}
	g_d3d12_srvDescriptorCount = 0;
#endif
	D3D12_CleanupOverlayResources();
	D3D12_ReleaseCaptureResources();
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
	(void)Extension;

	if (!D3D12_CreateOverlayResources(pSwapChain))
		HydraHookEngineLogError("HydraHook-OpenCV: D3D12_CreateOverlayResources failed after resize");
#ifdef _WIN64
	if (g_d3d12_imguiInitialized == false && g_d3d12_pDevice && g_d3d12_pSrvDescHeap)
	{
		DXGI_SWAP_CHAIN_DESC sd = {};
		pSwapChain->GetDesc(&sd);
		ImGui_ImplDX12_CreateDeviceObjects();
		g_d3d12_imguiInitialized = true;
	}
#endif
}

#pragma endregion
