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
 * @file  llpcBuilder.cpp
 * @brief LLPC source file: implementation of Llpc::Builder
 ***********************************************************************************************************************
 */
#include "llpcBuilderImpl.h"
#include "llpcPipelineState.h"
#include "llpcContext.h"
#include "llpcInternal.h"

#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"

#include <set>

#define DEBUG_TYPE "llpc-builder"

using namespace Llpc;
using namespace llvm;

// -use-builder-recorder
static cl::opt<uint32_t> UseBuilderRecorder("use-builder-recorder",
                                            cl::desc("Do lowering via recording and replaying LLPC builder:\n"
                                                     "0: Generate IR directly; no recording\n"
                                                     "1: Do lowering via recording and replaying LLPC builder (default)\n"
                                                     "2: Do lowering via recording; no replaying"),
                                            cl::init(1));

// =====================================================================================================================
// Create a Builder object
// If -use-builder-recorder is 0, this creates a BuilderImpl. Otherwise, it creates a BuilderRecorder.
Builder* Builder::Create(
    LLVMContext& context) // [in] LLVM context
{
    if (UseBuilderRecorder == 0)
    {
        // -use-builder-recorder=0: generate LLVM IR directly without recording
        return CreateBuilderImpl(context);
    }
    // -use-builder-recorder=1: record with BuilderRecorder and replay with BuilderReplayer
    // -use-builder-recorder=2: record with BuilderRecorder and do not replay
    return CreateBuilderRecorder(context, UseBuilderRecorder == 1 /*wantReplay*/);
}

// =====================================================================================================================
// Create a BuilderImpl object
Builder* Builder::CreateBuilderImpl(
    LLVMContext& context) // [in] LLVM context
{
    return new BuilderImpl(context);
}

// =====================================================================================================================
Builder::Builder(
    LLVMContext& context) // [in] LLPC context
    :
    IRBuilder<>(context)
{
    m_pPipelineState = new PipelineState(&context);
}

// =====================================================================================================================
Builder::~Builder()
{
    delete m_pPipelineState;
}

// =====================================================================================================================
// Get the type pElementTy, turned into a vector of the same vector width as pMaybeVecTy if the latter
// is a vector type.
Type* Builder::GetConditionallyVectorizedTy(
    Type* pElementTy,           // [in] Element type
    Type* pMaybeVecTy)          // [in] Possible vector type to get number of elements from
{
    if (auto pVecTy = dyn_cast<VectorType>(pMaybeVecTy))
    {
        return VectorType::get(pElementTy, pVecTy->getNumElements());
    }
    return pElementTy;
}

// =====================================================================================================================
// Set the resource mapping nodes for the given shader stage.
// This stores the nodes as IR metadata.
void Builder::SetUserDataNodes(
    ArrayRef<ResourceMappingNode>   nodes,            // The resource mapping nodes
    ArrayRef<DescriptorRangeValue>  rangeValues)      // The descriptor range values
{
    m_pPipelineState->SetUserDataNodes(nodes, rangeValues);
}

// =====================================================================================================================
// Base implementation of linking shader modules into a pipeline module.
Module* Builder::Link(
    ArrayRef<Module*> modules)     // Array of modules indexed by shader stage, with nullptr entry
                                   //  for any stage not present in the pipeline
{
    // Add IR metadata for the shader stage to each function in each shader, and rename the entrypoint to
    // ensure there is no clash on linking.
    uint32_t metaKindId = getContext().getMDKindID(LlpcName::ShaderStageMetadata);
    for (uint32_t stage = 0; stage < ShaderStageNativeStageCount; ++stage)
    {
        Module* pModule = modules[stage];
        if (pModule == nullptr)
        {
            continue;
        }

        auto pStageMetaNode = MDNode::get(getContext(), { ConstantAsMetadata::get(getInt32(stage)) });
        for (Function& func : *pModule)
        {
            if (func.isDeclaration() == false)
            {
                func.setMetadata(metaKindId, pStageMetaNode);
                if (func.getLinkage() != GlobalValue::InternalLinkage)
                {
                    func.setName(Twine(LlpcName::EntryPointPrefix) +
                                 GetShaderStageAbbreviation(static_cast<ShaderStage>(stage), true) +
                                 "." +
                                 func.getName());
                }
            }
        }
    }

    // If there is only one shader, just change the name on its module and return it.
    Module* pPipelineModule = nullptr;
    for (auto pModule : modules)
    {
        if (pPipelineModule == nullptr)
        {
            pPipelineModule = pModule;
        }
        else if (pModule != nullptr)
        {
            pPipelineModule = nullptr;
            break;
        }
    }

    if (pPipelineModule != nullptr)
    {
        pPipelineModule->setModuleIdentifier("llpcPipeline");

        // Record pipeline state into IR metadata.
        if (m_pPipelineState != nullptr)
        {
            m_pPipelineState->RecordState(pPipelineModule);
        }
    }
    else
    {
        // Create an empty module then link each shader module into it. We record pipeline state into IR
        // metadata before the link, to avoid problems with a Constant for an immutable descriptor value
        // disappearing when modules are deleted.
        bool result = true;
        pPipelineModule = new Module("llpcPipeline", getContext());
        static_cast<Llpc::Context*>(&getContext())->SetModuleTargetMachine(pPipelineModule);
        Linker linker(*pPipelineModule);

        if (m_pPipelineState != nullptr)
        {
            m_pPipelineState->RecordState(pPipelineModule);
        }

        for (int32_t stage = 0; stage < ShaderStageNativeStageCount; ++stage)
        {
            if (modules[stage] != nullptr)
            {
                // NOTE: We use unique_ptr here. The shader module will be destroyed after it is
                // linked into pipeline module.
                if (linker.linkInModule(std::unique_ptr<Module>(modules[stage])))
                {
                    result = false;
                }
            }
        }

        if (result == false)
        {
            delete pPipelineModule;
            pPipelineModule = nullptr;
        }
    }

    return pPipelineModule;
}

