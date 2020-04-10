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
 * @file  llpcPatchDescriptorLoad.h
 * @brief LLPC header file: contains declaration of class lgc::PatchDescriptorLoad.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/InstVisitor.h"

#include <unordered_set>
#include "llpcPatch.h"
#include "llpcPipelineShaders.h"
#include "llpcPipelineState.h"
#include "llpcSystemValues.h"

namespace lgc
{

// =====================================================================================================================
// Represents the pass of LLVM patching opertions for descriptor load.
class PatchDescriptorLoad:
    public Patch,
    public llvm::InstVisitor<PatchDescriptorLoad>
{
public:
    PatchDescriptorLoad();

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
    PatchDescriptorLoad(const PatchDescriptorLoad&) = delete;
    PatchDescriptorLoad& operator=(const PatchDescriptorLoad&) = delete;

    void processDescriptorGetPtr(llvm::CallInst* descPtrCall, llvm::StringRef descPtrCallName);
    llvm::Value* getDescPtrAndStride(ResourceNodeType        resType,
                                     unsigned                descSet,
                                     unsigned                binding,
                                     const ResourceNode*     topNode,
                                     const ResourceNode*     node,
                                     bool                    shadow,
                                     llvm::IRBuilder<>&      builder);
    llvm::Value* getDescPtr(ResourceNodeType resType,
                            unsigned                descSet,
                            unsigned                binding,
                            const ResourceNode*     topNode,
                            const ResourceNode*     node,
                            bool                    shadow,
                            llvm::IRBuilder<>&      builder);

    void processDescriptorIndex(llvm::CallInst* call);
    void processLoadDescFromPtr(llvm::CallInst* loadFromPtr);
    llvm::Value* loadBufferDescriptor(unsigned            descSet,
                                      unsigned            binding,
                                      llvm::Value*        arrayOffset,
                                      llvm::Instruction*  insertPoint);

    llvm::Value* buildInlineBufferDesc(llvm::Value* descPtr, llvm::IRBuilder<>& builder);
    llvm::Value* buildBufferCompactDesc(llvm::Value* desc, llvm::Instruction* insertPoint);

    // -----------------------------------------------------------------------------------------------------------------

    // Descriptor size
    static const unsigned  DescriptorSizeResource      = 8 * sizeof(unsigned);
    static const unsigned  DescriptorSizeSampler       = 4 * sizeof(unsigned);
    static const unsigned  DescriptorSizeBuffer        = 4 * sizeof(unsigned);
    static const unsigned  DescriptorSizeBufferCompact = 2 * sizeof(unsigned);

    bool                                m_changed;            // Whether the pass has modified the code
    PipelineSystemValues                m_pipelineSysValues;  // Cache of ShaderValues object per shader
    std::vector<llvm::CallInst*>        m_descLoadCalls;      // List of instructions to load descriptors
    std::unordered_set<llvm::Function*> m_descLoadFuncs;      // Set of descriptor load functions

    PipelineState*                  m_pipelineState = nullptr;
                                                              // Pipeline state from PipelineStateWrapper pass
};

} // lgc
