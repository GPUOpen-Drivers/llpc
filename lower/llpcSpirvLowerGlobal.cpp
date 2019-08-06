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
 * @file  llpcSpirvLowerGlobal.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerGlobal.
 ***********************************************************************************************************************
 */

#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Instructions.h"
#include "llvm/PassSupport.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <unordered_set>
#include "SPIRVInternal.h"
#include "llpcBuilder.h"
#include "llpcContext.h"
#include "llpcSpirvLowerGlobal.h"

#define DEBUG_TYPE "llpc-spirv-lower-global"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc
{

// The code here relies on the SPIR-V built-in kind being the same as the Builder built-in kind.

static_assert(Builder::BuiltInBaryCoordNoPersp          == static_cast<Builder::BuiltInKind>(spv::BuiltInBaryCoordNoPerspAMD), "Built-in kind mismatch");
static_assert(Builder::BuiltInBaryCoordNoPerspCentroid  == static_cast<Builder::BuiltInKind>(spv::BuiltInBaryCoordNoPerspCentroidAMD), "Built-in kind mismatch");
static_assert(Builder::BuiltInBaryCoordNoPerspSample    == static_cast<Builder::BuiltInKind>(spv::BuiltInBaryCoordNoPerspSampleAMD), "Built-in kind mismatch");
static_assert(Builder::BuiltInBaryCoordPullModel        == static_cast<Builder::BuiltInKind>(spv::BuiltInBaryCoordPullModelAMD), "Built-in kind mismatch");
static_assert(Builder::BuiltInBaryCoordSmooth           == static_cast<Builder::BuiltInKind>(spv::BuiltInBaryCoordSmoothAMD), "Built-in kind mismatch");
static_assert(Builder::BuiltInBaryCoordSmoothCentroid   == static_cast<Builder::BuiltInKind>(spv::BuiltInBaryCoordSmoothCentroidAMD), "Built-in kind mismatch");
static_assert(Builder::BuiltInBaryCoordSmoothSample     == static_cast<Builder::BuiltInKind>(spv::BuiltInBaryCoordSmoothSampleAMD), "Built-in kind mismatch");
static_assert(Builder::BuiltInBaseInstance              == static_cast<Builder::BuiltInKind>(spv::BuiltInBaseInstance), "Built-in kind mismatch");
static_assert(Builder::BuiltInBaseVertex                == static_cast<Builder::BuiltInKind>(spv::BuiltInBaseVertex), "Built-in kind mismatch");
static_assert(Builder::BuiltInClipDistance              == static_cast<Builder::BuiltInKind>(spv::BuiltInClipDistance), "Built-in kind mismatch");
static_assert(Builder::BuiltInCullDistance              == static_cast<Builder::BuiltInKind>(spv::BuiltInCullDistance), "Built-in kind mismatch");
static_assert(Builder::BuiltInDeviceIndex               == static_cast<Builder::BuiltInKind>(spv::BuiltInDeviceIndex), "Built-in kind mismatch");
static_assert(Builder::BuiltInDrawIndex                 == static_cast<Builder::BuiltInKind>(spv::BuiltInDrawIndex), "Built-in kind mismatch");
static_assert(Builder::BuiltInFragCoord                 == static_cast<Builder::BuiltInKind>(spv::BuiltInFragCoord), "Built-in kind mismatch");
static_assert(Builder::BuiltInFragDepth                 == static_cast<Builder::BuiltInKind>(spv::BuiltInFragDepth), "Built-in kind mismatch");
static_assert(Builder::BuiltInFragStencilRef            == static_cast<Builder::BuiltInKind>(spv::BuiltInFragStencilRefEXT), "Built-in kind mismatch");
static_assert(Builder::BuiltInFrontFacing               == static_cast<Builder::BuiltInKind>(spv::BuiltInFrontFacing), "Built-in kind mismatch");
static_assert(Builder::BuiltInGlobalInvocationId        == static_cast<Builder::BuiltInKind>(spv::BuiltInGlobalInvocationId), "Built-in kind mismatch");
static_assert(Builder::BuiltInHelperInvocation          == static_cast<Builder::BuiltInKind>(spv::BuiltInHelperInvocation), "Built-in kind mismatch");
static_assert(Builder::BuiltInInstanceIndex             == static_cast<Builder::BuiltInKind>(spv::BuiltInInstanceIndex), "Built-in kind mismatch");
static_assert(Builder::BuiltInInvocationId              == static_cast<Builder::BuiltInKind>(spv::BuiltInInvocationId), "Built-in kind mismatch");
static_assert(Builder::BuiltInLayer                     == static_cast<Builder::BuiltInKind>(spv::BuiltInLayer), "Built-in kind mismatch");
static_assert(Builder::BuiltInLocalInvocationId         == static_cast<Builder::BuiltInKind>(spv::BuiltInLocalInvocationId), "Built-in kind mismatch");
static_assert(Builder::BuiltInLocalInvocationIndex      == static_cast<Builder::BuiltInKind>(spv::BuiltInLocalInvocationIndex), "Built-in kind mismatch");
static_assert(Builder::BuiltInNumSubgroups              == static_cast<Builder::BuiltInKind>(spv::BuiltInNumSubgroups), "Built-in kind mismatch");
static_assert(Builder::BuiltInNumWorkgroups             == static_cast<Builder::BuiltInKind>(spv::BuiltInNumWorkgroups), "Built-in kind mismatch");
static_assert(Builder::BuiltInPatchVertices             == static_cast<Builder::BuiltInKind>(spv::BuiltInPatchVertices), "Built-in kind mismatch");
static_assert(Builder::BuiltInPointCoord                == static_cast<Builder::BuiltInKind>(spv::BuiltInPointCoord), "Built-in kind mismatch");
static_assert(Builder::BuiltInPointSize                 == static_cast<Builder::BuiltInKind>(spv::BuiltInPointSize), "Built-in kind mismatch");
static_assert(Builder::BuiltInPosition                  == static_cast<Builder::BuiltInKind>(spv::BuiltInPosition), "Built-in kind mismatch");
static_assert(Builder::BuiltInPrimitiveId               == static_cast<Builder::BuiltInKind>(spv::BuiltInPrimitiveId), "Built-in kind mismatch");
static_assert(Builder::BuiltInSampleId                  == static_cast<Builder::BuiltInKind>(spv::BuiltInSampleId), "Built-in kind mismatch");
static_assert(Builder::BuiltInSampleMask                == static_cast<Builder::BuiltInKind>(spv::BuiltInSampleMask), "Built-in kind mismatch");
static_assert(Builder::BuiltInSamplePosition            == static_cast<Builder::BuiltInKind>(spv::BuiltInSamplePosition), "Built-in kind mismatch");
static_assert(Builder::BuiltInSubgroupEqMask            == static_cast<Builder::BuiltInKind>(spv::BuiltInSubgroupEqMask), "Built-in kind mismatch");
static_assert(Builder::BuiltInSubgroupGeMask            == static_cast<Builder::BuiltInKind>(spv::BuiltInSubgroupGeMask), "Built-in kind mismatch");
static_assert(Builder::BuiltInSubgroupGtMask            == static_cast<Builder::BuiltInKind>(spv::BuiltInSubgroupGtMask), "Built-in kind mismatch");
static_assert(Builder::BuiltInSubgroupId                == static_cast<Builder::BuiltInKind>(spv::BuiltInSubgroupId), "Built-in kind mismatch");
static_assert(Builder::BuiltInSubgroupLeMask            == static_cast<Builder::BuiltInKind>(spv::BuiltInSubgroupLeMask), "Built-in kind mismatch");
static_assert(Builder::BuiltInSubgroupLocalInvocationId == static_cast<Builder::BuiltInKind>(spv::BuiltInSubgroupLocalInvocationId), "Built-in kind mismatch");
static_assert(Builder::BuiltInSubgroupLtMask            == static_cast<Builder::BuiltInKind>(spv::BuiltInSubgroupLtMask), "Built-in kind mismatch");
static_assert(Builder::BuiltInSubgroupSize              == static_cast<Builder::BuiltInKind>(spv::BuiltInSubgroupSize), "Built-in kind mismatch");
static_assert(Builder::BuiltInTessCoord                 == static_cast<Builder::BuiltInKind>(spv::BuiltInTessCoord), "Built-in kind mismatch");
static_assert(Builder::BuiltInTessLevelInner            == static_cast<Builder::BuiltInKind>(spv::BuiltInTessLevelInner), "Built-in kind mismatch");
static_assert(Builder::BuiltInTessLevelOuter            == static_cast<Builder::BuiltInKind>(spv::BuiltInTessLevelOuter), "Built-in kind mismatch");
static_assert(Builder::BuiltInVertexIndex               == static_cast<Builder::BuiltInKind>(spv::BuiltInVertexIndex), "Built-in kind mismatch");
static_assert(Builder::BuiltInViewIndex                 == static_cast<Builder::BuiltInKind>(spv::BuiltInViewIndex), "Built-in kind mismatch");
static_assert(Builder::BuiltInViewportIndex             == static_cast<Builder::BuiltInKind>(spv::BuiltInViewportIndex), "Built-in kind mismatch");
static_assert(Builder::BuiltInWorkgroupId               == static_cast<Builder::BuiltInKind>(spv::BuiltInWorkgroupId), "Built-in kind mismatch");

// =====================================================================================================================
// Initializes static members.
char SpirvLowerGlobal::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering operations for globals
ModulePass* CreateSpirvLowerGlobal()
{
    return new SpirvLowerGlobal();
}

// =====================================================================================================================
SpirvLowerGlobal::SpirvLowerGlobal()
    :
    SpirvLower(ID),
    m_pRetBlock(nullptr),
    m_lowerInputInPlace(false),
    m_lowerOutputInPlace(false)
{
    initializeSpirvLowerGlobalPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerGlobal::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Global\n");

    SpirvLower::Init(&module);

    // Map globals to proxy variables
    for (auto pGlobal = m_pModule->global_begin(), pEnd = m_pModule->global_end(); pGlobal != pEnd; ++pGlobal)
    {
        if (pGlobal->getType()->getAddressSpace() == SPIRAS_Private)
        {
            MapGlobalVariableToProxy(&*pGlobal);
        }
        else if (pGlobal->getType()->getAddressSpace() == SPIRAS_Input)
        {
            MapInputToProxy(&*pGlobal);
        }
        else if (pGlobal->getType()->getAddressSpace() == SPIRAS_Output)
        {
            MapOutputToProxy(&*pGlobal);
        }
    }

    // NOTE: Global variable, inlcude general global variable, input and output is a special constant variable, so if
    // it is referenced by constant expression, we need translate constant expression to normal instruction first,
    // Otherwise, we will hit assert in replaceAllUsesWith() when we replace global variable with proxy variable.
    RemoveConstantExpr();

    // Do lowering operations
    LowerGlobalVar();

    if (m_lowerInputInPlace && m_lowerOutputInPlace)
    {
        // Both input and output have to be lowered in-place (without proxy variables)
        LowerInOutInPlace(); // Just one lowering operation is sufficient
    }
    else
    {
        // Either input or output has to be lowered in-place, not both
        if (m_lowerInputInPlace)
        {
            LowerInOutInPlace();
        }
        else
        {
            LowerInput();
        }

        if (m_lowerOutputInPlace)
        {
            LowerInOutInPlace();
        }
        else
        {
            LowerOutput();
        }
    }

    LowerBufferBlock();
    LowerPushConsts();

    return true;
}

// =====================================================================================================================
// Visits "return" instruction.
void SpirvLowerGlobal::visitReturnInst(
    ReturnInst& retInst)    // [in] "Ret" instruction
{
    // Skip if "return" instructions are not expected to be handled.
    if (m_instVisitFlags.checkReturn == false)
    {
        return;
    }

    // We only handle the "return" in entry point
    if (retInst.getParent()->getParent()->getLinkage() == GlobalValue::InternalLinkage)
    {
        return;
    }

    LLPC_ASSERT(m_pRetBlock != nullptr); // Must have been created
    BranchInst::Create(m_pRetBlock, retInst.getParent());
    m_retInsts.insert(&retInst);
}

// =====================================================================================================================
// Visits "call" instruction.
void SpirvLowerGlobal::visitCallInst(
    CallInst& callInst) // [in] "Call" instruction
{
    // Skip if "emit" and interpolaton calls are not expected to be handled
    if ((m_instVisitFlags.checkEmitCall == false) && (m_instVisitFlags.checkInterpCall == false))
    {
        return;
    }

    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

    auto mangledName = pCallee->getName();

    if (m_instVisitFlags.checkEmitCall)
    {
        if (mangledName.startswith("_Z10EmitVertex") ||
            mangledName.startswith("_Z16EmitStreamVertex"))
        {
            m_emitCalls.insert(&callInst);
        }
    }
    else
    {
        LLPC_ASSERT(m_instVisitFlags.checkInterpCall);

        if (mangledName.startswith("_Z21interpolateAtCentroid") ||
            mangledName.startswith("_Z19interpolateAtSample") ||
            mangledName.startswith("_Z19interpolateAtOffset") ||
            mangledName.startswith("_Z22InterpolateAtVertexAMD"))
        {
            // Translate interpolation functions to LLPC intrinsic calls
            auto pLoadSrc = callInst.getArgOperand(0);
            uint32_t interpLoc = InterpLocUnknown;
            Value* pAuxInterpValue = nullptr;

            if (mangledName.startswith("_Z21interpolateAtCentroid"))
            {
                interpLoc = InterpLocCentroid;
            }
            else if (mangledName.startswith("_Z19interpolateAtSample"))
            {
                interpLoc = InterpLocSample;
                pAuxInterpValue = callInst.getArgOperand(1); // Sample ID
            }
            else if (mangledName.startswith("_Z19interpolateAtOffset"))
            {
                interpLoc = InterpLocCenter;
                pAuxInterpValue = callInst.getArgOperand(1); // Offset from pixel center
            }
            else
            {
                LLPC_ASSERT(mangledName.startswith("_Z22InterpolateAtVertexAMD"));
                interpLoc = InterpLocCustom;
                pAuxInterpValue = callInst.getArgOperand(1); // Vertex no.
            }

            if (isa<GetElementPtrInst>(pLoadSrc))
            {
                // The interpolant is an element of the input
                InterpolateInputElement(interpLoc, pAuxInterpValue, callInst);
            }
            else
            {
                // The interpolant is an input
                LLPC_ASSERT(isa<GlobalVariable>(pLoadSrc));

                auto pInput = cast<GlobalVariable>(pLoadSrc);
                auto pInputTy = pInput->getType()->getContainedType(0);

                MDNode* pMetaNode = pInput->getMetadata(gSPIRVMD::InOut);
                LLPC_ASSERT(pMetaNode != nullptr);
                auto pInputMeta = mdconst::dyn_extract<Constant>(pMetaNode->getOperand(0));

                auto pLoadValue = AddCallInstForInOutImport(pInputTy,
                                                            SPIRAS_Input,
                                                            pInputMeta,
                                                            nullptr,
                                                            0,
                                                            nullptr,
                                                            nullptr,
                                                            interpLoc,
                                                            pAuxInterpValue,
                                                            &callInst);

                m_interpCalls.insert(&callInst);
                callInst.replaceAllUsesWith(pLoadValue);
            }
        }
    }
}

// =====================================================================================================================
// Visits "load" instruction.
void SpirvLowerGlobal::visitLoadInst(
    LoadInst& loadInst) // [in] "Load" instruction
{
    Value* pLoadSrc = loadInst.getOperand(0);
    const uint32_t addrSpace = pLoadSrc->getType()->getPointerAddressSpace();

    if ((addrSpace != SPIRAS_Input) && (addrSpace != SPIRAS_Output))
    {
        return;
    }

    // Skip if "load" instructions are not expected to be handled
    const bool isTcsInput  = ((m_shaderStage == ShaderStageTessControl) && (addrSpace == SPIRAS_Input));
    const bool isTcsOutput = ((m_shaderStage == ShaderStageTessControl) && (addrSpace == SPIRAS_Output));
    const bool isTesInput  = ((m_shaderStage == ShaderStageTessEval) && (addrSpace == SPIRAS_Input));

    if ((m_instVisitFlags.checkLoad == false) ||
        ((isTcsInput == false) && (isTcsOutput == false) && (isTesInput == false)))
    {
        return;
    }

    if (GetElementPtrInst* const pGetElemPtr = dyn_cast<GetElementPtrInst>(pLoadSrc))
    {
        std::vector<Value*> indexOperands;

        GlobalVariable* pInOut = nullptr;

        // Loop back through the get element pointer to find the global variable.
        for (GetElementPtrInst* pCurrGetElemPtr = pGetElemPtr;
             pCurrGetElemPtr != nullptr;
             pCurrGetElemPtr = dyn_cast<GetElementPtrInst>(pCurrGetElemPtr->getPointerOperand()))
        {
            LLPC_ASSERT(pCurrGetElemPtr != nullptr);

            // If we have previous index operands, we need to remove the first operand (a zero index into the pointer)
            // when concatenating two GEP indices together.
            if (indexOperands.empty() == false)
            {
                indexOperands.erase(indexOperands.begin());
            }

            SmallVector<Value*, 8> indices;

            for (Value* const pIndex : pCurrGetElemPtr->indices())
            {
                indices.push_back(ToInt32Value(m_pContext, pIndex, &loadInst));
            }

            indexOperands.insert(indexOperands.begin(), indices.begin(), indices.end());

            pInOut = dyn_cast<GlobalVariable>(pCurrGetElemPtr->getPointerOperand());
        }

        // The root of the GEP should always be the global variable.
        LLPC_ASSERT(pInOut != nullptr);

        uint32_t operandIdx = 0;

        auto pInOutTy = pInOut->getType()->getContainedType(0);

        MDNode* pMetaNode = pInOut->getMetadata(gSPIRVMD::InOut);
        LLPC_ASSERT(pMetaNode != nullptr);
        auto pInOutMeta = mdconst::dyn_extract<Constant>(pMetaNode->getOperand(0));

        Value* pVertexIdx = nullptr;

        // If the input/output is arrayed, the outermost index might be used for vertex indexing
        if (pInOutTy->isArrayTy())
        {
            bool isVertexIdx = false;

            LLPC_ASSERT(pInOutMeta->getNumOperands() == 4);
            ShaderInOutMetadata inOutMeta = {};

            inOutMeta.U64All[0] = cast<ConstantInt>(pInOutMeta->getOperand(2))->getZExtValue();
            inOutMeta.U64All[1] = cast<ConstantInt>(pInOutMeta->getOperand(3))->getZExtValue();

            if (inOutMeta.IsBuiltIn)
            {
                uint32_t builtInId = inOutMeta.Value;
                isVertexIdx = ((builtInId == BuiltInPerVertex)    || // GLSL style per-vertex data
                               (builtInId == BuiltInPosition)     || // HLSL style per-vertex data
                               (builtInId == BuiltInPointSize)    ||
                               (builtInId == BuiltInClipDistance) ||
                               (builtInId == BuiltInCullDistance));
            }
            else
            {
                isVertexIdx = (inOutMeta.PerPatch == false);
            }

            if (isVertexIdx)
            {
                pInOutTy = pInOutTy->getArrayElementType();
                pVertexIdx = indexOperands[1];
                ++operandIdx;

                pInOutMeta = cast<Constant>(pInOutMeta->getOperand(1));
            }
        }

        auto pLoadValue = LoadInOutMember(pInOutTy,
                                          addrSpace,
                                          indexOperands,
                                          operandIdx,
                                          0,
                                          pInOutMeta,
                                          nullptr,
                                          pVertexIdx,
                                          InterpLocUnknown,
                                          nullptr,
                                          &loadInst);

        m_loadInsts.insert(&loadInst);
        loadInst.replaceAllUsesWith(pLoadValue);
    }
    else
    {
        LLPC_ASSERT(isa<GlobalVariable>(pLoadSrc));

        auto pInOut = cast<GlobalVariable>(pLoadSrc);
        auto pInOutTy = pInOut->getType()->getContainedType(0);

        MDNode* pMetaNode = pInOut->getMetadata(gSPIRVMD::InOut);
        LLPC_ASSERT(pMetaNode != nullptr);
        auto pInOutMeta = mdconst::dyn_extract<Constant>(pMetaNode->getOperand(0));

        Value* pLoadValue = UndefValue::get(pInOutTy);
        bool hasVertexIdx = false;

        if (pInOutTy->isArrayTy())
        {
            // Arrayed input/output
            LLPC_ASSERT(pInOutMeta->getNumOperands() == 4);
            ShaderInOutMetadata inOutMeta = {};
            inOutMeta.U64All[0] = cast<ConstantInt>(pInOutMeta->getOperand(2))->getZExtValue();
            inOutMeta.U64All[1] = cast<ConstantInt>(pInOutMeta->getOperand(3))->getZExtValue();

            // If the input/output is arrayed, the outermost dimension might for vertex indexing
            if (inOutMeta.IsBuiltIn)
            {
                uint32_t builtInId = inOutMeta.Value;
                hasVertexIdx = ((builtInId == BuiltInPerVertex)    || // GLSL style per-vertex data
                                (builtInId == BuiltInPosition)     || // HLSL style per-vertex data
                                (builtInId == BuiltInPointSize)    ||
                                (builtInId == BuiltInClipDistance) ||
                                (builtInId == BuiltInCullDistance));
            }
            else
            {
                hasVertexIdx = (inOutMeta.PerPatch == false);
            }
        }

        if (hasVertexIdx)
        {
            LLPC_ASSERT(pInOutTy->isArrayTy());

            auto pElemTy = pInOutTy->getArrayElementType();
            auto pElemMeta = cast<Constant>(pInOutMeta->getOperand(1));

            const uint32_t elemCount = pInOutTy->getArrayNumElements();
            for (uint32_t i = 0; i < elemCount; ++i)
            {
                Value* pVertexIdx = ConstantInt::get(m_pContext->Int32Ty(), i);
                auto pElemValue = AddCallInstForInOutImport(pElemTy,
                                                            addrSpace,
                                                            pElemMeta,
                                                            nullptr,
                                                            0,
                                                            nullptr,
                                                            pVertexIdx,
                                                            InterpLocUnknown,
                                                            nullptr,
                                                            &loadInst);

                std::vector<uint32_t> idxs;
                idxs.push_back(i);
                pLoadValue = InsertValueInst::Create(pLoadValue, pElemValue, idxs, "", &loadInst);
            }
        }
        else
        {
            pLoadValue = AddCallInstForInOutImport(pInOutTy,
                                                   addrSpace,
                                                   pInOutMeta,
                                                   nullptr,
                                                   0,
                                                   nullptr,
                                                   nullptr,
                                                   InterpLocUnknown,
                                                   nullptr,
                                                   &loadInst);
        }

        m_loadInsts.insert(&loadInst);
        loadInst.replaceAllUsesWith(pLoadValue);
    }
}

// =====================================================================================================================
// Visits "store" instruction.
void SpirvLowerGlobal::visitStoreInst(
    StoreInst& storeInst) // [in] "Store" instruction
{
    Value* pStoreValue = storeInst.getOperand(0);
    Value* pStoreDest  = storeInst.getOperand(1);

    const uint32_t addrSpace = pStoreDest->getType()->getPointerAddressSpace();

    if ((addrSpace != SPIRAS_Input) && (addrSpace != SPIRAS_Output))
    {
        return;
    }

    // Skip if "store" instructions are not expected to be handled
    const bool isTcsOutput = ((m_shaderStage == ShaderStageTessControl) && (addrSpace == SPIRAS_Output));
    if ((m_instVisitFlags.checkStore == false) || (isTcsOutput == false))
    {
        return;
    }

    if (GetElementPtrInst* const pGetElemPtr = dyn_cast<GetElementPtrInst>(pStoreDest))
    {
        std::vector<Value*> indexOperands;

        GlobalVariable* pOutput = nullptr;

        // Loop back through the get element pointer to find the global variable.
        for (GetElementPtrInst* pCurrGetElemPtr = pGetElemPtr;
             pCurrGetElemPtr != nullptr;
             pCurrGetElemPtr = dyn_cast<GetElementPtrInst>(pCurrGetElemPtr->getPointerOperand()))
        {
            LLPC_ASSERT(pCurrGetElemPtr != nullptr);

            // If we have previous index operands, we need to remove the first operand (a zero index into the pointer)
            // when concatenating two GEP indices together.
            if (indexOperands.empty() == false)
            {
                indexOperands.erase(indexOperands.begin());
            }

            SmallVector<Value*, 8> indices;

            for (Value* const pIndex : pCurrGetElemPtr->indices())
            {
                indices.push_back(ToInt32Value(m_pContext, pIndex, &storeInst));
            }

            indexOperands.insert(indexOperands.begin(), indices.begin(), indices.end());

            pOutput = dyn_cast<GlobalVariable>(pCurrGetElemPtr->getPointerOperand());
        }

        uint32_t operandIdx = 0;

        auto pOutputTy = pOutput->getType()->getContainedType(0);

        MDNode* pMetaNode = pOutput->getMetadata(gSPIRVMD::InOut);
        LLPC_ASSERT(pMetaNode != nullptr);
        auto pOutputMeta = mdconst::dyn_extract<Constant>(pMetaNode->getOperand(0));

        Value* pVertexIdx = nullptr;

        // If the output is arrayed, the outermost index might be used for vertex indexing
        if (pOutputTy->isArrayTy())
        {
            bool isVertexIdx = false;

            LLPC_ASSERT(pOutputMeta->getNumOperands() == 4);
            ShaderInOutMetadata outputMeta = {};
            outputMeta.U64All[0] = cast<ConstantInt>(pOutputMeta->getOperand(2))->getZExtValue();
            outputMeta.U64All[1] = cast<ConstantInt>(pOutputMeta->getOperand(3))->getZExtValue();

            if (outputMeta.IsBuiltIn)
            {
                uint32_t builtInId = outputMeta.Value;
                isVertexIdx = ((builtInId == BuiltInPerVertex)    || // GLSL style per-vertex data
                               (builtInId == BuiltInPosition)     || // HLSL style per-vertex data
                               (builtInId == BuiltInPointSize)    ||
                               (builtInId == BuiltInClipDistance) ||
                               (builtInId == BuiltInCullDistance));
            }
            else
            {
                isVertexIdx = (outputMeta.PerPatch == false);
            }

            if (isVertexIdx)
            {
                pOutputTy = pOutputTy->getArrayElementType();
                pVertexIdx = indexOperands[1];
                ++operandIdx;

                pOutputMeta = cast<Constant>(pOutputMeta->getOperand(1));
            }
        }

        StoreOutputMember(pOutputTy,
                          pStoreValue,
                          indexOperands,
                          operandIdx,
                          0,
                          pOutputMeta,
                          nullptr,
                          pVertexIdx,
                          &storeInst);

        m_storeInsts.insert(&storeInst);
    }
    else
    {
        LLPC_ASSERT(isa<GlobalVariable>(pStoreDest));

        auto pOutput = cast<GlobalVariable>(pStoreDest);
        auto pOutputy = pOutput->getType()->getContainedType(0);

        MDNode* pMetaNode = pOutput->getMetadata(gSPIRVMD::InOut);
        LLPC_ASSERT(pMetaNode != nullptr);
        auto pOutputMeta = mdconst::dyn_extract<Constant>(pMetaNode->getOperand(0));

        bool hasVertexIdx = false;

        // If the input/output is arrayed, the outermost dimension might for vertex indexing
        if (pOutputy->isArrayTy())
        {
            LLPC_ASSERT(pOutputMeta->getNumOperands() == 4);
            ShaderInOutMetadata outputMeta = {};
            outputMeta.U64All[0] = cast<ConstantInt>(pOutputMeta->getOperand(2))->getZExtValue();
            outputMeta.U64All[1] = cast<ConstantInt>(pOutputMeta->getOperand(3))->getZExtValue();

            if (outputMeta.IsBuiltIn)
            {
                uint32_t builtInId = outputMeta.Value;
                hasVertexIdx = ((builtInId == BuiltInPerVertex)    || // GLSL style per-vertex data
                                (builtInId == BuiltInPosition)     || // HLSL style per-vertex data
                                (builtInId == BuiltInPointSize)    ||
                                (builtInId == BuiltInClipDistance) ||
                                (builtInId == BuiltInCullDistance));
            }
            else
            {
                hasVertexIdx = (outputMeta.PerPatch == false);
            }
        }

        if (hasVertexIdx)
        {
            LLPC_ASSERT(pOutputy->isArrayTy());
            auto pElemMeta = cast<Constant>(pOutputMeta->getOperand(1));

            const uint32_t elemCount = pOutputy->getArrayNumElements();
            for (uint32_t i = 0; i < elemCount; ++i)
            {
                std::vector<uint32_t> idxs;
                idxs.push_back(i);
                auto pElemValue = ExtractValueInst::Create(pStoreValue, idxs, "", &storeInst);

                Value* pVertexIdx = ConstantInt::get(m_pContext->Int32Ty(), i);
                AddCallInstForOutputExport(pElemValue,
                                           pElemMeta,
                                           nullptr,
                                           0,
                                           InvalidValue,
                                           nullptr,
                                           pVertexIdx,
                                           InvalidValue,
                                           &storeInst);
            }
        }
        else
        {
            AddCallInstForOutputExport(pStoreValue,
                                       pOutputMeta,
                                       nullptr,
                                       0,
                                       InvalidValue,
                                       nullptr,
                                       nullptr,
                                       InvalidValue,
                                       &storeInst);
        }

        m_storeInsts.insert(&storeInst);
    }

}

// =====================================================================================================================
// Maps the specified global variable to proxy variable.
void SpirvLowerGlobal::MapGlobalVariableToProxy(
    GlobalVariable* pGlobalVar) // [in] Global variable to be mapped
{
    const auto& dataLayout = m_pModule->getDataLayout();
    Type* pGlobalVarTy = pGlobalVar->getType()->getContainedType(0);
    Twine prefix = LlpcName::GlobalProxyPrefix;
    auto pInsertPos = m_pEntryPoint->begin()->getFirstInsertionPt();

    auto pProxy = new AllocaInst(pGlobalVarTy,
                                 dataLayout.getAllocaAddrSpace(),
                                 prefix + pGlobalVar->getName(),
                                 &*pInsertPos);

    if (pGlobalVar->hasInitializer())
    {
        auto pInitializer = pGlobalVar->getInitializer();
        new StoreInst(pInitializer, pProxy, &*pInsertPos);
    }

    m_globalVarProxyMap[pGlobalVar] = pProxy;
}

// =====================================================================================================================
// Maps the specified input to proxy variable.
void SpirvLowerGlobal::MapInputToProxy(
    GlobalVariable* pInput) // [in] Input to be mapped
{
    // NOTE: For tessellation shader, we do not map inputs to real proxy variables. Instead, we directly replace
    // "load" instructions with import calls in the lowering operation.
    if ((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval))
    {
        m_inputProxyMap[pInput] = nullptr;
        m_lowerInputInPlace = true;
        return;
    }

    const auto& dataLayout = m_pModule->getDataLayout();
    Type* pInputTy = pInput->getType()->getContainedType(0);
    Twine prefix = LlpcName::InputProxyPrefix;
    auto pInsertPos = m_pEntryPoint->begin()->getFirstInsertionPt();

    MDNode* pMetaNode  = pInput->getMetadata(gSPIRVMD::InOut);
    LLPC_ASSERT(pMetaNode != nullptr);

    auto pMeta = mdconst::dyn_extract<Constant>(pMetaNode->getOperand(0));
    auto pProxy = new AllocaInst(pInputTy,
                                 dataLayout.getAllocaAddrSpace(),
                                 prefix + pInput->getName(),
                                 &*pInsertPos);

    // Import input to proxy variable
    auto pInputValue = AddCallInstForInOutImport(pInputTy,
                                                 SPIRAS_Input,
                                                 pMeta,
                                                 nullptr,
                                                 0,
                                                 nullptr,
                                                 nullptr,
                                                 InterpLocUnknown,
                                                 nullptr,
                                                 &*pInsertPos);
    new StoreInst(pInputValue, pProxy, &*pInsertPos);

    m_inputProxyMap[pInput] = pProxy;
}

// =====================================================================================================================
// Maps the specified output to proxy variable.
void SpirvLowerGlobal::MapOutputToProxy(
    GlobalVariable* pOutput) // [in] Output to be mapped
{
    // NOTE: For tessellation control shader, we do not map outputs to real proxy variables. Instead, we directly
    // replace "store" instructions with export calls in the lowering operation.
    if (m_shaderStage == ShaderStageTessControl)
    {
        m_outputProxyMap.push_back(std::pair<Value*, Value*>(pOutput, nullptr));
        m_lowerOutputInPlace = true;
        return;
    }

    const auto& dataLayout = m_pModule->getDataLayout();
    Type* pOutputTy = pOutput->getType()->getContainedType(0);
    Twine prefix = LlpcName::OutputProxyPrefix;
    auto pInsertPos = m_pEntryPoint->begin()->getFirstInsertionPt();

    auto pProxy = new AllocaInst(pOutputTy,
                                 dataLayout.getAllocaAddrSpace(),
                                 prefix + pOutput->getName(),
                                 &*pInsertPos);

    if (pOutput->hasInitializer())
    {
        auto pInitializer = pOutput->getInitializer();
        new StoreInst(pInitializer, pProxy, &*pInsertPos);
    }

    m_outputProxyMap.push_back(std::pair<Value*, Value*>(pOutput, pProxy));
}

// =====================================================================================================================
// Removes those constant expressions that reference global variables.
void SpirvLowerGlobal::RemoveConstantExpr()
{
    // Collect contant expressions and translate them to regular instructions
    for (GlobalVariable& global : m_pModule->globals())
    {
        auto addSpace = global.getType()->getAddressSpace();

        // Remove constant expressions for global variables in these address spaces
        bool isGlobalVar = (addSpace == SPIRAS_Private) || (addSpace == SPIRAS_Input) || (addSpace == SPIRAS_Output);

        if (isGlobalVar == false)
        {
            continue;
        }

        SmallVector<Constant*, 8> constantUsers;

        for (User* const pUser : global.users())
        {
            if (Constant* const pConst = dyn_cast<Constant>(pUser))
            {
                constantUsers.push_back(pConst);
            }
        }

        for (Constant* const pConst : constantUsers)
        {
            ReplaceConstWithInsts(pConst);
        }
    }
}

// =====================================================================================================================
// Does lowering opertions for SPIR-V global variables, replaces global variables with proxy variables.
void SpirvLowerGlobal::LowerGlobalVar()
{
    if (m_globalVarProxyMap.empty())
    {
        // Skip lowering if there is no global variable
        return;
    }

    // Replace global variable with proxy variable
    for (auto globalVarMap : m_globalVarProxyMap)
    {
        auto pGlobalVar = cast<GlobalVariable>(globalVarMap.first);
        auto pProxy = globalVarMap.second;
        pGlobalVar->mutateType(pProxy->getType()); // To clear address space for pointer to make replacement valid
        pGlobalVar->replaceAllUsesWith(pProxy);
        pGlobalVar->dropAllReferences();
        pGlobalVar->eraseFromParent();
    }
}

// =====================================================================================================================
// Does lowering opertions for SPIR-V inputs, replaces inputs with proxy variables.
void SpirvLowerGlobal::LowerInput()
{
    if (m_inputProxyMap.empty())
    {
        // Skip lowering if there is no input
        return;
    }

    // NOTE: For tessellation shader, we invoke handling of "load"/"store" instructions and replace all those
    // instructions with import/export calls in-place.
    LLPC_ASSERT((m_shaderStage != ShaderStageTessControl) && (m_shaderStage != ShaderStageTessEval));

    // NOTE: For fragment shader, we have to handle interpolation functions first since input interpolants must be
    // lowered in-place.
    if (m_shaderStage == ShaderStageFragment)
    {
        // Invoke handling of interpolation calls
        m_instVisitFlags.u32All = 0;
        m_instVisitFlags.checkInterpCall = true;
        visit(m_pModule);

        // Remove interpolation calls, they must have been replaced with LLPC intrinsics
        std::unordered_set<GetElementPtrInst*> getElemInsts;
        for (auto pInterpCall : m_interpCalls)
        {
            GetElementPtrInst* pGetElemPtr = dyn_cast<GetElementPtrInst>(pInterpCall->getArgOperand(0));
            if (pGetElemPtr != nullptr)
            {
                getElemInsts.insert(pGetElemPtr);
            }

            LLPC_ASSERT(pInterpCall->use_empty());
            pInterpCall->dropAllReferences();
            pInterpCall->eraseFromParent();
        }

        for (auto pGetElemPtr : getElemInsts)
        {
            if (pGetElemPtr->use_empty())
            {
                pGetElemPtr->dropAllReferences();
                pGetElemPtr->eraseFromParent();
            }
        }
    }

    for (auto inputMap : m_inputProxyMap)
    {
        auto pInput = cast<GlobalVariable>(inputMap.first);

        for (auto pUser = pInput->user_begin(), pEnd = pInput->user_end(); pUser != pEnd; ++pUser)
        {
            // NOTE: "Getelementptr" and "bitcast" will propogate the address space of pointer value (input variable)
            // to the element pointer value (destination). We have to clear the address space of this element pointer
            // value. The original pointer value has been lowered and therefore the address space is invalid now.
            Instruction* pInst = dyn_cast<Instruction>(*pUser);
            if (pInst != nullptr)
            {
                Type* pInstTy = pInst->getType();
                if (isa<PointerType>(pInstTy) && (pInstTy->getPointerAddressSpace() == SPIRAS_Input))
                {
                    LLPC_ASSERT(isa<GetElementPtrInst>(pInst) || isa<BitCastInst>(pInst));
                    Type* pNewInstTy = PointerType::get(pInstTy->getContainedType(0), SPIRAS_Private);
                    pInst->mutateType(pNewInstTy);
                }
            }
        }

        auto pProxy = inputMap.second;
        pInput->mutateType(pProxy->getType()); // To clear address space for pointer to make replacement valid
        pInput->replaceAllUsesWith(pProxy);
        pInput->eraseFromParent();
    }
}

// =====================================================================================================================
// Does lowering opertions for SPIR-V outputs, replaces outputs with proxy variables.
void SpirvLowerGlobal::LowerOutput()
{
    m_pRetBlock = BasicBlock::Create(*m_pContext, "", m_pEntryPoint);

    // Invoke handling of "return" instructions or "emit" calls
    m_instVisitFlags.u32All = 0;
    if (m_shaderStage == ShaderStageGeometry)
    {
        m_instVisitFlags.checkEmitCall = true;
        m_instVisitFlags.checkReturn = true;
    }
    else
    {
        m_instVisitFlags.checkReturn = true;
    }
    visit(m_pModule);

    auto pRetInst = ReturnInst::Create(*m_pContext, m_pRetBlock);

    for (auto retInst : m_retInsts)
    {
        retInst->dropAllReferences();
        retInst->eraseFromParent();
    }

    if (m_outputProxyMap.empty())
    {
        // Skip lowering if there is no output
        return;
    }

    // NOTE: For tessellation control shader, we invoke handling of "load"/"store" instructions and replace all those
    // instructions with import/export calls in-place.
    LLPC_ASSERT(m_shaderStage != ShaderStageTessControl);

    // Export output from the proxy variable prior to "return" instruction or "emit" calls
    for (auto outputMap : m_outputProxyMap)
    {
        auto pOutput = cast<GlobalVariable>(outputMap.first);
        auto pProxy  = outputMap.second;

        MDNode* pMetaNode = pOutput->getMetadata(gSPIRVMD::InOut);
        LLPC_ASSERT(pMetaNode != nullptr);

        auto pMeta = mdconst::dyn_extract<Constant>(pMetaNode->getOperand(0));

        if ((m_shaderStage == ShaderStageVertex) || (m_shaderStage == ShaderStageTessEval) ||
            (m_shaderStage == ShaderStageFragment))
        {
            Value* pOutputValue = new LoadInst(pProxy, "", pRetInst);
            AddCallInstForOutputExport(pOutputValue, pMeta, nullptr, 0, 0, nullptr, nullptr, InvalidValue, pRetInst);
        }
        else if (m_shaderStage == ShaderStageGeometry)
        {
            for (auto pEmitCall : m_emitCalls)
            {
                uint32_t emitStreamId = 0;

                auto mangledName = pEmitCall->getCalledFunction()->getName();
                if (mangledName.startswith("_Z16EmitStreamVertex"))
                {
                    emitStreamId = cast<ConstantInt>(pEmitCall->getOperand(0))->getZExtValue();
                }
                else
                {
                    LLPC_ASSERT(mangledName.startswith("_Z10EmitVertex"));
                }

                Value* pOutputValue = new LoadInst(pProxy, "", pEmitCall);
                AddCallInstForOutputExport(pOutputValue,
                                           pMeta,
                                           nullptr,
                                           0,
                                           0,
                                           nullptr,
                                           nullptr,
                                           emitStreamId,
                                           pEmitCall);
            }
        }
    }

    // Replace the Emit(Stream)Vertex calls with builder code.
    for (auto pEmitCall : m_emitCalls)
    {
        uint32_t emitStreamId = (pEmitCall->getNumArgOperands() != 0) ?
                                cast<ConstantInt>(pEmitCall->getArgOperand(0))->getZExtValue() : 0;
        m_pBuilder->SetInsertPoint(pEmitCall);
        m_pBuilder->CreateEmitVertex(emitStreamId);
        pEmitCall->eraseFromParent();
    }

    for (auto outputMap : m_outputProxyMap)
    {
        auto pOutput = cast<GlobalVariable>(outputMap.first);

        for (auto pUser = pOutput->user_begin(), pEnd = pOutput->user_end(); pUser != pEnd; ++pUser)
        {
            // NOTE: "Getelementptr" and "bitCast" will propogate the address space of pointer value (output variable)
            // to the element pointer value (destination). We have to clear the address space of this element pointer
            // value. The original pointer value has been lowered and therefore the address space is invalid now.
            Instruction* pInst = dyn_cast<Instruction>(*pUser);
            if (pInst != nullptr)
            {
                Type* pInstTy = pInst->getType();
                if (isa<PointerType>(pInstTy) && (pInstTy->getPointerAddressSpace() == SPIRAS_Output))
                {
                    LLPC_ASSERT(isa<GetElementPtrInst>(pInst) || isa<BitCastInst>(pInst));
                    Type* pNewInstTy = PointerType::get(pInstTy->getContainedType(0), SPIRAS_Private);
                    pInst->mutateType(pNewInstTy);
                }
            }
        }

        auto pProxy = outputMap.second;
        pOutput->mutateType(pProxy->getType()); // To clear address space for pointer to make replacement valid
        pOutput->replaceAllUsesWith(pProxy);
        pOutput->eraseFromParent();
    }
}

// =====================================================================================================================
// Does inplace lowering opertions for SPIR-V inputs/outputs, replaces "load" instructions with import calls and
// "store" instructions with export calls.
void SpirvLowerGlobal::LowerInOutInPlace()
{
    LLPC_ASSERT((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval));

    // Invoke handling of "load" and "store" instruction
    m_instVisitFlags.u32All = 0;
    m_instVisitFlags.checkLoad = true;
    if (m_shaderStage == ShaderStageTessControl)
    {
        m_instVisitFlags.checkStore = true;
    }
    visit(m_pModule);

    llvm::DenseSet<GetElementPtrInst*> getElemInsts;

    // Remove unnecessary "load" instructions
    for (auto pLoadInst : m_loadInsts)
    {
        GetElementPtrInst* const pGetElemPtr = dyn_cast<GetElementPtrInst>(pLoadInst->getPointerOperand());
        if (pGetElemPtr != nullptr)
        {
            getElemInsts.insert(pGetElemPtr);
        }

        LLPC_ASSERT(pLoadInst->use_empty());
        pLoadInst->dropAllReferences();
        pLoadInst->eraseFromParent();
    }

    m_loadInsts.clear();

    // Remove unnecessary "store" instructions
    for (auto pStoreInst : m_storeInsts)
    {
        GetElementPtrInst* const pGetElemPtr = dyn_cast<GetElementPtrInst>(pStoreInst->getPointerOperand());
        if (pGetElemPtr != nullptr)
        {
            getElemInsts.insert(pGetElemPtr);
        }

        LLPC_ASSERT(pStoreInst->use_empty());
        pStoreInst->dropAllReferences();
        pStoreInst->eraseFromParent();
    }

    m_storeInsts.clear();

    // Remove unnecessary "getelementptr" instructions
    while (getElemInsts.empty() == false)
    {
        GetElementPtrInst* const pGetElemPtr = *getElemInsts.begin();
        getElemInsts.erase(pGetElemPtr);

        // If the GEP still has any uses, skip processing it.
        if (pGetElemPtr->use_empty() == false)
        {
            continue;
        }

        // If the GEP is GEPing into another GEP, record that GEP as something we need to visit too.
        if (GetElementPtrInst* const pOtherGetElemInst = dyn_cast<GetElementPtrInst>(pGetElemPtr->getPointerOperand()))
        {
            getElemInsts.insert(pOtherGetElemInst);
        }

        pGetElemPtr->dropAllReferences();
        pGetElemPtr->eraseFromParent();
    }

    // Remove inputs if they are lowered in-place
    if (m_lowerInputInPlace)
    {
        for (auto inputMap : m_inputProxyMap)
        {
            auto pInput = cast<GlobalVariable>(inputMap.first);
            LLPC_ASSERT(pInput->use_empty());
            pInput->eraseFromParent();
        }
    }

    // Remove outputs if they are lowered in-place
    if (m_lowerOutputInPlace)
    {
        for (auto outputMap : m_outputProxyMap)
        {
            auto pOutput = cast<GlobalVariable>(outputMap.first);
            LLPC_ASSERT(pOutput->use_empty());
            pOutput->eraseFromParent();
        }
    }
}

// =====================================================================================================================
// Inserts LLVM call instruction to import input/output.
Value* SpirvLowerGlobal::AddCallInstForInOutImport(
    Type*        pInOutTy,          // [in] Type of value imported from input/output
    uint32_t     addrSpace,         // Address space
    Constant*    pInOutMeta,        // [in] Metadata of this input/output
    Value*       pLocOffset,        // [in] Relative location offset, passed from aggregate type
    uint32_t     maxLocOffset,      // Max+1 location offset if variable index has been encountered.
                                    //    For an array built-in with a variable index, this is the array size.
    Value*       pElemIdx,          // [in] Element index used for element indexing, valid for tessellation shader
                                    // (usually, it is vector component index, for built-in input/output, it could be
                                    // element index of scalar array)
    Value*       pVertexIdx,        // [in] Input array outermost index used for vertex indexing, valid for
                                    // tessellation shader and geometry shader
    uint32_t     interpLoc,         // Interpolation location, valid for fragment shader (use "InterpLocUnknown" as
                                    // don't-care value)
    Value*       pAuxInterpValue,   // [in] Auxiliary value of interpolation (valid for fragment shader)
                                    //   - Value is sample ID for "InterpLocSample"
                                    //   - Value is offset from the center of the pixel for "InterpLocCenter"
                                    //   - Value is vertex no. (0 ~ 2) for "InterpLocCustom"
    Instruction* pInsertPos)        // [in] Where to insert this call
{
    LLPC_ASSERT((addrSpace == SPIRAS_Input) ||
                ((addrSpace == SPIRAS_Output) && (m_shaderStage == ShaderStageTessControl)));

    Value* pInOutValue = UndefValue::get(pInOutTy);

    ShaderInOutMetadata inOutMeta = {};

    if (pInOutTy->isArrayTy())
    {
        // Array type
        LLPC_ASSERT(pElemIdx == nullptr);

        LLPC_ASSERT(pInOutMeta->getNumOperands() == 4);
        uint32_t stride = cast<ConstantInt>(pInOutMeta->getOperand(0))->getZExtValue();
        inOutMeta.U64All[0] = cast<ConstantInt>(pInOutMeta->getOperand(2))->getZExtValue();
        inOutMeta.U64All[1] = cast<ConstantInt>(pInOutMeta->getOperand(3))->getZExtValue();

        if (inOutMeta.IsBuiltIn)
        {
            LLPC_ASSERT(pLocOffset == nullptr);

            uint32_t builtInId = inOutMeta.Value;

            if ((pVertexIdx == nullptr) && (m_shaderStage == ShaderStageGeometry) &&
                ((builtInId == BuiltInPerVertex)    || // GLSL style per-vertex data
                 (builtInId == BuiltInPosition)     || // HLSL style per-vertex data
                 (builtInId == BuiltInPointSize)    ||
                 (builtInId == BuiltInClipDistance) ||
                 (builtInId == BuiltInCullDistance)))
            {
                // NOTE: We are handling vertex indexing of built-in inputs of geometry shader. For tessellation
                // shader, vertex indexing is handled by "load"/"store" instruction lowering.
                LLPC_ASSERT(pVertexIdx == nullptr); // For per-vertex data, make a serial of per-vertex import calls.

                LLPC_ASSERT((m_shaderStage == ShaderStageGeometry) ||
                            (m_shaderStage == ShaderStageTessControl) ||
                            (m_shaderStage == ShaderStageTessEval));

                auto pElemMeta = cast<Constant>(pInOutMeta->getOperand(1));
                auto pElemTy   = pInOutTy->getArrayElementType();

                const uint64_t elemCount = pInOutTy->getArrayNumElements();
                for (uint32_t elemIdx = 0; elemIdx < elemCount; ++elemIdx)
                {
                    // Handle array elements recursively
                    pVertexIdx = ConstantInt::get(m_pContext->Int32Ty(), elemIdx);
                    auto pElem = AddCallInstForInOutImport(pElemTy,
                                                           addrSpace,
                                                           pElemMeta,
                                                           nullptr,
                                                           maxLocOffset,
                                                           nullptr,
                                                           pVertexIdx,
                                                           interpLoc,
                                                           pAuxInterpValue,
                                                           pInsertPos);

                    std::vector<uint32_t> idxs;
                    idxs.push_back(elemIdx);
                    pInOutValue = InsertValueInst::Create(pInOutValue, pElem, idxs, "", pInsertPos);
                }
            }
            else
            {
                // Array built-in without vertex indexing (ClipDistance/CullDistance).
                Builder::InOutInfo inOutInfo;
                inOutInfo.SetArraySize(pInOutTy->getArrayNumElements());
                m_pBuilder->SetInsertPoint(pInsertPos);
                if (addrSpace == SPIRAS_Input)
                {
                    pInOutValue =  m_pBuilder->CreateReadBuiltInInput(
                                      static_cast<Builder::BuiltInKind>(inOutMeta.Value),
                                      inOutInfo,
                                      pVertexIdx,
                                      nullptr);
                }
                else
                {
                    pInOutValue =  m_pBuilder->CreateReadBuiltInOutput(
                                      static_cast<Builder::BuiltInKind>(inOutMeta.Value),
                                      inOutInfo,
                                      pVertexIdx,
                                      nullptr);
                }
            }
        }
        else
        {
            auto pElemMeta = cast<Constant>(pInOutMeta->getOperand(1));
            auto pElemTy   = pInOutTy->getArrayElementType();

            const uint64_t elemCount = pInOutTy->getArrayNumElements();

            if ((pVertexIdx == nullptr) && (m_shaderStage == ShaderStageGeometry))
            {
                // NOTE: We are handling vertex indexing of generic inputs of geometry shader. For tessellation shader,
                // vertex indexing is handled by "load"/"store" instruction lowering.
                for (uint32_t elemIdx = 0; elemIdx < elemCount; ++elemIdx)
                {
                    pVertexIdx = ConstantInt::get(m_pContext->Int32Ty(), elemIdx);
                    auto pElem = AddCallInstForInOutImport(pElemTy,
                                                           addrSpace,
                                                           pElemMeta,
                                                           pLocOffset,
                                                           maxLocOffset,
                                                           nullptr,
                                                           pVertexIdx,
                                                           InterpLocUnknown,
                                                           nullptr,
                                                           pInsertPos);

                    std::vector<uint32_t> idxs;
                    idxs.push_back(elemIdx);
                    pInOutValue = InsertValueInst::Create(pInOutValue, pElem, idxs, "", pInsertPos);
                }
            }
            else
            {
                // NOTE: If the relative location offset is not specified, initialize it to 0.
                if (pLocOffset == nullptr)
                {
                    pLocOffset = ConstantInt::get(m_pContext->Int32Ty(), 0);
                }

                for (uint32_t elemIdx = 0; elemIdx < elemCount; ++elemIdx)
                {
                    // Handle array elements recursively

                    // elemLocOffset = locOffset + stride * elemIdx
                    Value* pElemLocOffset = BinaryOperator::CreateMul(ConstantInt::get(m_pContext->Int32Ty(), stride),
                                                                      ConstantInt::get(m_pContext->Int32Ty(), elemIdx),
                                                                      "",
                                                                      pInsertPos);
                    pElemLocOffset = BinaryOperator::CreateAdd(pLocOffset, pElemLocOffset, "", pInsertPos);

                    auto pElem = AddCallInstForInOutImport(pElemTy,
                                                           addrSpace,
                                                           pElemMeta,
                                                           pElemLocOffset,
                                                           maxLocOffset,
                                                           pElemIdx,
                                                           pVertexIdx,
                                                           InterpLocUnknown,
                                                           nullptr,
                                                           pInsertPos);

                    std::vector<uint32_t> idxs;
                    idxs.push_back(elemIdx);
                    pInOutValue = InsertValueInst::Create(pInOutValue, pElem, idxs, "", pInsertPos);
                }
            }
        }
    }
    else if (pInOutTy->isStructTy())
    {
        // Structure type
        LLPC_ASSERT(pElemIdx == nullptr);

        const uint64_t memberCount = pInOutTy->getStructNumElements();
        for (uint32_t memberIdx = 0; memberIdx < memberCount; ++memberIdx)
        {
            // Handle structure member recursively
            auto pMemberTy = pInOutTy->getStructElementType(memberIdx);
            auto pMemberMeta = cast<Constant>(pInOutMeta->getOperand(memberIdx));

            auto pMember = AddCallInstForInOutImport(pMemberTy,
                                                     addrSpace,
                                                     pMemberMeta,
                                                     pLocOffset,
                                                     maxLocOffset,
                                                     nullptr,
                                                     pVertexIdx,
                                                     InterpLocUnknown,
                                                     nullptr,
                                                     pInsertPos);

            std::vector<uint32_t> idxs;
            idxs.push_back(memberIdx);
            pInOutValue = InsertValueInst::Create(pInOutValue, pMember, idxs, "", pInsertPos);
        }
    }
    else
    {
        std::vector<Value*> args;
        Constant* pInOutMetaConst = cast<Constant>(pInOutMeta);
        inOutMeta.U64All[0] = cast<ConstantInt>(pInOutMetaConst->getOperand(0))->getZExtValue();
        inOutMeta.U64All[1] = cast<ConstantInt>(pInOutMetaConst->getOperand(1))->getZExtValue();

        LLPC_ASSERT(inOutMeta.IsLoc || inOutMeta.IsBuiltIn);

        m_pBuilder->SetInsertPoint(pInsertPos);
        if (inOutMeta.IsBuiltIn)
        {
            auto builtIn = static_cast<Builder::BuiltInKind>(inOutMeta.Value);
            pElemIdx = (pElemIdx == m_pBuilder->getInt32(InvalidValue)) ? nullptr : pElemIdx;
            pVertexIdx = (pVertexIdx == m_pBuilder->getInt32(InvalidValue)) ? nullptr : pVertexIdx;

            Builder::InOutInfo inOutInfo;
            inOutInfo.SetArraySize(maxLocOffset);
            if (addrSpace == SPIRAS_Input)
            {
                pInOutValue = m_pBuilder->CreateReadBuiltInInput(builtIn, inOutInfo, pVertexIdx, pElemIdx);
            }
            else
            {
                pInOutValue = m_pBuilder->CreateReadBuiltInOutput(builtIn, inOutInfo, pVertexIdx, pElemIdx);
            }

            if (((builtIn == Builder::BuiltInSubgroupEqMask)     ||
                 (builtIn == Builder::BuiltInSubgroupGeMask)     ||
                 (builtIn == Builder::BuiltInSubgroupGtMask)     ||
                 (builtIn == Builder::BuiltInSubgroupLeMask)     ||
                 (builtIn == Builder::BuiltInSubgroupLtMask)) &&
                pInOutTy->isIntegerTy(64))
            {
                // NOTE: Glslang has a bug. For gl_SubGroupXXXMaskARB, they are implemented as "uint64_t" while
                // for gl_subgroupXXXMask they are "uvec4". And the SPIR-V enumerants "BuiltInSubgroupXXXMaskKHR"
                // and "BuiltInSubgroupXXXMask" share the same numeric values.
                pInOutValue = m_pBuilder->CreateBitCast(pInOutValue, VectorType::get(pInOutTy, 2));
                pInOutValue = m_pBuilder->CreateExtractElement(pInOutValue, uint64_t(0));
            }
            if (pInOutValue->getType()->isIntegerTy(1))
            {
                // Convert i1 to i32.
                pInOutValue = m_pBuilder->CreateZExt(pInOutValue, m_pBuilder->getInt32Ty());
            }
        }
        else
        {
            uint32_t elemIdx = inOutMeta.Component;
            LLPC_ASSERT(inOutMeta.Component <= 3);
            if (pInOutTy->getScalarSizeInBits() == 64)
            {
                LLPC_ASSERT(inOutMeta.Component % 2 == 0); // Must be even for 64-bit type
                elemIdx = inOutMeta.Component / 2;
            }
            pElemIdx = (pElemIdx == nullptr) ? m_pBuilder->getInt32(elemIdx) :
                                               m_pBuilder->CreateAdd(pElemIdx, m_pBuilder->getInt32(elemIdx));


            Builder::InOutInfo inOutInfo;
            if (pLocOffset == nullptr)
            {
                pLocOffset = m_pBuilder->getInt32(0);
            }

            if (addrSpace == SPIRAS_Input)
            {
                if (m_shaderStage == ShaderStageFragment)
                {
                    if (interpLoc != InterpLocUnknown)
                    {
                        // Use auxiliary value of interpolation (calcuated I/J or vertex no.) for
                        // interpolant inputs of fragment shader.
                        pVertexIdx = pAuxInterpValue;
                        inOutInfo.SetHasInterpAux();
                    }
                    else
                    {
                        interpLoc = inOutMeta.InterpLoc;
                    }
                    inOutInfo.SetInterpLoc(interpLoc);
                    inOutInfo.SetInterpMode(inOutMeta.InterpMode);
                }
                pInOutValue = m_pBuilder->CreateReadGenericInput(pInOutTy,
                                                                 inOutMeta.Value,
                                                                 pLocOffset,
                                                                 pElemIdx,
                                                                 maxLocOffset,
                                                                 inOutInfo,
                                                                 pVertexIdx);
            }
            else
            {
                pInOutValue = m_pBuilder->CreateReadGenericOutput(pInOutTy,
                                                                  inOutMeta.Value,
                                                                  pLocOffset,
                                                                  pElemIdx,
                                                                  maxLocOffset,
                                                                  inOutInfo,
                                                                  pVertexIdx);
            }
        }
    }

    return pInOutValue;
}

// =====================================================================================================================
// Inserts LLVM call instruction to export output.
void SpirvLowerGlobal::AddCallInstForOutputExport(
    Value*       pOutputValue, // [in] Value exported to output
    Constant*    pOutputMeta,  // [in] Metadata of this output
    Value*       pLocOffset,   // [in] Relative location offset, passed from aggregate type
    uint32_t     maxLocOffset, // Max+1 location offset if variable index has been encountered.
                               //    For an array built-in with a variable index, this is the array size.
    uint32_t     xfbLocOffset, // Transform feedback location offset (for array type)
    Value*       pElemIdx,     // [in] Element index used for element indexing, valid for tessellation control shader
                               // (usually, it is vector component index, for built-in input/output, it could be
                               // element index of scalar array)
    Value*       pVertexIdx,   // [in] Output array outermost index used for vertex indexing, valid for tessellation
                               // control shader
    uint32_t     emitStreamId, // ID of emitted vertex stream, valid for geometry shader (0xFFFFFFFF for other stages)
    Instruction* pInsertPos)   // [in] Where to insert this call
{
    Type* pOutputTy = pOutputValue->getType();

    ShaderInOutMetadata outputMeta = {};

    if (pOutputTy->isArrayTy())
    {
        // Array type
        LLPC_ASSERT(pElemIdx == nullptr);

        LLPC_ASSERT(pOutputMeta->getNumOperands() == 4);
        uint32_t stride = cast<ConstantInt>(pOutputMeta->getOperand(0))->getZExtValue();

        outputMeta.U64All[0] = cast<ConstantInt>(pOutputMeta->getOperand(2))->getZExtValue();
        outputMeta.U64All[1] = cast<ConstantInt>(pOutputMeta->getOperand(3))->getZExtValue();

        if ((m_shaderStage == ShaderStageGeometry) && (emitStreamId != outputMeta.StreamId))
        {
            // NOTE: For geometry shader, if the output is not bound to this vertex stream, we skip processing.
            return;
        }

        if (outputMeta.IsBuiltIn)
        {
            // NOTE: For geometry shader, we add stream ID for outputs.
            LLPC_ASSERT((m_shaderStage != ShaderStageGeometry) || (emitStreamId == outputMeta.StreamId));

            auto builtInId = static_cast<Builder::BuiltInKind>(outputMeta.Value);
            Builder::InOutInfo outputInfo;
            if (emitStreamId != InvalidValue)
            {
                outputInfo.SetStreamId(emitStreamId);
            }
            outputInfo.SetArraySize(pOutputTy->getArrayNumElements());
            m_pBuilder->SetInsertPoint(pInsertPos);
            m_pBuilder->CreateWriteBuiltInOutput(pOutputValue, builtInId, outputInfo, pVertexIdx, nullptr);
        }
        else
        {
            // NOTE: If the relative location offset is not specified, initialize it to 0.
            if (pLocOffset == nullptr)
            {
                pLocOffset = ConstantInt::get(m_pContext->Int32Ty(), 0);
            }

            auto pElemMeta = cast<Constant>(pOutputMeta->getOperand(1));

            const uint64_t elemCount = pOutputTy->getArrayNumElements();
            for (uint32_t elemIdx = 0; elemIdx < elemCount; ++elemIdx)
            {
                // Handle array elements recursively
                std::vector<uint32_t> idxs;
                idxs.push_back(elemIdx);
                Value* pElem = ExtractValueInst::Create(pOutputValue, idxs, "", pInsertPos);

                Value* pElemLocOffset = nullptr;
                ConstantInt* pLocOffsetConst = dyn_cast<ConstantInt>(pLocOffset);

                if (pLocOffsetConst != nullptr)
                {
                    uint32_t xfbLocOffset = pLocOffsetConst->getZExtValue();
                    pElemLocOffset = ConstantInt::get(m_pContext->Int32Ty(), xfbLocOffset + stride * elemIdx);
                }
                else
                {
                    // elemLocOffset = locOffset + stride * elemIdx
                    pElemLocOffset = BinaryOperator::CreateMul(ConstantInt::get(m_pContext->Int32Ty(), stride),
                                                               ConstantInt::get(m_pContext->Int32Ty(), elemIdx),
                                                               "",
                                                               pInsertPos);
                    pElemLocOffset = BinaryOperator::CreateAdd(pLocOffset, pElemLocOffset, "", pInsertPos);
                }

                AddCallInstForOutputExport(pElem,
                                           pElemMeta,
                                           pElemLocOffset,
                                           maxLocOffset,
                                           xfbLocOffset + outputMeta.XfbLocStride * elemIdx,
                                           nullptr,
                                           pVertexIdx,
                                           emitStreamId,
                                           pInsertPos);
            }
        }
    }
    else if (pOutputTy->isStructTy())
    {
        // Structure type
        LLPC_ASSERT(pElemIdx == nullptr);

        const uint64_t memberCount = pOutputTy->getStructNumElements();
        for (uint32_t memberIdx = 0; memberIdx < memberCount; ++memberIdx)
        {
            // Handle structure member recursively
            auto pMemberMeta = cast<Constant>(pOutputMeta->getOperand(memberIdx));

            std::vector<uint32_t> idxs;
            idxs.push_back(memberIdx);
            Value* pMember = ExtractValueInst::Create(pOutputValue, idxs, "", pInsertPos);

            AddCallInstForOutputExport(pMember,
                                       pMemberMeta,
                                       pLocOffset,
                                       maxLocOffset,
                                       xfbLocOffset,
                                       nullptr,
                                       pVertexIdx,
                                       emitStreamId,
                                       pInsertPos);
        }
    }
    else
    {
        // Normal scalar or vector type
        std::vector<Value*> args;
        m_pBuilder->SetInsertPoint(pInsertPos);
        Constant* pInOutMetaConst = cast<Constant>(pOutputMeta);
        outputMeta.U64All[0] = cast<ConstantInt>(pInOutMetaConst->getOperand(0))->getZExtValue();
        outputMeta.U64All[1] = cast<ConstantInt>(pInOutMetaConst->getOperand(1))->getZExtValue();

        if ((m_shaderStage == ShaderStageGeometry) && (emitStreamId != outputMeta.StreamId))
        {
            // NOTE: For geometry shader, if the output is not bound to this vertex stream, we skip processing.
            return;
        }

        LLPC_ASSERT(outputMeta.IsLoc || outputMeta.IsBuiltIn);

        Builder::InOutInfo outputInfo;
        if (emitStreamId != InvalidValue)
        {
            outputInfo.SetStreamId(emitStreamId);
        }
        outputInfo.SetIsSigned(outputMeta.Signedness);

        if (outputMeta.IsBuiltIn)
        {
            auto builtInId = static_cast<Builder::BuiltInKind>(outputMeta.Value);
            outputInfo.SetArraySize(maxLocOffset);
            if (outputMeta.IsXfb)
            {
                // NOTE: For transform feedback outputs, additional stream-out export call will be generated.
                LLPC_ASSERT(xfbLocOffset != InvalidValue);
                Value* pXfbOffset = m_pBuilder->getInt32(outputMeta.XfbOffset + xfbLocOffset + outputMeta.XfbLoc);
                m_pBuilder->CreateWriteXfbOutput(pOutputValue,
                                                 /*isBuiltIn=*/true,
                                                 builtInId,
                                                 outputMeta.XfbBuffer,
                                                 outputMeta.XfbStride,
                                                 pXfbOffset, outputInfo);
            }

            m_pBuilder->CreateWriteBuiltInOutput(pOutputValue, builtInId, outputInfo, pVertexIdx, pElemIdx);
            return;
        }

        uint32_t location = outputMeta.Value + outputMeta.Index;
        LLPC_ASSERT(((outputMeta.Index == 1) && (outputMeta.Value == 0)) || (outputMeta.Index == 0));
        LLPC_ASSERT(pOutputTy->isSingleValueType());

        uint32_t elemIdx = outputMeta.Component;
        LLPC_ASSERT(outputMeta.Component <= 3);
        if (pOutputTy->getScalarSizeInBits() == 64)
        {
            LLPC_ASSERT(outputMeta.Component % 2 == 0); // Must be even for 64-bit type
            elemIdx = outputMeta.Component / 2;
        }
        pElemIdx = (pElemIdx == nullptr) ? m_pBuilder->getInt32(elemIdx) :
                                           m_pBuilder->CreateAdd(pElemIdx, m_pBuilder->getInt32(elemIdx));
        pLocOffset = (pLocOffset == nullptr) ? m_pBuilder->getInt32(0) : pLocOffset;

        if (outputMeta.IsXfb)
        {
            // NOTE: For transform feedback outputs, additional stream-out export call will be generated.
            LLPC_ASSERT(xfbLocOffset != InvalidValue);
            Value* pXfbOffset = m_pBuilder->getInt32(outputMeta.XfbOffset + xfbLocOffset + outputMeta.XfbLoc);
            m_pBuilder->CreateWriteXfbOutput(pOutputValue,
                                             /*isBuiltIn=*/false,
                                             location + cast<ConstantInt>(pLocOffset)->getZExtValue(),
                                             outputMeta.XfbBuffer,
                                             outputMeta.XfbStride,
                                             pXfbOffset, outputInfo);
        }

        m_pBuilder->CreateWriteGenericOutput(pOutputValue,
                                             location,
                                             pLocOffset,
                                             pElemIdx,
                                             maxLocOffset,
                                             outputInfo,
                                             pVertexIdx);
    }
}

// =====================================================================================================================
// Inserts instructions to load value from input/ouput member.
Value* SpirvLowerGlobal::LoadInOutMember(
    Type*                      pInOutTy,        // [in] Type of this input/output member
    uint32_t                   addrSpace,       // Address space
    const std::vector<Value*>& indexOperands,   // [in] Index operands
    uint32_t                   operandIdx,      // Index of the index operand in processing
    uint32_t                   maxLocOffset,    // Max+1 location offset if variable index has been encountered
    Constant*                  pInOutMeta,      // [in] Metadata of this input/output member
    Value*                     pLocOffset,      // [in] Relative location offset of this input/output member
    Value*                     pVertexIdx,      // [in] Input array outermost index used for vertex indexing
    uint32_t                   interpLoc,       // Interpolation location, valid for fragment shader
                                                // (use "InterpLocUnknown" as don't-care value)
    Value*                     pAuxInterpValue, // [in] Auxiliary value of interpolation (valid for fragment shader):
                                                //   - Sample ID for "InterpLocSample"
                                                //   - Offset from the center of the pixel for "InterpLocCenter"
                                                //   - Vertex no. (0 ~ 2) for "InterpLocCustom"
    Instruction*               pInsertPos)      // [in] Where to insert calculation instructions
{
    LLPC_ASSERT((m_shaderStage == ShaderStageTessControl) ||
                (m_shaderStage == ShaderStageTessEval) ||
                (m_shaderStage == ShaderStageFragment));

    if (operandIdx < indexOperands.size() - 1)
    {
        if (pInOutTy->isArrayTy())
        {
            // Array type
            LLPC_ASSERT(pInOutMeta->getNumOperands() == 4);
            ShaderInOutMetadata inOutMeta = {};

            inOutMeta.U64All[0] = cast<ConstantInt>(pInOutMeta->getOperand(2))->getZExtValue();
            inOutMeta.U64All[1] = cast<ConstantInt>(pInOutMeta->getOperand(3))->getZExtValue();

            auto pElemMeta = cast<Constant>(pInOutMeta->getOperand(1));
            auto pElemTy   = pInOutTy->getArrayElementType();

            if (inOutMeta.IsBuiltIn)
            {
                LLPC_ASSERT(operandIdx + 1 == indexOperands.size() - 1);
                auto pElemIdx = indexOperands[operandIdx + 1];
                return AddCallInstForInOutImport(pElemTy,
                                                 addrSpace,
                                                 pElemMeta,
                                                 pLocOffset,
                                                 pInOutTy->getArrayNumElements(),
                                                 pElemIdx,
                                                 pVertexIdx,
                                                 interpLoc,
                                                 pAuxInterpValue,
                                                 pInsertPos);
            }
            else
            {
                // NOTE: If the relative location offset is not specified, initialize it to 0.
                if (pLocOffset == nullptr)
                {
                    pLocOffset = ConstantInt::get(m_pContext->Int32Ty(), 0);
                }

                // elemLocOffset = locOffset + stride * elemIdx
                uint32_t stride = cast<ConstantInt>(pInOutMeta->getOperand(0))->getZExtValue();
                auto pElemIdx = indexOperands[operandIdx + 1];
                Value* pElemLocOffset = BinaryOperator::CreateMul(ConstantInt::get(m_pContext->Int32Ty(), stride),
                                                                  pElemIdx,
                                                                  "",
                                                                  pInsertPos);
                pElemLocOffset = BinaryOperator::CreateAdd(pLocOffset, pElemLocOffset, "", pInsertPos);

                // Mark the end+1 possible location offset if the index is variable. The Builder call needs it
                // so it knows how many locations to mark as used by this access.
                if ((maxLocOffset == 0) && (isa<ConstantInt>(pElemIdx) == false))
                {
                    maxLocOffset = cast<ConstantInt>(pLocOffset)->getZExtValue() +
                                   stride * pInOutTy->getArrayNumElements();
                }

                return LoadInOutMember(pElemTy,
                                       addrSpace,
                                       indexOperands,
                                       operandIdx + 1,
                                       maxLocOffset,
                                       pElemMeta,
                                       pElemLocOffset,
                                       pVertexIdx,
                                       interpLoc,
                                       pAuxInterpValue,
                                       pInsertPos);
            }
        }
        else if (pInOutTy->isStructTy())
        {
            // Structure type
            uint32_t memberIdx = cast<ConstantInt>(indexOperands[operandIdx + 1])->getZExtValue();

            auto pMemberTy = pInOutTy->getStructElementType(memberIdx);
            auto pMemberMeta = cast<Constant>(pInOutMeta->getOperand(memberIdx));

            return LoadInOutMember(pMemberTy,
                                   addrSpace,
                                   indexOperands,
                                   operandIdx + 1,
                                   maxLocOffset,
                                   pMemberMeta,
                                   pLocOffset,
                                   pVertexIdx,
                                   interpLoc,
                                   pAuxInterpValue,
                                   pInsertPos);
        }
        else if (pInOutTy->isVectorTy())
        {
            // Vector type
            auto pCompTy = pInOutTy->getVectorElementType();

            LLPC_ASSERT(operandIdx + 1 == indexOperands.size() - 1);
            auto pCompIdx = indexOperands[operandIdx + 1];

            return AddCallInstForInOutImport(pCompTy,
                                             addrSpace,
                                             pInOutMeta,
                                             pLocOffset,
                                             maxLocOffset,
                                             pCompIdx,
                                             pVertexIdx,
                                             interpLoc,
                                             pAuxInterpValue,
                                             pInsertPos);
        }
    }
    else
    {
        // Last index operand
        LLPC_ASSERT(operandIdx == indexOperands.size() - 1);
        return AddCallInstForInOutImport(pInOutTy,
                                         addrSpace,
                                         pInOutMeta,
                                         pLocOffset,
                                         maxLocOffset,
                                         nullptr,
                                         pVertexIdx,
                                         interpLoc,
                                         pAuxInterpValue,
                                         pInsertPos);
    }

    LLPC_NEVER_CALLED();
    return nullptr;
}

// =====================================================================================================================
// Inserts instructions to store value to ouput member.
void SpirvLowerGlobal::StoreOutputMember(
    Type*                      pOutputTy,       // [in] Type of this output member
    Value*                     pStoreValue,     // [in] Value stored to output member
    const std::vector<Value*>& indexOperands,   // [in] Index operands
    uint32_t                   operandIdx,      // Index of the index operand in processing
    uint32_t                   maxLocOffset,    // Max+1 location offset if variable index has been encountered
    Constant*                  pOutputMeta,     // [in] Metadata of this output member
    Value*                     pLocOffset,      // [in] Relative location offset of this output member
    Value*                     pVertexIdx,      // [in] Input array outermost index used for vertex indexing
    Instruction*               pInsertPos)      // [in] Where to insert store instructions
{
    LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);

    if (operandIdx < indexOperands.size() - 1)
    {
        if (pOutputTy->isArrayTy())
        {
            LLPC_ASSERT(pOutputMeta->getNumOperands() == 4);
            ShaderInOutMetadata outputMeta = {};

            outputMeta.U64All[0] = cast<ConstantInt>(pOutputMeta->getOperand(2))->getZExtValue();
            outputMeta.U64All[1] = cast<ConstantInt>(pOutputMeta->getOperand(3))->getZExtValue();

            auto pElemMeta = cast<Constant>(pOutputMeta->getOperand(1));
            auto pElemTy   = pOutputTy->getArrayElementType();

            if (outputMeta.IsBuiltIn)
            {
                LLPC_ASSERT(pLocOffset == nullptr);
                LLPC_ASSERT(operandIdx + 1 == indexOperands.size() - 1);

                auto pElemIdx = indexOperands[operandIdx + 1];
                return AddCallInstForOutputExport(pStoreValue,
                                                  pElemMeta,
                                                  nullptr,
                                                  pOutputTy->getArrayNumElements(),
                                                  InvalidValue,
                                                  pElemIdx,
                                                  pVertexIdx,
                                                  InvalidValue,
                                                  pInsertPos);
            }
            else
            {
                // NOTE: If the relative location offset is not specified, initialize it.
                if (pLocOffset == nullptr)
                {
                    pLocOffset = ConstantInt::get(m_pContext->Int32Ty(), 0);
                }

                // elemLocOffset = locOffset + stride * elemIdx
                uint32_t stride = cast<ConstantInt>(pOutputMeta->getOperand(0))->getZExtValue();
                auto pElemIdx = indexOperands[operandIdx + 1];
                Value* pElemLocOffset = BinaryOperator::CreateMul(ConstantInt::get(m_pContext->Int32Ty(), stride),
                                                                  pElemIdx,
                                                                  "",
                                                                  pInsertPos);
                pElemLocOffset = BinaryOperator::CreateAdd(pLocOffset, pElemLocOffset, "", pInsertPos);

                // Mark the end+1 possible location offset if the index is variable. The Builder call needs it
                // so it knows how many locations to mark as used by this access.
                if ((maxLocOffset == 0) && (isa<ConstantInt>(pElemIdx) == false))
                {
                    maxLocOffset = cast<ConstantInt>(pLocOffset)->getZExtValue() +
                                   stride * pOutputTy->getArrayNumElements();
                }

                return StoreOutputMember(pElemTy,
                                         pStoreValue,
                                         indexOperands,
                                         operandIdx + 1,
                                         maxLocOffset,
                                         pElemMeta,
                                         pElemLocOffset,
                                         pVertexIdx,
                                         pInsertPos);
            }
        }
        else if (pOutputTy->isStructTy())
        {
            // Structure type
            uint32_t memberIdx = cast<ConstantInt>(indexOperands[operandIdx + 1])->getZExtValue();

            auto pMemberTy = pOutputTy->getStructElementType(memberIdx);
            auto pMemberMeta = cast<Constant>(pOutputMeta->getOperand(memberIdx));

            return StoreOutputMember(pMemberTy,
                                     pStoreValue,
                                     indexOperands,
                                     operandIdx + 1,
                                     maxLocOffset,
                                     pMemberMeta,
                                     pLocOffset,
                                     pVertexIdx,
                                     pInsertPos);
        }
        else if (pOutputTy->isVectorTy())
        {
            // Vector type
            LLPC_ASSERT(operandIdx + 1 == indexOperands.size() - 1);
            auto pCompIdx = indexOperands[operandIdx + 1];

            return AddCallInstForOutputExport(pStoreValue,
                                              pOutputMeta,
                                              pLocOffset,
                                              maxLocOffset,
                                              InvalidValue,
                                              pCompIdx,
                                              pVertexIdx,
                                              InvalidValue,
                                              pInsertPos);
        }
    }
    else
    {
        // Last index operand
        LLPC_ASSERT(operandIdx == indexOperands.size() - 1);

        return AddCallInstForOutputExport(pStoreValue,
                                          pOutputMeta,
                                          pLocOffset,
                                          maxLocOffset,
                                          InvalidValue,
                                          nullptr,
                                          pVertexIdx,
                                          InvalidValue,
                                          pInsertPos);
    }

    LLPC_NEVER_CALLED();
}

