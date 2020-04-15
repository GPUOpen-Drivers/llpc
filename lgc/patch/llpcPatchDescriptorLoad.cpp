/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPatchDescriptorLoad.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchDescriptorLoad.
 ***********************************************************************************************************************
 */
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "llpcPatchDescriptorLoad.h"
#include "llpcTargetInfo.h"

#define DEBUG_TYPE "llpc-patch-descriptor-load"

using namespace lgc;
using namespace llvm;

// -enable-shadow-desc: enable shadow desriptor table
static cl::opt<bool> EnableShadowDescriptorTable("enable-shadow-desc",
                                                 cl::desc("Enable shadow descriptor table"),
                                                 cl::init(true));

namespace lgc
{

// =====================================================================================================================
// Initializes static members.
char PatchDescriptorLoad::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for descriptor load
ModulePass* CreatePatchDescriptorLoad()
{
    return new PatchDescriptorLoad();
}

// =====================================================================================================================
PatchDescriptorLoad::PatchDescriptorLoad()
    :
    Patch(ID)
{
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
bool PatchDescriptorLoad::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Descriptor-Load\n");

    Patch::Init(&module);
    m_changed = false;

    m_pPipelineState = getAnalysis<PipelineStateWrapper>().GetPipelineState(&module);
    m_pipelineSysValues.Initialize(m_pPipelineState);

    // Invoke handling of "call" instruction
    auto pPipelineShaders = &getAnalysis<PipelineShaders>();
    for (uint32_t shaderStage = 0; shaderStage < ShaderStageCountInternal; ++shaderStage)
    {
        m_pEntryPoint = pPipelineShaders->GetEntryPoint(static_cast<ShaderStage>(shaderStage));
        if (m_pEntryPoint != nullptr)
        {
            m_shaderStage = static_cast<ShaderStage>(shaderStage);
            visit(*m_pEntryPoint);
        }
    }

    // Remove unnecessary descriptor load calls
    for (auto pCallInst : m_descLoadCalls)
    {
        pCallInst->dropAllReferences();
        pCallInst->eraseFromParent();
    }
    m_descLoadCalls.clear();

    // Remove unnecessary descriptor load functions
    for (auto pFunc : m_descLoadFuncs)
    {
        if (pFunc->user_empty())
        {
            pFunc->dropAllReferences();
            pFunc->eraseFromParent();
        }
    }
    m_descLoadFuncs.clear();

    // Remove dead llpc.descriptor.point* and llpc.descriptor.index calls that were not
    // processed by the code above. That happens if they were never used in llpc.descriptor.load.from.ptr.
    SmallVector<Function*, 4> deadDescFuncs;
    for (Function& func : *m_pModule)
    {
        if (func.isDeclaration() && (func.getName().startswith(lgcName::DescriptorGetPtrPrefix) ||
                                     func.getName().startswith(lgcName::DescriptorIndex)))
        {
            deadDescFuncs.push_back(&func);
        }
    }
    for (Function* pFunc : deadDescFuncs)
    {
        while (pFunc->use_empty() == false)
        {
            pFunc->use_begin()->set(UndefValue::get(pFunc->getType()));
        }
        pFunc->eraseFromParent();
    }

    m_pipelineSysValues.Clear();
    return m_changed;
}

// =====================================================================================================================
// Process llpc.descriptor.get.{resource|sampler|fmask}.ptr call.
// This generates code to build a {ptr,stride} struct.
void PatchDescriptorLoad::ProcessDescriptorGetPtr(
    CallInst* pDescPtrCall,     // [in] Call to llpc.descriptor.get.*.ptr
    StringRef descPtrCallName)  // Name of that call
{
    m_pEntryPoint = pDescPtrCall->getFunction();
    IRBuilder<> builder(*m_pContext);
    builder.SetInsertPoint(pDescPtrCall);

    // Find the resource node for the descriptor set and binding.
    uint32_t descSet = cast<ConstantInt>(pDescPtrCall->getArgOperand(0))->getZExtValue();
    uint32_t binding = cast<ConstantInt>(pDescPtrCall->getArgOperand(1))->getZExtValue();
    auto resType = ResourceNodeType::DescriptorResource;
    bool shadow = false;
    if (descPtrCallName.startswith(lgcName::DescriptorGetTexelBufferPtr))
    {
        resType = ResourceNodeType::DescriptorTexelBuffer;
    }
    else if (descPtrCallName.startswith(lgcName::DescriptorGetSamplerPtr))
    {
        resType = ResourceNodeType::DescriptorSampler;
    }
    else if (descPtrCallName.startswith(lgcName::DescriptorGetFmaskPtr))
    {
        shadow = EnableShadowDescriptorTable;
        resType = ResourceNodeType::DescriptorFmask;
    }

    // Find the descriptor node. For fmask with -enable-shadow-descriptor-table, if no fmask descriptor
    // is found, look for a resource (image) one instead.
    const ResourceNode* pTopNode = nullptr;
    const ResourceNode* pNode = nullptr;
    std::tie(pTopNode, pNode) = m_pPipelineState->FindResourceNode(resType, descSet, binding);
    if ((pNode == nullptr) && (resType == ResourceNodeType::DescriptorFmask) && shadow)
    {
        std::tie(pTopNode, pNode) = m_pPipelineState->FindResourceNode(ResourceNodeType::DescriptorResource,
                                                                       descSet,
                                                                       binding);
    }

    Value* pDescPtrAndStride = nullptr;
    if (pNode == nullptr)
    {
        // We did not find the resource node. Use an undef value.
        pDescPtrAndStride = UndefValue::get(pDescPtrCall->getType());
    }
    else
    {
        // Get the descriptor pointer and stride as a struct.
        pDescPtrAndStride = GetDescPtrAndStride(resType, descSet, binding, pTopNode, pNode, shadow, builder);
    }
    pDescPtrCall->replaceAllUsesWith(pDescPtrAndStride);
    m_descLoadCalls.push_back(pDescPtrCall);
    m_changed = true;
}

// =====================================================================================================================
// Get a struct containing the pointer and byte stride for a descriptor
Value* PatchDescriptorLoad::GetDescPtrAndStride(
    ResourceNodeType        resType,    // Resource type
    uint32_t                descSet,    // Descriptor set
    uint32_t                binding,    // Binding
    const ResourceNode*     pTopNode,   // Node in top-level descriptor table (TODO: nullptr for shader compilation)
    const ResourceNode*     pNode,      // The descriptor node itself (TODO: nullptr for shader compilation)
    bool                    shadow,     // Whether to load from shadow descriptor table
    IRBuilder<>&            builder)    // [in/out] IRBuilder
{
    // Determine the stride if possible without looking at the resource node.
    uint32_t byteSize = 0;
    Value* pStride = nullptr;
    switch (resType)
    {
    case ResourceNodeType::DescriptorBuffer:
    case ResourceNodeType::DescriptorTexelBuffer:
        byteSize = DescriptorSizeBuffer;
        if ((pNode != nullptr) && (pNode->type == ResourceNodeType::DescriptorBufferCompact))
        {
            byteSize = DescriptorSizeBufferCompact;
        }
        pStride = builder.getInt32(byteSize / 4);
        break;
    case ResourceNodeType::DescriptorSampler:
        byteSize = DescriptorSizeSampler;
        break;
    case ResourceNodeType::DescriptorResource:
    case ResourceNodeType::DescriptorFmask:
        byteSize = DescriptorSizeResource;
        break;
    default:
        llvm_unreachable("");
        break;
    }

    if (pStride == nullptr)
    {
        // Stride is not determinable just from the descriptor type requested by the Builder call.
        if (pNode == nullptr)
        {
            // TODO: Shader compilation: Get byte stride using a reloc.
            llvm_unreachable("");
        }
        else
        {
            // Pipeline compilation: Get the stride from the resource type in the node.
            switch (pNode->type)
            {
            case ResourceNodeType::DescriptorSampler:
                pStride = builder.getInt32(DescriptorSizeSampler / 4);
                break;
            case ResourceNodeType::DescriptorResource:
            case ResourceNodeType::DescriptorFmask:
                pStride = builder.getInt32(DescriptorSizeResource / 4);
                break;
            case ResourceNodeType::DescriptorCombinedTexture:
                pStride = builder.getInt32((DescriptorSizeResource + DescriptorSizeSampler) / 4);
                break;
            default:
                llvm_unreachable("Unexpected resource node type");
                break;
            }
        }
    }

    Value* pDescPtr = nullptr;
    if ((pNode != nullptr) && (pNode->pImmutableValue != nullptr) && (resType == ResourceNodeType::DescriptorSampler))
    {
        // This is an immutable sampler. Put the immutable value into a static variable and return a pointer
        // to that. For a simple non-variably-indexed immutable sampler not passed through a function call
        // or phi node, we rely on subsequent LLVM optimizations promoting the value back to a constant.
        std::string globalName = (Twine("_immutable_sampler_") + Twine(pNode->set) +
                                  " " + Twine(pNode->binding)).str();
        pDescPtr = m_pModule->getGlobalVariable(globalName, /*AllowInternal=*/true);
        if (pDescPtr == nullptr)
        {
            pDescPtr = new GlobalVariable(*m_pModule,
                                          pNode->pImmutableValue->getType(),
                                          /*isConstant=*/true,
                                          GlobalValue::InternalLinkage,
                                          pNode->pImmutableValue,
                                          globalName,
                                          nullptr,
                                          GlobalValue::NotThreadLocal,
                                          ADDR_SPACE_CONST);
        }
        pDescPtr = builder.CreateBitCast(pDescPtr, builder.getInt8Ty()->getPointerTo(ADDR_SPACE_CONST));

        // We need to change the stride to 4 dwords. It would otherwise be incorrectly set to 12 dwords
        // for a sampler in a combined texture.
        pStride = builder.getInt32(DescriptorSizeSampler / 4);
    }
    else
    {
        // Get a pointer to the descriptor.
        pDescPtr = GetDescPtr(resType, descSet, binding, pTopNode, pNode, shadow, builder);
    }

    // Cast the pointer to the right type and create and return the struct.
    pDescPtr = builder.CreateBitCast(
                            pDescPtr,
                            VectorType::get(builder.getInt32Ty(), byteSize / 4)->getPointerTo(ADDR_SPACE_CONST));
    Value* pDescPtrStruct = builder.CreateInsertValue(UndefValue::get(StructType::get(*m_pContext,
                                                                                      { pDescPtr->getType(),
                                                                                        builder.getInt32Ty() })),
                                                      pDescPtr,
                                                      0);
    pDescPtrStruct = builder.CreateInsertValue(pDescPtrStruct, pStride, 1);
    return pDescPtrStruct;
}

// =====================================================================================================================
// Get a pointer to a descriptor, as a pointer to i32
Value* PatchDescriptorLoad::GetDescPtr(
    ResourceNodeType        resType,    // Resource type
    uint32_t                descSet,    // Descriptor set
    uint32_t                binding,    // Binding
    const ResourceNode*     pTopNode,   // Node in top-level descriptor table (TODO: nullptr for shader compilation)
    const ResourceNode*     pNode,      // The descriptor node itself (TODO: nullptr for shader compilation)
    bool                    shadow,     // Whether to load from shadow descriptor table
    IRBuilder<>&            builder)    // [in/out] IRBuilder
{
    Value* pDescPtr = nullptr;
    // Get the descriptor table pointer.
    auto pSysValues = m_pipelineSysValues.Get(builder.GetInsertPoint()->getFunction());
    if ((pNode != nullptr) && (pNode == pTopNode))
    {
        // The descriptor is in the top-level table. Contrary to what used to happen, we just load from
        // the spill table, so we can get a pointer to the descriptor. It gets returned as a pointer
        // to array of i8.
        pDescPtr = pSysValues->GetSpillTablePtr();
    }
    else if (shadow)
    {
        // Get pointer to descriptor set's descriptor table as pointer to i8.
        pDescPtr = pSysValues->GetShadowDescTablePtr(descSet);
    }
    else
    {
        // Get pointer to descriptor set's descriptor table. This also gets returned as a pointer to
        // array of i8.
        pDescPtr = pSysValues->GetDescTablePtr(descSet);
    }

    // Add on the byte offset of the descriptor.
    Value* pOffset = nullptr;
    if (pNode == nullptr)
    {
        // TODO: Shader compilation: Get the offset for the descriptor using a reloc. The reloc symbol name
        // needs to contain the descriptor set and binding, and, for image, fmask or sampler, whether it is
        // a sampler.
        llvm_unreachable("");
    }
    else
    {
        // Get the offset for the descriptor. Where we are getting the second part of a combined resource,
        // add on the size of the first part.
        uint32_t offsetInDwords = pNode->offsetInDwords;
        if ((resType == ResourceNodeType::DescriptorSampler) &&
            (pNode->type == ResourceNodeType::DescriptorCombinedTexture))
        {
            offsetInDwords += DescriptorSizeResource / 4;
        }
        pOffset = builder.getInt32(offsetInDwords);
    }
    pDescPtr = builder.CreateBitCast(pDescPtr, builder.getInt32Ty()->getPointerTo(ADDR_SPACE_CONST));
    pDescPtr = builder.CreateGEP(builder.getInt32Ty(), pDescPtr, pOffset);

    return pDescPtr;
}

// =====================================================================================================================
// Process an llvm.descriptor.index : add an array index on to the descriptor pointer.
// llvm.descriptor.index has two operands: the "descriptor pointer" (actually a struct containing the actual
// pointer and an int giving the byte stride), and the index to add. It returns the updated "descriptor pointer".
void PatchDescriptorLoad::ProcessDescriptorIndex(
    CallInst* pCall)    // [in] llvm.descriptor.index call
{
    IRBuilder<> builder(*m_pContext);
    builder.SetInsertPoint(pCall);

    Value* pDescPtrStruct = pCall->getArgOperand(0);
    Value* pIndex = pCall->getArgOperand(1);
    Value* pStride = builder.CreateExtractValue(pDescPtrStruct, 1);
    Value* pDescPtr = builder.CreateExtractValue(pDescPtrStruct, 0);

    Value* pBytePtr = builder.CreateBitCast(pDescPtr, builder.getInt32Ty()->getPointerTo(ADDR_SPACE_CONST));
    pIndex = builder.CreateMul(pIndex, pStride);
    pBytePtr = builder.CreateGEP(builder.getInt32Ty(), pBytePtr, pIndex);
    pDescPtr = builder.CreateBitCast(pBytePtr, pDescPtr->getType());

    pDescPtrStruct = builder.CreateInsertValue(UndefValue::get(StructType::get(*m_pContext,
                                                                               { pDescPtr->getType(),
                                                                                 builder.getInt32Ty() })),
                                                      pDescPtr,
                                                      0);
    pDescPtrStruct = builder.CreateInsertValue(pDescPtrStruct, pStride, 1);

    pCall->replaceAllUsesWith(pDescPtrStruct);
    m_descLoadCalls.push_back(pCall);
    m_changed = true;
}

// =====================================================================================================================
// Process "llpc.descriptor.load.from.ptr" call.
void PatchDescriptorLoad::ProcessLoadDescFromPtr(
    CallInst* pLoadFromPtr)   // [in] Call to llpc.descriptor.load.from.ptr
{
    IRBuilder<> builder(*m_pContext);
    builder.SetInsertPoint(pLoadFromPtr);

    Value* pDescPtrStruct = pLoadFromPtr->getArgOperand(0);
    Value* pDescPtr = builder.CreateExtractValue(pDescPtrStruct, 0);
    Value* pDesc = builder.CreateLoad(pLoadFromPtr->getType(), pDescPtr);

    pLoadFromPtr->replaceAllUsesWith(pDesc);
    m_descLoadCalls.push_back(pLoadFromPtr);
    m_changed = true;
}

// =====================================================================================================================
// Visits "call" instruction.
void PatchDescriptorLoad::visitCallInst(
    CallInst& callInst)   // [in] "Call" instruction
{
    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

    StringRef mangledName = pCallee->getName();

    if (mangledName.startswith(lgcName::DescriptorGetPtrPrefix))
    {
        ProcessDescriptorGetPtr(&callInst, mangledName);
        return;
    }

    if (mangledName.startswith(lgcName::DescriptorIndex))
    {
        ProcessDescriptorIndex(&callInst);
        return;
    }

    if (mangledName.startswith(lgcName::DescriptorLoadFromPtr))
    {
        ProcessLoadDescFromPtr(&callInst);
        return;
    }

    if (mangledName.startswith(lgcName::DescriptorLoadSpillTable))
    {
        // Descriptor loading should be inlined and stay in shader entry-point
        assert(callInst.getParent()->getParent() == m_pEntryPoint);
        m_changed = true;

        if (callInst.use_empty() == false)
        {
            Value* pDesc = m_pipelineSysValues.Get(m_pEntryPoint)->GetSpilledPushConstTablePtr();
            if (pDesc->getType() != callInst.getType())
            {
                pDesc = new BitCastInst(pDesc, callInst.getType(), "", &callInst);
            }
            callInst.replaceAllUsesWith(pDesc);
        }
        m_descLoadCalls.push_back(&callInst);
        m_descLoadFuncs.insert(pCallee);
        return;
    }

    if (mangledName.startswith(lgcName::DescriptorLoadBuffer))
    {
        // Descriptor loading should be inlined and stay in shader entry-point
        assert(callInst.getParent()->getParent() == m_pEntryPoint);
        m_changed = true;

        if (callInst.use_empty() == false)
        {
            uint32_t descSet = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
            uint32_t binding = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
            Value* pArrayOffset = callInst.getOperand(2); // Offset for arrayed resource (index)
            Value* pDesc = LoadBufferDescriptor(descSet, binding, pArrayOffset, &callInst);
            callInst.replaceAllUsesWith(pDesc);
        }
        m_descLoadCalls.push_back(&callInst);
        m_descLoadFuncs.insert(pCallee);
        return;
    }
}

// =====================================================================================================================
// Generate the code for a buffer descriptor load.
// This is the handler for llpc.descriptor.load.buffer, which is also used for loading a descriptor
// from the global table or the per-shader table.
Value* PatchDescriptorLoad::LoadBufferDescriptor(
    uint32_t      descSet,        // Descriptor set
    uint32_t      binding,        // Binding
    Value*        pArrayOffset,   // [in] Index in descriptor array
    Instruction*  pInsertPoint)   // [in] Insert point
{
    IRBuilder<> builder(*m_pContext);
    builder.SetInsertPoint(pInsertPoint);

    // Handle the special cases. First get a pointer to the global/per-shader table as pointer to i8.
    Value* pDescPtr = nullptr;
    if (descSet == InternalResourceTable)
    {
        pDescPtr = m_pipelineSysValues.Get(m_pEntryPoint)->GetInternalGlobalTablePtr();
    }
    else if (descSet == InternalPerShaderTable)
    {
        pDescPtr = m_pipelineSysValues.Get(m_pEntryPoint)->GetInternalPerShaderTablePtr();
    }
    if (pDescPtr != nullptr)
    {
        // "binding" gives the offset, in units of v4i32 descriptors.
        // Add on the offset, giving pointer to i8.
        pDescPtr = builder.CreateGEP(builder.getInt8Ty(), pDescPtr, builder.getInt32(binding * DescriptorSizeBuffer));
        // Load the descriptor.
        Type* pDescTy = VectorType::get(builder.getInt32Ty(), DescriptorSizeBuffer / 4);
        pDescPtr = builder.CreateBitCast(pDescPtr, pDescTy->getPointerTo(ADDR_SPACE_CONST));
        Instruction* pLoad = builder.CreateLoad(pDescTy, pDescPtr);
        pLoad->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(pLoad->getContext(), {}));
        return pLoad;
    }

