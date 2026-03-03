@echo off
REM CI: Use pre-installed vcpkg deps from server (copy) or run prepare-deps.
REM Pre-installed path: C:\vcpkg-hydrahook-deps (or set VCPKG_PREINSTALLED_DEPS)
REM
REM Usage:
REM   prepare-deps-ci.bat --copy-only     Copy from cache if exists (run in install phase)
REM   prepare-deps-ci.bat Win32|x64       Ensure deps for platform (run in before_build)
setlocal
cd /d "%~dp0"

set "CACHE_ROOT="
if defined VCPKG_PREINSTALLED_DEPS (
    set "CACHE_ROOT=%VCPKG_PREINSTALLED_DEPS%"
) else (
    set "CACHE_ROOT=C:\vcpkg-hydrahook-deps"
)
REM Strip trailing backslash for consistent path joining
if defined CACHE_ROOT if "%CACHE_ROOT:~-1%"=="\" set "CACHE_ROOT=%CACHE_ROOT:~0,-1%"

REM --copy-only: Just copy from cache if available (no prepare-deps)
if "%~1"=="--copy-only" (
    set "CACHE_SRC="
    if exist "%CACHE_ROOT%\x86-windows-static\include\spdlog\spdlog.h" set "CACHE_SRC=%CACHE_ROOT%"
    if not defined CACHE_SRC if exist "%CACHE_ROOT%\vcpkg_installed\x86-windows-static\include\spdlog\spdlog.h" set "CACHE_SRC=%CACHE_ROOT%\vcpkg_installed"
    if not defined CACHE_SRC if exist "%CACHE_ROOT%\x86-windows-static" set "CACHE_SRC=%CACHE_ROOT%"
    if not defined CACHE_SRC (
        echo [CI] Pre-installed deps not found at %CACHE_ROOT%
        echo [CI] Checked: %CACHE_ROOT%\x86-windows-static\include\spdlog\spdlog.h
        echo [CI] Checked: %CACHE_ROOT%\vcpkg_installed\x86-windows-static\include\spdlog\spdlog.h
        echo [CI] Checked: %CACHE_ROOT%\x86-windows-static
        if exist "%CACHE_ROOT%" dir "%CACHE_ROOT%" 2>nul
        echo [CI] Will run prepare-deps in before_build
        exit /b 0
    )
    echo [CI] Using pre-installed vcpkg dependencies from %CACHE_SRC%
    if not exist "vcpkg_installed" mkdir "vcpkg_installed"
    xcopy /E /I /Y /Q "%CACHE_SRC%\*" "vcpkg_installed\"
    if errorlevel 1 (
        echo [CI] Failed to copy pre-installed deps
        exit /b 1
    )
    echo [CI] Copied successfully - skipping vcpkg install
    exit /b 0
)

REM Platform mode: ensure deps exist, run prepare-deps only if needed
set "PLATFORM=%~1"
if "%PLATFORM%"=="" set "PLATFORM=both"
if /i "%PLATFORM%"=="Win32" set "PLATFORM=x86"

set "TRIPLET=x64-windows-static"
if /i "%PLATFORM%"=="x86" set "TRIPLET=x86-windows-static"

REM Check if we already have deps for this platform (from --copy-only or previous run)
if exist "vcpkg_installed\%TRIPLET%\include\spdlog\spdlog.h" (
    echo [CI] vcpkg_installed\%TRIPLET% already present - skipping prepare-deps
    exit /b 0
)

REM Try copy from cache first (in case --copy-only wasn't run or failed)
set "CACHE_SRC="
if exist "%CACHE_ROOT%\x86-windows-static\include\spdlog\spdlog.h" set "CACHE_SRC=%CACHE_ROOT%"
if not defined CACHE_SRC if exist "%CACHE_ROOT%\vcpkg_installed\x86-windows-static\include\spdlog\spdlog.h" set "CACHE_SRC=%CACHE_ROOT%\vcpkg_installed"
if not defined CACHE_SRC if exist "%CACHE_ROOT%\x86-windows-static" set "CACHE_SRC=%CACHE_ROOT%"
if defined CACHE_SRC (
    echo [CI] Copying pre-installed deps from %CACHE_SRC%
    if not exist "vcpkg_installed" mkdir "vcpkg_installed"
    xcopy /E /I /Y /Q "%CACHE_SRC%\*" "vcpkg_installed\"
    if not errorlevel 1 (
        if exist "vcpkg_installed\%TRIPLET%\include\spdlog\spdlog.h" (
            echo [CI] Copied successfully - skipping prepare-deps
            exit /b 0
        )
    )
)

REM Cache miss - run prepare-deps
echo [CI] Running prepare-deps for %PLATFORM%...
call "%~dp0prepare-deps.bat" %*
if errorlevel 1 exit /b 1
exit /b 0
