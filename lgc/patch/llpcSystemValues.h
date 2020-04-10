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
 * @file  llpcSystemValues.h
 * @brief LLPC header file: per-shader per-pass generating and cache of shader pointers
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include "lgc/llpcBuilderBase.h"
#include "lgc/llpcPipeline.h"
#include "llpcInternal.h"

#include <map>

namespace lgc
{

class PipelineState;
struct ResourceNode;

// =====================================================================================================================
// "Shader system values" are values set up in a shader entrypoint, such as the ES->GS ring buffer descriptor, or the
// user descriptor table pointer, that some passes need access to. The ShaderSystemValues class has an instance for each
// shader in each pass that needs it, and it implements the on-demand emitting of the code to generate such a value, and
// caches the result for the duration of the pass using it. If multiple passes need the same value, then multiple copies
// of the generating code will be emitted, but that will be fixed by a later CSE pass.
class ShaderSystemValues
{
public:
    // Initialize this ShaderSystemValues if it was previously uninitialized.
    void Initialize(PipelineState* pPipelineState, llvm::Function* pEntryPoint);

    // Get ES-GS ring buffer descriptor (for VS/TES output or GS input)
    llvm::Value* GetEsGsRingBufDesc();

    // Get the descriptor for tessellation factor (TF) buffer (TCS output)
    llvm::Value* GetTessFactorBufDesc();

    // Extract value of primitive ID (TCS)
    llvm::Value* GetPrimitiveId();

    // Get invocation ID (TCS)
    llvm::Value* GetInvocationId();

    // Get relative patchId (TCS)
    llvm::Value* GetRelativeId();

    // Get offchip LDS descriptor (TCS and TES)
    llvm::Value* GetOffChipLdsDesc();

    // Get tessellated coordinate (TES)
    llvm::Value* GetTessCoord();

    // Get ES -> GS offsets (GS in)
    llvm::Value* GetEsGsOffsets();

    // Get GS -> VS ring buffer descriptor (GS out and copy shader in)
    llvm::Value* GetGsVsRingBufDesc(unsigned streamId);

    // Get pointers to emit counters (GS)
    llvm::ArrayRef<llvm::Value*> GetEmitCounterPtr();

    // Get descriptor table pointer
    llvm::Value* GetDescTablePtr(unsigned descSet);

    // Get shadow descriptor table pointer
    llvm::Value* GetShadowDescTablePtr(unsigned descSet);

    // Get global internal table pointer as pointer to i8.
    llvm::Value* GetInternalGlobalTablePtr();

    // Get internal per shader table pointer as pointer to i8.
    llvm::Value* GetInternalPerShaderTablePtr();

    // Get number of workgroups value
    llvm::Value* GetNumWorkgroups();

    // Get spilled push constant pointer
    llvm::Value* GetSpilledPushConstTablePtr();

    // Get vertex buffer table pointer
    llvm::Value* GetVertexBufTablePtr();

    // Get stream-out buffer descriptor
    llvm::Value* GetStreamOutBufDesc(unsigned xfbBuffer);

    // Get spill table pointer
    llvm::Instruction* GetSpillTablePtr();

    // Test if shadow descriptor table is enabled
    bool IsShadowDescTableEnabled() const;

private:
    // Get stream-out buffer table pointer
    llvm::Instruction* GetStreamOutTablePtr();

    // Make 64-bit pointer of specified type from 32-bit int, extending with the specified value, or PC if InvalidValue
    llvm::Instruction* MakePointer(llvm::Value* pLowValue, llvm::Type* pPtrTy, unsigned highValue);

    // Get 64-bit extended resource node value
    llvm::Value* GetExtendedResourceNodeValue(unsigned resNodeIdx, llvm::Type* pResNodeTy, unsigned highValue);

    // Get 32 bit resource node value
    llvm::Value* GetResourceNodeValue(unsigned resNodeIdx);

    // Load descriptor from driver table
    llvm::Instruction* LoadDescFromDriverTable(unsigned tableOffset, BuilderBase& builder);

    // Explicitly set the DATA_FORMAT of ring buffer descriptor.
    llvm::Value* SetRingBufferDataFormat(llvm::Value*       pBufDesc,
                                         unsigned           dataFormat,
                                         BuilderBase&       builder) const;

