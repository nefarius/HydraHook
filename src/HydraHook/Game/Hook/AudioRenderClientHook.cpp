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


#include "AudioRenderClientHook.h"
#include "Exceptions.hpp"

#include <wrl/client.h>

using namespace HydraHook::Core::Exceptions;
using Microsoft::WRL::ComPtr;

CoreAudioHooking::AudioRenderClientHook::AudioRenderClientHook()
{
	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	comInitialized_ = (coInit == S_OK || coInit == S_FALSE);

	ComPtr<IMMDeviceEnumerator> enumeratorPtr;
	HRESULT hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator),
		nullptr,
		CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator),
		reinterpret_cast<void**>(enumeratorPtr.GetAddressOf())
	);
	if (FAILED(hr))
	{
		throw ARCException("CoCreateInstance failed", hr);
	}

	ComPtr<IMMDevice> devicePtr;
	hr = enumeratorPtr->GetDefaultAudioEndpoint(eRender, eConsole, devicePtr.GetAddressOf());
	if (FAILED(hr))
	{
		throw ARCException("Failed to get default audio endpoint", hr);
	}

	ComPtr<IAudioClient> clientPtr;
	hr = devicePtr->Activate(
		__uuidof(IAudioClient),
		CLSCTX_ALL,
		nullptr,
		reinterpret_cast<void**>(clientPtr.GetAddressOf())
	);
	if (FAILED(hr))
	{
		throw ARCException("Failed to activate IAudioClient instance", hr);
	}

	hr = clientPtr->GetMixFormat(&pwfx);
	if (FAILED(hr) || !pwfx)
	{
		throw ARCException("Failed to query the mix format", FAILED(hr) ? hr : E_POINTER);
	}

	auto initializeAt = [&](DWORD sampleRate) -> HRESULT
	{
		pwfx->nSamplesPerSec = sampleRate;
		pwfx->nAvgBytesPerSec = sampleRate * pwfx->nBlockAlign;
		return clientPtr->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			0,
			10 * 1000 * 1000,
			0,
			pwfx,
			nullptr
		);
	};

	hr = clientPtr->Initialize(
		AUDCLNT_SHAREMODE_SHARED,
		0,
		10 * 1000 * 1000,
		0,
		pwfx,
		nullptr
	);
	if (hr != S_OK)
	{
		hr = initializeAt(48000);
		if (hr != S_OK)
			hr = initializeAt(44100);
	}
	if (FAILED(hr))
	{
		throw ARCException("Failed to initialize IAudioClient instance", hr);
	}

	ComPtr<IAudioRenderClient> arcPtr;
	hr = clientPtr->GetService(
		__uuidof(IAudioRenderClient),
		reinterpret_cast<void**>(arcPtr.GetAddressOf())
	);
	if (FAILED(hr))
	{
		throw ARCException("Failed to request IAudioRenderClient service", hr);
	}

	enumerator = enumeratorPtr.Detach();
	device = devicePtr.Detach();
	client = clientPtr.Detach();
	arc = arcPtr.Detach();
}

CoreAudioHooking::AudioRenderClientHook::~AudioRenderClientHook()
{
	if (arc)
		arc->Release();
	if (client)
		client->Release();
	if (device)
		device->Release();
	if (enumerator)
		enumerator->Release();
	if (pwfx)
		CoTaskMemFree(pwfx);
	if (comInitialized_)
		CoUninitialize();
}

std::vector<size_t> CoreAudioHooking::AudioRenderClientHook::vtable() const
{
	return std::vector<size_t>(*reinterpret_cast<size_t**>(arc), *reinterpret_cast<size_t**>(arc) + VTableElements);
}
