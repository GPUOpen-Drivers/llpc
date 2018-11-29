/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2018 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerBufferOp.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerBufferOp.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-spirv-lower-buffer-op"

#include "llvm/IR/Constant.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <stack>
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcSpirvLowerBufferOp.h"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char SpirvLowerBufferOp::ID = 0;

// =====================================================================================================================
SpirvLowerBufferOp::SpirvLowerBufferOp()
    :
    SpirvLower(ID),
    m_restoreMeta(false)
{
    initializeSpirvLowerBufferOpPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerBufferOp::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Buffer-Op\n");

    SpirvLower::Init(&module);

    // Visit module to restore per-instruction metadata
    m_restoreMeta = true;
    visit(m_pModule);
    m_restoreMeta = false;

    // Invoke handling of "load" and "store" instruction
    visit(m_pModule);

    std::unordered_set<GetElementPtrInst*> getElemInsts;

    // Remove unnecessary "load" instructions
    for (auto pLoadInst : m_loadInsts)
    {
        GetElementPtrInst* pGetElemInst = dyn_cast<GetElementPtrInst>(pLoadInst->getOperand(0)); // Load source
        if (pGetElemInst != nullptr)
        {
            getElemInsts.insert(pGetElemInst);
        }
        pLoadInst->dropAllReferences();
        pLoadInst->eraseFromParent();
    }

    // Remove unnecessary "getelementptr" instructions which are referenced by "load" instructions only
    for (auto pGetElemInst : getElemInsts)
    {
        if (pGetElemInst->use_empty())
        {
            pGetElemInst->dropAllReferences();
            pGetElemInst->eraseFromParent();
        }
    }
    m_loadInsts.clear();
    getElemInsts.clear();

    // Remove unnecessary "store" instructions
    for (auto pStoreInst : m_storeInsts)
    {
        GetElementPtrInst* pGetElemInst = dyn_cast<GetElementPtrInst>(pStoreInst->getOperand(1)); // Store destination
        if (pGetElemInst != nullptr)
        {
            getElemInsts.insert(pGetElemInst);
        }
        pStoreInst->dropAllReferences();
        pStoreInst->eraseFromParent();
    }

    // Remove unnecessary "getelementptr" instructions which are referenced by "store" instructions only
    for (auto pGetElemInst : getElemInsts)
    {
        if (pGetElemInst->use_empty())
        {
            pGetElemInst->dropAllReferences();
            pGetElemInst->eraseFromParent();
        }
    }
    m_storeInsts.clear();
    getElemInsts.clear();

    // Remove unnecessary "call" instructions
    for (auto pCallInst : m_callInsts)
    {
        GetElementPtrInst* pGetElemInst = dyn_cast<GetElementPtrInst>(pCallInst->getOperand(0)); // Memory pointer
        if (pGetElemInst != nullptr)
        {
            getElemInsts.insert(pGetElemInst);
        }
        pCallInst->dropAllReferences();
        pCallInst->eraseFromParent();
    }

    // Remove unnecessary "getelementptr" instructions which are referenced by "call" instructions only
    for (auto pGetElemInst : getElemInsts)
    {
        if (pGetElemInst->use_empty())
        {
            pGetElemInst->dropAllReferences();
            pGetElemInst->eraseFromParent();
        }
    }
    m_callInsts.clear();
    getElemInsts.clear();

    return true;
}

// =====================================================================================================================
// Visits "call" instruction.
void SpirvLowerBufferOp::visitCallInst(
    CallInst& callInst) // [in] "Call" instruction
{
    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

    auto mangledName = pCallee->getName();

    if (m_restoreMeta)
    {
        // Restore non-uniform metadata from metadata instruction.
        LLPC_ASSERT(strlen(gSPIRVMD::NonUniform) == 16);
        const std::string NonUniformPrefix = std::string("_Z16") + std::string(gSPIRVMD::NonUniform);
        if (mangledName.startswith(NonUniformPrefix))
        {
            auto pNonUniform = callInst.getOperand(0);
            cast<Instruction>(pNonUniform)->setMetadata(gSPIRVMD::NonUniform, m_pContext->GetEmptyMetadataNode());
        }
        return;
    }

    if (mangledName.find("ArrayLength") != std::string::npos)
    {
        // Array length call

        // result = ArrayLengthCall(pointer, member index)
        Value* pBufferPtr = callInst.getOperand(0);

        if (pBufferPtr->getType()->getPointerAddressSpace() == SPIRAS_Uniform)
        {
            GetElementPtrInst* pGetElemInst = nullptr;
            Instruction*       pConstExpr   = nullptr;

            if (isa<GetElementPtrInst>(pBufferPtr))
            {
                pGetElemInst = dyn_cast<GetElementPtrInst>(pBufferPtr);
            }
            else if (isa<ConstantExpr>(pBufferPtr))
            {
                pConstExpr = dyn_cast<ConstantExpr>(pBufferPtr)->getAsInstruction();
                pGetElemInst = dyn_cast<GetElementPtrInst>(pConstExpr);
            }

            GlobalVariable* pBlock = (pGetElemInst != nullptr) ?
                                         cast<GlobalVariable>(pGetElemInst->getPointerOperand()) :
                                         cast<GlobalVariable>(pBufferPtr);
            auto pBlockTy = pBlock->getType()->getPointerElementType();

            // Calculate block offset
            Value* pBlockOffset = nullptr;
            uint32_t stride  = 0;

            if (pGetElemInst != nullptr)
            {
                std::vector<Value*> indexOperands;
                for (uint32_t i = 0, indexOperandCount = pGetElemInst->getNumIndices(); i < indexOperandCount; ++i)
                {
                    indexOperands.push_back(ToInt32Value(m_pContext, pGetElemInst->getOperand(1 + i), &callInst));
                }

                pBlockOffset = CalcBlockOffset(pBlockTy, indexOperands, 0, &callInst, &stride);
            }
            else
            {
                pBlockOffset = ConstantInt::get(m_pContext->Int32Ty(), 0);
            }

            MDNode* pResMetaNode = pBlock->getMetadata(gSPIRVMD::Resource);
            LLPC_ASSERT(pResMetaNode != nullptr);
            LLPC_ASSERT(pResMetaNode->getNumOperands() == 3);

            auto descSet = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(0))->getZExtValue();
            auto binding = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(1))->getZExtValue();
            LLPC_ASSERT(static_cast<SPIRVBlockTypeKind>(
                mdconst::dyn_extract<ConstantInt>(
                pResMetaNode->getOperand(2))->getZExtValue())
                        == BlockTypeShaderStorage);

            // Ignore array dimensions, block must start with structure type
            while (pBlockTy->isArrayTy())
            {
                pBlockTy = pBlockTy->getArrayElementType();
            }

            MDNode*   pBlockMetaNode = pBlock->getMetadata(gSPIRVMD::Block);
            Constant* pBlockMeta = mdconst::dyn_extract<Constant>(pBlockMetaNode->getOperand(0));

            const uint32_t memberIndex = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
            LLPC_ASSERT(pBlockTy->getStructElementType(memberIndex)->isArrayTy());
            auto pStruct = cast<Constant>(pBlockMeta->getOperand(1));
            Constant* pMemberMeta = pStruct->getAggregateElement(memberIndex);

            // Build arguments and invoke buffer array length operations
            LLPC_ASSERT(pMemberMeta->getNumOperands() == 3);
            ShaderBlockMetadata blockMeta = {};
            blockMeta.U64All = cast<ConstantInt>(pMemberMeta->getOperand(1))->getZExtValue();

            std::unordered_set<Value*> checkedValues;
            bool isNonUniform = IsNonUniformValue(pBlockOffset, checkedValues);

            const uint32_t arrayOffset = blockMeta.offset;
            const uint32_t arrayStride = cast<ConstantInt>(pMemberMeta->getOperand(0))->getZExtValue();

            std::vector<Value*> args;
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), descSet));
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), binding));
            args.push_back(pBlockOffset);
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), arrayOffset));
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), arrayStride));
            args.push_back(ConstantInt::get(m_pContext->BoolTy(), isNonUniform ? true : false));
            Value* pArrayLengthCall = EmitCall(m_pModule,
                                               LlpcName::BufferArrayLength,
                                               callInst.getType(),
                                               args,
                                               NoAttrib,
                                               &callInst);

            callInst.replaceAllUsesWith(pArrayLengthCall);
            m_callInsts.insert(&callInst);

            if (pConstExpr != nullptr)
            {
                pConstExpr->dropAllReferences();
                pConstExpr->deleteValue();
            }
        }
    }
    else if ((mangledName.find("Atomic") != std::string::npos) &&
             ((mangledName.find("Pi") != std::string::npos) ||
              (mangledName.find("Pl") != std::string::npos)))
    {
        // Atomic call
        auto startPos = mangledName.find("Atomic");
        auto endPos = mangledName.find("Pi");
        if (endPos == std::string::npos)
        {
            endPos = mangledName.find("Pl");
            LLPC_ASSERT(endPos != std::string::npos);
        }

        startPos += strlen("Atomic");
        std::string atomicOpName = mangledName.substr(startPos, endPos - startPos);
        std::transform(atomicOpName.begin(), atomicOpName.end(), atomicOpName.begin(), ::tolower);

        // result = AtomicCall(pointer, SPIR-V scope, SPIR-V memory semantics, data0 [, ..., dataN])
        Value* pBufferPtr = callInst.getOperand(0);

        if (pBufferPtr->getType()->getPointerAddressSpace() == SPIRAS_Uniform)
        {
            // Atomic operations on buffer
            m_pContext->GetShaderResourceUsage(m_shaderStage)->resourceWrite = true;

            GetElementPtrInst* pGetElemInst = nullptr;
            Instruction*       pConstExpr   = nullptr;

            if (isa<GetElementPtrInst>(pBufferPtr))
            {
                pGetElemInst = dyn_cast<GetElementPtrInst>(pBufferPtr);
            }
            else if (isa<ConstantExpr>(pBufferPtr))
            {
                pConstExpr = dyn_cast<ConstantExpr>(pBufferPtr)->getAsInstruction();
                pGetElemInst = dyn_cast<GetElementPtrInst>(pConstExpr);
            }

            if (pGetElemInst != nullptr)
            {
                auto pBlock = cast<GlobalVariable>(pGetElemInst->getPointerOperand());
                auto pBlockTy = pBlock->getType()->getPointerElementType();

                std::vector<Value*> indexOperands;
                for (uint32_t i = 0, indexOperandCount = pGetElemInst->getNumIndices(); i < indexOperandCount; ++i)
                {
                    indexOperands.push_back(ToInt32Value(m_pContext, pGetElemInst->getOperand(1 + i), &callInst));
                }

                // Calculate block offset
                uint32_t stride = 0;
                Value* pBlockOffset = CalcBlockOffset(pBlockTy, indexOperands, 0, &callInst, &stride);

                MDNode* pResMetaNode = pBlock->getMetadata(gSPIRVMD::Resource);
                LLPC_ASSERT(pResMetaNode != nullptr);
                LLPC_ASSERT(pResMetaNode->getNumOperands() == 3);

                auto descSet = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(0))->getZExtValue();
                auto binding = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(1))->getZExtValue();
                LLPC_ASSERT(static_cast<SPIRVBlockTypeKind>(
                                mdconst::dyn_extract<ConstantInt>(
                                  pResMetaNode->getOperand(2))->getZExtValue())
                            == BlockTypeShaderStorage);

                // Ignore array dimensions, block must start with structure type
                uint32_t operandIdx = 0;
                while (pBlockTy->isArrayTy())
                {
                    pBlockTy = pBlockTy->getArrayElementType();
                    ++operandIdx;
                }

                // Calculate member offset and get corresponding resulting metadata
                Constant* pResultMeta = nullptr;
                MDNode*   pBlockMetaNode = pBlock->getMetadata(gSPIRVMD::Block);
                Constant* pBlockMeta = mdconst::dyn_extract<Constant>(pBlockMetaNode->getOperand(0));
                Value*    pMemberOffset = CalcBlockMemberOffset(pBlockTy,
                                                                indexOperands,
                                                                operandIdx,
                                                                pBlockMeta,
                                                                &callInst,
                                                                &pResultMeta);

                // Build arguments and invoke buffer atomic operations
                Type* pDataTy = (atomicOpName != "store") ? callInst.getType() : callInst.getOperand(3)->getType();
                std::vector<Value*> data;

                if (atomicOpName == "compareexchange")
                {
                     data.push_back(callInst.getOperand(4));
                     data.push_back(callInst.getOperand(5));
                }
                else if ((atomicOpName != "iincrement") && (atomicOpName != "idecrement") && (atomicOpName != "load"))
                {
                    data.push_back(callInst.getOperand(3));
                }

                Value* pAtomicValue = AddBufferAtomicInst(atomicOpName,
                                                          pDataTy,
                                                          data,
                                                          descSet,
                                                          binding,
                                                          pBlockOffset,
                                                          pMemberOffset,
                                                          pResultMeta,
                                                          &callInst);
                if (atomicOpName != "store")
                {
                    LLPC_ASSERT(pAtomicValue != nullptr);
                    callInst.replaceAllUsesWith(pAtomicValue);
                }
                m_callInsts.insert(&callInst);
            }

            if (pConstExpr != nullptr)
            {
                pConstExpr->dropAllReferences();
                pConstExpr->deleteValue();
            }
        }
    }
    else if (mangledName.find(gSPIRVMD::AccessChain) != std::string::npos)
    {
        uint32_t operandIdx = 0;
        Constant* pResultMeta = nullptr;
        Value* pSrc = callInst.getOperand(0);
        // Collect index arguments from the call
        std::vector<Value*> indexOperands(callInst.getNumOperands() - 3);
        std::copy(callInst.op_begin() + 2, callInst.op_end() - 1, indexOperands.begin());

        auto pLoadTy = callInst.getOperand(1)->getType();

        LLPC_ASSERT(DescriptorSizeBuffer == 4);
        auto pVec4Ty = VectorType::get(m_pContext->Int32Ty(), DescriptorSizeBuffer);

        auto pInst = cast<Instruction>(pSrc);
        MDNode* pBlockMetaNode = pInst->getMetadata(gSPIRVMD::Block);
        Value* pDesc  = ExtractValueInst::Create(pSrc, { 0 }, "", &callInst);
        Value* pOffset = ExtractValueInst::Create(pSrc, { 1 }, "", &callInst);

        Constant* pBlockMeta = mdconst::dyn_extract<Constant>(pBlockMetaNode->getOperand(0));
        auto pStructTy = StructType::get(*m_pContext, { pVec4Ty, m_pContext->Int32Ty() });

        Value* pMemberOffset = CalcBlockMemberOffset(pLoadTy,
                                                     indexOperands,
                                                     operandIdx,
                                                     pBlockMeta,
                                                     &callInst,
                                                     &pResultMeta);

        pOffset = BinaryOperator::CreateAdd(pOffset, pMemberOffset, "", &callInst);
        Value* pStruct = UndefValue::get(pStructTy);
        pStruct = InsertValueInst::Create(pStruct, pDesc, 0, "", &callInst);
        pStruct = InsertValueInst::Create(pStruct, pOffset, 1, "", &callInst);
        pInst = cast<Instruction>(pStruct);
        pInst->setMetadata(gSPIRVMD::Block, callInst.getMetadata(gSPIRVMD::Block));
        callInst.replaceAllUsesWith(pStruct);
        m_callInsts.insert(&callInst);
    }
    else if (mangledName.find(gSPIRVMD::BufferLoad) != std::string::npos)
    {
        Value* pStruct = callInst.getOperand(0);
        auto pBlockMetaNode = callInst.getMetadata(gSPIRVMD::Block);
        Constant* pBlockMeta = mdconst::dyn_extract<Constant>(pBlockMetaNode->getOperand(0));

        Value* pDesc = ExtractValueInst::Create(pStruct, { 0 }, "", &callInst);
        Value* pOffset = ExtractValueInst::Create(pStruct, { 1 }, "", &callInst);

        // Load variable from buffer block
        auto pLoadValue = AddBufferLoadDescInst(callInst.getType(),
                                                pDesc,
                                                pOffset,
                                                pBlockMeta,
                                                &callInst);
        callInst.replaceAllUsesWith(pLoadValue);
        m_callInsts.insert(&callInst);
    }
    else if (mangledName.find(gSPIRVMD::BufferStore) != std::string::npos)
    {
        Value* pStruct = callInst.getOperand(1);
        auto pBlockMetaNode = callInst.getMetadata(gSPIRVMD::Block);
        Constant* pBlockMeta = mdconst::dyn_extract<Constant>(pBlockMetaNode->getOperand(0));

        Value* pDesc = ExtractValueInst::Create(pStruct, { 0 }, "", &callInst);
        Value* pOffset = ExtractValueInst::Create(pStruct, { 1 }, "", &callInst);
        Value* pStoreValue = callInst.getOperand(0);

        // Store variable to buffer block
        AddBufferStoreDescInst(pStoreValue,
                               pDesc,
                               pOffset,
                               pBlockMeta,
                               &callInst);
        m_callInsts.insert(&callInst);
    }
    else if (mangledName.find(gSPIRVMD::StorageBufferCall) != std::string::npos)
    {
        // Translate the emulation getter call of storage buffer variable
        // to the structure information for variable pointer
        Value* pSrc = callInst.getOperand(0);
        LLPC_ASSERT(isa<GlobalVariable>(pSrc));
        auto pBlockVarPtr = dyn_cast<GlobalVariable>(pSrc);
        MDNode* pResMetaNode = pBlockVarPtr->getMetadata(gSPIRVMD::Resource);
        auto pDescSet = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(0));
        auto pBinding = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(1));
        auto pConstZero = ConstantInt::get(m_pContext->Int32Ty(), 0);
        auto pConstFalse = ConstantInt::get(m_pContext->BoolTy(), false);

        Value* args[] = { pDescSet, pBinding, pConstZero, pConstFalse };
        auto pVec4Ty = m_pContext->Int32x4Ty();
        auto pDesc = EmitCall(m_pModule, LlpcName::DescriptorLoadBuffer, pVec4Ty, args, NoAttrib, &callInst);
        auto pStructTy = StructType::get(*m_pContext, { pVec4Ty, m_pContext->Int32Ty() });
        Value* pStruct = UndefValue::get(pStructTy);
        pStruct = InsertValueInst::Create(pStruct, pDesc, 0, "", &callInst);
        pStruct = InsertValueInst::Create(pStruct, pConstZero, 1, "", &callInst);
        Instruction* pInst = dyn_cast<Instruction>(pStruct);
        pInst->setMetadata(gSPIRVMD::Block, pBlockVarPtr->getMetadata(gSPIRVMD::Block));
        callInst.replaceAllUsesWith(pStruct);
        m_callInsts.insert(&callInst);
    }
}