    // Normal buffer descriptor load.
    // Find the descriptor node, either a DescriptorBuffer or PushConst (inline buffer).
    const ResourceNode* pTopNode = nullptr;
    const ResourceNode* pNode = nullptr;
    std::tie(pTopNode, pNode) = m_pPipelineState->FindResourceNode(ResourceNodeType::DescriptorBuffer,
                                                                   descSet,
                                                                   binding);

    if (pNode == nullptr)
    {
        // We did not find the resource node. Use an undef value.
        return UndefValue::get(VectorType::get(builder.getInt32Ty(), 4));
    }

    if ((pNode == pTopNode) && (pNode->type == ResourceNodeType::DescriptorBufferCompact))
    {
        // This is a compact buffer descriptor (only two dwords) in the top-level table. We special-case
        // that to use user data SGPRs directly, if PatchEntryPointMutate managed to fit the value into
        // user data SGPRs.
        uint32_t resNodeIdx = pTopNode - m_pPipelineState->GetUserDataNodes().data();
        auto pIntfData = m_pPipelineState->GetShaderInterfaceData(m_shaderStage);
        uint32_t argIdx = pIntfData->entryArgIdxs.resNodeValues[resNodeIdx];
        if (argIdx > 0)
        {
            // Resource node isn't spilled. Load its value from function argument.
            Argument* pDescArg = m_pEntryPoint->getArg(argIdx);
            pDescArg->setName(Twine("resNode") + Twine(resNodeIdx));
            // The function argument is a vector of i32. Treat it as an array of <2 x i32> and
            // extract the required array element.
            pArrayOffset = builder.CreateMul(pArrayOffset, builder.getInt32(2));
            Value* pDescDword0 = builder.CreateExtractElement(pDescArg, pArrayOffset);
            pArrayOffset = builder.CreateAdd(pArrayOffset, builder.getInt32(1));
            Value* pDescDword1 = builder.CreateExtractElement(pDescArg, pArrayOffset);
            Value* pDesc = UndefValue::get(VectorType::get(builder.getInt32Ty(), 2));
            pDesc = builder.CreateInsertElement(pDesc, pDescDword0, uint64_t(0));
            pDesc = builder.CreateInsertElement(pDesc, pDescDword1, 1);
            return BuildBufferCompactDesc(pDesc, &*builder.GetInsertPoint());
        }
    }

