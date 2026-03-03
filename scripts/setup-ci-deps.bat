@echo off
REM One-time setup: install vcpkg dependencies to a persistent location for CI.
REM Run from "Developer Command Prompt for VS 2022" on the build server.
REM Re-run when vcpkg.json or the vcpkg submodule changes.
setlocal
set "VCPKG_INSTALL_ROOT=C:\vcpkg-hydrahook-deps"
set "REPO_ROOT=%~dp0.."
set "VCPKG_CACHE_PATH=C:\vcpkg-cache"

cd /d "%REPO_ROOT%"
call prepare-deps.bat x86
if errorlevel 1 exit /b 1
call prepare-deps.bat x64
if errorlevel 1 exit /b 1

echo Copying vcpkg_installed to %VCPKG_INSTALL_ROOT%...
xcopy /E /I /Y "%REPO_ROOT%\vcpkg_installed\*" "%VCPKG_INSTALL_ROOT%\"
echo Done. CI will use pre-installed deps when available.
