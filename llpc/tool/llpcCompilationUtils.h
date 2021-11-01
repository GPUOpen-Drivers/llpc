/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace Llpc {
namespace StandaloneCompiler {

// Represents the module info for a shader module.
struct ShaderModuleData {
  Llpc::ShaderStage shaderStage;          // Shader stage
  Llpc::BinaryData spirvBin;              // SPIR-V binary codes
  Llpc::ShaderModuleBuildInfo shaderInfo; // Info to build shader modules
  Llpc::ShaderModuleBuildOut shaderOut;   // Output of building shader modules
  void *shaderBuf;                        // Allocation buffer of building shader modules
};

// Represents global compilation info of LLPC standalone tool (as tool context).
struct CompileInfo {
  Llpc::GfxIpVersion gfxIp;                                            // Graphics IP version info
  VkFlags stageMask;                                                   // Shader stage mask
  std::vector<StandaloneCompiler::ShaderModuleData> shaderModuleDatas; // ShaderModule Data
  Llpc::GraphicsPipelineBuildInfo gfxPipelineInfo;                     // Info to build graphics pipeline
  Llpc::GraphicsPipelineBuildOut gfxPipelineOut;                       // Output of building graphics pipeline
  Llpc::ComputePipelineBuildInfo compPipelineInfo;                     // Info to build compute pipeline
  Llpc::ComputePipelineBuildOut compPipelineOut;                       // Output of building compute pipeline
  void *pipelineBuf;                                                   // Allocation buffer of building pipeline
  void *pipelineInfoFile;                                              // VFX-style file containing pipeline info
  const char *fileNames;                                               // Names of input shader source files
  std::string entryTarget;                                             // Name of the entry target function.
  bool unlinked;                  // Whether to generate unlinked shader/part-pipeline ELF
  bool relocatableShaderElf;      // Whether to enable relocatable shader compilation
  bool scalarBlockLayout;         // Whether to enable scalar block layout
  bool doAutoLayout;              // Whether to auto layout descriptors
  bool autoLayoutDesc;            // Whether to automatically create descriptor layout based on resource usages
  bool robustBufferAccess;        // Whether to enable robust buffer access
  bool scratchAccessBoundsChecks; // Whether to enable scratch access bounds checks
};

// Callback function to allocate buffer for building shader module and building pipeline.
void *VKAPI_CALL allocateBuffer(void *instance, void *userData, size_t size);

// Performs cleanup work for LLPC standalone compiler.
void cleanupCompileInfo(CompileInfo *compileInfo);

// GLSL compiler, compiles GLSL source text file (input) to SPIR-V binary file (output).
Result compileGlsl(const std::string &inFilename, ShaderStage *stage, std::string &outFilename,
                   const std::string &defaultEntryTarget);

// SPIR-V assembler, converts SPIR-V assembly text file (input) to SPIR-V binary file (output).
Result assembleSpirv(const std::string &inFilename, std::string &outFilename);

// Decodes the binary after building a pipeline and outputs the decoded info.
Result decodePipelineBinary(const BinaryData *pipelineBin, CompileInfo *compileInfo, bool isGraphics);

// Builds shader module based on the specified SPIR-V binary.
Result buildShaderModules(ICompiler *compiler, CompileInfo *compileInfo);

// Builds pipeline and does linking.
Result buildPipeline(ICompiler *compiler, CompileInfo *compileInfo,
                     llvm::Optional<Vkgc::PipelineDumpOptions> pipelineDumpOptions, bool timePasses);

// Output LLPC resulting binary (ELF binary, ISA assembly text, or LLVM bitcode) to the specified target file.
Result outputElf(CompileInfo *compileInfo, const std::string &suppliedOutFile, llvm::StringRef firstInFile);

// Processes and compiles one pipeline input file.
Result processInputPipeline(ICompiler *compiler, CompileInfo &compileInfo, const std::string &inFile, bool unlinked,
                            bool ignoreColorAttachmentFormats);

// Processes and compiles multiple shader stage input files.
Result processInputStages(ICompiler *compiler, CompileInfo &compileInfo, llvm::ArrayRef<std::string> inFiles,
                          bool validateSpirv, std::string &fileNames);

} // namespace StandaloneCompiler
} // namespace Llpc
