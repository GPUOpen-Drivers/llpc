##
 #######################################################################################################################
 #
 #  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
 #
 #  Permission is hereby granted, free of charge, to any person obtaining a copy
 #  of this software and associated documentation files (the "Software"), to deal
 #  in the Software without restriction, including without limitation the rights
 #  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 #  copies of the Software, and to permit persons to whom the Software is
 #  furnished to do so, subject to the following conditions:
 #
 #  The above copyright notice and this permission notice shall be included in all
 #  copies or substantial portions of the Software.
 #
 #  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 #  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 #  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 #  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 #  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 #  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 #  SOFTWARE.
 #
 #######################################################################################################################

add_llvm_library(LLVMlgc)

add_dependencies(LLVMlgc
    LLVMCore
    LLVMAMDGPUAsmParser
    LLVMAMDGPUCodeGen
)

### Cached Project Options #############################################################################################

### Compiler Options ###################################################################################################
if(UNIX)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        # SEE: https://gcc.gnu.org/onlinedocs/gcc-6.2.0/gcc/Option-Summary.html#Option-Summary
        # for a list of all options and documentation.
        #target_compile_options(LLVMlgc PRIVATE option)
        target_compile_options(LLVMlgc PRIVATE
            -Wno-unused-parameter
            -Wno-type-limits
            -Wno-switch
            -Wno-parentheses
            -Wno-maybe-uninitialized
            -Wno-delete-non-virtual-dtor
            -Wno-sign-compare
            -Wno-delete-incomplete
            -Wunused-variable
            -Werror=unused-variable
            -Wunused-function
            -Werror=unused-function
            -Wno-pedantic
            -Wno-extra
        )

        target_compile_options(LLVMlgc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -Wno-unused
            -Wno-ignored-qualifiers
            -Wno-missing-field-initializers
            -Wno-invalid-offsetof           # offsetof within non-standard-layout type 'x' is undefined
        >)

        target_compile_options(LLVMlgc PRIVATE -fno-strict-aliasing)

        target_compile_options(LLVMlgc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -std=c++14
            -fno-rtti
            -fPIC
        >)

        message(STATUS "Configured LLVMlgc compiler options for GCC.")
    elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        target_compile_options(LLVMlgc PRIVATE
            -fvisibility-inlines-hidden
            -fcolor-diagnostics
            -Wall
            -Werror
            -Wno-invalid-offsetof
            -Wno-missing-braces
            -Wno-nested-anon-types
            -Wno-gnu-anonymous-struct
            -Wno-covered-switch-default
            -Wno-missing-field-initializers
            -Wno-extra-semi
            -Wno-sign-compare
            -Wno-gnu-anonymous-struct
        )
        target_compile_options(LLVMlgc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -std=c++14
            -fno-rtti
            -fPIC
        >)
        #message(STATUS "Configured compiler options for Clang.")
        message(WARNING "Clang is untested.")
    else()
        message(FATAL_ERROR "Using unknown compiler.")
    endif()
elseif(WIN32)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
        # CMAKE-TODO: These are /W4 (level 4) warnings
        target_compile_options(LLVMlgc
            PRIVATE # Warnings in interface and src
                /wd4005 # 'DEBUG' : macro redefinition ??? Defined in toolchain ??? importedllvmincludellvm/Support/Debug.h
                /wd4018 # '<' : signed/unsigned mismatch
                /wd4100 # unreferenced formal parameter
                /wd4127 # conditional expression is constant
                /wd4141 # 'inline' : used more than once
                /wd4146 # unary minus operator applied to unsigned type, result still unsigned
                /wd4189 # local variable is initialized but not referenced
                /wd4201 # nonstandard extension used : nameless struct/union
                /wd4244 # 'X' : conversion from 'Y' to 'Z', possible loss of data
                /wd4245 # 'X' : conversion from 'Y' to 'Z', signed/unsigned mismatch
                /wd4250 # 'X': inherits 'Y' via dominance
                /wd4291 # no matching operator delete found; memory will not be freed if initialization throws an exception
                /wd4267 # 'return' : conversion from 'size_t' to 'unsigned int', possible loss of data
                /wd4389 # '==' : signed/unsigned mismatch
                /wd4505 # unreferenced local function has been removed
                /wd4510 # default constructor could not be generated
                /wd4512 # assignment operator could not be generated
                /wd4610 # struct 'X' can never be instantiated - user defined constructor required
                /wd4624 # destructor could not be generated because a base class destructor is inaccessible or deleted
                /wd4702 # unreachable code
                /wd4706 # assignment within conditional expression
                /wd4800 # forcing value to bool 'true' or 'false' (performance warning)
                /wd6246 # Local declaration of 'S' hides declaration of the same name in outer scope
                /wd6323 # Use of arithmetic operator on Boolean type(s)
        )

        target_compile_definitions(LLVMlgc PRIVATE _SCL_SECURE_NO_WARNINGS)
        target_compile_definitions(LLVMlgc PRIVATE _CRT_SECURE_NO_WARNINGS)

        message(STATUS "Configured LLVMlgc compiler options for MSVC.")
    else()
        message(FATAL_ERROR "Using unknown compiler")
    endif()
