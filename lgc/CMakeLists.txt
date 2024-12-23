##
 #######################################################################################################################
 #
 #  Copyright (c) 2020-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

### Create LGC Library ################################################################################################

add_llvm_library(LLVMlgc LINK_COMPONENTS
    AggressiveInstCombine
    AMDGPUAsmParser
    AMDGPUCodeGen
    AMDGPUDesc
    AMDGPUDisassembler
    AMDGPUInfo
    Analysis
    BinaryFormat
    BitWriter
    CodeGen
    Core
    InstCombine
    IRPrinter
    ipo
    Linker
    MC
    Object
    Passes
    ScalarOpts
    SelectionDAG
    Support
    Target
    TargetParser
    TransformUtils
    Vectorize
)

llvm_map_components_to_libnames(extra_llvm_libs CompilerUtils Raytracing)
target_link_libraries(LLVMlgc PUBLIC llvm_dialects ${extra_llvm_libs} llpc_version)

### Cached Project Options #############################################################################################
option(LLPC_ENABLE_WERROR "Build LLPC with more errors" OFF)

### Compiler Options ###################################################################################################
include(../cmake/CompilerFlags.cmake)
set_compiler_options(LLVMlgc ${LLPC_ENABLE_WERROR})

### TableGen for LGC dialect ###########################################################################################

# Determine whether LLVM has the nodivergencesource attribute.
include(../cmake/LlvmMainRevision.cmake)
if (${LLVM_MAIN_REVISION} GREATER_EQUAL 514862)
  set(LGC_TABLEGEN_FLAGS -DLLVM_HAVE_NODIVERGENCESOURCE_ATTR)
endif()

if (EXISTS ${LLVM_TOOLS_BINARY_PATH}/llvm-dialects-tblgen)
  set(LGC_TABLEGEN_EXE ${LLVM_TOOLS_BINARY_PATH}/llvm-dialects-tblgen)
else()
  set(LGC_TABLEGEN_EXE $<TARGET_FILE:llvm-dialects-tblgen>)
endif()
set(LGC_TABLEGEN_TARGET llvm-dialects-tblgen)
set(LLVM_TARGET_DEFINITIONS interface/lgc/LgcDialect.td)

if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/interface/lgc)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/interface/lgc")
endif()
tablegen(LGC interface/lgc/LgcDialect.h.inc -gen-dialect-decls --dialect lgc ${LGC_TABLEGEN_FLAGS}
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../imported/llvm-dialects/include
    )
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/state)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/state")
endif()
tablegen(LGC state/LgcDialect.cpp.inc -gen-dialect-defs --dialect lgc ${LGC_TABLEGEN_FLAGS}
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../imported/llvm-dialects/include
    )

add_public_tablegen_target(LgcDialectTableGen)

add_dependencies(LLVMlgc LgcDialectTableGen)

### Defines/Includes/Sources ###########################################################################################
if(WIN32)
    target_compile_definitions(LLVMlgc PRIVATE
        NOMINMAX    # windows.h defines min/max which conflicts with the use of std::min / max
        UNICODE     # CMAKE-TODO: What is this used for?
        _UNICODE
    )
endif()

target_include_directories(LLVMlgc
PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/interface>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/interface>
    $<INSTALL_INTERFACE:interface>
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/imported
    ${CMAKE_CURRENT_BINARY_DIR}
)

# lgc/builder
target_sources(LLVMlgc PRIVATE
    builder/ArithBuilder.cpp
    builder/Builder.cpp
    builder/BuilderBase.cpp
    builder/BuilderImpl.cpp
    builder/BuilderRecorder.cpp
    builder/BuilderRecorder.h
    builder/BuilderReplayer.cpp
    builder/DescBuilder.cpp
    builder/ImageBuilder.cpp
    builder/InOutBuilder.cpp
    builder/MatrixBuilder.cpp
    builder/MiscBuilder.cpp
    builder/SubgroupBuilder.cpp
    builder/YCbCrAddressHandler.cpp
    builder/YCbCrAddressHandler.h
    builder/YCbCrConverter.cpp
    builder/YCbCrConverter.h
)

