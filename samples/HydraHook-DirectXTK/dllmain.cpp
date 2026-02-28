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
#include <HydraHook/Engine/HydraHookCore.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include <directxtk/SpriteBatch.h>
#include <directxtk/SpriteFont.h>
#include <directxtk/CommonStates.h>

using namespace DirectX;

static HMODULE g_hModule = nullptr;

static constexpr float MARQUEE_SPEED_PX_PER_SEC = 80.0f;
static constexpr float MARQUEE_Y = 60.0f;
static constexpr float FPS_MARGIN = 15.0f;
static constexpr double FPS_SMOOTH_ALPHA = 0.1;

typedef struct _DX11_TEXT_CTX
{
	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	std::unique_ptr<SpriteBatch> spriteBatch;
	std::unique_ptr<SpriteFont> spriteFont;
	std::unique_ptr<CommonStates> commonStates;
	LARGE_INTEGER marqueeStartTime{};
	LARGE_INTEGER fpsLastFrameTime{};
	double fpsSmoothed = 60.0;
	bool fpsFirstFrame = true;
} DX11_TEXT_CTX, *PDX11_TEXT_CTX;

EVT_HYDRAHOOK_GAME_HOOKED EvtHydraHookGameHooked;
EVT_HYDRAHOOK_D3D11_PRE_PRESENT EvtHydraHookD3D11PrePresent;
EVT_HYDRAHOOK_GAME_UNHOOKED EvtHydraHookGamePostUnhooked;

static std::wstring GetFontPath()
{
	wchar_t path[MAX_PATH];
	DWORD len = GetModuleFileNameW(g_hModule, path, MAX_PATH);
	if (len == 0)
		return L"";
	// Detect truncation: GetModuleFileNameW returns len written; if buffer too small,
	// it returns MAX_PATH and sets ERROR_INSUFFICIENT_BUFFER
	if (len >= MAX_PATH && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
		return L"";

	std::wstring fontPath(path);
	const size_t lastSlash = fontPath.find_last_of(L"\\/");
	if (lastSlash != std::wstring::npos)
		fontPath = fontPath.substr(0, lastSlash + 1);
	fontPath += L"Arial.spritefont";
	return fontPath;
}

/**
 * \fn	BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
 *
 * \brief	Your typical DLL entry point function. We're not doing much here since a special
 * 			initialization routine gets called upon getting loaded by HydraHook.
 *
 * \author	Benjamin "Nefarius" Höglinger
 * \date	05.05.2018
 *
 * \param	hInstance 	The instance.
 * \param	dwReason  	The reason.
 * \param	parameter3	The third parameter.
 *
 * \returns	A WINAPI.
 */
BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
{
	//
	// We don't need to get notified in thread attach- or detachments
	// 
	DisableThreadLibraryCalls(static_cast<HMODULE>(hInstance));

	g_hModule = static_cast<HMODULE>(hInstance);

	HYDRAHOOK_ENGINE_CONFIG cfg;
	HYDRAHOOK_ENGINE_CONFIG_INIT(&cfg);

	// Only attempt to detect and hook D3D11
	cfg.Direct3D.HookDirect3D11 = TRUE;
	// Called once game as been hooked
	cfg.EvtHydraHookGameHooked = EvtHydraHookGameHooked;
	// Called after hooks have been removed
	cfg.EvtHydraHookGamePostUnhook = EvtHydraHookGamePostUnhooked;
	// Crash dump logic
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
		g_hModule = nullptr;

		break;
	default:
		break;
	}

	return TRUE;
}

