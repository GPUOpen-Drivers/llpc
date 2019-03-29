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
    case Opcode::LoadBufferDesc:
        return "load.buffer.desc";
    case Opcode::LoadSamplerDesc:
        return "load.sampler.desc";
    case Opcode::LoadResourceDesc:
        return "load.resource.desc";
    case Opcode::LoadTexelBufferDesc:
        return "load.texel.buffer.desc";
    case Opcode::LoadFmaskDesc:
        return "load.fmask.desc";
    case Opcode::LoadSpillTablePtr:
        return "load.spill.table.ptr.desc";
    case Opcode::Kill:
        return "kill";
    case Opcode::ReadClock:
        return "read.clock";
    case Opcode::WaterfallLoop:
        return "waterfall.loop";
    case Opcode::WaterfallStoreLoop:
        return "waterfall.store.loop";
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

#ifndef NDEBUG
// =====================================================================================================================
// Link the individual shader modules into a single pipeline module.
// This is overridden by BuilderRecorder only on a debug build so it can check that the frontend
// set shader stage consistently.
Module* BuilderRecorder::Link(
    ArrayRef<Module*> modules)    // Shader stage modules to link
{
    for (uint32_t stage = 0; stage != ShaderStageCount; ++stage)
    {
        if (Module* pModule = modules[stage])
        {
            for (auto& func : *pModule)
            {
                if (func.isDeclaration() == false)
                {
                    CheckFuncShaderStage(&func, static_cast<ShaderStage>(stage));
                }
            }
        }
    }
    return Builder::Link(modules);
}
#endif

// =====================================================================================================================
// Create a "kill". Only allowed in a fragment shader.
Instruction* BuilderRecorder::CreateKill(
    const Twine& instName)  // [in] Name to give final instruction
{
    return Record(Opcode::Kill, nullptr, {}, instName);
}

// =====================================================================================================================
// Create a "readclock".
Instruction* BuilderRecorder::CreateReadClock(
    bool         realtime,   // Whether to read real-time clock counter
    const Twine& instName)   // [in] Name to give final instruction
{
    return Record(Opcode::ReadClock, getInt64Ty(), getInt1(realtime), instName);
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
        pWaterfallLoop = Record(Opcode::WaterfallLoop, pNonUniformInst->getType(), args, instName);
    }
    else
    {
        // pNonUniformInst is a store with void return type, so we cannot pass its result through
        // llpc.call.waterfall.loop. Instead we pass one of its non-uniform inputs through
        // llpc.call.waterfall.store.loop. This situation needs to be specially handled in llpcBuilderReplayer.
        SetInsertPoint(pNonUniformInst);
        args[0] = pNonUniformInst->getOperand(operandIdxs[0]);
        auto pWaterfallStoreLoop = Record(Opcode::WaterfallStoreLoop, args[0]->getType(), args, instName);
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
    return Record(Opcode::LoadBufferDesc,
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
    return Record(Opcode::LoadSamplerDesc,
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
    return Record(Opcode::LoadResourceDesc,
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
    return Record(Opcode::LoadTexelBufferDesc,
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
    return Record(Opcode::LoadFmaskDesc,
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
    return Record(Opcode::LoadSpillTablePtr, pRetTy, {}, instName);
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
#ifndef NDEBUG
    // In a debug build, check that each enclosing function is consistently in the same shader stage.
    CheckFuncShaderStage(GetInsertBlock()->getParent(), m_shaderStage);
#endif

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

#ifndef NDEBUG
// =====================================================================================================================
// Check that the frontend is consistently telling us which shader stage a function is in.
void BuilderRecorder::CheckFuncShaderStage(
    Function*   pFunc,        // [in] Function to check
    ShaderStage shaderStage)  // Shader stage frontend says it is in
{
    LLPC_ASSERT(shaderStage < ShaderStageCount);
    if (pFunc != m_pEnclosingFunc)
    {
        auto mapIt = m_funcShaderStageMap.find(pFunc);
        if (mapIt != m_funcShaderStageMap.end())
        {
            LLPC_ASSERT((mapIt->second == shaderStage) && "Inconsistent use of Builder::SetShaderStage");
        }
        else
        {
            m_funcShaderStageMap[pFunc] = shaderStage;
        }
    }
    m_pEnclosingFunc = pFunc;
}
#endif

