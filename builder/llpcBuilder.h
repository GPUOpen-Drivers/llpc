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
 * @file  llpcBuilder.h
 * @brief LLPC header file: declaration of Llpc::Builder interface
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcDebug.h"

#include "llvm/IR/IRBuilder.h"

namespace llvm
{

class ModulePass;
class PassRegistry;

void initializeBuilderReplayerPass(PassRegistry&);

} // llvm

namespace Llpc
{

// =====================================================================================================================
// Initialize the pass that gets created by a Builder
inline static void InitializeBuilderPasses(
    llvm::PassRegistry& passRegistry)   // Pass registry
{
    initializeBuilderReplayerPass(passRegistry);
}

// =====================================================================================================================
// The LLPC Builder interface
class Builder : public llvm::IRBuilder<>
{
public:
    virtual ~Builder();

    // Create the BuilderImpl. In this implementation, each Builder call writes its IR immediately.
    static Builder* CreateBuilderImpl(llvm::LLVMContext& context);

    // Create the BuilderRecorder. In this implementation, each Builder call gets recorded (by inserting
    // an llpc.call.* call). The user then replays the Builder calls by running the pass created by
    // CreateBuilderReplayer. Setting wantReplay=false makes CreateBuilderReplayer return nullptr.
    static Builder* CreateBuilderRecorder(llvm::LLVMContext& context, bool wantReplay);

    // Create the BuilderImpl or BuilderRecorder, depending on -use-builder-recorder option
    static Builder* Create(llvm::LLVMContext& context);

    // If this is a BuilderRecorder, create the BuilderReplayer pass, otherwise return nullptr.
    virtual llvm::ModulePass* CreateBuilderReplayer() { return nullptr; }

    //
    // Methods implemented in BuilderImplMisc:
    //

    // Create a "kill". Only allowed in a fragment shader.
    virtual llvm::Instruction* CreateKill(const llvm::Twine& instName = "") = 0;

    //
    // Methods implemented in other BuilderImpl* subclasses...
    //

protected:
    Builder(llvm::LLVMContext& context) : llvm::IRBuilder<>(context) {}

private:
    LLPC_DISALLOW_DEFAULT_CTOR(Builder)
    LLPC_DISALLOW_COPY_AND_ASSIGN(Builder)
};

// Create BuilderReplayer pass
llvm::ModulePass* CreateBuilderReplayer(Builder* pBuilder);

} // Llpc

