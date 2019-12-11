##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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

project(LLPC C CXX)

### Create LLPC Library ################################################################################################
add_library(llpc STATIC "")

if(ICD_BUILD_LLPC)
    add_dependencies(llpc LLVMCore)
endif()

### Cached Project Options #############################################################################################
option(LLPC_BUILD_VEGA20   "LLPC support for Vega20?"   ON)
option(LLPC_BUILD_RAVEN2   "LLPC support for Raven2?"   ON)
option(LLPC_BUILD_LIT      "LLPC build lit test"        OFF)
#if VKI_BUILD_GFX10
option(LLPC_BUILD_GFX10   "LLPC support for GFX10?"   OFF)
option(LLPC_BUILD_NAVI14   "LLPC support for NAVI14?"   OFF)
#endif
# None

if(ICD_BUILD_LLPC)
    set(AMDLLPC_DIR ${CMAKE_CURRENT_BINARY_DIR} )
endif()

### Compiler Options ###################################################################################################
if(UNIX)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        # SEE: https://gcc.gnu.org/onlinedocs/gcc-6.2.0/gcc/Option-Summary.html#Option-Summary
        # for a list of all options and documentation.
        #target_compile_options(llpc PRIVATE option)
        target_compile_options(llpc PRIVATE
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
        )

        target_compile_options(llpc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -Wno-unused
            -Wno-ignored-qualifiers
            -Wno-missing-field-initializers
            -Wno-invalid-offsetof           # offsetof within non-standard-layout type 'x' is undefined
        >)

        target_compile_options(llpc PRIVATE -fno-strict-aliasing)

        target_compile_options(llpc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -std=c++14
            -fno-rtti
            -fPIC
        >)

        message(STATUS "Configured ${PROJECT_NAME} compiler options for GCC.")
    elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        target_compile_options(llpc PRIVATE
            -fvisibility-inlines-hidden
            -fcolor-diagnostics
            -Wall
            -Werror
            -Wno-invalid-offsetof
            -Wno-missing-braces
        )
        target_compile_options(llpc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
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
        target_compile_options(llpc
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

        target_compile_definitions(llpc PRIVATE _SCL_SECURE_NO_WARNINGS)
        target_compile_definitions(llpc PRIVATE _CRT_SECURE_NO_WARNINGS)

        message(STATUS "Configured ${PROJECT_NAME} compiler options for MSVC.")
    else()
        message(FATAL_ERROR "Using unknown compiler")
    endif()
endif()

if (LLPC_BUILD_VEGA20)
    target_compile_definitions(llpc PRIVATE LLPC_BUILD_VEGA20)
    target_compile_definitions(llpc PRIVATE CHIP_HDR_VEGA20)
endif()

if (LLPC_BUILD_RAVEN2)
    target_compile_definitions(llpc PRIVATE LLPC_BUILD_RAVEN2)
    target_compile_definitions(llpc PRIVATE CHIP_HDR_RAVEN2)
endif()

### Defines/Includes/Sources ###########################################################################################
if(ICD_BUILD_LLPC)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${XGL_LLVM_BUILD_PATH}/lib/cmake/llvm)
    include(LLVMConfig)
    message(STATUS "LLVM execuables: " ${LLVM_TOOLS_BINARY_DIR})
    message(STATUS "LLVM libraries: " ${LLVM_BUILD_LIBRARY_DIR})
    execute_process(
        COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-config --libs amdgpu analysis bitreader bitwriter codegen irreader linker mc passes support target transformutils
        OUTPUT_VARIABLE LLVM_LINK_FLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    message(STATUS "LLVM link options:" ${LLVM_LINK_FLAGS})
endif()
target_compile_definitions(llpc PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)
target_compile_definitions(llpc PRIVATE _SPIRV_LLVM_API)
target_compile_definitions(llpc PRIVATE PAL_CLIENT_INTERFACE_MAJOR_VERSION=${PAL_CLIENT_INTERFACE_MAJOR_VERSION})
if (LLPC_CLIENT_INTERFACE_MAJOR_VERSION)
    target_compile_definitions(llpc PRIVATE LLPC_CLIENT_INTERFACE_MAJOR_VERSION=${LLPC_CLIENT_INTERFACE_MAJOR_VERSION})
endif()
if(ICD_BUILD_LLPC)
    target_compile_definitions(llpc PRIVATE ICD_BUILD_LLPC)
endif()

#if VKI_BUILD_GFX10
if(LLPC_BUILD_GFX10)
  target_compile_definitions(llpc PRIVATE
      LLPC_BUILD_GFX10=1
      CHIP_HDR_GFX10=1
      PAL_BUILD_GFX9=1
      PAL_BUILD_GFX10=1
  )
  if(LLPC_BUILD_NAVI14)
    target_compile_definitions(llpc PRIVATE
        LLPC_BUILD_NAVI14=1
        CHIP_HDR_NAVI14=1
    )
  endif()
endif()
#endif

if(ICD_BUILD_LLPC)
    if(XGL_LLVM_UPSTREAM)
        target_compile_definitions(llpc PRIVATE XGL_LLVM_UPSTREAM=1)
    endif()
endif()

if(WIN32)
    target_compile_definitions(llpc PRIVATE
        NOMINMAX    # windows.h defines min/max which conflicts with the use of std::min / max
        UNICODE     # CMAKE-TODO: What is this used for?
        _UNICODE
    )
endif()

target_include_directories(llpc
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/builder
        ${PROJECT_SOURCE_DIR}/context
        ${PROJECT_SOURCE_DIR}/imported/chip/gfx6
        ${PROJECT_SOURCE_DIR}/imported/chip/gfx9
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/lower
        ${PROJECT_SOURCE_DIR}/patch
        ${PROJECT_SOURCE_DIR}/patch/gfx6/chip
        ${PROJECT_SOURCE_DIR}/patch/gfx9/chip
        ${PROJECT_SOURCE_DIR}/patch/gfx9
        ${PROJECT_SOURCE_DIR}/patch/generate
        ${PROJECT_SOURCE_DIR}/translator/include
        ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV
        ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV/libSPIRV
        ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV/Mangler
        ${PROJECT_SOURCE_DIR}/util
        ${XGL_PAL_PATH}/inc/core
        ${XGL_PAL_PATH}/inc/util
        ${LLVM_INCLUDE_DIRS}
)

# CMAKE-TODO: Figure out a better way to do this.
target_include_directories(llpc PRIVATE ${XGL_ICD_PATH}/api/include/khronos)

if(WIN32)
    target_compile_definitions(llpc PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

if(ICD_BUILD_LLPC)
# llpc/builder
    target_sources(llpc PRIVATE
        builder/llpcBuilder.cpp
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
    )

# llpc/context
    target_sources(llpc PRIVATE
        context/llpcCompiler.cpp
        context/llpcContext.cpp
        context/llpcComputeContext.cpp
        context/llpcGraphicsContext.cpp
        context/llpcShaderCache.cpp
        context/llpcPipelineContext.cpp
        context/llpcShaderCacheManager.cpp
    )

# llpc/lower
    target_sources(llpc PRIVATE
        lower/llpcSpirvLower.cpp
        lower/llpcSpirvLowerAccessChain.cpp
        lower/llpcSpirvLowerAlgebraTransform.cpp
        lower/llpcSpirvLowerConstImmediateStore.cpp
        lower/llpcSpirvLowerGlobal.cpp
        lower/llpcSpirvLowerInstMetaRemove.cpp
        lower/llpcSpirvLowerLoopUnrollControl.cpp
        lower/llpcSpirvLowerMemoryOp.cpp
        lower/llpcSpirvLowerResourceCollect.cpp
        lower/llpcSpirvLowerTranslator.cpp
    )

# llpc/patch
    target_sources(llpc PRIVATE
        patch/gfx6/chip/llpcGfx6Chip.cpp
        patch/gfx6/chip/llpcGfx6ConfigBuilder.cpp
        patch/gfx9/chip/llpcGfx9Chip.cpp
        patch/gfx9/chip/llpcGfx9ConfigBuilder.cpp
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
#if VKI_BUILD_GFX10
    if(LLPC_BUILD_GFX10)
        target_sources(llpc PRIVATE
            patch/gfx9/llpcNggLdsManager.cpp
            patch/gfx9/llpcNggPrimShader.cpp
        )
    endif()
#endif

# llpc/translator
    target_sources(llpc PRIVATE
        translator/lib/SPIRV/OCLUtil.cpp
        translator/lib/SPIRV/SPIRVLowerBool.cpp
        translator/lib/SPIRV/SPIRVLowerConstExpr.cpp
        translator/lib/SPIRV/SPIRVLowerMemmove.cpp
        translator/lib/SPIRV/SPIRVReader.cpp
        translator/lib/SPIRV/SPIRVRegularizeLLVM.cpp
        translator/lib/SPIRV/SPIRVUtil.cpp
    )

    target_sources(llpc PRIVATE
        translator/lib/SPIRV/libSPIRV/SPIRVBasicBlock.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVDebug.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVDecorate.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVEntry.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVFunction.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVInstruction.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVModule.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVStream.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVType.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVValue.cpp
    )

    target_sources(llpc PRIVATE
        translator/lib/SPIRV/Mangler/FunctionDescriptor.cpp
        translator/lib/SPIRV/Mangler/Mangler.cpp
        translator/lib/SPIRV/Mangler/ManglingUtils.cpp
        translator/lib/SPIRV/Mangler/ParameterType.cpp
    )

# llpc/util
    target_sources(llpc PRIVATE
        util/llpcDebug.cpp
        util/llpcElfReader.cpp
        util/llpcElfWriter.cpp
        util/llpcEmuLib.cpp
        util/llpcInternal.cpp
        util/llpcFile.cpp
        util/llpcPassDeadFuncRemove.cpp
        util/llpcPassManager.cpp
        util/llpcPipelineDumper.cpp
        util/llpcPipelineShaders.cpp
        util/llpcShaderModuleHelper.cpp
        util/llpcStartStopTimer.cpp
        util/llpcTimerProfiler.cpp
        util/llpcUtil.cpp
    )
else()
    target_sources(llpc PRIVATE
        util/llpcElfReader.cpp
        util/llpcPipelineDumper.cpp
        util/llpcUtil.cpp
        patch/gfx6/chip/llpcGfx6Chip.cpp
        patch/gfx9/chip/llpcGfx9Chip.cpp
    )
endif()

# llpc/imported/metrohash
if(NOT TARGET metrohash)
    add_subdirectory(imported/metrohash)
    target_link_libraries(llpc PRIVATE llpc_metrohash)
else()
    target_link_libraries(llpc PRIVATE metrohash)
    target_compile_definitions(llpc PRIVATE SINGLE_EXTERNAL_METROHASH)
endif()

target_link_libraries(llpc PRIVATE cwpack)

### LLPC Auto-generated Files ##########################################################################################
if(ICD_BUILD_LLPC)
set(OP_EMU_LIB_GEN_DIR ${PROJECT_SOURCE_DIR}/patch/generate)
set(GEN_GLSL_OP_EMU_LIB_SCRIPT ${OP_EMU_LIB_GEN_DIR}/script/genGlslEmuLib.py)

if(UNIX)
    set(BUILD_OS lnx)
elseif(WIN32)
    set(BUILD_OS win)
endif()

if(UNIX)
    set(LLVM_BIN_DIR ${XGL_LLVM_BUILD_PATH}/bin/)
elseif(WIN32)
    set(LLVM_BIN_DIR ${XGL_LLVM_BUILD_PATH}/\$\(Configuration\)/bin/)
endif()

endif()
### Link Libraries #####################################################################################################
# CMAKE-TODO: LLVM_LIB_DIR should probably be set in the op level CMake?
# Maybe add XGL_LLVM_PATH?
# How are these built? Can they be built through CMake?

### Visual Studio Filters ##############################################################################################
target_vs_filters(llpc)

### VFX library for Standalone Compiler ###################################################################################
if(ICD_BUILD_LLPC)
add_library(vfx STATIC "")

target_compile_definitions(vfx PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)

if (LLPC_CLIENT_INTERFACE_MAJOR_VERSION)
    target_compile_definitions(vfx PRIVATE LLPC_CLIENT_INTERFACE_MAJOR_VERSION=${LLPC_CLIENT_INTERFACE_MAJOR_VERSION})
    target_compile_definitions(vfx PRIVATE PAL_CLIENT_INTERFACE_MAJOR_VERSION=${PAL_CLIENT_INTERFACE_MAJOR_VERSION})
endif()

target_sources(vfx PRIVATE
    tool/vfx/vfxParser.cpp
    tool/vfx/vfxPipelineDoc.cpp
    tool/vfx/vfxRenderDoc.cpp
    tool/vfx/vfxSection.cpp
    tool/vfx/vfxEnumsConverter.cpp
)

#if VKI_BUILD_GFX10
if(LLPC_BUILD_GFX10)
  target_compile_definitions(vfx PRIVATE
      VKI_BUILD_GFX10=1
      LLPC_BUILD_GFX10=1
  )
endif()
#endif

target_include_directories(vfx
PUBLIC
    ${PROJECT_SOURCE_DIR}/tool/vfx
PRIVATE
    ${PROJECT_SOURCE_DIR}/imported/spirv
    ${PROJECT_SOURCE_DIR}/include
    ${XGL_ICD_PATH}/api/include/khronos
)

if(UNIX)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        target_compile_options(vfx PRIVATE -fno-strict-aliasing)
        target_compile_options(vfx PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++14 -fno-rtti>)

        target_compile_options(vfx PRIVATE -Wno-unused-parameter -Wno-shift-negative-value -Wno-type-limits -Wno-error=switch -Wno-error=sign-compare -Wno-error=parentheses -Wno-error=maybe-uninitialized -Wno-error=delete-non-virtual-dtor -Wno-sign-compare -Wno-error -Wunused-variable -Werror=unused-variable -Wunused-function -Werror=unused-function)
        target_compile_options(vfx PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused -Wno-unused-parameter -Wno-ignored-qualifiers -Wno-missing-field-initializers>)

        message(STATUS "Configured ${PROJECT_NAME} compiler options for GCC.")
    elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        target_compile_options(vfx PRIVATE
            -fvisibility-inlines-hidden
            -fcolor-diagnostics
            -Wall
            -Werror
        )
        target_compile_options(vfx PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -std=c++14
            -fno-rtti
        >)
        #message(STATUS "Configured compiler options for Clang.")
        message(WARNING "Clang is untested.")
    else()
        message(FATAL_ERROR "Using unknown compiler.")
    endif()
endif()
endif()
### Create Standalone Compiler ############################################################################################
if(ICD_BUILD_LLPC)
add_executable(amdllpc
    tool/amdllpc.cpp
    tool/llpcAutoLayout.cpp
)
add_dependencies(amdllpc llpc)

target_compile_definitions(amdllpc PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)
target_compile_definitions(amdllpc PRIVATE _SPIRV_LLVM_API)
if (LLPC_CLIENT_INTERFACE_MAJOR_VERSION)
    target_compile_definitions(amdllpc PRIVATE LLPC_CLIENT_INTERFACE_MAJOR_VERSION=${LLPC_CLIENT_INTERFACE_MAJOR_VERSION})
    target_compile_definitions(amdllpc PRIVATE PAL_CLIENT_INTERFACE_MAJOR_VERSION=${PAL_CLIENT_INTERFACE_MAJOR_VERSION})
endif()

target_compile_definitions(amdllpc PRIVATE ICD_BUILD_LLPC)

target_include_directories(amdllpc
PUBLIC
    ${PROJECT_SOURCE_DIR}/include
PRIVATE
    ${PROJECT_SOURCE_DIR}/context
    ${PROJECT_SOURCE_DIR}/imported/spirv
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/lower
    ${PROJECT_SOURCE_DIR}/patch
    ${PROJECT_SOURCE_DIR}/patch/gfx6/chip
    ${PROJECT_SOURCE_DIR}/patch/gfx9/chip
    ${PROJECT_SOURCE_DIR}/patch/generate
    ${PROJECT_SOURCE_DIR}/translator/include
    ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV
    ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV/libSPIRV
    ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV/Mangler
    ${PROJECT_SOURCE_DIR}/util
    ${XGL_PAL_PATH}/src/core/hw/gfxip/gfx6/chip
    ${XGL_PAL_PATH}/src/core/hw/gfxip/gfx9/chip
    ${XGL_PAL_PATH}/inc/core
    ${XGL_PAL_PATH}/inc/util
    ${LLVM_INCLUDE_DIRS}
)

#if VKI_BUILD_GFX10
if(LLPC_BUILD_GFX10)
  target_compile_definitions(amdllpc PRIVATE
      LLPC_BUILD_GFX10=1
      CHIP_HDR_GFX10=1
      PAL_BUILD_GFX9=1
      PAL_BUILD_GFX10=1
  )
endif()
#endif

set(VULKAN_HEADER_PATH ${XGL_ICD_PATH}/api/include/khronos)
target_include_directories(amdllpc PRIVATE ${VULKAN_HEADER_PATH})

if(UNIX)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        target_compile_options(amdllpc PRIVATE -fno-strict-aliasing)
        target_compile_options(amdllpc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++14 -fno-rtti>)

        target_compile_options(amdllpc PRIVATE -Wno-unused-parameter -Wno-shift-negative-value -Wno-type-limits -Wno-error=switch -Wno-error=sign-compare -Wno-error=parentheses -Wno-error=maybe-uninitialized -Wno-error=delete-non-virtual-dtor -Wno-sign-compare -Wno-error -Wunused-variable -Werror=unused-variable -Wunused-function -Werror=unused-function)
        target_compile_options(amdllpc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused -Wno-unused-parameter -Wno-ignored-qualifiers -Wno-missing-field-initializers>)

        message(STATUS "Configured ${PROJECT_NAME} compiler options for GCC.")
    elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        target_compile_options(amdllpc PRIVATE
            -fvisibility-inlines-hidden
            -fcolor-diagnostics
            -Wall
            -Werror
            -Wno-missing-braces
        )
        target_compile_options(amdllpc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -std=c++14
            -fno-rtti
        >)
        #message(STATUS "Configured compiler options for Clang.")
        message(WARNING "Clang is untested.")
    else()
        message(FATAL_ERROR "Using unknown compiler.")
    endif()
endif()

if(UNIX)
    target_link_libraries(amdllpc PRIVATE llpc vfx dl stdc++)
elseif(WIN32)
    target_link_libraries(amdllpc PRIVATE llpc vfx)
endif()
    llvm_map_components_to_libnames(llvm_libs amdgpucodegen amdgpuinfo amdgpuasmparser amdgpudisassembler LTO ipo analysis bitreader bitwriter codegen irreader linker mc passes support target transformutils coroutines aggressiveinstcombine)
target_link_libraries(amdllpc PRIVATE ${llvm_libs})
target_link_libraries(amdllpc PRIVATE cwpack)
endif()
### Add Subdirectories #################################################################################################
if(ICD_BUILD_LLPC)
# SPVGEN
set(XGL_SPVGEN_PATH ${PROJECT_SOURCE_DIR}/../spvgen CACHE PATH "Specify the path to SPVGEN.")

if(EXISTS ${XGL_SPVGEN_PATH})
    add_subdirectory(${XGL_SPVGEN_PATH} ${CMAKE_BINARY_DIR}/spvgen EXCLUDE_FROM_ALL)
endif()

# Lit tests
if(LLPC_BUILD_LIT)
    add_subdirectory(test)
endif()
endif()

