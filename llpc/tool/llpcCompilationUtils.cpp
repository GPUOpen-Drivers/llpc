/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llpcUtil.h"
#include "vkgcElfReader.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#ifndef LLPC_ENABLE_SPIRV_OPT
#define SPVGEN_STATIC_LIB 1
#endif

#include "spvgen.h"
#include "vfx.h"
#include <cassert>

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
  case SpvGenStageVertex:
    return ShaderStage::ShaderStageVertex;
  case SpvGenStageTessControl:
    return ShaderStage::ShaderStageTessControl;
  case SpvGenStageTessEvaluation:
    return ShaderStage::ShaderStageTessEval;
  case SpvGenStageGeometry:
    return ShaderStage::ShaderStageGeometry;
  case SpvGenStageFragment:
    return ShaderStage::ShaderStageFragment;
  case SpvGenStageCompute:
    return ShaderStage::ShaderStageCompute;
  default:
    llvm_unreachable("Unexpected shading language type!");
    return ShaderStage::ShaderStageInvalid;
  }
}

// =====================================================================================================================
// GLSL compiler, compiles GLSL source text file (input) to SPIR-V binary file (output).
//
// @param inFilename : Input filename, GLSL source text
// @param [out] stage : Shader stage
// @param defaultEntryTarget : Default shader entry point name
// @returns : Filename of the output SPIR-V binary on success, `ResultError` on failure
Expected<std::string> compileGlsl(const std::string &inFilename, ShaderStage *stage,
                                  const std::string &defaultEntryTarget) {
  if (!InitSpvGen())
    return createResultError(Result::ErrorUnavailable, "Failed to load SPVGEN -- cannot compile GLSL");

  bool isHlsl = false;
  SpvGenStage lang = spvGetStageTypeFromName(inFilename.c_str(), &isHlsl);
  if (lang == SpvGenStageInvalid)
    return createResultError(Result::ErrorInvalidShader,
                             Twine("File ") + inFilename + ": Bad file extension; try --help");
  *stage = sourceLangToShaderStage(lang);

  FILE *inFile = fopen(inFilename.c_str(), "r");
  if (!inFile)
    return createResultError(Result::ErrorUnavailable, Twine("Failed to open input file: ") + inFilename);
  auto closeInput = make_scope_exit([inFile] { fclose(inFile); });

  const std::string outFilename = sys::path::filename(inFilename).str() + Ext::SpirvBin.str();
  FILE *outFile = fopen(outFilename.c_str(), "wb");
  if (!outFile)
    return createResultError(Result::ErrorUnavailable,
                             Twine("Failed to compile GLSL input: Failed to open output file: ") + outFilename);
  auto closeOutput = make_scope_exit([outFile] { fclose(outFile); });

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
  int compileOption = SpvGenOptionDefaultDesktop | SpvGenOptionVulkanRules | SpvGenOptionDebug;
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
  fwrite(spvBin, 1, binSize, outFile);

  textSize = binSize * 10 + 1024;
  SmallVector<char> spvText(textSize, '\0');
  LLPC_OUTS("\nSPIR-V disassembly: " << outFilename << "\n");
  spvDisassembleSpirv(binSize, spvBin, textSize, spvText.data());
  LLPC_OUTS(spvText.data() << "\n");

  return outFilename;
}

