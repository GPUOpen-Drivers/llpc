##
 #######################################################################################################################
 #
 #  Copyright (c) 2020-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
    ipo
    Linker
    MC
    Object
    Passes
    ScalarOpts
    SelectionDAG
    Support
    Target
    TransformUtils
    Vectorize
)

### Cached Project Options #############################################################################################
option(LLPC_BUILD_NAVI12 "LLPC support for NAVI12?" OFF)
option(LLPC_ENABLE_WERROR "Build LLPC with more errors" OFF)

### Compiler Options ###################################################################################################
include(../cmake/CompilerFlags.cmake)
set_compiler_options(LLVMlgc ${LLPC_ENABLE_WERROR})

### Defines/Includes/Sources ###########################################################################################
target_compile_definitions(LLVMlgc PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)
if (LLPC_CLIENT_INTERFACE_MAJOR_VERSION)
    target_compile_definitions(LLVMlgc PRIVATE LLPC_CLIENT_INTERFACE_MAJOR_VERSION=${LLPC_CLIENT_INTERFACE_MAJOR_VERSION})
endif()
target_compile_definitions(LLVMlgc PRIVATE
        LITTLEENDIAN_CPU
        CHIP_HDR_NAVI14
        CHIP_HDR_NAVI21
        CHIP_HDR_NAVI22
        CHIP_HDR_NAVI23
        CHIP_HDR_NAVI24
        )

target_compile_definitions(LLVMlgc PRIVATE CHIP_HDR_RENOIR)

  if(LLPC_BUILD_NAVI12)
    target_compile_definitions(LLVMlgc PRIVATE
        LLPC_BUILD_NAVI12
        CHIP_HDR_NAVI12
    )
  endif()

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
    $<INSTALL_INTERFACE:interface>
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/imported
)

#if VKI_RAY_TRACING
if(VKI_RAY_TRACING)
  target_compile_definitions(LLVMlgc PRIVATE VKI_RAY_TRACING=1)
endif()
#endif

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
    elfLinker/FetchShader.cpp
    elfLinker/GlueShader.cpp
    elfLinker/NullFragmentShader.cpp
    elfLinker/RelocHandler.cpp
)

# lgc/patch
target_sources(LLVMlgc PRIVATE
    patch/ConfigBuilderBase.cpp
    patch/FragColorExport.cpp
    patch/Gfx6Chip.cpp
    patch/Gfx6ConfigBuilder.cpp
    patch/Gfx9Chip.cpp
    patch/Gfx9ConfigBuilder.cpp
    patch/MeshTaskShader.cpp
    patch/NggLdsManager.cpp
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
    patch/PatchLoopMetadata.cpp
    patch/PatchNullFragShader.cpp
    patch/PatchPeepholeOpt.cpp
    patch/PatchPreparePipelineAbi.cpp
    patch/PatchReadFirstLane.cpp
    patch/PatchResourceCollect.cpp
    patch/PatchSetupTargetFeatures.cpp
    patch/PatchInitializeWorkgroupMemory.cpp
    patch/PatchWorkarounds.cpp
    patch/ShaderInputs.cpp
    patch/ShaderMerger.cpp
    patch/SystemValues.cpp
    patch/VertexFetch.cpp
    patch/PatchWaveSizeAdjust.cpp
)

# lgc/state
target_sources(LLVMlgc PRIVATE
    state/Compiler.cpp
    state/LgcContext.cpp
    state/PalMetadata.cpp
    state/PassManagerCache.cpp
    state/PipelineShaders.cpp
    state/PipelineState.cpp
    state/ResourceUsage.cpp
    state/ShaderModes.cpp
    state/ShaderStage.cpp
    state/TargetInfo.cpp
)

# lgc/util
target_sources(LLVMlgc PRIVATE
    util/AddressExtender.cpp
    util/Debug.cpp
    util/GfxRegHandlerBase.cpp
    util/GfxRegHandler.cpp
    util/Internal.cpp
    util/PassManager.cpp
    util/StartStopTimer.cpp
)

add_subdirectory(disassembler)
add_subdirectory(tool/lgc)
add_subdirectory(test)

if (LLPC_BUILD_TESTS)
  add_subdirectory(unittests)
endif()