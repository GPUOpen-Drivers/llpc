##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
  add_dependencies(llpc LLVMlgc)
endif()

### Cached Project Options #############################################################################################
option(LLPC_BUILD_LIT        "LLPC build lit test"         OFF)
option(LLPC_BUILD_LLVM_TOOLS "Build LLVM tools"            OFF)
option(LLPC_ENABLE_WERROR    "Build LLPC with more errors" OFF)

if(ICD_BUILD_LLPC)
    set(AMDLLPC_DIR ${CMAKE_CURRENT_BINARY_DIR})
endif()

### Set Options and build LLVM #########################################################################################
if(ICD_BUILD_LLPC)
    # Set LLVM options and build LLVM
    # Add LGC as an LLVM external project, but only if its CMakeLists.txt exists.
    set(LLVM_EXTERNAL_PROJECTS lgc)
    set(LLVM_EXTERNAL_LGC_SOURCE_DIR ${PROJECT_SOURCE_DIR}/../lgc)

    # Set other LLVM settings.
    set(LLVM_ENABLE_ASSERTIONS ${CMAKE_BUILD_TYPE_DEBUG} CACHE BOOL Force)
    set(LLVM_TARGETS_TO_BUILD AMDGPU CACHE STRING Force)
    set(LLVM_BUILD_TESTS OFF CACHE BOOL Force)
    set(LLVM_BUILD_TOOLS ${LLPC_BUILD_LLVM_TOOLS} CACHE BOOL Force)
    set(LLVM_BUILD_UTILS OFF CACHE BOOL Force)
    set(LLVM_INCLUDE_DOCS OFF CACHE BOOL Force)
    set(LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL Force)
    set(LLVM_INCLUDE_GO_TESTS OFF CACHE BOOL Force)
    set(LLVM_INCLUDE_TESTS OFF CACHE BOOL Force)
    set(LLVM_INCLUDE_TOOLS ON CACHE BOOL Force)
    set(LLVM_INCLUDE_UTILS ON CACHE BOOL Force)
    set(LLVM_ENABLE_TERMINFO OFF CACHE BOOL Force)
    set(LLVM_ENABLE_ZLIB OFF CACHE BOOL Force)
    set(LLVM_OPTIMIZED_TABLEGEN ON CACHE BOOL Force)

    if(EXISTS ${PROJECT_SOURCE_DIR}/../../llvm-project/llvm)
        set(XGL_LLVM_SRC_PATH ${PROJECT_SOURCE_DIR}/../../llvm-project/llvm CACHE PATH "Specify the path to the LLVM.")
    elseif(EXISTS ${PROJECT_SOURCE_DIR}/../../../imported/llvm-project/llvm)
        set(XGL_LLVM_SRC_PATH ${PROJECT_SOURCE_DIR}/../../../imported/llvm-project/llvm CACHE PATH "Specify the path to the LLVM.")
    endif()

    add_subdirectory(${XGL_LLVM_SRC_PATH} ${PROJECT_BINARY_DIR}/llvm)
    set(XGL_LLVM_BUILD_PATH ${PROJECT_BINARY_DIR}/llvm)
    # Export the LLVM build path so that it's available in XGL.
    set(XGL_LLVM_BUILD_PATH ${XGL_LLVM_BUILD_PATH} PARENT_SCOPE)

    llvm_map_components_to_libnames(llvm_libs
        lgc
        AMDGPUAsmParser
        AMDGPUCodeGen
        AMDGPUDisassembler
        AMDGPUInfo
        Analysis
        BinaryFormat
        Core
        Coroutines
        LTO
        ipo
        BitReader
        BitWriter
        CodeGen
        InstCombine
        IRReader
        Linker
        MC
        Passes
        ScalarOpts
        Support
        Target
        TransformUtils
    )
    target_link_libraries(llpc PUBLIC ${llvm_libs})
endif()

### Compiler Options ###################################################################################################
include(../cmake/CompilerFlags.cmake)
set_compiler_options(llpc ${LLPC_ENABLE_WERROR})

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

option(LLPC_ENABLE_SHADER_CACHE "Enable experimental shader cache" OFF)
if(LLPC_ENABLE_SHADER_CACHE)
    target_compile_definitions(llpc PRIVATE LLPC_ENABLE_SHADER_CACHE=1)
endif()

if(ICD_BUILD_LLPC)
    if(XGL_LLVM_UPSTREAM)
        target_compile_definitions(llpc PRIVATE XGL_LLVM_UPSTREAM)
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
        ${PROJECT_SOURCE_DIR}/../lgc/interface
    PRIVATE
        ${PROJECT_SOURCE_DIR}/context
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/lower
        ${PROJECT_SOURCE_DIR}/translator/include
        ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV
        ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV/libSPIRV
        ${PROJECT_SOURCE_DIR}/util
        ${PROJECT_SOURCE_DIR}/../util
        ${PROJECT_SOURCE_DIR}/../tool/dumper
        ${XGL_PAL_PATH}/inc/core
        ${XGL_PAL_PATH}/inc/util
        ${LLVM_INCLUDE_DIRS}
)

if(WIN32)
    target_compile_definitions(llpc PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

if(ICD_BUILD_LLPC)
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
        lower/llpcSpirvLowerConstImmediateStore.cpp
        lower/llpcSpirvLowerGlobal.cpp
        lower/llpcSpirvLowerInstMetaRemove.cpp
        lower/llpcSpirvLowerMath.cpp
        lower/llpcSpirvLowerMemoryOp.cpp
        lower/llpcSpirvLowerResourceCollect.cpp
        lower/llpcSpirvLowerTerminator.cpp
        lower/llpcSpirvLowerTranslator.cpp
        lower/llpcSpirvLowerUtil.cpp
    )

