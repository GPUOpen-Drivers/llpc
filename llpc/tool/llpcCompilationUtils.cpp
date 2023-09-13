/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcCompilationUtils.cpp
 * @brief LLPC source file: contains the implementation of standalone LLPC compilation logic.
 ***********************************************************************************************************************
 */
#ifdef WIN_OS
// NOTE: Disable Windows-defined min()/max() because we use STL-defined std::min()/std::max() in LLPC.
#define NOMINMAX
#endif

#include "llpcCompilationUtils.h"
#include "llpcAutoLayout.h"
#include "llpcDebug.h"
#include "llpcError.h"
#include "llpcInputUtils.h"
#include "llpcShaderModuleHelper.h"
#include "llpcSpirvLowerUtil.h"
#include "llpcThreading.h"
#include "llpcUtil.h"
#include "spvgen.h"
#include "vfx.h"
#include "vkgcElfReader.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <mutex>

using namespace llvm;
using namespace Vkgc;

namespace Llpc {
namespace StandaloneCompiler {

// =====================================================================================================================
// Callback function to allocate buffer for building shader module and building pipeline.
//
// @param instance : Placeholder instance object, unused
// @param userData : User data
// @param size : Requested allocation size
// @returns : Pointer to the allocated memory
void *VKAPI_CALL allocateBuffer(void *instance, void *userData, size_t size) {
  (void)instance;
  void *allocBuf = malloc(size);
  memset(allocBuf, 0, size);

  assert(userData);
  auto *outBuf = reinterpret_cast<void **>(userData);
  *outBuf = allocBuf;
  return allocBuf;
}

// =====================================================================================================================
// Performs cleanup work for LLPC standalone compiler.
//
// @param [in/out] compileInfo : Compilation info of LLPC standalone tool
void cleanupCompileInfo(CompileInfo *compileInfo) {
  for (unsigned i = 0; i < compileInfo->shaderModuleDatas.size(); ++i) {
    // NOTE: We do not have to free SPIR-V binary for pipeline info file.
    // It will be freed when we close the VFX doc.
    if (!compileInfo->pipelineInfoFile)
      delete[] reinterpret_cast<const char *>(compileInfo->shaderModuleDatas[i].spirvBin.pCode);

    free(compileInfo->shaderModuleDatas[i].shaderBuf);
  }

  free(compileInfo->pipelineBuf);

  if (compileInfo->pipelineInfoFile)
    Vfx::vfxCloseDoc(compileInfo->pipelineInfoFile);
}

// =====================================================================================================================
// Translates GLSL source language to corresponding shader stage.
//
// @param sourceLang : GLSL source language
static ShaderStage sourceLangToShaderStage(SpvGenStage sourceLang) {
  switch (sourceLang) {
  case SpvGenStageTask:
    return ShaderStage::ShaderStageTask;
  case SpvGenStageVertex:
    return ShaderStage::ShaderStageVertex;
  case SpvGenStageTessControl:
    return ShaderStage::ShaderStageTessControl;
  case SpvGenStageTessEvaluation:
    return ShaderStage::ShaderStageTessEval;
  case SpvGenStageGeometry:
    return ShaderStage::ShaderStageGeometry;
  case SpvGenStageMesh:
    return ShaderStage::ShaderStageMesh;
  case SpvGenStageFragment:
    return ShaderStage::ShaderStageFragment;
  case SpvGenStageCompute:
    return ShaderStage::ShaderStageCompute;
  case SpvGenStageRayTracingRayGen:
    return ShaderStage::ShaderStageRayTracingRayGen;
  case SpvGenStageRayTracingIntersect:
    return ShaderStage::ShaderStageRayTracingIntersect;
  case SpvGenStageRayTracingAnyHit:
    return ShaderStage::ShaderStageRayTracingAnyHit;
  case SpvGenStageRayTracingClosestHit:
    return ShaderStage::ShaderStageRayTracingClosestHit;
  case SpvGenStageRayTracingMiss:
    return ShaderStage::ShaderStageRayTracingMiss;
  case SpvGenStageRayTracingCallable:
    return ShaderStage::ShaderStageRayTracingCallable;
  default:
    llvm_unreachable("Unexpected shading language type!");
    return ShaderStage::ShaderStageInvalid;
  }
}

// =====================================================================================================================
// Disassemble SPIR-V binary file.
//
// @param binSize: Binary size.
// @param code: Pointer to binary.
// @param header: Extra info.
static void disassembleSpirv(unsigned binSize, const void *code, const llvm::Twine &header) {
  assert(EnableOuts());
  unsigned textSize = binSize * 10 + 1024;
  std::vector<char> spvText(textSize);
  LLPC_OUTS("\nSPIR-V disassembly for " << header << ":\n");
  spvDisassembleSpirv(binSize, code, textSize, spvText.data());
  LLPC_OUTS(spvText.data() << "\n");
}

// =====================================================================================================================
// GLSL compiler, compiles GLSL source text file (input) to SPIR-V binary file (output).
//
// @param inFilename : Input filename, GLSL source text
// @param [out] stage : Shader stage
// @param defaultEntryTarget : Default shader entry point name
// @returns : BinaryData object of the output SPIR-V binary on success, `ResultError` on failure
Expected<BinaryData> compileGlsl(const std::string &inFilename, ShaderStage *stage,
                                 const std::string &defaultEntryTarget) {
  if (!InitSpvGen())
    return createResultError(Result::ErrorUnavailable, "Failed to load SPVGEN -- cannot compile GLSL");

  bool isHlsl = false;
  SpvGenStage lang = spvGetStageTypeFromName(inFilename.c_str(), &isHlsl);
  if (lang == SpvGenStageInvalid)
    return createResultError(Result::ErrorInvalidShader,
                             Twine("File ") + inFilename + ": Bad file extension; try --help");

  // Check that GLSL entry point is 'main'. For details, see
  // https://www.khronos.org/registry/OpenGL/specs/gl/GLSLangSpec.4.60.html#function-definitions.
  if (!isHlsl && defaultEntryTarget != "main")
    return createResultError(Result::ErrorInvalidShader,
                             Twine("GLSL requires the entry point to be 'main': ") + inFilename);

  *stage = sourceLangToShaderStage(lang);

  FILE *inFile = fopen(inFilename.c_str(), "r");
  if (!inFile)
    return createResultError(Result::ErrorUnavailable, Twine("Failed to open input file: ") + inFilename);
  auto closeInput = make_scope_exit([inFile] { fclose(inFile); });

  fseek(inFile, 0, SEEK_END);
  size_t textSize = ftell(inFile);
  fseek(inFile, 0, SEEK_SET);

  char *glslText = new char[textSize + 1]();
  auto deleteGlslText = make_scope_exit([&glslText] { delete[] glslText; });
  auto readSize = fread(glslText, 1, textSize, inFile);
  glslText[readSize] = '\0';

  LLPC_OUTS("===============================================================================\n");
  LLPC_OUTS("// GLSL sources: " << inFilename << "\n\n");
  LLPC_OUTS(glslText);
  LLPC_OUTS("\n\n");

  int sourceStringCount = 1;
  const char *const *sourceList[1] = {};
  const char *fileName = inFilename.c_str();
  const char *const *fileList[1] = {&fileName};
  sourceList[0] = &glslText;

  void *program = nullptr;
  const char *log = nullptr;
  int compileOption = SpvGenOptionDefaultDesktop | SpvGenOptionVulkanRules;
  compileOption |= isHlsl ? SpvGenOptionReadHlsl : 0;
  const char *entryPoints[] = {defaultEntryTarget.c_str()};
  bool compileResult = spvCompileAndLinkProgramEx(1, &lang, &sourceStringCount, sourceList, fileList,
                                                  isHlsl ? entryPoints : nullptr, &program, &log, compileOption);

  LLPC_OUTS("// GLSL program compile/link log\n");

  if (!compileResult)
    return createResultError(Result::ErrorInvalidShader,
                             Twine("Failed to compile GLSL input file:") + inFilename + "\n" + log);

  const unsigned *spvBin = nullptr;
  unsigned binSize = spvGetSpirvBinaryFromProgram(program, 0, &spvBin);

  // We create / copy the binary blob to a new allocation. The caller is
  // responsible for calling delete[] (note: this will normally happen as part of
  // cleanupCompileInfo).
  void *bin = new char[binSize]();
  llvm::copy(llvm::make_range(spvBin, spvBin + binSize / sizeof(unsigned)), reinterpret_cast<unsigned *>(bin));

  if (EnableOuts())
    disassembleSpirv(binSize, spvBin, inFilename);

  return BinaryData{static_cast<size_t>(binSize), bin};
}

// =====================================================================================================================
// SPIR-V assembler, converts SPIR-V assembly text file (input) to SPIR-V binary file (output).
//
// @param inFilename : Input filename, SPIR-V assembly text
// @returns : BinaryData object of the assembled SPIR-V on success, `ResultError` on failure.
Expected<BinaryData> assembleSpirv(const std::string &inFilename) {
  if (!InitSpvGen())
    return createResultError(Result::ErrorUnavailable,
                             "Failed to load SPVGEN -- cannot assemble SPIR-V assembler source");

  FILE *inFile = fopen(inFilename.c_str(), "r");
  if (!inFile)
    return createResultError(Result::ErrorUnavailable, Twine("Failed to open input file: ") + inFilename);
  auto closeInput = make_scope_exit([inFile] { fclose(inFile); });

  fseek(inFile, 0, SEEK_END);
  size_t textSize = ftell(inFile);
  fseek(inFile, 0, SEEK_SET);

  std::vector<char> spvText(textSize + 1, '\0');
  size_t realSize = fread(spvText.data(), 1, textSize, inFile);
  spvText[realSize] = '\0';

  int binSize = static_cast<int>(realSize) * 4 + 1024; // Estimated SPIR-V binary size.
  std::vector<unsigned> spvBin(binSize / sizeof(unsigned), 0);

  const char *log = nullptr;
  binSize = spvAssembleSpirv(spvText.data(), binSize, spvBin.data(), &log);
  if (binSize < 0)
    return createResultError(Result::ErrorInvalidShader, Twine("Failed to assemble SPIR-V: \n") + log);

  // Caller is responsible for calling delete[] (note: this will normally happen
  // as part of cleanupCompileInfo).
  char *bin = new char[binSize]();
  llvm::copy(llvm::make_range(spvBin.data(), spvBin.data() + binSize / sizeof(unsigned)),
             reinterpret_cast<unsigned *>(bin));

  LLPC_OUTS("===============================================================================\n");
  LLPC_OUTS("// SPIR-V disassembly: " << inFilename << "\n");
  LLPC_OUTS(spvText.data());
  LLPC_OUTS("\n\n");

  return BinaryData{static_cast<size_t>(binSize), bin};
}

// =====================================================================================================================
// Decodes the binary after building a pipeline and outputs the decoded info.
//
// @param pipelineBin : Pipeline binary
// @param [in/out] compileInfo : Compilation info of LLPC standalone tool
// @returns : Always returns Result::Success
Result decodePipelineBinary(const BinaryData *pipelineBin, CompileInfo *compileInfo) {
  // Ignore failure from ElfReader. It fails if pipelineBin is not ELF, as happens with
  // -filetype=asm.
  ElfReader<Elf64> reader(compileInfo->gfxIp);
  size_t readSize = 0;
  if (reader.ReadFromBuffer(pipelineBin->pCode, &readSize) == Result::Success) {
    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC final ELF info\n");
    LLPC_OUTS(reader);
  }

  return Result::Success;
}

// =====================================================================================================================
// Builds shader module based on the specified SPIR-V binary.
//
// @param compiler : LLPC compiler object
// @param [in/out] compileInfo : Compilation info of LLPC standalone tool
// @returns : `ErrorSuccess` on success, `ResultError` on failure
Error buildShaderModules(ICompiler *compiler, CompileInfo *compileInfo) {
  for (ShaderModuleData &shaderModuleData : compileInfo->shaderModuleDatas) {
    ShaderModuleBuildInfo *shaderInfo = &shaderModuleData.shaderInfo;
    ShaderModuleBuildOut *shaderOut = &shaderModuleData.shaderOut;

    shaderInfo->pInstance = nullptr; // Placeholder, unused.
    shaderInfo->pUserData = &shaderModuleData.shaderBuf;
    shaderInfo->pfnOutputAlloc = allocateBuffer;
    shaderInfo->shaderBin = shaderModuleData.spirvBin;

    Result result = compiler->BuildShaderModule(shaderInfo, shaderOut);
    if (result != Result::Success && result != Result::Delayed)
      return createResultError(result, Twine("Failed to build ") + getShaderStageName(shaderModuleData.shaderStage) +
                                           " shader module");
  }

  return Error::success();
}

// =====================================================================================================================
// Process one pipeline input file.
//
// @param compiler : LLPC compiler
// @param inputSpec : Input specification
// @param unlinked : Whether to build an unlinked shader/part-pipeline ELF
// @param ignoreColorAttachmentFormats : Whether to ignore color attachment formats
// @returns : `ErrorSuccess` on success, `ResultError` on failure
Error processInputPipeline(ICompiler *compiler, CompileInfo &compileInfo, const InputSpec &inputSpec, bool unlinked,
                           bool ignoreColorAttachmentFormats) {
  const std::string &inFile = inputSpec.filename;
  const char *log = nullptr;
  const bool vfxResult =
      Vfx::vfxParseFile(inFile.c_str(), 0, nullptr, VfxDocTypePipeline, &compileInfo.pipelineInfoFile, &log);
  if (!vfxResult)
    return createResultError(Result::ErrorInvalidShader, Twine("Failed to parse input file: ") + inFile + "\n" + log);

  VfxPipelineStatePtr pipelineState = nullptr;
  Vfx::vfxGetPipelineDoc(compileInfo.pipelineInfoFile, &pipelineState);

  if (pipelineState->version != Vkgc::Version) {
    std::string errMsg;
    raw_string_ostream os(errMsg);
    os << "Version incompatible, SPVGEN::Version = " << pipelineState->version << " LLPC::Version = " << Vkgc::Version;
    return createResultError(Result::ErrorInvalidShader, os.str());
  }

  LLPC_OUTS("===============================================================================\n");
  LLPC_OUTS("// Pipeline file info for " << inFile << " \n\n");

  if (log && strlen(log) > 0)
    LLPC_OUTS("Pipeline file parse warning:\n" << log << "\n");

  compileInfo.compPipelineInfo = pipelineState->compPipelineInfo;
  compileInfo.gfxPipelineInfo = pipelineState->gfxPipelineInfo;
  compileInfo.rayTracePipelineInfo = pipelineState->rayPipelineInfo;
  compileInfo.pipelineType = pipelineState->pipelineType;

  if (ignoreColorAttachmentFormats) {
    // NOTE: When this option is enabled, we set color attachment format to
    // R8G8B8A8_SRGB for color target 0. Also, for other color targets, if the
    // formats are not UNDEFINED, we set them to R8G8B8A8_SRGB as well.
    for (unsigned target = 0; target < MaxColorTargets; ++target) {
      if (target == 0 || compileInfo.gfxPipelineInfo.cbState.target[target].format != VK_FORMAT_UNDEFINED)
        compileInfo.gfxPipelineInfo.cbState.target[target].format = VK_FORMAT_R8G8B8A8_SRGB;
    }
  }

  if (EnableOuts() && !InitSpvGen())
    LLPC_OUTS("Failed to load SPVGEN -- cannot disassemble and validate SPIR-V\n");

  for (unsigned stage = 0; stage < pipelineState->numStages; ++stage) {
    if (pipelineState->stages[stage].dataSize > 0) {
      StandaloneCompiler::ShaderModuleData shaderModuleData = {};
      shaderModuleData.spirvBin.codeSize = pipelineState->stages[stage].dataSize;
      shaderModuleData.spirvBin.pCode = pipelineState->stages[stage].pData;
      shaderModuleData.shaderStage = pipelineState->stages[stage].stage;

      compileInfo.shaderModuleDatas.push_back(shaderModuleData);
      compileInfo.stageMask |= shaderStageToMask(pipelineState->stages[stage].stage);
      if (EnableOuts())
        disassembleSpirv(pipelineState->stages[stage].dataSize, shaderModuleData.spirvBin.pCode,
                         Twine(getShaderStageName(pipelineState->stages[stage].stage)) + " shader module");
    }
  }

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
  const BinaryData *shaderLibrary = nullptr;
  if (pipelineState->pipelineType == VfxPipelineTypeRayTracing) {
    shaderLibrary = &pipelineState->rayPipelineInfo.shaderTraceRay;
  } else if (pipelineState->pipelineType == VfxPipelineTypeCompute)
    shaderLibrary = &pipelineState->compPipelineInfo.shaderLibrary;
  else {
    assert(pipelineState->pipelineType == VfxPipelineTypeGraphics);
    shaderLibrary = &pipelineState->gfxPipelineInfo.shaderLibrary;
  }
  if (shaderLibrary->codeSize > 0 && EnableOuts())
    disassembleSpirv(shaderLibrary->codeSize, shaderLibrary->pCode, "Ray tracing library");
#endif

  const bool isGraphics = compileInfo.pipelineType == VfxPipelineTypeGraphics;
  assert(!(isGraphics && isComputePipeline(compileInfo.stageMask)) && "Bad stage mask");

  for (unsigned i = 0; i < compileInfo.shaderModuleDatas.size(); ++i) {
    compileInfo.shaderModuleDatas[i].shaderInfo.options.pipelineOptions =
        isGraphics ? compileInfo.gfxPipelineInfo.options : compileInfo.compPipelineInfo.options;
  }

  // For a .pipe, build an "unlinked" shader/part-pipeline ELF if -unlinked is on.
  compileInfo.unlinked = unlinked;
  compileInfo.doAutoLayout = false;
  compileInfo.inputSpecs.push_back(inputSpec);
  return Error::success();
}

// =====================================================================================================================
// Processes a single SPIR-V input file (text or binary).
//
// @param spirvInput : Input specification
// @param validateSpirv : Whether to validate the input SPIR-V
// @returns : `ShaderModuleData` on success, `ResultError` on failure
static Expected<ShaderModuleData> processInputSpirvStage(const InputSpec &spirvInput, bool validateSpirv) {
  assert(isSpirvBinaryFile(spirvInput.filename) || isSpirvTextFile(spirvInput.filename));
  BinaryData spvBin = {};
  // SPIR-V assembly text or SPIR-V binary.
  if (isSpirvTextFile(spirvInput.filename)) {
    auto spvBinDataOrErr = assembleSpirv(spirvInput.filename);
    if (Error err = spvBinDataOrErr.takeError())
      return std::move(err);
    spvBin = *spvBinDataOrErr;
  } else {
    auto spvBinOrErr = getSpirvBinaryFromFile(spirvInput.filename);
    if (Error err = spvBinOrErr.takeError())
      return std::move(err);
    spvBin = *spvBinOrErr;
  }

  const bool isSpvGenLoaded = InitSpvGen();
  if (!isSpvGenLoaded) {
    LLPC_OUTS("Failed to load SPVGEN -- no SPIR-V disassembler available\n");
  } else {
    if (EnableOuts())
      disassembleSpirv(spvBin.codeSize, spvBin.pCode, spirvInput.filename);
  }

  if (validateSpirv) {
    if (!isSpvGenLoaded) {
      LLPC_OUTS("Warning: Failed to load SPVGEN -- cannot validate SPIR-V\n");
    } else {
      char log[1024] = {};
      if (!spvValidateSpirv(spvBin.codeSize, spvBin.pCode, sizeof(log), log))
        return createResultError(Result::ErrorInvalidShader, Twine("Failed to validate SPIR-V:\n") + log);
    }
  }

  // NOTE: If the entry target is not specified, we set it to the one gotten from SPIR-V binary.
  std::string entryPoint = spirvInput.entryPoint;
  if (entryPoint.empty())
    entryPoint = Vkgc::getEntryPointNameFromSpirvBinary(&spvBin);

  unsigned stageMask = ShaderModuleHelper::getStageMaskFromSpirvBinary(&spvBin, entryPoint.c_str());
  auto shaderStages = maskToShaderStages(stageMask);
  if (shaderStages.empty())
    return createResultError(Result::ErrorInvalidShader,
                             Twine("Failed to identify shader stages by entry-point \"") + entryPoint + "\"");

  ShaderStage stage = shaderStages.front(); // Note: there can be more than one stage, but we always pick the first one.
  StandaloneCompiler::ShaderModuleData shaderModuleData = {};
  shaderModuleData.shaderStage = stage;
  shaderModuleData.entryPoint = entryPoint;
  shaderModuleData.spirvBin = spvBin;
  return std::move(shaderModuleData);
}

// =====================================================================================================================
// Processes a single LLVM IR input file.
//
// @param llvmIrInput : Input specification
// @returns : `ShaderModuleData` on success, `ResultError` on failure
static Expected<ShaderModuleData> processInputLlvmIrStage(const InputSpec &llvmIrInput) {
  assert(isLlvmIrFile(llvmIrInput.filename));
  LLVMContext context;
  SMDiagnostic errDiag;

  // Load LLVM IR.
  std::unique_ptr<Module> module = parseAssemblyFile(llvmIrInput.filename, errDiag, context, nullptr);
  if (!module) {
    std::string errMsg;
    raw_string_ostream errStream(errMsg);
    errDiag.print(llvmIrInput.filename.c_str(), errStream);
    return createResultError(Result::ErrorInvalidShader, errStream.str());
  }

  // Verify LLVM module.
  std::string errMsg;
  raw_string_ostream errStream(errMsg);
  if (verifyModule(*module.get(), &errStream)) {
    errStream.flush();
    return createResultError(Result::ErrorInvalidShader, Twine("File ") + llvmIrInput.filename +
                                                             " parsed, but failed to verify the module: " + errMsg);
  }

  // Check the shader stage of input module.
  ShaderStage shaderStage = getShaderStageFromModule(module.get());
  if (shaderStage == ShaderStageInvalid)
    return createResultError(Result::ErrorInvalidShader,
                             Twine("File ") + llvmIrInput.filename +
                                 " parsed, but failed to determine shader stage: " + errMsg);

  // Translate LLVM module to LLVM bitcode.
  SmallString<1024> bitcodeBuf;
  raw_svector_ostream bitcodeStream(bitcodeBuf);
  WriteBitcodeToFile(*module.get(), bitcodeStream);
  void *code = new uint8_t[bitcodeBuf.size()];
  memcpy(code, bitcodeBuf.data(), bitcodeBuf.size());

  StandaloneCompiler::ShaderModuleData shaderModuleData = {};
  shaderModuleData.spirvBin.codeSize = bitcodeBuf.size();
  shaderModuleData.spirvBin.pCode = code;
  shaderModuleData.shaderStage = shaderStage;
  shaderModuleData.disableDoAutoLayout = true;
  return std::move(shaderModuleData);
}

// =====================================================================================================================
// Processes a single GLSL input file. Translates the source to a SPIR-V binary.
//
// @param glslInput : Input specification
// @returns : `ShaderModuleData` on success, `ResultError` on failure
static Expected<ShaderModuleData> processInputGlslStage(const InputSpec &glslInput) {
  assert(isGlslShaderTextFile(glslInput.filename));
  // Note: If the entry target is not specified, we set it to the GLSL default.
  const std::string entryPoint = glslInput.entryPoint.empty() ? "main" : glslInput.entryPoint;
  ShaderStage stage = ShaderStageInvalid;
  auto spvBinOrErr = compileGlsl(glslInput.filename, &stage, entryPoint);
  if (Error err = spvBinOrErr.takeError())
    return std::move(err);

  StandaloneCompiler::ShaderModuleData shaderModuleData = {};
  shaderModuleData.shaderStage = stage;
  // In SPIR-V, we always set the entry point to "main", regardless of the entry point name in GLSL.
  shaderModuleData.entryPoint = entryPoint;
  shaderModuleData.spirvBin = *spvBinOrErr;

  return std::move(shaderModuleData);
}

// =====================================================================================================================
// Processes a single shader stage input file. Translates sources to SPIR-V binaries, if necessary.
//
// @param inputSpec : Input specification
// @param validateSpirv : Whether to run the validator on each final SPIR-V module
// @returns : `ShaderModuleData` on success, `ResultError` on failure
static Expected<ShaderModuleData> processInputStage(const InputSpec &inputSpec, bool validateSpirv) {
  const std::string &inFile = inputSpec.filename;

  if (isSpirvTextFile(inFile) || isSpirvBinaryFile(inFile))
    return processInputSpirvStage(inputSpec, validateSpirv);

  if (isLlvmIrFile(inFile))
    return processInputLlvmIrStage(inputSpec);

  if (isGlslShaderTextFile(inFile))
    return processInputGlslStage(inputSpec);

  return createResultError(Result::ErrorInvalidShader,
                           Twine("File ") + inFile +
                               " has an unknown extension; try -help to list supported input formats");
}

// =====================================================================================================================
// Processes multiple shader stage input files. Translates sources to SPIR-V binaries, if necessary.
// Adds compilation results to `compileInfo`.
//
// @param [in/out] compileInfo : Compilation context of the current pipeline
// @param inputSpec : Input specifications
// @param validateSpirv : Whether to run the validator on each final SPIR-V module
// @param numThreads : Number of CPU threads to use to process stages, where 0 means all logical cores
// @returns : `ErrorSuccess` on success, `ResultError` on failure
Error processInputStages(CompileInfo &compileInfo, ArrayRef<InputSpec> inputSpecs, bool validateSpirv,
                         unsigned numThreads) {
  std::mutex compileInfoMutex;

  return parallelFor(numThreads, inputSpecs, [&](const InputSpec &inputSpec) -> Error {
    auto dataOrErr = processInputStage(inputSpec, validateSpirv);
    if (Error err = dataOrErr.takeError())
      return err;

    ShaderModuleData &data = *dataOrErr;
    const ShaderStage stage = data.shaderStage;

    std::lock_guard<std::mutex> lock(compileInfoMutex);
    if (isShaderStageInMask(stage, compileInfo.stageMask))
      return createResultError(Result::ErrorInvalidShader,
                               Twine("Duplicate shader stage (") + getShaderStageName(stage) + ")");

    compileInfo.inputSpecs.push_back(inputSpec);
    compileInfo.stageMask |= shaderStageToMask(stage);
    if (data.disableDoAutoLayout)
      compileInfo.doAutoLayout = false;
    compileInfo.shaderModuleDatas.emplace_back(std::move(data));
    return Error::success();
  });
}

} // namespace StandaloneCompiler
} // namespace Llpc
