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
 * @file DXGI.h
 * @brief IDXGISwapChain vtable indices for D3D10/11/12 hooking.
 *
 * Defines vtable offsets for IDXGISwapChain and IDXGISwapChain1 methods
 * used when installing Present/ResizeBuffers/ResizeTarget hooks.
 *
 * @internal
 */

#pragma once

namespace DXGIHooking
{
    /** @brief IDXGISwapChain vtable indices. */
    enum DXGISwapChainVTbl : short
    {
        /* IUnknown */
        QueryInterface = 0,
        AddRef = 1,
        Release = 2,

        // IDXGIObject
        SetPrivateData = 3,
        SetPrivateDataInterface = 4,
        GetPrivateData = 5,
        GetParent = 6,

        // IDXGIDeviceSubObject
        GetDevice = 7,

        // IDXGISwapChain
        Present = 8,
        GetBuffer = 9,
        SetFullscreenState = 10,
        GetFullscreenState = 11,
        GetDesc = 12,
        ResizeBuffers = 13,
        ResizeTarget = 14,
        GetContainingOutput = 15,
        GetFrameStatistics = 16,
        GetLastPresentCount = 17
    };

    namespace DXGI1
    {
        /** @brief IDXGISwapChain1 vtable indices (extends IDXGISwapChain). */
        enum DXGISwapChain1VTbl : short
        {
            /* IUnknown */
            QueryInterface = 0,
            AddRef = 1,
            Release = 2,

            // IDXGIObject
            SetPrivateData = 3,
            SetPrivateDataInterface = 4,
            GetPrivateData = 5,
            GetParent = 6,

            // IDXGIDeviceSubObject
            GetDevice = 7,

            // IDXGISwapChain
            Present = 8,
            GetBuffer = 9,
            SetFullscreenState = 10,
            GetFullscreenState = 11,
            GetDesc = 12,
            ResizeBuffers = 13,
            ResizeTarget = 14,
            GetContainingOutput = 15,
            GetFrameStatistics = 16,
            GetLastPresentCount = 17,

            // IDXGISwapChain1
            GetDesc1 = 18,
            GetFullscreenDesc = 19,
            GetHwnd = 20,
            GetCoreWindow = 21,
            Present1 = 22,
            IsTemporaryMonoSupported = 23,
            GetRestrictToOutput = 24,
            SetBackgroundColor = 25,
            GetBackgroundColor = 26,
            SetRotation = 27,
            GetRotation = 28
        };
    }

    namespace DXGI2
    {
        /** @brief IDXGISwapChain2 vtable indices (extends IDXGISwapChain1). */
        enum DXGISwapChain2VTbl : short
        {
            SetSourceSize = 29,
            GetSourceSize = 30,
            SetMaximumFrameLatency = 31,
            GetMaximumFrameLatency = 32,
            GetFrameLatencyWaitableObject = 33
        };
    }

    namespace DXGI3
    {
        /** @brief IDXGISwapChain3 vtable indices (extends IDXGISwapChain2). */
        enum DXGISwapChain3VTbl : short
        {
            GetCurrentBackBufferIndex = 34,
            CheckColorSpaceSupport = 35,
            SetColorSpace1 = 36,
            ResizeBuffers1 = 37
        };
    }

    /** @brief DXGI vtable size constants. */
    class DXGI
    {
    public:
        static const int SwapChainVTableElements = 18;    /**< IDXGISwapChain method count. */
        static const int SwapChain1VTableElements = 29;   /**< IDXGISwapChain1 method count. */
        static const int SwapChain3VTableElements = 38;   /**< IDXGISwapChain3 method count. */
    };
}