# llpc/translator
    target_sources(llpc PRIVATE
        translator/lib/SPIRV/SPIRVReader.cpp
        translator/lib/SPIRV/SPIRVToLLVMDbgTran.cpp
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

# llpc/util
    target_sources(llpc PRIVATE
        util/llpcDebug.cpp
        util/llpcElfWriter.cpp
        util/llpcFile.cpp
        util/llpcShaderModuleHelper.cpp
        util/llpcTimerProfiler.cpp
        util/llpcUtil.cpp
    )
else()
    target_sources(llpc PRIVATE
        util/llpcUtil.cpp
    )
endif()

if(NOT TARGET dumper)
    set(DUMPER_ENABLE_WERROR ${LLPC_ENABLE_WERROR} CACHE BOOL "${PROJECT_NAME} override." FORCE)
    add_subdirectory(../tool/dumper ${PROJECT_BINARY_DIR}/../dumper)
endif()

### LLPC Auto-generated Files ##########################################################################################
if(ICD_BUILD_LLPC)

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

target_link_libraries(llpc PRIVATE dumper)
target_link_libraries(llpc PRIVATE cwpack)
target_link_libraries(llpc PUBLIC
    khronos_vulkan_interface
    khronos_spirv_interface
)

### Visual Studio Filters ##############################################################################################
target_find_headers(llpc)
if(MSVC)
    target_source_groups(llpc)
endif()

### VFX library for Standalone Compiler ###################################################################################
if(ICD_BUILD_LLPC)
    set(VFX_ENABLE_WERROR ${LLPC_ENABLE_WERROR} CACHE BOOL "${PROJECT_NAME} override." FORCE)
    add_subdirectory(${PROJECT_SOURCE_DIR}/../tool/vfx ${PROJECT_BINARY_DIR}/../vfx)
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

if(LLPC_ENABLE_SHADER_CACHE)
    target_compile_definitions(amdllpc PRIVATE LLPC_ENABLE_SHADER_CACHE=1)
endif()

target_include_directories(amdllpc
PUBLIC
    ${PROJECT_SOURCE_DIR}/include
PRIVATE
    ${PROJECT_SOURCE_DIR}/builder
    ${PROJECT_SOURCE_DIR}/context
    ${PROJECT_SOURCE_DIR}/../imported/spirv
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/../include
    ${PROJECT_SOURCE_DIR}/lower
    ${PROJECT_SOURCE_DIR}/patch
    ${PROJECT_SOURCE_DIR}/patch/gfx6/chip
    ${PROJECT_SOURCE_DIR}/patch/gfx9/chip
    ${PROJECT_SOURCE_DIR}/patch/generate
    ${PROJECT_SOURCE_DIR}/translator/include
    ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV
    ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV/libSPIRV
    ${PROJECT_SOURCE_DIR}/util
    ${PROJECT_SOURCE_DIR}/../util
    ${PROJECT_SOURCE_DIR}/../tool/dumper
    ${XGL_PAL_PATH}/src/core/hw/gfxip/gfx6/chip
    ${XGL_PAL_PATH}/src/core/hw/gfxip/gfx9/chip
    ${XGL_PAL_PATH}/inc/core
    ${XGL_PAL_PATH}/inc/util
    ${LLVM_INCLUDE_DIRS}
)

set_compiler_options(amdllpc ${LLPC_ENABLE_WERROR})

if(UNIX)
    target_link_libraries(amdllpc PRIVATE llpc vfx dl stdc++)
elseif(WIN32)
    target_link_libraries(amdllpc PRIVATE llpc vfx)
endif()
llvm_map_components_to_libnames(llvm_libs lgc amdgpucodegen amdgpuinfo amdgpuasmparser amdgpudisassembler LTO ipo analysis bitreader bitwriter codegen irreader linker mc passes support target transformutils coroutines aggressiveinstcombine)
target_link_libraries(amdllpc PRIVATE ${llvm_libs})
target_link_libraries(amdllpc PRIVATE cwpack)
endif()
### Add Subdirectories #################################################################################################
if(ICD_BUILD_LLPC)
# SPVGEN
if(EXISTS ${PROJECT_SOURCE_DIR}/../../spvgen)
    set(XGL_SPVGEN_PATH ${PROJECT_SOURCE_DIR}/../../spvgen CACHE PATH "Specify the path to SPVGEN.")
else()
    set(XGL_SPVGEN_PATH ${PROJECT_SOURCE_DIR}/../../../../tools/spvgen CACHE PATH "Specify the path to SPVGEN.")
endif()

if(EXISTS ${XGL_SPVGEN_PATH})
    set(SPVGEN_ENABLE_WERROR ${LLPC_ENABLE_WERROR} CACHE BOOL "${PROJECT_NAME} override." FORCE)
    add_subdirectory(${XGL_SPVGEN_PATH} ${CMAKE_BINARY_DIR}/spvgen EXCLUDE_FROM_ALL)
    # Propagate the spvgen build directory to the parent scope so that XGL tests can use it.
    set(XGL_SPVGEN_BUILD_PATH ${CMAKE_BINARY_DIR}/spvgen PARENT_SCOPE)
endif()

# Lit tests
if(LLPC_BUILD_LIT)
    add_subdirectory(test)
endif()
endif()
