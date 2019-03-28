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
#include "llpcIntrinsDefs.h"

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
    case Opcode::DescLoadBuffer:
        return "desc.load.buffer";
    case Opcode::DescLoadSampler:
        return "desc.load.sampler";
    case Opcode::DescLoadResource:
        return "desc.load.resource";
    case Opcode::DescLoadTexelBuffer:
        return "desc.load.texel.buffer";
    case Opcode::DescLoadFmask:
        return "desc.load.fmask";
    case Opcode::DescLoadSpillTablePtr:
        return "desc.load.spill.table.ptr";
    case Opcode::MiscKill:
        return "misc.kill";
    case Opcode::MiscReadClock:
        return "misc.read.clock";
    case Opcode::DescWaterfallLoop:
        return "desc.waterfall.loop";
    case Opcode::DescWaterfallStoreLoop:
        return "desc.waterfall.store.loop";
    }
    LLPC_NEVER_CALLED();
    return "";
}

// =====================================================================================================================
// BuilderRecordedMetadataKinds constructor : get the metadata kind IDs
BuilderRecorderMetadataKinds::BuilderRecorderMetadataKinds(
    llvm::LLVMContext& context)   // [in] LLVM context
{
    m_opcodeMetaKindId = context.getMDKindID(BuilderCallOpcodeMetadataName);
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
// Create a "kill". Only allowed in a fragment shader.
Instruction* BuilderRecorder::CreateKill(
    const Twine& instName)  // [in] Name to give final instruction
{
    return Record(Opcode::MiscKill, nullptr, {}, instName);
}

// =====================================================================================================================
// Create a "readclock".
Instruction* BuilderRecorder::CreateReadClock(
    bool         realtime,   // Whether to read real-time clock counter
    const Twine& instName)   // [in] Name to give final instruction
{
    return Record(Opcode::MiscReadClock, getInt64Ty(), getInt1(realtime), instName);
}

// =====================================================================================================================
// Create a waterfall loop containing the specified instruction.
Instruction* BuilderRecorder::CreateWaterfallLoop(
    Instruction*        pNonUniformInst,    // [in] The instruction to put in a waterfall loop
    ArrayRef<uint32_t>  operandIdxs,        // The operand index/indices for non-uniform inputs that need to be uniform
    const Twine&        instName)           // [in] Name to give instruction(s)
{
    LLPC_ASSERT(operandIdxs.empty() == false);
    LLPC_ASSERT(pNonUniformInst->use_empty());

    // This method is specified to ignore the insert point, and to put the waterfall loop around pNonUniformInst.
    // For this recording implementation, put the call after pNonUniformInst, unless it is a store.
    //auto savedInsertPoint = saveIP();
    SetInsertPoint(pNonUniformInst->getNextNode());
    SetCurrentDebugLocation(pNonUniformInst->getDebugLoc());

    SmallVector<Value*, 3> args;
    args.push_back(pNonUniformInst);
    for (uint32_t operandIdx : operandIdxs)
    {
        args.push_back(getInt32(operandIdx));
    }

    Instruction *pWaterfallLoop = nullptr;
    if (pNonUniformInst->getType()->isVoidTy() == false)
    {
        // Normal case that pNonUniformInst is not a store so has a return type.
        pWaterfallLoop = Record(Opcode::DescWaterfallLoop, pNonUniformInst->getType(), args, instName);
    }
    else
    {
        // pNonUniformInst is a store with void return type, so we cannot pass its result through
        // llpc.call.waterfall.loop. Instead we pass one of its non-uniform inputs through
        // llpc.call.waterfall.store.loop. This situation needs to be specially handled in llpcBuilderReplayer.
        SetInsertPoint(pNonUniformInst);
        args[0] = pNonUniformInst->getOperand(operandIdxs[0]);
        auto pWaterfallStoreLoop = Record(Opcode::DescWaterfallStoreLoop, args[0]->getType(), args, instName);
        pNonUniformInst->setOperand(operandIdxs[0], pWaterfallStoreLoop);
    }

    // TODO: While almost nothing uses the Builder, we run the risk of the saved insertion
    // point being invalid and this restoreIP crashing. So, for now, we just clear the insertion point.
    //restoreIP(savedInsertPoint);
    ClearInsertionPoint();

    return pWaterfallLoop;
}

// =====================================================================================================================
// Create a load of a buffer descriptor.
Value* BuilderRecorder::CreateLoadBufferDesc(
    uint32_t      descSet,          // Descriptor set
    uint32_t      binding,          // Descriptor binding
    Value*        pDescIndex,       // [in] Descriptor index
    bool          isNonUniform,     // Whether the descriptor index is non-uniform
    Type*         pPointeeTy,       // [in] Type that the returned pointer should point to
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    Type* pDescTy = (pPointeeTy == nullptr) ?
                        cast<Type>(VectorType::get(getInt32Ty(), 4)) :
                        cast<Type>(PointerType::get(pPointeeTy, ADDR_SPACE_CONST));
    return Record(Opcode::DescLoadBuffer,
                  pDescTy,
                  {
                      getInt32(descSet),
                      getInt32(binding),
                      pDescIndex,
                      getInt1(isNonUniform),
                  },
                  instName);
}

// =====================================================================================================================
// Create a load of a sampler descriptor. Returns a <4 x i32> descriptor.
Value* BuilderRecorder::CreateLoadSamplerDesc(
    uint32_t      descSet,          // Descriptor set
    uint32_t      binding,          // Descriptor binding
    Value*        pDescIndex,       // [in] Descriptor index
    bool          isNonUniform,     // Whether the descriptor index is non-uniform
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return Record(Opcode::DescLoadSampler,
                  VectorType::get(getInt32Ty(), 4),
                  {
                      getInt32(descSet),
                      getInt32(binding),
                      pDescIndex,
                      getInt1(isNonUniform),
                  },
                  instName);
}

// =====================================================================================================================
// Create a load of a resource descriptor. Returns a <8 x i32> descriptor.
Value* BuilderRecorder::CreateLoadResourceDesc(
    uint32_t      descSet,          // Descriptor set
    uint32_t      binding,          // Descriptor binding
    Value*        pDescIndex,       // [in] Descriptor index
    bool          isNonUniform,     // Whether the descriptor index is non-uniform
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return Record(Opcode::DescLoadResource,
                  VectorType::get(getInt32Ty(), 8),
                  {
                      getInt32(descSet),
                      getInt32(binding),
                      pDescIndex,
                      getInt1(isNonUniform),
                  },
                  instName);
}

// =====================================================================================================================
// Create a load of a texel buffer descriptor. Returns a <4 x i32> descriptor.
Value* BuilderRecorder::CreateLoadTexelBufferDesc(
    uint32_t      descSet,          // Descriptor set
    uint32_t      binding,          // Descriptor binding
    Value*        pDescIndex,       // [in] Descriptor index
    bool          isNonUniform,     // Whether the descriptor index is non-uniform
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return Record(Opcode::DescLoadTexelBuffer,
                  VectorType::get(getInt32Ty(), 4),
                  {
                      getInt32(descSet),
                      getInt32(binding),
                      pDescIndex,
                      getInt1(isNonUniform),
                  },
                  instName);
}

// =====================================================================================================================
// Create a load of a F-mask descriptor. Returns a <8 x i32> descriptor.
Value* BuilderRecorder::CreateLoadFmaskDesc(
    uint32_t      descSet,          // Descriptor set
    uint32_t      binding,          // Descriptor binding
    Value*        pDescIndex,       // [in] Descriptor index
    bool          isNonUniform,     // Whether the descriptor index is non-uniform
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return Record(Opcode::DescLoadFmask,
                  VectorType::get(getInt32Ty(), 8),
                  {
                      getInt32(descSet),
                      getInt32(binding),
                      pDescIndex,
                      getInt1(isNonUniform),
                  },
                  instName);
}

// =====================================================================================================================
// Create a load of the spill table pointer for push constants.
Value* BuilderRecorder::CreateLoadSpillTablePtr(
    Type*         pSpillTableTy,    // [in] Type of the spill table that the returned pointer will point to
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    Type* pRetTy = PointerType::get(pSpillTableTy, ADDR_SPACE_CONST);
    return Record(Opcode::DescLoadSpillTablePtr, pRetTy, {}, instName);
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
    // Create mangled name of builder call. This only needs to be mangled on return type.
    std::string mangledName;
    {
        raw_string_ostream mangledNameStream(mangledName);
        mangledNameStream << BuilderCallPrefix;
        mangledNameStream << GetCallName(opcode);
        if (pRetTy != nullptr)
        {
            mangledNameStream << ".";
            GetTypeNameForScalarOrVector(pRetTy, mangledNameStream);
        }
        else
        {
            pRetTy = Type::getVoidTy(getContext());
        }
    }

    // See if the declaration already exists in the module.
    Module* const pModule = GetInsertBlock()->getModule();
    Function* pFunc = dyn_cast_or_null<Function>(pModule->getFunction(mangledName));
    if (pFunc == nullptr)
    {
        // Does not exist. Create it as a varargs function.
        auto pFuncTy = FunctionType::get(pRetTy, {}, true);
        pFunc = Function::Create(pFuncTy, GlobalValue::ExternalLinkage, mangledName, pModule);

        MDNode* const pFuncMeta = MDNode::get(getContext(), ConstantAsMetadata::get(getInt32(opcode)));

        pFunc->setMetadata(m_opcodeMetaKindId, pFuncMeta);
    }

    // Create the call.
    auto pCall = CreateCall(pFunc, args, instName);

    return pCall;
}