// =====================================================================================================================
// Collects transform output info for geometry shader.
void SpirvLowerGlobal::CollectGsXfbOutputInfo(
    const llvm::Type *          pOutputTy,     // [in] Type of this output
    uint32_t                    locOffset,     // Relative location array offset, passed from aggregate type
    uint32_t                    xfbLocOffset,  // Transform feedback location offset (for array type)
    const ShaderInOutMetadata & outputMeta)    // [in] Metadata of this output
{
    LLPC_ASSERT(m_shaderStage == ShaderStageGeometry);
    LLPC_ASSERT(outputMeta.IsXfb == true);
    LLPC_ASSERT(outputMeta.IsBuiltIn == false);

    auto pResUsage = m_pContext->GetShaderResourceUsage(ShaderStageGeometry);

    uint32_t location = outputMeta.Value + outputMeta.Index + locOffset;

    GsOutLocInfo outLocInfo = {};
    outLocInfo.location = location;
    outLocInfo.isBuiltIn = false;
    outLocInfo.streamId = outputMeta.StreamId;

    XfbOutInfo xfbOutInfo = {};

    xfbOutInfo.xfbBuffer = outputMeta.XfbBuffer;
    xfbOutInfo.xfbOffset = outputMeta.XfbOffset;
    xfbOutInfo.is16bit = (pOutputTy->getScalarSizeInBits() == 16);
    xfbOutInfo.xfbLocOffset = xfbLocOffset;

    pResUsage->inOutUsage.gs.xfbOutsInfo[outLocInfo.u32All] = xfbOutInfo.u32All;
}

