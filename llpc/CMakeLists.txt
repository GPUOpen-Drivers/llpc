##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

project(LLPC C CXX)

### Create LLPC Library ################################################################################################
add_library(llpcinternal STATIC "")

if(ICD_BUILD_LLPC)
    add_dependencies(llpcinternal LLVMlgc)
endif()

### Cached Project Options #############################################################################################
option(LLPC_BUILD_TESTS      "LLPC build all tests"        OFF)
option(LLPC_BUILD_LLVM_TOOLS "Build LLVM tools"            OFF)
option(LLPC_ENABLE_WERROR    "Build LLPC with more errors" OFF)
option(LLPC_DISABLE_SPVGEN   "Build LLPC tools without SPVGEN" OFF)

if(ICD_BUILD_LLPC)
    set(AMDLLPC_DIR ${CMAKE_CURRENT_BINARY_DIR})
endif()

### Set Options and build LLVM #########################################################################################
if(ICD_BUILD_LLPC)
    # Add LGC and its dependencies as LLVM external projects.
    include("../cmake/lgc.cmake")
    add_lgc_projects()

    # Set other LLVM settings.
    set(LLVMRAYTRACING_BUILD_TESTS ${LLPC_BUILD_TESTS})
    set(LLVM_TARGETS_TO_BUILD AMDGPU CACHE STRING "LLVM targets to build")
    set(LLVM_BUILD_TESTS OFF CACHE BOOL "LLVM build tests")
    set(LLVM_BUILD_TOOLS ${LLPC_BUILD_LLVM_TOOLS} CACHE BOOL "LLVM build tools")
    set(LLVM_BUILD_UTILS OFF CACHE BOOL "LLVM build utils")
    set(LLVM_INCLUDE_DOCS OFF CACHE BOOL "LLVM include docs")
    set(LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL "LLVM include examples")
    set(LLVM_INCLUDE_GO_TESTS OFF CACHE BOOL "LLVM include go tests")
    set(LLVM_INCLUDE_TESTS ${LLPC_BUILD_TESTS} CACHE BOOL "LLVM include tests")
    set(LLVM_INCLUDE_TOOLS ON CACHE BOOL "LLVM include tools")
    set(LLVM_INCLUDE_UTILS ON CACHE BOOL "LLVM include utils")
    set(LLVM_ENABLE_TERMINFO OFF CACHE BOOL "LLVM enable terminfo")
    set(LLVM_RAM_PER_TABLEGEN_JOB 4000 CACHE STRING "LLVM RAM per tablegen job")
    set(LLVM_RAM_PER_LINK_JOB 5000 CACHE STRING "LLVM RAM per link job")
    if(CMAKE_BUILD_TYPE_DEBUG)
        # Build optimized version of llvm-tblgen even in debug builds, for faster build times.
        set(LLVM_OPTIMIZED_TABLEGEN ON CACHE BOOL "Build optimized llvm-tblgen")
#if _WIN32
        if(LLVM_OPTIMIZED_TABLEGEN AND WIN32 AND (CMAKE_GENERATOR MATCHES "Ninja"))
            # LLVM implements the Release build of llvm-tblgen as a cross-compile target, which fails to find
            # our DK-based toolchain (created with amd_generate_msvc_toolchain). However, we can inject the toolchain
            # argument into LLVM's add_custom_target that sets up this cross-compile build.
            # See: llvm-project/llvm/cmake/modules/CrossCompile.cmake
            set(CROSS_TOOLCHAIN_FLAGS_NATIVE "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}" CACHE STRING
                "Toolchain flags for native build" FORCE)
        endif()