// =====================================================================================================================
// Create a map to i32 function. Many AMDGCN intrinsics only take i32's, so we need to massage input data into an i32
// to allow us to call these intrinsics. This helper takes a function pointer, massage arguments, and passthrough
// arguments and massages the mappedArgs into i32's before calling the function pointer. Note that all massage
// arguments must have the same type.
Value* Builder::CreateMapToInt32(
    PFN_MapToInt32Func pfnMapFunc,      // [in] The function to call on each provided i32.
    ArrayRef<Value*>   mappedArgs,      // The arguments to be massaged into i32's and passed to function.
    ArrayRef<Value*>   passthroughArgs) // The arguments to be passed through as is (no massaging).
{
    // We must have at least one argument to massage.
    LLPC_ASSERT(mappedArgs.size() > 0);

    Type* const pType = mappedArgs[0]->getType();

    // Check the massage types all match.
    for (uint32_t i = 1; i < mappedArgs.size(); i++)
    {
        LLPC_ASSERT(mappedArgs[i]->getType() == pType);
    }

    if (mappedArgs[0]->getType()->isVectorTy())
    {
        // For vectors we extract each vector component and map them individually.
        const uint32_t compCount = pType->getVectorNumElements();

        SmallVector<Value*, 4> results;

        for (uint32_t i = 0; i < compCount; i++)
        {
            SmallVector<Value*, 4> newMappedArgs;

            for (Value* const pMappedArg : mappedArgs)
            {
                newMappedArgs.push_back(CreateExtractElement(pMappedArg, i));
            }

            results.push_back(CreateMapToInt32(pfnMapFunc, newMappedArgs, passthroughArgs));
        }

        Value* pResult = UndefValue::get(VectorType::get(results[0]->getType(), compCount));

        for (uint32_t i = 0; i < compCount; i++)
        {
            pResult = CreateInsertElement(pResult, results[i], i);
        }

        return pResult;
    }
    else if (pType->isIntegerTy() && pType->getIntegerBitWidth() == 1)
    {
        SmallVector<Value*, 4> newMappedArgs;

        for (Value* const pMappedArg : mappedArgs)
        {
            newMappedArgs.push_back(CreateZExt(pMappedArg, getInt32Ty()));
        }

        Value* const pResult = CreateMapToInt32(pfnMapFunc, newMappedArgs, passthroughArgs);
        return CreateTrunc(pResult, getInt1Ty());
    }
    else if (pType->isIntegerTy() && pType->getIntegerBitWidth() < 32)
    {
        SmallVector<Value*, 4> newMappedArgs;

        Type* const pVectorType = VectorType::get(pType, (pType->getPrimitiveSizeInBits() == 16) ? 2 : 4);
        Value* const pUndef = UndefValue::get(pVectorType);

        for (Value* const pMappedArg : mappedArgs)
        {
            Value* const pNewMappedArg = CreateInsertElement(pUndef, pMappedArg, static_cast<uint64_t>(0));
            newMappedArgs.push_back(CreateBitCast(pNewMappedArg, getInt32Ty()));
        }

        Value* const pResult = CreateMapToInt32(pfnMapFunc, newMappedArgs, passthroughArgs);
        return CreateExtractElement(CreateBitCast(pResult, pVectorType), static_cast<uint64_t>(0));
    }
    else if (pType->getPrimitiveSizeInBits() == 64)
    {
        SmallVector<Value*, 4> castMappedArgs;

        for (Value* const pMappedArg : mappedArgs)
        {
            castMappedArgs.push_back(CreateBitCast(pMappedArg, VectorType::get(getInt32Ty(), 2)));
        }

        Value* pResult = UndefValue::get(castMappedArgs[0]->getType());

        for (uint32_t i = 0; i < 2; i++)
        {
            SmallVector<Value*, 4> newMappedArgs;

            for (Value* const pCastMappedArg : castMappedArgs)
            {
                newMappedArgs.push_back(CreateExtractElement(pCastMappedArg, i));
            }

            Value* const pResultComp = CreateMapToInt32(pfnMapFunc, newMappedArgs, passthroughArgs);

            pResult = CreateInsertElement(pResult, pResultComp, i);
        }

        return CreateBitCast(pResult, pType);
    }
    else if (pType->isFloatingPointTy())
    {
        SmallVector<Value*, 4> newMappedArgs;

        for (Value* const pMappedArg : mappedArgs)
        {
            newMappedArgs.push_back(CreateBitCast(pMappedArg, getIntNTy(pMappedArg->getType()->getPrimitiveSizeInBits())));
        }

        Value* const pResult = CreateMapToInt32(pfnMapFunc, newMappedArgs, passthroughArgs);
        return CreateBitCast(pResult, pType);
    }
    else if (pType->isIntegerTy(32))
    {
        return pfnMapFunc(*this, mappedArgs, passthroughArgs);
    }
    else
    {
        LLPC_NEVER_CALLED();
        return nullptr;
    }
}

