/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcNggLdsManager.cpp
 * @brief LLPC source file: contains implementation of class lgc::NggLdsManager.
 ***********************************************************************************************************************
 */
#include "llpcNggLdsManager.h"
#include "llpcBuilderDebug.h"
#include "llpcGfx9Chip.h"
#include "llpcPatch.h"
#include "llpcPipelineState.h"
#include "llpcTargetInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "llpc-ngg-lds-manager"

using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Initialize static members
const unsigned NggLdsManager::LdsRegionSizes[LdsRegionCount] = {
    // LDS region size for ES-only

    // 1 DWORD (unsigned) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionDistribPrimId
                                                  // 4 DWORDs (vec4) per thread
    SizeOfVec4 *Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionPosData
                                                // 1 BYTE (uint8_t) per thread
    Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionDrawFlag
                                    // 1 DWORD per wave (8 potential waves) + 1 DWORD for the entire sub-group
    SizeOfDword *Gfx9::NggMaxWavesPerSubgroup +
        SizeOfDword, // LdsRegionPrimCountInWaves
                     // 1 DWORD per wave (8 potential waves) + 1 DWORD for the entire sub-group
    SizeOfDword *Gfx9::NggMaxWavesPerSubgroup + SizeOfDword, // LdsRegionVertCountInWaves
                                                             // 1 DWORD (unsigned) per thread
    SizeOfDword *Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionCullDistance
                                                 // 1 BYTE (uint8_t) per thread
    Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionVertThreadIdMap
                                    // 1 DWORD (unsigned) per thread
    SizeOfDword *Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionCompactVertexId
                                                 // 1 DWORD (unsigned) per thread
    SizeOfDword *Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionCompactInstanceId
                                                 // 1 DWORD (unsigned) per thread
    SizeOfDword *Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionCompactPrimId
                                                 // 1 DWORD (unsigned) per thread
    SizeOfDword *Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionCompactTessCoordX
                                                 // 1 DWORD (unsigned) per thread
    SizeOfDword *Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionCompactTessCoordY
                                                 // 1 DWORD (unsigned) per thread
    SizeOfDword *Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionCompactPatchId
                                                 // 1 DWORD (unsigned) per thread
    SizeOfDword *Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionCompactRelPatchId

    // LDS region size for ES-GS

    // ES-GS ring size is dynamically calculated (don't use it)
    InvalidValue, // LdsRegionEsGsRing
                  // 1 DWORD (unsigned) per thread
    SizeOfDword *Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionOutPrimData
                                                 // 1 DWORD per wave (8 potential waves) + 1 DWORD for the entire
                                                 // sub-group (4 GS streams)
    MaxGsStreams *(SizeOfDword *Gfx9::NggMaxWavesPerSubgroup + SizeOfDword), // LdsRegionOutVertCountInWaves
                                                                             // 1 DWORD (unsigned) per thread
    SizeOfDword *Gfx9::NggMaxThreadsPerSubgroup, // LdsRegionOutVertOffset
                                                 // GS-VS ring size is dynamically calculated (don't use it)
    InvalidValue, // LdsRegionGsVsRing
};

// =====================================================================================================================
// Initialize static members
const char *NggLdsManager::m_ldsRegionNames[LdsRegionCount] = {
    // LDS region name for ES-only
    "Distributed primitive ID",          // LdsRegionDistribPrimId
    "Vertex position data",              // LdsRegionPosData
    "Draw flag",                         // LdsRegionDrawFlag
    "Primitive count in waves",          // LdsRegionPrimCountInWaves
    "Vertex count in waves",             // LdsRegionVertCountInWaves
    "Cull distance",                     // LdsRegionCullDistance
    "Vertex thread ID map",              // LdsRegionVertThreadIdMap
    "Compacted vertex ID (VS)",          // LdsRegionCompactVertexId
    "Compacted instance ID (VS)",        // LdsRegionCompactInstanceId
    "Compacted primitive ID (VS)",       // LdsRegionCompactPrimId
    "Compacted tesscoord X (TES)",       // LdsRegionCompactTessCoordX
    "Compacted tesscoord Y (TES)",       // LdsRegionCompactTessCoordY
    "Compacted patch ID (TES)",          // LdsRegionCompactPatchId
    "Compacted relative patch ID (TES)", // LdsRegionCompactRelPatchId

    // LDS region name for ES-GS
    "ES-GS ring",                   // LdsRegionEsGsRing
    "GS out primitive data",        // LdsRegionOutPrimData
    "GS out vertex count in waves", // LdsRegionOutVertCountInWaves
    "GS out vertex offset",         // LdsRegionOutVertOffset
    "GS-VS ring",                   // LdsRegionGsVsRing
};