// =====================================================================================================================
// Visits "load" instruction.
void SpirvLowerBufferOp::visitLoadInst(
    LoadInst& loadInst) // [in] "Load" instruction
{
    if (m_restoreMeta)
    {
        return;
    }

    Value* pLoadSrc = loadInst.getOperand(0);

    if ((pLoadSrc->getType()->getPointerAddressSpace() == SPIRAS_Uniform) ||
        (pLoadSrc->getType()->getPointerAddressSpace() == SPIRAS_PushConst))
    {

        // Load from buffer block
        GetElementPtrInst* pGetElemInst = nullptr;
        Instruction*       pConstExpr   = nullptr;

        if (isa<GetElementPtrInst>(pLoadSrc))
        {
            pGetElemInst = dyn_cast<GetElementPtrInst>(pLoadSrc);
        }
        else if (isa<ConstantExpr>(pLoadSrc))
        {
            pConstExpr = dyn_cast<ConstantExpr>(pLoadSrc)->getAsInstruction();
            pGetElemInst = dyn_cast<GetElementPtrInst>(pConstExpr);
        }

        if (pGetElemInst != nullptr)
        {
            auto pBlock = cast<GlobalVariable>(pGetElemInst->getPointerOperand());
            auto pBlockTy = pBlock->getType()->getPointerElementType();

            std::vector<Value*> indexOperands;
            for (uint32_t i = 0, indexOperandCount = pGetElemInst->getNumIndices(); i < indexOperandCount; ++i)
            {
                indexOperands.push_back(ToInt32Value(m_pContext, pGetElemInst->getOperand(1 + i), &loadInst));
            }

            uint32_t descSet = InvalidValue;
            uint32_t binding = InvalidValue;

            uint32_t operandIdx = 0;

            Value* pBlockOffset = nullptr;

            bool isPushConst = (pLoadSrc->getType()->getPointerAddressSpace() == SPIRAS_PushConst);

            if (isPushConst == false)
            {
                // Calculate block offset, push constant is ignored
                uint32_t stride = 0;
                pBlockOffset = CalcBlockOffset(pBlockTy, indexOperands, 0, &loadInst, &stride);

                MDNode* pResMetaNode = pBlock->getMetadata(gSPIRVMD::Resource);
                LLPC_ASSERT(pResMetaNode != nullptr);
                LLPC_ASSERT(pResMetaNode->getNumOperands() == 3);

                descSet = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(0))->getZExtValue();
                binding = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(1))->getZExtValue();
                SPIRVBlockTypeKind blockKind = static_cast<SPIRVBlockTypeKind>(
                                                   mdconst::dyn_extract<ConstantInt>(
                                                       pResMetaNode->getOperand(2))->getZExtValue());
                LLPC_ASSERT((blockKind == BlockTypeUniform) || (blockKind == BlockTypeShaderStorage));
                LLPC_UNUSED(blockKind);

                // Ignore array dimensions, block must start with structure type
                while (pBlockTy->isArrayTy())
                {
                    pBlockTy = pBlockTy->getArrayElementType();
                    ++operandIdx;
                }
            }

            // Calculate member offset and get corresponding resulting metadata
            Constant* pResultMeta    = nullptr;
            MDNode*   pBlockMetaNode = pBlock->getMetadata(gSPIRVMD::Block);
            Constant* pBlockMeta     = mdconst::dyn_extract<Constant>(pBlockMetaNode->getOperand(0));
            Value*    pMemberOffset  = CalcBlockMemberOffset(pBlockTy,
                                                             indexOperands,
                                                             operandIdx,
                                                             pBlockMeta,
                                                             &loadInst,
                                                             &pResultMeta);

            const bool isScalarAligned = NeedScalarAlignment(
                loadInst.getType(), pBlockTy, indexOperands, operandIdx, pBlockMeta);

            // Load variable from buffer block
            Value* pLoadDest = AddBufferLoadInst(loadInst.getType(),
                                                 descSet,
                                                 binding,
                                                 isPushConst,
                                                 isScalarAligned,
                                                 pBlockOffset,
                                                 pMemberOffset,
                                                 pResultMeta,
                                                 &loadInst);

            m_loadInsts.insert(&loadInst);
            loadInst.replaceAllUsesWith(pLoadDest);
        }
        else
        {
            // Load variable from entire buffer block
            LLPC_ASSERT(isa<GlobalVariable>(pLoadSrc));

            auto pBlock = cast<GlobalVariable>(pLoadSrc);
            auto pBlockTy = pBlock->getType()->getPointerElementType();

            std::vector<Value*> indexOperands;
            indexOperands.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));

            Value* pLoadDest = LoadEntireBlock(pBlock, pBlockTy, indexOperands, &loadInst);
            m_loadInsts.insert(&loadInst);
            loadInst.replaceAllUsesWith(pLoadDest);
        }

        if (pConstExpr != nullptr)
        {
            pConstExpr->dropAllReferences();
            pConstExpr->deleteValue();
        }
    }
}