#endif
    endif()

    # This will greatly speed up debug builds because we won't be listing all the symbols with llvm-nm.
    set(LLVM_BUILD_LLVM_C_DYLIB OFF CACHE BOOL "LLVM build LLVM-C dylib")

    if(EXISTS ${PROJECT_SOURCE_DIR}/../../../imported/llvm-project/llvm)
        set(XGL_LLVM_SRC_PATH ${PROJECT_SOURCE_DIR}/../../../imported/llvm-project/llvm CACHE PATH "Specify the path to the LLVM.")
    elseif(EXISTS ${PROJECT_SOURCE_DIR}/../../llvm-project/llvm)
        set(XGL_LLVM_SRC_PATH ${PROJECT_SOURCE_DIR}/../../llvm-project/llvm CACHE PATH "Specify the path to the LLVM.")
    endif()

    add_subdirectory(${XGL_LLVM_SRC_PATH} ${PROJECT_BINARY_DIR}/llvm)
    set(XGL_LLVM_BUILD_PATH ${PROJECT_BINARY_DIR}/llvm)
    # Export the LLVM build path so that it's available in XGL.
    set(XGL_LLVM_BUILD_PATH ${XGL_LLVM_BUILD_PATH} PARENT_SCOPE)

    if (LLVM_LINK_LLVM_DYLIB)
        # Link dynamically against libLLVM-${version}.so
        target_link_libraries(llpcinternal PUBLIC LLVM)
    else()
        # Link statically against the required component libraries
        llvm_map_components_to_libnames(llvm_libs
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
            IRPrinter
            IRReader
            Linker
            MC
            Passes
            ScalarOpts
            Support
            Target
            TransformUtils
        )
        # Some of the games using old versions of the tcmalloc lib are crashing
        # when allocating aligned memory. C++17 enables aligned new by default,
        # so we need to disable it to prevent those crashes.
        if(NOT WIN32)
            foreach (lib ${llvm_libs})
                target_compile_options(${lib} PRIVATE "-fno-aligned-new")
            endforeach()
        endif()
        target_link_libraries(llpcinternal PUBLIC ${llvm_libs})
    endif()

    # Always link statically against libLLVMlgc
    llvm_map_components_to_libnames(extra_llvm_libs lgc Raytracing)
    if(NOT WIN32)
        foreach (lib ${extra_llvm_libs})
            target_compile_options(${lib} PRIVATE "-fno-aligned-new")
        endforeach()
    endif()
    target_link_libraries(llpcinternal PUBLIC ${extra_llvm_libs})
    target_link_libraries(llpcinternal PRIVATE gfxruntime)
endif()

### Compiler Options ###################################################################################################
include(../cmake/CompilerFlags.cmake)
set_compiler_options(llpcinternal ${LLPC_ENABLE_WERROR})

### TableGen for LLPC dialect ########################################################################################
if (ICD_BUILD_LLPC)
    if (EXISTS ${LLVM_TOOLS_BINARY_PATH}/llvm-dialects-tblgen)
      set(LLPC_TABLEGEN_EXE ${LLVM_TOOLS_BINARY_PATH}/llvm-dialects-tblgen)
    else()
      set(LLPC_TABLEGEN_EXE $<TARGET_FILE:llvm-dialects-tblgen>)
    endif()
    set(LLPC_TABLEGEN_TARGET llvm-dialects-tblgen)
    set(LLVM_TARGET_DEFINITIONS include/LlpcDialect.td)

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include")
    tablegen(LLPC include/LlpcDialect.h.inc -gen-dialect-decls --dialect llpc
        EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../imported/llvm-dialects/include
        )
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include/context")
    tablegen(LLPC include/context/LlpcDialect.cpp.inc -gen-dialect-defs --dialect llpc
        EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../imported/llvm-dialects/include
        )

    add_public_tablegen_target(LlpcDialectTableGen)
    add_dependencies(llpcinternal LlpcDialectTableGen)
endif()