    // Get a pointer to the descriptor, as a pointer to i32.
    pDescPtr = GetDescPtr(ResourceNodeType::DescriptorBuffer,
                          descSet,
                          binding,
                          pTopNode,
                          pNode,
                          /*shadow=*/false,
                          builder);

    if ((pNode != nullptr) && (pNode->type == ResourceNodeType::PushConst))
    {
        // Inline buffer.
        return BuildInlineBufferDesc(pDescPtr, builder);
    }

    // Add on the index.
    uint32_t dwordStride = DescriptorSizeBuffer / 4;
    if ((pNode != nullptr) && (pNode->type == ResourceNodeType::DescriptorBufferCompact))
    {
        dwordStride = DescriptorSizeBufferCompact / 4;
    }
    pArrayOffset = builder.CreateMul(pArrayOffset, builder.getInt32(dwordStride));
    pDescPtr = builder.CreateGEP(builder.getInt32Ty(), pDescPtr, pArrayOffset);

    if (dwordStride == DescriptorSizeBufferCompact / 4)
    {
        // Load compact buffer descriptor and convert it into a normal buffer descriptor.
        Type* pDescTy = VectorType::get(builder.getInt32Ty(), DescriptorSizeBufferCompact / 4);
        pDescPtr = builder.CreateBitCast(pDescPtr, pDescTy->getPointerTo(ADDR_SPACE_CONST));
        Value* pDesc = builder.CreateLoad(pDescTy, pDescPtr);
        return BuildBufferCompactDesc(pDesc, &*builder.GetInsertPoint());
    }