// =====================================================================================================================
// Visits "store" instruction.
void SpirvLowerBufferOp::visitStoreInst(
    StoreInst& storeInst) // [in] "Store" instruction
{
    if (m_restoreMeta)
    {
        return;
    }

    Value* pStoreSrc  = storeInst.getOperand(0);
    Value* pStoreDest = storeInst.getOperand(1);

    if (pStoreDest->getType()->getPointerAddressSpace() == SPIRAS_Uniform)
    {
        // Store to buffer block
        m_pContext->GetShaderResourceUsage(m_shaderStage)->resourceWrite = true;

        GetElementPtrInst* pGetElemInst = nullptr;
        Instruction*       pConstExpr   = nullptr;

        if (isa<GetElementPtrInst>(pStoreDest))
        {
            pGetElemInst = dyn_cast<GetElementPtrInst>(pStoreDest);
        }
        else if (isa<ConstantExpr>(pStoreDest))
        {
            pConstExpr = dyn_cast<ConstantExpr>(pStoreDest)->getAsInstruction();
            pGetElemInst = dyn_cast<GetElementPtrInst>(pConstExpr);
        }

        if (pGetElemInst != nullptr)
        {
            auto pBlock = cast<GlobalVariable>(pGetElemInst->getPointerOperand());
            auto pBlockTy = pBlock->getType()->getPointerElementType();

            std::vector<Value*> indexOperands;
            for (uint32_t i = 0, indexOperandCount = pGetElemInst->getNumIndices(); i < indexOperandCount; ++i)
            {
                indexOperands.push_back(ToInt32Value(m_pContext, pGetElemInst->getOperand(1 + i), &storeInst));
            }

            // Calculate block offset
            uint32_t stride  = 0;
            Value* pBlockOffset = CalcBlockOffset(pBlockTy, indexOperands, 0, &storeInst, &stride);

            MDNode* pResMetaNode = pBlock->getMetadata(gSPIRVMD::Resource);
            LLPC_ASSERT(pResMetaNode != nullptr);
            LLPC_ASSERT(pResMetaNode->getNumOperands() == 3);

            auto descSet   = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(0))->getZExtValue();
            auto binding   = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(1))->getZExtValue();
            LLPC_ASSERT(static_cast<SPIRVBlockTypeKind>(
                                               mdconst::dyn_extract<ConstantInt>(
                                                   pResMetaNode->getOperand(2))->getZExtValue())
                        == BlockTypeShaderStorage); // Must be shader storage block

            // Ignore array dimensions, block must start with structure type
            uint32_t operandIdx = 0;
            while (pBlockTy->isArrayTy())
            {
                pBlockTy = pBlockTy->getArrayElementType();
                ++operandIdx;
            }

            // Calculate member offset and get corresponding resulting metadata
            Constant* pResultMeta    = nullptr;
            MDNode*   pBlockMetaNode = pBlock->getMetadata(gSPIRVMD::Block);
            Constant* pBlockMeta     = mdconst::dyn_extract<Constant>(pBlockMetaNode->getOperand(0));
            Value*    pMemberOffset  = CalcBlockMemberOffset(pBlockTy,
                                                             indexOperands,
                                                             operandIdx,
                                                             pBlockMeta,
                                                             &storeInst,
                                                             &pResultMeta);

            const bool isScalarAligned = NeedScalarAlignment(
                pStoreSrc->getType(), pBlockTy, indexOperands, operandIdx, pBlockMeta);

            // Store variable to buffer block
            AddBufferStoreInst(pStoreSrc,
                               descSet,
                               binding,
                               isScalarAligned,
                               pBlockOffset,
                               pMemberOffset,
                               pResultMeta,
                               &storeInst);
            m_storeInsts.insert(&storeInst);
        }
        else
        {
            // Store variable to entire buffer block
            LLPC_ASSERT(isa<GlobalVariable>(pStoreDest));

            auto pBlock = cast<GlobalVariable>(pStoreDest);

            std::vector<Value*> indexOperands;
            indexOperands.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));

            StoreEntireBlock(pBlock, pStoreSrc, indexOperands, &storeInst);
            m_storeInsts.insert(&storeInst);
        }

        if (pConstExpr != nullptr)
        {
            pConstExpr->dropAllReferences();
            pConstExpr->deleteValue();
        }
    }
}

// =====================================================================================================================
// Inserts instructions to calculate element offset for arrayed block.
Value* SpirvLowerBufferOp::CalcBlockOffset(
    const Type*                pBlockTy,      // [in] Type of this block
    const std::vector<Value*>& indexOperands, // [in] Index operands
    uint32_t                   operandIdx,    // Index of the index operand in processing
    Instruction*               pInsertPos,    // [in] Where to insert calculation instructions
    uint32_t*                  pStride)       // [in,out] Stride for arrayed resources
{
    if (pBlockTy->isArrayTy())
    {
        // Accumulate resource offset for arrayed block
        auto pSubelemOffset = CalcBlockOffset(pBlockTy->getArrayElementType(),
                                              indexOperands,
                                              operandIdx + 1,
                                              pInsertPos,
                                              pStride);

        *pStride *= pBlockTy->getArrayNumElements();
        auto pElemOffset = BinaryOperator::CreateMul(indexOperands[operandIdx],
                                                     ConstantInt::get(m_pContext->Int32Ty(), *pStride),
                                                     "",
                                                     pInsertPos);

        return BinaryOperator::CreateAdd(pElemOffset, pSubelemOffset, "", pInsertPos);
    }
    else
    {
        *pStride = 1;
        return indexOperands[operandIdx];
    }
}

// =====================================================================================================================
// Determines if a value needs a scalar aligned load or not.
bool SpirvLowerBufferOp::NeedScalarAlignment(
    const Type* const      pValueTy,        // [in] The data type we are going to load/store
    const Type*            pBlockTy,        // [in] Type of this block
    const ArrayRef<Value*> indexOperands,   // The index operands
    const uint32_t         startOperandIdx, // The initial index into indexOperands
    const Constant* const  pBlockMeta)      // [in] The block metadata
{
    // If the elements of our load/store type are 4 bytes (32-bits) or less, we never need scalar alignment.
    if (pValueTy->getScalarSizeInBits() >= 32)
    {
        return false;
    }

    // If our load/store is not a vector type, we do not need scalar alignment.
    if (pValueTy->isVectorTy() == false)
    {
        return false;
    }

    // Our required alignment is 2 for < 4 bytes, otherwise 4 (dword aligned).
    const uint32_t requiredAlignmentForNonScalarLoads = (pValueTy->getPrimitiveSizeInBits() < 4) ? 2 : 4;

    const Constant* pValueMeta = pBlockMeta;

    for (uint32_t operandIdx = startOperandIdx; operandIdx < indexOperands.size(); operandIdx++)
    {
        ShaderBlockMetadata blockMeta = {};
        if (pBlockTy->isStructTy())
        {
            LLPC_ASSERT(pValueMeta->getNumOperands() == 2);

            blockMeta.U64All = cast<ConstantInt>(pValueMeta->getOperand(0))->getZExtValue();

            // If the offset of the struct does not meet the required alignment, we need a scalar aligned load!
            if ((blockMeta.offset % requiredAlignmentForNonScalarLoads) != 0)
            {
                return true;
            }

            const uint32_t memberIdx = cast<ConstantInt>(indexOperands[operandIdx + 1])->getZExtValue();

            pValueMeta = cast<Constant>(pValueMeta->getOperand(1))->getAggregateElement(memberIdx);
            pBlockTy = pBlockTy->getStructElementType(memberIdx);
        }
        else if (pBlockTy->isArrayTy())
        {
            LLPC_ASSERT(pValueMeta->getNumOperands() == 3);

            const uint32_t stride = cast<ConstantInt>(pValueMeta->getOperand(0))->getZExtValue();

            // If the stride of the array does not meet the required alignment, we need a scalar aligned load!
            if ((stride % requiredAlignmentForNonScalarLoads) != 0)
            {
                return true;
            }

            blockMeta.U64All = cast<ConstantInt>(pValueMeta->getOperand(1))->getZExtValue();

            // If the offset of the array does not meet the required alignment, we need a scalar aligned load!
            if ((blockMeta.offset % requiredAlignmentForNonScalarLoads) != 0)
            {
                return true;
            }

            pValueMeta = cast<Constant>(pValueMeta->getOperand(2));
            pBlockTy = pBlockTy->getArrayElementType();
        }
        else if (pBlockTy->isPointerTy())
        {
            LLPC_ASSERT(pValueMeta->getNumOperands() == 3);

            const uint32_t stride = cast<ConstantInt>(pValueMeta->getOperand(0))->getZExtValue();

            // If the stride of the pointer does not meet the required alignment, we need a scalar aligned load!
            if ((stride % requiredAlignmentForNonScalarLoads) != 0)
            {
                return true;
            }

            blockMeta.U64All = cast<ConstantInt>(pValueMeta->getOperand(1))->getZExtValue();

            // If the offset of the pointer does not meet the required alignment, we need a scalar aligned load!
            if ((blockMeta.offset % requiredAlignmentForNonScalarLoads) != 0)
            {
                return true;
            }

            pValueMeta = cast<Constant>(pValueMeta->getOperand(2));
            pBlockTy = pBlockTy->getPointerElementType();
        }
        else
        {
            LLPC_ASSERT(pValueMeta->getNumOperands() == 0);

            blockMeta.U64All = cast<ConstantInt>(pValueMeta)->getZExtValue();

            // If the offset of the pointer does not meet the required alignment, we need a scalar aligned load!
            if ((blockMeta.offset % requiredAlignmentForNonScalarLoads) != 0)
            {
                return true;
            }

            break;
        }
    }

    // We do not need a scalar aligned load!
    return false;
}

