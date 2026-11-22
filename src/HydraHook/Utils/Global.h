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

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

/**
 * Get the full path of the current process executable.
 * @return Full process image path (e.g. \Device\HarddiskVolume1\...\app.exe).
 */

/**
 * Get the directory containing the main process executable, including a trailing path separator.
 * @return Directory path with trailing separator (e.g. C:\Program Files\Game\), or empty string on failure.
 */

/**
 * Get the directory containing the specified module, including a trailing path separator.
 * @param hMod Handle to the module (HMODULE).
 * @return Directory path with trailing separator, or empty string on failure.
 */
namespace HydraHook
{
    namespace Core
    {
        namespace Util
        {
            /**
             * @brief Expands environment variables (e.g. %TEMP%) in a string.
             * @param str Input string possibly containing %VAR% placeholders.
             * @return Expanded string.
             */
            inline std::string expand_environment_variables(const std::string & str)
            {
                std::string expandedStr;
                const DWORD neededSize = ExpandEnvironmentStringsA(str.c_str(),
                    nullptr, 0);
                if (neededSize)
                {
                    expandedStr.resize(neededSize);
                    if (0 == ExpandEnvironmentStringsA(str.c_str(),
                        &expandedStr[0],
                        neededSize))
                    {
                        // pathological case requires a copy
                        expandedStr = str;
                    }
                }
                // RVO here
                return expandedStr;
            }

            /**
             * @brief Returns the full path of the current process executable.
             * @return Process image path (e.g. \Device\HarddiskVolume1\...\app.exe).
             */
            inline std::string process_name()
            {
                const auto nSize = MAX_PATH + 1;
                char procName[nSize];
                GetProcessImageFileName(GetCurrentProcess(), procName, nSize);

                return std::string(procName);
            }

            /**
             * @brief Returns the directory containing the main process executable.
             * @return Directory path (e.g. C:\Program Files\Game\), or empty string on failure.
             */
            inline std::string get_process_directory()
            {
                char path[MAX_PATH];
                if (0 == GetModuleFileNameA(NULL, path, MAX_PATH)) {
                    return std::string();
                }
                std::string s(path);
                const auto pos = s.find_last_of("\\/");
                if (pos == std::string::npos) {
                    return std::string();
                }
                return s.substr(0, pos + 1);
            }

            /**
             * @brief Returns the directory containing the given module (e.g. the DLL).
             * @param hMod Module handle (HMODULE).
             * @return Directory path, or empty string on failure.
             */
            inline std::string get_module_directory(HMODULE hMod)
            {
                char path[MAX_PATH];
                if (0 == GetModuleFileNameA(hMod, path, MAX_PATH)) {
                    return std::string();
                }
                std::string s(path);
                const auto pos = s.find_last_of("\\/");
                if (pos == std::string::npos) {
                    return std::string();
                }
                return s.substr(0, pos + 1);
            }
        };
    };
};