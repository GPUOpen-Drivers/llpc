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
 * @file  llpcSpirvLowerImageOp.h
 * @brief LLPC header file: contains declaration of class Llpc::SpirvLowerImageOp.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/InstVisitor.h"

#include <unordered_set>
#include "llpcSpirvLower.h"

namespace Llpc
{

// =====================================================================================================================
// Represents the pass of SPIR-V lowering opertions for image operations (sample, fetch, gather, and read/write).
class SpirvLowerImageOp:
    public SpirvLower,
    public llvm::InstVisitor<SpirvLowerImageOp>
{
public:
    SpirvLowerImageOp();

    virtual bool runOnModule(llvm::Module& module);
    virtual void visitCallInst(llvm::CallInst& callInst);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    // F-mask mode used in handling image call
    enum FmaskMode
    {
        FmaskNone,  // No F-mask
        FmaskBased, // Texel fetching based on F-mask
        FmaskOnly   // Only return F-mask data
    };

    LLPC_DISALLOW_COPY_AND_ASSIGN(SpirvLowerImageOp);

    void ExtractBindingInfo(llvm::LoadInst*     pLoadInst,
                            llvm::ConstantInt** ppDescSet,
                            llvm::ConstantInt** ppBinding,
                            llvm::Value**       ppIndex,
                            llvm::ConstantInt** ppMemoryQualifier);

    FmaskMode GetFmaskMode(const ShaderImageCallMetadata&  imageCallMeta,
                           const std::string&              callName);

    void PatchImageCallForFmask(const ShaderImageCallMetadata&  imageCallMeta,
                                FmaskMode                       fmaskMode,
                                std::string&                    callName);

    llvm::Value* GetFragCoord(llvm::Instruction* pInsertPos);

    llvm::Value* GetViewIndex(llvm::Instruction* pInsertPos);

    llvm::Value* LoadTexelBufferDescriptor(uint32_t            descSet,
                                           uint32_t            binding,
                                           llvm::Value*        pBlockOffset,
                                           bool                isNonUniform,
                                           llvm::Instruction*  pInsertPos);

    llvm::Value* LoadResourceDescriptor(uint32_t            descSet,
                                        uint32_t            binding,
                                        llvm::Value*        pBlockOffset,
                                        bool                isNonUniform,
                                        llvm::Instruction*  pInsertPos);

    llvm::Value* LoadFmaskDescriptor(uint32_t            descSet,
                                     uint32_t            binding,
                                     llvm::Value*        pBlockOffset,
                                     bool                isNonUniform,
                                     llvm::Instruction*  pInsertPos);

    llvm::Value* LoadSamplerDescriptor(uint32_t            descSet,
                                       uint32_t            binding,
                                       llvm::Value*        pBlockOffset,
                                       bool                isNonUniform,
                                       llvm::Instruction*  pInsertPos);

    // -----------------------------------------------------------------------------------------------------------------

    std::unordered_set<llvm::CallInst*>    m_imageCalls;  // List of "call" instructions to emulate SPIR-V image operations
    std::unordered_set<llvm::Instruction*> m_imageLoads;  // List of "load" or "call" instructions to emulate SPIR-V image load
    std::unordered_set<llvm::Instruction*> m_imageLoadOperands; // List of instructions to emulate SPIR-V image load operands

    bool m_restoreMeta;                                   // Restore metadata from metadata instructions
};

} // Llpc