// =====================================================================================================================
// Inserts instructions to calculate within-block offset for block members.
Value* SpirvLowerBufferOp::CalcBlockMemberOffset(
    const Type*                pBlockMemberTy, // [in] Type of this block member
    const std::vector<Value*>& indexOperands,  // [in] Index operands
    uint32_t                   operandIdx,     // Index of the index operand in processing
    Constant*                  pMeta,          // [in] Metadata of this block member
    Instruction*               pInsertPos,     // [in] Where to insert calculation instructions
    Constant**                 ppResultMeta)   // [out] Resulting block metadata after calculations
{
    ShaderBlockMetadata blockMeta = {};
    Value* pOffset = nullptr;
    if (pBlockMemberTy->isStructTy())
    {
        // Block member is structure-typed
        blockMeta.U64All = cast<ConstantInt>(pMeta->getOperand(0))->getZExtValue();
        pOffset = ConstantInt::get(m_pContext->Int32Ty(), blockMeta.offset);

        if (operandIdx + 1 < indexOperands.size())
        {
            auto pStruct = cast<Constant>(pMeta->getOperand(1));
            uint32_t memberIdx = cast<ConstantInt>(indexOperands[operandIdx + 1])->getZExtValue();
            auto pMemberMeta = cast<Constant>(pStruct->getAggregateElement(memberIdx)); // Metadata is structure-typed

            Value* pMemberOffset = CalcBlockMemberOffset(pBlockMemberTy->getStructElementType(memberIdx),
                                                         indexOperands,
                                                         operandIdx + 1,
                                                         pMemberMeta,
                                                         pInsertPos,
                                                         ppResultMeta);
            return BinaryOperator::CreateAdd(pOffset, pMemberOffset, "", pInsertPos);
        }
        else
        {
            *ppResultMeta = pMeta;
            return pOffset;
        }
    }
    else if (pBlockMemberTy->isArrayTy())
    {
        // Block member is array-typed
        LLPC_ASSERT(pMeta->getNumOperands() == 3);
        blockMeta.U64All = cast<ConstantInt>(pMeta->getOperand(1))->getZExtValue();
        auto pElemMeta = cast<Constant>(pMeta->getOperand(2));
        pOffset = ConstantInt::get(m_pContext->Int32Ty(), blockMeta.offset);
        // This offset is for the remaining
        if (operandIdx + 1 < indexOperands.size())
        {
            auto pSubelemOffset = CalcBlockMemberOffset(pBlockMemberTy->getArrayElementType(),
                                                        indexOperands,
                                                        operandIdx + 1,
                                                        pElemMeta,
                                                        pInsertPos,
                                                        ppResultMeta);

            uint32_t stride = cast<ConstantInt>(pMeta->getOperand(0))->getZExtValue();
            if (blockMeta.IsRowMajor && blockMeta.IsMatrix)
            {
                auto pCompTy = pBlockMemberTy->getArrayElementType()->getVectorElementType();
                stride = pCompTy->getScalarSizeInBits() / 8;
            }

            auto pElemOffset = BinaryOperator::CreateMul(ConstantInt::get(m_pContext->Int32Ty(), stride),
                                                         indexOperands[operandIdx + 1] ,
                                                         "",
                                                         pInsertPos);

           pElemOffset = BinaryOperator::CreateAdd(pElemOffset, pSubelemOffset, "", pInsertPos);
           return BinaryOperator::CreateAdd(pElemOffset, pOffset, "", pInsertPos);

        }
        else
        {
            *ppResultMeta = pMeta;
            return pOffset;
        }
    }
    else if (pBlockMemberTy->isVectorTy())
    {
        // Block member is vector-typed
        *ppResultMeta = pMeta;
        blockMeta.U64All = cast<ConstantInt>(pMeta)->getZExtValue();
        auto pVecOffset = ConstantInt::get(m_pContext->Int32Ty(), blockMeta.offset);

        if (operandIdx + 1 < indexOperands.size())
        {
            const uint32_t stride = blockMeta.IsRowMajor ?
                blockMeta.MatrixStride :
                pBlockMemberTy->getScalarSizeInBits() / 8;

            auto pCompOffset = BinaryOperator::CreateMul(ConstantInt::get(m_pContext->Int32Ty(), stride),
                                                         indexOperands[operandIdx + 1],
                                                         "",
                                                         pInsertPos);
            return BinaryOperator::CreateAdd(pVecOffset, pCompOffset, "", pInsertPos);
        }
        else
        {
            return pVecOffset;
        }
    }
    else if (pBlockMemberTy->isPointerTy())
    {
        // Stride of pointer is used to calculate the position of element index of PtrAccessChain
        uint32_t stride = cast<ConstantInt>(pMeta->getOperand(0))->getZExtValue();
        blockMeta.U64All = cast<ConstantInt>(pMeta->getOperand(1))->getZExtValue();
        Value* pOffset = ConstantInt::get(m_pContext->Int32Ty(), blockMeta.offset);

        if ((operandIdx + 1) < indexOperands.size())
        {
            auto pElemTy = pBlockMemberTy->getPointerElementType();
            auto pElemMeta = cast<Constant>(pMeta->getOperand(2));
            pOffset = BinaryOperator::CreateMul(ConstantInt::get(m_pContext->Int32Ty(), stride),
                                                indexOperands[operandIdx + 1],
                                                "",
                                                pInsertPos);
            auto pElemOffset = CalcBlockMemberOffset(pElemTy,
                                                     indexOperands,
                                                     operandIdx + 1,
                                                     pElemMeta,
                                                     pInsertPos,
                                                     ppResultMeta);
            pOffset = BinaryOperator::CreateAdd(pOffset, pElemOffset, "", pInsertPos);
            return pOffset;
        }
        else
        {
            *ppResultMeta = pMeta;
            return pOffset;
        }
    }
    // Last index operand
    else if (pBlockMemberTy->isSingleValueType())
    {
        LLPC_ASSERT(operandIdx == indexOperands.size() - 1);
        *ppResultMeta = pMeta;
        // Last access type is vector or scalar, directly return the offset
        ShaderBlockMetadata blockMeta = {};
        blockMeta.U64All = cast<ConstantInt>(pMeta)->getZExtValue();
        return ConstantInt::get(m_pContext->Int32Ty(), blockMeta.offset);
    }
    else
    {
        *ppResultMeta = pMeta;
        // NOTE: If last access type is aggregate type, return 0 as don't-care value. The offset is stored in
        // resulting metadata and will be obtained from that.
        return ConstantInt::get(m_pContext->Int32Ty(), 0);
    }
}

// =====================================================================================================================
// Inserts instructions to load variable from storage buffer block.
Value* SpirvLowerBufferOp::AddBufferLoadInst(
    Type*        pLoadTy,            // [in] Type of value loaded from buffer block
    uint32_t     descSet,            // Descriptor set of buffer block
    uint32_t     binding,            // Descriptor binding of buffer block
    bool         isPushConst,        // Whether the block is actually a push constant
    bool         isScalarAligned,    // Whether scalar alignment has to be used for the load.
    Value*       pBlockOffset,       // [in] Block offset
    Value*       pBlockMemberOffset, // [in] Block member offset
    Constant*    pBlockMemberMeta,   // [in] Block member metadata
    Instruction* pInsertPos)         // [in] Where to insert instructions
{
    Value* pLoadValue = UndefValue::get(pLoadTy);
    std::unordered_set<Value*> checkedValues;
    bool isNonUniform = isPushConst ? false : IsNonUniformValue(pBlockOffset, checkedValues);

    if (pLoadTy->isSingleValueType())
    {
        // Load scalar or vector type
        ShaderBlockMetadata blockMeta = {};
        blockMeta.U64All = cast<ConstantInt>(pBlockMemberMeta)->getZExtValue();

        if (blockMeta.IsRowMajor && pLoadTy->isVectorTy())
        {
            // NOTE: For row-major matrix, loading a column vector is done by loading its own components separately.
            auto pCompTy = pLoadTy->getVectorElementType();
            const uint32_t compCount = pLoadTy->getVectorNumElements();

            // Cast type of the component type to <n x i8>
            uint32_t loadSize = pCompTy->getPrimitiveSizeInBits() / 8;
            Type* pCastTy = VectorType::get(m_pContext->Int8Ty(), loadSize);
            std::string suffix = GetTypeNameForScalarOrVector(pCastTy);

            for (uint32_t i = 0; i < compCount; ++i)
            {
                const char* pInstName = nullptr;

                // Build arguments for buffer load
                std::vector<Value*> args;

                if (isPushConst)
                {
                    pInstName = LlpcName::PushConstLoad;
                }
                else
                {
                    pInstName = LlpcName::BufferLoad;
                    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), descSet));
                    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), binding));
                    args.push_back(pBlockOffset);
                }
                args.push_back(pBlockMemberOffset);

                if (isPushConst == false)
                {
                    args.push_back(ConstantInt::get(m_pContext->BoolTy(),
                                                    blockMeta.NonWritable ? true : false)); // readonly
                }
                args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Coherent ? true : false)); // glc
                args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Volatile ? true : false)); // slc
                args.push_back(ConstantInt::get(m_pContext->BoolTy(), isNonUniform ? true : false)); // nonUniform

                auto pCompValue = EmitCall(m_pModule, pInstName + suffix, pCastTy, args, NoAttrib, pInsertPos);

                LLPC_ASSERT(CanBitCast(pCastTy, pCompTy));
                pCompValue = new BitCastInst(pCompValue, pCompTy, "", pInsertPos);

                pLoadValue = InsertElementInst::Create(pLoadValue,
                                                       pCompValue,
                                                       ConstantInt::get(m_pContext->Int32Ty(), i),
                                                       "",
                                                       pInsertPos);

                // Update the block member offset for the next component
                pBlockMemberOffset = BinaryOperator::CreateAdd(pBlockMemberOffset,
                                                               ConstantInt::get(m_pContext->Int32Ty(),
                                                                                blockMeta.MatrixStride),
                                                               "",
                                                               pInsertPos);
            }
        }
        else
        {
            // Cast type of the load type to <n x i8>
            const uint32_t loadSize = pLoadTy->getPrimitiveSizeInBits() / 8;

            Type* pActualLoadTy = nullptr;

            // If we don't have a push constant, and need a scalar aligned load.
            if ((isPushConst == false) && isScalarAligned)
            {
                if (pLoadTy->getVectorElementType()->isHalfTy())
                {
                    pActualLoadTy = VectorType::get(m_pContext->Int16Ty(), pLoadTy->getVectorNumElements());
                }
                else
                {
                    LLPC_ASSERT(pLoadTy->isIntOrIntVectorTy(8) || pLoadTy->isIntOrIntVectorTy(16));
                    pActualLoadTy = pLoadTy;
                }
            }
            else if (loadSize == 1)
            {
                pActualLoadTy = pLoadTy;
            }
            else
            {
                pActualLoadTy = VectorType::get(m_pContext->Int8Ty(), loadSize);
            }

            const char* pInstName = nullptr;

            // Build arguments for buffer load
            std::vector<Value*> args;

            if (isPushConst)
            {
                pInstName = LlpcName::PushConstLoad;
            }
            else
            {
                pInstName = isScalarAligned ? LlpcName::BufferLoadScalarAligned : LlpcName::BufferLoad;
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), descSet));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), binding));
                args.push_back(pBlockOffset);
            }
            args.push_back(pBlockMemberOffset);

            if (isPushConst == false)
            {
                args.push_back(ConstantInt::get(m_pContext->BoolTy(),
                                                blockMeta.NonWritable ? true : false)); // readonly
            }
            args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Coherent ? true : false)); // glc
            args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Volatile ? true : false)); // slc
            args.push_back(ConstantInt::get(m_pContext->BoolTy(), isNonUniform ? true : false)); // nonUniform

            std::string suffix = GetTypeNameForScalarOrVector(pActualLoadTy);

            pLoadValue = EmitCall(m_pModule, pInstName + suffix, pActualLoadTy, args, NoAttrib, pInsertPos);

            if (pActualLoadTy != pLoadTy)
            {
                LLPC_ASSERT(CanBitCast(pActualLoadTy, pLoadTy));
                pLoadValue = new BitCastInst(pLoadValue, pLoadTy, "", pInsertPos);
            }
        }
    }
    else if (pLoadTy->isArrayTy())
    {
        // Load array and matrix
        LLPC_ASSERT(pBlockMemberMeta->getNumOperands() == 3);
        auto pStride = cast<ConstantInt>(pBlockMemberMeta->getOperand(0));
        ShaderBlockMetadata arrayMeta = {};
        arrayMeta.U64All = cast<ConstantInt>(pBlockMemberMeta->getOperand(1))->getZExtValue();
        auto pElemMeta = cast<Constant>(pBlockMemberMeta->getOperand(2));

        const bool isRowMajorMatrix = (arrayMeta.IsMatrix && arrayMeta.IsRowMajor);

        auto pElemTy = pLoadTy->getArrayElementType();
        uint32_t elemCount = pLoadTy->getArrayNumElements();

        if (isRowMajorMatrix)
        {
            // NOTE: For row-major matrix, we process it with its transposed form.
            const auto pColVecTy = pElemTy;
            LLPC_ASSERT(pColVecTy->isVectorTy());
            const auto colCount = elemCount;
            const auto rowCount = pColVecTy->getVectorNumElements();

            auto pCompTy = pColVecTy->getVectorElementType();

            auto pRowVecTy = VectorType::get(pCompTy, colCount);
            auto pTransposeTy = ArrayType::get(pRowVecTy, rowCount);

            // NOTE: Here, we have to revise the initial value of load value, element type, and element count.
            pLoadValue = UndefValue::get(pTransposeTy);
            pElemTy = pRowVecTy;
            elemCount = rowCount;

            // NOTE: Here, we have to clear "row-major" flag in metadata since the matrix is processed as
            // "column-major" style.
            ShaderBlockMetadata elemMeta = {};
            elemMeta.U64All = cast<ConstantInt>(pElemMeta)->getZExtValue();
            elemMeta.IsRowMajor = false;
            pElemMeta = ConstantInt::get(m_pContext->Int64Ty(), elemMeta.U64All);
        }

        for (uint32_t elemIdx = 0; elemIdx < elemCount; ++elemIdx)
        {
            auto pElemIdx = ConstantInt::get(m_pContext->Int32Ty(), elemIdx);

            // Calculate array element offset
            auto pElemOffset = BinaryOperator::CreateMul(pStride, pElemIdx, "", pInsertPos);
            pElemOffset = BinaryOperator::CreateAdd(pBlockMemberOffset, pElemOffset, "", pInsertPos);
            if (pElemTy->isSingleValueType())
            {
                ShaderBlockMetadata elemMeta = {};
                elemMeta.U64All = cast<ConstantInt>(pElemMeta)->getZExtValue();
                pElemOffset = BinaryOperator::CreateAdd(pElemOffset,
                                                        ConstantInt::get(m_pContext->Int32Ty(), elemMeta.offset),
                                                        "",
                                                        pInsertPos);
            }

            // Load array element
            auto pElem = AddBufferLoadInst(pElemTy,
                                           descSet,
                                           binding,
                                           isPushConst,
                                           isScalarAligned,
                                           pBlockOffset,
                                           pElemOffset,
                                           pElemMeta,
                                           pInsertPos);

            // Insert array element to load value
            std::vector<uint32_t> idxs;
            idxs.push_back(elemIdx);
            pLoadValue = InsertValueInst::Create(pLoadValue, pElem,  idxs, "", pInsertPos);
        }

        if (isRowMajorMatrix)
        {
            // NOTE: Here, we have to revise the load value (do transposing).
            pLoadValue = TransposeMatrix(pLoadValue, pInsertPos);
        }
    }
    else
    {
        // Load sructure type

        // NOTE: Calculated block member offset is 0 when the member type is aggregate. So the specified
        // "pBlockMemberOffset" does not include the offset of the structure. We have to add it here.
        LLPC_ASSERT(pLoadTy->isStructTy());

        const uint32_t memberCount = pLoadTy->getStructNumElements();
        for (uint32_t memberIdx = 0; memberIdx < memberCount; ++memberIdx)
        {
            auto pMemberTy = pLoadTy->getStructElementType(memberIdx);
            auto pStruct = cast<Constant>(pBlockMemberMeta->getOperand(1));
            auto pMemberMeta = cast<Constant>(pStruct->getAggregateElement(memberIdx));

            Value* pMemberOffset = nullptr;
            ShaderBlockMetadata blockMeta = {};
            if (pMemberTy->isSingleValueType())
            {
                blockMeta.U64All = cast<ConstantInt>(pMemberMeta)->getZExtValue();
            }
            else if (pMemberTy->isArrayTy())
            {
                blockMeta.U64All = cast<ConstantInt>(pMemberMeta->getOperand(1))->getZExtValue();
            }
            else
            {
                LLPC_ASSERT(pMemberTy->isStructTy() == true);
                blockMeta.U64All = cast<ConstantInt>(pMemberMeta->getOperand(0))->getZExtValue();
            }

            pMemberOffset = BinaryOperator::CreateAdd(pBlockMemberOffset,
                                                      ConstantInt::get(m_pContext->Int32Ty(), blockMeta.offset),
                                                      "",
                                                      pInsertPos);

            // Load structure member
            auto pMember = AddBufferLoadInst(pMemberTy,
                                             descSet,
                                             binding,
                                             isPushConst,
                                             isScalarAligned,
                                             pBlockOffset,
                                             pMemberOffset,
                                             pMemberMeta,
                                             pInsertPos);

            // Insert structure member to load value
            std::vector<uint32_t> idxs;
            idxs.push_back(memberIdx);
            pLoadValue = InsertValueInst::Create(pLoadValue, pMember,  idxs, "", pInsertPos);
        }
    }

    return pLoadValue;
}

