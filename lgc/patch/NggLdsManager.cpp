/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  NggLdsManager.cpp
 * @brief LLPC source file: contains implementation of class lgc::NggLdsManager.
 ***********************************************************************************************************************
 */
#include "NggLdsManager.h"
#include "Gfx9Chip.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Debug.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "lgc-ngg-lds-manager"

using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Initialize static members
const unsigned NggLdsManager::LdsRegionSizes[LdsRegionCount] = {
    // clang-format off
    //
    // LDS region size for ES-only
    //
    // Distributed primitive ID size is dynamically calculated (don't use it)
    InvalidValue,                                             // LdsRegionDistribPrimId
    // Transform feedback output size is dynamically calculated (don't use it)
    InvalidValue,                                             // LdsRegionXfbOutput
    // 4 dwords (vec4) per thread
    SizeOfVec4 * Gfx9::NggMaxThreadsPerSubgroup,              // LdsRegionVertPosData
    // Vertex cull info size is dynamically calculated (don't use it)
    InvalidValue,                                             // LdsRegionVertCullInfo
    // 1 dword per XFB buffer: dword written, 1 dword: primitives to write
    SizeOfDword * MaxTransformFeedbackBuffers + SizeOfDword,  // LdsRegionXfbStatInfo
    // 1 dword per wave (8 potential waves) + 1 dword for the entire sub-group
    SizeOfDword * Gfx9::NggMaxWavesPerSubgroup + SizeOfDword, // LdsRegionVertCountInWaves
    // 1 dword (uint32) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup,             // LdsRegionVertThreadIdMap

    //
    // LDS region size for ES-GS
    //
    // ES-GS ring size is dynamically calculated (don't use it)
    InvalidValue,                                             // LdsRegionEsGsRing
    // 1 dword (uint32) per thread, 4 GS streams
    (SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup) * MaxGsStreams,
                                                              // LdsRegionOutPrimData
    // 1 dword per wave (8 potential waves) + 1 dword for the entire sub-group, 4 GS streams
    (SizeOfDword * Gfx9::NggMaxWavesPerSubgroup + SizeOfDword) * MaxGsStreams,
                                                              // LdsRegionOutPrimCountInWaves
    // 1 dword (uint32) per thread, 4 GS streams
    (SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup) * MaxGsStreams,
                                                              // LdsRegionOutPrimThreadIdMap
    // 1 dword per wave (8 potential waves) + 1 dword for the entire sub-group, 4 GS streams
    (SizeOfDword * Gfx9::NggMaxWavesPerSubgroup + SizeOfDword) * MaxGsStreams,
                                                              // LdsRegionOutVertCountInWaves
    // 1 dword (uint32) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup,             // LdsRegionOutVertThreadIdMap
    // 1 dword per XFB buffer: dword written, 1 dword per vertex stream: primitives to write
    SizeOfDword * MaxTransformFeedbackBuffers + SizeOfDword * MaxGsStreams,
                                                              // LdsRegionGsXfbStatInfo
    // GS-VS ring size is dynamically calculated (don't use it)
    InvalidValue,                                             // LdsRegionGsVsRing
    // clang-format on
};

// =====================================================================================================================
// Initialize static members
const char *NggLdsManager::m_ldsRegionNames[LdsRegionCount] = {
    // clang-format off
    //
    // LDS region name for ES-only
    //
    "Distributed primitive ID",             // LdsRegionDistribPrimId
    "Transform feedback output",            // LdsRegionXfbOutput
    "Vertex position data",                 // LdsRegionVertPosData
    "Vertex cull info",                     // LdsRegionVertCullInfo
    "Transform feedback statistics",        // LdsRegionXfbStatInfo
    "Vertex count in waves",                // LdsRegionVertCountInWaves
    "Vertex thread ID map",                 // LdsRegionVertThreadIdMap

    //
    // LDS region name for ES-GS
    //
    "ES-GS ring",                           // LdsRegionEsGsRing
    "GS out primitive data",                // LdsRegionOutPrimData
    "GS out primitive count in waves",      // LdsRegionOutPrimCountInWaves
    "GS out primitive thread ID map",       // LdsRegionOutPrimThreadIdMap
    "GS out vertex count in waves",         // LdsRegionOutVertCountInWaves
    "GS out vertex thread ID map",          // LdsRegionOutVertThreadIdMap
    "GS transform feedback statistics",     // LdsRegionGsXfbStatInfo
    "GS-VS ring",                           // LdsRegionGsVsRing
    // clang-format on
};

