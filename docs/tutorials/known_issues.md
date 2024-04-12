Troubleshooting
===============

## GPU-specific Issues

### General Usage

- **Problem**: FP64 data type is unsupported on current platform.
  - **Cause**: FP64 is not natively supported by the [Intel® Data Center GPU Flex Series](https://www.intel.com/content/www/us/en/products/docs/discrete-gpus/data-center-gpu/flex-series/overview.html) platform. 
    If you run any AI workload on that platform and receive this error message, it means a kernel requires FP64 instructions that are not supported and the execution is stopped.
- **Problem**: Runtime error `invalid device pointer` if `import horovod.torch as hvd` before `import intel_extension_for_pytorch`
  - **Cause**: Intel® Optimization for Horovod\* uses utilities provided by Intel® Extension for PyTorch\*. The improper import order causes Intel® Extension for PyTorch\* to be unloaded before Intel®
    Optimization for Horovod\* at the end of the execution and triggers this error.
  - **Solution**: Do `import intel_extension_for_pytorch` before `import horovod.torch as hvd`.
- **Problem**: Number of dpcpp devices should be greater than zero.
  - **Cause**: If you use Intel® Extension for PyTorch* in a conda environment, you might encounter this error. Conda also ships the libstdc++.so dynamic library file that may conflict with the one shipped
    in the OS. 
  - **Solution**: Export the `libstdc++.so` file path in the OS to an environment variable `LD_PRELOAD`.
- **Problem**: Symbol undefined caused by `_GLIBCXX_USE_CXX11_ABI`.
    ```bash
    ImportError: undefined symbol: _ZNK5torch8autograd4Node4nameB5cxx11Ev
    ```
  - **Cause**: DPC++ does not support `_GLIBCXX_USE_CXX11_ABI=0`, Intel® Extension for PyTorch\* is always compiled with `_GLIBCXX_USE_CXX11_ABI=1`. This symbol undefined issue appears when PyTorch\* is
    compiled with `_GLIBCXX_USE_CXX11_ABI=0`.
  - **Solution**: Pass `export GLIBCXX_USE_CXX11_ABI=1` and compile PyTorch\* with particular compiler which supports `_GLIBCXX_USE_CXX11_ABI=1`. We recommend using prebuilt wheels 
    in [download server](https:// developer.intel.com/ipex-whl-stable-xpu) to avoid this issue.
- **Problem**: Bad termination after AI model execution finishes when using Intel MPI.
  - **Cause**: This is a random issue when the AI model (e.g. RN50 training) execution finishes in an Intel MPI environment. It is not user-friendly as the model execution ends ungracefully.
  - **Solution**: Add `dist.destroy_process_group()` during the cleanup stage in the model script, as described 
    in [Getting Started with Distributed Data Parallel](https://pytorch.org/tutorials/intermediate/ddp_tutorial.html).
- **Problem**: `-997 runtime error` when running some AI models on Intel® Arc™ A-Series GPUs.
  - **Cause**:  Some of the `-997 runtime error` are actually out-of-memory errors. As Intel® Arc™ A-Series GPUs have less device memory than Intel® Data Center GPU Flex Series 170 and Intel® Data Center GPU 
    Max  Series, running some AI models on them may trigger out-of-memory errors and cause them to report failure such as `-997 runtime error` most likely. This is expected. Memory usage optimization is a work in progress to allow Intel® Arc™ A-Series GPUs to support more AI models.
- **Problem**: Building from source for Intel® Arc™ A-Series GPUs fails on WSL2 without any error thrown.
  - **Cause**: Your system probably does not have enough RAM, so Linux kernel's Out-of-memory killer was invoked. You can verify this by running `dmesg` on bash (WSL2 terminal).
  - **Solution**: If the OOM killer had indeed killed the build process, then you can try increasing the swap-size of WSL2, and/or decreasing the number of parallel build jobs with the environment 
    variable `MAX_JOBS` (by default, it's equal to the number of logical CPU cores. So, setting `MAX_JOBS` to 1 is a very conservative approach that would slow things down a lot).
- **Problem**: Some workloads terminate with an error `CL_DEVICE_NOT_FOUND` after some time on WSL2.
  - **Cause**:  This issue is due to the [TDR feature](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/tdr-registry-keys#tdrdelay) on Windows.
  - **Solution**: Try increasing TDRDelay in your Windows Registry to a large value, such as 20 (it is 2 seconds, by default), and reboot.
- **Problem**: Runtime error `Unable to find TSan function` might be raised when running some CPU AI workloads in certain scenarios.
  - **Cause**:  This issue is probably caused by the compatibility issue of OMP tool libraries.
  - **Solution**: Please try the workaround: disable OMP tool libraries by `export OMP_TOOL="disabled"`, to unblock your workload. We are working on the final solution and will release it as soon as possible.
- **Problem**: The profiled data on GPU operators using legacy profiler is not accurate sometimes.
  - **Cause**: Compiler in 2024.0 oneAPI basekit optimizes barrier implementation which brings negative impact on legacy profiler.
  - **Solution**: Use Kineto profiler instead. Or use legacy profiler with `export UR_L0_IN_ORDER_BARRIER_BY_SIGNAL=0` to workaround this issue.
- **Problem**: Random bad termination after AI model convergence test (>24 hours) finishes.
  - **Cause**: This is a random issue when some AI model convergence test execution finishes. It is not user-friendly as the model execution ends ungracefully.
  - **Solution**: Kill the process after the convergence test finished, or use checkpoints to divide the convergence test into several phases and execute separately.
- **Problem**: Random GPU hang issue when executing the first allreduce in LLM inference workloads on 1 Intel® Data Center GPU Max 1550 card.
  - **Cause**: Race condition happens between oneDNN kernels and oneCCL Bindings for Pytorch\* allreduce primitive. 
  - **Solution**: Use `TORCH_LLM_ALLREDUCE=0` to workaround this issue.
- **Problem**: GPU hang issue when executing LLM inference workloads on multi Intel® Data Center GPU Max series cards over PCIe communication.
  - **Cause**: oneCCL Bindings for Pytorch\* allreduce primitive does not support PCIe for cross-cards communication.
  - **Solution**: Enable XeLink for cross-cards communication, or use `TORCH_LLM_ALLREDUCE=0` for the PCIe only environments.
### Library Dependencies

- **Problem**: Cannot find oneMKL library when building Intel® Extension for PyTorch\* without oneMKL.

  ```bash
  /usr/bin/ld: cannot find -lmkl_sycl
  /usr/bin/ld: cannot find -lmkl_intel_ilp64
  /usr/bin/ld: cannot find -lmkl_core
  /usr/bin/ld: cannot find -lmkl_tbb_thread
  dpcpp: error: linker command failed with exit code 1 (use -v to see invocation)
  ```
  
  - **Cause**: When PyTorch\* is built with oneMKL library and Intel® Extension for PyTorch\* is built without MKL library, this linker issue may occur.
  - **Solution**: Resolve the issue by setting:

    ```bash
    export USE_ONEMKL=OFF
    export MKL_DPCPP_ROOT=${HOME}/intel/oneapi/mkl/latest
    ```

   Then clean build Intel® Extension for PyTorch\*.

- **Problem**: Undefined symbol: `mkl_lapack_dspevd`. Intel MKL FATAL ERROR: cannot load `libmkl_vml_avx512.so.2` or `libmkl_vml_def.so.2.
  - **Cause**: This issue may occur when Intel® Extension for PyTorch\* is built with oneMKL library and PyTorch\* is not build with any MKL library. The oneMKL kernel may run into CPU backend incorrectly 
    and trigger this issue. 
  - **Solution**: Resolve the issue by installing the oneMKL library from conda:

    ```bash
    conda install mkl
    conda install mkl-include
    ```

   Then clean build PyTorch\*.

- **Problem**: OSError: `libmkl_intel_lp64.so.2`: cannot open shared object file: No such file or directory.
  - **Cause**: Wrong MKL library is used when multiple MKL libraries exist in system.
  - **Solution**: Preload oneMKL by:

    ```bash
    export LD_PRELOAD=${MKL_DPCPP_ROOT}/lib/intel64/libmkl_intel_lp64.so.2:${MKL_DPCPP_ROOT}/lib/intel64/libmkl_intel_ilp64.so.2:${MKL_DPCPP_ROOT}/lib/intel64/libmkl_gnu_thread.so.2:${MKL_DPCPP_ROOT}/lib/intel64/libmkl_core.so.2:${MKL_DPCPP_ROOT}/lib/intel64/libmkl_sycl.so.2
    ```

    If you continue seeing similar issues for other shared object files, add the corresponding files under `${MKL_DPCPP_ROOT}/lib/intel64/` by `LD_PRELOAD`. Note that the suffix of the libraries may change (e.g. from .1 to .2), if more than one oneMKL library is installed on the system.

### Unit Test

- Unit test failures on Intel® Data Center GPU Flex Series 170

  The following unit test fails on Intel® Data Center GPU Flex Series 170 but the same test case passes on Intel® Data Center GPU Max Series. The root cause of the failure is under investigation.
    - `test_weight_norm.py::TestNNMethod::test_weight_norm_differnt_type`

  The following unit tests fail in Windows environment on Intel® Arc™ A770 Graphic card. The root cause of the failures is under investigation.
     - `test_foreach.py::TestTorchMethod::test_foreach_cos`
     - `test_foreach.py::TestTorchMethod::test_foreach_sin`
     - `test_polar.py::TestTorchMethod::test_polar_float`
     - `test_special_ops.py::TestTorchMethod::test_special_spherical_bessel_j0`
     - `test_transducer_loss.py::TestNNMethod::test_vallina_transducer_loss`