    // Load normal buffer descriptor.
    Type* pDescTy = VectorType::get(builder.getInt32Ty(), DescriptorSizeBuffer / 4);
    pDescPtr = builder.CreateBitCast(pDescPtr, pDescTy->getPointerTo(ADDR_SPACE_CONST));
    Instruction* pLoad = builder.CreateLoad(pDescTy, pDescPtr);
    pLoad->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(pLoad->getContext(), {}));
    return pLoad;
}

// =====================================================================================================================
// Calculate a buffer descriptor for an inline buffer
Value* PatchDescriptorLoad::BuildInlineBufferDesc(
    Value*        pDescPtr,   // [in] Pointer to inline buffer
    IRBuilder<>&  builder)    // [in] Builder
{
    // Bitcast the pointer to v2i32
    pDescPtr = builder.CreatePtrToInt(pDescPtr, builder.getInt64Ty());
    pDescPtr = builder.CreateBitCast(pDescPtr, VectorType::get(builder.getInt32Ty(), 2));

    // Build descriptor words.
    SqBufRsrcWord1 sqBufRsrcWord1 = {};
    SqBufRsrcWord2 sqBufRsrcWord2 = {};
    SqBufRsrcWord3 sqBufRsrcWord3 = {};

    sqBufRsrcWord1.bits.BASE_ADDRESS_HI = UINT16_MAX;
    sqBufRsrcWord2.bits.NUM_RECORDS = UINT32_MAX;

    sqBufRsrcWord3.bits.DST_SEL_X = BUF_DST_SEL_X;
    sqBufRsrcWord3.bits.DST_SEL_Y = BUF_DST_SEL_Y;
    sqBufRsrcWord3.bits.DST_SEL_Z = BUF_DST_SEL_Z;
    sqBufRsrcWord3.bits.DST_SEL_W = BUF_DST_SEL_W;
    sqBufRsrcWord3.gfx6.NUM_FORMAT = BUF_NUM_FORMAT_UINT;
    sqBufRsrcWord3.gfx6.DATA_FORMAT = BUF_DATA_FORMAT_32;
    assert(sqBufRsrcWord3.u32All == 0x24FAC);

    // DWORD0
    Value* pDesc = UndefValue::get(VectorType::get(builder.getInt32Ty(), 4));
    Value* pDescElem0 = builder.CreateExtractElement(pDescPtr, uint64_t(0));
    pDesc = builder.CreateInsertElement(pDesc, pDescElem0, uint64_t(0));

    // DWORD1
    Value* pDescElem1 = builder.CreateExtractElement(pDescPtr, 1);
    pDescElem1 = builder.CreateAnd(pDescElem1, builder.getInt32(sqBufRsrcWord1.u32All));
    pDesc = builder.CreateInsertElement(pDesc, pDescElem1, 1);

    // DWORD2
    pDesc = builder.CreateInsertElement(pDesc, builder.getInt32(sqBufRsrcWord2.u32All), 2);

    // DWORD3
    pDesc = builder.CreateInsertElement(pDesc, builder.getInt32(sqBufRsrcWord3.u32All), 3);

    return pDesc;
}