// =====================================================================================================================
// SPIR-V assembler, converts SPIR-V assembly text file (input) to SPIR-V binary file (output).
//
// @param inFilename : Input filename, SPIR-V assembly text
// @returns : Filename of the assembled SPIR-V on success, `ResultError` on failure.
Expected<std::string> assembleSpirv(const std::string &inFilename) {
  if (!InitSpvGen())
    return createResultError(Result::ErrorUnavailable,
                             "Failed to load SPVGEN -- cannot assemble SPIR-V assembler source");

  FILE *inFile = fopen(inFilename.c_str(), "r");
  if (!inFile)
    return createResultError(Result::ErrorUnavailable, Twine("Failed to open input file: ") + inFilename);
  auto closeInput = make_scope_exit([inFile] { fclose(inFile); });

  const std::string outFilename = sys::path::stem(sys::path::filename(inFilename)).str() + Ext::SpirvBin.str();
  FILE *outFile = fopen(outFilename.c_str(), "wb");
  if (!outFile)
    return createResultError(Result::ErrorUnavailable, Twine("Failed to open output file: ") + outFilename);
  auto closeOutput = make_scope_exit([outFile] { fclose(outFile); });

  fseek(inFile, 0, SEEK_END);
  size_t textSize = ftell(inFile);
  fseek(inFile, 0, SEEK_SET);

  SmallVector<char, 0> spvText(textSize + 1, '\0');
  size_t realSize = fread(spvText.data(), 1, textSize, inFile);
  spvText[realSize] = '\0';

  int binSize = static_cast<int>(realSize) * 4 + 1024; // Estimated SPIR-V binary size.
  SmallVector<unsigned> spvBin(binSize / sizeof(unsigned), 0);

  const char *log = nullptr;
  binSize = spvAssembleSpirv(spvText.data(), binSize, spvBin.data(), &log);
  if (binSize < 0)
    return createResultError(Result::ErrorInvalidShader, Twine("Failed to assemble SPIR-V: \n") + log);

  fwrite(spvBin.data(), 1, binSize, outFile);

  LLPC_OUTS("===============================================================================\n");
  LLPC_OUTS("// SPIR-V disassembly: " << inFilename << "\n");
  LLPC_OUTS(spvText);
  LLPC_OUTS("\n\n");

  return outFilename;
}

// =====================================================================================================================
// Decodes the binary after building a pipeline and outputs the decoded info.
//
// @param pipelineBin : Pipeline binary
// @param [in/out] compileInfo : Compilation info of LLPC standalone tool
// @param isGraphics : Whether it is graphics pipeline
// @returns : Always returns Result::Success
Result decodePipelineBinary(const BinaryData *pipelineBin, CompileInfo *compileInfo, bool isGraphics) {
  // Ignore failure from ElfReader. It fails if pPipelineBin is not ELF, as happens with
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
// Output LLPC resulting binary (ELF binary, ISA assembly text, or LLVM bitcode) to the specified target file.
//
// @param compileInfo : Compilation info of LLPC standalone tool
// @param suppliedOutFile : Name of the file to output ELF binary (specify "" to use base name of first input file with
// appropriate extension; specify "-" to use stdout)
// @param firstInFile : Name of first input file
// @returns : `ErrorSuccess` on success, `ResultError` on failure
Error outputElf(CompileInfo *compileInfo, const std::string &suppliedOutFile, StringRef firstInFile) {
  const BinaryData &pipelineBin = (compileInfo->stageMask & shaderStageToMask(ShaderStageCompute))
                                      ? compileInfo->compPipelineOut.pipelineBin
                                      : compileInfo->gfxPipelineOut.pipelineBin;
  SmallString<64> outFileName(suppliedOutFile);
  if (outFileName.empty()) {
    // Detect the data type as we are unable to access the values of the options "-filetype" and "-emit-llvm".
    StringLiteral ext = fileExtFromBinary(pipelineBin);
    outFileName = sys::path::filename(firstInFile);
    sys::path::replace_extension(outFileName, ext);
  }

  return writeFile(pipelineBin, outFileName);
}

// =====================================================================================================================
// Process one pipeline input file.
//
// @param compiler : LLPC compiler
// @param inFile : Input filename
// @param unlinked : Whether to build an unlinked shader/part-pipeline ELF
// @param ignoreColorAttachmentFormats : Whether to ignore color attachment formats
// @returns : `ErrorSuccess` on success, `ResultError` on failure
Error processInputPipeline(ICompiler *compiler, CompileInfo &compileInfo, const std::string &inFile, bool unlinked,
                           bool ignoreColorAttachmentFormats) {
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

      if (spvDisassembleSpirv) {
        unsigned binSize = pipelineState->stages[stage].dataSize;
        unsigned textSize = binSize * 10 + 1024;
        LLPC_OUTS("\nSPIR-V disassembly for " << getShaderStageName(pipelineState->stages[stage].stage)
                                              << " shader module:\n");
        SmallVector<char> spvText(textSize);
        spvDisassembleSpirv(binSize, shaderModuleData.spirvBin.pCode, textSize, spvText.data());
        LLPC_OUTS(spvText.data() << "\n");
      }
    }
  }

  const bool isGraphics = (compileInfo.stageMask & shaderStageToMask(ShaderStageCompute)) == 0;
  for (unsigned i = 0; i < compileInfo.shaderModuleDatas.size(); ++i) {
    compileInfo.shaderModuleDatas[i].shaderInfo.options.pipelineOptions =
        isGraphics ? compileInfo.gfxPipelineInfo.options : compileInfo.compPipelineInfo.options;
  }

  // For a .pipe, build an "unlinked" shader/part-pipeline ELF if -unlinked is on.
  compileInfo.unlinked = unlinked;
  compileInfo.doAutoLayout = false;
  return Error::success();
}

