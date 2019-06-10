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
                GetElementPtrInst* pGetElemPtr = cast<GetElementPtrInst>(pLoadSrc);

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

                auto pLoadValue = LoadInOutMember(pInputTy,
                                                  SPIRAS_Input,
                                                  indexOperands,
                                                  operandIdx,
                                                  pInputMeta,
                                                  nullptr,
                                                  nullptr,
                                                  interpLoc,
                                                  pAuxInterpValue,
                                                  &callInst);

                m_interpCalls.insert(&callInst);
                callInst.replaceAllUsesWith(pLoadValue);
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
                BuiltIn builtInId = static_cast<BuiltIn>(inOutMeta.Value);
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
                BuiltIn builtInId = static_cast<BuiltIn>(inOutMeta.Value);
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
                BuiltIn builtInId = static_cast<BuiltIn>(outputMeta.Value);
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
                BuiltIn builtInId = static_cast<BuiltIn>(outputMeta.Value);
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
            AddCallInstForOutputExport(pOutputValue, pMeta, nullptr, 0, nullptr, nullptr, InvalidValue, pRetInst);
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
                AddCallInstForOutputExport(pOutputValue, pMeta, nullptr, 0, nullptr, nullptr, emitStreamId, pEmitCall);
            }
        }
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

            BuiltIn builtInId = static_cast<BuiltIn>(inOutMeta.Value);

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
                // Normal built-ins without vertex indexing
                std::string builtInName = getNameMap(builtInId).map(builtInId);
                LLPC_ASSERT(builtInName.find("BuiltIn")  == 0);
                std::string instName =
                    (addrSpace == SPIRAS_Input) ? LlpcName::InputImportBuiltIn : LlpcName::OutputImportBuiltIn;
                instName += builtInName.substr(strlen("BuiltIn"));

                std::vector<Value*> args;
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), builtInId));

                if ((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval))
                {
                    // NOTE: For tessellation shader, we add element index as an addition parameter to do addressing for
                    // the input/output. Here, this is invalid value.
                    pElemIdx = ConstantInt::get(m_pContext->Int32Ty(), InvalidValue);
                    args.push_back(pElemIdx);
                }

                if ((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval) ||
                    (m_shaderStage == ShaderStageGeometry))
                {
                    // NOTE: For gl_in[i].XXX/gl_out[i].XXX, we add vertex indexing as an additional parameter to do
                    // addressing for the input/output.
                    if (pVertexIdx == nullptr)
                    {
                        // When vertex indexing is not specified, we set it to don't-care value
                        pVertexIdx = ConstantInt::get(m_pContext->Int32Ty(), InvalidValue);
                    }
                    args.push_back(pVertexIdx);
                }
                else
                {
                    // Vertex indexing is not valid for other shader stages
                    LLPC_ASSERT(pVertexIdx == nullptr);
                }

                AddTypeMangling(pInOutTy, args, instName);
                pInOutValue = EmitCall(m_pModule, instName, pInOutTy, args, NoAttrib, pInsertPos);
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

        std::string instName;

        if (interpLoc != InterpLocUnknown)
        {
            LLPC_ASSERT(m_shaderStage == ShaderStageFragment);

            if (interpLoc != InterpLocCustom)
            {
                // Add intrinsic to calculate I/J for interpolation function
                std::string evalInstName;
                std::vector<Value*> evalArgs;
                auto pResUsage = m_pContext->GetShaderResourceUsage(ShaderStageFragment);

                if (interpLoc == InterpLocCentroid)
                {
                    evalInstName = LlpcName::InputImportBuiltIn;
                    if (inOutMeta.InterpMode == InterpModeNoPersp)
                    {
                        evalInstName += "InterpLinearCentroid";
                        evalArgs.push_back(ConstantInt::get(m_pContext->Int32Ty(), BuiltInInterpLinearCentroid));
                        pResUsage->builtInUsage.fs.noperspective = true;
                        pResUsage->builtInUsage.fs.centroid = true;
                    }
                    else
                    {
                        evalInstName += "InterpPerspCentroid";
                        evalArgs.push_back(ConstantInt::get(m_pContext->Int32Ty(), BuiltInInterpPerspCentroid));
                        pResUsage->builtInUsage.fs.smooth = true;
                        pResUsage->builtInUsage.fs.centroid = true;
                    }
                }
                else
                {
                    evalInstName = LlpcName::InputInterpEval;
                    std::string suffix;

                    if (interpLoc == InterpLocCenter)
                    {
                        evalInstName += "offset";
                        evalArgs.push_back(pAuxInterpValue);

                        // NOTE: Here we add suffix to differentiate the type of "offset" (could be 16-bit or 32-bit
                        // floating-point type)
                        suffix = "." + GetTypeName(pAuxInterpValue->getType());
                    }
                    else
                    {
                        evalInstName += "sample";
                        evalArgs.push_back(pAuxInterpValue);
                        pResUsage->builtInUsage.fs.runAtSampleRate = true;
                    }

                    if (inOutMeta.InterpMode == InterpModeNoPersp)
                    {
                        evalInstName += ".noperspective";
                        pResUsage->builtInUsage.fs.noperspective = true;
                        pResUsage->builtInUsage.fs.center = true;
                    }
                    else
                    {
                        pResUsage->builtInUsage.fs.smooth = true;
                        pResUsage->builtInUsage.fs.pullMode = true;
                    }

                    evalInstName += suffix;
                }

                auto pIJ = EmitCall(m_pModule, evalInstName, m_pContext->Floatx2Ty(), evalArgs, NoAttrib, pInsertPos);
                pAuxInterpValue = pIJ;
            }

            // Prepare arguments for input import call
            instName  = LlpcName::InputImportInterpolant;

            auto pLoc = ConstantInt::get(m_pContext->Int32Ty(), inOutMeta.Value);

            // NOTE: If the relative location offset is not specified, initialize it to 0.
            if (pLocOffset == nullptr)
            {
                pLocOffset = ConstantInt::get(m_pContext->Int32Ty(), 0);
            }

            args.push_back(pLoc);
            args.push_back(pLocOffset);
        }
        else if (inOutMeta.IsBuiltIn)
        {
            instName = (addrSpace == SPIRAS_Input) ? LlpcName::InputImportBuiltIn : LlpcName::OutputImportBuiltIn;

            BuiltIn builtInId = static_cast<BuiltIn>(inOutMeta.Value);
            std::string builtInName = getNameMap(builtInId).map(builtInId);

            LLPC_ASSERT(builtInName.find("BuiltIn") == 0);
            instName += builtInName.substr(strlen("BuiltIn"));

            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), builtInId));
        }
        else
        {
            instName = (addrSpace == SPIRAS_Input) ? LlpcName::InputImportGeneric : LlpcName::OutputImportGeneric;

            auto pLoc = ConstantInt::get(m_pContext->Int32Ty(), inOutMeta.Value);

            // NOTE: If the relative location offset is not specified, initialize it to 0.
            if (pLocOffset == nullptr)
            {
                pLocOffset = ConstantInt::get(m_pContext->Int32Ty(), 0);
            }

            if ((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval))
            {
                // NOTE: For tessellation shader, we treats the location to two parts:
                // startLoc = loc + locOffset
                args.push_back(pLoc);
                args.push_back(pLocOffset);
            }
            else
            {
                auto pStartLoc = BinaryOperator::CreateAdd(pLoc, pLocOffset, "", pInsertPos);
                args.push_back(pStartLoc);
            }
        }

        if ((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval) ||
            (interpLoc != InterpLocUnknown))
        {
            if (inOutMeta.IsBuiltIn)
            {
                if (pElemIdx == nullptr)
                {
                    // When element indexing is not specified, we set it to don't-care value
                    pElemIdx = ConstantInt::get(m_pContext->Int32Ty(), InvalidValue);
                }
            }
            else
            {
                LLPC_ASSERT(pInOutTy->isSingleValueType());

                uint32_t elemIdx = inOutMeta.Component;
                LLPC_ASSERT(inOutMeta.Component <= 3);
                if (pInOutTy->getScalarSizeInBits() == 64)
                {
                    LLPC_ASSERT(inOutMeta.Component % 2 == 0); // Must be even for 64-bit type
                    elemIdx = inOutMeta.Component / 2;
                }

                if (pElemIdx != nullptr)
                {
                    pElemIdx = BinaryOperator::CreateAdd(pElemIdx,
                                                         ConstantInt::get(m_pContext->Int32Ty(), elemIdx),
                                                         "",
                                                         pInsertPos);
                }
                else
                {
                    pElemIdx = ConstantInt::get(m_pContext->Int32Ty(), elemIdx);
                }
            }

            args.push_back(pElemIdx);
        }
        else
        {
            // Element indexing is not valid for other shader stages
            LLPC_ASSERT(pElemIdx == nullptr);

            if ((inOutMeta.IsBuiltIn == false) && (m_shaderStage != ShaderStageCompute))
            {
                LLPC_ASSERT(pInOutTy->isSingleValueType());

                uint32_t elemIdx = inOutMeta.Component;
                LLPC_ASSERT(inOutMeta.Component <= 3);
                if (pInOutTy->getScalarSizeInBits() == 64)
                {
                    LLPC_ASSERT(inOutMeta.Component % 2 == 0); // Must be even for 64-bit type
                    elemIdx = inOutMeta.Component / 2;
                }

                pElemIdx = ConstantInt::get(m_pContext->Int32Ty(), elemIdx);
                args.push_back(pElemIdx);
            }
        }

        if ((m_shaderStage == ShaderStageTessControl) ||
            (m_shaderStage == ShaderStageTessEval) ||
            (m_shaderStage == ShaderStageGeometry))
        {
            // NOTE: For tessellation shader and geometry shader, we add vertex indexing as an addition parameter to
            // do addressing for the input/output.
            if (pVertexIdx == nullptr)
            {
                // When vertex indexing is not specified, we set it to don't-care value
                pVertexIdx = ConstantInt::get(m_pContext->Int32Ty(), InvalidValue);
            }
            args.push_back(pVertexIdx);
        }
        else
        {
            // Vertex indexing is not valid for other shader stages
            LLPC_ASSERT(pVertexIdx == nullptr);
        }

        if (interpLoc != InterpLocUnknown)
        {
            // Add interpolation mode and auxiliary value of interpolation (calcuated I/J or vertex no.) for
            // interpolant inputs of fragment shader
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), inOutMeta.InterpMode));
            args.push_back(pAuxInterpValue);
        }
        else if ((m_shaderStage == ShaderStageFragment) && (inOutMeta.IsBuiltIn == false))
        {
            // Add interpolation mode and location for generic inputs of fragment shader
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), inOutMeta.InterpMode));
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), inOutMeta.InterpLoc));
        }

        //
        // VS:  @llpc.input.import.generic.%Type%(i32 location, i32 elemIdx)
        //      @llpc.input.import.builtin.%BuiltIn%.%Type%(i32 builtInId)
        //
        // TCS: @llpc.input.import.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexIdx)
        //      @llpc.input.import.builtin.%BuiltIn%.%Type%(i32 builtInId, i32 elemIdx, i32 vertexIdx)
        //
        //      @llpc.output.import.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexIdx)
        //      @llpc.output.import.builtin.%BuiltIn%.%Type%(i32 builtInId, i32 elemIdx, i32 vertexIdx)
        //
        //
        // TES: @llpc.input.import.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexIdx)
        //      @llpc.input.import.builtin.%BuiltIn%.%Type%(i32 builtInId, i32 elemIdx, i32 vertexIdx)

        // GS:  @llpc.input.import.generic.%Type%(i32 location, i32 elemIdx, i32 vertexIdx)
        //      @llpc.input.import.builtin.%BuiltIn%.%Type%(i32 builtInId, i32 vertexIdx)
        //
        // FS:  @llpc.input.import.generic.%Type%(i32 location, i32 elemIdx, i32 interpMode, i32 interpLoc)
        //      @llpc.input.import.builtin.%BuiltIn%.%Type%(i32 builtInId)
        //      @llpc.input.import.interpolant.%Type%(i32 location, i32 locOffset, i32 elemIdx,
        //                                            i32 interpMode, <2 x float> | i32 auxInterpValue)
        //
        // CS:  @llpc.input.import.builtin.%BuiltIn%.%Type%(i32 builtInId)
        //
        //
        // Common: @llpc.input.import.builtin.%BuiltIn%.%Type%(i32 builtInId)
        //
        if (inOutMeta.IsBuiltIn)
        {
            BuiltIn builtInId = static_cast<BuiltIn>(inOutMeta.Value);
            if ((builtInId == BuiltInSubgroupLocalInvocationId) ||
                (builtInId == BuiltInSubgroupEqMaskKHR)         ||
                (builtInId == BuiltInSubgroupGeMaskKHR)         ||
                (builtInId == BuiltInSubgroupGtMaskKHR)         ||
                (builtInId == BuiltInSubgroupLeMaskKHR)         ||
                (builtInId == BuiltInSubgroupLtMaskKHR))
            {
                // NOTE: For those common built-ins that are stage independent and the implementation body is in the
                // external GLSL emulation libarary (.ll files), the import calls could be simplified.
                args.clear();
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), builtInId));

                if ((builtInId == BuiltInSubgroupEqMaskKHR)     ||
                    (builtInId == BuiltInSubgroupGeMaskKHR)     ||
                    (builtInId == BuiltInSubgroupGtMaskKHR)     ||
                    (builtInId == BuiltInSubgroupLeMaskKHR)     ||
                    (builtInId == BuiltInSubgroupLtMaskKHR))
                {
                    // NOTE: Glslang has a bug. For gl_SubGroupXXXMaskARB, they are implemented as "uint64_t" while
                    // for gl_subgroupXXXMask they are "uvec4". And the SPIR-V enumerants "BuiltInSubgroupXXXMaskKHR"
                    // and "BuiltInSubgroupXXXMask" share the same numeric values.
                    if (pInOutTy->isIntegerTy(64) == false)
                    {
                        // Not uint64_t, must be uvec4
                        LLPC_ASSERT(pInOutTy->isVectorTy() &&
                                    pInOutTy->getVectorElementType()->isIntegerTy(32) &&
                                    pInOutTy->getVectorNumElements() == 4);

                        instName.replace(instName.find("KHR"), 3, ""); // Get rid of "KHR" suffix
                    }
                }
            }
        }

        AddTypeMangling(pInOutTy, args, instName);
        pInOutValue = EmitCall(m_pModule, instName, pInOutTy, args, NoAttrib, pInsertPos);
    }

    return pInOutValue;
}

