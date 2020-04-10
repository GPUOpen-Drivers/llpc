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
//
// @param passRegistry : Pass registry
inline static void initializeLowerPasses(
    llvm::PassRegistry& passRegistry)
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

llvm::ModulePass* createSpirvLowerAccessChain();
llvm::ModulePass* createSpirvLowerAlgebraTransform(bool enableConstFolding, bool enableFloatOpt);
llvm::ModulePass* createSpirvLowerConstImmediateStore();
llvm::ModulePass* createSpirvLowerMemoryOp();
llvm::ModulePass* createSpirvLowerGlobal();
llvm::ModulePass* createSpirvLowerInstMetaRemove();
llvm::ModulePass* createSpirvLowerLoopUnrollControl(unsigned forceLoopUnrollCount);
llvm::ModulePass* createSpirvLowerResourceCollect(bool collectDetailUsage);
llvm::ModulePass* createSpirvLowerTranslator(ShaderStage stage, const PipelineShaderInfo* shaderInfo);

// =====================================================================================================================
// Represents the pass of SPIR-V lowering operations, as the base class.
class SpirvLower: public llvm::ModulePass
{
public:
    explicit SpirvLower(char& pid)
        :
        llvm::ModulePass(pid),
        m_module(nullptr),
        m_context(nullptr),
        m_shaderStage(ShaderStageInvalid),
        m_entryPoint(nullptr)
    {
    }

    // Add per-shader lowering passes to pass manager
    static void addPasses(Context*                    context,
                          ShaderStage                 stage,
                          llvm::legacy::PassManager&  passMgr,
                          llvm::Timer*                lowerTimer,
                          unsigned                    forceLoopUnrollCount);

    static void removeConstantExpr(Context* context, llvm::GlobalVariable* global);
    static void replaceConstWithInsts(Context* context, llvm::Constant* const constVal);

protected:
    void init(llvm::Module* module);

    // -----------------------------------------------------------------------------------------------------------------

    llvm::Module*   m_module;      // LLVM module to be run on
    Context*        m_context;     // Associated LLPC context of the LLVM module that passes run on
    ShaderStage     m_shaderStage;  // Shader stage
    llvm::Function* m_entryPoint;  // Entry point of input module
    lgc::Builder*   m_builder;     // LGC builder object

private:
    SpirvLower() = delete;
    SpirvLower(const SpirvLower&) = delete;
    SpirvLower& operator=(const SpirvLower&) = delete;
};

} // Llpc
