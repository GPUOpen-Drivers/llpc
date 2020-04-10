/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerUtil.cpp
 * @brief LLPC source file: utilities for use by LLPC front-end
 ***********************************************************************************************************************
 */

#include "llpcSpirvLower.h"
#include "llpcSpirvLowerUtil.h"
#include "llpcUtil.h"
#include "SPIRVInternal.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
// Gets the entry point (valid for AMD GPU) of a LLVM module.
Function* getEntryPoint(
    Module* module) // [in] LLVM module
{
    Function* entryPoint = nullptr;

    for (auto func = module->begin(), end = module->end(); func != end; ++func)
    {
        if (!func->empty() && func->getLinkage() == GlobalValue::ExternalLinkage)
        {
            entryPoint = &*func;
            break;
        }
    }

    assert(entryPoint );
    return entryPoint;
}

// =====================================================================================================================
// Gets the shader stage from the specified single-shader LLVM module.
ShaderStage getShaderStageFromModule(
    Module* module)  // [in] LLVM module
{
    Function* func = getEntryPoint(module);

    // Check for the execution model metadata that is added by the SPIR-V reader.
    MDNode* execModelNode = func->getMetadata(gSPIRVMD::ExecutionModel);
    if (!execModelNode )
        return ShaderStageInvalid;
    auto execModel = mdconst::dyn_extract<ConstantInt>(execModelNode->getOperand(0))->getZExtValue();
    return convertToStageShage(execModel);
}

// =====================================================================================================================
// Set the shader stage to the specified LLVM module entry function.
void setShaderStageToModule(
    Module*     module,        // [in] LLVM module to set shader stage
    ShaderStage shaderStage)    // Shader stage
{
    LLVMContext& context = module->getContext();
    Function* func = getEntryPoint(module);
    auto execModel = convertToExecModel(shaderStage);
    Metadata* execModelMeta[] = {
        ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), execModel))
    };
    auto execModelMetaNode = MDNode::get(context, execModelMeta);
    func->setMetadata(gSPIRVMD::ExecutionModel, execModelMetaNode);
}

} // Llpc
