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
 * @file  llpcPipelineShaders.h
 * @brief LLPC header file: contains declaration of class Llpc::PipelineShaders
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Pass.h"
#include <map>

#include "llpc.h"

namespace llvm
{

void initializePipelineShadersPass(PassRegistry&);

} // llvm

namespace Llpc
{

// =====================================================================================================================
// Simple analysis pass that finds the shaders in the pipeline module
class PipelineShaders : public llvm::ModulePass
{
public:
    static char ID;
    PipelineShaders() : ModulePass(ID)
    {
        initializePipelineShadersPass(*llvm::PassRegistry::getPassRegistry());
    }

    bool runOnModule(llvm::Module& module) override;

    void getAnalysisUsage(llvm::AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.setPreservesAll();
    }

    llvm::Function* GetEntryPoint(ShaderStage shaderStage) const;

    ShaderStage GetShaderStage(const llvm::Function* pFunc) const;

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PipelineShaders);

    // -----------------------------------------------------------------------------------------------------------------

    llvm::Function* m_entryPoints[ShaderStageCountInternal];      // The entry-point for each shader stage.
    std::map<const llvm::Function*, ShaderStage> m_entryPointMap; // Map from shader entry-point to shader stage.
};

llvm::ModulePass* CreatePipelineShaders();

} // Llpc