// =====================================================================================================================
// Gets new matrix type after doing matrix transposing.
Type* Builder::GetTransposedMatrixTy(
    Type* const pMatrixType // [in] The matrix type to get the transposed type from.
    ) const
{
    LLPC_ASSERT(pMatrixType->isArrayTy());

    Type* const pColumnVectorType = pMatrixType->getArrayElementType();
    LLPC_ASSERT(pColumnVectorType->isVectorTy());

    const uint32_t columnCount = pMatrixType->getArrayNumElements();
    const uint32_t rowCount = pColumnVectorType->getVectorNumElements();

    return ArrayType::get(VectorType::get(pColumnVectorType->getVectorElementType(), columnCount), rowCount);
}

// =====================================================================================================================
// Get the LLPC context. This overrides the IRBuilder method that gets the LLVM context.
Context& Builder::getContext() const
{
    return *static_cast<Llpc::Context*>(&IRBuilder<>::getContext());
}

// =====================================================================================================================
// Get the type of pointer returned by CreateLoadBufferDesc.
PointerType* Builder::GetBufferDescTy(
    Type*         pPointeeTy)         // [in] Type that the returned pointer should point to.
{
    return PointerType::get(pPointeeTy, ADDR_SPACE_BUFFER_FAT_POINTER);
}

// =====================================================================================================================
// Get the type of an image descriptor
VectorType* Builder::GetImageDescTy()
{
    return VectorType::get(getInt32Ty(), 8);
}

// =====================================================================================================================
// Get the type of an fmask descriptor
VectorType* Builder::GetFmaskDescTy()
{
    return VectorType::get(getInt32Ty(), 8);
}

// =====================================================================================================================
// Get the type of a texel buffer descriptor
VectorType* Builder::GetTexelBufferDescTy()
{
    return VectorType::get(getInt32Ty(), 4);
}

// =====================================================================================================================
// Get the type of a sampler descriptor
VectorType* Builder::GetSamplerDescTy()
{
    return VectorType::get(getInt32Ty(), 4);
}

// =====================================================================================================================
// Get the type of pointer to image descriptor.
// This is in fact a struct containing the pointer itself plus the stride in dwords.
Type* Builder::GetImageDescPtrTy()
{
    return StructType::get(getContext(), { PointerType::get(GetImageDescTy(), ADDR_SPACE_CONST), getInt32Ty() });
}

// =====================================================================================================================
// Get the type of pointer to fmask descriptor.
// This is in fact a struct containing the pointer itself plus the stride in dwords.
Type* Builder::GetFmaskDescPtrTy()
{
    return StructType::get(getContext(), { PointerType::get(GetFmaskDescTy(), ADDR_SPACE_CONST), getInt32Ty() });
}