//
// Essential game functions successfully hooked, do further bootstrapping here
// 
void EvtHydraHookGameHooked(
	PHYDRAHOOK_ENGINE EngineHandle,
	const HYDRAHOOK_D3D_VERSION GameVersion
)
{
	//
	// At this stage we assume D3D11
	// 
	assert(GameVersion == HydraHookDirect3DVersion11);

	PDX11_TEXT_CTX pCtx = nullptr;

	//
	// Allocate context memory
	// 
	if (HydraHookEngineAllocCustomContext(
		EngineHandle,
		(PVOID*)&pCtx,
		sizeof(DX11_TEXT_CTX)
	) != HYDRAHOOK_ERROR_NONE || !pCtx)
	{
		HydraHookEngineLogError("Failed to allocate custom context for DirectXTK sample");
		return;
	}

	// Placement new to construct the struct (it has non-trivial members)
	new(pCtx) DX11_TEXT_CTX();

	HYDRAHOOK_D3D11_EVENT_CALLBACKS d3d11;
	HYDRAHOOK_D3D11_EVENT_CALLBACKS_INIT(&d3d11);
	d3d11.EvtHydraHookD3D11PrePresent = EvtHydraHookD3D11PrePresent;

	// Begin invoking render hook callbacks
	HydraHookEngineSetD3D11EventCallbacks(EngineHandle, &d3d11);
}

//
// Game unloading, hooks are removed
// 
void EvtHydraHookGamePostUnhooked(PHYDRAHOOK_ENGINE EngineHandle)
{
	auto pCtx = PDX11_TEXT_CTX(HydraHookEngineGetCustomContext(EngineHandle));
	if (pCtx)
		pCtx->~DX11_TEXT_CTX();
}

