/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @brief LLPC source file: contains implementation of class Llpc::NggLdsManager.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-ngg-lds-manager"

#include "llvm/Linker/Linker.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include "llpcContext.h"
#include "llpcGraphicsContext.h"
#include "llpcGfx9Chip.h"
#include "llpcNggLdsManager.h"
#include "llpcPatch.h"

using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
// Initialize static members
uint32_t NggLdsManager::LdsRegionSizes[LdsRegionCount] =
{
    // LDS region size for ES-only

    // 1 DWORD (uint32_t) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup,                     // LdsRegionDistribPrimId
    // 4 DWORDs (vec4) per thread
    SizeOfVec4 * Gfx9::NggMaxThreadsPerSubgroup,                      // LdsRegionPosData
    // 1 BYTE (uint8_t) per thread
    Gfx9::NggMaxThreadsPerSubgroup,                                   // LdsRegionDrawFlag
    // 1 DWORD per wave (8 potential waves) + 1 DWORD for the entire sub-group
    SizeOfDword * Gfx9::NggMaxWavesPerSubgroup + SizeOfDword,         // LdsRegionPrimCountInWaves
    // 1 DWORD per wave (8 potential waves) + 1 DWORD for the entire sub-group
    SizeOfDword * Gfx9::NggMaxWavesPerSubgroup + SizeOfDword,         // LdsRegionVertCountInWaves
    // 1 DWORD (uint32_t) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup,                     // LdsRegionCullDistance
    // 1 BYTE (uint8_t) per thread
    Gfx9::NggMaxThreadsPerSubgroup,                                   // LdsRegionCompactThreadIdInSubgroup
    // 1 DWORD (uint32_t) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup,                     // LdsRegionCompactVertexId
    // 1 DWORD (uint32_t) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup,                     // LdsRegionCompactInstanceId
    // 1 DWORD (uint32_t) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup,                     // LdsRegionCompactPrimId
    // 1 DWORD (uint32_t) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup,                     // LdsRegionCompactTessCoordX
    // 1 DWORD (uint32_t) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup,                     // LdsRegionCompactTessCoordY
    // 1 DWORD (uint32_t) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup,                     // LdsRegionCompactPatchId
    // 1 DWORD (uint32_t) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup,                     // LdsRegionCompactRelPatchId

    // LDS region size for ES-GS

    // ES-GS ring size is dynamically calculated (don't use it)
    InvalidValue,                                                                 // LdsRegionEsGsRing
    // 1 DWORD (uint32_t) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup,                      // LdsRegionPrimData
    // 1 DWORD per wave (8 potential waves) + 1 DWORD for the entire sub-group (4 GS streams)
    MaxGsStreams * (SizeOfDword * Gfx9::NggMaxWavesPerSubgroup + SizeOfDword),   // LdsRegionGsOutVertCountInWaves
    // 1 DWORD (uint32_t) per thread
    SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup,                      // LdsRegionGsVsRingItemOffset
    // GS-VS ring size is dynamically calculated (don't use it)
    InvalidValue,                                                                 // LdsRegionGsVsRing
};

// =====================================================================================================================
// Initialize static members
const char* NggLdsManager::LdsRegionNames[LdsRegionCount] =
{
    // LDS region name for ES-only
    "Distributed primitive ID",             // LdsRegionDistribPrimId
    "Vertex position data",                 // LdsRegionPosData
    "Draw flag",                            // LdsRegionDrawFlag
    "Primitive count in waves",             // LdsRegionPrimCountInWaves
    "Vertex count in waves",                // LdsRegionVertCountInWaves
    "Cull distance",                        // LdsRegionCullDistance
    "Compacted thread ID in sub-group",     // LdsRegionCompactThreadIdInSubgroup
    "Compacted vertex ID (VS)",             // LdsRegionCompactVertexId
    "Compacted instance ID (VS)",           // LdsRegionCompactInstanceId
    "Compacted primitive ID (VS)",          // LdsRegionCompactPrimId
    "Compacted tesscoord X (TES)",          // LdsRegionCompactTessCoordX
    "Compacted tesscoord Y (TES)",          // LdsRegionCompactTessCoordY
    "Compacted patch ID (TES)",             // LdsRegionCompactPatchId
    "Compacted relative patch ID (TES)",    // LdsRegionCompactRelPatchId

    // LDS region name for ES-GS
    "ES-GS ring",                           // LdsRegionEsGsRing
    "GS output primitive data",             // LdsRegionPrimData
    "GS output vertex count in waves",      // LdsRegionGsOutVertCountInWaves
    "GS-VS ring item offset",               // LdsRegionGsVsRingItemOffset
    "GS-VS ring",                           // LdsRegionGsVsRing
};