// =====================================================================================================================
// Process multiple shader stage input files. Translates sources to SPIR-V binaries, if necessary.
//
// @param compiler : LLPC compiler
// @param inFiles : Input filenames
// @param validateSpirv : Whether to run the validator on each final SPIR-V module
// @param [out] filenames : List of used input file names
// @returns : `ErrorSuccess` on success, other `ResultError` on failure
Error processInputStages(ICompiler *compiler, CompileInfo &compileInfo, ArrayRef<std::string> inFiles,
                         bool validateSpirv, SmallVectorImpl<std::string> &fileNames) {
  for (const std::string &inFile : inFiles) {
    fileNames.push_back(inFile);
    std::string spvBinFile;

    if (isSpirvTextFile(inFile) || isSpirvBinaryFile(inFile)) {
      // SPIR-V assembly text or SPIR-V binary.
      if (isSpirvTextFile(inFile)) {
        auto spvBinFilenameOrErr = assembleSpirv(inFile);
        if (Error err = spvBinFilenameOrErr.takeError())
          return err;
        spvBinFile = *spvBinFilenameOrErr;
      } else {
        spvBinFile = inFile;
      }

      auto spvBinOrErr = getSpirvBinaryFromFile(spvBinFile);
      if (Error err = spvBinOrErr.takeError())
        return err;
      BinaryData spvBin = *spvBinOrErr;

      if (!InitSpvGen()) {
        LLPC_OUTS("Failed to load SPVGEN -- no SPIR-V disassembler available\n");
      } else {
        // Disassemble SPIR-V code
        unsigned textSize = spvBin.codeSize * 10 + 1024;
        SmallVector<char> spvText(textSize);
        LLPC_OUTS("\nSPIR-V disassembly for " << inFile << "\n");
        spvDisassembleSpirv(spvBin.codeSize, spvBin.pCode, textSize, spvText.data());
        LLPC_OUTS(spvText.data() << "\n");
      }

      if (validateSpirv) {
        if (!InitSpvGen()) {
          LLPC_OUTS("Warning: Failed to load SPVGEN -- cannot validate SPIR-V\n");
        } else {
          char log[1024] = {};
          if (!spvValidateSpirv(spvBin.codeSize, spvBin.pCode, sizeof(log), log))
            return createResultError(Result::ErrorInvalidShader, Twine("Failed to validate SPIR-V:\n") + log);
        }
      }

      // NOTE: If the entry target is not specified, we set it to the one gotten from SPIR-V binary.
      if (compileInfo.entryTarget.empty())
        compileInfo.entryTarget = Vkgc::getEntryPointNameFromSpirvBinary(&spvBin);

      unsigned stageMask = ShaderModuleHelper::getStageMaskFromSpirvBinary(&spvBin, compileInfo.entryTarget.c_str());
      if ((stageMask & compileInfo.stageMask) != 0)
        break;

      if (stageMask == 0)
        return createResultError(Result::ErrorInvalidShader,
                                 Twine("Failed to identify shader stages by entry-point \"") + compileInfo.entryTarget +
                                     "\"");

      for (unsigned stage = ShaderStageVertex; stage < ShaderStageCount; ++stage) {
        if (stageMask & shaderStageToMask(static_cast<ShaderStage>(stage))) {
          StandaloneCompiler::ShaderModuleData shaderModuleData = {};
          shaderModuleData.shaderStage = static_cast<ShaderStage>(stage);
          shaderModuleData.spirvBin = spvBin;
          compileInfo.shaderModuleDatas.push_back(shaderModuleData);
          compileInfo.stageMask |= shaderStageToMask(static_cast<ShaderStage>(stage));
          break;
        }
      }
    } else if (isLlvmIrFile(inFile)) {
      LLVMContext context;
      SMDiagnostic errDiag;

      // Load LLVM IR.
      std::unique_ptr<Module> module = parseAssemblyFile(inFile, errDiag, context, nullptr);
      if (!module) {
        std::string errMsg;
        raw_string_ostream errStream(errMsg);
        errDiag.print(inFile.c_str(), errStream);
        errStream.flush();
        return createResultError(Result::ErrorInvalidShader, errMsg);
      }

      // Verify LLVM module.
      std::string errMsg;
      raw_string_ostream errStream(errMsg);
      if (verifyModule(*module.get(), &errStream)) {
        errStream.flush();
        return createResultError(Result::ErrorInvalidShader,
                                 Twine("File ") + inFile + " parsed, but failed to verify the module: " + errMsg);
      }

      // Check the shader stage of input module.
      ShaderStage shaderStage = getShaderStageFromModule(module.get());
      if (shaderStage == ShaderStageInvalid) {
        return createResultError(Result::ErrorInvalidShader,
                                 Twine("File ") + inFile + " parsed, but failed to determine shader stage: " + errMsg);
      }

      if (compileInfo.stageMask & shaderStageToMask(static_cast<ShaderStage>(shaderStage)))
        break;

      // Translate LLVM module to LLVM bitcode.
      SmallString<1024> bitcodeBuf;
      raw_svector_ostream bitcodeStream(bitcodeBuf);
      WriteBitcodeToFile(*module.get(), bitcodeStream);
      void *code = new uint8_t[bitcodeBuf.size()];
      memcpy(code, bitcodeBuf.data(), bitcodeBuf.size());

      StandaloneCompiler::ShaderModuleData shaderModuledata = {};
      shaderModuledata.spirvBin.codeSize = bitcodeBuf.size();
      shaderModuledata.spirvBin.pCode = code;
      shaderModuledata.shaderStage = shaderStage;
      compileInfo.shaderModuleDatas.push_back(shaderModuledata);
      compileInfo.stageMask |= shaderStageToMask(static_cast<ShaderStage>(shaderStage));
      compileInfo.doAutoLayout = false;
    } else if (isGlslShaderTextFile(inFile)) {
      // GLSL source text

      // NOTE: If the entry target is not specified, we set it to GLSL default ("main").
      if (compileInfo.entryTarget.empty())
        compileInfo.entryTarget = "main";

      ShaderStage stage = ShaderStageInvalid;
      auto spvBinFilenameOrErr = compileGlsl(inFile, &stage, compileInfo.entryTarget);
      if (Error err = spvBinFilenameOrErr.takeError())
        return err;
      spvBinFile = *spvBinFilenameOrErr;

      if (compileInfo.stageMask & shaderStageToMask(static_cast<ShaderStage>(stage)))
        break;

      compileInfo.stageMask |= shaderStageToMask(stage);
      StandaloneCompiler::ShaderModuleData shaderModuleData = {};

      auto spvBinOrErr = getSpirvBinaryFromFile(spvBinFile);
      if (Error err = spvBinOrErr.takeError())
        return err;
      shaderModuleData.spirvBin = *spvBinOrErr;

      shaderModuleData.shaderStage = stage;
      compileInfo.shaderModuleDatas.push_back(shaderModuleData);
    } else {
      return createResultError(Result::ErrorInvalidShader,
                               Twine("File ") + inFile +
                                   " has an unknown extension; try -help to list supported input formats");
    }
  }
  return Error::success();
}

} // namespace StandaloneCompiler
} // namespace Llpc