//
// Present is about to get called
// 
void EvtHydraHookD3D11PrePresent(
	IDXGISwapChain* pSwapChain,
	UINT SyncInterval,
	UINT Flags,
	PHYDRAHOOK_EVT_PRE_EXTENSION Extension
)
{
	UNREFERENCED_PARAMETER(SyncInterval);
	UNREFERENCED_PARAMETER(Flags);

	const auto pCtx = PDX11_TEXT_CTX(Extension->Context);
	ID3D11Device* pDeviceTmp = nullptr;

	if (FAILED(D3D11_DEVICE_FROM_SWAPCHAIN(pSwapChain, &pDeviceTmp)))
	{
		HydraHookEngineLogError("Failed to get device pointer from swapchain");
		return;
	}

	/*
	 * Swapchain associated Device and Context pointers can become invalid
	 * when the host process destroys and re-creates them (like RetroArch
	 * does when switching cores) so compare to ones grabbed earlier and
	 * re-request both if necessary.
	 */
	bool devChanged = (pCtx->dev != pDeviceTmp);
	if (devChanged)
	{
		pCtx->spriteBatch.reset();
		pCtx->spriteFont.reset();
		pCtx->commonStates.reset();

		D3D11_DEVICE_IMMEDIATE_CONTEXT_FROM_SWAPCHAIN(
			pSwapChain,
			&pCtx->dev,
			&pCtx->ctx
		);

		const std::wstring fontPath = GetFontPath();
		if (fontPath.empty() || GetFileAttributesW(fontPath.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			pDeviceTmp->Release();
			HydraHookEngineLogError(
				"Arial.spritefont not found next to DLL. Run MakeSpriteFont on arial.ttf and place output alongside the DLL.");
			return;
		}

		try
		{
			pCtx->commonStates = std::make_unique<CommonStates>(pCtx->dev);
			pCtx->spriteBatch = std::make_unique<SpriteBatch>(pCtx->ctx);
			pCtx->spriteFont = std::make_unique<SpriteFont>(pCtx->dev, fontPath.c_str());
		}
		catch (const std::exception& e)
		{
			pDeviceTmp->Release();
			HydraHookEngineLogError("Failed to create DirectXTK resources: %s", e.what());
			return;
		}
	}

	pDeviceTmp->Release();

	ID3D11Texture2D* pBackBuffer = nullptr;
	if (FAILED(D3D11_BACKBUFFER_FROM_SWAPCHAIN(pSwapChain, &pBackBuffer)))
		return;

	D3D11_TEXTURE2D_DESC bbDesc{};
	pBackBuffer->GetDesc(&bbDesc);

	LARGE_INTEGER qpcNow, qpcFreq;
	if (!QueryPerformanceCounter(&qpcNow) || !QueryPerformanceFrequency(&qpcFreq))
	{
		pBackBuffer->Release();
		return;
	}

	ID3D11RenderTargetView* pRTV = nullptr;
	HRESULT hr = pCtx->dev->CreateRenderTargetView(pBackBuffer, nullptr, &pRTV);
	pBackBuffer->Release();
	if (FAILED(hr) || !pRTV)
		return;

	pCtx->ctx->OMSetRenderTargets(1, &pRTV, nullptr);

	D3D11_VIEWPORT vp{};
	vp.Width = (float)bbDesc.Width;
	vp.Height = (float)bbDesc.Height;
	vp.MinDepth = 0.f;
	vp.MaxDepth = 1.f;
	pCtx->ctx->RSSetViewports(1, &vp);

	const float viewportWidth = static_cast<float>(bbDesc.Width);

	if (pCtx->fpsFirstFrame)
	{
		pCtx->marqueeStartTime = qpcNow;
		pCtx->fpsLastFrameTime = qpcNow;
		pCtx->fpsFirstFrame = false;
	}

	try
	{
		pCtx->spriteBatch->Begin(SpriteSortMode_Deferred, pCtx->commonStates->AlphaBlend());

		// Marquee: time-based scroll, FPS-independent
		{
			const wchar_t* marqueeText = L"Injected via HydraHook by Nefarius";
			XMVECTOR textSize = pCtx->spriteFont->MeasureString(marqueeText);
			const float textWidth = XMVectorGetX(textSize);
			const double elapsedSec = static_cast<double>(qpcNow.QuadPart - pCtx->marqueeStartTime.QuadPart) / static_cast<double>(qpcFreq.QuadPart);
			const float cycleLength = viewportWidth + textWidth;
			const float offset = static_cast<float>(std::fmod(elapsedSec * MARQUEE_SPEED_PX_PER_SEC, cycleLength));
			const float marqueeX = viewportWidth - offset;

			pCtx->spriteFont->DrawString(
				pCtx->spriteBatch.get(),
				marqueeText,
				XMFLOAT2(marqueeX, MARQUEE_Y),
				Colors::DeepPink,
				0.0f,
				XMFLOAT2(0, 0),
				1.0f
			);
		}

		// FPS counter: top-right corner
		{
			const double deltaSec = static_cast<double>(qpcNow.QuadPart - pCtx->fpsLastFrameTime.QuadPart) / static_cast<double>(qpcFreq.QuadPart);
			if (deltaSec > 0.0)
			{
				const double instantFps = 1.0 / deltaSec;
				pCtx->fpsSmoothed = pCtx->fpsSmoothed * (1.0 - FPS_SMOOTH_ALPHA) + instantFps * FPS_SMOOTH_ALPHA;
			}

			wchar_t fpsBuf[32];
			swprintf_s(fpsBuf, static_cast<size_t>(sizeof(fpsBuf) / sizeof(wchar_t)), L"FPS: %.1f", pCtx->fpsSmoothed);
			XMVECTOR fpsTextSize = pCtx->spriteFont->MeasureString(fpsBuf);
			const float fpsTextWidth = XMVectorGetX(fpsTextSize);
			const float fpsX = viewportWidth - FPS_MARGIN - fpsTextWidth;

			pCtx->spriteFont->DrawString(
				pCtx->spriteBatch.get(),
				fpsBuf,
				XMFLOAT2(fpsX, FPS_MARGIN),
				Colors::White,
				0.0f,
				XMFLOAT2(0, 0),
				1.0f
			);
		}

		pCtx->fpsLastFrameTime = qpcNow;
		pCtx->spriteBatch->End();
	}
	catch (const std::exception& e)
	{
		HydraHookEngineLogError("SpriteBatch failed: %s", e.what());
	}

	pRTV->Release();
}
