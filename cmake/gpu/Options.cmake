## Included by CMakeLists
if(Options_GPU_cmake_included)
    return()
endif()
set(Options_GPU_cmake_included true)

# The options to build xpu
include(CMakeDependentOption)
option(USE_ONEMKL "Use oneMKL BLAS" ON)
option(USE_CHANNELS_LAST_1D "Use channels last 1d" ON)
option(USE_PERSIST_STREAM "Use persistent oneDNN stream" ON)
option(USE_SCRATCHPAD_MODE "Use oneDNN scratchpad mode" ON)
option(USE_PRIMITIVE_CACHE "Cache oneDNN primitives by FRAMEWORK for specific operators" ON)
option(USE_QUEUE_BARRIER "Use queue submit_barrier, otherwise use dummy kernel" ON)
set(USE_AOT_DEVLIST "" CACHE STRING "Set device list for AOT build")
option(USE_DS_KERNELS "Build deepspeed kernels" ON)
if (WIN32 OR MSVC)
  set(USE_DS_KERNELS OFF)
endif()
if (NOT USE_ONEMKL)
  set(USE_DS_KERNELS OFF)
endif()
option(USE_SYCL_ASSERT "Enables assert in sycl kernel" OFF)
option(USE_ITT_ANNOTATION "Enables ITT annotation in sycl kernel" OFF)
option(USE_SPLIT_FP64_LOOPS "Split FP64 loops into separate kernel for element-wise kernels" ON)
set(USE_XETLA "ON" CACHE STRING "Use XeTLA based customer kernels; Specify a comma-sep list of gpu architectures (e.g. xe_lpg,xe_hpg) to only enable kernels for specific platforms")
set(USE_ONEDNN_DIR "" CACHE STRING "Specify oneDNN source path which contains its include directory and lib directory")
set(USE_XETLA_SRC "${IPEX_GPU_ROOT_DIR}/aten/operators/xetla/kernels/" CACHE STRING "Specify XETLA source path which contains its include dir")

option(BUILD_BY_PER_KERNEL "Build by DPC++ per_kernel option (exclusive with USE_AOT_DEVLIST)" OFF)
option(BUILD_INTERNAL_DEBUG "Use internal debug code path" OFF)
option(BUILD_SEPARATE_OPS "Build each operator in separate library" OFF)
option(BUILD_SIMPLE_TRACE "Build simple trace for each registered operator" ON)
#FIXME: For now oneDNN is not ready to support strided source
option(BUILD_CONV_CONTIGUOUS "Require contiguous in oneDNN conv" ON)
set(BUILD_OPT_LEVEL "" CACHE STRING "Add build option -Ox, accept values: 0/1")
set(BUILD_WITH_SANITIZER "" CACHE STRING "Build with sanitizer check. Support one of address, thread, and leak options at a time. The default option is address.") 

set(EXTRA_BUILD_OPTION)
if (DEFINED ENV{IPEX_GPU_EXTRA_BUILD_OPTION})
  # An interface to setup extra build option.
  # Example, set env var: IPEX_GPU_EXTRA_BUILD_OPTION="gcc-install-dir=[path]"
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag($ENV{IPEX_GPU_EXTRA_BUILD_OPTION} OPT_CHECK_PASS)
  if(OPT_CHECK_PASS)
    set(EXTRA_BUILD_OPTION "$ENV{IPEX_GPU_EXTRA_BUILD_OPTION}")
  else()
    message(WARNING
    "Invalid build options: $ENV{IPEX_GPU_EXTRA_BUILD_OPTION}")
  endif()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${EXTRA_BUILD_OPTION}")
endif()

