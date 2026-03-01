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

#include "Direct3D11.h"
#include <dxgi1_4.h>
#include "DXGI.h"
#include "Exceptions.hpp"

using namespace HydraHook::Core::Exceptions;

Direct3D11Hooking::Direct3D11::Direct3D11() :
	pd3dDevice(nullptr), pd3dDeviceContext(nullptr), pSwapChain(nullptr)
{
	temp_window = std::make_unique<Window>("TempDirect3D11OverlayWindow");

	const auto hModDXGI = GetModuleHandle("DXGI.dll");
	const auto hModD3D11 = GetModuleHandle("D3D11.dll");

	if (hModDXGI == nullptr)
	{
		throw ModuleNotFoundException("Could not get handle to DXGI.dll");
	}

	if (hModD3D11 == nullptr)
	{
		throw ModuleNotFoundException("Could not get handle to D3D11.dll");
	}

	// Setup swap chain
	DXGI_SWAP_CHAIN_DESC sd;
	{
		ZeroMemory(&sd, sizeof(sd));
		sd.BufferCount = 2;
		sd.BufferDesc.Width = 0;
		sd.BufferDesc.Height = 0;
		sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferDesc.RefreshRate.Numerator = 60;
		sd.BufferDesc.RefreshRate.Denominator = 1;
		sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow = temp_window->windowHandle();
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		sd.Windowed = TRUE;
		sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	}

	UINT createDeviceFlags = 0;
	D3D_FEATURE_LEVEL featureLevel;
	// Note: requesting lower feature level in case of missing hardware support
	const D3D_FEATURE_LEVEL featureLevelArray[3] = {
		D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_11_0
	};

	const auto hD3D11CreateDeviceAndSwapChain = static_cast<LPVOID>(GetProcAddress(
		hModD3D11,
		"D3D11CreateDeviceAndSwapChain"
	));
	if (hD3D11CreateDeviceAndSwapChain == nullptr)
	{
		throw ProcAddressNotFoundException("Could not get handle to D3D11CreateDeviceAndSwapChain");
	}

	const auto hr11 = static_cast<HRESULT(WINAPI *)(
		IDXGIAdapter*,
		D3D_DRIVER_TYPE,
		HMODULE,
		UINT,
		const D3D_FEATURE_LEVEL*,
		UINT,
		UINT,
		const DXGI_SWAP_CHAIN_DESC*,
		IDXGISwapChain**,
		ID3D11Device**,
		D3D_FEATURE_LEVEL*,
		ID3D11DeviceContext**)>(hD3D11CreateDeviceAndSwapChain)(
		nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		createDeviceFlags,
		featureLevelArray,
		1,
		D3D11_SDK_VERSION,
		&sd,
		&pSwapChain,
		&pd3dDevice,
		&featureLevel,
		&pd3dDeviceContext);

	if (FAILED(hr11))
	{
		throw DXAPIException("Could not create D3D11 device", hr11);
	}
}

std::vector<size_t> Direct3D11Hooking::Direct3D11::vtable() const
{
	IDXGISwapChain3* pSC3 = nullptr;
	if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&pSC3))))
	{
		pSC3->Release();
		return std::vector<size_t>(*reinterpret_cast<size_t**>(pSwapChain),
		                           *reinterpret_cast<size_t**>(pSwapChain) + DXGIHooking::DXGI::SwapChain3VTableElements);
	}
	return std::vector<size_t>(*reinterpret_cast<size_t**>(pSwapChain),
	                           *reinterpret_cast<size_t**>(pSwapChain) + DXGIHooking::DXGI::SwapChainVTableElements);
}

Direct3D11Hooking::Direct3D11::~Direct3D11()
{
	if (pSwapChain)
		pSwapChain->Release();

	if (pd3dDevice)
		pd3dDevice->Release();

	if (pd3dDeviceContext)
		pd3dDeviceContext->Release();
}
