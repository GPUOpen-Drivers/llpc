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
 * @file  llpcPatchResourceCollect.h
 * @brief LLPC header file: contains declaration of class Llpc::PatchResourceCollect.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/InstVisitor.h"

#include <unordered_set>
#include "llpcPatch.h"
#include "llpcPipelineShaders.h"
#include "llpcPipelineState.h"

namespace Llpc
{

struct NggControl;

// =====================================================================================================================
// Represents the pass of LLVM patching opertions for resource collecting
class PatchResourceCollect:
    public Patch,
    public llvm::InstVisitor<PatchResourceCollect>
{
public:
    PatchResourceCollect();

    void getAnalysisUsage(llvm::AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineStateWrapper>();
        analysisUsage.addRequired<PipelineShaders>();
        analysisUsage.addPreserved<PipelineShaders>();
    }

    virtual bool runOnModule(llvm::Module& module) override;
    virtual void visitCallInst(llvm::CallInst& callInst);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PatchResourceCollect);

    // Determines whether GS on-chip mode is valid for this pipeline, also computes ES-GS/GS-VS ring item size.
    bool CheckGsOnChipValidity();

    // Sets NGG control settings
    void SetNggControl();
    void BuildNggCullingControlRegister(NggControl& nggControl);

    void ProcessShader();

    void ClearInactiveInput();
    void ClearInactiveOutput();

    void MatchGenericInOut();
    void MapBuiltInToGenericInOut();

    void ReviseTessExecutionMode();
    void MapGsGenericOutput(GsOutLocInfo outLocInfo);
    void MapGsBuiltInOutput(uint32_t builtInId, uint32_t elemCount);

    // -----------------------------------------------------------------------------------------------------------------

    PipelineState*                  m_pPipelineState;           // Pipeline state

    std::unordered_set<llvm::CallInst*> m_deadCalls;            // Dead calls

    std::unordered_set<uint32_t>    m_activeInputLocs;          // Locations of active generic inputs
    std::unordered_set<uint32_t>    m_activeInputBuiltIns;      // IDs of active built-in inputs
    std::unordered_set<uint32_t>    m_activeOutputBuiltIns;     // IDs of active built-in outputs

    std::unordered_set<uint32_t>    m_importedOutputLocs;       // Locations of imported generic outputs
    std::unordered_set<uint32_t>    m_importedOutputBuiltIns;   // IDs of imported built-in outputs

    bool            m_hasPushConstOp;           // Whether push constant is active
    bool            m_hasDynIndexedInput;       // Whether dynamic indices are used in generic input addressing (valid
                                                // for tessellation shader, fragment shader with input interpolation)
    bool            m_hasDynIndexedOutput;      // Whether dynamic indices are used in generic output addressing (valid
                                                // for tessellation control shader)
    ResourceUsage*  m_pResUsage;                // Pointer to shader resource usage
};

} // Llpc
