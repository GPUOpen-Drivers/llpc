/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilder.cpp
 * @brief LLPC source file: implementation of Llpc::Builder
 ***********************************************************************************************************************
 */
#include "llpcBuilderImpl.h"
#include "llpcContext.h"
#include "llpcInternal.h"

#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"

#include <set>

#define DEBUG_TYPE "llpc-builder"

using namespace Llpc;
using namespace llvm;

// -use-builder-recorder
static cl::opt<uint32_t> UseBuilderRecorder("use-builder-recorder",
                                            cl::desc("Do lowering via recording and replaying LLPC builder:\n"
                                                     "0: Generate IR directly; no recording\n"
                                                     "1: Do lowering via recording and replaying LLPC builder (default)\n"
                                                     "2: Do lowering via recording; no replaying"),
                                            cl::init(1));

// =====================================================================================================================
// Create a Builder object
// If -use-builder-recorder is 0, this creates a BuilderImpl. Otherwise, it creates a BuilderRecorder.
Builder* Builder::Create(
    LLVMContext& context) // [in] LLVM context
{
    if (UseBuilderRecorder == 0)
    {
        // -use-builder-recorder=0: generate LLVM IR directly without recording
        return CreateBuilderImpl(context);
    }
    // -use-builder-recorder=1: record with BuilderRecorder and replay with BuilderReplayer
    // -use-builder-recorder=2: record with BuilderRecorder and do not replay
    return CreateBuilderRecorder(context, UseBuilderRecorder == 1 /*wantReplay*/);
}

// =====================================================================================================================
// Create a BuilderImpl object
Builder* Builder::CreateBuilderImpl(
    LLVMContext& context) // [in] LLVM context
{
    return new BuilderImpl(context);
}

// =====================================================================================================================
Builder::~Builder()
{
}

// =====================================================================================================================
// Base implementation of linking shader modules into a pipeline module.
Module* Builder::Link(
    ArrayRef<Module*> modules)     // Array of modules indexed by shader stage, with nullptr entry
                                   //  for any stage not present in the pipeline
{
    // Add IR metadata for the shader stage to each function in each shader, and rename the entrypoint to
    // ensure there is no clash on linking.
    uint32_t metaKindId = getContext().getMDKindID(LlpcName::ShaderStageMetadata);
    for (uint32_t stage = 0; stage < ShaderStageCount; ++stage)
    {
        Module* pModule = modules[stage];
        if (pModule == nullptr)
        {
            continue;
        }

        auto pStageMetaNode = MDNode::get(getContext(), { ConstantAsMetadata::get(getInt32(stage)) });
        for (Function& func : *pModule)
        {
            if (func.isDeclaration() == false)
            {
                func.setMetadata(metaKindId, pStageMetaNode);
                if (func.getLinkage() != GlobalValue::InternalLinkage)
                {
                    func.setName(Twine(LlpcName::EntryPointPrefix) +
                                 GetShaderStageAbbreviation(static_cast<ShaderStage>(stage), true) +
                                 "." +
                                 func.getName());
                }
            }
        }
    }

    // If there is only one shader, just change the name on its module and return it.
    Module* pPipelineModule = nullptr;
    for (auto pModule : modules)
    {
        if (pPipelineModule == nullptr)
        {
            pPipelineModule = pModule;
        }
        else if (pModule != nullptr)
        {
            pPipelineModule = nullptr;
            break;
        }
    }

    if (pPipelineModule != nullptr)
    {
        pPipelineModule->setModuleIdentifier("llpcPipeline");
    }
    else
    {
        // Create an empty module then link each shader module into it.
        bool result = true;
        pPipelineModule = new Module("llpcPipeline", getContext());
        static_cast<Llpc::Context*>(&getContext())->SetModuleTargetMachine(pPipelineModule);
        Linker linker(*pPipelineModule);

        for (int32_t stage = 0; stage < ShaderStageCount; ++stage)
        {
            if (modules[stage] != nullptr)
            {
                // NOTE: We use unique_ptr here. The shader module will be destroyed after it is
                // linked into pipeline module.
                if (linker.linkInModule(std::unique_ptr<Module>(modules[stage])))
                {
                    result = false;
                }
            }
        }

        if (result == false)
        {
            delete pPipelineModule;
            pPipelineModule = nullptr;
        }
    }

    return pPipelineModule;
}

