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
* @file  llpcSpirvLowerTranslator.cpp
* @brief LLPC source file: contains implementation of Llpc::SpirvLowerTranslator
***********************************************************************************************************************
*/
#include "llpcCompiler.h"
#include "llpcContext.h"
#include "llpcSpirvLowerTranslator.h"

#define DEBUG_TYPE "llpc-spirv-lower-translator"

using namespace llvm;
using namespace Llpc;

char SpirvLowerTranslator::ID = 0;

// =====================================================================================================================
// Creates the pass of translating SPIR-V to LLVM IR.
ModulePass* Llpc::CreateSpirvLowerTranslator(
    ShaderStage                 stage,        // Shader stage
    const PipelineShaderInfo*   pShaderInfo)  // [in] Shader info for this shader
{
    return new SpirvLowerTranslator(stage, pShaderInfo);
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
bool SpirvLowerTranslator::runOnModule(
    llvm::Module& module)  // [in,out] LLVM module to be run on (empty on entry)
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Translator\n");

    SpirvLower::Init(&module);

#ifdef LLPC_ENABLE_SPIRV_OPT
    InitSpvGen();
#endif

    m_pContext = static_cast<Context*>(&module.getContext());

    // Translate SPIR-V binary to machine-independent LLVM module
    Compiler::TranslateSpirvToLlvm(m_pShaderInfo,
                                   &module);
    return true;
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(SpirvLowerTranslator, DEBUG_TYPE, "LLPC translate SPIR-V binary to LLVM IR", false, false)

