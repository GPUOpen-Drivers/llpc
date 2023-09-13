/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021 Google LLC. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcCompilationUtils.h
 * @brief LLPC header file: compilation logic for standalone LLPC compilers.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llpcInputUtils.h"
#include "vfx.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Error.h"
#include <optional>

namespace Llpc {
namespace StandaloneCompiler {

// Represents the module info for a shader module.
struct ShaderModuleData {
  Llpc::ShaderStage shaderStage;          // Shader stage
  std::string entryPoint;                 // Shader entry point
  Llpc::BinaryData spirvBin;              // SPIR-V binary codes
  Llpc::ShaderModuleBuildInfo shaderInfo; // Info to build shader modules
  Llpc::ShaderModuleBuildOut shaderOut;   // Output of building shader modules
  void *shaderBuf;                        // Allocation buffer of building shader modules
  bool disableDoAutoLayout;               // Indicates whether to disable auto layout of descriptors
};

// Represents a single compilation context of a pipeline or a group of shaders.
// This is only used by the standalone compiler tool.
struct CompileInfo {
  Llpc::GfxIpVersion gfxIp;                                                  // Graphics IP version info
  llvm::SmallVector<InputSpec> inputSpecs;                                   // Input shader specification
  VkFlags stageMask;                                                         // Shader stage mask
  llvm::SmallVector<StandaloneCompiler::ShaderModuleData> shaderModuleDatas; // ShaderModule Data
  Llpc::GraphicsPipelineBuildInfo gfxPipelineInfo;                           // Info to build graphics pipeline
  Llpc::GraphicsPipelineBuildOut gfxPipelineOut;                             // Output of building graphics pipeline
  Llpc::ComputePipelineBuildInfo compPipelineInfo;                           // Info to build compute pipeline
  Llpc::ComputePipelineBuildOut compPipelineOut;                             // Output of building compute pipeline
  RayTracingPipelineBuildInfo rayTracePipelineInfo;                          // Info to build ray tracing pipeline
  RayTracingPipelineBuildOut rayTracingPipelineOut;                          // Output of building ray tracing pipeline
  unsigned bvhNodeStride;
  void *pipelineBuf;                   // Allocation buffer of building pipeline
  void *pipelineInfoFile;              // VFX-style file containing pipeline info
  bool unlinked;                       // Whether to generate unlinked shader/part-pipeline ELF
  bool relocatableShaderElf;           // Whether to enable relocatable shader compilation
  bool scalarBlockLayout;              // Whether to enable scalar block layout
  bool doAutoLayout;                   // Whether to auto layout descriptors
  bool autoLayoutDesc;                 // Whether to automatically create descriptor layout based on resource usages
  bool robustBufferAccess;             // Whether to enable robust buffer access
  bool scratchAccessBoundsChecks;      // Whether to enable scratch access bounds checks
  bool enableImplicitInvariantExports; // Whether to enable implicit marking of position exports as invariant
  VfxPipelineType pipelineType;        // Pipeline type
  std::optional<llvm::CodeGenOpt::Level> optimizationLevel; // The optimization level to pass the compiler
  bool internalRtShaders;                                   // Whether to enable intrinsics for internal RT shaders
  bool enableColorExportShader; // Enable color export shader, only compile each stage of the pipeline without linking
};

// Callback function to allocate buffer for building shader module and building pipeline.
void *VKAPI_CALL allocateBuffer(void *instance, void *userData, size_t size);

// Performs cleanup work for LLPC standalone compiler.
void cleanupCompileInfo(CompileInfo *compileInfo);

// GLSL compiler, compiles GLSL source text file (input) to SPIR-V BinaryData object (output).
llvm::Expected<BinaryData> compileGlsl(const std::string &inFilename, ShaderStage *stage,
                                       const std::string &defaultEntryTarget);

// SPIR-V assembler, converts SPIR-V assembly text file (input) to SPIR-V BinaryData object (output).
llvm::Expected<BinaryData> assembleSpirv(const std::string &inFilename);

// Decodes the binary after building a pipeline and outputs the decoded info.
LLPC_NODISCARD Result decodePipelineBinary(const BinaryData *pipelineBin, CompileInfo *compileInfo);

// Builds shader module based on the specified SPIR-V binary.
llvm::Error buildShaderModules(ICompiler *compiler, CompileInfo *compileInfo);

// Processes and compiles one pipeline input file.
llvm::Error processInputPipeline(ICompiler *compiler, CompileInfo &compileInfo, const InputSpec &inputSpec,
                                 bool unlinked, bool ignoreColorAttachmentFormats);

// Processes and compiles multiple shader stage input files.
llvm::Error processInputStages(CompileInfo &compileInfo, llvm::ArrayRef<InputSpec> inputSpecs, bool validateSpirv,
                               unsigned numThreads);

} // namespace StandaloneCompiler
} // namespace Llpc