// =====================================================================================================================
//
// @param module : LLVM module
// @param pipelineState : Pipeline state
// @param builder : LLVM IR builder
NggLdsManager::NggLdsManager(Module *module, PipelineState *pipelineState, IRBuilder<> *builder)
    : m_pipelineState(pipelineState), m_context(&pipelineState->getContext()), m_builder(builder) {
  assert(builder);

  const auto nggControl = m_pipelineState->getNggControl();
  assert(nggControl->enableNgg);

  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;

  const bool hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);

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

  if (hasGs) {
    //
    // The LDS layout is something like this:
    //
    // +------------+-----------------------+---------------------------------+--------------------------------+
    // | ES-GS ring | GS out primitive data | GS out vertex counts (waves)    | GS out vertex thread ID map    | >>>
    // +------------+-----------------------+---------------------------------+--------------------------------+
    //                                      | GS out primitive counts (waves) | GS out primitive thread ID map |
    //                                      +---------------------------------+--------------------------------+
    //       +-------------------+------------+
    //   >>> | GS XFB statistics | GS-VS ring |
    //       +-------------------+------------+
    //

    // NOTE: We round ES-GS LDS size to 4-dword alignment. This is for later LDS read/write operations of mutilple
    // dwords (such as DS128).
    const unsigned esGsRingLdsSize = alignTo(calcFactor.esGsLdsSize, 4u) * SizeOfDword;
    const unsigned gsVsRingLdsSize =
        calcFactor.gsOnChipLdsSize * SizeOfDword - esGsRingLdsSize - calcGsExtraLdsSize(m_pipelineState);

    unsigned ldsRegionStart = 0;

    for (unsigned region = LdsRegionGsBeginRange; region <= LdsRegionGsEndRange; ++region) {
      // NOTE: For vertex compactionless mode, this region is unnecessary
      if (region == LdsRegionOutVertThreadIdMap && nggControl->compactMode == NggCompactDisable)
        continue;

      if (m_pipelineState->enableSwXfb()) {
        if (region == LdsRegionOutVertCountInWaves || region == LdsRegionOutVertThreadIdMap) {
          // NOTE: The two regions are overlapped with OutPrimCountInWaves and OutPrimThreadIdMap when SW-emulated
          // stream-out is enabled.
          auto overlappedRegion =
              region == LdsRegionOutVertCountInWaves ? LdsRegionOutPrimCountInWaves : LdsRegionOutPrimThreadIdMap;

          assert(m_ldsRegionStart[overlappedRegion] != InvalidValue); // Must be valid
          m_ldsRegionStart[region] = m_ldsRegionStart[overlappedRegion];
          unsigned ldsRegionSize = LdsRegionSizes[overlappedRegion];

          LLPC_OUTS(format("%-40s : offset = 0x%04" PRIX32 ", size = 0x%04" PRIX32, m_ldsRegionNames[region],
                           m_ldsRegionStart[region], ldsRegionSize)
                    << "\n");
          continue;
        }
      } else {
        // NOTE: Such regions are for SW-emulated stream-out. Skip handling them if SW-emulated stream-out is disabled.
        if (region == LdsRegionOutPrimCountInWaves || region == LdsRegionOutPrimThreadIdMap ||
            region == LdsRegionGsXfbStatInfo)
          continue;
      }

      unsigned ldsRegionSize = LdsRegionSizes[region];

      // NOTE: LDS size of ES-GS ring is calculated (by rounding it up to 16-byte alignment)
      if (region == LdsRegionEsGsRing)
        ldsRegionSize = esGsRingLdsSize;

      // NOTE: LDS size of GS-VS ring is calculated
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
                     m_ldsRegionStart[LdsRegionDistribPrimId], calcFactor.esVertsPerSubgroup * SizeOfDword)
              << "\n");

    if (!nggControl->passthroughMode) {
      //
      // The LDS layout is something like this:
      //
      // +--------------------------+
      // | Distributed primitive ID |
      // +--------------------------+
      //
      // +----------------------+-------------------------------+----------------+-------------------------+
      // | Vertex position data | Vertex cull info (ES-GS ring) | XFB statistics | Vertex count (in waves) | >>>
      // +----------------------+-------------------------------+----------------+-------------------------+
      //       +----------------------+
      //   >>> | Vertex thread ID map |
      //       +----------------------+
      //
      unsigned ldsRegionStart = 0;
      for (unsigned region = LdsRegionEsBeginRange; region <= LdsRegionEsEndRange; ++region) {
        // NOTE: For NGG culling mode, distributed primitive ID region is partially overlapped with vertex cull info
        // region.
        if (region == LdsRegionDistribPrimId)
          continue;

        // NOTE: For NGG culling mode, transform feedback output region is stored as part of vertex cull info. There
        // is no dedicated region.
        if (region == LdsRegionXfbOutput)
          continue;

        // NOTE: For transform feedback statistics info region, if SW-emuated stream-out is disabled, it is unused.
        if (region == LdsRegionXfbStatInfo && !m_pipelineState->enableSwXfb())
          continue;

        // NOTE: For vertex compactionless mode, this region is unnecessary
        if (region == LdsRegionVertThreadIdMap && nggControl->compactMode == NggCompactDisable)
          continue;

        unsigned ldsRegionSize = LdsRegionSizes[region];

        // NOTE: LDS size of vertex cull info (ES-GS ring) is calculated
        if (region == LdsRegionVertCullInfo)
          ldsRegionSize = calcFactor.esGsRingItemSize * calcFactor.esVertsPerSubgroup * SizeOfDword;

        m_ldsRegionStart[region] = ldsRegionStart;
        assert(ldsRegionSize != InvalidValue);
        ldsRegionStart += ldsRegionSize;

        LLPC_OUTS(format("%-40s : offset = 0x%04" PRIX32 ", size = 0x%04" PRIX32, m_ldsRegionNames[region],
                         m_ldsRegionStart[region], ldsRegionSize)
                  << "\n");
      }
    } else {
      //
      // The LDS layout is something like this:
      //
      // +--------------------------+
      // | Distributed primitive ID |
      // +--------------------------+
      //
      // +--------------------------------+---------------------+
      // | XFB output ([n x <4 x dword>]) | XFB statistics info |
      // +--------------------------------+---------------------+
      //
      if (m_pipelineState->enableSwXfb()) {
        // NOTE: For GFX11+, transform feedback outputs are stored as a ES-GS ring item.
        m_ldsRegionStart[LdsRegionXfbOutput] = 0;
        const unsigned esGsRingLdsSize = calcFactor.esVertsPerSubgroup * calcFactor.esGsRingItemSize * SizeOfDword;
        LLPC_OUTS(format("%-40s : offset = 0x%04" PRIX32 ", size = 0x%04" PRIX32, m_ldsRegionNames[LdsRegionXfbOutput],
                         m_ldsRegionStart[LdsRegionXfbOutput], esGsRingLdsSize)
                  << "\n");

        m_ldsRegionStart[LdsRegionXfbStatInfo] = esGsRingLdsSize;
        LLPC_OUTS(format("%-40s : offset = 0x%04" PRIX32 ", size = 0x%04" PRIX32,
                         m_ldsRegionNames[LdsRegionXfbStatInfo], m_ldsRegionStart[LdsRegionXfbStatInfo],
                         LdsRegionSizes[LdsRegionXfbStatInfo])
                  << "\n");
      }
    }
  }

  LLPC_OUTS(format("%-40s :                  size = 0x%04" PRIX32, static_cast<const char *>("LDS total"),
                   calcFactor.gsOnChipLdsSize * SizeOfDword)
            << "\n\n");
}