// =====================================================================================================================
NggLdsManager::NggLdsManager(
    Module*      pModule,    // [in] LLVM module
    Context*     pContext,   // [in] LLPC context
    IRBuilder<>* pBuilder)   // [in] LLVM IR builder
    :
    m_pContext(pContext),
    m_waveCountInSubgroup(Gfx9::NggMaxThreadsPerSubgroup / pContext->GetGpuProperty()->waveSize),
    m_pBuilder(pBuilder)
{
    LLPC_ASSERT(pBuilder != nullptr);

    const auto pNggControl = pContext->GetNggControl();
    LLPC_ASSERT(pNggControl->enableNgg);

    const uint32_t stageMask = pContext->GetShaderStageMask();
    const bool hasGs = (stageMask & ShaderStageToMask(ShaderStageGeometry));
    const bool hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                                      ShaderStageToMask(ShaderStageTessEval))) != 0);

    //
    // Create global variable modeling LDS
    //
    m_pLds = Patch::GetLdsVariable(pModule);

    memset(&m_ldsRegionStart, InvalidValue, sizeof(m_ldsRegionStart)); // Initialized to invalid value (0xFFFFFFFF)

    //
    // Calculate start LDS offset for all available LDS region types
    //

    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC NGG LDS region info (in bytes)\n\n");

    if (hasGs)
    {
        //
        // The LDS layout is something like this:
        //
        // +------------+------------------------+--------------------------------+------------+
        // | ES-GS ring | GS out primitive data  | GS out vertex count (in waves) | GS-VS ring |
        // +------------+------------------------+--------------------------------+------------+
        //              | GS-VS ring item offset |
        //              +------------------------+
        //
        const auto& calcFactor = pContext->GetShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;
        // NOTE: We round ES-GS LDS size to 4-DWORD alignment. This is for later LDS read/write operations of mutilple
        // DWORDs (such as DS128).
        const uint32_t esGsRingLdsSize = RoundUpToMultiple(calcFactor.esGsLdsSize, 4u) * SizeOfDword;
        const uint32_t gsVsRingLdsSize = calcFactor.gsOnChipLdsSize * SizeOfDword - esGsRingLdsSize -
                                         CalcGsExtraLdsSize(static_cast<GraphicsContext*>(
                                             pContext->GetPipelineContext()));

        uint32_t ldsRegionStart = 0;

        for (uint32_t region = LdsRegionGsBeginRange; region <= LdsRegionGsEndRange; ++region)
        {
            uint32_t ldsRegionSize = LdsRegionSizes[region];

            if (region == LdsRegionGsVsRingItemOffset)
            {
                // An overlapped region, reused
                m_ldsRegionStart[LdsRegionGsVsRingItemOffset] = m_ldsRegionStart[LdsRegionGsOutPrimData];

                LLPC_OUTS(format("%-40s : offset = 0x%05" PRIX32 ", size = 0x%05" PRIX32,
                    LdsRegionNames[region], m_ldsRegionStart[region], ldsRegionSize) << "\n");

                continue;
            }

            // NOTE: LDS size of ES-GS ring is calculated (by rounding it up to 16-byte alignment)
            if (region == LdsRegionEsGsRing)
            {
                ldsRegionSize = esGsRingLdsSize;
            }

            // NOTE: LDS size of ES-GS ring is calculated
            if (region == LdsRegionGsVsRing)
            {
                ldsRegionSize = gsVsRingLdsSize;
            }

            m_ldsRegionStart[region] = ldsRegionStart;
            LLPC_ASSERT(ldsRegionSize != InvalidValue);
            ldsRegionStart += ldsRegionSize;

            LLPC_OUTS(format("%-40s : offset = 0x%05" PRIX32 ", size = 0x%05" PRIX32,
                LdsRegionNames[region], m_ldsRegionStart[region], ldsRegionSize) << "\n");
        }
    }
    else
    {
        m_ldsRegionStart[LdsRegionDistribPrimId] = 0;

        LLPC_OUTS(format("%-40s : offset = 0x%05" PRIX32 ", size = 0x%05" PRIX32,
                         LdsRegionNames[LdsRegionDistribPrimId],
                         m_ldsRegionStart[LdsRegionDistribPrimId],
                         LdsRegionSizes[LdsRegionDistribPrimId]) << "\n");

        if (pNggControl->passthroughMode == false)
        {
            //
            // The LDS layout is something like this:
            //
            // +--------------------------+-----------+----------------------------+---------------+
            // | Vertex position data     | Draw flag | Vertex count (in waves)    | Cull distance | >>>
            // +--------------------------+-----------+----------------------------+---------------+
            // | Distributed primitive ID |           | Primitive count (in waves) |
            // +--------------------------+           +----------------------------+
            //
            //     | =============== Compacted data region (for vertex compaction) ================ |
            //     +------------------+-------------+-------------+-------------+
            // >>> | Vertex thread ID | Vertex ID   | Instance ID | Primtive ID |                     (VS)
            //     +------------------+-------------+-------------+-------------+-------------------+
            //                        | Tesscoord X | Tesscoord Y | Patch ID    | Relative patch ID | (TES)
            //                        +-------------+-------------+-------------+-------------------+
            //
            uint32_t ldsRegionStart = 0;
            for (uint32_t region = LdsRegionEsBeginRange; region <= LdsRegionEsEndRange; ++region)
            {
                // NOTE: For NGG non pass-through mode, primitive ID region is overlapped with position data.
                if (region == LdsRegionDistribPrimId)
                {
                    continue;
                }

                // NOTE: If cull distance culling is disabled, skip this region
                if ((region == LdsRegionCullDistance) && (pNggControl->enableCullDistanceCulling == false))
                {
                    continue;
                }

                // NOTE: If NGG compaction is based on sub-group, those regions that are for vertex compaction should be
                // skipped.
                if ((pNggControl->compactMode == NggCompactSubgroup) &&
                    ((region >= LdsRegionCompactBeginRange) && (region <= LdsRegionCompactEndRange)))
                {
                    continue;
                }

                if (hasTs)
                {
                    // Skip those regions that are for VS only
                    if ((region == LdsRegionCompactVertexId) || (region == LdsRegionCompactInstanceId) ||
                        (region == LdsRegionCompactPrimId))
                    {
                        continue;
                    }
                }
                else
                {
                    // Skip those regions that are for TES only
                    if ((region == LdsRegionCompactTessCoordX) || (region == LdsRegionCompactTessCoordY) ||
                        (region == LdsRegionCompactRelPatchId) || (region == LdsRegionCompactPatchId))
                    {
                        continue;
                    }
                }

                m_ldsRegionStart[region] = ldsRegionStart;
                ldsRegionStart += LdsRegionSizes[region];

                LLPC_OUTS(format("%-40s : offset = 0x%05" PRIX32 ", size = 0x%05" PRIX32,
                    LdsRegionNames[region], m_ldsRegionStart[region], LdsRegionSizes[region]) << "\n");
            }
        }
    }

    LLPC_OUTS("\n");
}