// =====================================================================================================================
// Build buffer compact descriptor
Value* PatchDescriptorLoad::BuildBufferCompactDesc(
    Value*       pDesc,          // [in] The buffer descriptor base to build for the buffer compact descriptor
    Instruction* pInsertPoint)   // [in] Insert point
{
    // Extract compact buffer descriptor
    Value* pDescElem0 = ExtractElementInst::Create(pDesc,
                                                    ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                                    "",
                                                    pInsertPoint);

    Value* pDescElem1 = ExtractElementInst::Create(pDesc,
                                                    ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                    "",
                                                    pInsertPoint);

    // Build normal buffer descriptor
    auto pBufDescTy = VectorType::get(Type::getInt32Ty(*m_pContext), 4);
    Value* pBufDesc = UndefValue::get(pBufDescTy);

    // DWORD0
    pBufDesc = InsertElementInst::Create(pBufDesc,
                                        pDescElem0,
                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                        "",
                                        pInsertPoint);

    // DWORD1
    SqBufRsrcWord1 sqBufRsrcWord1 = {};
    sqBufRsrcWord1.bits.BASE_ADDRESS_HI = UINT16_MAX;

    pDescElem1 = BinaryOperator::CreateAnd(pDescElem1,
                                            ConstantInt::get(Type::getInt32Ty(*m_pContext), sqBufRsrcWord1.u32All),
                                            "",
                                            pInsertPoint);

    pBufDesc = InsertElementInst::Create(pBufDesc,
                                        pDescElem1,
                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                        "",
                                        pInsertPoint);

    // DWORD2
    SqBufRsrcWord2 sqBufRsrcWord2 = {};
    sqBufRsrcWord2.bits.NUM_RECORDS = UINT32_MAX;

    pBufDesc = InsertElementInst::Create(pBufDesc,
                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), sqBufRsrcWord2.u32All),
                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 2),
                                        "",
                                        pInsertPoint);

    // DWORD3
    const GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();
    if (gfxIp.major < 10)
    {
        SqBufRsrcWord3 sqBufRsrcWord3 = {};
        sqBufRsrcWord3.bits.DST_SEL_X = BUF_DST_SEL_X;
        sqBufRsrcWord3.bits.DST_SEL_Y = BUF_DST_SEL_Y;
        sqBufRsrcWord3.bits.DST_SEL_Z = BUF_DST_SEL_Z;
        sqBufRsrcWord3.bits.DST_SEL_W = BUF_DST_SEL_W;
        sqBufRsrcWord3.gfx6.NUM_FORMAT = BUF_NUM_FORMAT_UINT;
        sqBufRsrcWord3.gfx6.DATA_FORMAT = BUF_DATA_FORMAT_32;
        assert(sqBufRsrcWord3.u32All == 0x24FAC);

        pBufDesc = InsertElementInst::Create(pBufDesc,
                                            ConstantInt::get(Type::getInt32Ty(*m_pContext), sqBufRsrcWord3.u32All),
                                            ConstantInt::get(Type::getInt32Ty(*m_pContext), 3),
                                            "",
                                            pInsertPoint);
    }
    else if (gfxIp.major == 10)
    {
        SqBufRsrcWord3 sqBufRsrcWord3 = {};
        sqBufRsrcWord3.bits.DST_SEL_X = BUF_DST_SEL_X;
        sqBufRsrcWord3.bits.DST_SEL_Y = BUF_DST_SEL_Y;
        sqBufRsrcWord3.bits.DST_SEL_Z = BUF_DST_SEL_Z;
        sqBufRsrcWord3.bits.DST_SEL_W = BUF_DST_SEL_W;
        sqBufRsrcWord3.gfx10.FORMAT = BUF_FORMAT_32_UINT;
        sqBufRsrcWord3.gfx10.RESOURCE_LEVEL = 1;
        sqBufRsrcWord3.gfx10.OOB_SELECT = 2;
        assert(sqBufRsrcWord3.u32All == 0x21014FAC);

        pBufDesc = InsertElementInst::Create(pBufDesc,
                                            ConstantInt::get(Type::getInt32Ty(*m_pContext), sqBufRsrcWord3.u32All),
                                            ConstantInt::get(Type::getInt32Ty(*m_pContext), 3),
                                            "",
                                            pInsertPoint);
    }
    else
    {
        llvm_unreachable("Not implemented!");
    }

    return pBufDesc;
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM patching opertions for descriptor load.
INITIALIZE_PASS(PatchDescriptorLoad, DEBUG_TYPE,
                "Patch LLVM for descriptor load operations", false, false)