// =====================================================================================================================
// Checks if this NGG pipeline needs LDS space.
//
// @param pipelineState : Pipeline state
bool NggLdsManager::needsLds(PipelineState *pipelineState) {
  const auto nggControl = pipelineState->getNggControl();
  if (!nggControl->enableNgg)
    return false;

  // NGG GS always needs LDS
  const bool hasGs = pipelineState->hasShaderStage(ShaderStageGeometry);
  if (hasGs)
    return true;

  // NGG culling mode always needs LDS
  if (!nggControl->passthroughMode)
    return true;

  // To distribute primitive ID, we need LDS
  const bool hasTs =
      pipelineState->hasShaderStage(ShaderStageTessControl) || pipelineState->hasShaderStage(ShaderStageTessEval);
  if (!hasTs) {
    const bool distribPrimitiveId =
        pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs.primitiveId;
    if (distribPrimitiveId)
      return true;
  }

  // SW-emulated stream-out needs LDS
  if (pipelineState->enableSwXfb())
    return true;

  return false;
}

// =====================================================================================================================
// Calculates ES extra LDS size.
//
// @param pipelineState : Pipeline state
unsigned NggLdsManager::calcEsExtraLdsSize(PipelineState *pipelineState) {
  const auto nggControl = pipelineState->getNggControl();
  if (!nggControl->enableNgg)
    return 0;

  const bool hasGs = pipelineState->hasShaderStage(ShaderStageGeometry);
  if (hasGs) {
    // NOTE: Not need ES extra LDS when GS is present.
    return 0;
  }

  if (nggControl->passthroughMode) {
    if (pipelineState->enableSwXfb())
      return LdsRegionSizes[LdsRegionXfbStatInfo];

    return 0;
  }

  return LdsRegionSizes[LdsRegionVertPosData] + LdsRegionSizes[LdsRegionVertCountInWaves] +
         (nggControl->compactMode == NggCompactDisable ? 0 : LdsRegionSizes[LdsRegionVertThreadIdMap]);
}