// =====================================================================================================================
//
// @param module : LLVM module
// @param pipelineState : Pipeline state
// @param builder : LLVM IR builder
NggLdsManager::NggLdsManager(Module *module, PipelineState *pipelineState, IRBuilder<> *builder)
    : m_pipelineState(pipelineState), m_context(&pipelineState->getContext()),
      m_waveCountInSubgroup(Gfx9::NggMaxThreadsPerSubgroup /
                            m_pipelineState->getTargetInfo().getGpuProperty().waveSize),
      m_builder(builder) {
  assert(builder);

  const auto nggControl = m_pipelineState->getNggControl();
  assert(nggControl->enableNgg);

  const unsigned stageMask = m_pipelineState->getShaderStageMask();
  const bool hasGs = (stageMask & shaderStageToMask(ShaderStageGeometry));
  const bool hasTs =
      ((stageMask & (shaderStageToMask(ShaderStageTessControl) | shaderStageToMask(ShaderStageTessEval))) != 0);

  //
  // Create global variable modeling LDS
  //
  m_lds = Patch::getLdsVariable(m_pipelineState, module);

  memset(&m_ldsRegionStart, InvalidValue, sizeof(m_ldsRegionStart)); // Initialized to invalid value (0xFFFFFFFF)

  //
  // Calculate start LDS offset for all available LDS region types
  //

  LLPC_OUTS("===============================================================================\n");
  LLPC_OUTS("// LLPC NGG LDS region info (in bytes)\n\n");

  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;

  if (hasGs) {
    //
    // The LDS layout is something like this:
    //
    // +------------+-----------------------+--------------------------------+------------+
    // | ES-GS ring | GS out primitive data | GS out vertex count (in waves) | GS-VS ring |
    // +------------+-----------------------+--------------------------------+------------+
    //              | GS out vertex  offset |
    //              +-----------------------+
    //

    // NOTE: We round ES-GS LDS size to 4-DWORD alignment. This is for later LDS read/write operations of mutilple
    // DWORDs (such as DS128).
    const unsigned esGsRingLdsSize = alignTo(calcFactor.esGsLdsSize, 4u) * SizeOfDword;
    const unsigned gsVsRingLdsSize =
        calcFactor.gsOnChipLdsSize * SizeOfDword - esGsRingLdsSize - calcGsExtraLdsSize(m_pipelineState);

    unsigned ldsRegionStart = 0;

    for (unsigned region = LdsRegionGsBeginRange; region <= LdsRegionGsEndRange; ++region) {
      unsigned ldsRegionSize = LdsRegionSizes[region];

      if (region == LdsRegionOutVertOffset) {
        // An overlapped region, reused
        m_ldsRegionStart[LdsRegionOutVertOffset] = m_ldsRegionStart[LdsRegionOutPrimData];

        LLPC_OUTS(format("%-40s : offset = 0x%04" PRIX32 ", size = 0x%04" PRIX32, m_ldsRegionNames[region],
                         m_ldsRegionStart[region], ldsRegionSize)
                  << "\n");

        continue;
      }

      // NOTE: LDS size of ES-GS ring is calculated (by rounding it up to 16-byte alignment)
      if (region == LdsRegionEsGsRing)
        ldsRegionSize = esGsRingLdsSize;

      // NOTE: LDS size of ES-GS ring is calculated
      if (region == LdsRegionGsVsRing)
        ldsRegionSize = gsVsRingLdsSize;

      m_ldsRegionStart[region] = ldsRegionStart;
      assert(ldsRegionSize != InvalidValue);
      ldsRegionStart += ldsRegionSize;

      LLPC_OUTS(format("%-40s : offset = 0x%04" PRIX32 ", size = 0x%04" PRIX32, m_ldsRegionNames[region],
                       m_ldsRegionStart[region], ldsRegionSize)
                << "\n");
    }
  } else {
    m_ldsRegionStart[LdsRegionDistribPrimId] = 0;

    LLPC_OUTS(format("%-40s : offset = 0x%04" PRIX32 ", size = 0x%04" PRIX32, m_ldsRegionNames[LdsRegionDistribPrimId],
                     m_ldsRegionStart[LdsRegionDistribPrimId], LdsRegionSizes[LdsRegionDistribPrimId])
              << "\n");

    if (!nggControl->passthroughMode) {
      //
      // The LDS layout is something like this:
      //
      // +--------------------------+-----------+----------------------------+---------------+
      // | Vertex position data     | Draw flag | Vertex count (in waves)    | Cull distance | >>>
      // +--------------------------+-----------+----------------------------+---------------+
      // | Distributed primitive ID |           | Primitive count (in waves) |
      // +--------------------------+           +----------------------------+
      //
      //                            | ====== Compacted data region (for vertex compaction) ====== |
      //     +----------------------+-------------+-------------+-------------+
      // >>> | Vertex thread ID map | Vertex ID   | Instance ID | Primtive ID |                     (VS)
      //     +----------------------+-------------+-------------+-------------+-------------------+
      //                            | Tesscoord X | Tesscoord Y | Patch ID    | Relative patch ID | (TES)
      //                            +-------------+-------------+-------------+-------------------+
      //
      unsigned ldsRegionStart = 0;
      for (unsigned region = LdsRegionEsBeginRange; region <= LdsRegionEsEndRange; ++region) {
        // NOTE: For NGG non pass-through mode, primitive ID region is overlapped with position data.
        if (region == LdsRegionDistribPrimId)
          continue;

        // NOTE: If cull distance culling is disabled, skip this region
        if (region == LdsRegionCullDistance && !nggControl->enableCullDistanceCulling)
          continue;

        // NOTE: If NGG compaction is based on sub-group, those regions that are for vertex compaction should be
        // skipped.
        if (nggControl->compactMode == NggCompactSubgroup &&
            (region >= LdsRegionCompactBeginRange && region <= LdsRegionCompactEndRange))
          continue;

        if (hasTs) {
          // Skip those regions that are for VS only
          if (region == LdsRegionCompactVertexId || region == LdsRegionCompactInstanceId ||
              region == LdsRegionCompactPrimId)
            continue;
        } else {
          // Skip those regions that are for TES only
          if (region == LdsRegionCompactTessCoordX || region == LdsRegionCompactTessCoordY ||
              region == LdsRegionCompactRelPatchId || region == LdsRegionCompactPatchId)
            continue;
        }

        m_ldsRegionStart[region] = ldsRegionStart;
        ldsRegionStart += LdsRegionSizes[region];

        LLPC_OUTS(format("%-40s : offset = 0x%04" PRIX32 ", size = 0x%04" PRIX32, m_ldsRegionNames[region],
                         m_ldsRegionStart[region], LdsRegionSizes[region])
                  << "\n");
      }
    }
  }

  LLPC_OUTS(format("%-40s :                  size = 0x%04" PRIX32, static_cast<const char *>("LDS total"),
                   calcFactor.gsOnChipLdsSize * SizeOfDword)
            << "\n\n");
}

