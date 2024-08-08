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
tablegen(LGC interface/lgc/LgcDialect.h.inc -gen-dialect-decls --dialect lgc
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../imported/llvm-dialects/include
    )
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/state)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/state")
endif()
tablegen(LGC state/LgcDialect.cpp.inc -gen-dialect-defs --dialect lgc
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

# lgc/elfLinker
target_sources(LLVMlgc PRIVATE
    elfLinker/ColorExportShader.cpp
    elfLinker/ElfLinker.cpp
    elfLinker/GlueShader.cpp
    elfLinker/NullFragmentShader.cpp
)

# lgc/patch
target_sources(LLVMlgc PRIVATE
    patch/ConfigBuilderBase.cpp
    patch/Continufy.cpp
    patch/FragColorExport.cpp
    patch/LowerDebugPrintf.cpp
    patch/LowerDesc.cpp
    patch/LowerSubgroupOps.cpp
    patch/MeshTaskShader.cpp
    patch/NggPrimShader.cpp
    patch/Patch.cpp
    patch/PatchBufferOp.cpp
    patch/PatchCheckShaderCache.cpp
    patch/PatchCopyShader.cpp
    patch/PatchEntryPointMutate.cpp
    patch/PatchImageDerivatives.cpp
    patch/PatchInOutImportExport.cpp
    patch/PatchInvariantLoads.cpp
    patch/PatchLlvmIrInclusion.cpp
    patch/PatchLoadScalarizer.cpp
    patch/PatchMulDx9Zero.cpp
    patch/PatchLoopMetadata.cpp
    patch/PatchNullFragShader.cpp
    patch/PatchPeepholeOpt.cpp
    patch/PatchPreparePipelineAbi.cpp
    patch/PatchReadFirstLane.cpp
    patch/PatchResourceCollect.cpp
    patch/PatchSetupTargetFeatures.cpp
    patch/TcsPassthroughShader.cpp
    patch/PatchInitializeWorkgroupMemory.cpp
    patch/PatchWorkarounds.cpp
    patch/ShaderInputs.cpp
    patch/ShaderMerger.cpp
    patch/SystemValues.cpp
    patch/VertexFetch.cpp
    patch/PatchImageOpCollect.cpp
    patch/RegisterMetadataBuilder.cpp
    patch/CombineCooperativeMatrix.cpp
    patch/LowerCooperativeMatrix.cpp
    patch/LowerGpuRt.cpp
    patch/LowerRayQueryWrapper.cpp
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

# lgc/util
target_sources(LLVMlgc PRIVATE
    util/AddressExtender.cpp
    util/Debug.cpp
    util/GfxRegHandlerBase.cpp
    util/GfxRegHandler.cpp
    util/Internal.cpp
    util/MbStandardInstrumentations.cpp
    util/ModuleBunch.cpp
    util/PassManager.cpp
    util/StartStopTimer.cpp
)

# lgc/interface/lgc
target_sources(LLVMlgc PRIVATE interface/lgc/LgcDialect.td)

add_subdirectory(disassembler)
add_subdirectory(tool/lgc)
add_subdirectory(test)

if (LLPC_BUILD_TESTS)
  add_subdirectory(unittests)
endif()

target_link_libraries(LLVMlgc PRIVATE LLVMRaytracing)
