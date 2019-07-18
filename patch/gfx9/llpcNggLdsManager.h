/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @brief LLPC header file: contains declaration of class Llpc::NggLdsManager.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"

#include "llpc.h"
#include "llpcInternal.h"

namespace Llpc
{

class Context;
class GraphicsContext;

// Enumerates the types of LDS regions used in NGG.
enum NggLdsRegionType
{
    LdsRegionDistribPrimId = 0,         // Distributed primitive ID (a special region, overlapped with position data in
                                        // NGG non pass-through mode)
    LdsRegionPosData,                   // Position data to export
    LdsRegionDrawFlag,                  // Draw flag indicating whether the primitive survives
    LdsRegionPrimCountInWaves,          // Primitive count accumulated per wave (8 potential waves) and per sub-group
    LdsRegionVertCountInWaves,          // Vertex count accumulated per wave (8 potential waves) and per sub-group
    LdsRegionCullDistance,              // Aggregated sign value of cull distance (bitmask)
    // Below regions are for vertex compaction
    LdsRegionCompactThreadIdInSubgroup, // Thread ID in sub-group to export primitive data
    LdsRegionCompactVertexId,           // Vertex ID (VS only)
    LdsRegionCompactInstanceId,         // Instance ID (VS only)
    LdsRegionCompactPrimId,             // Primitive ID (VS only)
    LdsRegionCompactTessCoordX,         // X of tessCoord (U) (TES only)
    LdsRegionCompactTessCoordY,         // Y of tessCoord (V) (TES only)
    LdsRegionCompactPatchId,            // Patch ID (TES only)
    LdsRegionCompactRelPatchId,         // Relative patch ID (TES only)

    LdsRegionCount,

    LdsRegionCompactBeginRange = LdsRegionCompactThreadIdInSubgroup,
    LdsRegionCompactEndRange = LdsRegionCompactPatchId,
};

// Size of a DWORD
static uint32_t SizeOfDword = sizeof(uint32_t);

// =====================================================================================================================
// Represents the manager doing shader merge operations.
class NggLdsManager
{
public:
    NggLdsManager(llvm::Module* pModule, Context* pContext);

    static uint32_t CalcLdsRegionTotalSize(GraphicsContext* pContext);

    // Gets LDS size for the specified region
    uint32_t GetLdsRegionSize(NggLdsRegionType region) const { return LdsRegionSizes[region]; }

    // Gets the LDS starting offset for the specified region
    uint32_t GetLdsRegionStart(NggLdsRegionType region) const
    {
        uint32_t regionStart = m_ldsRegionStart[region];
        LLPC_ASSERT(regionStart != InvalidValue);
        return regionStart;
    }

    llvm::Value* ReadValueFromLds(llvm::Type* pReadTy, llvm::Value* pLdsOffset, llvm::BasicBlock* pInsertAtEnd);
    void WriteValueToLds(llvm::Value* pWriteValue, llvm::Value* pLdsOffset, llvm::BasicBlock* pInsertAtEnd);

    void AtomicOpWithLds(llvm::AtomicRMWInst::BinOp atomicOp,
                         llvm::Value*               pAtomicValue,
                         llvm::Value*               pLdsOffset,
                         llvm::BasicBlock*          pInsertAtEnd);

private:
    LLPC_DISALLOW_DEFAULT_CTOR(NggLdsManager);
    LLPC_DISALLOW_COPY_AND_ASSIGN(NggLdsManager);

    // -----------------------------------------------------------------------------------------------------------------

    static const uint32_t LdsRegionSizes[LdsRegionCount];  // LDS sizes for all LDS region types (in BYTEs)
    static const char*    LdsRegionNames[LdsRegionCount];  // Name strings for all LDS region types

    Context*        m_pContext;     // LLPC context

    llvm::GlobalValue*  m_pLds;     // Global variable to model NGG LDS

    uint32_t        m_ldsRegionStart[LdsRegionCount]; // Start LDS offsets for all LDS region types (in BYTEs)

    uint32_t        m_waveCountInSubgroup; // Wave count in sub-group
};

} // Llpc
