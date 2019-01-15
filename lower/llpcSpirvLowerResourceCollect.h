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
 * @file  llpcSpirvLowerResourceCollect.h
 * @brief LLPC header file: contains declaration of class Llpc::SpirvLowerResourceCollect.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/InstVisitor.h"

#include "SPIRVInternal.h"
#include "llpc.h"
#include "llpcSpirvLower.h"

namespace Llpc
{

struct DescriptorBinding;
class PipelineShaders;
struct ResourceUsage;

// =====================================================================================================================
// Represents the pass of SPIR-V lowering opertions for resource collecting.
class SpirvLowerResourceCollect:
    public SpirvLower,
    public llvm::InstVisitor<SpirvLowerResourceCollect>
{
public:
    SpirvLowerResourceCollect();

    void getAnalysisUsage(AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineShaders>();
        analysisUsage.addPreserved<PipelineShaders>();
        analysisUsage.addRequired<CallGraphWrapperPass>();
        analysisUsage.addPreserved<CallGraphWrapperPass>();
    }

    virtual bool runOnModule(llvm::Module& module) override;

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(SpirvLowerResourceCollect);

    uint32_t GetFlattenArrayElementCount(const llvm::Type* pTy) const;
    const llvm::Type* GetFlattenArrayElementType(const llvm::Type* pTy) const;

    void CollectExecutionModeUsage(ShaderStage shaderStage);
    void CollectDescriptorUsage(ShaderStage               shaderStage,
                                uint32_t                  descSet,
                                uint32_t                  binding,
                                const DescriptorBinding*  pBinding);
    void CollectInOutUsage(ShaderStage        shaderStage,
                           const llvm::Type*  pInOutTy,
                           llvm::Constant*    pInOutMeta,
                           SPIRAddressSpace   addrSpace);
    void CollectVertexInputUsage(const llvm::Type* pVertexTy, bool signedness, uint32_t startLoc, uint32_t locCount);
    void CollectGsOutputInfo(const Type* pOutputTy, uint32_t location, uint32_t locOffset, const ShaderInOutMetadata& outputMeta);
    void CollectXfbOutputInfo(ShaderStage  shaderStage, const llvm::Type* pOutputTy, const ShaderInOutMetadata& inOutMeta);

    uint32_t GetGlobalShaderUse(GlobalValue* pGlobal);
    void SetFunctionShaderUse();

    // -----------------------------------------------------------------------------------------------------------------

    PipelineShaders*                        m_pPipelineShaders;   // Pipeline shaders analysis
    std::unordered_map<Function*, uint32_t> m_funcShaderUseMap;   // Map of which shader(s) use a function
};

} // Llpc