// =====================================================================================================================
// Calculates ES extra LDS size.
//
// @param pipelineState : Pipeline state
unsigned NggLdsManager::calcEsExtraLdsSize(PipelineState *pipelineState) {
  const auto nggControl = pipelineState->getNggControl();
  if (!nggControl->enableNgg)
    return 0;

  const unsigned stageMask = pipelineState->getShaderStageMask();
  const bool hasGs = ((stageMask & shaderStageToMask(ShaderStageGeometry)) != 0);

  if (hasGs) {
    // NOTE: Not need ES extra LDS when GS is present.
    return 0;
  }

  const bool hasTs =
      ((stageMask & (shaderStageToMask(ShaderStageTessControl) | shaderStageToMask(ShaderStageTessEval))) != 0);

  unsigned esExtraLdsSize = 0;

  if (nggControl->passthroughMode) {
    // NOTE: For NGG pass-through mode, only primitive ID region is valid.
    bool distributePrimId = false;
    if (!hasTs) {
      const auto &builtInUsage = pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
      distributePrimId = builtInUsage.primitiveId;
    }

    esExtraLdsSize = distributePrimId ? LdsRegionSizes[LdsRegionDistribPrimId] : 0;
  } else {
    for (unsigned region = LdsRegionEsBeginRange; region <= LdsRegionEsEndRange; ++region) {
      // NOTE: For NGG non pass-through mode, primitive ID region is overlapped with position data.
      if (region == LdsRegionDistribPrimId)
        continue;

      // NOTE: If cull distance culling is disabled, skip this region
      if (region == LdsRegionCullDistance && !nggControl->enableCullDistanceCulling)
        continue;

      // NOTE: If NGG compaction is based on sub-group, those regions that are for vertex compaction should be
      // skipped.
      if (nggControl->compactMode == NggCompactSubgroup &&
          (region >= LdsRegionCompactBeginRange && region <= LdsRegionCompactEndRange))
        continue;

      if (hasTs) {
        // Skip those regions that are for VS only
        if (region == LdsRegionCompactVertexId || region == LdsRegionCompactInstanceId ||
            region == LdsRegionCompactPrimId)
          continue;
      } else {
        // Skip those regions that are for TES only
        if (region == LdsRegionCompactTessCoordX || region == LdsRegionCompactTessCoordY ||
            region == LdsRegionCompactRelPatchId || region == LdsRegionCompactPatchId)
          continue;
      }

      esExtraLdsSize += LdsRegionSizes[region];
    }
  }

  return esExtraLdsSize;
}