function (print_xpu_config_summary)
  # Fetch configurations of intel-ext-pt-gpu
  get_target_property(SANITIZER_OPTION intel-ext-pt-gpu SANITIZER_OPTION)
  get_target_property(GPU_NATIVE_DEFINITIONS intel-ext-pt-gpu COMPILE_DEFINITIONS)
  get_target_property(GPU_LINK_LIBRARIES intel-ext-pt-gpu LINK_LIBRARIES)
  get_target_property(ONEDNN_INCLUDE_DIR intel-ext-pt-gpu ONEDNN_INCLUDE_DIR)
  get_target_property(ONEDNN_LIBRARY intel-ext-pt-gpu ONEDNN_LIBRARY)
  get_target_property(ONEMKL_INCLUDE_DIR intel-ext-pt-gpu ONEMKL_INCLUDE_DIR)

    print_config_summary()
    message(STATUS "******** Summary on XPU ********")
    message(STATUS "General:")

    message(STATUS "  C compiler            : ${CMAKE_C_COMPILER}")

    message(STATUS "  C++ compiler          : ${CMAKE_CXX_COMPILER}")
    message(STATUS "  C++ compiler ID       : ${CMAKE_CXX_COMPILER_ID}")
    message(STATUS "  C++ compiler version  : ${CMAKE_CXX_COMPILER_VERSION}")

    message(STATUS "  CXX flags             : ${CMAKE_CXX_FLAGS}")
    message(STATUS "  Compile definitions   : ${GPU_NATIVE_DEFINITIONS}")
    message(STATUS "  Extra build options   : ${EXTRA_BUILD_OPTION}")
    message(STATUS "  CXX Linker options    : ${CMAKE_SHARED_LINKER_FLAGS}")
    message(STATUS "  Link libraries        : ${GPU_LINK_LIBRARIES}")

    message(STATUS "  SYCL Language version : ${SYCL_LANGUAGE_VERSION}")
    message(STATUS "  SYCL Compiler version : ${SYCL_COMPILER_VERSION}")
    message(STATUS "  SYCL Kernel flags     : ${IPEX_SYCL_KERNEL_FLAGS}")
    message(STATUS "  SYCL Link flags       : ${IPEX_SYCL_LINK_FLAGS}")

    message(STATUS "  Intel SYCL instance ID: ${SYCL_IMPLEMENTATION_ID}")
    message(STATUS "  Intel SYCL include    : ${SYCL_INCLUDE_DIR}")
    message(STATUS "  Intel SYCL library    : ${SYCL_LIBRARY_DIR}")

    message(STATUS "  Intel IGC version     : ${IGC_OCLOC_VERSION}")

    message(STATUS "  LevelZero version     : ${LEVEL_ZERO_VERSION}")
    message(STATUS "  LevelZero include     : ${LevelZero_INCLUDE_DIR}")
    message(STATUS "  LevelZero library     : ${LevelZero_LIBRARY}")

    message(STATUS "  OpenCL include        : ${OpenCL_INCLUDE_DIR}")
    message(STATUS "  OpenCL library        : ${OpenCL_LIBRARY}")

    message(STATUS "  Torch version         : ${Torch_VERSION}")
    message(STATUS "  Torch include         : ${TORCH_INCLUDE_DIRS}")

    message(STATUS "  oneDNN include        : ${ONEDNN_INCLUDE_DIR}")
    message(STATUS "  oneDNN library        : ${ONEDNN_LIBRARY}")
  if (USE_ONEMKL)
    message(STATUS "  oneMKL include        : ${ONEMKL_INCLUDE_DIR}")
  endif(USE_ONEMKL)

    message(STATUS "Options:")
    message(STATUS "  USE_XETLA             : ${USE_XETLA}")
  if (USE_XETLA)
    message(STATUS "  USE_XETLA_SRC         : ${USE_XETLA_SRC}")
  endif(USE_XETLA)
    message(STATUS "  USE_DS_KERNELS        : ${USE_DS_KERNELS}")
    message(STATUS "  USE_ONEMKL            : ${USE_ONEMKL}")
    message(STATUS "  USE_CHANNELS_LAST_1D  : ${USE_CHANNELS_LAST_1D}")
    message(STATUS "  USE_PERSIST_STREAM    : ${USE_PERSIST_STREAM}")
    message(STATUS "  USE_PRIMITIVE_CACHE   : ${USE_PRIMITIVE_CACHE}")
    message(STATUS "  USE_QUEUE_BARRIER     : ${USE_QUEUE_BARRIER}")
    message(STATUS "  USE_SCRATCHPAD_MODE   : ${USE_SCRATCHPAD_MODE}")
    message(STATUS "  USE_SYCL_ASSERT       : ${USE_SYCL_ASSERT}")
    message(STATUS "  USE_ITT_ANNOTATION    : ${USE_ITT_ANNOTATION}")
    message(STATUS "  USE_SPLIT_FP64_LOOPS  : ${USE_SPLIT_FP64_LOOPS}")

  if(NOT BUILD_BY_PER_KERNEL AND USE_AOT_DEVLIST)
    message(STATUS "  USE_AOT_DEVLIST       : ${USE_AOT_DEVLIST}")
  else()
    message(STATUS "  USE_AOT_DEVLIST       : OFF")
  endif()
  
  if(SANITIZER_OPTION)
    message(STATUS "  BUILD_WITH_SANITIZER  : ${SANITIZER_OPTION}")
  else()
    message(STATUS "  BUILD_WITH_SANITIZER  : OFF")
  endif()
  
  if (USE_ONEMKL)
    message(STATUS "  BUILD_STATIC_ONEMKL   : ${BUILD_STATIC_ONEMKL}")
  endif(USE_ONEMKL)
    message(STATUS "  BUILD_BY_PER_KERNEL   : ${BUILD_BY_PER_KERNEL}")
    message(STATUS "  BUILD_INTERNAL_DEBUG  : ${BUILD_INTERNAL_DEBUG}")
    message(STATUS "  BUILD_SEPARATE_OPS    : ${BUILD_SEPARATE_OPS}")
    message(STATUS "  BUILD_SIMPLE_TRACE    : ${BUILD_SIMPLE_TRACE}")
    message(STATUS "  BUILD_CONV_CONTIGUOUS : ${BUILD_CONV_CONTIGUOUS}")
    message(STATUS "")
    message(STATUS "********************************")
endfunction()