// =====================================================================================================================
// Calculates GS extra LDS size (used for operations other than ES-GS ring and GS-VS ring read/write).
//
// @param pipelineState : Pipeline state
unsigned NggLdsManager::calcGsExtraLdsSize(PipelineState *pipelineState) {
  const auto nggControl = pipelineState->getNggControl();
  if (!nggControl->enableNgg)
    return 0;

  const bool hasGs = pipelineState->hasShaderStage(ShaderStageGeometry);
  if (!hasGs) {
    // NOTE: Not need GS extra LDS when GS is not present.
    return 0;
  }

  if (pipelineState->enableSwXfb()) {
    // NOTE: The region OutVertCountInWaves and OutVertThreadIdMap is overlapped with OutPrimCountInWaves and
    // OutPrimThreadIdMap when SW-emulated stream-out is enabled. Therefore, they are not counted in here.
    return LdsRegionSizes[LdsRegionOutPrimData] + LdsRegionSizes[LdsRegionOutPrimCountInWaves] +
           LdsRegionSizes[LdsRegionOutPrimThreadIdMap] + LdsRegionSizes[LdsRegionGsXfbStatInfo];
  }

  return LdsRegionSizes[LdsRegionOutPrimData] + LdsRegionSizes[LdsRegionOutVertCountInWaves] +
         (nggControl->compactMode == NggCompactDisable ? 0 : LdsRegionSizes[LdsRegionOutVertThreadIdMap]);
}

