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
 * @file  llpcBuilderRecorder.cpp
 * @brief LLPC source file: BuilderRecorder implementation
 ***********************************************************************************************************************
 */
#include "llpcBuilderRecorder.h"
#include "llpcInternal.h"

#define DEBUG_TYPE "llpc-builder-recorder"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
// Given an opcode, get the call name (without the "llpc.call." prefix)
StringRef BuilderRecorder::GetCallName(
    Opcode opcode)    // Opcode
{
    switch (opcode)
    {
    case Opcode::Nop:
        return "nop";
    case Opcode::MiscKill:
        return "misc.kill";
    }
    LLPC_NEVER_CALLED();
    return "";
}

// =====================================================================================================================
// Create a BuilderRecorder
Builder* Builder::CreateBuilderRecorder(
    LLVMContext&  context,    // [in] LLVM context
    bool          wantReplay) // TRUE to make CreateBuilderReplayer return a replayer pass
{
    return new BuilderRecorder(context, wantReplay);
}

// =====================================================================================================================
// Recorder implementations of MiscBuilder methods
Instruction* BuilderRecorder::CreateKill(
    const Twine& instName)  // [in] Name to give final instruction
{
    return Record(Opcode::MiscKill, nullptr, ArrayRef<Value*>(), instName);
}

// =====================================================================================================================
// This is a BuilderRecorder. If it was created with wantReplay=true, create the BuilderReplayer pass.
ModulePass* BuilderRecorder::CreateBuilderReplayer()
{
    if (m_wantReplay)
    {
        // Create a new BuilderImpl to replay the recorded Builder calls in.
        return ::CreateBuilderReplayer(Builder::CreateBuilderImpl(getContext()));
    }
    return nullptr;
}

// =====================================================================================================================
// Record one Builder call
Instruction* BuilderRecorder::Record(
    BuilderRecorder::Opcode opcode,       // Opcode of Builder method call being recorded
    Type*                   pRetTy,       // [in] Return type (can be nullptr for void)
    ArrayRef<Value*>        args,         // Arguments
    const Twine&            instName)     // [in] Name to give instruction
{
    // Create mangled name of builder call.
    if (pRetTy == nullptr)
    {
        pRetTy = Type::getVoidTy(getContext());
    }
    std::string mangledName = (Twine(BuilderCallPrefix) + GetCallName(opcode)).str();
    AddTypeMangling(pRetTy, args, mangledName);

    // Add opcode and sequence number to front of args, and step the sequence number.
    SmallVector<Value*, 8> extendedArgs;
    extendedArgs.push_back(getInt32(opcode));
    extendedArgs.push_back(getInt32(m_seqNum++));
    extendedArgs.insert(extendedArgs.end(), args.begin(), args.end());

    // See if the declaration already exists in the module.
    Module* pModule = GetInsertBlock()->getParent()->getParent();
    Function* pFunc = dyn_cast_or_null<Function>(pModule->getFunction(mangledName));
    if (pFunc == nullptr)
    {
        // Does not exist. Create it.
        std::vector<Type*> argTys;
        for (auto arg : extendedArgs)
        {
            argTys.push_back(arg->getType());
        }

        auto pFuncTy = FunctionType::get(pRetTy, argTys, false);
        pFunc = Function::Create(pFuncTy, GlobalValue::ExternalLinkage, mangledName, pModule);
    }

    // Create the call.
    return CreateCall(pFunc, extendedArgs, instName);
}
