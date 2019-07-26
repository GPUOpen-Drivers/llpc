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
const uint32_t NggLdsManager::LdsRegionSizes[LdsRegionCount] =
{
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
};

// =====================================================================================================================
// Initialize static members
const char* NggLdsManager::LdsRegionNames[LdsRegionCount] =
{
    "Distributed primitive ID",             // LdsRegionDistribPrimId
    "Vertex position data",                 // LdsRegionPosData
    "Draw flag",                            // LdsRegionDrawFlag
    "Primitive count in sub-group",         // LdsRegionPrimCountInWaves
    "Vertex count in sub-group",            // LdsRegionVertCountInWaves
    "Cull distance",                        // LdsRegionCullDistance
    "Compacted thread ID in sub-group",     // LdsRegionCompactThreadIdInSubgroup
    "Compacted vertex ID (VS)",             // LdsRegionCompactVertexId
    "Compacted instance ID (VS)",           // LdsRegionCompactInstanceId
    "Compacted primitive ID (VS)",          // LdsRegionCompactPrimId
    "Compacted tesscoord X (TES)",          // LdsRegionCompactTessCoordX
    "Compacted tesscoord Y (TES)",          // LdsRegionCompactTessCoordY
    "Compacted patch ID (TES)",             // LdsRegionCompactPatchId
    "Compacted relative patch ID (TES)",    // LdsRegionCompactRelPatchId
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
    const bool hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                                      ShaderStageToMask(ShaderStageTessEval))) != 0);

    //
    // Create global variable modeling LDS
    //
    m_pLds = Patch::GetLdsVariable(pModule);

    //
    // Calculate start LDS offset for all LDS region types
    //
    memset(&m_ldsRegionStart, InvalidValue, sizeof(m_ldsRegionStart)); // Initialize them to invalid value (0xFFFFFFFF)

    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC NGG LDS region info\n\n");

    m_ldsRegionStart[LdsRegionDistribPrimId] = 0;

    LLPC_OUTS(format("%-40s : offset = %5u, size = %5u",
              LdsRegionNames[LdsRegionDistribPrimId],
              m_ldsRegionStart[LdsRegionDistribPrimId],
              LdsRegionSizes[LdsRegionDistribPrimId]) << "\n");

    if (pNggControl->passthroughMode == false)
    {
        uint32_t ldsRegionStart = 0;
        for (uint32_t region = 0; region < LdsRegionCount; ++region)
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

            LLPC_OUTS(format("%-40s : offset = %5u, size = %5u",
                      LdsRegionNames[region], m_ldsRegionStart[region], LdsRegionSizes[region]) << "\n");
        }
    }

    LLPC_OUTS("\n");
}

// =====================================================================================================================
// Calculates total size for all LDS region types (extra LDS size used for NGG).
uint32_t NggLdsManager::CalcLdsRegionTotalSize(
    GraphicsContext* pContext) // [in] LLPC graphics context
{
    const auto pNggControl = pContext->GetNggControl();
    if (pNggControl->enableNgg == false)
    {
        return 0;
    }

    uint32_t regionTotalSize = 0;

    const uint32_t stageMask = pContext->GetShaderStageMask();
    const bool hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                                      ShaderStageToMask(ShaderStageTessEval))) != 0);
    const bool hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

    if (pNggControl->passthroughMode)
    {
        // NOTE: For NGG pass-through mode, only primitive ID region is valid.
        bool distributePrimId = false;
        if (hasGs)
        {
            // TODO: Support GS in primitive shader.
            LLPC_NOT_IMPLEMENTED();
        }
        else
        {
            if (hasTs == false)
            {
                const auto& builtInUsage = pContext->GetShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
                distributePrimId = builtInUsage.primitiveId;
            }
        }

        regionTotalSize = distributePrimId ? LdsRegionSizes[LdsRegionDistribPrimId] : 0;
    }
    else
    {
        for (uint32_t region = 0; region < LdsRegionCount; ++region)
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

            regionTotalSize += LdsRegionSizes[region];
        }
    }

    return regionTotalSize;
}

// =====================================================================================================================
// Reads value from LDS.
Value* NggLdsManager::ReadValueFromLds(
    Type*        pReadTy,       // [in] Type of value read from LDS
    Value*       pLdsOffset)    // [in] Start offset to do LDS read operations
{
    LLPC_ASSERT(m_pLds != nullptr);
    LLPC_ASSERT(pReadTy->isIntOrIntVectorTy() || pReadTy->isFPOrFPVectorTy());

    const uint32_t readBits = pReadTy->getPrimitiveSizeInBits();

    uint32_t bitWidth = 0;
    uint32_t compCount = 0;

    if (readBits % 128 == 0)
    {
        bitWidth = 128;
        compCount = readBits / 128;
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

    Type* pCompTy = IntegerType::get(*m_pContext, bitWidth);
    Value* pReadValue =  UndefValue::get((compCount > 1) ? VectorType::get(pCompTy, compCount) : pCompTy);

    // NOTE: LDS variable is defined as a pointer to i32 array. We cast it to a pointer to i8 array first.
    auto pLds = ConstantExpr::getBitCast(m_pLds,
                    PointerType::get(m_pContext->Int8Ty(), m_pLds->getType()->getPointerAddressSpace()));

    for (uint32_t i = 0; i < compCount; ++i)
    {
        Value* pLoadPtr = m_pBuilder->CreateGEP(pLds, pLdsOffset);
        if (bitWidth != 8)
        {
            pLoadPtr = m_pBuilder->CreateBitCast(pLoadPtr, PointerType::get(pCompTy, ADDR_SPACE_LOCAL));
        }

        // NOTE: Use "volatile" for load to prevent optimization.
        Value* pLoadValue = m_pBuilder->CreateAlignedLoad(pLoadPtr, m_pLds->getAlignment(), true);

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
    Value*        pLdsOffset)       // [in] Start offset to do LDS write operations
{
    LLPC_ASSERT(m_pLds != nullptr);

    auto pWriteTy = pWriteValue->getType();
    LLPC_ASSERT(pWriteTy->isIntOrIntVectorTy() || pWriteTy->isFPOrFPVectorTy());

    const uint32_t writeBits = pWriteTy->getPrimitiveSizeInBits();

    uint32_t bitWidth = 0;
    uint32_t compCount = 0;

    if (writeBits % 128 == 0)
    {
        bitWidth = 128;
        compCount = writeBits / 128;
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

    Type* pCompTy = IntegerType::get(*m_pContext, bitWidth);
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
            pStorePtr = m_pBuilder->CreateBitCast(pStorePtr, PointerType::get(pCompTy, ADDR_SPACE_LOCAL));
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

        // NOTE: Use "volatile" for store to prevent optimization.
        m_pBuilder->CreateAlignedStore(pStoreValue, pStorePtr, m_pLds->getAlignment(), true);

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
    pLdsOffset = m_pBuilder->CreateLShr(pLdsOffset, m_pBuilder->getInt32(2));

    Value* pAtomicPtr = m_pBuilder->CreateGEP(m_pLds, { m_pBuilder->getInt32(0), pLdsOffset });

    auto pAtomicInst = m_pBuilder->CreateAtomicRMW(atomicOp,
                                                   pAtomicPtr,
                                                   pAtomicValue,
                                                   AtomicOrdering::SequentiallyConsistent,
                                                   SyncScope::System);
    pAtomicInst->setVolatile(true);
}

} // Llpc
