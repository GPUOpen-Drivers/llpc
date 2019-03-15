/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerImageOp.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerImageOp.
 ***********************************************************************************************************************
 */
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "SPIRVInternal.h"
#include "llpcBuilder.h"
#include "llpcContext.h"
#include "llpcSpirvLowerImageOp.h"

#define DEBUG_TYPE "llpc-spirv-lower-image-op"

using namespace llvm;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char SpirvLowerImageOp::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering operations for image operations
ModulePass* CreateSpirvLowerImageOp()
{
    return new SpirvLowerImageOp();
}

// =====================================================================================================================
SpirvLowerImageOp::SpirvLowerImageOp()
    :
    SpirvLower(ID),
    m_restoreMeta(false)
{
    initializeSpirvLowerImageOpPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerImageOp::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Image-Op\n");

    SpirvLower::Init(&module);

    // Visit module to restore per-instruction metadata
    m_restoreMeta = true;
    visit(m_pModule);
    m_restoreMeta = false;

    // Invoke handling of "call" instruction
    visit(m_pModule);

    for (auto pCallInst: m_imageCalls)
    {
        pCallInst->dropAllReferences();
        pCallInst->eraseFromParent();
    }
    m_imageCalls.clear();

    for (auto pInst : m_imageLoads)
    {
        if (pInst->use_empty())
        {
            pInst->dropAllReferences();
            pInst->eraseFromParent();
            m_imageLoadOperands.erase(pInst);
        }
    }
    m_imageLoads.clear();

    // NOTE: The set of image load operands is the operands of image load instructions. We must free image load
    // instructions first. Otherwise, the user of those image load operands will not be empty.
    for (auto pOperand : m_imageLoadOperands)
    {
        if (pOperand->use_empty())
        {
            pOperand->dropAllReferences();
            pOperand->eraseFromParent();
        }
    }
    m_imageLoadOperands.clear();

    return true;
}