// =====================================================================================================================
// Calculates ES extra LDS size.
uint32_t NggLdsManager::CalcEsExtraLdsSize(
    GraphicsContext* pContext) // [in] LLPC graphics context
{
    const auto pNggControl = pContext->GetNggControl();
    if (pNggControl->enableNgg == false)
    {
        return 0;
    }

    const uint32_t stageMask = pContext->GetShaderStageMask();
    const bool hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

    if (hasGs)
    {
        // NOTE: Not need ES extra LDS when GS is present.
        return 0;
    }

    const bool hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                                      ShaderStageToMask(ShaderStageTessEval))) != 0);

    uint32_t esExtraLdsSize = 0;

    if (pNggControl->passthroughMode)
    {
        // NOTE: For NGG pass-through mode, only primitive ID region is valid.
        bool distributePrimId = false;
        if (hasTs == false)
        {
            const auto& builtInUsage = pContext->GetShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
            distributePrimId = builtInUsage.primitiveId;
        }

        esExtraLdsSize = distributePrimId ? LdsRegionSizes[LdsRegionDistribPrimId] : 0;
    }
    else
    {
        for (uint32_t region = LdsRegionEsBeginRange; region <= LdsRegionEsEndRange; ++region)
        {
            // NOTE: For NGG non pass-through mode, primitive ID region is overlapped with position data.
            if (region == LdsRegionDistribPrimId)
            {
                continue;
            }

            // NOTE: If cull distance culling is disabled, skip this region
            if ((region == LdsRegionCullDistance) && (pNggControl->enableCullDistanceCulling == false))
            {
                continue;
            }

            // NOTE: If NGG compaction is based on sub-group, those regions that are for vertex compaction should be
            // skipped.
            if ((pNggControl->compactMode == NggCompactSubgroup) &&
                ((region >= LdsRegionCompactBeginRange) && (region <= LdsRegionCompactEndRange)))
            {
                continue;
            }

            if (hasTs)
            {
                // Skip those regions that are for VS only
                if ((region == LdsRegionCompactVertexId) || (region == LdsRegionCompactInstanceId) ||
                    (region == LdsRegionCompactPrimId))
                {
                    continue;
                }
            }
            else
            {
                // Skip those regions that are for TES only
                if ((region == LdsRegionCompactTessCoordX) || (region == LdsRegionCompactTessCoordY) ||
                    (region == LdsRegionCompactRelPatchId) || (region == LdsRegionCompactPatchId))
                {
                    continue;
                }
            }

            esExtraLdsSize += LdsRegionSizes[region];
        }
    }

    return esExtraLdsSize;
}

