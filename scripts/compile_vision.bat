@echo off

set "VER_TORCHVISION=v0.16.0"

rem Be verbose now
echo on

rem Install Python dependencies
python -m pip install cmake astunparse numpy ninja pyyaml setuptools cffi typing_extensions future six requests dataclasses Pillow

rem TorchVision 
cd vision
call conda install -y --force-reinstall libpng libjpeg-turbo -c conda-forge
python setup.py clean
python setup.py bdist_wheel

call conda remove libpng libjpeg-turbo -y
for %%f in ("dist\*.whl") do python -m pip install --force-reinstall --no-deps "%%f"
cd ..