// =====================================================================================================================
// Visits "call" instruction.
void SpirvLowerImageOp::visitCallInst(
    CallInst& callInst) // [in] "Call" instruction
{
    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

    // Skip image lowering operations except entry-points
    if (callInst.getParent()->getParent()->getLinkage() == GlobalValue::InternalLinkage)
    {
        return;
    }

    if (m_restoreMeta)
    {
        // Restore non-uniform metadata from metadata instruction.
        LLPC_ASSERT(strlen(gSPIRVMD::NonUniform) == 16);
        const std::string NonUniformPrefix = std::string("_Z16") + std::string(gSPIRVMD::NonUniform);
        if (pCallee->getName().startswith(NonUniformPrefix))
        {
            auto pNonUniform = callInst.getOperand(0);
            cast<Instruction>(pNonUniform)->setMetadata(gSPIRVMD::NonUniform, m_pContext->GetEmptyMetadataNode());
        }
        return;
    }

    bool isUndefImage = false;

    if (pCallee->getName().startswith(gSPIRVName::ImageCallPrefix))
    {
        ShaderImageCallMetadata imageCallMeta = {};
        LLPC_ASSERT(callInst.getNumArgOperands() >= 2);
        uint32_t metaOperandIdx = callInst.getNumArgOperands() - 1; // Image call metadata is last argument
        imageCallMeta.U32All =  cast<ConstantInt>(callInst.getArgOperand(metaOperandIdx))->getZExtValue();
        std::string mangledName = pCallee->getName();
        auto fmaskMode = GetFmaskMode(imageCallMeta, mangledName);

        if ((imageCallMeta.OpKind == ImageOpWrite) || isImageAtomicOp(imageCallMeta.OpKind))
        {
            m_pContext->GetShaderResourceUsage(m_shaderStage)->resourceWrite = true;
        }
        else if (imageCallMeta.OpKind == ImageOpRead)
        {
            m_pContext->GetShaderResourceUsage(m_shaderStage)->resourceRead = true;
        }

        ConstantInt* pMemoryQualifier = nullptr;

        Value* pResourceDesc    = nullptr;
        Value* pSamplerDesc     = nullptr;
        Value* pTexelBufferDesc = nullptr;
        Value* pFmaskDesc       = nullptr;

        bool hasResource    = (fmaskMode != FmaskOnly);
        bool hasSampler     = ((imageCallMeta.OpKind == ImageOpSample) ||
                               (imageCallMeta.OpKind == ImageOpGather) ||
                               (imageCallMeta.OpKind == ImageOpQueryLod));
        bool hasTexelBuffer = (hasResource && (imageCallMeta.Dim == DimBuffer));
        bool hasFmask       = (fmaskMode != FmaskNone);

        if (isa<LoadInst>(callInst.getOperand(0)))
        {
            // Load from opaque type
            auto pLoadCombined = cast<LoadInst>(callInst.getOperand(0));

            if (hasSampler)
            {
                pSamplerDesc = LoadImageDescriptor(pLoadCombined,
                                                   ResourceMappingNodeType::DescriptorSampler,
                                                   &imageCallMeta,
                                                   &pMemoryQualifier);
            }

            if (hasTexelBuffer)
            {
                pTexelBufferDesc = LoadImageDescriptor(pLoadCombined,
                                                       ResourceMappingNodeType::DescriptorTexelBuffer,
                                                       &imageCallMeta,
                                                       &pMemoryQualifier);
            }
            else if (hasResource)
            {
                pResourceDesc = LoadImageDescriptor(pLoadCombined,
                                                    ResourceMappingNodeType::DescriptorResource,
                                                    &imageCallMeta,
                                                    &pMemoryQualifier);
            }

            if (hasFmask)
            {
                pFmaskDesc = LoadImageDescriptor(pLoadCombined,
                                                 ResourceMappingNodeType::DescriptorFmask,
                                                 &imageCallMeta,
                                                 &pMemoryQualifier);
            }

            m_imageLoads.insert(pLoadCombined);
        }
        else if (isa<CallInst>(callInst.getOperand(0)))
        {
            auto pLoadCall = cast<CallInst>(callInst.getOperand(0));
            mangledName = pLoadCall->getCalledFunction()->getName();
            if (mangledName.find("_Z12SampledImage") == 0)
            {
                // Seperated resource and sampler (from SPIR-V "OpSampledImage")
                if (isa<UndefValue>(pLoadCall->getOperand(0)) || isa<UndefValue>(pLoadCall->getOperand(1)))
                {
                    isUndefImage = true;
                    m_imageLoads.insert(pLoadCall);
                }
                else
                {
                    auto pLoadResource = cast<Instruction>(pLoadCall->getOperand(0));
                    auto pLoadSampler  = cast<Instruction>(pLoadCall->getOperand(1));

                    if (hasSampler)
                    {
                        pSamplerDesc = LoadImageDescriptor(pLoadSampler,
                                                           ResourceMappingNodeType::DescriptorSampler,
                                                           &imageCallMeta,
                                                           &pMemoryQualifier);
                    }

                    if (hasTexelBuffer)
                    {
                        pTexelBufferDesc = LoadImageDescriptor(pLoadResource,
                                                               ResourceMappingNodeType::DescriptorTexelBuffer,
                                                               &imageCallMeta,
                                                               &pMemoryQualifier);
                    }
                    else if (hasResource)
                    {
                        pResourceDesc = LoadImageDescriptor(pLoadResource,
                                                            ResourceMappingNodeType::DescriptorResource,
                                                            &imageCallMeta,
                                                            &pMemoryQualifier);
                    }

                    if (hasFmask)
                    {
                        pFmaskDesc = LoadImageDescriptor(pLoadResource,
                                                         ResourceMappingNodeType::DescriptorFmask,
                                                         &imageCallMeta,
                                                         &pMemoryQualifier);
                    }

                    m_imageLoads.insert(pLoadCall);

                    m_imageLoadOperands.insert(pLoadResource);
                    m_imageLoadOperands.insert(pLoadSampler);
                }
            }
            else if (mangledName.find("_Z5Image") == 0)
            {
                // Resource only (from SPIR-V "OpImage")
                if (isa<UndefValue>(pLoadCall->getOperand(0)))
                {
                    isUndefImage = true;
                    m_imageLoads.insert(pLoadCall);
                }
                else if (isa<LoadInst>(pLoadCall->getOperand(0)))
                {
                    // Extract resource from load instruction
                    auto pLoadResource = cast<LoadInst>(pLoadCall->getOperand(0));

                    if (hasTexelBuffer)
                    {
                        pTexelBufferDesc = LoadImageDescriptor(pLoadResource,
                                                               ResourceMappingNodeType::DescriptorTexelBuffer,
                                                               &imageCallMeta,
                                                               &pMemoryQualifier);
                    }
                    else if (hasResource)
                    {
                        pResourceDesc = LoadImageDescriptor(pLoadResource,
                                                            ResourceMappingNodeType::DescriptorResource,
                                                            &imageCallMeta,
                                                            &pMemoryQualifier);
                    }

                    if (hasFmask)
                    {
                        pFmaskDesc = LoadImageDescriptor(pLoadResource,
                                                         ResourceMappingNodeType::DescriptorFmask,
                                                         &imageCallMeta,
                                                         &pMemoryQualifier);
                    }

                    m_imageLoadOperands.insert(pLoadResource);
                    m_imageLoads.insert(pLoadCall);
                }
                else
                {
                    // Extract resource from separated resource and sampler (from SPIR-V "OpSampledImage")
                    LLPC_ASSERT(isa<CallInst>(pLoadCall->getOperand(0)));
                    m_imageLoads.insert(pLoadCall);
                    pLoadCall = cast<CallInst>(pLoadCall->getOperand(0));

                    mangledName = pLoadCall->getCalledFunction()->getName();
                    LLPC_ASSERT(mangledName.find("_Z12SampledImage") == 0);

                    auto pLoadResource = cast<LoadInst>(pLoadCall->getOperand(0));

                    if (hasTexelBuffer)
                    {
                        pTexelBufferDesc = LoadImageDescriptor(pLoadResource,
                                                               ResourceMappingNodeType::DescriptorTexelBuffer,
                                                               &imageCallMeta,
                                                               &pMemoryQualifier);
                    }
                    else if (hasResource)
                    {
                        pResourceDesc = LoadImageDescriptor(pLoadResource,
                                                            ResourceMappingNodeType::DescriptorResource,
                                                            &imageCallMeta,
                                                            &pMemoryQualifier);
                    }

                    if (hasFmask)
                    {
                        pFmaskDesc = LoadImageDescriptor(pLoadResource,
                                                         ResourceMappingNodeType::DescriptorFmask,
                                                         &imageCallMeta,
                                                         &pMemoryQualifier);
                    }

                    m_imageLoadOperands.insert(pLoadCall);
                }
            }
        }
        else if (isa<UndefValue>(callInst.getOperand(0)))
        {
            isUndefImage = true;
        }

        if (isUndefImage)
        {
            // Replace undef-image call with undefined value.
            auto pUndef = UndefValue::get(callInst.getType());
            callInst.replaceAllUsesWith(pUndef);
            m_imageCalls.insert(&callInst);
        }
        else
        {
            mangledName = pCallee->getName();

            std::vector<Value*> args;

            if ((imageCallMeta.OpKind == ImageOpSample) ||
                (imageCallMeta.OpKind == ImageOpGather) ||
                (imageCallMeta.OpKind == ImageOpQueryLod))
            {
                // Add sampler only for image sample and image gather
                LLPC_ASSERT(pSamplerDesc != nullptr);
                args.push_back(pSamplerDesc);
            }

            imageCallMeta.WriteOnly = callInst.getType()->isVoidTy();

            if (fmaskMode != FmaskOnly)
            {
                if (imageCallMeta.Dim != DimBuffer)
                {
                    LLPC_ASSERT(pResourceDesc != nullptr);
                    args.push_back(pResourceDesc);
                }
                else
                {
                    LLPC_ASSERT(pTexelBufferDesc != nullptr);
                    args.push_back(pTexelBufferDesc);
                }
            }

            if (hasFmask)
            {
                LLPC_ASSERT(pFmaskDesc != nullptr);
                args.push_back(pFmaskDesc);
            }

            if (imageCallMeta.OpKind != ImageOpQueryNonLod)
            {
                // NOTE: Here, we reduce the size of coordinate to its actual size. According to SPIR-V spec, coordinate
                // is allowed to be a vector larger than needed, this will cause LLVM type mismatch when linking.
                Dim dim = static_cast<Dim>(imageCallMeta.Dim);
                uint32_t requiredCompCount;
                SPIRVDimCoordNumMap::find(dim, &requiredCompCount);
                if (imageCallMeta.Arrayed)
                {
                    ++requiredCompCount;
                }
                if (mangledName.find(gSPIRVName::ImageCallModProj) != std::string::npos)
                {
                    ++requiredCompCount;
                }

                Value* pCoord = callInst.getArgOperand(1);
                Type* pCoordTy = pCoord->getType();

                bool coordIsVec = pCoordTy->isVectorTy();
                uint32_t coordCompCount = coordIsVec ? pCoordTy->getVectorNumElements() : 1;
                Type* pCoordCompTy = coordIsVec ? pCoordTy->getVectorElementType() : pCoordTy;

                if (coordCompCount > requiredCompCount)
                {
                    // Need vector size reduction for coordinate
                    VectorType* pNewCoordTy = VectorType::get(pCoordCompTy, requiredCompCount);
                    Value* coordComps[] = { nullptr, nullptr, nullptr, nullptr };
                    for (uint32_t i = 0; i < requiredCompCount; ++i)
                    {
                        coordComps[i] = ExtractElementInst::Create(pCoord,
                                                                   ConstantInt::get(m_pContext->Int32Ty(), i, true),
                                                                   "",
                                                                   &callInst);
                    }

                    if (requiredCompCount == 1)
                    {
                        args.push_back(coordComps[0]);
                    }
                    else
                    {
                        Value* pNewCoord = UndefValue::get(pNewCoordTy);
                        for (uint32_t i = 0; i < requiredCompCount; ++i)
                        {
                            pNewCoord = InsertElementInst::Create(pNewCoord,
                                                                  coordComps[i],
                                                                  ConstantInt::get(m_pContext->Int32Ty(), i, true),
                                                                  "",
                                                                  &callInst);
                        }
                        args.push_back(pNewCoord);
                    }
                }
                else
                {
                    if (dim == DimSubpassData)
                    {
                        LLPC_ASSERT(m_shaderStage == ShaderStageFragment);

                        Value* pFragCoord = GetFragCoord(&callInst);
                        m_pBuilder->SetInsertPoint(&callInst);

                        const auto enableMultiView = (reinterpret_cast<const GraphicsPipelineBuildInfo*>(
                            m_pContext->GetPipelineBuildInfo()))->iaState.enableMultiView;

                        if (enableMultiView)
                        {
                            // For subpass data with multiview, the coordinate is formed of
                            // ( fptosi(fragcoord.x), fptosi(fragcoord.y), viewindex )
                            Value* pViewIndex = GetViewIndex(&callInst);

                            // Narrow pFragCoord from <4 x float> to <3 x float>.
                            pCoord = m_pBuilder->CreateShuffleVector(pFragCoord, pFragCoord, { 0, 1, 4 }, "");

                            // Convert to int and insert view index as element 2.
                            pCoord = m_pBuilder->CreateFPToSI(pCoord, m_pContext->Int32x3Ty(), "");
                            pCoord = m_pBuilder->CreateInsertElement(pCoord, pViewIndex, 2, "");
                        }
                        else
                        {
                            // For subpass data without multiview, we add
                            // ( fptosi(fragcoord.x), fptosi(fragcoord.y) ) to the provided coordinate.
                            pFragCoord = m_pBuilder->CreateShuffleVector(pFragCoord, pFragCoord, { 0, 1 }, "");
                            pFragCoord = m_pBuilder->CreateFPToSI(pFragCoord, m_pContext->Int32x2Ty(), "");
                            pCoord = m_pBuilder->CreateAdd(pFragCoord, pCoord, "");
                        }
                    }
                    args.push_back(pCoord);
                }

                for (uint32_t i = 2; i < callInst.getNumArgOperands() - 1; ++i)
                {
                    auto pArg = callInst.getArgOperand(i);
                    args.push_back(pArg);
                }
                // ImageCallMeta may be changed due to non-uniform index, so we can't copy it from callInst.
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), imageCallMeta.U32All));
            }
            else
            {
                for (uint32_t i = 1; i < callInst.getNumArgOperands() - 1; ++i)
                {
                    auto pArg = callInst.getArgOperand(i);
                    args.push_back(pArg);
                }
                // ImageCallMeta may be changed due to non-uniform index, so we can't copy it from callInst.
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), imageCallMeta.U32All));
            }

            bool isCoherent = false;
            bool isVolatile = false;

            if (pMemoryQualifier != nullptr)
            {
                ShaderImageMemoryMetadata imageMemoryMeta = {};
                imageMemoryMeta.U32All = pMemoryQualifier->getZExtValue();
                isCoherent = imageMemoryMeta.Coherent;
                isVolatile = imageMemoryMeta.Volatile;
            }

            size_t searchPos = mangledName.find(gSPIRVName::ImageCallMakeTexelVisible);
            if (searchPos != std::string::npos)
            {
                mangledName.erase(searchPos, strlen(gSPIRVName::ImageCallMakeTexelVisible));
                const uint64_t scope = cast<ConstantInt>(args.back())->getZExtValue();
                args.pop_back();

                // Memory model coherency is defined per call
                isCoherent |= (scope != ScopeInvocation);
            }

            searchPos = mangledName.find(gSPIRVName::ImageCallMakeTexelAvailable);
            if (searchPos != std::string::npos)
            {
                mangledName.erase(searchPos, strlen(gSPIRVName::ImageCallMakeTexelAvailable));
                const uint64_t scope = cast<ConstantInt>(args.back())->getZExtValue();
                args.pop_back();

                // Memory model coherency is defined per call
                isCoherent |= (scope != ScopeInvocation);
            }

            searchPos = mangledName.find(gSPIRVName::ImageCallNonPrivateTexel);
            if (searchPos != std::string::npos)
            {
                mangledName.erase(searchPos, strlen(gSPIRVName::ImageCallMakeTexelAvailable));
            }

            searchPos = mangledName.find(gSPIRVName::ImageCallVolatileTexel);
            if (searchPos != std::string::npos)
            {
                mangledName.erase(searchPos, strlen(gSPIRVName::ImageCallVolatileTexel));

                // Memory model volatility is defined per call
                isVolatile = true;
            }

            // Process image memory metadata
            if ((imageCallMeta.OpKind == ImageOpRead) || (imageCallMeta.OpKind == ImageOpWrite))
            {
                args.pop_back();
                CoherentFlag coherentFlag = {};
                coherentFlag.bits.glc = isCoherent;
                coherentFlag.bits.slc = isVolatile;
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), coherentFlag.u32All)); // coherent
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), imageCallMeta.U32All)); //imageCallMeta
            }
            else if (isImageAtomicOp(imageCallMeta.OpKind))
            {
                args.pop_back();
                CoherentFlag coherentFlag = {};
                coherentFlag.bits.slc = isVolatile;
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), coherentFlag.u32All)); // coherent
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), imageCallMeta.U32All)); //imageCallMeta
            }

            if (imageCallMeta.OpKind == ImageOpQueryNonLod)
            {
                // Format: ...".query.op.imagesig.dim[.rettype]"...
                size_t pos = mangledName.find(gSPIRVName::ImageCallQueryNonLodPrefix);
                LLPC_ASSERT(pos != std::string::npos);

                // Skip the query operation name
                pos = mangledName.find(".", pos + 1);

                // Find the name string for image signature and remove it
                size_t startPos = mangledName.find(".", pos + 1);
                size_t endPos   = mangledName.find(".", startPos + 1);
                if (endPos == std::string::npos)
                {
                    endPos = mangledName.length();
                }
                mangledName = mangledName.substr(0, startPos) + mangledName.substr(endPos);
            }
            else if (imageCallMeta.OpKind == ImageOpQueryLod)
            {
                // NOTE: "Array" and "shadow" modifiers do not have real impacts.
                // They are only to keep function uniqueness (avoid overloading).
                // Format: ...".querylod.dim"
                size_t pos = mangledName.find(gSPIRVName::ImageCallQueryLodPrefix);
                LLPC_ASSERT(pos != std::string::npos);
                pos = mangledName.find(".", pos + 1);
                size_t modPos = mangledName.find("Array", pos + 1);
                if (modPos != std::string::npos)
                {
                    mangledName = mangledName.substr(0, modPos);
                }

                modPos = mangledName.find("Shadow", pos);
                if (modPos != std::string::npos)
                {
                    mangledName = mangledName.substr(0, modPos);
                }
            }

            // Change the name prefix of image call (from "spirv.image" to "llpc.image")
            StringRef prefix(gSPIRVName::ImageCallPrefix);
            std::string callName = LlpcName::ImageCallPrefix + mangledName.substr(prefix.size());

            if (imageCallMeta.Dim != DimBuffer)
            {
                callName += gSPIRVName::ImageCallDimAwareSuffix;
            }

            PatchImageCallForFmask(imageCallMeta, fmaskMode, callName);

            // Image call replacement
            Instruction* pImageCall =
                cast<CallInst>(EmitCall(m_pModule, callName, callInst.getType(), args, NoAttrib, &callInst));

            // Add waterfall loop if necessary.
            SmallVector<uint32_t, 2> nonUniformOperandIdxs;
            if (imageCallMeta.NonUniformResource)
            {
                if ((imageCallMeta.OpKind == ImageOpSample) ||
                    (imageCallMeta.OpKind == ImageOpGather) ||
                    (imageCallMeta.OpKind == ImageOpQueryLod))
                {
                    nonUniformOperandIdxs.push_back(1);
                }
                else
                {
                    nonUniformOperandIdxs.push_back(0);
                }
            }

            if (imageCallMeta.NonUniformSampler)
            {
                nonUniformOperandIdxs.push_back(0);
            }

            if (nonUniformOperandIdxs.empty() == false)
            {
                pImageCall = m_pBuilder->CreateWaterfallLoop(pImageCall, nonUniformOperandIdxs);
            }

            if (pImageCall != nullptr)
            {
                callInst.replaceAllUsesWith(pImageCall);
            }
            m_imageCalls.insert(&callInst);
        }
    }
}