// =====================================================================================================================
// Inserts instructions to load variable from buffer block (with descriptor).
Value* SpirvLowerBufferOp::AddBufferLoadDescInst(
    Type*        pLoadTy,            // [in] Type of value loaded from buffer block
    Value*       pDesc,              // [in] Descriptor of buffer block
    Value*       pBlockMemberOffset, // [in] Block member offset
    Constant*    pBlockMemberMeta,   // [in] Element meta Data
    Instruction* pInsertPos)         // [in] Where to insert instructions
{
    Value* pLoadValue = UndefValue::get(pLoadTy);;

    if (pLoadTy->isSingleValueType())
    {
        // Load scalar or vector type
        ShaderBlockMetadata blockMeta = {};
        if (blockMeta.IsRowMajor && pLoadTy->isVectorTy())
        {
            // NOTE: For row-major matrix, loading a column vector is done by loading its own components separately.
            auto pCompTy = pLoadTy->getVectorElementType();
            const uint32_t compCount = pLoadTy->getVectorNumElements();

            // Cast type of the component type to <n x i8>
            uint32_t loadSize = pCompTy->getPrimitiveSizeInBits() / 8;
            Type* pCastTy = VectorType::get(m_pContext->Int8Ty(), loadSize);
            std::string suffix = GetTypeNameForScalarOrVector(pCastTy);

            for (uint32_t i = 0; i < compCount; ++i)
            {
                const char* pInstName = nullptr;

                // Build arguments for buffer load
                std::vector<Value*> args;
                args.push_back(pDesc);
                args.push_back(pBlockMemberOffset);
                pInstName = LlpcName::BufferLoadDesc;

                args.push_back(ConstantInt::get(m_pContext->BoolTy(),
                    blockMeta.NonWritable ? true : false)); // readonly

                args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Coherent ? true : false)); // glc
                args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Volatile ? true : false)); // slc

                auto pCompValue = EmitCall(m_pModule, pInstName + suffix, pCastTy, args, NoAttrib, pInsertPos);

                LLPC_ASSERT(CanBitCast(pCastTy, pCompTy));
                pCompValue = new BitCastInst(pCompValue, pCompTy, "", pInsertPos);

                pLoadValue = InsertElementInst::Create(pLoadValue,
                                                       pCompValue,
                                                       ConstantInt::get(m_pContext->Int32Ty(), i),
                                                       "",
                                                       pInsertPos);

                // Update the block member offset for the next component
                pBlockMemberOffset = BinaryOperator::CreateAdd(pBlockMemberOffset,
                                                               ConstantInt::get(m_pContext->Int32Ty(),
                                                               blockMeta.MatrixStride),
                                                               "",
                                                               pInsertPos);
            }
        }
        else
        {
            const uint32_t loadSize = pLoadTy->getPrimitiveSizeInBits() / 8;

            // If scalar block layout is enabled, we need to treat vector types with 1 / 2 byte components differently.
            const bool isScalarBlockLayout = false;
            const bool isSmallVector = pLoadTy->isVectorTy() && (pLoadTy->getScalarSizeInBits() < 32);
            const bool needScalarAlignedLoad = isScalarBlockLayout && isSmallVector;

            Type* pActualLoadTy = nullptr;

            // If we need a scalar aligned load.
            if (needScalarAlignedLoad)
            {
                if (pLoadTy->getVectorElementType()->isHalfTy())
                {
                    pActualLoadTy = VectorType::get(m_pContext->Int16Ty(), pLoadTy->getVectorNumElements());
                }
                else
                {
                    LLPC_ASSERT(pLoadTy->isIntOrIntVectorTy(8) || pLoadTy->isIntOrIntVectorTy(16));
                    pActualLoadTy = pLoadTy;
                }
            }
            else if (loadSize == 1)
            {
                pActualLoadTy = pLoadTy;
            }
            else
            {
                // Cast type of the load type to <n x i8>
                pActualLoadTy = VectorType::get(m_pContext->Int8Ty(), loadSize);
            }

            const char* pInstName = needScalarAlignedLoad ?
                LlpcName::BufferLoadScalarAlignedDesc : LlpcName::BufferLoadDesc;

            // Build arguments for buffer load
            std::vector<Value*> args;
            args.push_back(pDesc);
            args.push_back(pBlockMemberOffset);
            args.push_back(ConstantInt::get(m_pContext->BoolTy(),
                blockMeta.NonWritable ? true : false)); // readonly

            args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Coherent ? true : false)); // glc
            args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Volatile ? true : false)); // slc

            std::string suffix = GetTypeNameForScalarOrVector(pActualLoadTy);

            pLoadValue = EmitCall(m_pModule, pInstName + suffix, pActualLoadTy, args, NoAttrib, pInsertPos);

            if (pActualLoadTy != pLoadTy)
            {
                LLPC_ASSERT(CanBitCast(pActualLoadTy, pLoadTy));
                pLoadValue = new BitCastInst(pLoadValue, pLoadTy, "", pInsertPos);
            }
        }
    }
    else if (pLoadTy->isArrayTy())
    {
        // Load array and matrix
        LLPC_ASSERT(pBlockMemberMeta->getNumOperands() == 3);
        auto pStride = cast<ConstantInt>(pBlockMemberMeta->getOperand(0));
        ShaderBlockMetadata arrayMeta = {};
        arrayMeta.U64All = cast<ConstantInt>(pBlockMemberMeta->getOperand(1))->getZExtValue();
        auto pElemMeta = cast<Constant>(pBlockMemberMeta->getOperand(2));

        const bool isRowMajorMatrix = (arrayMeta.IsMatrix && arrayMeta.IsRowMajor);

        auto pElemTy = pLoadTy->getArrayElementType();
        uint32_t elemCount = pLoadTy->getArrayNumElements();

        if (isRowMajorMatrix)
        {
            // NOTE: For row-major matrix, we process it with its transposed form.
            const auto pColVecTy = pElemTy;
            LLPC_ASSERT(pColVecTy->isVectorTy());
            const auto colCount = elemCount;
            const auto rowCount = pColVecTy->getVectorNumElements();

            auto pCompTy = pColVecTy->getVectorElementType();

            auto pRowVecTy = VectorType::get(pCompTy, colCount);
            auto pTransposeTy = ArrayType::get(pRowVecTy, rowCount);

            // NOTE: Here, we have to revise the initial value of load value, element type, and element count.
            pLoadValue = UndefValue::get(pTransposeTy);
            pElemTy = pRowVecTy;
            elemCount = rowCount;

            // NOTE: Here, we have to clear "row-major" flag in metadata since the matrix is processed as
            // "column-major" style.
            ShaderBlockMetadata elemMeta = {};
            elemMeta.U64All = cast<ConstantInt>(pElemMeta)->getZExtValue();
            elemMeta.IsRowMajor = false;
            pElemMeta = ConstantInt::get(m_pContext->Int64Ty(), elemMeta.U64All);
        }

        for (uint32_t elemIdx = 0; elemIdx < elemCount; ++elemIdx)
        {
            auto pElemIdx = ConstantInt::get(m_pContext->Int32Ty(), elemIdx);

            // Calculate array element offset
            auto pElemOffset = BinaryOperator::CreateMul(pStride, pElemIdx, "", pInsertPos);
            pElemOffset = BinaryOperator::CreateAdd(pBlockMemberOffset, pElemOffset, "", pInsertPos);
            if (pElemTy->isSingleValueType())
            {
                ShaderBlockMetadata elemMeta = {};
                elemMeta.U64All = cast<ConstantInt>(pElemMeta)->getZExtValue();
                pElemOffset = BinaryOperator::CreateAdd(pElemOffset,
                                                        ConstantInt::get(m_pContext->Int32Ty(), elemMeta.offset),
                                                        "",
                                                        pInsertPos);
            }

            // Load array element
            auto pElem = AddBufferLoadDescInst(pElemTy,
                                               pDesc,
                                               pElemOffset,
                                               pElemMeta,
                                               pInsertPos);

            // Insert array element to load value
            std::vector<uint32_t> idxs;
            idxs.push_back(elemIdx);
            pLoadValue = InsertValueInst::Create(pLoadValue, pElem, idxs, "", pInsertPos);
        }

        if (isRowMajorMatrix)
        {
            // NOTE: Here, we have to revise the load value (do transposing).
            pLoadValue = TransposeMatrix(pLoadValue, pInsertPos);
        }
    }
    else
    {
        // Load structure type

        // NOTE: Calculated block member offset is 0 when the member type is aggregate. So the specified
        // "pBlockMemberOffset" does not include the offset of the structure. We have to add it here.
        LLPC_ASSERT(pLoadTy->isStructTy());

        const uint32_t memberCount = pLoadTy->getStructNumElements();
        for (uint32_t memberIdx = 0; memberIdx < memberCount; ++memberIdx)
        {
            auto pMemberTy = pLoadTy->getStructElementType(memberIdx);
            auto pStruct = cast<Constant>(pBlockMemberMeta->getOperand(1));
            auto pMemberMeta = cast<Constant>(pStruct->getAggregateElement(memberIdx));

            Value* pMemberOffset = nullptr;
            ShaderBlockMetadata blockMeta = {};
            if (pMemberTy->isSingleValueType())
            {
                blockMeta.U64All = cast<ConstantInt>(pMemberMeta)->getZExtValue();
            }
            else if (pMemberTy->isArrayTy())
            {
                blockMeta.U64All = cast<ConstantInt>(pMemberMeta->getOperand(1))->getZExtValue();
            }
            else
            {
                LLPC_ASSERT(pMemberTy->isStructTy() == true);
                blockMeta.U64All = cast<ConstantInt>(pMemberMeta->getOperand(0))->getZExtValue();
            }

            pMemberOffset = BinaryOperator::CreateAdd(pBlockMemberOffset,
                                                      ConstantInt::get(m_pContext->Int32Ty(), blockMeta.offset),
                                                      "",
                                                      pInsertPos);

            // Load structure member
            auto pMember = AddBufferLoadDescInst(pMemberTy,
                                                pDesc,
                                                pMemberOffset,
                                                pMemberMeta,
                                                pInsertPos);

            // Insert structure member to load value
            std::vector<uint32_t> idxs;
            idxs.push_back(memberIdx);
            pLoadValue = InsertValueInst::Create(pLoadValue, pMember, idxs, "", pInsertPos);
        }
    }

    return pLoadValue;
}

