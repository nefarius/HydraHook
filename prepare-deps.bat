@echo off
REM Install vcpkg dependencies. Run this from "Developer Command Prompt for VS 2026" or
REM "x64 Native Tools Command Prompt for VS 2026" (or "x86 Native Tools") before building from Visual Studio.
REM
REM Usage: prepare-deps.bat [platform]
REM   platform: Win32|x86 - install x86-windows-static only (for Win32 builds)
REM             x64       - install x64-windows-static only (for x64 builds)
REM             (none)    - install both triplets (default, for local dev)
REM
REM The first build from VS may fail with D8050 or "unable to detect the active compiler"
REM because vcpkg runs in a context where the compiler toolchain is not set up.
REM Running this script once populates vcpkg_installed; subsequent VS builds work.

setlocal enabledelayedexpansion
cd /d "%~dp0"

set "PLATFORM=%~1"
if "%PLATFORM%"=="" set "PLATFORM=all"
if /i "%PLATFORM%"=="both" set "PLATFORM=all"

if not exist "vcpkg\vcpkg.exe" (
    echo Bootstrapping vcpkg...
    call vcpkg\bootstrap-vcpkg.bat
    if errorlevel 1 (
        echo Bootstrap failed.
        exit /b 1
    )
)

REM Use persistent binary cache on CI (set VCPKG_CACHE_PATH to a cache directory)
set "VCPKG_EXTRA_OPTIONS="
if defined VCPKG_CACHE_PATH (
    set "VCPKG_BINARY_SOURCES=clear;files,%VCPKG_CACHE_PATH%,readwrite"
    REM Pin tool versions to stabilize ABI hash across runs (avoids cache misses)
    set "VCPKG_EXTRA_OPTIONS=--x-abi-tools-use-exact-versions"
    if not exist "%VCPKG_CACHE_PATH%" mkdir "%VCPKG_CACHE_PATH%"
)

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=!SCRIPT_DIR:~0,-1!"

REM Locate VS for "all" mode (each triplet needs different vcvars)
set "VS_PATH="
if /i "%PLATFORM%"=="all" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VS_PATH=%%i"
    if not defined VS_PATH (
        echo vswhere not found or no VS installation. Run from Developer Command Prompt or specify a platform.
        exit /b 1
    )
)

if /i "%PLATFORM%"=="Win32" set "PLATFORM=x86"

REM vcpkg writes to <install-root>\<triplet>\. Pass the per-triplet directory as the
REM install root so packages land in vcpkg_installed\<triplet>\<triplet>\, which is
REM what MSBuild consumes. Separate roots keep an x86 install from deleting x64.
if /i "%PLATFORM%"=="x86" (
    echo Installing dependencies for x86-windows-static...
    vcpkg\vcpkg.exe install --triplet x86-windows-static --x-install-root="!SCRIPT_DIR!\vcpkg_installed\x86-windows-static" %VCPKG_EXTRA_OPTIONS%
    if errorlevel 1 exit /b 1
) else if /i "%PLATFORM%"=="x64" (
    echo Installing dependencies for x64-windows-static...
    vcpkg\vcpkg.exe install --triplet x64-windows-static --x-install-root="!SCRIPT_DIR!\vcpkg_installed\x64-windows-static" %VCPKG_EXTRA_OPTIONS%
    if errorlevel 1 exit /b 1
) else if /i "%PLATFORM%"=="all" (
    echo Installing dependencies for x86-windows-static and x64-windows-static...

    echo [1/2] Building x86-windows-static...
    call "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
    if errorlevel 1 (
        echo vcvarsall x86 failed.
        exit /b 1
    )
    vcpkg\vcpkg.exe install --triplet x86-windows-static --x-install-root="!SCRIPT_DIR!\vcpkg_installed\x86-windows-static" %VCPKG_EXTRA_OPTIONS%
    if errorlevel 1 exit /b 1

    echo [2/2] Building x64-windows-static...
    call "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
    if errorlevel 1 (
        echo vcvarsall x64 failed.
        exit /b 1
    )
    vcpkg\vcpkg.exe install --triplet x64-windows-static --x-install-root="!SCRIPT_DIR!\vcpkg_installed\x64-windows-static" %VCPKG_EXTRA_OPTIONS%
    if errorlevel 1 exit /b 1
) else (
    echo Unknown platform: %PLATFORM%
    echo Usage: prepare-deps.bat [Win32^|x86^|x64]
    exit /b 1
)

echo.
echo Dependencies installed. You can now build from Visual Studio.
exit /b 0
