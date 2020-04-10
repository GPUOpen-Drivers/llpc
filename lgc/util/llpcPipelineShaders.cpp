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
* @file  llpcPipelineShaders.cpp
* @brief LLPC source file: contains implementation of class Llpc::PipelineShaders
***********************************************************************************************************************
*/
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

#include "llpcInternal.h"
#include "llpcPipelineShaders.h"

#define DEBUG_TYPE "llpc-pipeline-shaders"

using namespace lgc;
using namespace llvm;

char PipelineShaders::ID = 0;

// =====================================================================================================================
// Create an instance of the pass.
ModulePass* createPipelineShaders()
{
    return new PipelineShaders();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// This populates the shader array. In the pipeline module, a shader entrypoint is a non-internal function definition,
// and it has metadata giving the SPIR-V execution model.
bool PipelineShaders::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Pipeline-Shaders\n");

    m_entryPointMap.clear();
    for (auto& entryPoint : m_entryPoints)
        entryPoint = nullptr;

    for (auto& func : module)
    {
        if ((func.empty() == false) && (func.getLinkage() != GlobalValue::InternalLinkage))
        {
            auto shaderStage = getShaderStageFromFunction(&func);

            if (shaderStage != ShaderStageInvalid)
            {
                m_entryPoints[shaderStage] = &func;
                m_entryPointMap[&func] = shaderStage;
            }
        }
    }
    return false;
}

// =====================================================================================================================
// Get the shader for a particular API shader stage, or nullptr if none
Function* PipelineShaders::getEntryPoint(
    ShaderStage shaderStage     // Shader stage
    ) const
{
    assert((unsigned)shaderStage < ShaderStageCountInternal);
    return m_entryPoints[shaderStage];
}

// =====================================================================================================================
// Get the ABI shader stage for a particular function, or ShaderStageInvalid if not a shader entrypoint.
ShaderStage PipelineShaders::getShaderStage(
    const Function* func  // [in] Function to look up
    ) const
{
    auto entryMapIt = m_entryPointMap.find(func);
    if (entryMapIt == m_entryPointMap.end())
        return ShaderStageInvalid;
    return entryMapIt->second;
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PipelineShaders, DEBUG_TYPE, "LLVM pass for getting pipeline shaders", false, true)
