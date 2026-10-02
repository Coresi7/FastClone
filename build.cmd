@echo off
rem ---------------------------------------------------------------------------
rem FastClone one-click build (Windows / this machine)
rem
rem   build.cmd                          msvc  + Release + x64
rem   build.cmd clang                    clang-cl + Release + x64
rem   build.cmd clang Debug              clang-cl + Debug   + x64
rem   build.cmd msvc Release x64         explicit
rem   build.cmd clang Release x64 test   build then run ctest
rem
rem Notes:
rem   - Both toolchains use the Ninja generator (uniform, and ctest works
rem     reliably under it; the old VS generator produced an empty test list).
rem   - Every CMake *configure* bumps the patch version (see CMakeLists.txt
rem     auto-version block), so re-running this script advances 1.0.x.
rem   - Requires C:\xxHash-install (static xxhash, matches release.yml).
rem ---------------------------------------------------------------------------
setlocal

set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Enterprise"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat"
set "CMAKEBIN=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "NINJABIN=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "LLVMBIN=%VSROOT%\VC\Tools\Llvm\x64\bin"
set "XXHASH=C:/xxHash-install"

set "TC=msvc"
set "CFG=Release"
set "ARCH=x64"
set "DOTEST=0"
if not "%~1"=="" set "TC=%~1"
if not "%~2"=="" set "CFG=%~2"
if not "%~3"=="" set "ARCH=%~3"
if /i "%~4"=="test" set "DOTEST=1"
rem Optional 5th arg overrides the build directory (useful for clean verification
rem without touching an existing tree).
set "BDIR_OVR=%~5"

if /i "%~1"=="-h" goto :usage
if /i "%~1"=="--help" goto :usage

rem --- vcvars argument by target arch ---------------------------------------
if /i "%ARCH%"=="x64" (
    set "VCARG=amd64"
) else if /i "%ARCH%"=="arm64" (
    set "VCARG=amd64_arm64"
) else if /i "%ARCH%"=="x86" (
    set "VCARG=x86"
) else (
    echo [build] unknown arch "%ARCH%" ^(use x64 / arm64 / x86^)
    exit /b 2
)

if not exist "%VCVARS%" (
    echo [build] vcvarsall.bat not found: "%VCVARS%"
    exit /b 1
)

echo [build] toolchain=%TC% config=%CFG% arch=%ARCH%
call "%VCVARS%" %VCARG%
if errorlevel 1 (
    echo [build] vcvarsall failed
    exit /b 1
)

set "PATH=%CMAKEBIN%;%NINJABIN%;%LLVMBIN%;%PATH%"

rem --- toolchain selection ---------------------------------------------------
if /i "%TC%"=="clang" (
    set "CCOMP=clang-cl"
    set "CXXCOMP=clang-cl"
    set "BDIR=build-clang-%ARCH%"
) else if /i "%TC%"=="msvc" (
    set "CCOMP=cl"
    set "CXXCOMP=cl"
    set "BDIR=build-msvc-%ARCH%"
) else (
    echo [build] unknown toolchain "%TC%" ^(use msvc / clang^)
    exit /b 2
)

if not "%BDIR_OVR%"=="" set "BDIR=%BDIR_OVR%"

rem --- runtime library: static CRT for Release only (matches release.yml) ----
set "RTFLAG="
if /i "%CFG%"=="Release" set "RTFLAG=-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"

echo [build] configure: %BDIR%
rem CMAKE_C_COMPILER is intentionally NOT passed: project() declares LANGUAGES CXX
rem only, so CMake warns "manually-specified variable was not used".
cmake -S . -B "%BDIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=%CFG% ^
    -DCMAKE_CXX_COMPILER=%CXXCOMP% ^
    -DCMAKE_PREFIX_PATH=%XXHASH% ^
    %RTFLAG%
if errorlevel 1 (
    echo [build] configure FAILED
    exit /b 1
)

echo [build] compile
cmake --build "%BDIR%" --parallel
if errorlevel 1 (
    echo [build] compile FAILED
    exit /b 1
)

if "%DOTEST%"=="1" (
    echo [build] ctest
    ctest --test-dir "%BDIR%" --output-on-failure
    if errorlevel 1 (
        echo [build] ctest FAILED
        exit /b 1
    )
)

echo [build] OK  -^> %BDIR%
exit /b 0

:usage
echo Usage: build.cmd [msvc^|clang] [Release^|Debug] [x64^|arm64^|x86] [test]
exit /b 0
