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
 * @file  llpcPatchNullFragShader.cpp
 * @brief LLPC source file: contains declaration and implementation of class Llpc::PatchNullFragShader.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-patch-null-frag-shader"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcGraphicsContext.h"
#include "llpcIntrinsDefs.h"
#include "llpcInternal.h"
#include "llpcPatch.h"

using namespace Llpc;
using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
// Pass to generate null fragment shader if required
class PatchNullFragShader : public Patch
{
public:
    static char ID;
    PatchNullFragShader() : Patch(ID)
    {
        initializePatchNullFragShaderPass(*PassRegistry::getPassRegistry());
    }

    bool runOnModule(Module& module) override;

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PatchNullFragShader);
};

char PatchNullFragShader::ID = 0;

} // Llpc

// =====================================================================================================================
// Create the pass that generates a null fragment shader if required.
ModulePass* Llpc::CreatePatchNullFragShader()
{
    return new PatchNullFragShader();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
bool PatchNullFragShader::runOnModule(
    llvm::Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Null-Frag-Shader\n");

    Patch::Init(&module);

    const bool hasCs = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageCompute)) != 0);
    const bool hasFs = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageFragment)) != 0);
    if (m_pContext->NeedAutoLayoutDesc() || hasCs || hasFs)
    {
        // This is a non-pipeline compile, or a compute pipeline, or a graphics pipeline that already has a
        // fragment shader. A null fragment shader is not required.
        return false;
    }

    // Create the null fragment shader:
    // define void @llpc.shader.FS.null() !spirv.ExecutionModel !5
    // {
    // .entry:
    //     %0 = tail call float @llpc.input.import.generic.f32(i32 0, i32 0, i32 0, i32 1)
    //     tail call void @llpc.output.export.generic.f32(i32 0, i32 0, float %0)
    //     ret void
    // }

    // Create type of new function: void()
    auto pEntryPointTy = FunctionType::get(m_pContext->VoidTy(), ArrayRef<Type*>(), false);

    // Create function for the null fragment shader entrypoint.
    auto pEntryPoint = Function::Create(pEntryPointTy,
                                        GlobalValue::ExternalLinkage,
                                        LlpcName::NullFsEntryPoint,
                                        &module);

    // Create its basic block, and terminate it with return.
    auto pBlock = BasicBlock::Create(*m_pContext, "", pEntryPoint, nullptr);
    auto pInsertPos = ReturnInst::Create(*m_pContext, pBlock);

    // Add its code. First the import.
    auto pZero = ConstantInt::get(m_pContext->Int32Ty(), 0);
    auto pOne = ConstantInt::get(m_pContext->Int32Ty(), 1);
    Value* importArgs[] = { pZero, pZero, pZero, pOne };
    auto pInputTy = m_pContext->FloatTy();
    std::string importName = LlpcName::InputImportGeneric;
    AddTypeMangling(pInputTy, importArgs, importName);
    auto pInput = EmitCall(&module, importName, pInputTy, importArgs, NoAttrib, pInsertPos);

    // Then the export.
    Value* exportArgs[] = { pZero, pZero, pInput };
    std::string exportName = LlpcName::OutputExportGeneric;
    AddTypeMangling(m_pContext->VoidTy(), exportArgs, exportName);
    EmitCall(&module, exportName, m_pContext->VoidTy(), exportArgs, NoAttrib, pInsertPos);

    // Add SPIR-V execution model metadata to the function.
    auto pExecModelMeta = ConstantAsMetadata::get(ConstantInt::get(m_pContext->Int32Ty(), ExecutionModelFragment));
    auto pExecModelMetaNode = MDNode::get(*m_pContext, pExecModelMeta);
    pEntryPoint->addMetadata(gSPIRVMD::ExecutionModel, *pExecModelMetaNode);

    // Initialize shader info.
    GraphicsContext* pGraphicsContext = static_cast<GraphicsContext*>(m_pContext->GetPipelineContext());
    pGraphicsContext->InitShaderInfoForNullFs();

    return true;
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PatchNullFragShader, DEBUG_TYPE, "Patch LLVM for null fragment shader generation", false, false)

