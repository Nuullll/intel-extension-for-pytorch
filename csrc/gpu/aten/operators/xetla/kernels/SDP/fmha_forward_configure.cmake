function(fmha_forward_configure XETLA_USED_ARCHS)
set(FMHA_FORWARD_KERNEL_SRCS)  # output
set(L_TYPES "fp16" "bf16")
set(L_BOOLS "false" "true")
set(BOOL_FLAG_false "f")
set(BOOL_FLAG_true "t")

# cmake arch to gpu_arch::xxxx
set(gpu_arch_xe_lpg "XeLpg")
set(gpu_arch_xe_hpg "XeHpg")
set(gpu_arch_xe_hpc "XeHpc")


foreach(arch ${XETLA_USED_ARCHS})
foreach(IMPL_T ${L_TYPES})
foreach(IMPL_KUSEALIBI ${L_BOOLS})
foreach(IMPL_KUSEBIAS ${L_BOOLS})
foreach(IMPL_KISCAUSAL ${L_BOOLS})
foreach(IMPL_KSEQLAST ${L_BOOLS})
foreach(IMPL_KISTRAINING ${L_BOOLS})
foreach(IMPL_KISDROPOUT ${L_BOOLS})
    if((arch STREQUAL "xe_lpg") AND (IMPL_T STREQUAL "bf16"))
        continue()
    endif()
    
    # currently sdp for hpg is not ready and it is dispatched to lpg impl
    if(arch STREQUAL "xe_hpg")
        set(arch "xe_lpg")
    endif()

    set(IMPL_ARCH_TAG "${gpu_arch_${arch}}")

    set(FILE_SUFFIX "${arch}_${IMPL_T}_")
    set(FILE_SUFFIX "${FILE_SUFFIX}${BOOL_FLAG_${IMPL_KUSEALIBI}}")
    set(FILE_SUFFIX "${FILE_SUFFIX}${BOOL_FLAG_${IMPL_KUSEBIAS}}")
    set(FILE_SUFFIX "${FILE_SUFFIX}${BOOL_FLAG_${IMPL_KISCAUSAL}}")
    set(FILE_SUFFIX "${FILE_SUFFIX}${BOOL_FLAG_${IMPL_KSEQLAST}}")
    set(FILE_SUFFIX "${FILE_SUFFIX}${BOOL_FLAG_${IMPL_KISTRAINING}}")
    set(FILE_SUFFIX "${FILE_SUFFIX}${BOOL_FLAG_${IMPL_KISDROPOUT}}")
    configure_file(SDP/fmha_forward_kernel.cpp.in "fmha_forward_kernel_${FILE_SUFFIX}.cpp")
    list(APPEND FMHA_FORWARD_KERNEL_SRCS "${CMAKE_CURRENT_BINARY_DIR}/fmha_forward_kernel_${FILE_SUFFIX}.cpp")
endforeach()
endforeach()
endforeach()
endforeach()
endforeach()
endforeach()
endforeach()
endforeach()

list(REMOVE_DUPLICATES FMHA_FORWARD_KERNEL_SRCS)
list(LENGTH FMHA_FORWARD_KERNEL_SRCS FMHA_FORWARD_KERNEL_SRCS_LENGTH)
message(STATUS "Generated FMHA forward kernel sources: ${FMHA_FORWARD_KERNEL_SRCS_LENGTH}")
set(FMHA_FORWARD_KERNEL_SRCS ${FMHA_FORWARD_KERNEL_SRCS} PARENT_SCOPE)
endfunction()