// =====================================================================================================================
// Loads image descriptor from the provided load source value. Meanwhile, extract image call metadata and memory
// qualifier.
llvm::Value* SpirvLowerImageOp::LoadImageDescriptor(
    Value*                   pLoadSrc,                // [in] Source value from which the descriptor is loaded
    ResourceMappingNodeType  descType,                // Descriptor type
    ShaderImageCallMetadata* pImageCallMeta,          // [out] Image call metadata
    ConstantInt**            ppMemoryQualifier)       // [out] Memory qualifier
{
    Value*       pDesc = nullptr;

    if (isa<SelectInst>(pLoadSrc))
    {
        auto pSelectInst = cast<SelectInst>(pLoadSrc);
        Value* pValue1 = pSelectInst->getTrueValue();
        Value* pValue2 = pSelectInst->getFalseValue();
        pValue1  = LoadImageDescriptor(pValue1,  descType, pImageCallMeta, ppMemoryQualifier);
        pValue2 = LoadImageDescriptor(pValue2, descType, pImageCallMeta, ppMemoryQualifier);
        pDesc       = SelectInst::Create(pSelectInst->getCondition(), pValue1, pValue2, "", pSelectInst);
    }
    else if (isa<LoadInst>(pLoadSrc))
    {
        ConstantInt* pDescSet    = nullptr;
        ConstantInt* pBinding    = nullptr;
        Value*       pArrayIndex = nullptr;

        LoadInst* pLoadInst = cast<LoadInst>(pLoadSrc);
        Value* pLoadSrc = pLoadInst->getOperand(0);
        MDNode* pResMetaNode = nullptr;
        MDNode* pImageMemoryMetaNode = nullptr;

        GetElementPtrInst* pGetElemPtrInst = nullptr;
        Instruction* pConstExpr = nullptr;

        if (isa<GetElementPtrInst>(pLoadSrc))
        {
            pGetElemPtrInst = dyn_cast<GetElementPtrInst>(pLoadSrc);
        }
        else if (isa<ConstantExpr>(pLoadSrc))
        {
            pConstExpr = dyn_cast<ConstantExpr>(pLoadSrc)->getAsInstruction();
            pGetElemPtrInst = dyn_cast<GetElementPtrInst>(pConstExpr);
        }

        // Calculate descriptor index for arrayed binding
        if (pGetElemPtrInst != nullptr)
        {
            // Process image array access

            // Get stride of each array dimension
            std::vector<uint32_t> strides;
            Type* pSrcTy = pGetElemPtrInst->getSourceElementType();
            LLPC_ASSERT(pSrcTy->isArrayTy());

            Type* pElemTy = pSrcTy->getArrayElementType();
            while (pElemTy->isArrayTy())
            {
                const uint32_t elemCount = pElemTy->getArrayNumElements();
                for (uint32_t i = 0; i < strides.size(); ++i)
                {
                    strides[i] *= elemCount;
                }

                strides.push_back(elemCount);
                pElemTy = pElemTy->getArrayElementType();
            }
            strides.push_back(1);

            // Calculate flatten array index
            const uint32_t operandCount = pGetElemPtrInst->getNumOperands();
            LLPC_ASSERT((operandCount - 2) == strides.size());

            for (uint32_t i = 2; i < operandCount; ++i)
            {
                Value* pIndex = pGetElemPtrInst->getOperand(i);
                bool is64bit  = (pIndex->getType()->getPrimitiveSizeInBits() == 64);
                Constant* pStride = ConstantInt::get(m_pContext->Int32Ty(), strides[i - 2]);

                if (is64bit)
                {
                    pIndex = new TruncInst(pIndex, m_pContext->Int32Ty(), "", pLoadInst);
                }
                pIndex = BinaryOperator::CreateMul(pStride, pIndex, "", pLoadInst);
                if (pArrayIndex == nullptr)
                {
                    pArrayIndex = pIndex;
                }
                else
                {
                    pArrayIndex = BinaryOperator::CreateAdd(pArrayIndex, pIndex, "", pLoadInst);
                }
            }

            // Get resource binding metadata node from global variable
            Value*  pPointer = pGetElemPtrInst->getPointerOperand();
            pResMetaNode = cast<GlobalVariable>(pPointer)->getMetadata(gSPIRVMD::Resource);
            pImageMemoryMetaNode = cast<GlobalVariable>(pPointer)->getMetadata(gSPIRVMD::ImageMemory);
        }
        else
        {
            // Load image from global variable
            pArrayIndex = ConstantInt::get(m_pContext->Int32Ty(), 0);

            // Get resource binding metadata node from global variable
            pResMetaNode = cast<GlobalVariable>(pLoadSrc)->getMetadata(gSPIRVMD::Resource);
            pImageMemoryMetaNode = cast<GlobalVariable>(pLoadSrc)->getMetadata(gSPIRVMD::ImageMemory);
        }

        if (pConstExpr != nullptr)
        {
            pConstExpr->dropAllReferences();
            pConstExpr->deleteValue();
        }

        // Get descriptor set and descriptor binding
        LLPC_ASSERT(pResMetaNode != nullptr);

        pDescSet = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(0));
        pBinding = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(1));

        if (pImageMemoryMetaNode != nullptr)
        {
            *ppMemoryQualifier = mdconst::dyn_extract<ConstantInt>(pImageMemoryMetaNode->getOperand(0));
        }

        std::unordered_set<Value*> checkedValues;
        bool isNonUniform = IsNonUniformValue(pArrayIndex, checkedValues);

        m_pBuilder->SetInsertPoint(cast<Instruction>(pLoadInst));

        switch (descType)
        {
        case ResourceMappingNodeType::DescriptorSampler:
            {
                pImageCallMeta->NonUniformSampler = isNonUniform ? 1 : 0;
                pDesc = m_pBuilder->CreateLoadSamplerDesc(
                    pDescSet->getZExtValue(),
                    pBinding->getZExtValue(),
                    pArrayIndex,
                    isNonUniform);
                break;
            }
        case ResourceMappingNodeType::DescriptorResource:
            {
                pImageCallMeta->NonUniformResource = isNonUniform ? 1 : 0;
                pDesc = m_pBuilder->CreateLoadResourceDesc(
                    pDescSet->getZExtValue(),
                    pBinding->getZExtValue(),
                    pArrayIndex,
                    isNonUniform);
                break;
            }
        case ResourceMappingNodeType::DescriptorTexelBuffer:
            {
                pImageCallMeta->NonUniformResource = isNonUniform ? 1 : 0;
                pDesc = m_pBuilder->CreateLoadTexelBufferDesc(
                    pDescSet->getZExtValue(),
                    pBinding->getZExtValue(),
                    pArrayIndex,
                    isNonUniform);
                break;
            }
        case ResourceMappingNodeType::DescriptorFmask:
            {
                pImageCallMeta->NonUniformResource = isNonUniform ? 1 : 0;
                pDesc = m_pBuilder->CreateLoadFmaskDesc(
                    pDescSet->getZExtValue(),
                    pBinding->getZExtValue(),
                    pArrayIndex,
                    isNonUniform);
                break;
            }
        }
    }
    else
    {
        LLPC_NEVER_CALLED();
    }
    return pDesc;
}