// =====================================================================================================================
// Replace a constant with instructions using a builder.
void SpirvLowerGlobal::ReplaceConstWithInsts(
    Constant* const pConst)   // [in/out] The constant to replace with instructions.
{
    SmallSet<Constant*, 8> otherConsts;

    for (User* const pUser : pConst->users())
    {
        if (Constant* const pOtherConst = dyn_cast<Constant>(pUser))
        {
            otherConsts.insert(pOtherConst);
        }
    }

    for (Constant* const pOtherConst : otherConsts)
    {
        ReplaceConstWithInsts(pOtherConst);
    }

    otherConsts.clear();

    SmallVector<Value*, 8> users;

    for (User* const pUser : pConst->users())
    {
        users.push_back(pUser);
    }

    for (Value* const pUser : users)
    {
        Instruction* const pInst = dyn_cast<Instruction>(pUser);
        LLPC_ASSERT(pInst != nullptr);

        // If the instruction is a phi node, we have to insert the new instructions in the correct predecessor.
        if (PHINode* const pPhiNode = dyn_cast<PHINode>(pInst))
        {
            const uint32_t incomingValueCount = pPhiNode->getNumIncomingValues();
            for (uint32_t i = 0; i < incomingValueCount; i++)
            {
                if (pPhiNode->getIncomingValue(i) == pConst)
                {
                    m_pBuilder->SetInsertPoint(pPhiNode->getIncomingBlock(i)->getTerminator());
                    break;
                }
            }
        }
        else
        {
            m_pBuilder->SetInsertPoint(pInst);
        }

        if (ConstantExpr* const pConstExpr = dyn_cast<ConstantExpr>(pConst))
        {
            Instruction* const pInsertInst = m_pBuilder->Insert(pConstExpr->getAsInstruction());
            pInst->replaceUsesOfWith(pConstExpr, pInsertInst);
        }
        else if (ConstantVector* const pConstVector = dyn_cast<ConstantVector>(pConst))
        {
            Value* pResultValue = UndefValue::get(pConstVector->getType());
            for (uint32_t i = 0; i < pConstVector->getNumOperands(); i++)
            {
                // Have to not use the builder here because it will constant fold and we are trying to undo that now!
                Instruction* const pInsertInst = InsertElementInst::Create(pResultValue,
                                                                           pConstVector->getOperand(i),
                                                                           m_pBuilder->getInt32(i));
                pResultValue = m_pBuilder->Insert(pInsertInst);
            }
            pInst->replaceUsesOfWith(pConstVector, pResultValue);
        }
        else
        {
            LLPC_NEVER_CALLED();
        }
    }

    pConst->removeDeadConstantUsers();
    pConst->destroyConstant();
}

