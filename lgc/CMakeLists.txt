##
 #######################################################################################################################
 #
 #  Copyright (c) 2020-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
target_link_libraries(LLVMlgc PUBLIC llvm_dialects ${extra_llvm_libs} sharedme_xdl llpc_version)

### Compiler Options ###################################################################################################
include(../cmake/CompilerFlags.cmake)
set_compiler_options(LLVMlgc)

### TableGen for LGC dialect ###########################################################################################

# Determine whether LLVM has the nodivergencesource attribute.
include(../cmake/LlvmMainRevision.cmake)
if (${LLVM_MAIN_REVISION} GREATER_EQUAL 514862)
  set(LGC_TABLEGEN_FLAGS -DLLVM_HAVE_NODIVERGENCESOURCE_ATTR)
endif()

include(../cmake/DialectsTablegen.cmake)
set_dialects_tablegen_exe(LGC)
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

# lgc/lowering
target_sources(LLVMlgc PRIVATE
    lowering/ConfigBuilderBase.cpp
    lowering/Continufy.cpp
    lowering/FragmentColorExport.cpp
    lowering/LowerDebugPrintf.cpp
    lowering/LowerDesc.cpp
    lowering/LowerPopsInterlock.cpp
    lowering/LowerSubgroupOps.cpp
    lowering/MeshTaskShader.cpp
    lowering/NggPrimShader.cpp
    lowering/LgcLowering.cpp
    lowering/StructurizeBuffers.cpp
    lowering/LowerBufferOperations.cpp
    lowering/CheckShaderCache.cpp
    lowering/GenerateCopyShader.cpp
    lowering/MutateEntryPoint.cpp
    lowering/LowerImageDerivatives.cpp
    lowering/LowerInOut.cpp
    lowering/LowerInvariantLoads.cpp
    lowering/IncludeLlvmIr.cpp
    lowering/InitializeWorkgroupMemory.cpp
    lowering/ScalarizeLoads.cpp
    lowering/LowerMulDx9Zero.cpp
    lowering/AddLoopMetadata.cpp
    lowering/GenerateNullFragmentShader.cpp
    lowering/PeepholeOptimization.cpp
    lowering/PreparePipelineAbi.cpp
    lowering/LowerReadFirstLane.cpp
    lowering/CollectResourceUsage.cpp
    lowering/SetupTargetFeatures.cpp
    lowering/PassthroughHullShader.cpp
    lowering/ApplyWorkarounds.cpp
    lowering/ShaderInputs.cpp
    lowering/ShaderMerger.cpp
    lowering/SystemValues.cpp
    lowering/VertexFetch.cpp
    lowering/CollectImageOperations.cpp
    lowering/RegisterMetadataBuilder.cpp
#if LLPC_BUILD_STRIX1  || LLPC_BUILD_STRIX_HALO
    lowering/WorkaroundDsSubdwordWrite.cpp
#endif
    lowering/CombineCooperativeMatrix.cpp
    lowering/LowerCooperativeMatrix.cpp
    lowering/LowerGpuRt.cpp
    lowering/LowerRayQueryWrapper.cpp
)

#if LLPC_BUILD_GFX12
if(LLPC_BUILD_GFX12)
    target_sources(LLVMlgc PRIVATE lowering/AddBufferOperationMetadata.cpp)
endif()
#endif

# include/lgc/lowering
target_sources(LLVMlgc PRIVATE
    include/lgc/lowering/AddLoopMetadata.h
    include/lgc/lowering/ApplyWorkarounds.h
    include/lgc/lowering/CheckShaderCache.h
    include/lgc/lowering/CollectImageOperations.h
    include/lgc/lowering/CollectResourceUsage.h
    include/lgc/lowering/CombineCooperativeMatrix.h
    include/lgc/lowering/Continufy.h
    include/lgc/lowering/FragmentColorExport.h
    include/lgc/lowering/GenerateCopyShader.h
    include/lgc/lowering/IncludeLlvmIr.h
    include/lgc/lowering/InitializeWorkgroupMemory.h
    include/lgc/lowering/LgcLowering.h
    include/lgc/lowering/LowerBufferOperations.h
    include/lgc/lowering/LowerCooperativeMatrix.h
    include/lgc/lowering/LowerDebugPrintf.h
    include/lgc/lowering/LowerDesc.h
    include/lgc/lowering/LowerGpuRt.h
    include/lgc/lowering/LowerImageDerivatives.h
    include/lgc/lowering/LowerInOut.h
    include/lgc/lowering/LowerInvariantLoads.h
    include/lgc/lowering/LowerMulDx9Zero.h
    include/lgc/lowering/LowerReadFirstLane.h
    include/lgc/lowering/LowerSubgroupOps.h
    include/lgc/lowering/MutateEntryPoint.h
    include/lgc/lowering/PassthroughHullShader.h
    include/lgc/lowering/PeepholeOptimization.h
    include/lgc/lowering/PreparePipelineAbi.h
    include/lgc/lowering/ScalarizeLoads.h
    include/lgc/lowering/SetupTargetFeatures.h
    include/lgc/lowering/ShaderInputs.h
    include/lgc/lowering/StructurizeBuffers.h
    include/lgc/lowering/SystemValues.h
    include/lgc/lowering/VertexFetch.h
    include/lgc/lowering/WorkaroundDsSubdwordWrite.h
)

#if LLPC_BUILD_GFX12
if(LLPC_BUILD_GFX12)
    target_sources(LLVMlgc PRIVATE include/lgc/lowering/AddBufferOperationMetadata.h)
endif()
#endif

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
    util/BufferResource.cpp
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