endif()

### Defines/Includes/Sources ###########################################################################################
target_compile_definitions(LLVMlgc PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)
if (LLPC_CLIENT_INTERFACE_MAJOR_VERSION)
    target_compile_definitions(LLVMlgc PRIVATE LLPC_CLIENT_INTERFACE_MAJOR_VERSION=${LLPC_CLIENT_INTERFACE_MAJOR_VERSION})
endif()
target_compile_definitions(LLVMlgc PRIVATE
        LITTLEENDIAN_CPU
        CHIP_HDR_GFX10
        CHIP_HDR_VEGA20
        CHIP_HDR_RAVEN2
        CHIP_HDR_NAVI14
        )

if(WIN32)
    target_compile_definitions(LLVMlgc PRIVATE
        NOMINMAX    # windows.h defines min/max which conflicts with the use of std::min / max
        UNICODE     # CMAKE-TODO: What is this used for?
        _UNICODE
    )
endif()

target_include_directories(LLVMlgc
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/builder
    ${CMAKE_CURRENT_SOURCE_DIR}/imported/chip/gfx6
    ${CMAKE_CURRENT_SOURCE_DIR}/imported/chip/gfx9
    ${CMAKE_CURRENT_SOURCE_DIR}/patch
    ${CMAKE_CURRENT_SOURCE_DIR}/patch/gfx6/chip
    ${CMAKE_CURRENT_SOURCE_DIR}/patch/gfx9/chip
    ${CMAKE_CURRENT_SOURCE_DIR}/patch/gfx9
    ${CMAKE_CURRENT_SOURCE_DIR}/patch/generate
    ${CMAKE_CURRENT_SOURCE_DIR}/util
)

# lgc/builder
target_sources(LLVMlgc PRIVATE
    builder/llpcBuilder.cpp
    builder/llpcBuilderBase.cpp
    builder/llpcBuilderContext.cpp
    builder/llpcBuilderImpl.cpp
    builder/llpcBuilderImplArith.cpp
    builder/llpcBuilderImplDesc.cpp
    builder/llpcBuilderImplImage.cpp
    builder/llpcBuilderImplInOut.cpp
    builder/llpcBuilderImplMatrix.cpp
    builder/llpcBuilderImplMisc.cpp
    builder/llpcBuilderImplSubgroup.cpp
    builder/llpcBuilderRecorder.cpp
    builder/llpcBuilderReplayer.cpp
    builder/llpcPipelineState.cpp
    builder/llpcShaderModes.cpp
    builder/llpcYCbCrAddressHandler.cpp
    builder/llpcTargetInfo.cpp
)

# lgc/patch
target_sources(LLVMlgc PRIVATE
    patch/gfx6/chip/llpcGfx6Chip.cpp
    patch/gfx6/chip/llpcGfx6ConfigBuilder.cpp
    patch/gfx9/chip/llpcGfx9Chip.cpp
    patch/gfx9/chip/llpcGfx9ConfigBuilder.cpp
    patch/gfx9/llpcNggLdsManager.cpp
    patch/gfx9/llpcNggPrimShader.cpp
    patch/gfx9/llpcShaderMerger.cpp
    patch/llpcCodeGenManager.cpp
    patch/llpcConfigBuilderBase.cpp
    patch/llpcFragColorExport.cpp
    patch/llpcPatch.cpp
    patch/llpcPatchBufferOp.cpp
    patch/llpcPatchCheckShaderCache.cpp
    patch/llpcPatchCopyShader.cpp
    patch/llpcPatchDescriptorLoad.cpp
    patch/llpcPatchEntryPointMutate.cpp
    patch/llpcPatchInOutImportExport.cpp
    patch/llpcPatchIntrinsicSimplify.cpp
    patch/llpcPatchLlvmIrInclusion.cpp
    patch/llpcPatchLoadScalarizer.cpp
    patch/llpcPatchNullFragShader.cpp
    patch/llpcPatchPeepholeOpt.cpp
    patch/llpcPatchPreparePipelineAbi.cpp
    patch/llpcPatchPushConstOp.cpp
    patch/llpcPatchResourceCollect.cpp
    patch/llpcPatchSetupTargetFeatures.cpp
    patch/llpcSystemValues.cpp
    patch/llpcVertexFetch.cpp
)

# lgc/util
target_sources(LLVMlgc PRIVATE
    util/llpcBuilderDebug.cpp
    util/llpcGfxRegHandlerBase.cpp
    util/llpcGfxRegHandler.cpp
    util/llpcInternal.cpp
    util/llpcPassManager.cpp
    util/llpcPipelineShaders.cpp
    util/llpcStartStopTimer.cpp
)