// =====================================================================================================================
// Lowers buffer blocks.
void SpirvLowerGlobal::LowerBufferBlock()
{
    SmallVector<GlobalVariable*, 8> globalsToRemove;

    for (GlobalVariable& global : m_pModule->globals())
    {
        // Skip anything that is not a block.
        if (global.getAddressSpace() != SPIRAS_Uniform)
        {
            continue;
        }

        MDNode* const pResMetaNode = global.getMetadata(gSPIRVMD::Resource);
        LLPC_ASSERT(pResMetaNode != nullptr);

        const uint32_t descSet = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(0))->getZExtValue();
        const uint32_t binding = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(1))->getZExtValue();

        SmallVector<Constant*, 8> constantUsers;

        for (User* const pUser : global.users())
        {
            if (Constant* const pConst = dyn_cast<Constant>(pUser))
            {
                constantUsers.push_back(pConst);
            }
        }

        for (Constant* const pConst : constantUsers)
        {
            ReplaceConstWithInsts(pConst);
        }

        // Record of all the functions that our global is used within.
        SmallSet<Function*, 4> funcsUsedIn;

        for (User* const pUser : global.users())
        {
            if (Instruction* const pInst = dyn_cast<Instruction>(pUser))
            {
                funcsUsedIn.insert(pInst->getFunction());
            }
        }

        for (Function* const pFunc : funcsUsedIn)
        {
            // Check if our block is an array of blocks.
            if (global.getType()->getPointerElementType()->isArrayTy())
            {
                Type* const pElementType = global.getType()->getPointerElementType()->getArrayElementType();
                Type* const pBlockType = pElementType->getPointerTo(global.getAddressSpace());

                SmallVector<BitCastInst*, 8> bitCastsToModify;
                SmallVector<GetElementPtrInst*, 8> getElemPtrsToReplace;

                // We need to run over the users of the global, find the GEPs, and add a load for each.
                for (User* const pUser : global.users())
                {
                    // Skip over non-instructions.
                    if (isa<Instruction>(pUser) == false)
                    {
                        continue;
                    }

                    GetElementPtrInst* pGetElemPtr = dyn_cast<GetElementPtrInst>(pUser);

                    if (pGetElemPtr == nullptr)
                    {
                        // Skip all bitcasts, looking for a GEP.
                        for (BitCastInst* pBitCast = dyn_cast<BitCastInst>(pUser);
                             pBitCast != nullptr;
                             pBitCast = dyn_cast<BitCastInst>(pBitCast->getOperand(0)))
                        {
                            pGetElemPtr = dyn_cast<GetElementPtrInst>(pBitCast);
                        }

                        // If even after we've stripped away all the bitcasts we did not find a GEP, we need to modify
                        // the bitcast instead.
                        if (pGetElemPtr == nullptr)
                        {
                            BitCastInst* const pBitCast = dyn_cast<BitCastInst>(pUser);
                            LLPC_ASSERT(pBitCast != nullptr);

                            bitCastsToModify.push_back(pBitCast);
                            continue;
                        }
                    }

                    // Skip instructions in other functions.
                    if (pGetElemPtr->getFunction() != pFunc)
                    {
                        continue;
                    }

                    getElemPtrsToReplace.push_back(pGetElemPtr);
                }

                // All bitcasts recorded here are for GEPs that indexed by 0, 0 into the arrayed resource, and LLVM
                // has been clever enough to realise that doing a GEP of 0, 0 is actually a no-op (because the pointer
                // does not change!), and has removed it.
                for (BitCastInst* const pBitCast : bitCastsToModify)
                {
                    m_pBuilder->SetInsertPoint(pBitCast);

                    Value* const pBufferDesc = m_pBuilder->CreateLoadBufferDesc(descSet,
                                                                                binding,
                                                                                m_pBuilder->getInt32(0),
                                                                                false,
                                                                                m_pBuilder->getInt8Ty());

                    // If the global variable is a constant, the data it points to is invariant.
                    if (global.isConstant())
                    {
                        m_pBuilder->CreateInvariantStart(pBufferDesc);
                    }

                    pBitCast->replaceUsesOfWith(&global, m_pBuilder->CreateBitCast(pBufferDesc, pBlockType));
                }

                for (GetElementPtrInst* const pGetElemPtr : getElemPtrsToReplace)
                {
                    // The second index is the block offset, so we need at least two indices!
                    LLPC_ASSERT(pGetElemPtr->getNumIndices() >= 2);

                    m_pBuilder->SetInsertPoint(pGetElemPtr);

                    SmallVector<Value*, 8> indices;

                    for (Value* const pIndex : pGetElemPtr->indices())
                    {
                        indices.push_back(pIndex);
                    }

                    // The first index should always be zero.
                    LLPC_ASSERT(isa<ConstantInt>(indices[0]) && (cast<ConstantInt>(indices[0])->getZExtValue() == 0));

                    // The second index is the block index.
                    Value* const pBlockIndex = indices[1];

                    bool isNonUniform = false;

                    // Run the users of the block index to check for any nonuniform calls.
                    for (User* const pUser : pBlockIndex->users())
                    {
                        CallInst* const pCall = dyn_cast<CallInst>(pUser);

                        // If the user is not a call, bail.
                        if (pCall == nullptr)
                        {
                            continue;
                        }

                        const std::string nonUniformPrefix = std::string("_Z16") + std::string(gSPIRVMD::NonUniform);

                        // If the call is our non uniform decoration, record we are non uniform.
                        if (pCall->getCalledFunction()->getName().startswith(nonUniformPrefix))
                        {
                            isNonUniform = true;
                            break;
                        }
                    }

                    Value* const pBufferDesc = m_pBuilder->CreateLoadBufferDesc(descSet,
                                                                                binding,
                                                                                pBlockIndex,
                                                                                isNonUniform,
                                                                                m_pBuilder->getInt8Ty());

                    // If the global variable is a constant, the data it points to is invariant.
                    if (global.isConstant())
                    {
                        m_pBuilder->CreateInvariantStart(pBufferDesc);
                    }

                    Value* const pBitCast = m_pBuilder->CreateBitCast(pBufferDesc, pBlockType);

                    // We need to remove the block index from the original GEP indices so that we can use them.
                    indices[1] = indices[0];

                    ArrayRef<Value*> newIndices(indices);
                    newIndices = newIndices.drop_front(1);

                    Value* pNewGetElemPtr = nullptr;

                    if (pGetElemPtr->isInBounds())
                    {
                        pNewGetElemPtr = m_pBuilder->CreateInBoundsGEP(pBitCast, newIndices);
                    }
                    else
                    {
                        pNewGetElemPtr = m_pBuilder->CreateGEP(pBitCast, newIndices);
                    }

                    pGetElemPtr->replaceAllUsesWith(pNewGetElemPtr);
                    pGetElemPtr->eraseFromParent();
                }
            }
            else
            {
                m_pBuilder->SetInsertPoint(&pFunc->getEntryBlock(), pFunc->getEntryBlock().getFirstInsertionPt());

                Value* const pBufferDesc = m_pBuilder->CreateLoadBufferDesc(descSet,
                                                                            binding,
                                                                            m_pBuilder->getInt32(0),
                                                                            false,
                                                                            m_pBuilder->getInt8Ty());

                // If the global variable is a constant, the data it points to is invariant.
                if (global.isConstant())
                {
                    m_pBuilder->CreateInvariantStart(pBufferDesc);
                }

                Value* const pBitCast = m_pBuilder->CreateBitCast(pBufferDesc, global.getType());

                SmallVector<Instruction*, 8> usesToReplace;

                for (User* const pUser : global.users())
                {
                    // Skip over non-instructions that we've already made useless.
                    if (isa<Instruction>(pUser) == false)
                    {
                        continue;
                    }

                    Instruction* const pInst = cast<Instruction>(pUser);

                    // Skip instructions in other functions.
                    if (pInst->getFunction() != pFunc)
                    {
                        continue;
                    }

                    usesToReplace.push_back(pInst);
                }

                for (Instruction* const pUse : usesToReplace)
                {
                    pUse->replaceUsesOfWith(&global, pBitCast);
                }
            }
        }

        globalsToRemove.push_back(&global);
    }

    for (GlobalVariable* const pGlobal : globalsToRemove)
    {
        m_pContext->GetShaderResourceUsage(m_shaderStage)->resourceRead = true;
        if (pGlobal->isConstant() == false)
        {
            m_pContext->GetShaderResourceUsage(m_shaderStage)->resourceWrite = true;
        }

        pGlobal->dropAllReferences();
        pGlobal->eraseFromParent();
    }
}

