/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLower.h
 * @brief LLPC header file: contains declaration of class Llpc::SpirvLower.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/Pass.h"

#include "llpc.h"
#include "llpcUtil.h"

namespace llvm
{

class Constant;
class GlobalVariable;
class Timer;

namespace legacy
{

class PassManager;

} // legacy

class PassRegistry;
void initializeSpirvLowerAccessChainPass(PassRegistry&);
void initializeSpirvLowerAlgebraTransformPass(PassRegistry&);
void initializeSpirvLowerConstImmediateStorePass(PassRegistry&);
void initializeSpirvLowerMemoryOpPass(PassRegistry&);
void initializeSpirvLowerGlobalPass(PassRegistry&);
void initializeSpirvLowerInstMetaRemovePass(PassRegistry&);
void initializeSpirvLowerLoopUnrollControlPass(PassRegistry&);
void initializeSpirvLowerResourceCollectPass(PassRegistry&);
void initializeSpirvLowerTranslatorPass(PassRegistry&);
} // llvm

namespace lgc
{

class Builder;

} // lgc

namespace Llpc
{

// Initialize passes for SPIR-V lowering
inline static void InitializeLowerPasses(
    llvm::PassRegistry& passRegistry)   // Pass registry
{
    initializeSpirvLowerAccessChainPass(passRegistry);
    initializeSpirvLowerAlgebraTransformPass(passRegistry);
    initializeSpirvLowerConstImmediateStorePass(passRegistry);
    initializeSpirvLowerMemoryOpPass(passRegistry);
    initializeSpirvLowerGlobalPass(passRegistry);
    initializeSpirvLowerInstMetaRemovePass(passRegistry);
    initializeSpirvLowerLoopUnrollControlPass(passRegistry);
    initializeSpirvLowerResourceCollectPass(passRegistry);
    initializeSpirvLowerTranslatorPass(passRegistry);
}

class Context;

llvm::ModulePass* CreateSpirvLowerAccessChain();
llvm::ModulePass* CreateSpirvLowerAlgebraTransform(bool enableConstFolding, bool enableFloatOpt);
llvm::ModulePass* CreateSpirvLowerConstImmediateStore();
llvm::ModulePass* CreateSpirvLowerMemoryOp();
llvm::ModulePass* CreateSpirvLowerGlobal();
llvm::ModulePass* CreateSpirvLowerInstMetaRemove();
llvm::ModulePass* CreateSpirvLowerLoopUnrollControl(uint32_t forceLoopUnrollCount);
llvm::ModulePass* CreateSpirvLowerResourceCollect(bool collectDetailUsage);
llvm::ModulePass* CreateSpirvLowerTranslator(ShaderStage stage, const PipelineShaderInfo* pShaderInfo);

// =====================================================================================================================
// Represents the pass of SPIR-V lowering operations, as the base class.
class SpirvLower: public llvm::ModulePass
{
public:
    explicit SpirvLower(char& Pid)
        :
        llvm::ModulePass(Pid),
        m_pModule(nullptr),
        m_pContext(nullptr),
        m_shaderStage(ShaderStageInvalid),
        m_pEntryPoint(nullptr)
    {
    }

    // Add per-shader lowering passes to pass manager
    static void AddPasses(Context*                    pContext,
                          ShaderStage                 stage,
                          llvm::legacy::PassManager&  passMgr,
                          llvm::Timer*                pLowerTimer,
                          uint32_t                    forceLoopUnrollCount);

    static void RemoveConstantExpr(Context* pContext, llvm::GlobalVariable* pGlobal);
    static void ReplaceConstWithInsts(Context* pContext, llvm::Constant* const pConstVal);

protected:
    void Init(llvm::Module* pModule);

    // -----------------------------------------------------------------------------------------------------------------

    llvm::Module*   m_pModule;      // LLVM module to be run on
    Context*        m_pContext;     // Associated LLPC context of the LLVM module that passes run on
    ShaderStage     m_shaderStage;  // Shader stage
    llvm::Function* m_pEntryPoint;  // Entry point of input module
    lgc::Builder*   m_pBuilder;     // LGC builder object

private:
    SpirvLower() = delete;
    SpirvLower(const SpirvLower&) = delete;
    SpirvLower& operator=(const SpirvLower&) = delete;
};

} // Llpc