// =====================================================================================================================
// Get fmask mode: FmaskNone, FmaskBased, FmaskOnly
SpirvLowerImageOp::FmaskMode SpirvLowerImageOp::GetFmaskMode(
    const ShaderImageCallMetadata&  imageCallMeta,    // [in] Image call metadata
    const std::string&              callName)         // [in] Call name
{
    // For multi-sampled image, F-mask is only taken into account for texel fetch (not for query)
    if ((imageCallMeta.Multisampled == false) || (imageCallMeta.OpKind == ImageOpQueryNonLod))
    {
        return FmaskNone;
    }

    if (callName.find(gSPIRVName::ImageCallModPatchFmaskUsage) == std::string::npos)
    {
        if (callName.find(gSPIRVName::ImageCallModFmaskValue) != std::string::npos)
        {
            return FmaskOnly;
        }
        return FmaskNone;
    }

    // Fmask based fetch only can work for texel fetch or load subpass data
    if((imageCallMeta.OpKind == ImageOpFetch) ||
       ((imageCallMeta.OpKind == ImageOpRead) &&
        (imageCallMeta.Dim == DimSubpassData)))
    {
        return FmaskBased;
    }
    return FmaskNone;
}

// =====================================================================================================================
// Modify the about-to-be-emitted image call name for multisampled or subpass data
void SpirvLowerImageOp::PatchImageCallForFmask(
    const ShaderImageCallMetadata&  imageCallMeta,    // [in] Image call metadata
    FmaskMode                       fmaskMode,        // F-mask mode
    std::string&                    callName)         // [in/out] Name of image call
{
    // For multi-sampled image, F-mask is only taken into account for texel fetch (not for query)
    if (imageCallMeta.Multisampled && (imageCallMeta.OpKind != ImageOpQueryNonLod))
    {
        auto fmaskPatchPos = callName.find(gSPIRVName::ImageCallModPatchFmaskUsage);
        if (fmaskPatchPos != std::string::npos)
        {
            std::string fmaskPatchString = "";
            switch (fmaskMode)
            {
            case FmaskBased:
                // F-mask based fetch only can work for texel fetch or load subpass data
                if((imageCallMeta.OpKind == ImageOpFetch) ||
                    ((imageCallMeta.OpKind == ImageOpRead) &&
                    (imageCallMeta.Dim == DimSubpassData)))
                {
                    fmaskPatchString = gSPIRVName::ImageCallModFmaskBased;
                }
                break;
            case FmaskOnly:
                fmaskPatchString = gSPIRVName::ImageCallModFmaskId;
                break;
            case FmaskNone:
                break;
            default:
                LLPC_NEVER_CALLED();
                break;
            }

            callName = callName.replace(fmaskPatchPos,
                                        strlen(gSPIRVName::ImageCallModPatchFmaskUsage),
                                        fmaskPatchString);
        }
    }

    if (imageCallMeta.Dim == DimSubpassData)
    {
        const auto enableMultiView = (reinterpret_cast<const GraphicsPipelineBuildInfo*>(
            m_pContext->GetPipelineBuildInfo()))->iaState.enableMultiView;

        if (enableMultiView)
        {
            // Replace dimension SubpassData with SubpassDataArray
            std::string dimSubpassData = SPIRVDimNameMap::map(DimSubpassData);
            callName.replace(callName.find(dimSubpassData), dimSubpassData.length(), dimSubpassData + "Array");
        }
    }
}

