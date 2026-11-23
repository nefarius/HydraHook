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

#include "Window.h"
#include <utility>
#include "Exceptions.hpp"

using namespace HydraHook::Core::Exceptions;

Window::Window(std::string windowClassName) : temp_window(nullptr), window_class_name(std::move(windowClassName))
{
    ZeroMemory(&window_class, sizeof(WNDCLASSEX));

    window_class.cbSize = sizeof(WNDCLASSEX);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = DefWindowProc;
    window_class.lpszClassName = window_class_name.c_str();

    window_class.hInstance = GetModuleHandle(nullptr);
    if (window_class.hInstance == nullptr)
    {
        throw GenericWinAPIException("Could not get the instance handle");
    }

    if (!RegisterClassEx(&window_class))
    {
        throw GenericWinAPIException("Could not register the window class");
    }

    temp_window = CreateWindow(window_class.lpszClassName, "Temporary DirectX Overlay Window",
        WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, window_class.hInstance, NULL);
    if (temp_window == nullptr)
    {
        UnregisterClass(window_class.lpszClassName, window_class.hInstance);
        throw GenericWinAPIException("Could not create the temporary window");
    }
}

Window::~Window()
{
    if (temp_window)
    {
        DestroyWindow(temp_window);
    }
    if (window_class.lpszClassName)
    {
        UnregisterClass(window_class.lpszClassName, window_class.hInstance);
    }
}

HWND Window::windowHandle() const
{
    return temp_window;
}
