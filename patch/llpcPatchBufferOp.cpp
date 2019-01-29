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
 * @file  llpcPatchBufferOp.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchBufferOp.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-patch-buffer-op"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcIntrinsDefs.h"
#include "llpcPatchBufferOp.h"
#include "llpcPipelineShaders.h"

using namespace llvm;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char PatchBufferOp::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching opertions for buffer operations
ModulePass* CreatePatchBufferOp()
{
    return new PatchBufferOp();
}

// =====================================================================================================================
PatchBufferOp::PatchBufferOp()
    :
    Patch(ID)
{
    initializePipelineShadersPass(*PassRegistry::getPassRegistry());
    initializePatchBufferOpPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
bool PatchBufferOp::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Buffer-Op\n");

    Patch::Init(&module);
    m_changed = false;

    // Invoke handling of "call" instruction
    auto pPipelineShaders = &getAnalysis<PipelineShaders>();
    for (uint32_t shaderStage = 0; shaderStage < ShaderStageCountInternal; ++shaderStage)
    {
        m_pEntryPoint = pPipelineShaders->GetEntryPoint(ShaderStage(shaderStage));
        if (m_pEntryPoint != nullptr)
        {
            m_shaderStage = ShaderStage(shaderStage);
            visit(*m_pEntryPoint);
        }
    }

    for (auto pCall : m_replacedCalls)
    {
        LLPC_ASSERT(pCall->user_empty());
        pCall->dropAllReferences();
        pCall->eraseFromParent();
    }
    m_replacedCalls.clear();

    return m_changed;
}

// =====================================================================================================================
// Visits "call" instruction.
void PatchBufferOp::visitCallInst(
    CallInst& callInst) // [in] "Call" instruction
{
    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

    auto mangledName = pCallee->getName();

    if (mangledName.startswith(LlpcName::BufferLoadDesc) ||
        mangledName.startswith(LlpcName::BufferStoreDesc) ||
        mangledName.startswith(LlpcName::BufferAtomicDesc))
    {
        // Variable pointer does not support nonuniform descriptor
    }
    else if (mangledName.startswith(LlpcName::BufferCallPrefix))
    {
        m_changed = true;
        uint32_t descSet = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
        uint32_t binding = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
        auto isNonUniform = cast<ConstantInt>(callInst.getOperand(callInst.getNumArgOperands() - 1))->getZExtValue();
        bool isInlineConst = IsInlineConst(descSet, binding);

        if (isNonUniform != 0)
        {
            AddWaterFallInst(2, -1, &callInst);
        }

        if (mangledName.startswith(LlpcName::BufferLoadScalarAligned))
        {
            // We do not replace scalar aligned buffer loads.
        }
        else if (mangledName.startswith(LlpcName::BufferLoad))
        {
            const bool readOnly = cast<ConstantInt>(callInst.getOperand(4))->getZExtValue();

            if (isInlineConst)
            {
                ReplaceCallee(&callInst, LlpcName::BufferLoad, LlpcName::InlineConstLoadUniform);
            }
            else if (readOnly)
            {
                ReplaceCallee(&callInst, LlpcName::BufferLoad, LlpcName::BufferLoadUniform);
            }
        }
        else if (mangledName.startswith(LlpcName::BufferStore))
        {
            // NOTE: Only uniform block support inline constant now
            LLPC_ASSERT(isInlineConst == false);

            auto pGpuWorkarounds = m_pContext->GetGpuWorkarounds();
            if (pGpuWorkarounds->gfx6.shader8b16bLocalWriteCorruption)
            {
                // NOTE: Enable workaround for the 8-bit/16-bit local write corruption bug. When doing 8-bit or
                // 16-bit UAV writes, a line could incorrectly be left valid in TCP's cache when not all bytes
                // were written. The workaround is to set the GLC (globally coherent) bit for such writes,
                // effectively disabling the L1 cache.
                if ((mangledName.find(".i8") != StringRef::npos) ||
                    (mangledName.find(".v2i8") != StringRef::npos) ||
                    (mangledName.find(".v3i8") != StringRef::npos) ||
                    (mangledName.find(".v6i8") != StringRef::npos))
                {
                    uint32_t glcOperandIdx = 5;
                    if (mangledName.startswith(LlpcName::BufferStoreDesc))
                    {
                        glcOperandIdx = 3;
                    }
                    callInst.setOperand(glcOperandIdx, ConstantInt::getTrue(*m_pContext));
                }
            }

            // TODO: Translate buffer store operation to s_store if the offset is uniform, which is similar to buffer
            // load operation.
        }
        else
        {
            // NOTE: Only uniform block support inline constant now, and we can't translate other buffer operations to
            // scalar
            LLPC_ASSERT(isInlineConst == false);
        }
    }
}