# include/lgc/builder
target_sources(LLVMlgc PRIVATE
    include/lgc/builder/BuilderImpl.h
    include/lgc/builder/BuilderReplayer.h
    include/lgc/builder/SubgroupBuilder.h
)

# lgc/elfLinker
target_sources(LLVMlgc PRIVATE
    elfLinker/ColorExportShader.cpp
    elfLinker/ColorExportShader.h
    elfLinker/ElfLinker.cpp
    elfLinker/ElfLinkerImpl.h
    elfLinker/GlueShader.cpp
    elfLinker/GlueShader.h
    elfLinker/NullFragmentShader.cpp
    elfLinker/NullFragmentShader.h
)

# lgc/patch
target_sources(LLVMlgc PRIVATE
    patch/ConfigBuilderBase.cpp
    patch/Continufy.cpp
    patch/FragmentColorExport.cpp
    patch/LowerDebugPrintf.cpp
    patch/LowerDesc.cpp
    patch/LowerPopsInterlock.cpp
    patch/LowerSubgroupOps.cpp
    patch/MeshTaskShader.cpp
    patch/NggPrimShader.cpp
    patch/LgcLowering.cpp
    patch/StructurizeBuffers.cpp
    patch/LowerBufferOperations.cpp
    patch/CheckShaderCache.cpp
    patch/GenerateCopyShader.cpp
    patch/MutateEntryPoint.cpp
    patch/LowerImageDerivatives.cpp
    patch/LowerInOut.cpp
    patch/LowerInvariantLoads.cpp
    patch/IncludeLlvmIr.cpp
    patch/InitializeWorkgroupMemory.cpp
    patch/ScalarizeLoads.cpp
    patch/LowerMulDx9Zero.cpp
    patch/AddLoopMetadata.cpp
    patch/GenerateNullFragmentShader.cpp
    patch/PeepholeOptimization.cpp
    patch/PreparePipelineAbi.cpp
    patch/LowerReadFirstLane.cpp
    patch/CollectResourceUsage.cpp
    patch/SetupTargetFeatures.cpp
    patch/PassthroughHullShader.cpp
    patch/ApplyWorkarounds.cpp
    patch/ShaderInputs.cpp
    patch/ShaderMerger.cpp
    patch/SystemValues.cpp
    patch/VertexFetch.cpp
    patch/CollectImageOperations.cpp
    patch/RegisterMetadataBuilder.cpp
#if LLPC_BUILD_STRIX1
    patch/WorkaroundDsSubdwordWrite.cpp
#endif
    patch/CombineCooperativeMatrix.cpp
    patch/LowerCooperativeMatrix.cpp
    patch/LowerGpuRt.cpp
    patch/LowerRayQueryWrapper.cpp
)

# include/lgc/patch
target_sources(LLVMlgc PRIVATE
    include/lgc/patch/AddLoopMetadata.h
    include/lgc/patch/ApplyWorkarounds.h
    include/lgc/patch/CheckShaderCache.h
    include/lgc/patch/CollectImageOperations.h
    include/lgc/patch/CollectResourceUsage.h
    include/lgc/patch/CombineCooperativeMatrix.h
    include/lgc/patch/Continufy.h
    include/lgc/patch/FragmentColorExport.h
    include/lgc/patch/GenerateCopyShader.h
    include/lgc/patch/IncludeLlvmIr.h
    include/lgc/patch/InitializeWorkgroupMemory.h
    include/lgc/patch/LgcLowering.h
    include/lgc/patch/LowerBufferOperations.h
    include/lgc/patch/LowerCooperativeMatrix.h
    include/lgc/patch/LowerDebugPrintf.h
    include/lgc/patch/LowerDesc.h
    include/lgc/patch/LowerGpuRt.h
    include/lgc/patch/LowerImageDerivatives.h
    include/lgc/patch/LowerInOut.h
    include/lgc/patch/LowerInvariantLoads.h
    include/lgc/patch/LowerMulDx9Zero.h
    include/lgc/patch/LowerReadFirstLane.h
    include/lgc/patch/LowerSubgroupOps.h
    include/lgc/patch/MutateEntryPoint.h
    include/lgc/patch/PassthroughHullShader.h
    include/lgc/patch/PeepholeOptimization.h
    include/lgc/patch/PreparePipelineAbi.h
    include/lgc/patch/ScalarizeLoads.h
    include/lgc/patch/SetupTargetFeatures.h
    include/lgc/patch/ShaderInputs.h
    include/lgc/patch/StructurizeBuffers.h
    include/lgc/patch/SystemValues.h
    include/lgc/patch/VertexFetch.h
    include/lgc/patch/WorkaroundDsSubdwordWrite.h
)

