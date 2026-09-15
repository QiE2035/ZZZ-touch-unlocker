@echo off
setlocal EnableExtensions

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build\msvc-x64"
set "DIST=%ROOT%\dist"
set "HOOK_DLL=%BUILD%\Release\zzz-touch-hook.dll"
set "UNLOCKER_EXE=%BUILD%\Release\zzz-touch-unlocker.exe"

where cmake >nul 2>nul || (
    echo [error] cmake was not found in PATH.
    exit /b 1
)

set "GENERATOR="
cmake --help | findstr /C:"Visual Studio 18 2026" >nul && set "GENERATOR=Visual Studio 18 2026"
if not defined GENERATOR cmake --help | findstr /C:"Visual Studio 17 2022" >nul && set "GENERATOR=Visual Studio 17 2022"

if not defined GENERATOR (
    echo [error] Visual Studio 2022 or newer with C++ support was not detected by CMake.
    exit /b 1
)

echo [build] generator = %GENERATOR%
echo [build] configuring x64 Release build...
cmake -S "%ROOT%" -B "%BUILD%" -G "%GENERATOR%" -A x64
if errorlevel 1 exit /b 1

echo [build] building...
cmake --build "%BUILD%" --config Release --parallel
if errorlevel 1 exit /b 1

if not exist "%HOOK_DLL%" (
    echo [error] build output not found: %HOOK_DLL%
    exit /b 1
)
if not exist "%UNLOCKER_EXE%" (
    echo [error] build output not found: %UNLOCKER_EXE%
    exit /b 1
)

if not exist "%DIST%" mkdir "%DIST%"
copy /Y "%HOOK_DLL%" "%DIST%\zzz-touch-hook.dll" >nul
if errorlevel 1 exit /b 1
copy /Y "%UNLOCKER_EXE%" "%DIST%\zzz-touch-unlocker.exe" >nul
if errorlevel 1 exit /b 1

echo [build] done:
dir /B "%DIST%"
endlocal
