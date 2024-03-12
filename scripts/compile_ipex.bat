@echo off

set "VER_IPEX=xpu-main"

if "%~2"=="" (
    echo Usage: %~nx0 ^<DPCPPROOT^> ^<MKLROOT^> [AOT]
    echo DPCPPROOT and MKLROOT are mandatory, should be absolute or relative path to the root directory of DPC++ compiler and oneMKL respectively.
    echo AOT is optional, should be the text string for environment variable USE_AOT_DEVLIST.
    exit /b 1
)
set "DPCPP_ROOT=%~1"
set "ONEMKL_ROOT=%~2"
set "AOT="
if not "%~3"=="" (
    set "AOT=%~3"
)

rem Check existance of DPCPP and ONEMKL environments
set "DPCPP_ENV=%DPCPP_ROOT%\env\vars.bat"
if NOT EXIST "%DPCPP_ENV%" (
    echo DPC++ compiler environment "%DPCPP_ENV%" doesn't seem to exist.
    exit /b 2
)

set "ONEMKL_ENV=%ONEMKL_ROOT%\env\vars.bat"
if NOT EXIST "%ONEMKL_ENV%" (
    echo oneMKL environment "%ONEMKL_ENV%" doesn't seem to exist.
    exit /b 3
)

rem Be verbose now
echo on

rem Install Python dependencies
python -m pip install cmake astunparse numpy ninja pyyaml setuptools cffi typing_extensions future six requests dataclasses Pillow

call "%DPCPP_ENV%"
call "%ONEMKL_ENV%"

rem Intel® Extension for PyTorch*
cd intel-extension-for-pytorch
python -m pip install -r requirements.txt
if NOT "%AOT%"=="" (
    set "USE_AOT_DEVLIST=%AOT%"
)
set "BUILD_WITH_CPU=0"
set "USE_MULTI_CONTEXT=1"
set "DISTUTILS_USE_SDK=1"
python setup.py clean
python setup.py bdist_wheel

set "DISTUTILS_USE_SDK="
set "USE_MULTI_CONTEXT="
set "BUILD_WITH_CPU="
for %%f in ("dist\*.whl") do python -m pip install --force-reinstall --no-deps "%%f"
cd ..