# lgc/state
target_sources(LLVMlgc PRIVATE
    state/Compiler.cpp
    state/LgcContext.cpp
    state/LgcDialect.cpp
    state/PalMetadata.cpp
    state/PassManagerCache.cpp
    state/PipelineShaders.cpp
    state/PipelineState.cpp
    state/RayTracingLibrarySummary.cpp
    state/ResourceUsage.cpp
    state/ShaderModes.cpp
    state/ShaderStage.cpp
    state/TargetInfo.cpp
    state/RuntimeContext.cpp
)

# include/lgc/state
target_sources(LLVMlgc PRIVATE
    include/lgc/state/Abi.h
    include/lgc/state/AbiMetadata.h
    include/lgc/state/AbiUnlinked.h
    include/lgc/state/Defs.h
    include/lgc/state/IntrinsDefs.h
    include/lgc/state/PalMetadata.h
    include/lgc/state/PassManagerCache.h
    include/lgc/state/PipelineShaders.h
    include/lgc/state/PipelineState.h
    include/lgc/state/ResourceUsage.h
    include/lgc/state/ShaderStage.h
    include/lgc/state/TargetInfo.h
)

# lgc/util
target_sources(LLVMlgc PRIVATE
    util/AddressExtender.cpp
    util/Debug.cpp
    util/GfxRegHandlerBase.cpp
    util/GfxRegHandler.cpp
    util/Internal.cpp
    util/MsgPackScanner.cpp
    util/PassManager.cpp
    util/RegStackUsage.cpp
    util/StartStopTimer.cpp
    util/WorkgroupLayout.cpp
)

# include/lgc/util
target_sources(LLVMlgc PRIVATE
    include/lgc/util/AddressExtender.h
    include/lgc/util/BuilderBase.h
    include/lgc/util/GfxRegHandler.h
    include/lgc/util/GfxRegHandlerBase.h
    include/lgc/util/Internal.h
    include/lgc/util/MsgPackScanner.h
    include/lgc/util/WorkgroupLayout.h
)

# lgc/interface/lgc
target_sources(LLVMlgc PRIVATE
    interface/lgc/Builder.h
    interface/lgc/BuilderCommon.h
    interface/lgc/BuiltInDefs.h
    interface/lgc/BuiltIns.h
    interface/lgc/CommonDefs.h
    interface/lgc/Debug.h
    interface/lgc/Disassembler.h
    interface/lgc/ElfLinker.h
    interface/lgc/EnumIterator.h
    interface/lgc/LgcContext.h
    interface/lgc/LgcDialect.h
    interface/lgc/PassManager.h
    interface/lgc/Pipeline.h
    interface/lgc/RayTracingLibrarySummary.h
    interface/lgc/RegStackUsage.h
    interface/lgc/RuntimeContext.h
    interface/lgc/LgcDialect.td
)

add_subdirectory(disassembler)
add_subdirectory(tool/lgc)
add_subdirectory(test)

if (LLPC_BUILD_TESTS)
  add_subdirectory(unittests)
endif()

target_link_libraries(LLVMlgc PRIVATE LLVMRaytracing)