    // Find resource node by type
    const ResourceNode* FindResourceNodeByType(ResourceNodeType type);

    // Find resource node by descriptor set ID
    unsigned FindResourceNodeByDescSet(unsigned descSet);

    // -----------------------------------------------------------------------------------------------------------------

    llvm::Function*     m_pEntryPoint = nullptr;        // Shader entrypoint
    llvm::LLVMContext*  m_pContext;                     // LLVM context
    PipelineState*      m_pPipelineState;               // Pipeline state
    ShaderStage         m_shaderStage;                  // Shader stage

    llvm::Value*        m_pEsGsRingBufDesc = nullptr;   // ES -> GS ring buffer descriptor (VS, TES, and GS)
    llvm::Value*        m_pTfBufDesc = nullptr;         // Descriptor for tessellation factor (TF) buffer (TCS)
    llvm::Value*        m_pOffChipLdsDesc = nullptr;    // Descriptor for off-chip LDS buffer (TCS and TES)
    llvm::SmallVector<llvm::Value*, MaxGsStreams>
                        m_gsVsRingBufDescs;             // GS -> VS ring buffer descriptor (GS out and copy shader in)
    llvm::SmallVector<llvm::Value*, MaxTransformFeedbackBuffers>
                        m_streamOutBufDescs;            // Stream-out buffer descriptors

    llvm::Value*        m_pPrimitiveId = nullptr;       // PrimitiveId (TCS)
    llvm::Value*        m_pInvocationId = nullptr;      // InvocationId (TCS)
    llvm::Value*        m_pRelativeId = nullptr;        // Relative PatchId (TCS)
    llvm::Value*        m_pTessCoord = nullptr;         // Tessellated coordinate (TES)
    llvm::Value*        m_pEsGsOffsets = nullptr;       // ES -> GS offsets (GS in)
    llvm::SmallVector<llvm::Value*, MaxGsStreams>
                        m_emitCounterPtrs;              // Pointers to emit counters (GS)
    llvm::Value*        m_pNumWorkgroups = nullptr;     // NumWorkgroups

    llvm::SmallVector<llvm::Value *, 8>
                        m_descTablePtrs;                // Descriptor table pointers
    llvm::SmallVector<llvm::Value *, 8>
                        m_shadowDescTablePtrs;          // Shadow descriptor table pointers
    llvm::Value*        m_pInternalGlobalTablePtr = nullptr;
                                                        // Internal global table pointer
    llvm::Value*        m_pInternalPerShaderTablePtr = nullptr;
                                                        // Internal per shader table pointer
    llvm::Value*        m_pSpilledPushConstTablePtr = nullptr;
                                                        // Spilled push constant pointer
    llvm::Value*        m_pVbTablePtr = nullptr;        // Vertex buffer table pointer
    llvm::Instruction*  m_pStreamOutTablePtr;           // Stream-out buffer table pointer
    llvm::Instruction*  m_pSpillTablePtr = nullptr;     // Spill table pointer
    llvm::Instruction*  m_pPc = nullptr;                // Program counter as <2 x i32>

    bool                m_enableShadowDescTable = true; // Enable shadow descriptor table
    unsigned            m_shadowDescTablePtrHigh = 2;   // High part of VA for shadow table pointer
                                                        // 2 is a dummy value for use in offline compiling
};

// =====================================================================================================================
// A class that provides a mapping from a shader entrypoint to its ShaderSystemValues object
class PipelineSystemValues
{
public:
    // Initialize this PipelineSystemValues.
    void Initialize(PipelineState* pPipelineState) { m_pPipelineState = pPipelineState; }

    // Get the ShaderSystemValues object for the given shader entrypoint.
    ShaderSystemValues* Get(llvm::Function* pEntryPoint)
    {
        auto pShaderSysValues = &m_shaderSysValuesMap[pEntryPoint];
        pShaderSysValues->Initialize(m_pPipelineState, pEntryPoint);
        return pShaderSysValues;
    }

    // Clear at the end of a pass run.
    void Clear()
    {
        m_shaderSysValuesMap.clear();
    }

private:
    PipelineState*                                m_pPipelineState;
    std::map<llvm::Function*, ShaderSystemValues> m_shaderSysValuesMap;
};

} // lgc
