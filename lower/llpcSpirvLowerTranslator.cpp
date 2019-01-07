/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Debug.h"

#include <sstream>

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
    ArrayRef<const PipelineShaderInfo*> shaderInfo) // Shader info array
{
    return new SpirvLowerTranslator(shaderInfo);
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
    Linker linker(module);

    // Translate SPIR-V binary to machine-independent LLVM module
    for (uint32_t stage = 0; stage < m_shaderInfo.size(); ++stage)
    {
        const PipelineShaderInfo* pShaderInfo = m_shaderInfo[stage];
        if ((pShaderInfo == nullptr) || (pShaderInfo->pModuleData == nullptr))
        {
            continue;
        }

        const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);
        LLPC_ASSERT(pModuleData->binType == BinaryType::Spirv);

        Module* pShaderModule = Compiler::TranslateSpirvToLlvm(&pModuleData->binCode,
                                                               static_cast<ShaderStage>(stage),
                                                               pShaderInfo->pEntryTarget,
                                                               pShaderInfo->pSpecializationInfo,
                                                               m_pContext);

        if (pShaderModule == nullptr)
        {
            continue;
        }

        // Ensure the name of the shader entrypoint of this shader does not clash with any other, by qualifying
        // the name with the shader stage.
        Function* pEntryPoint = nullptr;
        for (auto& func : *pShaderModule)
        {
            if ((func.empty() == false) && (func.getLinkage() != GlobalValue::InternalLinkage))
            {
                pEntryPoint = &func;
                func.setName(Twine(LlpcName::EntryPointPrefix) +
                             GetShaderStageAbbreviation(ShaderStage(stage), true) +
                             "." +
                             func.getName());
            }
        }
        // Ensure the names of globals do not clash with other shader stages.
        // Also implement any global initializer with a store at the shader entrypoint, to avoid a problem
        // where the linking process removes a shader export global having an initializer but not otherwise referenced.
        for (auto& global : pShaderModule->globals())
        {
            if ((global.getLinkage() != GlobalValue::InternalLinkage) &&
                  (global.getLinkage() != GlobalValue::PrivateLinkage))
            {
                global.setName(
                    Twine(GetShaderStageAbbreviation(ShaderStage(stage), true)) + "_" + global.getName());
            }
            if (global.hasInitializer())
            {
                auto pInitializer = global.getInitializer();
                if (isa<UndefValue>(pInitializer) == false)
                {
                    new StoreInst(pInitializer, &global, &*pEntryPoint->begin()->getFirstInsertionPt());
                    global.setInitializer(UndefValue::get(pInitializer->getType()));
                }
            }
        }

        // Link shader module into pipeline module.
        // NOTE: We use unique_ptr here. The shader module will be destroyed after it is
        // linked into pipeline module.
        if (linker.linkInModule(std::unique_ptr<Module>(pShaderModule)))
        {
            report_fatal_error(Twine("Failed to link shader module into pipeline module (") +
                                 GetShaderStageName(ShaderStage(stage)) + " shader)",
                               false);
        }
    }

    return true;
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(SpirvLowerTranslator, DEBUG_TYPE, "LLPC translate SPIR-V binary to LLVM IR", false, false)

