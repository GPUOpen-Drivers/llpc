/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  NggLdsManager.h
 * @brief LLPC header file: contains declaration of class lgc::NggLdsManager.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/util/Internal.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace lgc {

class PipelineState;

// Enumerates the types of LDS regions used in NGG.
enum NggLdsRegionType {
  // clang-format off
  //
  // LDS region for ES only (no GS)
  //
  LdsRegionDistribPrimId,     // Distributed primitive ID (VS only, for both pass-through and culling modes)
  LdsRegionXfbOutput,         // Transform feedback output (pass-through mode only)
  LdsRegionVertPosData,       // Vertex position data
  LdsRegionVertCullInfo,      // Vertex cull info
  LdsRegionXfbStatInfo,       // Transform feedback statistics info
  LdsRegionVertCountInWaves,  // Vertex count accumulated per wave (8 potential waves) and per subgroup
  LdsRegionVertThreadIdMap,   // Vertex thread ID map (compacted -> uncompacted), for vertex compaction

  LdsRegionEsBeginRange = LdsRegionDistribPrimId,
  LdsRegionEsEndRange = LdsRegionVertThreadIdMap,

  //
  // LDS region for ES-GS
  //
  LdsRegionEsGsRing,            // ES-GS ring
  LdsRegionOutPrimData,         // GS output primitive data
  LdsRegionOutPrimCountInWaves, // GS output primitive count accumulated per wave (8 potential waves) and per subgroup
  LdsRegionOutPrimThreadIdMap,  // GS output primitive thread ID map (compacted -> uncompacted)
  LdsRegionOutVertCountInWaves, // GS output vertex count accumulated per wave (8 potential waves) and per subgroup
  LdsRegionOutVertThreadIdMap,  // GS output vertex thread ID map (compacted -> uncompacted), for vertex compaction
  LdsRegionGsXfbStatInfo,       // GS transform feedback statistics info
  LdsRegionGsVsRing,            // GS-VS ring

  LdsRegionGsBeginRange = LdsRegionEsGsRing,
  LdsRegionGsEndRange = LdsRegionGsVsRing,

  // Total
  LdsRegionCount
  // clang-format on
};

// Size of a dword
static const unsigned SizeOfDword = sizeof(unsigned);

// =====================================================================================================================
// Represents the manager doing shader merge operations.
class NggLdsManager {
public:
  NggLdsManager(llvm::Module *module, PipelineState *pipelineState, llvm::IRBuilder<> *builder);

  static bool needsLds(PipelineState *pipelineState);
  static unsigned calcEsExtraLdsSize(PipelineState *pipelineState);
  static unsigned calcGsExtraLdsSize(PipelineState *pipelineState);

  // Gets the LDS starting offset for the specified region
  unsigned getLdsRegionStart(NggLdsRegionType region) const {
    unsigned regionStart = m_ldsRegionStart[region];
    assert(regionStart != InvalidValue);
    return regionStart;
  }

  llvm::Value *readValueFromLds(llvm::Type *readTy, llvm::Value *ldsOffset, bool useDs128 = false);
  void writeValueToLds(llvm::Value *writeValue, llvm::Value *ldsOffset, bool useDs128 = false);

  void atomicOpWithLds(llvm::AtomicRMWInst::BinOp atomicOp, llvm::Value *atomicValue, llvm::Value *ldsOffset);

private:
  NggLdsManager() = delete;
  NggLdsManager(const NggLdsManager &) = delete;
  NggLdsManager &operator=(const NggLdsManager &) = delete;

  static const unsigned LdsRegionSizes[LdsRegionCount]; // LDS sizes for all LDS region types (in bytes)
  static const char *m_ldsRegionNames[LdsRegionCount];  // Name strings for all LDS region types

  PipelineState *m_pipelineState; // Pipeline state
  llvm::LLVMContext *m_context;   // LLVM context

  llvm::GlobalValue *m_lds; // Global variable to model NGG LDS

  unsigned m_ldsRegionStart[LdsRegionCount]; // Start LDS offsets for all available LDS region types (in bytes)

  llvm::IRBuilder<> *m_builder; // LLVM IR builder
};

} // namespace lgc
