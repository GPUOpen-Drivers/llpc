/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ShaderMerger.h
 * @brief LLPC header file: contains declaration of class lgc::ShaderMerger.
 ***********************************************************************************************************************
 */
#pragma once

#include "NggPrimShader.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Internal.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Module.h"

namespace lgc {

class BuilderBase;
class PipelineState;

// Enumerates first 8 SGPRs (always loaded) for the LS-HS merged shader
namespace LsHs {
enum SpecialSgprInput : unsigned {
  UserDataAddrLow = 0,
  UserDataAddrHigh,
  OffChipLdsBase,
  MergedWaveInfo,
  TfBufferBase,
  HsShaderAddrLow,
  HsShaderAddrHigh,

  // GFX9~GFX10
  SharedScratchOffset,

  // GFX11+
  waveIdInGroup,
};
} // namespace LsHs

// Enumerates first 8 SGPRs (always loaded) for the ES-GS merged shader
namespace EsGs {
enum SpecialSgprInput : unsigned {
  UserDataAddrLow = 0,
  UserDataAddrHigh,
  MergedWaveInfo,
  OffChipLdsBase,
  GsShaderAddrLow,
  GsShaderAddrHigh,

  // GFX9~GFX10
  GsVsOffset, // Non-NGG
  SharedScratchOffset,

  // GFX10+
  MergedGroupInfo, // NGG

  // GFX11+
  AttribRingBase,
  FlatScratchLow,
  FlatScratchHigh,
};
} // namespace EsGs

static constexpr unsigned NumSpecialSgprInputs = 8; // First 8 SGPRs are defined or reserved by HW

// =====================================================================================================================
// Represents the manager doing shader merge operations.
class ShaderMerger {
public:
  ShaderMerger(PipelineState *pipelineState, PipelineShadersResult *pipelineShaders);

  static unsigned getSpecialSgprInputIndex(GfxIpVersion gfxIp, LsHs::SpecialSgprInput sgprInput);
  static unsigned getSpecialSgprInputIndex(GfxIpVersion gfxIp, EsGs::SpecialSgprInput sgprInput, bool useNgg = true);

  llvm::Function *generateLsHsEntryPoint(llvm::Function *lsEntryPoint, llvm::Function *hsEntryPoint);
  llvm::Function *generateEsGsEntryPoint(llvm::Function *esEntryPoint, llvm::Function *gsEntryPoint);
  llvm::Function *buildPrimShader(llvm::Function *esEntryPoint, llvm::Function *gsEntryPoint,
                                  llvm::Function *copyShaderEntryPoint);

private:
  ShaderMerger() = delete;
  ShaderMerger(const ShaderMerger &) = delete;
  ShaderMerger &operator=(const ShaderMerger &) = delete;

  llvm::FunctionType *generateLsHsEntryPointType(uint64_t *inRegMask) const;
  llvm::FunctionType *generateEsGsEntryPointType(uint64_t *inRegMask) const;

  void appendUserData(BuilderBase &builder, llvm::SmallVectorImpl<llvm::Value *> &args, llvm::Function *target,
                      unsigned argIdx, llvm::Value *userData, unsigned userDataCount,
                      llvm::ArrayRef<std::pair<unsigned, unsigned>> substitutions = {});
  void appendVertexFetchTypes(std::vector<llvm::Type *> &argTys) const;
  void appendArguments(llvm::SmallVectorImpl<llvm::Value *> &args, llvm::ArrayRef<llvm::Argument *> argsToAppend) const;

  void gatherTuningAttributes(llvm::AttrBuilder &tuningAttrs, const llvm::Function *srcEntryPoint) const;
  void applyTuningAttributes(llvm::Function *dstEntryPoint, const llvm::AttrBuilder &tuningAttrs) const;

  void processRayQueryLdsStack(llvm::Function *entryPoint1, llvm::Function *entryPoint2) const;

  void storeTessFactorsWithOpt(llvm::Value *threadIdInWave, llvm::IRBuilder<> &builder);
  llvm::Value *readValueFromLds(llvm::Type *readTy, llvm::Value *ldsOffset, llvm::IRBuilder<> &builder);
  void writeValueToLds(llvm::Value *writeValue, llvm::Value *ldsOffset, llvm::IRBuilder<> &builder);
  void createBarrier(llvm::IRBuilder<> &builder);

  PipelineState *m_pipelineState; // Pipeline state
  llvm::LLVMContext *m_context;   // LLVM context
  GfxIpVersion m_gfxIp;           // Graphics IP version info

  bool m_hasVs;  // Whether the pipeline has vertex shader
  bool m_hasTcs; // Whether the pipeline has tessellation control shader
  bool m_hasTes; // Whether the pipeline has tessellation evaluation shader
  bool m_hasGs;  // Whether the pipeline has geometry shader
};

} // namespace lgc