// =====================================================================================================================
// Emits import call to get the value of built-in gl_FragCoord
Value* SpirvLowerImageOp::GetFragCoord(
    Instruction* pInsertPos)    // [in] Insert code before this instruction
{
    LLPC_ASSERT(m_shaderStage == ShaderStageFragment);
    std::string instName = LlpcName::InputImportBuiltIn;
    instName += ".FragCoord";

    std::vector<Value*> args;
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), BuiltInFragCoord));
    auto pReturnTy = m_pContext->Floatx4Ty();
    AddTypeMangling(pReturnTy, args, instName);
    return EmitCall(m_pModule, instName, pReturnTy, args, NoAttrib, pInsertPos);
}

// =====================================================================================================================
// Emits import call to get the value of built-in gl_ViewIndex
Value* SpirvLowerImageOp::GetViewIndex(
    Instruction* pInsertPos)    // [in] Insert code before this instruction
{
    LLPC_ASSERT(m_shaderStage == ShaderStageFragment);
    std::string instName = LlpcName::InputImportBuiltIn;
    instName += ".ViewIndex";

    std::vector<Value*> args;
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), BuiltInViewIndex));
    auto pReturnTy = m_pContext->Int32Ty();
    AddTypeMangling(pReturnTy, args, instName);
    return EmitCall(m_pModule, instName, pReturnTy, args, NoAttrib, pInsertPos);
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for image operations.
INITIALIZE_PASS(SpirvLowerImageOp, DEBUG_TYPE,
                "Lower SPIR-V image operations (sample, fetch, gather, read/write)", false, false)