// =====================================================================================================================
// Lowers push constants.
void SpirvLowerGlobal::LowerPushConsts()
{
    SmallVector<GlobalVariable*, 1> globalsToRemove;

    for (GlobalVariable& global : m_pModule->globals())
    {
        // Skip anything that is not a push constant.
        if ((global.getAddressSpace() != SPIRAS_Constant) || (global.hasMetadata(gSPIRVMD::PushConst) == false))
        {
            continue;
        }

        // There should only be a single push constant variable!
        LLPC_ASSERT(globalsToRemove.empty());

        SmallVector<Constant*, 8> constantUsers;

        for (User* const pUser : global.users())
        {
            if (Constant* const pConst = dyn_cast<Constant>(pUser))
            {
                constantUsers.push_back(pConst);
            }
        }

        for (Constant* const pConst : constantUsers)
        {
            ReplaceConstWithInsts(pConst);
        }

        // Record of all the functions that our global is used within.
        SmallSet<Function*, 4> funcsUsedIn;

        for (User* const pUser : global.users())
        {
            if (Instruction* const pInst = dyn_cast<Instruction>(pUser))
            {
                funcsUsedIn.insert(pInst->getFunction());
            }
        }

        for (Function* const pFunc : funcsUsedIn)
        {
            m_pBuilder->SetInsertPoint(&pFunc->getEntryBlock(), pFunc->getEntryBlock().getFirstInsertionPt());

            Type* const pPushConstantsType = ArrayType::get(m_pBuilder->getInt8Ty(), 512);
            Value* pPushConstants = m_pBuilder->CreateLoadPushConstantsPtr(pPushConstantsType);

            Type* const pCastType = global.getType()->getPointerElementType()->getPointerTo(ADDR_SPACE_CONST);
            pPushConstants = m_pBuilder->CreateBitCast(pPushConstants, pCastType);

            SmallVector<Instruction*, 8> usesToReplace;

            for (User* const pUser : global.users())
            {
                // Skip over non-instructions that we've already made useless.
                if (isa<Instruction>(pUser) == false)
                {
                    continue;
                }

                Instruction* const pInst = cast<Instruction>(pUser);

                // Skip instructions in other functions.
                if (pInst->getFunction() != pFunc)
                {
                    continue;
                }

                usesToReplace.push_back(pInst);
            }

            for (Instruction* const pInst : usesToReplace)
            {
                pInst->replaceUsesOfWith(&global, pPushConstants);
            }
        }

        globalsToRemove.push_back(&global);
    }

    for (GlobalVariable* const pGlobal : globalsToRemove)
    {
        pGlobal->dropAllReferences();
        pGlobal->eraseFromParent();
    }
}