// =====================================================================================================================
// Checks whether the specified pair of descriptor set/binding represent an inline constant buffer.
bool PatchBufferOp::IsInlineConst(
    uint32_t descSet,     // Descriptor set
    uint32_t binding)     // Descriptor binding
{
    bool isInlineConst = false;
    bool exist = false;
    auto pShaderInfo = m_pContext->GetPipelineShaderInfo(m_shaderStage);

    for (uint32_t i = 0; (i < pShaderInfo->userDataNodeCount) && (exist == false); ++i)
    {
        auto pResNode = &pShaderInfo->pUserDataNodes[i];
        if (pResNode->type == ResourceMappingNodeType::DescriptorTableVaPtr)
        {
            for (uint32_t j = 0; j < pResNode->tablePtr.nodeCount; ++j)
            {
                auto pSubNode = &pResNode->tablePtr.pNext[j];
                if ((pSubNode->srdRange.set == descSet) &&
                    (pSubNode->srdRange.binding == binding))
                {
                    exist = true;
                    isInlineConst = (pSubNode->type == ResourceMappingNodeType::PushConst);
                    break;
                }
            }
        }
    }

    return isInlineConst;
}

// =====================================================================================================================
// Replaces the callee in the specified "call" instruction.
void PatchBufferOp::ReplaceCallee(
    CallInst* pCallInst,               // [in] Call instruction
    const char* pOrigNamePrefix,       // [in] Original function name's prefix
    const char* pNewNamePrefix)        // [in] New function name's prefix
{
    LLPC_ASSERT(pCallInst->getCalledFunction()->getName().startswith(pOrigNamePrefix));

    // Build name for the new callee
    auto origName = pCallInst->getCalledFunction()->getName();
    auto nameSuffix = origName.substr(strlen(pOrigNamePrefix));
    std::string newName = pNewNamePrefix;
    newName += nameSuffix;

    // Copy instruction name, argumentss, metadata and attributes
    std::string instName;
    if ((pCallInst->getType()->isVoidTy() == false) && pCallInst->hasName())
    {
        instName = pCallInst->getName();
        pCallInst->setName(instName + ".old");
    }

    auto pArgs = getArguments(pCallInst);
    auto attrList = pCallInst->getAttributes();
    std::vector<Attribute::AttrKind> attrs;

    for (auto attrSet : attrList)
    {
        for (auto attr : attrSet)
        {
            attrs.push_back(attr.getKindAsEnum());
        }
    }

    SmallVector<std::pair<unsigned, MDNode *>, 8> allMeta;
    pCallInst->getAllMetadata(allMeta);

    // Create new call instruction
    auto pNewCall = cast<CallInst>(EmitCall(m_pModule, newName, pCallInst->getType(), pArgs, attrs, pCallInst));

    for (auto meta : allMeta)
    {
        pNewCall->setMetadata(meta.first, meta.second);
    }

    pCallInst->replaceAllUsesWith(pNewCall);
    m_replacedCalls.insert(pCallInst);
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM patch operations for buffer operations.
INITIALIZE_PASS(PatchBufferOp, DEBUG_TYPE,
                "Patch LLVM for buffer operations", false, false)
