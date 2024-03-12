@echo off

set "VER_PYTORCH=v2.1.0"

rem Be verbose now
echo on

rem Install Python dependencies
python -m pip install cmake astunparse numpy ninja pyyaml setuptools cffi typing_extensions future six requests dataclasses Pillow

rem PyTorch
cd pytorch
for %%f in ("..\intel-extension-for-pytorch\torch_patches\*.patch") do git apply "%%f"
python -m pip install -r requirements.txt
call conda install --force-reinstall intel::mkl-static intel::mkl-include -y
call conda install conda-forge::libuv -y
rem Ensure cmake can find python packages when using conda or virtualenv
if defined CONDA_PREFIX (
    set "CMAKE_PREFIX_PATH=%CONDA_PREFIX%"
) else if defined VIRTUAL_ENV (
     set "CMAKE_PREFIX_PATH=%VIRTUAL_ENV%"
)
set "CMAKE_INCLUDE_PATH=%CONDA_PREFIX%\Library\include"
set "LIB=%CONDA_PREFIX%\Library\lib;%LIB%"
set "USE_NUMA=0"
set "USE_CUDA=0"
python setup.py clean
python setup.py bdist_wheel

set "USE_CUDA="
set "USE_NUMA="
set "LIB="
set "CMAKE_INCLUDE_PATH="
set "CMAKE_PREFIX_PATH="
call conda remove mkl-static mkl-include -y
for %%f in ("dist\*.whl") do python -m pip install --force-reinstall --no-deps "%%f"
