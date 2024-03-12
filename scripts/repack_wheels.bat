@echo off
setlocal enabledelayedexpansion

if "%~2"=="" (
    echo Usage: %~nx0 ^<DPCPPROOT^> ^<MKLROOT^>
    echo DPCPPROOT and MKLROOT are mandatory, should be absolute or relative path to the root directory of DPC++ compiler and oneMKL respectively.
    exit /b 1
)

set "DPCPP_ROOT=%~1"
set "ONEMKL_ROOT=%~2"

rem Check existence of DPCPP and ONEMKL environments
set "DPCPP_ENV=%DPCPP_ROOT%\env\vars.bat"
if NOT EXIST "%DPCPP_ENV%" (
    echo DPC++ compiler environment "%DPCPP_ENV%" doesn't seem to exist.
    exit /b 2
)
call "%DPCPP_ENV%"

set "ONEMKL_ENV=%ONEMKL_ROOT%\env\vars.bat"
if NOT EXIST "%ONEMKL_ENV%" (
    echo oneMKL environment "%ONEMKL_ENV%" doesn't seem to exist.
    exit /b 3
)
call "%ONEMKL_ENV%"

python -m pip install wheel

set "CWD=%cd%\repack"
if EXIST "%CWD%" rmdir "%CWD%" /s /q
mkdir "%CWD%"
cd "%CWD%"

for %%a in ("../pytorch/dist/*") do set TORCH_WHL=%%a
for %%a in ("../intel-extension-for-pytorch/dist/*") do set IPEX_WHL=%%a

wheel unpack "../pytorch/dist/%TORCH_WHL%"
for /f "tokens=*" %%g in ('dir /a:d /b ^| findstr ^^torch') do set TORCH_REPACK_DIR=%%g

wheel unpack "../intel-extension-for-pytorch/dist/%IPEX_WHL%"
for /f "tokens=*" %%g in ('dir /a:d /b ^| findstr ^^intel_extension_for_pytorch') do set IPEX_REPACK_DIR=%%g

set "torch_dlls=uv.dll;libiomp5md.dll"
for %%d in ("%torch_dlls:;=";"%") do (
    set /a FOUND=0
    for %%p in ("%PATH:;=";"%") do (
        if !FOUND! EQU 0 (
            if EXIST "%%~p\%%~d" (
                set /a FOUND=1
                xcopy /s "%%~p\%%~d" "%TORCH_REPACK_DIR%\torch\lib"
            )
        )
    )
    if !FOUND! EQU 0 (
        echo %%d not found in current environment!
        exit /b 1
    )
)

set "ipex_dlls=sycl7.dll;pi_level_zero.dll;pi_win_proxy_loader.dll;mkl_core.2.dll;mkl_sycl_blas.4.dll;mkl_sycl_lapack.4.dll;mkl_sycl_dft.4.dll;mkl_tbb_thread.2.dll;libmmd.dll;svml_dispmd.dll"

for %%d in ("%ipex_dlls:;=";"%") do (
    set /a FOUND=0
    for %%p in ("%PATH:;=";"%") do (
        if !FOUND! EQU 0 (
            if EXIST "%%~p\%%~d" (
                set /a FOUND=1
                xcopy /s "%%~p\%%~d" "%IPEX_REPACK_DIR%\intel_extension_for_pytorch\bin"
            )
        )
    )
    if !FOUND! EQU 0 (
        echo %%d not found in current environment!
        exit /b 1
    )
)

wheel pack %TORCH_REPACK_DIR%
wheel pack %IPEX_REPACK_DIR%

cd ..
