@echo off
REM Install vcpkg dependencies. Run this from "Developer Command Prompt for VS 2022" or "x64 Native Tools Command Prompt for VS 2022"
REM (or "Developer Command Prompt for VS") before building from Visual Studio.
REM
REM Usage: prepare-deps.bat [platform]
REM   platform: Win32|x86  - install x86-windows-static only (for Win32 builds)
REM             x64        - install x64-windows-static only (for x64 builds)
REM             (none)     - install both triplets (default, for local dev)
REM
REM The first build from VS may fail with "unable to detect the active compiler"
REM because vcpkg runs in a context where the compiler toolchain is not set up.
REM Running this script once populates vcpkg_installed; subsequent VS builds work.

setlocal
cd /d "%~dp0"

set "PLATFORM=%~1"
if "%PLATFORM%"=="" set "PLATFORM=both"

if not exist "vcpkg\vcpkg.exe" (
    echo Bootstrapping vcpkg...
    call vcpkg\bootstrap-vcpkg.bat
    if errorlevel 1 (
        echo Bootstrap failed.
        exit /b 1
    )
)

REM Use persistent binary cache on CI (set VCPKG_CACHE_PATH on server or in appveyor.yml)
if defined VCPKG_CACHE_PATH (
    set "VCPKG_BINARY_SOURCES=clear;files,%VCPKG_CACHE_PATH%,readwrite"
)

REM vcpkg manifest mode keeps only one triplet per build; install for requested platform(s)
if /i "%PLATFORM%"=="Win32" set "PLATFORM=x86"
if /i "%PLATFORM%"=="x86" (
    echo Installing dependencies for x86-windows-static...
    vcpkg\vcpkg.exe install --triplet x86-windows-static
    if errorlevel 1 exit /b 1
) else if /i "%PLATFORM%"=="x64" (
    echo Installing dependencies for x64-windows-static...
    vcpkg\vcpkg.exe install --triplet x64-windows-static
    if errorlevel 1 exit /b 1
) else (
    echo Installing dependencies for x86-windows-static and x64-windows-static...
    vcpkg\vcpkg.exe install --triplet x86-windows-static
    if errorlevel 1 exit /b 1
    vcpkg\vcpkg.exe install --triplet x64-windows-static
    if errorlevel 1 exit /b 1
)

echo.
echo Dependencies installed. You can now build from Visual Studio.
exit /b 0
