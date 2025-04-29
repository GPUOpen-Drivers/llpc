##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 #
 #  Permission is hereby granted, free of charge, to any person obtaining a copy
 #  of this software and associated documentation files (the "Software"), to
 #  deal in the Software without restriction, including without limitation the
 #  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 #  sell copies of the Software, and to permit persons to whom the Software is
 #  furnished to do so, subject to the following conditions:
 #
 #  The above copyright notice and this permission notice shall be included in all
 #  copies or substantial portions of the Software.
 #
 #  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 #  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 #  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 #  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 #  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 #  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 #  IN THE SOFTWARE.
 #
 #######################################################################################################################

#
# CMake file for top level of LLPC repository.
#
# Some variables that the client might set for enabling components:
#
# - ICD_BUILD_LLPC: Enable LLPC on Vulkan and OGLP.
#
#
#
#
#

cmake_minimum_required(VERSION 3.21)

set(LLPC_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

### Standalone LLPC build handling
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(cmake/CompilerStandalone.cmake)
    llpc_standalone_pre_project_setup()
endif()

project(LLPCrepo LANGUAGES C CXX)

### Standalone LLPC build handling
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    llpc_standalone_post_project_setup()
    set(LLPC_IS_STANDALONE ON)
endif()

### Version info #######################################################################################################
# Also runs findllvm.cmake.
include(cmake/llpc_version.cmake)
add_llpc_version_projects()

include(CMakeDependentOption)

### Cached Project Options #############################################################################################
option(LLPC_BUILD_TOOLS "LLPC build all tools" OFF)

#if LLPC_RAY_TRACING
option(LLPC_RAY_TRACING "Enable raytracing" OFF)
#endif
#if LLPC_AMD_YUV_IMAGE
option(LLPC_AMD_YUV_IMAGE "Build with AMD_YUV_IMAGE" OFF)
#endif
#if LLPC_AMD_LVR_INTEROP
option(LLPC_AMD_LVR_INTEROP "Build with AMD_LVR_INTEROP" OFF)
#endif

cmake_dependent_option(LLPC_SHAREDME_XDL_BUILD_TEST "Build tests for xdl." ON "LLPC_BUILD_TOOLS" OFF)

### VKGC aspects ###################################################################
# For drivers that use VKGC, as the interface to the LLPC front-end
if (FALSE
          OR ICD_BUILD_LLPC
)
    include("cmake/vkgc.cmake")
endif()

### LLPC dependencies ##############################################################
if (ICD_BUILD_LLPC)
    # Add LLPC's dependencies (including LGC) as LLVM external projects. This needs
    # to be done before LLVM, whereas LLPC itself needs to be done after LLVM.
    include("cmake/llpcdeps.cmake")
    add_llpcdeps_projects()
endif()

### LLVM ###########################################################################
if (LLVM_EXTERNAL_PROJECTS)
    if (ICD_BUILD_LLPC AND LLPC_BUILD_TESTS)
        set(LLVM_INCLUDE_TESTS ON CACHE BOOL "Force enable LLVM_INCLUDE_TESTS to include gmock" FORCE)
    endif()
    # Build LLVM.
    include("cmake/llvm.cmake")
endif()

### Build LLPC #####################################################################
if(ICD_BUILD_LLPC)
    include("cmake/compilerutils.cmake")
    add_compilerutils_projects()

    target_include_directories(vkgc
        INTERFACE
            ${PROJECT_SOURCE_DIR}/llpc/include
            ${PROJECT_SOURCE_DIR}/lgc/include
    )

    add_subdirectory(llpc ${PROJECT_BINARY_DIR}/llpc)

    if(LLPC_BUILD_TESTS)
        add_subdirectory(test)
    endif()

    # Export the LLVM and spvgen build paths so that they are available in XGL,
    # when this is not an LLPC standalone build.
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(XGL_LLVM_BUILD_PATH ${XGL_LLVM_BUILD_PATH} PARENT_SCOPE)
        if(DEFINED XGL_SPVGEN_BUILD_PATH)
            set(XGL_SPVGEN_BUILD_PATH ${XGL_SPVGEN_BUILD_PATH} PARENT_SCOPE)
        endif()
    endif()

    target_link_libraries(vkgc INTERFACE llpc)
endif()

### Post-processing for standalone compiler build #################################
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(cmake/PostCompilerStandalone.cmake)
endif()

### Set sub library properties ####################################################
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")

    if (TARGET dumper)
        set_property(TARGET dumper_base PROPERTY FOLDER Compiler)
        set_property(TARGET dumper PROPERTY FOLDER Compiler)
    endif()

    if (TARGET vfx)
        set_property(TARGET vfx PROPERTY FOLDER Compiler)
    endif()

    if (TARGET gfxruntime)
        set_property(TARGET gfxruntime PROPERTY FOLDER Compiler)
    endif()

    if(ICD_BUILD_LLPC)
        set_property(TARGET llpc PROPERTY FOLDER Compiler)
        set_property(TARGET llpcinternal PROPERTY FOLDER Compiler)
#if LLPC_RAY_TRACING
        if(LLPC_RAY_TRACING AND NOT LLPC_IS_STANDALONE)
            set_property(TARGET vkgc_gpurtshim PROPERTY FOLDER Compiler)
        endif()
#endif
        set_property(TARGET vkgc_util PROPERTY FOLDER Compiler)
        if (LLPC_BUILD_TOOLS)
            set_property(TARGET amdllpc PROPERTY FOLDER Compiler)
        endif()

        set_property(TARGET all-targets PROPERTY FOLDER Misc)
        set_property(TARGET AMDGPU PROPERTY FOLDER Misc)
        if (TARGET benchmark)
          set_property(TARGET benchmark PROPERTY FOLDER Misc)
        endif()
        if (TARGET benchmark_main)
          set_property(TARGET benchmark_main PROPERTY FOLDER Misc)
        endif()
        set_property(TARGET distribution PROPERTY FOLDER Misc)
        set_property(TARGET Engine PROPERTY FOLDER Misc)
        set_property(TARGET install-distribution PROPERTY FOLDER Misc)
        set_property(TARGET install-distribution-stripped PROPERTY FOLDER Misc)
        set_property(TARGET LLVMSupportBlake3 PROPERTY FOLDER Misc)
        set_property(TARGET Native PROPERTY FOLDER Misc)
        set_property(TARGET NativeCodeGen PROPERTY FOLDER Misc)
        set_property(TARGET opt-viewer PROPERTY FOLDER Misc)
        if (TARGET llvm-dialects-example)
            set_property(TARGET llvm-dialects-example PROPERTY FOLDER Misc)
        endif()
        if (NOT WIN32 AND LLVM_OPTIMIZED_TABLEGEN)
            set_property(TARGET llvm_nm_target PROPERTY FOLDER Misc)
            set_property(TARGET llvm_readobj_target PROPERTY FOLDER Misc)
            set_property(TARGET llvm-min-tblgen-host PROPERTY FOLDER Misc)
            set_property(TARGET llvm-tblgen-host PROPERTY FOLDER Misc)
            set_property(TARGET CONFIGURE_LLVM_NATIVE PROPERTY FOLDER Misc)
            set_property(TARGET CREATE_LLVM_NATIVE PROPERTY FOLDER Misc)
        endif()
        if (LLPC_BUILD_TESTS)
            if(NOT LLPC_IS_STANDALONE)
                set_property(TARGET check-amber PROPERTY FOLDER "LLPC Tests")
            endif()
            set_property(TARGET check-amdllpc PROPERTY FOLDER "LLPC Tests")
            set_property(TARGET check-amdllpc-units PROPERTY FOLDER "LLPC Tests")
            set_property(TARGET check-llvmraytracing PROPERTY FOLDER "LLVMRaytracing Tests")
            set_property(TARGET check-llvmraytracing-units PROPERTY FOLDER "LLVMRaytracing Tests")
            set_property(TARGET check-lgc-units PROPERTY FOLDER "LGC Tests")
        endif()
    endif()

    if (TARGET llvm-tblgen)
        # The 32-bit llvm-tblgen can easily run out of memory. Tell the linker to allow addresses larger than 2GB.
        set_property(TARGET llvm-tblgen PROPERTY LINK_FLAGS "/LARGEADDRESSAWARE")
    endif()

endif()
