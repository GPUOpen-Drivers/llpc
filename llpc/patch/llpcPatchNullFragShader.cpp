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
 * @file  llpcPatchNullFragShader.cpp
 * @brief LLPC source file: contains declaration and implementation of class Llpc::PatchNullFragShader.
 ***********************************************************************************************************************
 */
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#include "llpcBuilderContext.h"
#include "llpcDebug.h"
#include "llpcIntrinsDefs.h"
#include "llpcInternal.h"
#include "llpcPatch.h"
#include "llpcPipelineState.h"

#define DEBUG_TYPE "llpc-patch-null-frag-shader"

using namespace Llpc;
using namespace llvm;

namespace llvm
{

namespace cl
{

// -disable-null-frag-shader: disable to generate null fragment shader
opt<bool> DisableNullFragShader("disable-null-frag-shader",
                                cl::desc("Disable to add a null fragment shader"), cl::init(false));

} // cl

} // llvm

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
    }

    void getAnalysisUsage(llvm::AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineStateWrapper>();
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

    auto pipelineState = getAnalysis<PipelineStateWrapper>().GetPipelineState(&module);

    if (cl::DisableNullFragShader || pipelineState->GetBuilderContext()->BuildingRelocatableElf())
    {
        // NOTE: If the option -disable-null-frag-shader is set to TRUE, we skip this pass. This is done by
        // standalone compiler.
        return false;
    }

    PipelineState* pPipelineState = getAnalysis<PipelineStateWrapper>().GetPipelineState(&module);
    const bool hasCs = pPipelineState->HasShaderStage(ShaderStageCompute);
    const bool hasVs = pPipelineState->HasShaderStage(ShaderStageVertex);
    const bool hasTes = pPipelineState->HasShaderStage(ShaderStageTessEval);
    const bool hasGs = pPipelineState->HasShaderStage(ShaderStageGeometry);
    const bool hasFs = pPipelineState->HasShaderStage(ShaderStageFragment);
    if (hasCs || hasFs || ((hasVs == false) && (hasTes == false) && (hasGs == false)))
    {
        // This is an incomplete graphics pipeline from the amdllpc command-line tool, or a compute pipeline, or a
        // graphics pipeline that already has a fragment shader. A null fragment shader is not required.
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
    auto pEntryPointTy = FunctionType::get(Type::getVoidTy(*m_pContext), ArrayRef<Type*>(), false);

    // Create function for the null fragment shader entrypoint.
    auto pEntryPoint = Function::Create(pEntryPointTy,
                                        GlobalValue::ExternalLinkage,
                                        LlpcName::NullFsEntryPoint,
                                        &module);

    // Create its basic block, and terminate it with return.
    auto pBlock = BasicBlock::Create(*m_pContext, "", pEntryPoint, nullptr);
    auto pInsertPos = ReturnInst::Create(*m_pContext, pBlock);

    // Add its code. First the import.
    auto pZero = ConstantInt::get(Type::getInt32Ty(*m_pContext), 0);
    auto pOne = ConstantInt::get(Type::getInt32Ty(*m_pContext), 1);
    Value* importArgs[] = { pZero, pZero, pZero, pOne };
    auto pInputTy = Type::getFloatTy(*m_pContext);
    std::string importName = LlpcName::InputImportGeneric;
    AddTypeMangling(pInputTy, importArgs, importName);
    auto pInput = EmitCall(importName, pInputTy, importArgs, NoAttrib, pInsertPos);

    // Then the export.
    Value* exportArgs[] = { pZero, pZero, pInput };
    std::string exportName = LlpcName::OutputExportGeneric;
    AddTypeMangling(Type::getVoidTy(*m_pContext), exportArgs, exportName);
    EmitCall(exportName, Type::getVoidTy(*m_pContext), exportArgs, NoAttrib, pInsertPos);

    // Add execution model metadata to the function.
    auto pExecModelMeta = ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*m_pContext), ShaderStageFragment));
    auto pExecModelMetaNode = MDNode::get(*m_pContext, pExecModelMeta);
    pEntryPoint->addMetadata(LlpcName::ShaderStageMetadata, *pExecModelMetaNode);

    // Initialize shader info.
    auto pResUsage = pPipelineState->GetShaderResourceUsage(ShaderStageFragment);
    pPipelineState->SetShaderStageMask(pPipelineState->GetShaderStageMask() | ShaderStageToMask(ShaderStageFragment));

    // Add usage info for dummy input
    FsInterpInfo interpInfo = { 0, false, false, false };
    pResUsage->builtInUsage.fs.smooth = true;
    pResUsage->inOutUsage.inputLocMap[0] = InvalidValue;
    pResUsage->inOutUsage.fs.interpInfo.push_back(interpInfo);

    // Add usage info for dummy output
    pResUsage->inOutUsage.fs.cbShaderMask = 0;
    pResUsage->inOutUsage.fs.dummyExport = true;
    pResUsage->inOutUsage.fs.isNullFs = true;
    pResUsage->inOutUsage.outputLocMap[0] = InvalidValue;

    return true;
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PatchNullFragShader, DEBUG_TYPE, "Patch LLVM for null fragment shader generation", false, false)