### Defines/Includes/Sources ###########################################################################################
if(ICD_BUILD_LLPC)
    list(APPEND CMAKE_MODULE_PATH
        "${XGL_LLVM_BUILD_PATH}/lib/cmake/llvm"
        "${XGL_LLVM_BUILD_PATH}/${CMAKE_CFG_INTDIR}/lib/cmake/llvm" # Workaround for VS generator with older LLVM.
    )
    include(LLVMConfig)
    message(STATUS "LLVM executables: " ${LLVM_TOOLS_BINARY_DIR})
    message(STATUS "LLVM libraries: " ${LLVM_BUILD_LIBRARY_DIR})
    execute_process(
        COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-config --libs amdgpu analysis bitreader bitwriter codegen irreader linker mc passes support target transformutils
        OUTPUT_VARIABLE LLVM_LINK_FLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    message(STATUS "LLVM link options:" ${LLVM_LINK_FLAGS})
endif()
target_compile_definitions(llpcinternal PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)
target_compile_definitions(llpcinternal PRIVATE _SPIRV_LLVM_API)
target_compile_definitions(llpcinternal PRIVATE PAL_CLIENT_INTERFACE_MAJOR_VERSION=${PAL_CLIENT_INTERFACE_MAJOR_VERSION})
if(ICD_BUILD_LLPC)
    target_compile_definitions(llpcinternal PRIVATE ICD_BUILD_LLPC)
endif()

if(ICD_BUILD_LLPC)
    if(XGL_LLVM_UPSTREAM)
        target_compile_definitions(llpcinternal PRIVATE XGL_LLVM_UPSTREAM)
    endif()
endif()

#if _WIN32
if(WIN32)
    target_compile_definitions(llpcinternal PRIVATE
        NOMINMAX    # windows.h defines min/max which conflicts with the use of std::min / max
        UNICODE     # CMAKE-TODO: What is this used for?
        _UNICODE
    )
endif()
#endif

target_include_directories(llpcinternal
    PUBLIC
        include
        ../include
        context
        lowering
        translator/include
        translator/lib/SPIRV
        translator/lib/SPIRV/libSPIRV
        util
        ../util
        ${PROJECT_BINARY_DIR}/include
        ${XGL_PAL_PATH}/inc/core
        ${XGL_PAL_PATH}/inc/util
        ${LLVM_INCLUDE_DIRS}
)

#if _WIN32
if(WIN32)
    target_compile_definitions(llpcinternal PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()
#endif

if(ICD_BUILD_LLPC)
# llpc/context
    target_sources(llpcinternal PRIVATE
        context/llpcCompiler.cpp
        context/llpcCompiler.h
        context/llpcComputeContext.cpp
        context/llpcComputeContext.h
        context/llpcContext.cpp
        context/llpcContext.h
        context/llpcDialect.cpp
        context/llpcDialect.h
        context/llpcGraphicsContext.cpp
        context/llpcGraphicsContext.h
        context/llpcPipelineContext.cpp
        context/llpcPipelineContext.h
        context/llpcRayTracingContext.cpp
        context/llpcRayTracingContext.h
    )

# llpc/lowering
    target_sources(llpcinternal PRIVATE
        lowering/LowerAccessChain.cpp
        lowering/LowerAccessChain.h
        lowering/LowerAdvancedBlend.cpp
        lowering/LowerAdvancedBlend.h
        lowering/LowerCfgMerges.cpp
        lowering/LowerCfgMerges.h
        lowering/LowerConstImmediateStore.cpp
        lowering/LowerConstImmediateStore.h
        lowering/LowerCooperativeMatrix.cpp
        lowering/LowerCooperativeMatrix.h
        lowering/LowerGlCompatibility.cpp
        lowering/LowerGlCompatibility.h
        lowering/LowerGlobals.cpp
        lowering/LowerGlobals.h
        lowering/LowerInstMetaRemove.cpp
        lowering/LowerInstMetaRemove.h
        lowering/Lowering.cpp
        lowering/Lowering.h
        lowering/LowerMath.cpp
        lowering/LowerMath.h
        lowering/LowerMemoryOp.cpp
        lowering/LowerMemoryOp.h
        lowering/LowerPostInline.cpp
        lowering/LowerPostInline.h
        lowering/LowerRayTracing.cpp
        lowering/LowerRayTracing.h
        lowering/LowerTerminator.cpp
        lowering/LowerTerminator.h
        lowering/LowerTranslator.cpp
        lowering/LowerTranslator.h
        lowering/LoweringUtil.cpp
        lowering/LoweringUtil.h
        lowering/LowerInternalLibraryIntrinsic.cpp
        lowering/LowerInternalLibraryIntrinsic.h
        lowering/ProcessGpuRtLibrary.cpp
        lowering/ProcessGpuRtLibrary.h
        lowering/PrepareContinuations.cpp
        lowering/PrepareContinuations.h
        lowering/ScalarReplacementOfBuiltins.cpp
        lowering/ScalarReplacementOfBuiltins.h
        lowering/ProcessGfxRuntimeLibrary.cpp
        lowering/ProcessGfxRuntimeLibrary.h
    )

# llpc/translator
    target_sources(llpcinternal PRIVATE
        translator/lib/SPIRV/SPIRVReader.cpp
        translator/lib/SPIRV/SPIRVReader.h
        translator/lib/SPIRV/SPIRVToLLVMDbgTran.cpp
        translator/lib/SPIRV/SPIRVToLLVMDbgTran.h
        translator/lib/SPIRV/SPIRVUtil.cpp
    )

    target_sources(llpcinternal PRIVATE
        translator/lib/SPIRV/libSPIRV/SPIRVBasicBlock.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVBasicBlock.h
        translator/lib/SPIRV/libSPIRV/SPIRVDebug.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVDebug.h
        translator/lib/SPIRV/libSPIRV/SPIRVDecorate.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVDecorate.h
        translator/lib/SPIRV/libSPIRV/SPIRVEntry.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVEntry.h
        translator/lib/SPIRV/libSPIRV/SPIRVFunction.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVFunction.h
        translator/lib/SPIRV/libSPIRV/SPIRVInstruction.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVInstruction.h
        translator/lib/SPIRV/libSPIRV/SPIRVModule.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVModule.h
        translator/lib/SPIRV/libSPIRV/SPIRVStream.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVStream.h
        translator/lib/SPIRV/libSPIRV/SPIRVType.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVType.h
        translator/lib/SPIRV/libSPIRV/SPIRVValue.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVValue.h
        translator/lib/SPIRV/libSPIRV/SPIRVUtil.h
    )

# llpc/util
    target_sources(llpcinternal PRIVATE
        util/llpcCacheAccessor.cpp
        util/llpcCacheAccessor.h
        util/llpcDebug.cpp
        util/llpcDebug.h
        util/llpcElfWriter.cpp
        util/llpcElfWriter.h
        util/llpcError.cpp
        util/llpcError.h
        util/llpcFile.cpp
        util/llpcFile.h
        util/llpcShaderModuleHelper.cpp
        util/llpcShaderModuleHelper.h
        util/llpcTimerProfiler.cpp
        util/llpcTimerProfiler.h
        util/llpcUtil.cpp
        util/llpcUtil.h
    )
else()
    target_sources(llpcinternal PRIVATE
        util/llpcUtil.cpp
        util/llpcUtil.h
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
#if _WIN32
elseif(WIN32)
    set(BUILD_OS win)
#endif
endif()

endif()
### Link Libraries #####################################################################################################
# CMAKE-TODO: LLVM_LIB_DIR should probably be set in the op level CMake?
# Maybe add XGL_LLVM_PATH?
# How are these built? Can they be built through CMake?

target_link_libraries(llpcinternal PUBLIC dumper)
target_link_libraries(llpcinternal PUBLIC cwpack)
target_link_libraries(llpcinternal PUBLIC metrohash)
target_link_libraries(llpcinternal PUBLIC vkgc_headers)
target_link_libraries(llpcinternal PUBLIC
    khronos_vulkan_interface
    khronos_spirv_interface
)

if(VKI_RAY_TRACING AND NOT LLPC_IS_STANDALONE)
    target_link_libraries(llpcinternal PRIVATE vkgc_gpurtshim)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(llpcinternal PRIVATE Threads::Threads)

### Create LLPC Library ################################################################################################
add_library(llpc STATIC "")

add_dependencies(llpc llpcinternal)

target_include_directories(llpc
  PUBLIC
    include
)

set_compiler_options(llpc ${LLPC_ENABLE_WERROR})

# This one source file is here just to stop getting cmake and ar errors about having no source files.
target_sources(llpc PRIVATE
    context/llpcStub.cpp
)

target_link_libraries(llpc
  PUBLIC
    ${llvm_libs}
    ${extra_llvm_libs}
    khronos_vulkan_interface
    khronos_spirv_interface
  PRIVATE
    llpcinternal
)

if(LLPC_BUILD_TOOLS)
### VFX library for Standalone Compiler ###################################################################################
if(NOT TARGET vfx)
    set(VFX_ENABLE_WERROR ${LLPC_ENABLE_WERROR} CACHE BOOL "${PROJECT_NAME} override." FORCE)
    add_subdirectory(${PROJECT_SOURCE_DIR}/../tool/vfx ${PROJECT_BINARY_DIR}/../vfx)
endif()
### Create Standalone Compiler ############################################################################################
if(ICD_BUILD_LLPC)

# Add a common library for standalone compilers based on LLPC.
add_library(llpc_standalone_compiler
    tool/llpcAutoLayout.cpp
    tool/llpcAutoLayout.h
    tool/llpcCompilationUtils.cpp
    tool/llpcCompilationUtils.h
    tool/llpcComputePipelineBuilder.cpp
    tool/llpcComputePipelineBuilder.h
    tool/llpcGraphicsPipelineBuilder.cpp
    tool/llpcGraphicsPipelineBuilder.h
    tool/llpcInputUtils.cpp
    tool/llpcInputUtils.h
    tool/llpcPipelineBuilder.cpp
    tool/llpcPipelineBuilder.h
    tool/llpcRayTracingPipelineBuilder.cpp
    tool/llpcRayTracingPipelineBuilder.h
    tool/llpcShaderCache.cpp
    tool/llpcShaderCache.h
    tool/llpcShaderCacheWrap.cpp
    tool/llpcShaderCacheWrap.h
)

add_dependencies(llpc_standalone_compiler llpc)

target_compile_definitions(llpc_standalone_compiler PUBLIC
    ICD_BUILD_LLPC
    ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU
    _SPIRV_LLVM_API
)

if (LLPC_DISABLE_SPVGEN)
  target_compile_definitions(llpc_standalone_compiler PUBLIC LLPC_DISABLE_SPVGEN)
endif()

if (LLPC_CLIENT_INTERFACE_MAJOR_VERSION)
    target_compile_definitions(llpc_standalone_compiler PUBLIC
        PAL_CLIENT_INTERFACE_MAJOR_VERSION=${PAL_CLIENT_INTERFACE_MAJOR_VERSION}
    )
endif()

target_include_directories(llpc_standalone_compiler PUBLIC
    ${PROJECT_SOURCE_DIR}/../util
    ${PROJECT_SOURCE_DIR}/context
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/lowering
    ${PROJECT_SOURCE_DIR}/tool
    ${PROJECT_SOURCE_DIR}/translator/include
    ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV
    ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV/libSPIRV
    ${PROJECT_SOURCE_DIR}/util
    ${XGL_PAL_PATH}/src/core/hw/gfxip/gfx9/chip
    ${XGL_PAL_PATH}/inc/core
    ${XGL_PAL_PATH}/inc/util
    ${LLVM_INCLUDE_DIRS}
)

target_link_libraries(llpc_standalone_compiler PUBLIC
    cwpack
    llpc
    metrohash
    vfx
    vkgc_headers
    vkgc_util
    ${CMAKE_DL_LIBS}
)

if (NOT LLPC_DISABLE_SPVGEN)
    target_link_libraries(llpc_standalone_compiler PUBLIC spvgen_static)
endif()

if (NOT LLVM_LINK_LLVM_DYLIB)
    llvm_map_components_to_libnames(llvm_libs
        AsmParser
    )
    target_link_libraries(llpc_standalone_compiler PUBLIC ${llvm_libs})
endif()

set_compiler_options(llpc_standalone_compiler ${LLPC_ENABLE_WERROR})

# Add an executable for the amdllpc standalone compiler.
add_executable(amdllpc tool/amdllpc.cpp)
add_dependencies(amdllpc llpc_standalone_compiler)
target_link_libraries(amdllpc PRIVATE llpc_standalone_compiler)
set_compiler_options(amdllpc ${LLPC_ENABLE_WERROR})
add_compile_definitions(amdllpc PRIVATE SH_EXPORTING)

endif()
endif()
### Add Subdirectories #################################################################################################
if(ICD_BUILD_LLPC AND LLPC_BUILD_TESTS)
    # Unit tests.
    add_subdirectory(unittests)

    # LIT tests.
    add_subdirectory(test)
endif()
