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
 * @file  llpcSpirvLowerTranslator.cpp
 * @brief LLPC source file: contains implementation of Llpc::SpirvLowerTranslator
 ***********************************************************************************************************************
*/
#include "lgc/llpcBuilder.h"
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
//
// @param stage : Shader stage
// @param shaderInfo : Shader info for this shader
ModulePass* Llpc::createSpirvLowerTranslator(
    ShaderStage                 stage,
    const PipelineShaderInfo*   shaderInfo)
{
    return new SpirvLowerTranslator(stage, shaderInfo);
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on (empty on entry)
bool SpirvLowerTranslator::runOnModule(
    llvm::Module& module)
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Translator\n");

    SpirvLower::init(&module);

#ifdef LLPC_ENABLE_SPIRV_OPT
    InitSpvGen();
#endif

    m_context = static_cast<Context*>(&module.getContext());

    // Translate SPIR-V binary to machine-independent LLVM module
    translateSpirvToLlvm(m_shaderInfo, &module);
    return true;
}

// =====================================================================================================================
// Translates SPIR-V binary to machine-independent LLVM module.
//
// @param shaderInfo : Specialization info
// @param [in/out] module : Module to translate into, initially empty
void SpirvLowerTranslator::translateSpirvToLlvm(
    const PipelineShaderInfo*   shaderInfo,
    Module*                     module)
{
    BinaryData  optimizedSpirvBin = {};
    const ShaderModuleData* moduleData = reinterpret_cast<const ShaderModuleData*>(shaderInfo->pModuleData);
    assert(moduleData->binType == BinaryType::Spirv);
    const BinaryData* spirvBin = &moduleData->binCode;
    if (ShaderModuleHelper::optimizeSpirv(spirvBin, &optimizedSpirvBin) == Result::Success)
        spirvBin = &optimizedSpirvBin;

    std::string spirvCode(static_cast<const char*>(spirvBin->pCode), spirvBin->codeSize);
    std::istringstream spirvStream(spirvCode);
    std::string errMsg;
    SPIRV::SPIRVSpecConstMap specConstMap;
    ShaderStage entryStage = shaderInfo->entryStage;
    // Build specialization constant map
    if (shaderInfo->pSpecializationInfo )
    {
        for (unsigned i = 0; i < shaderInfo->pSpecializationInfo->mapEntryCount; ++i)
        {
            SPIRV::SPIRVSpecConstEntry specConstEntry  = {};
            auto mapEntry = &shaderInfo->pSpecializationInfo->pMapEntries[i];
            specConstEntry.DataSize= mapEntry->size;
            specConstEntry.Data = voidPtrInc(shaderInfo->pSpecializationInfo->pData, mapEntry->offset);
            specConstMap[mapEntry->constantID] = specConstEntry;
        }
    }

    Context* context = static_cast<Context*>(&module->getContext());

    if (!readSpirv(context->getBuilder(),
                  &(moduleData->usage),
                  spirvStream,
                  convertToExecModel(entryStage),
                  shaderInfo->pEntryTarget,
                  specConstMap,
                  module,
                  errMsg))
    {
        report_fatal_error(Twine("Failed to translate SPIR-V to LLVM (") +
                           getShaderStageName(static_cast<ShaderStage>(entryStage)) + " shader): " +
                           errMsg,
                           false);
    }

    // Ensure the shader modes are recorded in IR metadata in the case that this is a shader compile
    // rather than a pipeline compile.
    m_context->getBuilder()->recordShaderModes(module);

    ShaderModuleHelper::cleanOptimizedSpirv(&optimizedSpirvBin);

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
    for (auto& func : *module)
    {
        if (func.empty())
            continue;

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
