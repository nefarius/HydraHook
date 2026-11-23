/**
 * @file Global.h
 * @brief Utility functions for environment expansion and process info.
 *
 * @internal
 * @copyright MIT License (c) 2018-2026 Benjamin Höglinger-Stelzer
 */

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

#pragma once

#include <string>
#include <Psapi.h>

namespace HydraHook
{
    namespace Core
    {
        namespace Util
        {
            /**
             * @brief Expands environment variables (e.g. %TEMP%) in a string.
             * @param str Input string possibly containing %VAR% placeholders.
             * @return Expanded string. The result does not contain a trailing NUL inside size().
             */
            inline std::string expand_environment_variables(const std::string & str)
            {
                const DWORD neededSize = ExpandEnvironmentStringsA(str.c_str(), nullptr, 0);
                if (neededSize <= 1)
                    return str;

                std::string expanded(static_cast<size_t>(neededSize), '\0');
                const DWORD written = ExpandEnvironmentStringsA(
                    str.c_str(),
                    expanded.data(),
                    neededSize);
                if (written == 0 || written > neededSize)
                    return str;

                // ExpandEnvironmentStringsA counts the terminating NUL.
                expanded.resize(static_cast<size_t>(written - 1));
                return expanded;
            }

            /**
             * @brief Returns the full path of the current process executable.
             * @return Process image path (for example \\Device\\HarddiskVolume1\\...\\app.exe), or empty on failure.
             */
            inline std::string process_name()
            {
                DWORD capacity = MAX_PATH;
                std::string path(capacity, '\0');
                for (;;)
                {
                    const DWORD len = GetProcessImageFileNameA(
                        GetCurrentProcess(),
                        path.data(),
                        capacity);
                    if (len == 0)
                    {
                        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || capacity > 32768)
                            return std::string();
                        capacity *= 2;
                        path.assign(capacity, '\0');
                        continue;
                    }
                    path.resize(len);
                    return path;
                }
            }

            /**
             * @brief Returns the directory containing the given module.
             * @param hMod Module handle, or NULL for the process executable.
             * @return Directory path with a trailing separator, or empty string on failure.
             */
            inline std::string get_module_directory(HMODULE hMod)
            {
                DWORD capacity = MAX_PATH;
                std::string path(capacity, '\0');
                for (;;)
                {
                    const DWORD len = GetModuleFileNameA(hMod, path.data(), capacity);
                    if (len == 0)
                        return std::string();
                    if (len >= capacity)
                    {
                        if (capacity > 32768)
                            return std::string();
                        capacity *= 2;
                        path.assign(capacity, '\0');
                        continue;
                    }
                    path.resize(len);
                    const auto pos = path.find_last_of("\\/");
                    if (pos == std::string::npos)
                        return std::string();
                    return path.substr(0, pos + 1);
                }
            }

            /**
             * @brief Returns the directory containing the main process executable.
             * @return Directory path (for example C:\\Program Files\\Game\\), or empty string on failure.
             */
            inline std::string get_process_directory()
            {
                return get_module_directory(nullptr);
            }
        };
    };
};