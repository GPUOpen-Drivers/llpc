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

#include "llpc.h"
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
//
// The Builder interface is used by the frontend to generate IR for LLPC-specific operations. It is
// a subclass of llvm::IRBuilder, so it uses its concept of an insertion point with debug location,
// and it exposes all the IRBuilder methods for building IR. However, unlike IRBuilder, LLPC's
// Builder is designed to have a single instance that contains some other state used during the IR
// building process.
//
// The frontend can use Builder in one of three ways:
// 1. BuilderImpl-only with full pipeline state
// 2. BuilderRecorder with full pipeline state
// 3. Per-shader frontend compilation (This is proposed but currently unsupported and untested.)
//
// 1. BuilderImpl-only with full pipeline state
//
//    This is used where the frontend has full pipeline state, and it wants to generate IR for LLPC
//    operations directly, instead of recording it in the frontend and then replaying the recorded
//    calls at the start of the middle-end.
//
//    The frontend does this:
//
//    * Create an instance of BuilderImpl.
//    * Create an IR module per shader stage.
//    * Populate the per-shader-stage IR modules, using Builder::Create* calls to generate the IR
//      for LLPC operations.
//    * After finishing, call Builder::Link() to link the per-stage IR modules into a single
//      pipeline module.
//    * Run middle-end passes on it.
//
// 2. BuilderRecorder with full pipeline state
//
//    This is also used where the frontend has full pipeline state, but it wants to record its
//    Builder::Create* calls such that they get replayed (and generated into normal IR) as the first
//    middle-end pass.
//
//    The frontend's actions are pretty much the same as in (1):
//
//    * Create an instance of BuilderRecorder.
//    * Create an IR module per shader stage.
//    * Populate the per-shader-stage IR modules, using Builder::Create* calls to generate the IR
//      for LLPC operations.
//    * After finishing, call Builder::Link() to link the per-stage IR modules into a single
//      pipeline module.
//    * Run middle-end passes on it, starting with BuilderReplayer to replay all the recorded
//      Builder::Create* calls into its own instance of BuilderImpl (but with a single pipeline IR
//      module).
//
//    With this scheme, the intention is that the whole-pipeline IR module after linking is a
//    representation of the pipeline. For testing purposes, the IR module could be output to a .ll
//    file, and later read in and compiled through the middle-end passes and backend to ISA.
//    However, that is not supported yet, as there is still some outside-IR state at that point.
//
// 3. Per-shader frontend compilation (This is proposed but currently unsupported and untested.)
//
//    The frontend can compile a single shader with no pipeline state available using
//    BuilderRecorder, without linking at the end, giving a shader IR module containing recorded
//    llpc.call.* calls but no pipeline state.
//
//    The frontend does this:
//
//    * Per shader:
//      - Create an instance of BuilderRecorder.
//      - Create an IR module per shader stage.
//      - Populate the per-shader-stage IR modules, using Builder::Create* calls to generate the IR
//        for LLPC operations.
//    * Then, later on, bring the shader IR modules together, and link them with Builder::Link()
//      into a single pipeline IR module.
//    * Run middle-end passes on it, starting with BuilderReplayer to replay all the recorded
//      Builder::Create* calls into its own instance of BuilderImpl (but with a single pipeline IR
//      module).
//
class Builder : public llvm::IRBuilder<>
{
public:
    // The group arithmetic operations the builder can consume.
    // NOTE: We rely on casting this implicitly to an integer, so we cannot use an enum class.
    enum GroupArithOp
    {
        IAdd,
        FAdd,
        IMul,
        FMul,
        SMin,
        UMin,
        FMin,
        SMax,
        UMax,
        FMax,
        And,
        Or,
        Xor
    };

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

    // Set the current shader stage.
    void SetShaderStage(ShaderStage stage) { m_shaderStage = stage; }

    // Link the individual shader modules into a single pipeline module. The frontend must have
    // finished calling Builder::Create* methods and finished building the IR. In the case that
    // there are multiple shader modules, they are all freed by this call, and the linked pipeline
    // module is returned. If there is a single shader module, this might instead just return that.
    // Before calling this, each shader module needs to have one global function for the shader
    // entrypoint, then all other functions with internal linkage.
    // Returns the pipeline module, or nullptr on link failure.
    virtual llvm::Module* Link(
        llvm::ArrayRef<llvm::Module*> modules);     // Array of modules indexed by shader stage, with nullptr entry
                                                    //  for any stage not present in the pipeline

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

    //
    // Methods implemented in BuilderImplSubgroup:
    //

