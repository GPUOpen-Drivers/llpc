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
static const char* BuilderCallPrefix = "llpc.call.";

// =====================================================================================================================
// Builder recorder, to record all Builder calls as intrinsics
// Each call to a Builder method causes the insertion of a call to llpc.call.* with a sequence number, so the
// Builder calls can be replayed in order later on.
class BuilderRecorder : public Builder
{
public:
    // llpc.call.* opcodes
    enum Opcode : uint32_t
    {
        // NOP
        Nop = 0,

        // Misc.
        MiscKill,
    };

    // Given an opcode, get the call name (without the "llpc.call." prefix)
    static llvm::StringRef GetCallName(Opcode opcode);

    BuilderRecorder(llvm::LLVMContext& context, bool wantReplay) : Builder(context), m_wantReplay(wantReplay) {}
    ~BuilderRecorder() {}

    // Builder methods implemented in BuilderImplMisc
    llvm::Instruction* CreateKill(const llvm::Twine& instName = "") override;

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

    // -----------------------------------------------------------------------------------------------------------------
    bool      m_wantReplay;     // true to make CreateBuilderReplayer return a replayer pass
    uint32_t  m_seqNum = 0;     // Sequence number of next builder call
};

} // Llpc