// =====================================================================================================================
// Interpolates an element of the input.
void SpirvLowerGlobal::InterpolateInputElement(
    uint32_t        interpLoc,          // [in] Interpolation location, valid for fragment shader
                                        // (use "InterpLocUnknown" as don't-care value)
    Value*          pAuxInterpValue,    // [in] Auxiliary value of interpolation (valid for fragment shader):
                                        //   - Sample ID for "InterpLocSample"
                                        //   - Offset from the center of the pixel for "InterpLocCenter"
                                        //   - Vertex no. (0 ~ 2) for "InterpLocCustom"
    CallInst&       callInst)           // [in] "Call" instruction
{
    GetElementPtrInst* pGetElemPtr = cast<GetElementPtrInst>(callInst.getArgOperand(0));

    std::vector<Value*> indexOperands;
    for (uint32_t i = 0, indexOperandCount = pGetElemPtr->getNumIndices(); i < indexOperandCount; ++i)
    {
        indexOperands.push_back(ToInt32Value(m_pContext, pGetElemPtr->getOperand(1 + i), &callInst));
    }
    uint32_t operandIdx = 0;

    auto pInput = cast<GlobalVariable>(pGetElemPtr->getPointerOperand());
    auto pInputTy = pInput->getType()->getContainedType(0);

    MDNode* pMetaNode = pInput->getMetadata(gSPIRVMD::InOut);
    LLPC_ASSERT(pMetaNode != nullptr);
    auto pInputMeta = mdconst::dyn_extract<Constant>(pMetaNode->getOperand(0));

    if (pGetElemPtr->hasAllConstantIndices())
    {
        auto pLoadValue = LoadInOutMember(pInputTy,
                                          SPIRAS_Input,
                                          indexOperands,
                                          operandIdx,
                                          0,
                                          pInputMeta,
                                          nullptr,
                                          nullptr,
                                          interpLoc,
                                          pAuxInterpValue,
                                          &callInst);

        m_interpCalls.insert(&callInst);
        callInst.replaceAllUsesWith(pLoadValue);
    }
    else  // Interpolant an element via dynamic index by extending interpolant to each element
    {
        auto pInterpValueTy = pInputTy;
        auto pInterpPtr = new AllocaInst(pInterpValueTy,
                                         m_pModule->getDataLayout().getAllocaAddrSpace(),
                                         "",
                                         &*(m_pEntryPoint->begin()->getFirstInsertionPt()));

        std::vector<uint32_t> arraySizes;
        std::vector<uint32_t> indexOperandIdxs;
        uint32_t flattenElemCount = 1;
        auto pElemTy = pInputTy;
        for (uint32_t i = 1, indexOperandCount = indexOperands.size(); i < indexOperandCount; ++i)
        {
            if (isa<ConstantInt>(indexOperands[i]))
            {
                uint32_t index = (cast<ConstantInt>(indexOperands[i]))->getZExtValue();
                pElemTy = pElemTy->getContainedType(index);
            }
            else
            {
                arraySizes.push_back(cast<ArrayType>(pElemTy)->getNumElements());
                pElemTy = pElemTy->getArrayElementType();
                flattenElemCount *= arraySizes.back();
                indexOperandIdxs.push_back(i);
            }
        }

        const uint32_t arraySizeCount = arraySizes.size();
        SmallVector<uint32_t, 4> elemStrides;
        elemStrides.resize(arraySizeCount, 1);
        for (uint32_t i = arraySizeCount - 1; i > 0; --i)
        {
            elemStrides[i - 1] = arraySizes[i] * elemStrides[i];
        }

        std::vector<Value*> newIndexOperands = indexOperands;
        Value* pInterpValue = UndefValue::get(pInterpValueTy);

        for (uint32_t elemIdx = 0; elemIdx < flattenElemCount; ++elemIdx)
        {
            uint32_t flattenElemIdx = elemIdx;
            for (uint32_t arraySizeIdx = 0; arraySizeIdx < arraySizeCount; ++arraySizeIdx)
            {
                uint32_t index = flattenElemIdx / elemStrides[arraySizeIdx];
                flattenElemIdx = flattenElemIdx - index * elemStrides[arraySizeIdx];
                newIndexOperands[indexOperandIdxs[arraySizeIdx]] = ConstantInt::get(m_pContext->Int32Ty(), index, true);
            }

            auto pLoadValue = LoadInOutMember(pInputTy,
                                              SPIRAS_Input,
                                              newIndexOperands,
                                              operandIdx,
                                              0,
                                              pInputMeta,
                                              nullptr,
                                              nullptr,
                                              interpLoc,
                                              pAuxInterpValue,
                                              &callInst);

            std::vector<uint32_t> idxs;
            for (auto indexIt = newIndexOperands.begin() + 1; indexIt != newIndexOperands.end(); ++indexIt)
            {
                idxs.push_back((cast<ConstantInt>(*indexIt))->getZExtValue());
            }
            pInterpValue = InsertValueInst::Create(pInterpValue, pLoadValue, idxs, "", &callInst);
        }
        new StoreInst(pInterpValue, pInterpPtr, &callInst);

        auto pInterpElemPtr = GetElementPtrInst::Create(nullptr, pInterpPtr, indexOperands, "", &callInst);

        auto pInterpElemValue = new LoadInst(pInterpElemPtr, "", &callInst);
        callInst.replaceAllUsesWith(pInterpElemValue);

        if (callInst.user_empty())
        {
            callInst.dropAllReferences();
            callInst.eraseFromParent();
        }
    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for globals.
INITIALIZE_PASS(SpirvLowerGlobal, DEBUG_TYPE,
                "Lower SPIR-V globals (global variables, inputs, and outputs)", false, false)