// =====================================================================================================================
// Inserts instructions to store variable to buffer block.
void SpirvLowerBufferOp::AddBufferStoreInst(
    Value*       pStoreValue,        // [in] Value stored to buffer block
    uint32_t     descSet,            // Descriptor set of buffer block
    uint32_t     binding,            // Descriptor binding of buffer block
    bool         isScalarAligned,    // Is the store scalar aligned
    Value*       pBlockOffset,       // [in] Block offset
    Value*       pBlockMemberOffset, // [in] Block member offset
    Constant*    pBlockMemberMeta,   // [in] Metadata of buffer block
    Instruction* pInsertPos)         // [in] Where to insert instructions
{
    const auto pStoreTy = pStoreValue->getType();
    std::unordered_set<Value*> checkedValues;
    bool isNonUniform = IsNonUniformValue(pBlockOffset, checkedValues);

    if (pStoreTy->isSingleValueType())
    {
        // Store scalar or vector type

        ShaderBlockMetadata blockMeta = {};
        blockMeta.U64All = cast<ConstantInt>(pBlockMemberMeta)->getZExtValue();

        if (blockMeta.IsRowMajor && pStoreTy->isVectorTy())
        {
            // NOTE: For row-major matrix, storing a column vector is done by storing its own components separately.
            auto pCompTy = pStoreTy->getVectorElementType();
            const uint32_t compCount = pStoreTy->getVectorNumElements();

            // Cast type of the component type to <n x i8>
            uint32_t storeSize = pCompTy->getPrimitiveSizeInBits() / 8;
            Type* pCastTy = VectorType::get(m_pContext->Int8Ty(), storeSize);
            std::string suffix = GetTypeNameForScalarOrVector(pCastTy);

            for (uint32_t i = 0; i < compCount; ++i)
            {
                Value* pCompValue = ExtractElementInst::Create(pStoreValue,
                                                               ConstantInt::get(m_pContext->Int32Ty(), i),
                                                               "",
                                                               pInsertPos);

                LLPC_ASSERT(CanBitCast(pCompTy, pCastTy));
                pCompValue = new BitCastInst(pCompValue, pCastTy, "", pInsertPos);

                // Build arguments for buffer store
                std::vector<Value*> args;
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), descSet));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), binding));
                args.push_back(pBlockOffset);
                args.push_back(pBlockMemberOffset);
                args.push_back(pCompValue);
                args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Coherent ? true : false)); // glc
                args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Volatile ? true : false)); // slc
                args.push_back(ConstantInt::get(m_pContext->BoolTy(), isNonUniform ? true : false)); // nonUniform
                EmitCall(m_pModule, LlpcName::BufferStore + suffix, m_pContext->VoidTy(), args, NoAttrib, pInsertPos);

                // Update the block member offset for the next component
                pBlockMemberOffset = BinaryOperator::CreateAdd(pBlockMemberOffset,
                                                               ConstantInt::get(m_pContext->Int32Ty(),
                                                                                blockMeta.MatrixStride),
                                                               "",
                                                               pInsertPos);
            }
        }
        else
        {
            const uint32_t storeSize = pStoreTy->getPrimitiveSizeInBits() / 8;

            Type* pActualStoreTy = nullptr;

            // If we need a scalar aligned store.
            if (isScalarAligned)
            {
                if (pStoreTy->getVectorElementType()->isHalfTy())
                {
                    pActualStoreTy = VectorType::get(m_pContext->Int16Ty(), pStoreTy->getVectorNumElements());
                }
                else
                {
                    LLPC_ASSERT(pStoreTy->isIntOrIntVectorTy(8) || pStoreTy->isIntOrIntVectorTy(16));
                    pActualStoreTy = pStoreTy;
                }
            }
            else if (storeSize == 1)
            {
                pActualStoreTy = pStoreTy;
            }
            else
            {
                // Cast type of the store value to <n x i8>
                pActualStoreTy = VectorType::get(m_pContext->Int8Ty(), storeSize);
            }

            if (pActualStoreTy != pStoreTy)
            {
                LLPC_ASSERT(CanBitCast(pStoreTy, pActualStoreTy));
                pStoreValue = new BitCastInst(pStoreValue, pActualStoreTy, "", pInsertPos);
            }

            // Build arguments for buffer store
            std::vector<Value*> args;
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), descSet));
            args.push_back(ConstantInt::get(m_pContext->Int32Ty(), binding));
            args.push_back(pBlockOffset);
            args.push_back(pBlockMemberOffset);
            args.push_back(pStoreValue);
            args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Coherent ? true : false)); // glc
            args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Volatile ? true : false)); // slc
            args.push_back(ConstantInt::get(m_pContext->BoolTy(), isNonUniform ? true : false)); // nonUniform

            std::string suffix = GetTypeNameForScalarOrVector(pActualStoreTy);
            const char* pInstName = isScalarAligned ?
                LlpcName::BufferStoreScalarAligned : LlpcName::BufferStore;

            EmitCall(m_pModule, pInstName + suffix, m_pContext->VoidTy(), args, NoAttrib, pInsertPos);
        }
    }
    else if (pStoreTy->isArrayTy())
    {
        // Store array or matrix type
        LLPC_ASSERT(pBlockMemberMeta->getNumOperands() == 3);
        auto pStride = cast<ConstantInt>(pBlockMemberMeta->getOperand(0));
        ShaderBlockMetadata arrayMeta = {};
        arrayMeta.U64All = cast<ConstantInt>(pBlockMemberMeta->getOperand(1))->getZExtValue();
        auto pElemMeta = cast<Constant>(pBlockMemberMeta->getOperand(2));

        const bool isRowMajorMatrix = (arrayMeta.IsMatrix && arrayMeta.IsRowMajor);

        auto pElemTy = pStoreTy->getArrayElementType();
        uint32_t elemCount = pStoreTy->getArrayNumElements();

        if (isRowMajorMatrix)
        {
            // NOTE: For row-major matrix, we process it with its transposed form.
            const auto pColVecTy = pElemTy;
            LLPC_ASSERT(pColVecTy->isVectorTy());
            const auto colCount = elemCount;
            const auto rowCount = pColVecTy->getVectorNumElements();

            auto pCompTy = pColVecTy->getVectorElementType();

            auto pRowVecTy = VectorType::get(pCompTy, colCount);

            // NOTE: Here, we have to revise the store value (do transposing), element type, and element count..
            pStoreValue = TransposeMatrix(pStoreValue, pInsertPos);
            pElemTy = pRowVecTy;
            elemCount = rowCount;

            // NOTE: Here, we have to clear "row-major" flag in metadata since the matrix is processed as
            // "column-major" style.
            ShaderBlockMetadata elemMeta = {};
            elemMeta.U64All = cast<ConstantInt>(pElemMeta)->getZExtValue();
            elemMeta.IsRowMajor = false;
            pElemMeta = ConstantInt::get(m_pContext->Int64Ty(), elemMeta.U64All);
        }

        for (uint32_t elemIdx = 0; elemIdx < elemCount; ++elemIdx)
        {
            auto pElemIdx = ConstantInt::get(m_pContext->Int32Ty(), elemIdx);

            // Extract array element from store value
            std::vector<uint32_t> idxs;
            idxs.push_back(elemIdx);
            Value* pElem = ExtractValueInst::Create(pStoreValue, idxs, "", pInsertPos);

            // Calculate array element offset
            auto pElemOffset = BinaryOperator::CreateMul(pStride, pElemIdx, "", pInsertPos);
            pElemOffset = BinaryOperator::CreateAdd(pBlockMemberOffset, pElemOffset, "", pInsertPos);
            if (pElemTy->isSingleValueType())
            {
                ShaderBlockMetadata elemMeta = {};
                elemMeta.U64All = cast<ConstantInt>(pElemMeta)->getZExtValue();
                pElemOffset = BinaryOperator::CreateAdd(pElemOffset,
                                                        ConstantInt::get(m_pContext->Int32Ty(), elemMeta.offset),
                                                        "",
                                                        pInsertPos);
            }

            // Store array element
            AddBufferStoreInst(pElem,
                               descSet,
                               binding,
                               isScalarAligned,
                               pBlockOffset,
                               pElemOffset,
                               pElemMeta,
                               pInsertPos);
        }
    }
    else
    {
        // Store structure type

        // NOTE: Calculated block member offset is 0 when the member type is aggregate. So the specified
        // "pBlockMemberOffset" does not include the offset of the structure. We have to add it here.
        LLPC_ASSERT(pStoreTy->isStructTy());

        const uint32_t memberCount = pStoreTy->getStructNumElements();
        for (uint32_t memberIdx = 0; memberIdx < memberCount; ++memberIdx)
        {
            auto pMemberTy = pStoreTy->getStructElementType(memberIdx);

            // Extract structure member from store value
            std::vector<uint32_t> idxs;
            idxs.push_back(memberIdx);
            Value* pMember = ExtractValueInst::Create(pStoreValue, idxs, "", pInsertPos);
            auto pStruct = cast<Constant>(pBlockMemberMeta->getOperand(1));
            auto pMemberMeta = cast<Constant>(pStruct->getAggregateElement(memberIdx));

            Value* pMemberOffset = nullptr;
            ShaderBlockMetadata blockMeta = {};
            if (pMemberTy->isSingleValueType())
            {
                blockMeta.U64All = cast<ConstantInt>(pMemberMeta)->getZExtValue();
            }
            else if (pMemberTy->isArrayTy())
            {
                blockMeta.U64All = cast<ConstantInt>(pMemberMeta->getOperand(1))->getZExtValue();
            }
            else
            {
                LLPC_ASSERT(pMemberTy->isStructTy() == true);
                blockMeta.U64All = cast<ConstantInt>(pMemberMeta->getOperand(0))->getZExtValue();
            }

            pMemberOffset = BinaryOperator::CreateAdd(pBlockMemberOffset,
                                                      ConstantInt::get(m_pContext->Int32Ty(), blockMeta.offset),
                                                      "",
                                                      pInsertPos);

            // Store structure member
            AddBufferStoreInst(pMember,
                               descSet,
                               binding,
                               isScalarAligned,
                               pBlockOffset,
                               pMemberOffset,
                               pMemberMeta,
                               pInsertPos);
        }
    }
}

