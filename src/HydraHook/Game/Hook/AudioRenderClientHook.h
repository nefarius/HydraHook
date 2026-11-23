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

/**
 * @file AudioRenderClientHook.h
 * @brief IAudioRenderClient vtable hook for Core Audio interception.
 * @internal
 */

#pragma once

#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <vector>

namespace CoreAudioHooking
{
    enum AudioRenderClientVTbl : short
    {
        // IUnknown
        QueryInterface = 0,
        AddRef = 1,
        Release = 2,

        // IAudioRenderClient
        GetBuffer = 3,
        ReleaseBuffer = 4
    };

    class AudioRenderClientHook
    {
        IMMDeviceEnumerator *enumerator{};
        IMMDevice *device{};
        IAudioClient *client{};
        WAVEFORMATEX *pwfx{};
        IAudioRenderClient *arc{};
        bool comInitialized_{};

    public:
        AudioRenderClientHook();
        ~AudioRenderClientHook();

        static const int VTableElements = 5;

        std::vector<size_t> vtable() const;
    };
};
