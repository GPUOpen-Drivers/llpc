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
    // Methods implemented in BuilderImplDesc:
    //

    // Create a waterfall loop containing the specified instruction.
    // This does not use the current insert point; new code is inserted before and after pNonUniformInst.
    virtual llvm::Instruction* CreateWaterfallLoop(
        llvm::Instruction*        pNonUniformInst,    // [in] The instruction to put in a waterfall loop
        llvm::ArrayRef<uint32_t>  operandIdxs,        // The operand index/indices for non-uniform inputs that need to
                                                      //  be uniform
        const llvm::Twine&        instName = "") = 0; // [in] Name to give instruction(s)

    // Create a load of a buffer descriptor.
    // Currently supports returning non-fat-pointer <4 x i32> descriptor when pPointeeTy is nullptr.
    // TODO: It is intended to remove that functionality once LLPC has switched to fat pointers.
    virtual llvm::Value* CreateLoadBufferDesc(
        uint32_t            descSet,            // Descriptor set
        uint32_t            binding,            // Descriptor binding
        llvm::Value*        pDescIndex,         // [in] Descriptor index
        bool                isNonUniform,       // Whether the descriptor index is non-uniform
        llvm::Type*         pPointeeTy,         // [in] Type that the returned pointer should point to (null to return
                                                //    a non-fat-pointer <4 x i32> descriptor instead)
        const llvm::Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Create a load of a sampler descriptor. Returns a <4 x i32> descriptor.
    virtual llvm::Value* CreateLoadSamplerDesc(
        uint32_t            descSet,          // Descriptor set
        uint32_t            binding,          // Descriptor binding
        llvm::Value*        pDescIndex,       // [in] Descriptor index
        bool                isNonUniform,     // Whether the descriptor index is non-uniform
        const llvm::Twine&  instName = ""     // [in] Name to give instruction(s)
    ) = 0;

    // Create a load of a resource descriptor. Returns a <8 x i32> descriptor.
    virtual llvm::Value* CreateLoadResourceDesc(
        uint32_t            descSet,          // Descriptor set
        uint32_t            binding,          // Descriptor binding
        llvm::Value*        pDescIndex,       // [in] Descriptor index
        bool                isNonUniform,     // Whether the descriptor index is non-uniform
        const llvm::Twine&  instName = ""     // [in] Name to give instruction(s)
    ) = 0;

    // Create a load of a texel buffer descriptor. Returns a <4 x i32> descriptor.
    virtual llvm::Value* CreateLoadTexelBufferDesc(
        uint32_t            descSet,          // Descriptor set
        uint32_t            binding,          // Descriptor binding
        llvm::Value*        pDescIndex,       // [in] Descriptor index
        bool                isNonUniform,     // Whether the descriptor index is non-uniform
        const llvm::Twine&  instName = ""     // [in] Name to give instruction(s)
    ) = 0;

    // Create a load of a F-mask descriptor. Returns a <8 x i32> descriptor.
    virtual llvm::Value* CreateLoadFmaskDesc(
        uint32_t            descSet,          // Descriptor set
        uint32_t            binding,          // Descriptor binding
        llvm::Value*        pDescIndex,       // [in] Descriptor index
        bool                isNonUniform,     // Whether the descriptor index is non-uniform
        const llvm::Twine&  instName = ""     // [in] Name to give instruction(s)
    ) = 0;

    // Create a load of the spill table pointer for push constants.
    virtual llvm::Value* CreateLoadSpillTablePtr(
        llvm::Type*         pSpillTableTy,      // [in] Type of the spill table that the returned pointer will point to
        const llvm::Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    //
    // Methods implemented in BuilderImplMisc:
    //

    // Create a "kill". Only allowed in a fragment shader.
    virtual llvm::Instruction* CreateKill(
        const llvm::Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Create a "readclock".
    virtual llvm::Instruction* CreateReadClock(
        bool                realtime,           // Whether to read real-time clock counter
        const llvm::Twine&  instName = "") = 0; // [in] Name to give instruction(s)

protected:
    Builder(llvm::LLVMContext& context) : llvm::IRBuilder<>(context) {}

private:
    LLPC_DISALLOW_DEFAULT_CTOR(Builder)
    LLPC_DISALLOW_COPY_AND_ASSIGN(Builder)
};

// Create BuilderReplayer pass
llvm::ModulePass* CreateBuilderReplayer(Builder* pBuilder);

} // Llpc