// =====================================================================================================================
// Inserts instructions to do atomic operations on buffer block.
Value* SpirvLowerBufferOp::AddBufferAtomicInst(
    std::string                atomicOpName,       // Name of atomic operation
    Type*                      pDataTy,            // [in] Type of data involved in atomic operations
    const std::vector<Value*>& data,               // [in] Data involved in atomic operations
    uint32_t                   descSet,            // Descriptor set of buffer block
    uint32_t                   binding,            // Descriptor binding of buffer block
    Value*                     pBlockOffset,       // [in] Block offset
    Value*                     pBlockMemberOffset, // [in] Block member offset
    Constant*                  pBlockMemberMeta,   // [in] Metadata of buffer block
    Instruction*               pInsertPos)         // [in] Where to insert instructions
{
    LLPC_ASSERT(pDataTy->isIntegerTy() || pDataTy->isFloatingPointTy());
    const uint32_t bitWidth = pDataTy->getScalarSizeInBits();
    LLPC_ASSERT((bitWidth == 32) || (bitWidth == 64));

    std::unordered_set<Value*> checkedValues;
    bool isNonUniform = IsNonUniformValue(pBlockOffset, checkedValues);

    Value* pAtomicValue = nullptr;

    std::string suffix = ".i" + std::to_string(bitWidth);

    ShaderBlockMetadata blockMeta = {};
    blockMeta.U64All = cast<ConstantInt>(pBlockMemberMeta)->getZExtValue();

    std::vector<Value*> args;
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), descSet));
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), binding));
    args.push_back(pBlockOffset);
    args.push_back(pBlockMemberOffset);
    for (auto pData : data)
    {
        args.push_back(pData);
    }
    args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Volatile ? true : false)); // slc
    args.push_back(ConstantInt::get(m_pContext->BoolTy(), isNonUniform ? true : false)); // nonUniform

    if (atomicOpName == "store")
    {
        EmitCall(m_pModule,
                 LlpcName::BufferAtomic + atomicOpName + suffix,
                 m_pContext->VoidTy(),
                 args,
                 NoAttrib,
                 pInsertPos);
    }
    else
    {
        pAtomicValue = EmitCall(m_pModule,
                                LlpcName::BufferAtomic + atomicOpName + suffix,
                                pDataTy,
                                args,
                                NoAttrib,
                                pInsertPos);
    }

    return pAtomicValue;
}

// =====================================================================================================================
// Tranposes a specified matrix (used for processing row-major matrix only).
Value* SpirvLowerBufferOp::TransposeMatrix(
    Value*       pMatrix,       // [in] Matrix to be transposed
    Instruction* pInsertPos)    // [in] Where to insert transpose instructions
{
    auto pMatrixTy = pMatrix->getType();
    LLPC_ASSERT(pMatrixTy->isArrayTy());

    auto pColVecTy = pMatrixTy->getArrayElementType();
    LLPC_ASSERT(pColVecTy->isVectorTy());
    const uint32_t colCount = pMatrixTy->getArrayNumElements();
    const uint32_t rowCount = pColVecTy->getVectorNumElements();

    auto pCompTy = pColVecTy->getVectorElementType();

    auto pRowVecTy = VectorType::get(pCompTy, colCount);
    auto pTransposeTy = ArrayType::get(pRowVecTy, rowCount);
    Value* pTranspose = UndefValue::get(pTransposeTy);

    std::vector<Value*> rowVecs;
    for (uint32_t i = 0; i < rowCount; ++i)
    {
        // Initialize row vectors
        rowVecs.push_back(UndefValue::get(pRowVecTy));
    }

    for (uint32_t i = 0; i < colCount; ++i)
    {
        // Extract components from column vectors and insert them to corresponding row vectors
        std::vector<uint32_t> idxs;
        idxs.push_back(i);
        auto pColVec = ExtractValueInst::Create(pMatrix, idxs, "", pInsertPos);

        for (uint32_t j = 0; j < rowCount; ++j)
        {
            auto pColComp = ExtractElementInst::Create(pColVec,
                                                       ConstantInt::get(m_pContext->Int32Ty(), j),
                                                       "",
                                                       pInsertPos);
            rowVecs[j] = InsertElementInst::Create(rowVecs[j],
                                                   pColComp,
                                                   ConstantInt::get(m_pContext->Int32Ty(), i),
                                                   "",
                                                   pInsertPos);
        }
    }

    for (uint32_t i = 0; i < rowCount; ++i)
    {
        // Insert row vectors
        std::vector<uint32_t> idxs;
        idxs.push_back(i);
        pTranspose = InsertValueInst::Create(pTranspose, rowVecs[i], idxs, "", pInsertPos);
    }

    return pTranspose;
}

// =====================================================================================================================
// Loads variable from entire buffer block.
Value* SpirvLowerBufferOp::LoadEntireBlock(
    GlobalVariable*      pBlock,        // [in] Buffer block
    Type*                pLoadTy,       // [in] Type of value loaded from buffer block
    std::vector<Value*>& indexOperands, // [in] Index operands (used for block array dimension)
    Instruction*         pInsertPos)    // [in] Where to insert instructions
{
    Value* pLoadValue = UndefValue::get(pLoadTy);

    if (pLoadTy->isArrayTy())
    {
        // Handle block array
        auto pElemTy = pLoadTy->getArrayElementType();
        const uint64_t elemCount = pLoadTy->getArrayNumElements();

        for (uint32_t elemIdx = 0; elemIdx < elemCount; ++elemIdx)
        {
            // Handle array elements recursively
            indexOperands.push_back(ConstantInt::get(m_pContext->Int32Ty(), elemIdx));
            auto pElem = LoadEntireBlock(pBlock, pElemTy, indexOperands, pInsertPos);
            indexOperands.pop_back();

            std::vector<uint32_t> idxs;
            idxs.push_back(elemIdx);
            pLoadValue = InsertValueInst::Create(pLoadValue, pElem, idxs, "", pInsertPos);
        }
    }
    else
    {
        auto pBlockTy = pBlock->getType()->getPointerElementType();

        uint32_t descSet = InvalidValue;
        uint32_t binding = InvalidValue;

        uint32_t operandIdx = 0;

        Value* pBlockOffset = nullptr;

        bool isPushConst = (pBlock->getType()->getPointerAddressSpace() == SPIRAS_PushConst);

        if (isPushConst == false)
        {
            // Calculate block offset, push constant is ignored
            uint32_t stride = 0;
            pBlockOffset = CalcBlockOffset(pBlockTy, indexOperands, 0, pInsertPos, &stride);

            MDNode* pResMetaNode = pBlock->getMetadata(gSPIRVMD::Resource);
            LLPC_ASSERT(pResMetaNode != nullptr);
            LLPC_ASSERT(pResMetaNode->getNumOperands() == 3);

            descSet = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(0))->getZExtValue();
            binding = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(1))->getZExtValue();
            SPIRVBlockTypeKind blockKind = static_cast<SPIRVBlockTypeKind>(
                                               mdconst::dyn_extract<ConstantInt>(
                                                   pResMetaNode->getOperand(2))->getZExtValue());
            LLPC_ASSERT((blockKind == BlockTypeUniform) || (blockKind == BlockTypeShaderStorage));
            LLPC_UNUSED(blockKind);

            // Ignore array dimensions, block must start with structure type
            while (pBlockTy->isArrayTy())
            {
                pBlockTy = pBlockTy->getArrayElementType();
                ++operandIdx;
            }
        }

        // Calculate member offset and get corresponding resulting metadata
        Constant* pResultMeta    = nullptr;
        MDNode*   pBlockMetaNode = pBlock->getMetadata(gSPIRVMD::Block);
        Constant* pBlockMeta     = mdconst::dyn_extract<Constant>(pBlockMetaNode->getOperand(0));
        Value*    pMemberOffset  = CalcBlockMemberOffset(pBlockTy,
                                                         indexOperands,
                                                         operandIdx,
                                                         pBlockMeta,
                                                         pInsertPos,
                                                         &pResultMeta);

        const bool isScalarAligned = NeedScalarAlignment(
            pLoadTy, pBlockTy, indexOperands, operandIdx, pBlockMeta);

        // Load variable from buffer block
        pLoadValue = AddBufferLoadInst(pLoadTy,
                                       descSet,
                                       binding,
                                       isPushConst,
                                       isScalarAligned,
                                       pBlockOffset,
                                       pMemberOffset,
                                       pResultMeta,
                                       pInsertPos);
    }

    return pLoadValue;
}

