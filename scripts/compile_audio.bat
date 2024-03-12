@echo off

set "VER_TORCHAUDIO=v2.1.0"

rem Be verbose now
echo on

rem Install Python dependencies
python -m pip install cmake astunparse numpy ninja pyyaml setuptools cffi typing_extensions future six requests dataclasses Pillow

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64

rem TorchAudio 
cd audio
python -m pip install -r requirements.txt
set "DISTUTILS_USE_SDK=1"
python setup.py clean
python setup.py bdist_wheel

set "DISTUTILS_USE_SDK="
for %%f in ("dist\*.whl") do python -m pip install --force-reinstall --no-deps "%%f"
cd ..
