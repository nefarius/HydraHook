@echo off
REM CI wrapper: use pre-installed deps if available, otherwise run prepare-deps.
setlocal
cd /d "%~dp0"

if exist "C:\vcpkg-hydrahook-deps\x86-windows-static" (
    echo Using pre-installed vcpkg dependencies...
    xcopy /E /I /Y "C:\vcpkg-hydrahook-deps\*" "vcpkg_installed\"
    exit /b 0
)

call "%~dp0prepare-deps.bat" %*
if errorlevel 1 exit /b 1
exit /b 0