// =====================================================================================================================
// Stores variable to entire buffer block.
void SpirvLowerBufferOp::StoreEntireBlock(
    GlobalVariable*      pBlock,        // [in] Buffer block
    Value*               pStoreValue,   // [in] Value stored to buffer block
    std::vector<Value*>& indexOperands, // [in] Index operands (used for block array dimension)
    Instruction*         pInsertPos)    // [in] Where to insert instructions
{
    const auto pStoreTy = pStoreValue->getType();

    if (pStoreTy->isArrayTy())
    {
        // Handle block array
        const uint64_t elemCount = pStoreTy->getArrayNumElements();

        for (uint32_t elemIdx = 0; elemIdx < elemCount; ++elemIdx)
        {
            // Handle array elements recursively
            std::vector<uint32_t> idxs;
            idxs.push_back(elemIdx);
            auto pElem = ExtractValueInst::Create(pStoreValue, idxs, "", pInsertPos);

            indexOperands.push_back(ConstantInt::get(m_pContext->Int32Ty(), elemIdx));
            StoreEntireBlock(pBlock, pElem, indexOperands, pInsertPos);
            indexOperands.pop_back();
        }
    }
    else
    {
        auto pBlockTy = pBlock->getType()->getPointerElementType();

        // Calculate block offset
        uint32_t stride  = 0;
        Value* pBlockOffset = CalcBlockOffset(pBlockTy, indexOperands, 0, pInsertPos, &stride);

        MDNode* pResMetaNode = pBlock->getMetadata(gSPIRVMD::Resource);
        LLPC_ASSERT(pResMetaNode != nullptr);
        LLPC_ASSERT(pResMetaNode->getNumOperands() == 3);

        auto descSet   = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(0))->getZExtValue();
        auto binding   = mdconst::dyn_extract<ConstantInt>(pResMetaNode->getOperand(1))->getZExtValue();
        LLPC_ASSERT(static_cast<SPIRVBlockTypeKind>(
                                           mdconst::dyn_extract<ConstantInt>(
                                               pResMetaNode->getOperand(2))->getZExtValue())
                    == BlockTypeShaderStorage); // Must be shader storage block

        // Ignore array dimensions, block must start with structure type
        uint32_t operandIdx = 0;
        while (pBlockTy->isArrayTy())
        {
            pBlockTy = pBlockTy->getArrayElementType();
            ++operandIdx;
        }

        // Calculate member offset and get corresponding resulting metadata
        Constant* pResultMeta    = nullptr;
        MDNode*   pBlockMetaNode = pBlock->getMetadata(gSPIRVMD::Block);
        Constant* pBlockMeta     = mdconst::dyn_extract<Constant>(pBlockMetaNode->getOperand(0));
        Value*    pMemberOffset  = CalcBlockMemberOffset(pBlockTy,
                                                         indexOperands,
                                                         operandIdx,
                                                         pBlockMeta,
                                                         pInsertPos,
                                                         &pResultMeta);

        const bool isScalarAligned = NeedScalarAlignment(
            pStoreValue->getType(), pBlockTy, indexOperands, operandIdx, pBlockMeta);

        // Store variable to buffer block
        AddBufferStoreInst(pStoreValue,
                           descSet,
                           binding,
                           isScalarAligned,
                           pBlockOffset,
                           pMemberOffset,
                           pResultMeta,
                           pInsertPos);
    }
}

// =====================================================================================================================
// Inserts instructions to store variable to buffer block (with descriptor).
void SpirvLowerBufferOp::AddBufferStoreDescInst(
    Value*       pStoreValue,        // [in] Value stored to buffer block
    Value*       pDesc,              // [in] Descriptor set of buffer block
    Value*       pBlockMemberOffset, // [in] Block member offset
    Constant*    pBlockMemberMeta,   // [in] Metadata of buffer block
    Instruction* pInsertPos)         // [in] Where to insert instructions
{
    const auto pStoreTy = pStoreValue->getType();
    if (pStoreTy->isSingleValueType())
    {
        // Store scalar or vector type

        ShaderBlockMetadata blockMeta = {};
        if (blockMeta.IsRowMajor && pStoreTy->isVectorTy())
        {
            // NOTE: For row-major matrix, storing a column vector is done by storing its own components separately.
            auto pCompTy = pStoreTy->getVectorElementType();
            const uint32_t compCount = pStoreTy->getVectorNumElements();

            // Cast type of the component type to <n x i8>
            uint32_t storeSize = pCompTy->getPrimitiveSizeInBits() / 8;
            Type* pCastTy = VectorType::get(m_pContext->Int8Ty(), storeSize);
            std::string suffix = GetTypeNameForScalarOrVector(pCastTy);

            for (uint32_t i = 0; i < compCount; ++i)
            {
                Value* pCompValue = ExtractElementInst::Create(pStoreValue,
                    ConstantInt::get(m_pContext->Int32Ty(), i),
                    "",
                    pInsertPos);

                LLPC_ASSERT(CanBitCast(pCompTy, pCastTy));
                pCompValue = new BitCastInst(pCompValue, pCastTy, "", pInsertPos);

                // Build arguments for buffer store
                std::vector<Value*> args;

                args.push_back(pDesc);
                args.push_back(pBlockMemberOffset);
                args.push_back(pCompValue);
                args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Coherent ? true : false)); // glc
                args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Volatile ? true : false)); // slc

                EmitCall(m_pModule, LlpcName::BufferStoreDesc + suffix, m_pContext->VoidTy(), args, NoAttrib, pInsertPos);

                // Update the block member offset for the next component
                pBlockMemberOffset = BinaryOperator::CreateAdd(pBlockMemberOffset,
                                                               ConstantInt::get(m_pContext->Int32Ty(),
                                                               blockMeta.MatrixStride),
                                                               "",
                                                               pInsertPos);
            }
        }
        else
        {
            const uint32_t storeSize = pStoreTy->getPrimitiveSizeInBits() / 8;

            // If scalar block layout is enabled, we need to treat vector types with 1 / 2 byte components differently.
            const bool isScalarBlockLayout = false;
            const bool isSmallVector = pStoreTy->isVectorTy() && (pStoreTy->getScalarSizeInBits() < 32);
            const bool needScalarAlignedStore = isScalarBlockLayout && isSmallVector;

            Type* pActualStoreTy = nullptr;

            // If we need a scalar aligned store.
            if (needScalarAlignedStore)
            {
                if (pStoreTy->getVectorElementType()->isHalfTy())
                {
                    pActualStoreTy = VectorType::get(m_pContext->Int16Ty(), pStoreTy->getVectorNumElements());
                }
                else
                {
                    LLPC_ASSERT(pStoreTy->isIntOrIntVectorTy(8) || pStoreTy->isIntOrIntVectorTy(16));
                    pActualStoreTy = pStoreTy;
                }
            }
            else if (storeSize == 1)
            {
                pActualStoreTy = pStoreTy;
            }
            else
            {
                // Cast type of the store value to <n x i8>
                pActualStoreTy = VectorType::get(m_pContext->Int8Ty(), storeSize);
            }

            if (pActualStoreTy != pStoreTy)
            {
                LLPC_ASSERT(CanBitCast(pStoreTy, pActualStoreTy));
                pStoreValue = new BitCastInst(pStoreValue, pActualStoreTy, "", pInsertPos);
            }

            // Build arguments for buffer store
            std::vector<Value*> args;
            args.push_back(pDesc);
            args.push_back(pBlockMemberOffset);
            args.push_back(pStoreValue);
            args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Coherent ? true : false)); // glc
            args.push_back(ConstantInt::get(m_pContext->BoolTy(), blockMeta.Volatile ? true : false)); // slc

            std::string suffix = GetTypeNameForScalarOrVector(pActualStoreTy);
            const char* pInstName = needScalarAlignedStore ?
                LlpcName::BufferStoreScalarAlignedDesc : LlpcName::BufferStoreDesc;

            EmitCall(m_pModule, pInstName + suffix, m_pContext->VoidTy(), args, NoAttrib, pInsertPos);
        }
    }
    else if (pStoreTy->isArrayTy())
    {
        // Store array or matrix type
        LLPC_ASSERT(pBlockMemberMeta->getNumOperands() == 3);
        auto pStride = cast<ConstantInt>(pBlockMemberMeta->getOperand(0));
        ShaderBlockMetadata arrayMeta = {};
        arrayMeta.U64All = cast<ConstantInt>(pBlockMemberMeta->getOperand(1))->getZExtValue();
        auto pElemMeta = cast<Constant>(pBlockMemberMeta->getOperand(2));

        const bool isRowMajorMatrix = (arrayMeta.IsMatrix && arrayMeta.IsRowMajor);

        auto pElemTy = pStoreTy->getArrayElementType();
        uint32_t elemCount = pStoreTy->getArrayNumElements();

        if (isRowMajorMatrix)
        {
            // NOTE: For row-major matrix, we process it with its transposed form.
            const auto pColVecTy = pElemTy;
            LLPC_ASSERT(pColVecTy->isVectorTy());
            const auto colCount = elemCount;
            const auto rowCount = pColVecTy->getVectorNumElements();

            auto pCompTy = pColVecTy->getVectorElementType();

            auto pRowVecTy = VectorType::get(pCompTy, colCount);

            // NOTE: Here, we have to revise the store value (do transposing), element type, and element count..
            pStoreValue = TransposeMatrix(pStoreValue, pInsertPos);
            pElemTy = pRowVecTy;
            elemCount = rowCount;

            // NOTE: Here, we have to clear "row-major" flag in metadata since the matrix is processed as
            // "column-major" style.
            ShaderBlockMetadata elemMeta = {};
            elemMeta.U64All = cast<ConstantInt>(pElemMeta)->getZExtValue();
            elemMeta.IsRowMajor = false;
            pElemMeta = ConstantInt::get(m_pContext->Int64Ty(), elemMeta.U64All);
        }

        for (uint32_t elemIdx = 0; elemIdx < elemCount; ++elemIdx)
        {
            auto pElemIdx = ConstantInt::get(m_pContext->Int32Ty(), elemIdx);

            // Extract array element from store value
            std::vector<uint32_t> idxs;
            idxs.push_back(elemIdx);
            Value* pElem = ExtractValueInst::Create(pStoreValue, idxs, "", pInsertPos);

            // Calculate array element offset
            auto pElemOffset = BinaryOperator::CreateMul(pStride, pElemIdx, "", pInsertPos);
            pElemOffset = BinaryOperator::CreateAdd(pBlockMemberOffset, pElemOffset, "", pInsertPos);
            if (pElemTy->isSingleValueType())
            {
                ShaderBlockMetadata elemMeta = {};
                elemMeta.U64All = cast<ConstantInt>(pElemMeta)->getZExtValue();
                pElemOffset = BinaryOperator::CreateAdd(pElemOffset,
                                                        ConstantInt::get(m_pContext->Int32Ty(), elemMeta.offset),
                                                        "",
                                                        pInsertPos);
            }

            // Store array element
            AddBufferStoreDescInst(pElem,
                                   pDesc,
                                   pElemOffset,
                                   pElemMeta,
                                   pInsertPos);
        }
    }
    else
    {
        // Store structure type

        // NOTE: Calculated block member offset is 0 when the member type is aggregate. So the specified
        // "pBlockMemberOffset" does not include the offset of the structure. We have to add it here.
        LLPC_ASSERT(pStoreTy->isStructTy());

        const uint32_t memberCount = pStoreTy->getStructNumElements();
        for (uint32_t memberIdx = 0; memberIdx < memberCount; ++memberIdx)
        {
            auto pMemberTy = pStoreTy->getStructElementType(memberIdx);

            // Extract structure member from store value
            std::vector<uint32_t> idxs;
            idxs.push_back(memberIdx);
            Value* pMember = ExtractValueInst::Create(pStoreValue, idxs, "", pInsertPos);
            auto pStruct = cast<Constant>(pBlockMemberMeta->getOperand(1));
            auto pMemberMeta = cast<Constant>(pStruct->getAggregateElement(memberIdx));

            Value* pMemberOffset = nullptr;
            ShaderBlockMetadata blockMeta = {};
            if (pMemberTy->isSingleValueType())
            {
                blockMeta.U64All = cast<ConstantInt>(pMemberMeta)->getZExtValue();
            }
            else if(pMemberTy->isArrayTy())
            {
                blockMeta.U64All = cast<ConstantInt>(pMemberMeta->getOperand(1))->getZExtValue();
            }
            else
            {
                LLPC_ASSERT(pMemberTy->isStructTy() == true);
                blockMeta.U64All = cast<ConstantInt>(pMemberMeta->getOperand(0))->getZExtValue();
            }

            pMemberOffset = BinaryOperator::CreateAdd(pBlockMemberOffset,
                                                      ConstantInt::get(m_pContext->Int32Ty(), blockMeta.offset),
                                                      "",
                                                      pInsertPos);

            // Store structure member
            AddBufferStoreDescInst(pMember,
                                   pDesc,
                                   pMemberOffset,
                                   pMemberMeta,
                                   pInsertPos);
        }
    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for buffer operations.
INITIALIZE_PASS(SpirvLowerBufferOp, "Spirv-lower-buffer-op",
                "Lower SPIR-V buffer operations (load and store)", false, false)