// =====================================================================================================================
// Calculates GS extra LDS size (used for operations other than ES-GS ring and GS-VS ring read/write).
//
// @param pipelineState : Pipeline state
unsigned NggLdsManager::calcGsExtraLdsSize(PipelineState *pipelineState) {
  const auto nggControl = pipelineState->getNggControl();
  if (!nggControl->enableNgg)
    return 0;

  const unsigned stageMask = pipelineState->getShaderStageMask();
  const bool hasGs = ((stageMask & shaderStageToMask(ShaderStageGeometry)) != 0);
  if (!hasGs) {
    // NOTE: Not need GS extra LDS when GS is not present.
    return 0;
  }

  unsigned gsExtraLdsSize = LdsRegionSizes[LdsRegionOutPrimData] + LdsRegionSizes[LdsRegionOutVertCountInWaves];

  return gsExtraLdsSize;
}

// =====================================================================================================================
// Reads value from LDS.
//
// @param readTy : Type of value read from LDS
// @param ldsOffset : Start offset to do LDS read operations
// @param useDs128 : Whether to use 128-bit LDS load, 16-byte alignment is guaranteed by caller
Value *NggLdsManager::readValueFromLds(Type *readTy, Value *ldsOffset, bool useDs128) {
  assert(m_lds);
  assert(readTy->isIntOrIntVectorTy() || readTy->isFPOrFPVectorTy());

  const unsigned readBits = readTy->getPrimitiveSizeInBits();

  unsigned bitWidth = 0;
  unsigned compCount = 0;
  unsigned alignment = 4;

  if (readBits % 128 == 0) {
    bitWidth = 128;
    compCount = readBits / 128;

    if (useDs128)
      alignment = 16; // Set alignment to 16-byte to use 128-bit LDS load
  } else if (readBits % 64 == 0) {
    bitWidth = 64;
    compCount = readBits / 64;
  } else if (readBits % 32 == 0) {
    bitWidth = 32;
    compCount = readBits / 32;
  } else if (readBits % 16 == 0) {
    bitWidth = 16;
    compCount = readBits / 16;
  } else {
    assert(readBits % 8 == 0);
    bitWidth = 8;
    compCount = readBits / 8;
  }

  Type *compTy = m_builder->getIntNTy(bitWidth);
  Value *readValue = UndefValue::get(compCount > 1 ? VectorType::get(compTy, compCount) : compTy);

  // NOTE: LDS variable is defined as a pointer to i32 array. We cast it to a pointer to i8 array first.
  auto lds = ConstantExpr::getBitCast(
      m_lds, PointerType::get(Type::getInt8Ty(*m_context), m_lds->getType()->getPointerAddressSpace()));

  for (unsigned i = 0; i < compCount; ++i) {
    Value *loadPtr = m_builder->CreateGEP(lds, ldsOffset);
    if (bitWidth != 8)
      loadPtr = m_builder->CreateBitCast(loadPtr, PointerType::get(compTy, ADDR_SPACE_LOCAL));

    Value *loadValue = m_builder->CreateAlignedLoad(loadPtr, MaybeAlign(alignment));

    if (compCount > 1)
      readValue = m_builder->CreateInsertElement(readValue, loadValue, i);
    else
      readValue = loadValue;

    if (compCount > 1)
      ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(bitWidth / 8));
  }

  if (readValue->getType() != readTy)
    readValue = m_builder->CreateBitCast(readValue, readTy);

  return readValue;
}