    // Create a get subgroup size query.
    virtual llvm::Value* CreateGetSubgroupSize(
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup elect.
    virtual llvm::Value* CreateSubgroupElect(
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup all.
    virtual llvm::Value* CreateSubgroupAll(
        llvm::Value* const pValue,             // [in] The value to compare
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup any
    virtual llvm::Value* CreateSubgroupAny(
        llvm::Value* const pValue,             // [in] The value to compare
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup all equal.
    virtual llvm::Value* CreateSubgroupAllEqual(
        llvm::Value* const pValue,             // [in] The value to compare
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup broadcast.
    virtual llvm::Value* CreateSubgroupBroadcast(
        llvm::Value* const pValue,             // [in] The value to broadcast
        llvm::Value* const pIndex,             // [in] The index to broadcast from
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup broadcast first.
    virtual llvm::Value* CreateSubgroupBroadcastFirst(
        llvm::Value* const pValue,             // [in] The value to broadcast
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot.
    virtual llvm::Value* CreateSubgroupBallot(
        llvm::Value* const pValue,             // [in] The value to contribute
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup inverse ballot.
    virtual llvm::Value* CreateSubgroupInverseBallot(
        llvm::Value* const pValue,             // [in] The ballot value
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot bit extract.
    virtual llvm::Value* CreateSubgroupBallotBitExtract(
        llvm::Value* const pValue,             // [in] The ballot value
        llvm::Value* const pIndex,             // [in] The index to extract from the ballot
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot bit count.
    virtual llvm::Value* CreateSubgroupBallotBitCount(
        llvm::Value* const pValue,             // [in] The ballot value
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot inclusive bit count.
    virtual llvm::Value* CreateSubgroupBallotInclusiveBitCount(
        llvm::Value* const pValue,             // [in] The ballot value
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot exclusive bit count.
    virtual llvm::Value* CreateSubgroupBallotExclusiveBitCount(
        llvm::Value* const pValue,             // [in] The ballot value
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot find least significant bit.
    virtual llvm::Value* CreateSubgroupBallotFindLsb(
        llvm::Value* const pValue,             // [in] The ballot value
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot find most significant bit.
    virtual llvm::Value* CreateSubgroupBallotFindMsb(
        llvm::Value* const pValue,             // [in] The ballot value
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup shuffle.
    virtual llvm::Value* CreateSubgroupShuffle(
        llvm::Value* const pValue,             // [in] The value to shuffle
        llvm::Value* const pIndex,             // [in] The index to shuffle from
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup shuffle xor.
    virtual llvm::Value* CreateSubgroupShuffleXor(
        llvm::Value* const pValue,             // [in] The value to shuffle
        llvm::Value* const pMask,              // [in] The mask to shuffle with
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup shuffle up.
    virtual llvm::Value* CreateSubgroupShuffleUp(
        llvm::Value* const pValue,             // [in] The value to shuffle
        llvm::Value* const pDelta,             // [in] The delta to shuffle up to
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup shuffle down.
    virtual llvm::Value* CreateSubgroupShuffleDown(
        llvm::Value* const pValue,             // [in] The value to shuffle
        llvm::Value* const pDelta,             // [in] The delta to shuffle down to
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup clustered reduction.
    virtual llvm::Value* CreateSubgroupClusteredReduction(
        GroupArithOp       groupArithOp,       // The group arithmetic operation to perform
        llvm::Value* const pValue,             // [in] The value to perform on
        llvm::Value* const pClusterSize,       // [in] The cluster size
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup clustered inclusive scan.
    virtual llvm::Value* CreateSubgroupClusteredInclusive(
        GroupArithOp       groupArithOp,       // The group arithmetic operation to perform
        llvm::Value* const pValue,             // [in] The value to perform on
        llvm::Value* const pClusterSize,       // [in] The cluster size
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup clustered exclusive scan.
    virtual llvm::Value* CreateSubgroupClusteredExclusive(
        GroupArithOp       groupArithOp,       // The group arithmetic operation to perform
        llvm::Value* const pValue,             // [in] The value to perform on
        llvm::Value* const pClusterSize,       // [in] The cluster size
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup quad broadcast.
    virtual llvm::Value* CreateSubgroupQuadBroadcast(
        llvm::Value* const pValue,             // [in] The value to broadcast
        llvm::Value* const pIndex,             // [in] the index within the quad to broadcast from
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup quad swap horizontal.
    virtual llvm::Value* CreateSubgroupQuadSwapHorizontal(
        llvm::Value* const pValue,             // [in] The value to swap
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup quad swap vertical.
    virtual llvm::Value* CreateSubgroupQuadSwapVertical(
        llvm::Value* const pValue,             // [in] The value to swap
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup quad swap diagonal.
    virtual llvm::Value* CreateSubgroupQuadSwapDiagonal(
        llvm::Value* const pValue,             // [in] The value to swap
        const llvm::Twine& instName = "") = 0; // [in] Name to give instruction(s)

protected:
    Builder(llvm::LLVMContext& context) : llvm::IRBuilder<>(context) {}

    // -----------------------------------------------------------------------------------------------------------------

    ShaderStage m_shaderStage = ShaderStageInvalid;   // Current shader stage being built.

    typedef llvm::Value* (*PFN_MapToInt32Func)(Builder&                     builder,
                                               llvm::ArrayRef<llvm::Value*> mappedArgs,
                                               llvm::ArrayRef<llvm::Value*> passthroughArgs);

    // Create a call that'll map the massage arguments to an i32 type (for functions that only take i32).
    llvm::Value* CreateMapToInt32(
        PFN_MapToInt32Func           pfnMapFunc,       // [in] Pointer to the function to call on each i32.
        llvm::ArrayRef<llvm::Value*> mappedArgs,       // The arguments to massage into an i32 type.
        llvm::ArrayRef<llvm::Value*> passthroughArgs); // The arguments to pass-through without massaging.

private:
    LLPC_DISALLOW_DEFAULT_CTOR(Builder)
    LLPC_DISALLOW_COPY_AND_ASSIGN(Builder)
};

// Create BuilderReplayer pass
llvm::ModulePass* CreateBuilderReplayer(Builder* pBuilder);

} // Llpc

