/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPatch.h
 * @brief LLPC header file: contains declaration of class Llpc::Patch.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/Pass.h"

#include "llpc.h"
#include "llpcDebug.h"
#include "llpcPipeline.h"

namespace llvm
{

class CallInst;
class PassRegistry;

namespace legacy
{

class PassManager;

} // legacy

void initializePatchBufferOpPass(PassRegistry&);
void initializePatchCheckShaderCachePass(PassRegistry&);
void initializePatchCopyShaderPass(PassRegistry&);
void initializePatchDescriptorLoadPass(PassRegistry&);
void initializePatchEntryPointMutatePass(PassRegistry&);
void initializePatchInOutImportExportPass(PassRegistry&);
void initializePatchIntrinsicSimplifyPass(PassRegistry&);
void initializePatchLlvmIrInclusionPass(PassRegistry&);
void initializePatchLoadScalarizerPass(PassRegistry&);
void initializePatchNullFragShaderPass(PassRegistry&);
void initializePatchPeepholeOptPass(PassRegistry&);
void initializePatchPreparePipelineAbiPass(PassRegistry&);
void initializePatchPushConstOpPass(PassRegistry&);
void initializePatchResourceCollectPass(PassRegistry&);
void initializePatchSetupTargetFeaturesPass(PassRegistry&);

} // llvm

namespace Llpc
{

using namespace llvm;

class PatchCheckShaderCache;

// Initialize passes for patching
inline static void InitializePatchPasses(
    llvm::PassRegistry& passRegistry)   // Pass registry
{
  initializePatchBufferOpPass(passRegistry);
  initializePatchCheckShaderCachePass(passRegistry);
  initializePatchCopyShaderPass(passRegistry);
  initializePatchDescriptorLoadPass(passRegistry);
  initializePatchEntryPointMutatePass(passRegistry);
  initializePatchInOutImportExportPass(passRegistry);
  initializePatchIntrinsicSimplifyPass(passRegistry);
  initializePatchLlvmIrInclusionPass(passRegistry);
  initializePatchLoadScalarizerPass(passRegistry);
  initializePatchNullFragShaderPass(passRegistry);
  initializePatchPeepholeOptPass(passRegistry);
  initializePatchPreparePipelineAbiPass(passRegistry);
  initializePatchPushConstOpPass(passRegistry);
  initializePatchResourceCollectPass(passRegistry);
  initializePatchSetupTargetFeaturesPass(passRegistry);
}

llvm::FunctionPass* CreatePatchBufferOp();
PatchCheckShaderCache* CreatePatchCheckShaderCache();
llvm::ModulePass* CreatePatchCopyShader();
llvm::ModulePass* CreatePatchDescriptorLoad();
llvm::ModulePass* CreatePatchEntryPointMutate();
llvm::ModulePass* CreatePatchInOutImportExport();
llvm::FunctionPass* CreatePatchIntrinsicSimplify();
llvm::ModulePass* CreatePatchLlvmIrInclusion();
llvm::FunctionPass* CreatePatchLoadScalarizer();
llvm::ModulePass* CreatePatchNullFragShader();
llvm::FunctionPass* CreatePatchPeepholeOpt(bool enableDiscardOpt = false);
llvm::ModulePass* CreatePatchPreparePipelineAbi(bool onlySetCallingConvs);
llvm::ModulePass* CreatePatchPushConstOp();
llvm::ModulePass* CreatePatchResourceCollect();
llvm::ModulePass* CreatePatchSetupTargetFeatures();

class PipelineState;

// =====================================================================================================================
// Represents the pass of LLVM patching operations, as the base class.
class Patch: public llvm::ModulePass
{
public:
    explicit Patch(char& Pid)
        :
        llvm::ModulePass(Pid),
        m_pModule(nullptr),
        m_pContext(nullptr),
        m_shaderStage(ShaderStageInvalid),
        m_pEntryPoint(nullptr)
    {
    }
    virtual ~Patch() {}

    static void AddPasses(PipelineState*                  pPipelineState,
                          llvm::legacy::PassManager&      passMgr,
                          llvm::ModulePass*               pReplayerPass,
                          llvm::Timer*                    pPatchTimer,
                          llvm::Timer*                    pOptTimer,
                          Pipeline::CheckShaderCacheFunc  checkShaderCacheFunc);

    static llvm::GlobalVariable* GetLdsVariable(PipelineState* pPipelineState, llvm::Module* pModule);

protected:
    void Init(llvm::Module* pModule);

    // -----------------------------------------------------------------------------------------------------------------

    llvm::Module*   m_pModule;      // LLVM module to be run on
    llvm::LLVMContext*        m_pContext;     // Associated LLVM context of the LLVM module that passes run on
    ShaderStage     m_shaderStage;  // Shader stage
    llvm::Function* m_pEntryPoint;  // Entry-point

private:
    static void AddOptimizationPasses(llvm::legacy::PassManager& passMgr);

    Patch() = delete;
    Patch(const Patch&) = delete;
    Patch& operator=(const Patch&) = delete;
};
} // Llpc
