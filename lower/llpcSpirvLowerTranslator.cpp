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

#include "LLVMSPIRVLib.h"
#include <sstream>
#include <string>

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
    TranslateSpirvToLlvm(m_pShaderInfo, &module);
    return true;
}

// =====================================================================================================================
// Translates SPIR-V binary to machine-independent LLVM module.
void SpirvLowerTranslator::TranslateSpirvToLlvm(
    const PipelineShaderInfo*   pShaderInfo,         // [in] Specialization info
    Module*                     pModule)             // [in/out] Module to translate into, initially empty
{
    BinaryData  optimizedSpirvBin = {};
    const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);
    LLPC_ASSERT(pModuleData->binType == BinaryType::Spirv);
    const BinaryData* pSpirvBin = &pModuleData->binCode;
    if (ShaderModuleHelper::OptimizeSpirv(pSpirvBin, &optimizedSpirvBin) == Result::Success)
    {
        pSpirvBin = &optimizedSpirvBin;
    }

    std::string spirvCode(static_cast<const char*>(pSpirvBin->pCode), pSpirvBin->codeSize);
    std::istringstream spirvStream(spirvCode);
    std::string errMsg;
    SPIRV::SPIRVSpecConstMap specConstMap;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 21
    ShaderStage entryStage = pShaderInfo->entryStage;
#else
    ShaderStage entryStage = ShaderStageInvalid;
#endif
    // Build specialization constant map
    if (pShaderInfo->pSpecializationInfo != nullptr)
    {
        for (uint32_t i = 0; i < pShaderInfo->pSpecializationInfo->mapEntryCount; ++i)
        {
            SPIRV::SPIRVSpecConstEntry specConstEntry  = {};
            auto pMapEntry = &pShaderInfo->pSpecializationInfo->pMapEntries[i];
            specConstEntry.DataSize= pMapEntry->size;
            specConstEntry.Data = VoidPtrInc(pShaderInfo->pSpecializationInfo->pData, pMapEntry->offset);
            specConstMap[pMapEntry->constantID] = specConstEntry;
        }
    }

    Context* pContext = static_cast<Context*>(&pModule->getContext());

    if (readSpirv(pContext->GetBuilder(),
                  pShaderInfo->pModuleData,
                  spirvStream,
                  ConvertToExecModel(entryStage),
                  pShaderInfo->pEntryTarget,
                  specConstMap,
                  pModule,
                  errMsg) == false)
    {
        report_fatal_error(Twine("Failed to translate SPIR-V to LLVM (") +
                           GetShaderStageName(static_cast<ShaderStage>(entryStage)) + " shader): " +
                           errMsg,
                           false);
    }

    ShaderModuleHelper::CleanOptimizedSpirv(&optimizedSpirvBin);

    // NOTE: Our shader entrypoint is marked in the SPIR-V reader as dllexport. Here we mark it as follows:
    //   * remove the dllexport;
    //   * ensure it is public.
    // Also mark all other functions internal and always_inline.
    //
    // TODO: We should rationalize this code as follows:
    //   1. Add code to the spir-v reader to add the entrypoint name as metadata;
    //   2. change this code here to detect that, instead of DLLExport;
    //   3. remove the code we added to the spir-v reader to detect the required entrypoint and mark it as DLLExport;
    //   4. remove the required entrypoint name and execution model args that we added to the spir-v reader API, to
    //      make it closer to the upstream Khronos copy of that code.
    for (auto& func : *pModule)
    {
        if (func.empty())
        {
            continue;
        }

        if (func.getDLLStorageClass() == GlobalValue::DLLExportStorageClass)
        {
            func.setDLLStorageClass(GlobalValue::DefaultStorageClass);
            func.setLinkage(GlobalValue::ExternalLinkage);
        }
        else
        {
            func.setLinkage(GlobalValue::InternalLinkage);
            func.addFnAttr(Attribute::AlwaysInline);
        }
    }
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(SpirvLowerTranslator, DEBUG_TYPE, "LLPC translate SPIR-V binary to LLVM IR", false, false)

