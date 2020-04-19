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
option(LLPC_BUILD_RENOIR  "LLPC support for Renoir?"    ON)
option(LLPC_ENABLE_WERROR "Build LLPC with more errors" OFF)

### Compiler Options ###################################################################################################
function(lgc_set_compiler_options PROJECT_NAME)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(LLPC_ENABLE_WERROR)
            target_compile_options("${PROJECT_NAME}" PRIVATE
                -Werror
                -Wno-error=deprecated-declarations
            )
        endif()

        # SEE: https://gcc.gnu.org/onlinedocs/gcc-6.2.0/gcc/Option-Summary.html#Option-Summary
        # for a list of all options and documentation.
        target_compile_options(${PROJECT_NAME} PRIVATE
            -fno-strict-aliasing
            -fvisibility-inlines-hidden
            -Wall
            -Wno-delete-incomplete
            -Wno-delete-non-virtual-dtor
            -Wno-invalid-offsetof
            -Wno-missing-braces
            -Wno-missing-field-initializers
            -Wno-parentheses
            -Wno-sign-compare
            -Wno-switch
            -Wno-type-limits
            -Wno-unused-parameter
            -Wunused-variable
            -Werror=unused-variable
            -Wunused-function
            -Werror=unused-function
        )

        target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -fno-rtti
            -fPIC
            -std=c++14
            -Wno-ignored-qualifiers
            -Wno-missing-field-initializers
            -Wno-invalid-offsetof           # offsetof within non-standard-layout type 'x' is undefined
        >)

        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options("${PROJECT_NAME}" PRIVATE
                -Wno-extra
                -Wno-maybe-uninitialized
                -Wno-pedantic
            )
            if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8.4)
                target_compile_options("${PROJECT_NAME}" PRIVATE
                    -Wno-class-memaccess
                )
            endif()
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options("${PROJECT_NAME}" PRIVATE
                -Wno-covered-switch-default
                -Wno-extra-semi
                -Wno-gnu-anonymous-struct
                -Wno-nested-anon-types
            )
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # CMAKE-TODO: These are /W4 (level 4) warnings
        target_compile_options(${PROJECT_NAME}
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
                /wd4589 # constructor of abstract class 'type' ignores initializer for virtual base class 'type'
                /wd4610 # struct 'X' can never be instantiated - user defined constructor required
                /wd4624 # destructor could not be generated because a base class destructor is inaccessible or deleted
                /wd4702 # unreachable code
                /wd4706 # assignment within conditional expression
                /wd4800 # forcing value to bool 'true' or 'false' (performance warning)
                /wd6246 # Local declaration of 'S' hides declaration of the same name in outer scope
                /wd6323 # Use of arithmetic operator on Boolean type(s)
        )

        target_compile_definitions(${PROJECT_NAME} PRIVATE _SCL_SECURE_NO_WARNINGS)
        target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        message(FATAL_ERROR "Using unknown compiler")
    endif()
endfunction()
lgc_set_compiler_options(LLVMlgc)

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

if (LLPC_BUILD_RENOIR)
    target_compile_definitions(LLVMlgc PRIVATE LLPC_BUILD_RENOIR)
    target_compile_definitions(LLVMlgc PRIVATE CHIP_HDR_RENOIR)
endif()

if(WIN32)
    target_compile_definitions(LLVMlgc PRIVATE
        NOMINMAX    # windows.h defines min/max which conflicts with the use of std::min / max
        UNICODE     # CMAKE-TODO: What is this used for?
        _UNICODE
    )
endif()

target_include_directories(LLVMlgc
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/interface
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/imported
)

# lgc/builder
target_sources(LLVMlgc PRIVATE
    builder/ArithBuilder.cpp
    builder/Builder.cpp
    builder/BuilderBase.cpp
    builder/BuilderImpl.cpp
    builder/BuilderRecorder.cpp
    builder/BuilderReplayer.cpp
    builder/DescBuilder.cpp
    builder/ImageBuilder.cpp
    builder/InOutBuilder.cpp
    builder/MatrixBuilder.cpp
    builder/MiscBuilder.cpp
    builder/SubgroupBuilder.cpp
    builder/YCbCrAddressHandler.cpp
    builder/YCbCrConverter.cpp
)

# lgc/patch
target_sources(LLVMlgc PRIVATE
    patch/ConfigBuilderBase.cpp
    patch/FragColorExport.cpp
    patch/Gfx6Chip.cpp
    patch/Gfx6ConfigBuilder.cpp
    patch/Gfx9Chip.cpp
    patch/Gfx9ConfigBuilder.cpp
    patch/NggLdsManager.cpp
    patch/NggPrimShader.cpp
    patch/Patch.cpp
    patch/PatchBufferOp.cpp
    patch/PatchCheckShaderCache.cpp
    patch/PatchCopyShader.cpp
    patch/PatchDescriptorLoad.cpp
    patch/PatchEntryPointMutate.cpp
    patch/PatchInOutImportExport.cpp
    patch/PatchIntrinsicSimplify.cpp
    patch/PatchLlvmIrInclusion.cpp
    patch/PatchLoadScalarizer.cpp
    patch/PatchNullFragShader.cpp
    patch/PatchPeepholeOpt.cpp
    patch/PatchPreparePipelineAbi.cpp
    patch/PatchPushConstOp.cpp
    patch/PatchResourceCollect.cpp
    patch/PatchSetupTargetFeatures.cpp
    patch/ShaderMerger.cpp
    patch/SystemValues.cpp
    patch/VertexFetch.cpp
)

# lgc/state
target_sources(LLVMlgc PRIVATE
    state/Compiler.cpp
    state/LgcContext.cpp
    state/PalMetadata.cpp
    state/PipelineShaders.cpp
    state/PipelineState.cpp
    state/ResourceUsage.cpp
    state/ShaderModes.cpp
    state/ShaderStage.cpp
    state/TargetInfo.cpp
)

# lgc/util
target_sources(LLVMlgc PRIVATE
    util/Debug.cpp
    util/GfxRegHandlerBase.cpp
    util/GfxRegHandler.cpp
    util/Internal.cpp
    util/PassManager.cpp
    util/StartStopTimer.cpp
)

add_subdirectory(tool/lgc)
add_subdirectory(test)
