/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/IR/Module.h"

namespace lgc {

class PipelineState;

// Enumerates special system values for the LS-HS merged shader (the assigned numeric values are identical to SGPR
// numbers defined by hardware).
enum LsHsSpecialSysValue {
  LsHsSysValueUserDataAddrLow = 0,
  LsHsSysValueUserDataAddrHigh = 1,
  LsHsSysValueOffChipLdsBase = 2,
  LsHsSysValueMergedWaveInfo = 3,
  LsHsSysValueTfBufferBase = 4,
  LsHsSysValueSharedScratchOffset = 5,
  LsHsSysValueLsShaderAddrLow = 6,
  LsHsSysValueLsShaderAddrHigh = 7,

  LsHsSpecialSysValueCount,
};

// Enumerates special system values for the ES-GS merged shader (the assigned numeric values are identical to SGPR
// numbers defined by hardware).
enum EsGsSpecialSysValue {
  EsGsSysValueUserDataAddrLow = 0,
  EsGsSysValueUserDataAddrHigh = 1,
  EsGsSysValueGsVsOffset = 2,
  EsGsSysValueMergedGroupInfo = 2,
  EsGsSysValueMergedWaveInfo = 3,
  EsGsSysValueOffChipLdsBase = 4,
  EsGsSysValueSharedScratchOffset = 5,
  EsGsSysValueGsShaderAddrLow = 6,
  EsGsSysValueGsShaderAddrHigh = 7,
  EsGsSysValuePrimShaderTableAddrLow = 6,
  EsGsSysValuePrimShaderTableAddrHigh = 7,

  EsGsSpecialSysValueCount,
};

// =====================================================================================================================
// Represents the manager doing shader merge operations.
class ShaderMerger {
public:
  ShaderMerger(PipelineState *pipelineState, PipelineShaders *pipelineShaders);

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

  void appendVertexFetchTypes(std::vector<llvm::Type *> &argTys) const;
  void appendArguments(std::vector<llvm::Value *> &args, llvm::Argument *begin, llvm::Argument *end) const;

  PipelineState *m_pipelineState; // Pipeline state
  llvm::LLVMContext *m_context;   // LLVM context
  GfxIpVersion m_gfxIp;           // Graphics IP version info

  bool m_hasVs;  // Whether the pipeline has vertex shader
  bool m_hasTcs; // Whether the pipeline has tessellation control shader
  bool m_hasTes; // Whether the pipeline has tessellation evaluation shader
  bool m_hasGs;  // Whether the pipeline has geometry shader
};

} // namespace lgc