// =====================================================================================================================
// Get the type of pointer to texel buffer descriptor.
// This is in fact a struct containing the pointer itself plus the stride in dwords.
Type* Builder::GetTexelBufferDescPtrTy()
{
    return StructType::get(getContext(), { PointerType::get(GetTexelBufferDescTy(), ADDR_SPACE_CONST), getInt32Ty() });
}

// =====================================================================================================================
// Get the type of pointer to sampler descriptor.
// This is in fact a struct containing the pointer itself plus the stride in dwords.
Type* Builder::GetSamplerDescPtrTy()
{
    return StructType::get(getContext(), { PointerType::get(GetSamplerDescTy(), ADDR_SPACE_CONST), getInt32Ty() });
}

// =====================================================================================================================
// Get the type of a built-in. Where the built-in has a shader-defined array size (ClipDistance,
// CullDistance, SampleMask), inOutInfo.GetArraySize() is used as the array size.
Type* Builder::GetBuiltInTy(
    BuiltInKind   builtIn,            // Built-in kind
    InOutInfo     inOutInfo)          // Extra input/output info (shader-defined array size)
{
    enum TypeCode: uint32_t
    {
        a2f32,
        a4f32,
        af32,
        ai32,
        f32,
        i1,
        i32,
        i64,
        mask,
        v2f32,
        v3f32,
        v3i32,
        v4f32,
        v4i32,
    };

    uint32_t arraySize = inOutInfo.GetArraySize();
    TypeCode typeCode = TypeCode::i32;
    switch (builtIn)
    {
#define BUILTIN(name, number, out, in, type) \
    case BuiltIn ## name: \
        typeCode = TypeCode:: type; \
        break;
#include "llpcBuilderBuiltIns.h"
#undef BUILTIN
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    switch (typeCode)
    {
    case TypeCode::a2f32: return ArrayType::get(getFloatTy(), 2);
    case TypeCode::a4f32: return ArrayType::get(getFloatTy(), 4);
    // For ClipDistance and CullDistance, the shader determines the array size.
    case TypeCode::af32: return ArrayType::get(getFloatTy(), arraySize);
    // For SampleMask, the shader determines the array size.
    case TypeCode::ai32: return ArrayType::get(getInt32Ty(), arraySize);
    case TypeCode::f32: return getFloatTy();
    case TypeCode::i1: return getInt1Ty();
    case TypeCode::i32: return getInt32Ty();
    case TypeCode::i64: return getInt64Ty();
    case TypeCode::v2f32: return VectorType::get(getFloatTy(), 2);
    case TypeCode::v3f32: return VectorType::get(getFloatTy(), 3);
    case TypeCode::v4f32: return VectorType::get(getFloatTy(), 4);
    case TypeCode::v3i32: return VectorType::get(getInt32Ty(), 3);
    case TypeCode::v4i32: return VectorType::get(getInt32Ty(), 4);
    default:
        LLPC_NEVER_CALLED();
        return nullptr;
    }
}

// =====================================================================================================================
// Get a constant of FP or vector of FP type from the given APFloat, converting APFloat semantics where necessary
Constant* Builder::GetFpConstant(
    Type*           pTy,    // [in] FP scalar or vector type
    APFloat         value)  // APFloat value
{
    const fltSemantics* pSemantics = &APFloat::IEEEdouble();
    Type* pScalarTy = pTy->getScalarType();
    if (pScalarTy->isHalfTy())
    {
        pSemantics = &APFloat::IEEEhalf();
    }
    else if (pScalarTy->isFloatTy())
    {
        pSemantics = &APFloat::IEEEsingle();
    }
    bool ignored = true;
    value.convert(*pSemantics, APFloat::rmNearestTiesToEven, &ignored);
    return ConstantFP::get(pTy, value);
}

// =====================================================================================================================
// Get a constant of FP or vector of FP type for the value PI/180, for converting radians to degrees.
Constant* Builder::GetPiOver180(
    Type* pTy)    // [in] FP scalar or vector type
{
    // PI/180, 0.017453292
    // TODO: Use a value that works for double as well.
    return GetFpConstant(pTy, APFloat(APFloat::IEEEdouble(), APInt(64, 0x3F91DF46A0000000)));
}

// =====================================================================================================================
// Get a constant of FP or vector of FP type for the value 180/PI, for converting degrees to radians.
Constant* Builder::Get180OverPi(
    Type* pTy)    // [in] FP scalar or vector type
{
    // 180/PI, 57.29577951308232
    // TODO: Use a value that works for double as well.
    return GetFpConstant(pTy, APFloat(APFloat::IEEEdouble(), APInt(64, 0x404CA5DC20000000)));
}