// =====================================================================================================================
// Calculates GS extra LDS size (used for operations other than ES-GS ring and GS-VS ring read/write).
uint32_t NggLdsManager::CalcGsExtraLdsSize(
    GraphicsContext* pContext) // [in] LLPC graphics context
{
    const auto pNggControl = pContext->GetNggControl();
    if (pNggControl->enableNgg == false)
    {
        return 0;
    }

    const uint32_t stageMask = pContext->GetShaderStageMask();
    const bool hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);
    if (hasGs == false)
    {
        // NOTE: Not need GS extra LDS when GS is not present.
        return 0;
    }

    uint32_t gsExtraLdsSize = LdsRegionSizes[LdsRegionGsOutPrimData] + LdsRegionSizes[LdsRegionGsOutVertCountInWaves];

    return gsExtraLdsSize;
}

// =====================================================================================================================
// Reads value from LDS.
Value* NggLdsManager::ReadValueFromLds(
    Type*        pReadTy,       // [in] Type of value read from LDS
    Value*       pLdsOffset,    // [in] Start offset to do LDS read operations
    bool         useDs128)      // Whether to use 128-bit LDS load, 16-byte alignment is guaranteed by caller
{
    LLPC_ASSERT(m_pLds != nullptr);
    LLPC_ASSERT(pReadTy->isIntOrIntVectorTy() || pReadTy->isFPOrFPVectorTy());

    const uint32_t readBits = pReadTy->getPrimitiveSizeInBits();

    uint32_t bitWidth = 0;
    uint32_t compCount = 0;
    uint32_t alignment = 4;

    if (readBits % 128 == 0)
    {
        bitWidth = 128;
        compCount = readBits / 128;

        if (useDs128)
        {
            alignment = 16; // Set alignment to 16-byte to use 128-bit LDS load
        }
    }
    else if (readBits % 64 == 0)
    {
        bitWidth = 64;
        compCount = readBits / 64;
    }
    else if (readBits % 32 == 0)
    {
        bitWidth = 32;
        compCount = readBits / 32;
    }
    else if (readBits % 16 == 0)
    {
        bitWidth = 16;
        compCount = readBits / 16;
    }
    else
    {
        LLPC_ASSERT(readBits % 8 == 0);
        bitWidth = 8;
        compCount = readBits / 8;
    }

    Type* pCompTy = m_pBuilder->getIntNTy(bitWidth);
    Value* pReadValue =  UndefValue::get((compCount > 1) ? VectorType::get(pCompTy, compCount) : pCompTy);

    // NOTE: LDS variable is defined as a pointer to i32 array. We cast it to a pointer to i8 array first.
    auto pLds = ConstantExpr::getBitCast(m_pLds,
                    PointerType::get(m_pContext->Int8Ty(), m_pLds->getType()->getPointerAddressSpace()));

    for (uint32_t i = 0; i < compCount; ++i)
    {
        Value* pLoadPtr = m_pBuilder->CreateGEP(pLds, pLdsOffset);
        if (bitWidth != 8)
        {
            if (useDs128 && (bitWidth == 128))
            {
                // NOTE: For single 128-bit LDS load, the friendly data type for backend compiler is <4 x i32>.
                pLoadPtr =
                    m_pBuilder->CreateBitCast(pLoadPtr, PointerType::get(m_pContext->Int32x4Ty(), ADDR_SPACE_LOCAL));
            }
            else
            {
                pLoadPtr = m_pBuilder->CreateBitCast(pLoadPtr, PointerType::get(pCompTy, ADDR_SPACE_LOCAL));
            }
        }

        // NOTE: Use "volatile" for load to prevent optimization.
        Value* pLoadValue = m_pBuilder->CreateAlignedLoad(pLoadPtr, alignment, true);
        if (useDs128 && (bitWidth == 128))
        {
            // Convert <4 x i32> to i128
            pLoadValue = m_pBuilder->CreateBitCast(pLoadValue, m_pBuilder->getInt128Ty());
        }

        if (compCount > 1)
        {
            pReadValue = m_pBuilder->CreateInsertElement(pReadValue, pLoadValue, i);
        }
        else
        {
            pReadValue = pLoadValue;
        }

        if (compCount > 1)
        {
            pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(bitWidth / 8));
        }
    }

    if (pReadValue->getType() != pReadTy)
    {
        pReadValue = m_pBuilder->CreateBitCast(pReadValue, pReadTy);
    }

    return pReadValue;
}