// =====================================================================================================================
// Inserts LLVM call instruction to export output.
void SpirvLowerGlobal::AddCallInstForOutputExport(
    Value*       pOutputValue, // [in] Value exported to output
    Constant*    pOutputMeta,  // [in] Metadata of this output
    Value*       pLocOffset,   // [in] Relative location offset, passed from aggregate type
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
            BuiltIn builtInId = static_cast<BuiltIn>(outputMeta.Value);

            // NOTE: For tessellation shader, vertex indexing is handled by "load"/"store" instruction lowering.
            std::string instName = LlpcName::OutputExportBuiltIn;
            std::string builtInName = getNameMap(builtInId).map(builtInId);

            LLPC_ASSERT(builtInName.find("BuiltIn")  == 0);
            instName += builtInName.substr(strlen("BuiltIn"));

            std::vector<Value*> args;
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), builtInId));

            if (m_shaderStage == ShaderStageTessControl)
            {
                // NOTE: For tessellation control shader, we add element index as an addition parameter to do
                // addressing for the output. Here, this is invalid value.
                pElemIdx = ConstantInt::get(m_pContext->Int32Ty(), InvalidValue);
                args.push_back(pElemIdx);

                // NOTE: For gl_out[i].XXX, we add vertex indexing as an additional parameter to do addressing
                // for the output.
                if (pVertexIdx == nullptr)
                {
                    // When vertex indexing is not specified, we set it to don't-care value
                    pVertexIdx = ConstantInt::get(m_pContext->Int32Ty(), InvalidValue);
                }
                args.push_back(pVertexIdx);
            }
            else
            {
                // Vertex indexing is not valid for other shader stages
                LLPC_ASSERT(pVertexIdx == nullptr);
            }

            if (m_shaderStage == ShaderStageGeometry)
            {
                // NOTE: For geometry shader, we add stream ID for outputs.
                LLPC_ASSERT(emitStreamId == outputMeta.StreamId);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), emitStreamId));
            }
            else
            {
                // ID of emitted vertex stream is not valid for other shader stages
                LLPC_ASSERT(emitStreamId == InvalidValue);
            }

            args.push_back(pOutputValue);

            AddTypeMangling(nullptr, args, instName);
            EmitCall(m_pModule, instName, m_pContext->VoidTy(), args, NoAttrib, pInsertPos);
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

            AddCallInstForOutputExport(pMember, pMemberMeta, pLocOffset, xfbLocOffset, nullptr, pVertexIdx, emitStreamId, pInsertPos);
        }
    }
    else
    {
        // Normal scalar or vector type
        std::vector<Value*> args;
        Constant* pInOutMetaConst = cast<Constant>(pOutputMeta);
        outputMeta.U64All[0] = cast<ConstantInt>(pInOutMetaConst->getOperand(0))->getZExtValue();
        outputMeta.U64All[1] = cast<ConstantInt>(pInOutMetaConst->getOperand(1))->getZExtValue();

        if ((m_shaderStage == ShaderStageGeometry) && (emitStreamId != outputMeta.StreamId))
        {
            // NOTE: For geometry shader, if the output is not bound to this vertex stream, we skip processing.
            return;
        }

        LLPC_ASSERT(outputMeta.IsLoc || outputMeta.IsBuiltIn || outputMeta.IsXfb);

        std::string instName;

        // NOTE: For transform feedback outputs, additional stream-out export call will be generated.
        if (outputMeta.IsXfb)
        {
            uint32_t locOffset = 0;
            if (pLocOffset != nullptr)
            {
                locOffset = (dyn_cast<ConstantInt>(pLocOffset))->getZExtValue();
            }

            LLPC_ASSERT(xfbLocOffset != InvalidValue);
            auto pXfbLocOffset = ConstantInt::get(m_pContext->Int32Ty(), xfbLocOffset + outputMeta.XfbLoc);

            // XFB: @llpc.output.export.xfb.%Type%(i32 xfbBuffer, i32 xfbOffset, i32 xfbLocOffset, %Type% outputValue)
            instName = LlpcName::OutputExportXfb;
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), outputMeta.XfbBuffer));
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), outputMeta.XfbOffset));
            args.push_back(pXfbLocOffset);
            args.push_back(pOutputValue);
            AddTypeMangling(nullptr, args, instName);
            EmitCall(m_pModule, instName, m_pContext->VoidTy(), args, NoAttrib, pInsertPos);

            if ((m_shaderStage == ShaderStageGeometry) && (outputMeta.IsBuiltIn == false))
            {
                CollectGsXfbOutputInfo(pOutputValue->getType(),
                                       locOffset,
                                       xfbLocOffset + outputMeta.XfbLoc,
                                       outputMeta);
            }
        }

        args.clear();
        if (outputMeta.IsBuiltIn)
        {
            instName = LlpcName::OutputExportBuiltIn;
            BuiltIn builtInId = static_cast<BuiltIn>(outputMeta.Value);
            std::string builtInName = getNameMap(builtInId).map(builtInId);

            LLPC_ASSERT(builtInName.find("BuiltIn")  == 0);
            instName += builtInName.substr(strlen("BuiltIn"));

            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), builtInId));
        }
        else
        {
            instName = LlpcName::OutputExportGeneric;

            LLPC_ASSERT(((outputMeta.Index == 1) && (outputMeta.Value == 0)) || (outputMeta.Index == 0));
            auto pLoc = ConstantInt::get(m_pContext->Int32Ty(), outputMeta.Value + outputMeta.Index);

            // NOTE: If the relative location offset is not specified, initialize it to 0.
            if (pLocOffset == nullptr)
            {
                pLocOffset = ConstantInt::get(m_pContext->Int32Ty(), 0);
            }

            if (m_shaderStage == ShaderStageTessControl)
            {
                // NOTE: For tessellation control shader, we treat the location as two parts:
                // startLoc = loc + locOffset
                args.push_back(pLoc);
                args.push_back(pLocOffset);
            }
            else
            {
                auto pStartLoc = BinaryOperator::CreateAdd(pLoc, pLocOffset, "", pInsertPos);
                args.push_back(pStartLoc);
            }
        }

        if (m_shaderStage == ShaderStageTessControl)
        {
            if (outputMeta.IsBuiltIn)
            {
                if (pElemIdx == nullptr)
                {
                    // When element indexing is not specified, we set it to don't-care value
                    pElemIdx = ConstantInt::get(m_pContext->Int32Ty(), InvalidValue);
                }
            }
            else
            {
                LLPC_ASSERT(pOutputTy->isSingleValueType());

                uint32_t elemIdx = outputMeta.Component;
                LLPC_ASSERT(outputMeta.Component <= 3);
                if (pOutputTy->getScalarSizeInBits() == 64)
                {
                    LLPC_ASSERT(outputMeta.Component % 2 == 0); // Must be even for 64-bit type
                    elemIdx = outputMeta.Component / 2;
                }

                if (pElemIdx != nullptr)
                {
                    pElemIdx = BinaryOperator::CreateAdd(pElemIdx,
                                                         ConstantInt::get(m_pContext->Int32Ty(), elemIdx),
                                                         "",
                                                         pInsertPos);
                }
                else
                {
                    pElemIdx = ConstantInt::get(m_pContext->Int32Ty(), elemIdx);
                }
            }

            args.push_back(pElemIdx);
        }
        else
        {
            // Element indexing is not valid for other shader stages
            LLPC_ASSERT(pElemIdx == nullptr);

            if (m_shaderStage != ShaderStageCompute)
            {
                LLPC_ASSERT(pOutputTy->isSingleValueType());

                uint32_t elemIdx = outputMeta.Component;
                LLPC_ASSERT(outputMeta.Component <= 3);
                if (pOutputTy->getScalarSizeInBits() == 64)
                {
                    LLPC_ASSERT(outputMeta.Component % 2 == 0); // Must be even for 64-bit type
                    elemIdx = outputMeta.Component / 2;
                }

                pElemIdx = ConstantInt::get(m_pContext->Int32Ty(), elemIdx);
                args.push_back(pElemIdx);
            }
        }

        if (m_shaderStage == ShaderStageTessControl)
        {
            // NOTE: For tessellation control shader, we add vertex indexing as an addition parameter to do addressing
            // for the output.
            if (pVertexIdx == nullptr)
            {
                // When vertex indexing is not specified, we set it to don't-care value
                pVertexIdx = ConstantInt::get(m_pContext->Int32Ty(), InvalidValue);
            }
            args.push_back(pVertexIdx);
        }
        else
        {
            // Vertex indexing is not valid for other shader stages
            LLPC_ASSERT(pVertexIdx == nullptr);
        }

        if (m_shaderStage == ShaderStageGeometry)
        {
            // NOTE: For geometry shader, we add stream ID for outputs.
            LLPC_ASSERT(emitStreamId == outputMeta.StreamId);
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), emitStreamId));
        }
        else
        {
            // ID of emitted vertex stream is not valid for other shader stages
            LLPC_ASSERT(emitStreamId == InvalidValue);
        }

        args.push_back(pOutputValue);

        //
        // VS:  @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
        //      @llpc.output.export.builtin.%BuiltIn%(i32 builtInId, %Type% outputValue)
        //
        // TCS: @llpc.output.export.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexIdx,
        //                                         %Type% outputValue)
        //      @llpc.output.export.builtin.%BuiltIn%.%Type%(i32 builtInId, i32 elemIdx, i32 vertexIdx,
        //                                                   %Type% outputValue)
        //
        // TES: @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
        //      @llpc.output.export.builtin.%BuiltIn%.%Type%(i32 builtInId, %Type% outputValue)

        // GS:  @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, i32 streamId, %Type% outputValue)
        //      @llpc.output.export.builtin.%BuiltIn%(i32 builtInId, i32 streamId, %Type% outputValue)
        //
        // FS:  @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
        //      @llpc.output.export.builtin.%BuiltIn%(i32 builtInId, %Type% outputValue)
        //
        AddTypeMangling(nullptr, args, instName);
        EmitCall(m_pModule, instName, m_pContext->VoidTy(), args, NoAttrib, pInsertPos);
    }
}

// =====================================================================================================================
// Inserts instructions to load value from input/ouput member.
Value* SpirvLowerGlobal::LoadInOutMember(
    Type*                      pInOutTy,        // [in] Type of this input/output member
    uint32_t                   addrSpace,       // Address space
    const std::vector<Value*>& indexOperands,   // [in] Index operands
    uint32_t                   operandIdx,      // Index of the index operand in processing
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

                return LoadInOutMember(pElemTy,
                                       addrSpace,
                                       indexOperands,
                                       operandIdx + 1,
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

                return StoreOutputMember(pElemTy,
                                         pStoreValue,
                                         indexOperands,
                                         operandIdx + 1,
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

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for globals.
INITIALIZE_PASS(SpirvLowerGlobal, DEBUG_TYPE,
                "Lower SPIR-V globals (global variables, inputs, and outputs)", false, false)
