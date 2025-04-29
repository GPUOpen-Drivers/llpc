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

project(LLPC C CXX)

### Create LLPC Library ################################################################################################
add_library(llpcinternal STATIC "")

### Cached Project Options #############################################################################################
option(LLPC_BUILD_TESTS      "LLPC build all tests"        OFF)
option(LLPC_BUILD_LLVM_TOOLS "Build LLVM tools"            OFF)
option(LLPC_DISABLE_SPVGEN   "Build LLPC tools without SPVGEN" OFF)

if(ICD_BUILD_LLPC)
    set(AMDLLPC_DIR ${CMAKE_CURRENT_BINARY_DIR})
endif()

### Link against LLVM and the components we build as LLVM external projects ############################################
if(ICD_BUILD_LLPC)
    target_link_libraries(llpcinternal PUBLIC ${LLPC_LLVM_LIBS})

    # Always link statically against libLLVMlgc
    llvm_map_components_to_libnames(extra_llvm_libs lgc Raytracing)
    if(NOT WIN32)
        foreach (lib ${extra_llvm_libs})
            target_compile_options(${lib} PRIVATE "-fno-aligned-new")
        endforeach()
    endif()
    target_link_libraries(llpcinternal PUBLIC ${extra_llvm_libs})
    target_link_libraries(llpcinternal PRIVATE LLVMlgc gfxruntime sharedme_xdl)
endif()

### Compiler Options ###################################################################################################
include(../cmake/CompilerFlags.cmake)
set_compiler_options(llpcinternal)

### TableGen for LLPC dialect ########################################################################################
if (ICD_BUILD_LLPC)
    include(../cmake/DialectsTablegen.cmake)
    set_dialects_tablegen_exe(LLPC)
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
        ${PAL_SOURCE_DIR}/inc/core
        ${PAL_SOURCE_DIR}/inc/util
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
        lowering/MemCpyRecognize.cpp
        lowering/MemCpyRecognize.h
        lowering/ProcessGpuRtLibrary.cpp
        lowering/ProcessGpuRtLibrary.h
        lowering/PrepareContinuations.cpp
        lowering/PrepareContinuations.h
        lowering/ScalarReplacementOfBuiltins.cpp
        lowering/ScalarReplacementOfBuiltins.h
        lowering/ProcessGfxRuntimeLibrary.cpp
        lowering/ProcessGfxRuntimeLibrary.h
        lowering/LinkTransformShaders.cpp
        lowering/LinkTransformShaders.h
        lowering/PrepareTransformVertexShader.cpp
        lowering/PrepareTransformVertexShader.h
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
        util/llpcThreading.cpp
        util/llpcThreading.h
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

#if LLPC_RAY_TRACING
if(LLPC_RAY_TRACING AND NOT LLPC_IS_STANDALONE)
    target_link_libraries(llpcinternal PRIVATE vkgc_gpurtshim)
endif()
#endif

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

set_compiler_options(llpc)

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
    ${PAL_SOURCE_DIR}/src/core/hw/gfxip/gfx9/chip
    ${PAL_SOURCE_DIR}/inc/core
    ${PAL_SOURCE_DIR}/inc/util
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

set_compiler_options(llpc_standalone_compiler)

# Add an executable for the amdllpc standalone compiler.
add_executable(amdllpc tool/amdllpc.cpp)
add_dependencies(amdllpc llpc_standalone_compiler)
target_link_libraries(amdllpc PRIVATE llpc_standalone_compiler)
set_compiler_options(amdllpc)
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
