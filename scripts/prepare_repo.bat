@echo off

set "VER_PYTORCH=v2.1.0"
set "VER_TORCHVISION=v0.16.0"
set "VER_TORCHAUDIO=v2.1.0"
set "VER_IPEX=xpu-main"

rem Checkout individual components
if NOT EXIST pytorch (
    git clone https://github.com/pytorch/pytorch.git
)
if NOT EXIST vision (
    git clone https://github.com/pytorch/vision.git
)
if NOT EXIST audio (
    git clone https://github.com/pytorch/audio.git
)
if NOT EXIST intel-extension-for-pytorch (
    git clone https://github.com/intel/intel-extension-for-pytorch.git
)

rem Checkout required branch/commit and update submodules
cd pytorch
if not "%VER_PYTORCH%"=="" (
    git rm -rf .
    git clean -fxd
    git reset
    git checkout .
    git checkout main
    git pull
    git checkout %VER_PYTORCH%
)
git submodule sync
git submodule update --init --recursive
cd ..

cd vision
if not "%VER_TORCHVISION%"=="" (
    git rm -rf .
    git clean -fxd
    git reset
    git checkout .
    git checkout main
    git pull
    git checkout %VER_TORCHVISION%
)
git submodule sync
git submodule update --init --recursive
cd ..

cd audio
if not "%VER_TORCHAUDIO%"=="" (
    git rm -rf .
    git clean -fxd
    git reset
    git checkout .
    git checkout main
    git pull
    git checkout %VER_TORCHAUDIO%
)
git submodule sync
git submodule update --init --recursive
cd ..

cd intel-extension-for-pytorch
if not "%VER_IPEX%"=="" (
    git rm -rf .
    git clean -fxd
    git reset
    git checkout .
    git checkout main
    git pull
    git checkout %VER_IPEX%
)
git submodule sync
git submodule update --init --recursive
cd ..