// =====================================================================================================================
// Writes value to LDS.
//
// @param writeValue : Value written to LDS
// @param ldsOffset : Start offset to do LDS write operations
// @param useDs128 : Whether to use 128-bit LDS store, 16-byte alignment is guaranteed by caller
void NggLdsManager::writeValueToLds(Value *writeValue, Value *ldsOffset, bool useDs128) {
  assert(m_lds);

  auto writeTy = writeValue->getType();
  assert(writeTy->isIntOrIntVectorTy() || writeTy->isFPOrFPVectorTy());

  const unsigned writeBits = writeTy->getPrimitiveSizeInBits();

  unsigned bitWidth = 0;
  unsigned compCount = 0;
  unsigned alignment = 4;

  if (writeBits % 128 == 0) {
    bitWidth = 128;
    compCount = writeBits / 128;

    if (useDs128)
      alignment = 16; // Set alignment to 16-byte to use 128-bit LDS store
  } else if (writeBits % 64 == 0) {
    bitWidth = 64;
    compCount = writeBits / 64;
  } else if (writeBits % 32 == 0) {
    bitWidth = 32;
    compCount = writeBits / 32;
  } else if (writeBits % 16 == 0) {
    bitWidth = 16;
    compCount = writeBits / 16;
  } else {
    assert(writeBits % 8 == 0);
    bitWidth = 8;
    compCount = writeBits / 8;
  }

  Type *compTy = m_builder->getIntNTy(bitWidth);
  writeTy = compCount > 1 ? VectorType::get(compTy, compCount) : compTy;

  if (writeValue->getType() != writeTy)
    writeValue = m_builder->CreateBitCast(writeValue, writeTy);

  // NOTE: LDS variable is defined as a pointer to i32 array. We cast it to a pointer to i8 array first.
  auto lds = ConstantExpr::getBitCast(
      m_lds, PointerType::get(Type::getInt8Ty(*m_context), m_lds->getType()->getPointerAddressSpace()));

  for (unsigned i = 0; i < compCount; ++i) {
    Value *storePtr = m_builder->CreateGEP(lds, ldsOffset);
    if (bitWidth != 8)
      storePtr = m_builder->CreateBitCast(storePtr, PointerType::get(compTy, ADDR_SPACE_LOCAL));

    Value *storeValue = nullptr;
    if (compCount > 1)
      storeValue = m_builder->CreateExtractElement(writeValue, i);
    else
      storeValue = writeValue;

    m_builder->CreateAlignedStore(storeValue, storePtr, MaybeAlign(alignment));

    if (compCount > 1)
      ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(bitWidth / 8));
  }
}

// =====================================================================================================================
// Does atomic binary operation with the value stored in LDS.
//
// @param atomicOp : Atomic binary operation
// @param atomicValue : Value to do atomic operation
// @param ldsOffset : Start offset to do LDS atomic operations
void NggLdsManager::atomicOpWithLds(AtomicRMWInst::BinOp atomicOp, Value *atomicValue, Value *ldsOffset) {
  assert(atomicValue->getType()->isIntegerTy(32));

  // NOTE: LDS variable is defined as a pointer to i32 array. The LDS offset here has to be casted to DWORD offset
  // from BYTE offset.
  ldsOffset = m_builder->CreateLShr(ldsOffset, 2);

  Value *atomicPtr = m_builder->CreateGEP(m_lds, {m_builder->getInt32(0), ldsOffset});

  auto atomicInst = m_builder->CreateAtomicRMW(atomicOp, atomicPtr, atomicValue, AtomicOrdering::SequentiallyConsistent,
                                               SyncScope::System);
  atomicInst->setVolatile(true);
}

} // namespace lgc