// =====================================================================================================================
// Writes value to LDS.
void NggLdsManager::WriteValueToLds(
    Value*        pWriteValue,      // [in] Value written to LDS
    Value*        pLdsOffset,       // [in] Start offset to do LDS write operations
    bool          useDs128)         // Whether to use 128-bit LDS store, 16-byte alignment is guaranteed by caller
{
    LLPC_ASSERT(m_pLds != nullptr);

    auto pWriteTy = pWriteValue->getType();
    LLPC_ASSERT(pWriteTy->isIntOrIntVectorTy() || pWriteTy->isFPOrFPVectorTy());

    const uint32_t writeBits = pWriteTy->getPrimitiveSizeInBits();

    uint32_t bitWidth = 0;
    uint32_t compCount = 0;
    uint32_t alignment = 4;

    if (writeBits % 128 == 0)
    {
        bitWidth = 128;
        compCount = writeBits / 128;

        if (useDs128)
        {
            alignment = 16; // Set alignment to 16-byte to use 128-bit LDS store
        }
    }
    else if (writeBits % 64 == 0)
    {
        bitWidth = 64;
        compCount = writeBits / 64;
    }
    else if (writeBits % 32 == 0)
    {
        bitWidth = 32;
        compCount = writeBits / 32;
    }
    else if (writeBits % 16 == 0)
    {
        bitWidth = 16;
        compCount = writeBits / 16;
    }
    else
    {
        LLPC_ASSERT(writeBits % 8 == 0);
        bitWidth = 8;
        compCount = writeBits / 8;
    }

    Type* pCompTy = m_pBuilder->getIntNTy(bitWidth);
    pWriteTy = (compCount > 1) ? VectorType::get(pCompTy, compCount) : pCompTy;

    if (pWriteValue->getType() != pWriteTy)
    {
        pWriteValue = m_pBuilder->CreateBitCast(pWriteValue, pWriteTy);
    }

    // NOTE: LDS variable is defined as a pointer to i32 array. We cast it to a pointer to i8 array first.
    auto pLds = ConstantExpr::getBitCast(m_pLds,
                  PointerType::get(m_pContext->Int8Ty(), m_pLds->getType()->getPointerAddressSpace()));

    for (uint32_t i = 0; i < compCount; ++i)
    {
        Value* pStorePtr = m_pBuilder->CreateGEP(pLds, pLdsOffset);
        if (bitWidth != 8)
        {
            if (useDs128 && (bitWidth == 128))
            {
                // NOTE: For single 128-bit LDS store, the friendly data type for backend compiler is <4 x i32>.
                pStorePtr =
                    m_pBuilder->CreateBitCast(pStorePtr, PointerType::get(m_pContext->Int32x4Ty(), ADDR_SPACE_LOCAL));
            }
            else
            {
                pStorePtr = m_pBuilder->CreateBitCast(pStorePtr, PointerType::get(pCompTy, ADDR_SPACE_LOCAL));
            }
        }

        Value* pStoreValue = nullptr;
        if (compCount > 1)
        {
            pStoreValue = m_pBuilder->CreateExtractElement(pWriteValue, i);
        }
        else
        {
            pStoreValue = pWriteValue;
        }

        if (useDs128 && (bitWidth == 128))
        {
            // Convert i128 to <4 x i32>
            pStoreValue = m_pBuilder->CreateBitCast(pStoreValue, m_pContext->Int32x4Ty());
        }

        // NOTE: Use "volatile" for store to prevent optimization.
        m_pBuilder->CreateAlignedStore(pStoreValue, pStorePtr, alignment, true);

        if (compCount > 1)
        {
            pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(bitWidth / 8));
        }
    }
}

// =====================================================================================================================
// Does atomic binary operation with the value stored in LDS.
void NggLdsManager::AtomicOpWithLds(
    AtomicRMWInst::BinOp atomicOp,      // Atomic binary operation
    Value*               pAtomicValue,  // [in] Value to do atomic operation
    Value*               pLdsOffset)    // [in] Start offset to do LDS atomic operations
{
    LLPC_ASSERT(pAtomicValue->getType()->isIntegerTy(32));

    // NOTE: LDS variable is defined as a pointer to i32 array. The LDS offset here has to be casted to DWORD offset
    // from BYTE offset.
    pLdsOffset = m_pBuilder->CreateLShr(pLdsOffset, 2);

    Value* pAtomicPtr = m_pBuilder->CreateGEP(m_pLds, { m_pBuilder->getInt32(0), pLdsOffset });

    auto pAtomicInst = m_pBuilder->CreateAtomicRMW(atomicOp,
                                                   pAtomicPtr,
                                                   pAtomicValue,
                                                   AtomicOrdering::SequentiallyConsistent,
                                                   SyncScope::System);
    pAtomicInst->setVolatile(true);
}

} // Llpc