// =====================================================================================================================
// Reads value from LDS.
//
// @param readTy : Type of value read from LDS
// @param ldsOffset : Start offset to do LDS read operations
// @param useDs128 : Whether to use 128-bit LDS load, 16-byte alignment is guaranteed by caller
Value *NggLdsManager::readValueFromLds(Type *readTy, Value *ldsOffset, bool useDs128) {
  assert(readTy->isIntOrIntVectorTy() || readTy->isFPOrFPVectorTy());

  unsigned alignment = readTy->getScalarSizeInBits() / 8;
  if (useDs128) {
    assert(readTy->getPrimitiveSizeInBits() == 128);
    alignment = 16;
  }

  // NOTE: LDS variable is defined as a pointer to i32 array. We cast it to a pointer to i8 array first.
  assert(m_lds);
  auto lds = ConstantExpr::getBitCast(
      m_lds, PointerType::get(m_builder->getInt8Ty(), m_lds->getType()->getPointerAddressSpace()));

  Value *readPtr = m_builder->CreateGEP(m_builder->getInt8Ty(), lds, ldsOffset);
  readPtr = m_builder->CreateBitCast(readPtr, PointerType::get(readTy, ADDR_SPACE_LOCAL));

  return m_builder->CreateAlignedLoad(readTy, readPtr, Align(alignment));
}

// =====================================================================================================================
// Writes value to LDS.
//
// @param writeValue : Value written to LDS
// @param ldsOffset : Start offset to do LDS write operations
// @param useDs128 : Whether to use 128-bit LDS store, 16-byte alignment is guaranteed by caller
void NggLdsManager::writeValueToLds(Value *writeValue, Value *ldsOffset, bool useDs128) {
  auto writeTy = writeValue->getType();
  assert(writeTy->isIntOrIntVectorTy() || writeTy->isFPOrFPVectorTy());

  unsigned alignment = writeTy->getScalarSizeInBits() / 8;
  if (useDs128) {
    assert(writeTy->getPrimitiveSizeInBits() == 128);
    alignment = 16;
  }

  // NOTE: LDS variable is defined as a pointer to i32 array. We cast it to a pointer to i8 array first.
  assert(m_lds != nullptr);
  auto lds = ConstantExpr::getBitCast(
      m_lds, PointerType::get(m_builder->getInt8Ty(), m_lds->getType()->getPointerAddressSpace()));

  Value *writePtr = m_builder->CreateGEP(m_builder->getInt8Ty(), lds, ldsOffset);
  writePtr = m_builder->CreateBitCast(writePtr, PointerType::get(writeTy, ADDR_SPACE_LOCAL));

  m_builder->CreateAlignedStore(writeValue, writePtr, Align(alignment));
}

// =====================================================================================================================
// Does atomic binary operation with the value stored in LDS.
//
// @param atomicOp : Atomic binary operation
// @param atomicValue : Value to do atomic operation
// @param ldsOffset : Start offset to do LDS atomic operations
void NggLdsManager::atomicOpWithLds(AtomicRMWInst::BinOp atomicOp, Value *atomicValue, Value *ldsOffset) {
  assert(atomicValue->getType()->isIntegerTy(32));

  // NOTE: LDS variable is defined as a pointer to i32 array. The LDS offset here has to be casted to dword offset
  // from byte offset.
  ldsOffset = m_builder->CreateLShr(ldsOffset, 2);

  Value *atomicPtr = m_builder->CreateGEP(m_lds->getValueType(), m_lds, {m_builder->getInt32(0), ldsOffset});

  auto atomicInst = m_builder->CreateAtomicRMW(atomicOp, atomicPtr, atomicValue, MaybeAlign(),
                                               AtomicOrdering::SequentiallyConsistent, SyncScope::System);
  atomicInst->setVolatile(true);
}

} // namespace lgc
