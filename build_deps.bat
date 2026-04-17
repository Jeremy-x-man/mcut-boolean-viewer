@echo off
REM =============================================================================
REM build_deps.bat — mcut-boolean-viewer one-shot build script (Windows MSVC)
REM
REM Inspired by OrcaSlicer's deps build workflow:
REM   1. Builds all dependencies via deps\CMakeLists.txt (ExternalProject_Add)
REM      into deps_build\install\ — NO vcpkg, NO manual install needed.
REM   2. Configures and builds mcut_viewer.exe using the installed deps.
REM
REM Usage (from x64 Native Tools Command Prompt for VS):
REM   build_deps.bat              — build deps + app (Release)
REM   build_deps.bat Debug        — build deps + app (Debug)
REM   build_deps.bat --deps-only  — build deps only
REM   build_deps.bat --app-only   — build app only (deps must already exist)
REM   build_deps.bat clean        — clean everything and rebuild from scratch
REM
REM Requirements:
REM   - Visual Studio 2019 or 2022 with "Desktop development with C++" workload
REM   - CMake >= 3.16 (included with VS, or from cmake.org)
REM   - Git (for downloading dependencies)
REM   Run this script from:
REM     "x64 Native Tools Command Prompt for VS 2022" (or 2019)
REM =============================================================================

setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

REM ---- Fix: Convert backslashes to forward slashes to avoid CMake escape issues ----
set "SCRIPT_DIR_FWD=%SCRIPT_DIR:\=/%"

set BUILD_TYPE=Release
set BUILD_DEPS=1
set BUILD_APP=1

REM ---- Parse arguments -------------------------------------------------------
:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="Debug"        set BUILD_TYPE=Debug     & shift & goto :parse_args
if /I "%~1"=="Release"      set BUILD_TYPE=Release   & shift & goto :parse_args
if /I "%~1"=="--deps-only"  set BUILD_APP=0          & shift & goto :parse_args
if /I "%~1"=="--app-only"   set BUILD_DEPS=0         & shift & goto :parse_args
if /I "%~1"=="clean"        goto :do_clean
echo Unknown argument: %~1
exit /b 1
:args_done

REM ---- Use forward-slash paths for CMake to prevent escape sequences ----
set "DEPS_BUILD_DIR=%SCRIPT_DIR_FWD%/deps_build"
set "DEPS_INSTALL_DIR=%DEPS_BUILD_DIR%/install"
set "APP_BUILD_DIR=%SCRIPT_DIR_FWD%/build"

REM ---- Detect CPU cores -------------------------------------------------------
for /f "tokens=2 delims==" %%i in ('wmic cpu get NumberOfLogicalProcessors /value 2^>nul ^| find "="') do set NPROC=%%i
if not defined NPROC set NPROC=4

echo.
echo ============================================================
echo  mcut-boolean-viewer build (Windows MSVC)
echo   Build type  : %BUILD_TYPE%
echo   Parallel    : %NPROC% jobs
echo   Deps prefix : %DEPS_INSTALL_DIR%
echo ============================================================
echo.

REM ===========================================================================
REM Step 1: Build dependencies
REM ===========================================================================
if %BUILD_DEPS%==0 goto :skip_deps

echo [1/2] Configuring dependencies ...
if not exist "%DEPS_BUILD_DIR%" mkdir "%DEPS_BUILD_DIR%"

cmake -S "%SCRIPT_DIR%\deps" ^
      -B "%DEPS_BUILD_DIR%" ^
      -DDESTDIR="%DEPS_INSTALL_DIR%"

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] deps cmake configure failed.
    exit /b 1
)

echo.
echo [1/2] Building dependencies (first run downloads ^& compiles ~10 min) ...
echo       Subsequent runs are instant (all cached).
echo.
REM Use /m:1 at top level so ExternalProject log output stays readable
cmake --build "%DEPS_BUILD_DIR%" --config %BUILD_TYPE% -j1

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] deps build failed.
    exit /b 1
)

echo.
echo [1/2] Dependencies installed to: %DEPS_INSTALL_DIR%
echo.

:skip_deps

REM ===========================================================================
REM Step 2: Build main application
REM ===========================================================================
if %BUILD_APP%==0 goto :skip_app

if not exist "%DEPS_INSTALL_DIR%" (
    echo [ERROR] Deps install directory not found: %DEPS_INSTALL_DIR%
    echo         Run without --app-only first to build dependencies.
    exit /b 1
)

echo [2/2] Configuring mcut_viewer ...
if not exist "%APP_BUILD_DIR%" mkdir "%APP_BUILD_DIR%"

cmake -S "%SCRIPT_DIR%" ^
      -B "%APP_BUILD_DIR%" ^
      -DCMAKE_PREFIX_PATH="%DEPS_INSTALL_DIR%"

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] mcut_viewer cmake configure failed.
    exit /b 1
)

echo.
echo [2/2] Building mcut_viewer ...
cmake --build "%APP_BUILD_DIR%" --config %BUILD_TYPE% -- /m:%NPROC%

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] mcut_viewer build failed.
    exit /b 1
)

set BINARY=%APP_BUILD_DIR%\%BUILD_TYPE%\mcut_viewer.exe
if not exist "%BINARY%" set BINARY=%APP_BUILD_DIR%\mcut_viewer.exe

echo.
echo ============================================================
if exist "%BINARY%" (
    echo  Build successful!
    echo   Executable : %BINARY%
    echo.
    echo  Run with:
    echo    cd %APP_BUILD_DIR%\%BUILD_TYPE%
    echo    mcut_viewer.exe
) else (
    echo  [ERROR] Build failed - executable not found.
    exit /b 1
)
echo ============================================================
echo.

:skip_app
endlocal
exit /b 0

REM ===========================================================================
REM Clean
REM ===========================================================================
:do_clean
echo [clean] Removing deps_build\ and build\ ...
if exist "%SCRIPT_DIR%\deps_build" rmdir /s /q "%SCRIPT_DIR%\deps_build"
if exist "%SCRIPT_DIR%\build"      rmdir /s /q "%SCRIPT_DIR%\build"
echo [clean] Done.
endlocal
exit /b 0
