/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcNggLdsManager.h
 * @brief LLPC header file: contains declaration of class lgc::NggLdsManager.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcInternal.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace lgc {

class PipelineState;

// Enumerates the types of LDS regions used in NGG.
enum NggLdsRegionType {
  // LDS region for ES only (no GS)
  LdsRegionDistribPrimId = 0, // Distributed primitive ID (a special region, overlapped with the region of
                              //   position data in NGG non pass-through mode)
  LdsRegionPosData,           // Position data to export
  LdsRegionDrawFlag,          // Draw flag indicating whether the vertex survives
  LdsRegionPrimCountInWaves,  // Primitive count accumulated per wave (8 potential waves) and per sub-group
  LdsRegionVertCountInWaves,  // Vertex count accumulated per wave (8 potential waves) and per sub-group
  LdsRegionCullDistance,      // Aggregated sign value of cull distance (bitmask)
                         // Below regions are for vertex compaction
  LdsRegionVertThreadIdMap,   // Vertex thread ID map (uncompacted -> compacted)
  LdsRegionCompactVertexId,   // Vertex ID (VS only)
  LdsRegionCompactInstanceId, // Instance ID (VS only)
  LdsRegionCompactPrimId,     // Primitive ID (VS only)
  LdsRegionCompactTessCoordX, // X of tessCoord (U) (TES only)
  LdsRegionCompactTessCoordY, // Y of tessCoord (V) (TES only)
  LdsRegionCompactPatchId,    // Patch ID (TES only)
  LdsRegionCompactRelPatchId, // Relative patch ID (TES only)

  LdsRegionCompactBeginRange = LdsRegionVertThreadIdMap,
  LdsRegionCompactEndRange = LdsRegionCompactRelPatchId,

  LdsRegionEsBeginRange = LdsRegionDistribPrimId,
  LdsRegionEsEndRange = LdsRegionCompactRelPatchId,

  // LDS region for ES-GS
  LdsRegionEsGsRing,            // ES-GS ring
  LdsRegionOutPrimData,         // GS output primitive data
  LdsRegionOutVertCountInWaves, // GS output vertex count accumulated per wave (8 potential waves) and per
                                //   sub-group for each stream (4 GS streams)
  LdsRegionOutVertOffset,       // GS output vertex (exported vertex data) offset in GS-VS ring
                                //   (overlapped with the region of exported primitive data, LDS reused)
  LdsRegionGsVsRing,            // GS-VS ring

  LdsRegionGsBeginRange = LdsRegionEsGsRing,
  LdsRegionGsEndRange = LdsRegionGsVsRing,

  // Total
  LdsRegionCount
};

// Size of a DWORD
static const unsigned SizeOfDword = sizeof(unsigned);

// =====================================================================================================================
// Represents the manager doing shader merge operations.
class NggLdsManager {
public:
  NggLdsManager(llvm::Module *module, PipelineState *pipelineState, llvm::IRBuilder<> *builder);

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

  // -----------------------------------------------------------------------------------------------------------------

  static const unsigned LdsRegionSizes[LdsRegionCount]; // LDS sizes for all LDS region types (in BYTEs)
  static const char *m_ldsRegionNames[LdsRegionCount];  // Name strings for all LDS region types

  PipelineState *m_pipelineState; // Pipeline state
  llvm::LLVMContext *m_context;   // LLVM context

  llvm::GlobalValue *m_lds; // Global variable to model NGG LDS

  unsigned m_ldsRegionStart[LdsRegionCount]; // Start LDS offsets for all available LDS region types (in BYTEs)

  unsigned m_waveCountInSubgroup; // Wave count in sub-group

  llvm::IRBuilder<> *m_builder; // LLVM IR builder
};

} // namespace lgc
