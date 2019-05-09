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
 * @file  llpcBuilderRecorder.h
 * @brief LLPC header file: declaration of Llpc::BuilderRecorder
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcBuilder.h"

namespace Llpc
{

// Prefix of all recorded calls.
static const char* const BuilderCallPrefix = "llpc.call.";

// LLPC call opcode metadata name.
static const char* const BuilderCallOpcodeMetadataName = "llpc.call.opcode";

// =====================================================================================================================
// A class that caches the metadata kind IDs used by BuilderRecorder and BuilderReplayer.
class BuilderRecorderMetadataKinds
{
public:
    BuilderRecorderMetadataKinds() {}
    BuilderRecorderMetadataKinds(llvm::LLVMContext& context);

    uint32_t        m_opcodeMetaKindId;                         // Cached metadata kinds for opcode
};

// =====================================================================================================================
// Builder recorder, to record all Builder calls as intrinsics
// Each call to a Builder method causes the insertion of a call to llpc.call.*, so the Builder calls can be replayed
// later on.
class BuilderRecorder final : public Builder, BuilderRecorderMetadataKinds
{
public:
    // llpc.call.* opcodes
    enum Opcode : uint32_t
    {
        // NOP
        Nop = 0,

        // Descriptor
        DescWaterfallLoop,
        DescWaterfallStoreLoop,
        DescLoadBuffer,
        DescLoadSampler,
        DescLoadResource,
        DescLoadTexelBuffer,
        DescLoadFmask,
        DescLoadSpillTablePtr,

        // Misc.
        MiscKill,
        MiscReadClock,
    };

    // Given an opcode, get the call name (without the "llpc.call." prefix)
    static llvm::StringRef GetCallName(Opcode opcode);

    BuilderRecorder(llvm::LLVMContext& context, bool wantReplay)
        : Builder(context), BuilderRecorderMetadataKinds(context), m_wantReplay(wantReplay)
    {}

    ~BuilderRecorder() {}

#ifndef NDEBUG
    // Link the individual shader modules into a single pipeline module.
    // This is overridden by BuilderRecorder only on a debug build so it can check that the frontend
    // set shader stage consistently.
    llvm::Module* Link(llvm::ArrayRef<llvm::Module*> modules) override final;
#endif

    //
    // Builder methods implemented in BuilderImplDesc
    //

    llvm::Instruction* CreateWaterfallLoop(llvm::Instruction*       pNonUniformInst,
                                           llvm::ArrayRef<uint32_t> operandIdxs,
                                           const llvm::Twine&       instName) override final;

    llvm::Value* CreateLoadBufferDesc(uint32_t            descSet,
                                      uint32_t            binding,
                                      llvm::Value*        pDescIndex,
                                      bool                isNonUniform,
                                      llvm::Type*         pPointeeTy,
                                      const llvm::Twine&  instName) override final;

    llvm::Value* CreateLoadSamplerDesc(uint32_t            descSet,
                                       uint32_t            binding,
                                       llvm::Value*        pDescIndex,
                                       bool                isNonUniform,
                                       const llvm::Twine&  instName) override final;

    llvm::Value* CreateLoadResourceDesc(uint32_t            descSet,
                                        uint32_t            binding,
                                        llvm::Value*        pDescIndex,
                                        bool                isNonUniform,
                                        const llvm::Twine&  instName) override final;

    llvm::Value* CreateLoadTexelBufferDesc(uint32_t            descSet,
                                           uint32_t            binding,
                                           llvm::Value*        pDescIndex,
                                           bool                isNonUniform,
                                           const llvm::Twine&  instName) override final;

    llvm::Value* CreateLoadFmaskDesc(uint32_t            descSet,
                                     uint32_t            binding,
                                     llvm::Value*        pDescIndex,
                                     bool                isNonUniform,
                                     const llvm::Twine&  instName) override final;

    llvm::Value* CreateLoadSpillTablePtr(llvm::Type*         pSpillTableTy,
                                         const llvm::Twine&  instName) override final;

    //
    // Builder methods implemented in BuilderImplMisc
    //

    llvm::Instruction* CreateKill(const llvm::Twine& instName = "") override final;
    llvm::Instruction* CreateReadClock(bool realtime, const llvm::Twine& instName = "") override final;

    // If this is a BuilderRecorder created with wantReplay=true, create the BuilderReplayer pass.
    llvm::ModulePass* CreateBuilderReplayer() override;

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderRecorder)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderRecorder)

    // Record one Builder call
    llvm::Instruction* Record(Opcode                        opcode,
                              llvm::Type*                   pReturnTy,
                              llvm::ArrayRef<llvm::Value*>  args,
                              const llvm::Twine&            instName);

#ifndef NDEBUG
    // Check that the frontend is consistently telling us which shader stage a function is in.
    void CheckFuncShaderStage(llvm::Function* pFunc, ShaderStage shaderStage);
#endif

    // -----------------------------------------------------------------------------------------------------------------

    bool            m_wantReplay;                             // true to make CreateBuilderReplayer return a replayer
                                                              //   pass
#ifndef NDEBUG
    // Only used in a debug build to ensure SetShaderStage is being used consistently.
    std::map<llvm::Function*, ShaderStage>  m_funcShaderStageMap;           // Map from function to shader stage
    llvm::Function*                         m_pEnclosingFunc = nullptr;     // Last function written with current
                                                                            //   shader stage
#endif
};

} // Llpc
