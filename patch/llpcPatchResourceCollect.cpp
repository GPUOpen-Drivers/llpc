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
 * @file  llpcPatchResourceCollect.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchResourceCollect.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-patch-resource-collect"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcIntrinsDefs.h"
#include "llpcPatchResourceCollect.h"
#include "llpcPipelineShaders.h"
#include "llpcBuilder.h"

using namespace llvm;
using namespace Llpc;

namespace llvm
{

namespace cl
{
    extern opt<bool> PackIo;
}

}

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char PatchResourceCollect::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for resource collecting
ModulePass* CreatePatchResourceCollect()
{
    return new PatchResourceCollect();
}

// =====================================================================================================================
PatchResourceCollect::PatchResourceCollect()
    :
    Patch(ID),
    m_hasPushConstOp(false),
    m_hasDynIndexedInput(false),
    m_hasDynIndexedOutput(false),
    m_hasInterpolantInput(false),
    m_pResUsage(nullptr)
{
    initializePipelineShadersPass(*PassRegistry::getPassRegistry());
    initializePatchResourceCollectPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
bool PatchResourceCollect::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Resource-Collect\n");

    Patch::Init(&module);

    // Process each shader stage, in reverse order.
    auto pPipelineShaders = &getAnalysis<PipelineShaders>();
    for (int32_t shaderStage = ShaderStageCountInternal - 1; shaderStage >= 0; --shaderStage)
    {
        m_pEntryPoint = pPipelineShaders->GetEntryPoint(static_cast<ShaderStage>(shaderStage));
        if (m_pEntryPoint != nullptr)
        {
            m_shaderStage = static_cast<ShaderStage>(shaderStage);
            ProcessShader();
        }
    }

    if (m_pContext->IsGraphics())
    {
#if LLPC_BUILD_GFX10
        // Set NGG control settings
        m_pContext->SetNggControl();
#endif

        // Determine whether or not GS on-chip mode is valid for this pipeline
        bool hasGs = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageGeometry)) != 0);
#if LLPC_BUILD_GFX10
        bool checkGsOnChip = hasGs || m_pContext->GetNggControl()->enableNgg;
#else
        bool checkGsOnChip = hasGs;
#endif

        if (checkGsOnChip)
        {
            bool gsOnChip = m_pContext->CheckGsOnChipValidity();
            m_pContext->SetGsOnChip(gsOnChip);
        }
    }

    return true;
}

// =====================================================================================================================
// Process a single shader
void PatchResourceCollect::ProcessShader()
{
    m_hasPushConstOp = false;
    m_hasDynIndexedInput = false;
    m_hasDynIndexedOutput = false;
    m_pResUsage = m_pContext->GetShaderResourceUsage(m_shaderStage);

    // Invoke handling of "call" instruction
    visit(m_pEntryPoint);

    // Disable push constant if not used
    if (m_hasPushConstOp == false)
    {
        m_pResUsage->pushConstSizeInBytes = 0;
    }

    ClearInactiveInput();
    ClearInactiveOutput();

    if (m_pContext->IsGraphics())
    {
        // Check if it is a VS-FS pipeline with PackIo option turned on
        if (CheckValidityForInOutPack())
        {
            // Do pack input/output in vertex stage
            if (m_shaderStage == ShaderStageVertex)
            {
                // The description of pack in/out workflow
                // NOTE: Pack in/out performs in VS-FS pipeline by default which is controled by cl::PackIo

                // 1. In lower global pass, split a vector to scalars (see MapInputToProxy(), MapOutputToProxy())
                // We find VS' output call of writing to a vector and FS' input call of reading from a vector.
                // We split these vector calls into scalar calls.

                // 2. In patch resource collect pass, merge scalars to component fully occupied vector
                // 1) Collect pack info (see ProcessCallForInOutPack())
                // InOutPackInfo represents valid pack info which is the key of inputLocMap/outputLocMap.
                // Fill inputLocMap/outputLocMap by collecting pack info from scalar call.

                // 2) Remove unused elements from VS' outputLocMap without mappings in FS' inputLocMap.

                // 3) Core function of packing in/out (see PackGenericInOut())
                // 3.1) Collect pack group info (see PrepareForInOutPack())
                // Scalar calls with the same interpMode, interpLoc and bitWidht will be packed together.
                // Collect PackGroup infos from FS' inputLocMap which is sorted as pack group requirement.
                // Resort VS' output calls based on the key of FS' inputLocMap in order to be grouped same as FS.
                // Add input/output scalar calls to dead call set.

                // 3.2) Create new generic input/output calls (CreateGenericInOut())
                // Input and output calls are grouped and merged following the same rules so that they can build
                // a one-to-one correspondence of locations.
                // For VS, new vector output calls are emitted. The location is the key of outputLocMap, The value of
                // the component index is 0 and the output value is constructed by mergeing multiple scalar values
                // outputLocMap as packed location is updated for a new vector call (CreateGenericOutput()).
                // For FS, new scalar input calls are emitted with new location and component index.
                // The location is the key of inputLocMap, the component index is recomputed according to the merging
                // rule in a pack group.
                // The value of inputLocMap as packed location is updated according to the merging rule
                // (CreateGenericInput()).

                // 3.2) Create new input/output calls for interpolant usage (CreateInterpolantInOut())
                // For VS, the scalar calls corresponding to an interpolant input call are restored to a vector call
                // with the key of outputLocMap as location and 0 as component index.
                // The value of outputLocMap as packed location is updated for a new call.
                // For FS, the scalar call is recreated with the key of inputLocMap as location.
                // The value of inputLocMap as packed location is updated for a new call with non-processed location.

                MatchGenericInOutWithPack();

                // Map FS built-in then VS built-in
                MapBuiltInToGenericInOutWithPack();
            }
        }
        else
        {
            MatchGenericInOut();
            MapBuiltInToGenericInOut();
        }
    }

    if ((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval))
    {
        ReviseTessExecutionMode();
    }
    else if (m_shaderStage == ShaderStageFragment)
    {
        if (m_pResUsage->builtInUsage.fs.fragCoord || m_pResUsage->builtInUsage.fs.sampleMaskIn)
        {
            const GraphicsPipelineBuildInfo* pPipelineInfo =
                reinterpret_cast<const GraphicsPipelineBuildInfo*>(m_pContext->GetPipelineBuildInfo());
            if (pPipelineInfo->rsState.perSampleShading)
            {
                m_pResUsage->builtInUsage.fs.runAtSampleRate = true;
            }
        }
    }
    else if (m_shaderStage == ShaderStageVertex)
    {
        // Collect resource usages from vertex input create info
        auto pPipelineInfo = static_cast<const GraphicsPipelineBuildInfo*>(m_pContext->GetPipelineBuildInfo());
        auto pVertexInput = pPipelineInfo->pVertexInput;

        // TODO: In the future, we might check if the corresponding vertex attribute is active in vertex shader
        // and set the usage based on this info.
        if (pVertexInput != nullptr)
        {
            for (uint32_t i = 0; i < pVertexInput->vertexBindingDescriptionCount; ++i)
            {
                auto pBinding = &pVertexInput->pVertexBindingDescriptions[i];
                if (pBinding->inputRate == VK_VERTEX_INPUT_RATE_VERTEX)
                {
                    m_pResUsage->builtInUsage.vs.vertexIndex = true;
                    m_pResUsage->builtInUsage.vs.baseVertex = true;
                }
                else
                {
                    //LLPC_ASSERT(pBinding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE);
                    m_pResUsage->builtInUsage.vs.instanceIndex = true;
                    m_pResUsage->builtInUsage.vs.baseInstance = true;
                }
            }
        }
    }

    // Remove dead calls
    for (auto pCall : m_deadCalls)
    {
        LLPC_ASSERT(pCall->user_empty());
        pCall->dropAllReferences();
        pCall->eraseFromParent();
    }
    m_deadCalls.clear();
}

// =====================================================================================================================
// Visits "call" instruction.
void PatchResourceCollect::visitCallInst(
    CallInst& callInst) // [in] "Call" instruction
{
    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

    bool isDeadCall = callInst.user_empty();

    auto mangledName = pCallee->getName();

    if (mangledName.startswith(LlpcName::PushConstLoad) ||
        mangledName.startswith(LlpcName::DescriptorLoadSpillTable))
    {
        // Push constant operations
        if (isDeadCall)
        {
            m_deadCalls.insert(&callInst);
        }
        else
        {
            m_hasPushConstOp = true;
        }
    }
    else if (mangledName.startswith(LlpcName::DescriptorLoadBuffer) ||
             mangledName.startswith(LlpcName::DescriptorGetTexelBufferPtr) ||
             mangledName.startswith(LlpcName::DescriptorGetResourcePtr) ||
             mangledName.startswith(LlpcName::DescriptorGetFmaskPtr) ||
             mangledName.startswith(LlpcName::DescriptorGetSamplerPtr))
    {
        uint32_t descSet = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
        uint32_t binding = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
        DescriptorPair descPair = { descSet, binding };
        m_pResUsage->descPairs.insert(descPair.u64All);
    }
    else if (mangledName.startswith(LlpcName::BufferLoad))
    {
        if (isDeadCall)
        {
            m_deadCalls.insert(&callInst);
        }
    }
    else if (mangledName.startswith(LlpcName::InputImportGeneric))
    {
        // Generic input import
        if (isDeadCall)
        {
            m_deadCalls.insert(&callInst);
        }
        else
        {
            auto pInputTy = callInst.getType();
            LLPC_ASSERT(pInputTy->isSingleValueType());

            auto loc = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();

            if ((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval))
            {
                auto pLocOffset = callInst.getOperand(1);
                auto pCompIdx = callInst.getOperand(2);

                if (isa<ConstantInt>(pLocOffset))
                {
                    // Location offset is constant
                    auto locOffset = cast<ConstantInt>(pLocOffset)->getZExtValue();
                    loc += locOffset;

                    auto bitWidth = pInputTy->getScalarSizeInBits();
                    if (bitWidth == 64)
                    {
                        if (isa<ConstantInt>(pCompIdx))
                        {
                            auto compIdx = cast<ConstantInt>(pCompIdx)->getZExtValue();

                            m_activeInputLocs.insert(loc);
                            if (compIdx >= 2)
                            {
                                // NOTE: For the addressing of .z/.w component of 64-bit vector/scalar, the count of
                                // occupied locations are two.
                                m_activeInputLocs.insert(loc + 1);
                            }
                        }
                        else
                        {
                            // NOTE: If vector component index is not constant, we treat this as dynamic indexing.
                            m_hasDynIndexedInput = true;
                        }
                    }
                    else
                    {
                        // NOTE: For non 64-bit vector/scalar, one location is sufficient regardless of vector component
                        // addressing.
                        LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32));
                        m_activeInputLocs.insert(loc);
                    }
                }
                else
                {
                    // NOTE: If location offset is not constant, we treat this as dynamic indexing.
                    m_hasDynIndexedInput = true;
                }
            }
            else
            {
                // Currently support to pack for VS export and FS import
                if (m_pContext->CheckPackInOutValidity(m_shaderStage, false))
                {
                    ProcessCallForInOutPack(&callInst);
                }
                else
                {
                    m_activeInputLocs.insert(loc);
                    if (pInputTy->getPrimitiveSizeInBits() > (8 * SizeOfVec4))
                    {
                        LLPC_ASSERT(pInputTy->getPrimitiveSizeInBits() <= (8 * 2 * SizeOfVec4));
                        m_activeInputLocs.insert(loc + 1);
                    }
                }
            }
        }
    }
    else if (mangledName.startswith(LlpcName::InputImportInterpolant))
    {
        // Interpolant input import
        LLPC_ASSERT(m_shaderStage == ShaderStageFragment);

        if (isDeadCall)
        {
            m_deadCalls.insert(&callInst);
        }
        else
        {
            LLPC_ASSERT(callInst.getType()->isSingleValueType());

            // Currently support to pack for VS export and FS import
            if (m_pContext->CheckPackInOutValidity(m_shaderStage, false))
            {
                ProcessCallForInOutPack(&callInst);
            }
            else
            {
                auto pLocOffset = callInst.getOperand(1);
                if (isa<ConstantInt>(pLocOffset))
                {
                    // Location offset is constant
                    auto loc = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
                    auto locOffset = cast<ConstantInt>(pLocOffset)->getZExtValue();
                    loc += locOffset;

                    LLPC_ASSERT(callInst.getType()->getPrimitiveSizeInBits() <= (8 * SizeOfVec4));
                    m_activeInputLocs.insert(loc);
                }
                else
                {
                    // NOTE: If location offset is not constant, we consider dynamic indexing occurs.
                    m_hasDynIndexedInput = true;
                }
            }
        }
    }
    else if (mangledName.startswith(LlpcName::InputImportBuiltIn))
    {
        // Built-in input import
        if (isDeadCall)
        {
            m_deadCalls.insert(&callInst);
        }
        else
        {
            uint32_t builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
            m_activeInputBuiltIns.insert(builtInId);
        }
    }
    else if (mangledName.startswith(LlpcName::OutputImportGeneric))
    {
        // Generic output import
        LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);

        auto pOutputTy = callInst.getType();
        LLPC_ASSERT(pOutputTy->isSingleValueType());

        auto loc = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
        auto pLocOffset = callInst.getOperand(1);
        auto pCompIdx = callInst.getOperand(2);

        if (isa<ConstantInt>(pLocOffset))
        {
            // Location offset is constant
            auto locOffset = cast<ConstantInt>(pLocOffset)->getZExtValue();
            loc += locOffset;

            auto bitWidth = pOutputTy->getScalarSizeInBits();
            if (bitWidth == 64)
            {
                if (isa<ConstantInt>(pCompIdx))
                {
                    auto compIdx = cast<ConstantInt>(pCompIdx)->getZExtValue();

                    m_importedOutputLocs.insert(loc);
                    if (compIdx >= 2)
                    {
                        // NOTE: For the addressing of .z/.w component of 64-bit vector/scalar, the count of
                        // occupied locations are two.
                        m_importedOutputLocs.insert(loc + 1);
                    }
                }
                else
                {
                    // NOTE: If vector component index is not constant, we treat this as dynamic indexing.
                    m_hasDynIndexedOutput = true;
                }
            }
            else
            {
                // NOTE: For non 64-bit vector/scalar, one location is sufficient regardless of vector component
                // addressing.
                LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32));
                m_importedOutputLocs.insert(loc);
            }
        }
        else
        {
            // NOTE: If location offset is not constant, we treat this as dynamic indexing.
            m_hasDynIndexedOutput = true;
        }
    }
    else if (mangledName.startswith(LlpcName::OutputImportBuiltIn))
    {
        // Built-in output import
        LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);

        uint32_t builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
        m_importedOutputBuiltIns.insert(builtInId);
    }
    else if (mangledName.startswith(LlpcName::OutputExportGeneric))
    {
        // Generic output export
        if (m_shaderStage == ShaderStageTessControl)
        {
            auto pOutput = callInst.getOperand(callInst.getNumArgOperands() - 1);
            auto pOutputTy = pOutput->getType();
            LLPC_ASSERT(pOutputTy->isSingleValueType());

            auto pLocOffset = callInst.getOperand(1);
            auto pCompIdx = callInst.getOperand(2);

            if (isa<ConstantInt>(pLocOffset))
            {
                // Location offset is constant
                auto bitWidth = pOutputTy->getScalarSizeInBits();
                if ((bitWidth == 64) && (isa<ConstantInt>(pCompIdx) == false))
                {
                    // NOTE: If vector component index is not constant and it is vector component addressing for
                    // 64-bit vector, we treat this as dynamic indexing.
                    m_hasDynIndexedOutput = true;
                }
            }
            else
            {
                // NOTE: If location offset is not constant, we consider dynamic indexing occurs.
                m_hasDynIndexedOutput = true;
            }
        }

        // Currently support to pack for VS export and FS import
        if (m_pContext->CheckPackInOutValidity(m_shaderStage, true))
        {
            ProcessCallForInOutPack(&callInst);
        }
    }
    else if (mangledName.startswith(LlpcName::OutputExportBuiltIn))
    {
        // NOTE: If output value is undefined one, we can safely drop it and remove the output export call.
        // Currently, do this for geometry shader.
        if (m_shaderStage == ShaderStageGeometry)
        {
            auto* pOutputValue = callInst.getArgOperand(callInst.getNumArgOperands() - 1);
            if (isa<UndefValue>(pOutputValue))
            {
                m_deadCalls.insert(&callInst);
            }
            else
            {
                uint32_t builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
                m_activeOutputBuiltIns.insert(builtInId);
            }
        }
    }
}

// =====================================================================================================================
// Clears inactive (those actually unused) inputs.
void PatchResourceCollect::ClearInactiveInput()
{
    // Clear those inactive generic inputs, remove them from location mappings
    if (m_pContext->IsGraphics() && (m_hasDynIndexedInput == false) && (m_shaderStage != ShaderStageTessEval))
    {
        // TODO: Here, we keep all generic inputs of tessellation evaluation shader. This is because corresponding
        // generic outputs of tessellation control shader might involve in output import and dynamic indexing, which
        // is easy to cause incorrectness of location mapping.

        // Clear normal inputs
        std::unordered_set<uint32_t> unusedLocs;
        for (auto locMap : m_pResUsage->inOutUsage.inputLocMap)
        {
            uint32_t loc = locMap.first;
            if (m_activeInputLocs.find(loc) == m_activeInputLocs.end())
            {
                 unusedLocs.insert(loc);
            }
        }

        for (auto loc : unusedLocs)
        {
            m_pResUsage->inOutUsage.inputLocMap.erase(loc);
        }

        // Clear per-patch inputs
        if (m_shaderStage == ShaderStageTessEval)
        {
            unusedLocs.clear();
            for (auto locMap : m_pResUsage->inOutUsage.perPatchInputLocMap)
            {
                uint32_t loc = locMap.first;
                if (m_activeInputLocs.find(loc) == m_activeInputLocs.end())
                {
                     unusedLocs.insert(loc);
                }
            }

            for (auto loc : unusedLocs)
            {
                m_pResUsage->inOutUsage.perPatchInputLocMap.erase(loc);
            }
        }
        else
        {
            // For other stages, must be empty
            LLPC_ASSERT(m_pResUsage->inOutUsage.perPatchInputLocMap.empty());
        }
    }

    // Clear those inactive built-in inputs (some are not checked, whose usage flags do not rely on their
    // actual uses)
    auto& builtInUsage = m_pResUsage->builtInUsage;

    // Check per-stage built-in usage
    if (m_shaderStage == ShaderStageVertex)
    {
        if (builtInUsage.vs.drawIndex &&
            (m_activeInputBuiltIns.find(BuiltInDrawIndex) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.vs.drawIndex = false;
        }
    }
    else if (m_shaderStage == ShaderStageTessControl)
    {
        if (builtInUsage.tcs.pointSizeIn &&
            (m_activeInputBuiltIns.find(BuiltInPointSize) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.pointSizeIn = false;
        }

        if (builtInUsage.tcs.positionIn &&
            (m_activeInputBuiltIns.find(BuiltInPosition) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.positionIn = false;
        }

        if ((builtInUsage.tcs.clipDistanceIn > 0) &&
            (m_activeInputBuiltIns.find(BuiltInClipDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.clipDistanceIn = 0;
        }

        if ((builtInUsage.tcs.cullDistanceIn > 0) &&
            (m_activeInputBuiltIns.find(BuiltInCullDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.cullDistanceIn = 0;
        }

        if (builtInUsage.tcs.patchVertices &&
            (m_activeInputBuiltIns.find(BuiltInPatchVertices) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.patchVertices = false;
        }

        if (builtInUsage.tcs.primitiveId &&
            (m_activeInputBuiltIns.find(BuiltInPrimitiveId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.primitiveId = false;
        }

        if (builtInUsage.tcs.invocationId &&
            (m_activeInputBuiltIns.find(BuiltInInvocationId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.invocationId = false;
        }
    }
    else if (m_shaderStage == ShaderStageTessEval)
    {
        if (builtInUsage.tes.pointSizeIn &&
            (m_activeInputBuiltIns.find(BuiltInPointSize) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.pointSizeIn = false;
        }

        if (builtInUsage.tes.positionIn &&
            (m_activeInputBuiltIns.find(BuiltInPosition) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.positionIn = false;
        }

        if ((builtInUsage.tes.clipDistanceIn > 0) &&
            (m_activeInputBuiltIns.find(BuiltInClipDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.clipDistanceIn = 0;
        }

        if ((builtInUsage.tes.cullDistanceIn > 0) &&
            (m_activeInputBuiltIns.find(BuiltInCullDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.cullDistanceIn = 0;
        }

        if (builtInUsage.tes.patchVertices &&
            (m_activeInputBuiltIns.find(BuiltInPatchVertices) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.patchVertices = false;
        }

        if (builtInUsage.tes.primitiveId &&
            (m_activeInputBuiltIns.find(BuiltInPrimitiveId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.primitiveId = false;
        }

        if (builtInUsage.tes.tessCoord &&
            (m_activeInputBuiltIns.find(BuiltInTessCoord) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.tessCoord = false;
        }

        if (builtInUsage.tes.tessLevelOuter &&
            (m_activeInputBuiltIns.find(BuiltInTessLevelOuter) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.tessLevelOuter = false;
        }

        if (builtInUsage.tes.tessLevelInner &&
            (m_activeInputBuiltIns.find(BuiltInTessLevelInner) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.tessLevelInner = false;
        }
    }
    else if (m_shaderStage == ShaderStageGeometry)
    {
        if (builtInUsage.gs.pointSizeIn &&
            (m_activeInputBuiltIns.find(BuiltInPointSize) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.gs.pointSizeIn = false;
        }

        if (builtInUsage.gs.positionIn &&
            (m_activeInputBuiltIns.find(BuiltInPosition) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.gs.positionIn = false;
        }

        if ((builtInUsage.gs.clipDistanceIn > 0) &&
            (m_activeInputBuiltIns.find(BuiltInClipDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.gs.clipDistanceIn = 0;
        }

        if ((builtInUsage.gs.cullDistanceIn > 0) &&
            (m_activeInputBuiltIns.find(BuiltInCullDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.gs.cullDistanceIn = 0;
        }

        if (builtInUsage.gs.primitiveIdIn &&
            (m_activeInputBuiltIns.find(BuiltInPrimitiveId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.gs.primitiveIdIn = false;
        }

        if (builtInUsage.gs.invocationId &&
            (m_activeInputBuiltIns.find(BuiltInInvocationId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.gs.invocationId = false;
        }
    }
    else if (m_shaderStage == ShaderStageFragment)
    {
        if (builtInUsage.fs.fragCoord &&
            (m_activeInputBuiltIns.find(BuiltInFragCoord) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.fragCoord = false;
        }

        if (builtInUsage.fs.frontFacing &&
            (m_activeInputBuiltIns.find(BuiltInFrontFacing) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.frontFacing = false;
        }

        if (builtInUsage.fs.fragCoord &&
            (m_activeInputBuiltIns.find(BuiltInFragCoord) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.fragCoord = false;
        }

        if ((builtInUsage.fs.clipDistance > 0) &&
            (m_activeInputBuiltIns.find(BuiltInClipDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.clipDistance = 0;
        }

        if ((builtInUsage.fs.cullDistance > 0) &&
            (m_activeInputBuiltIns.find(BuiltInCullDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.cullDistance = 0;
        }

        if (builtInUsage.fs.pointCoord &&
            (m_activeInputBuiltIns.find(BuiltInPointCoord) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.pointCoord = false;
        }

        if (builtInUsage.fs.primitiveId &&
            (m_activeInputBuiltIns.find(BuiltInPrimitiveId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.primitiveId = false;
        }

        if (builtInUsage.fs.sampleId &&
            (m_activeInputBuiltIns.find(BuiltInSampleId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.sampleId = false;
        }

        if (builtInUsage.fs.samplePosition &&
            (m_activeInputBuiltIns.find(BuiltInSamplePosition) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.samplePosition = false;
        }

        if (builtInUsage.fs.sampleMaskIn &&
            (m_activeInputBuiltIns.find(BuiltInSampleMask) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.sampleMaskIn = false;
        }

        if (builtInUsage.fs.layer &&
            (m_activeInputBuiltIns.find(BuiltInLayer) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.layer = false;
        }

        if (builtInUsage.fs.viewIndex &&
            (m_activeInputBuiltIns.find(BuiltInViewIndex) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.viewIndex = false;
        }

        if (builtInUsage.fs.viewportIndex &&
            (m_activeInputBuiltIns.find(BuiltInViewportIndex) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.viewportIndex = false;
        }

        if (builtInUsage.fs.helperInvocation &&
            (m_activeInputBuiltIns.find(BuiltInHelperInvocation) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.helperInvocation = false;
        }

        if (builtInUsage.fs.baryCoordNoPersp &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordNoPerspAMD) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordNoPersp = false;
        }

        if (builtInUsage.fs.baryCoordNoPerspCentroid &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordNoPerspCentroidAMD) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordNoPerspCentroid = false;
        }

        if (builtInUsage.fs.baryCoordNoPerspSample &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordNoPerspSampleAMD) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordNoPerspSample = false;
        }

        if (builtInUsage.fs.baryCoordSmooth &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordSmoothAMD) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordSmooth = false;
        }

        if (builtInUsage.fs.baryCoordSmoothCentroid &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordSmoothCentroidAMD) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordSmoothCentroid = false;
        }

        if (builtInUsage.fs.baryCoordSmoothSample &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordSmoothSampleAMD) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordNoPerspSample = false;
        }

        if (builtInUsage.fs.baryCoordPullModel &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordPullModelAMD) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordPullModel = false;
        }
    }
    else if (m_shaderStage == ShaderStageCompute)
    {
        if (builtInUsage.cs.numWorkgroups &&
            (m_activeInputBuiltIns.find(BuiltInNumWorkgroups) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.cs.numWorkgroups = false;
        }

        if (builtInUsage.cs.localInvocationId &&
            ((m_activeInputBuiltIns.find(BuiltInLocalInvocationId) == m_activeInputBuiltIns.end()) &&
                (m_activeInputBuiltIns.find(BuiltInGlobalInvocationId) == m_activeInputBuiltIns.end()) &&
                (m_activeInputBuiltIns.find(BuiltInLocalInvocationIndex) == m_activeInputBuiltIns.end()) &&
                (m_activeInputBuiltIns.find(BuiltInSubgroupId) == m_activeInputBuiltIns.end())))
        {
            builtInUsage.cs.localInvocationId = false;
        }

        if (builtInUsage.cs.workgroupId &&
            ((m_activeInputBuiltIns.find(BuiltInWorkgroupId) == m_activeInputBuiltIns.end()) &&
                (m_activeInputBuiltIns.find(BuiltInGlobalInvocationId) == m_activeInputBuiltIns.end()) &&
                (m_activeInputBuiltIns.find(BuiltInLocalInvocationIndex) == m_activeInputBuiltIns.end()) &&
                (m_activeInputBuiltIns.find(BuiltInSubgroupId) == m_activeInputBuiltIns.end())))
        {
            builtInUsage.cs.workgroupId = false;
        }

        if (builtInUsage.cs.subgroupId &&
            (m_activeInputBuiltIns.find(BuiltInSubgroupId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.cs.subgroupId = false;
        }

        if (builtInUsage.cs.numSubgroups &&
            (m_activeInputBuiltIns.find(BuiltInNumSubgroups) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.cs.numSubgroups = false;
        }
    }

    // Check common built-in usage
    if (builtInUsage.common.subgroupSize &&
        ((m_activeInputBuiltIns.find(BuiltInSubgroupSize) == m_activeInputBuiltIns.end()) &&
            (m_activeInputBuiltIns.find(BuiltInNumSubgroups) == m_activeInputBuiltIns.end()) &&
            (m_activeInputBuiltIns.find(BuiltInSubgroupId) == m_activeInputBuiltIns.end())))
    {
        builtInUsage.common.subgroupSize = false;
    }

    if (builtInUsage.common.subgroupLocalInvocationId &&
        (m_activeInputBuiltIns.find(BuiltInSubgroupLocalInvocationId) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.subgroupLocalInvocationId = false;
    }

    if (builtInUsage.common.subgroupEqMask &&
        (m_activeInputBuiltIns.find(BuiltInSubgroupEqMaskKHR) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.subgroupEqMask = false;
    }

    if (builtInUsage.common.subgroupGeMask &&
        (m_activeInputBuiltIns.find(BuiltInSubgroupGeMaskKHR) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.subgroupGeMask = false;
    }

    if (builtInUsage.common.subgroupGtMask &&
        (m_activeInputBuiltIns.find(BuiltInSubgroupGtMaskKHR) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.subgroupGtMask = false;
    }

    if (builtInUsage.common.subgroupLeMask &&
        (m_activeInputBuiltIns.find(BuiltInSubgroupLeMaskKHR) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.subgroupLeMask = false;
    }

    if (builtInUsage.common.subgroupLtMask &&
        (m_activeInputBuiltIns.find(BuiltInSubgroupLtMaskKHR) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.subgroupLtMask = false;
    }

    if (builtInUsage.common.deviceIndex &&
        (m_activeInputBuiltIns.find(BuiltInDeviceIndex) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.deviceIndex = false;
    }
}

// =====================================================================================================================
// Clears inactive (those actually unused) outputs.
void PatchResourceCollect::ClearInactiveOutput()
{
    // Clear inactive output builtins
    if (m_shaderStage == ShaderStageGeometry)
    {
        auto& builtInUsage = m_pResUsage->builtInUsage.gs;

        if (builtInUsage.position &&
            (m_activeOutputBuiltIns.find(BuiltInPosition) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.position = false;
        }

        if (builtInUsage.pointSize &&
            (m_activeOutputBuiltIns.find(BuiltInPointSize) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.pointSize = false;
        }

        if (builtInUsage.clipDistance &&
            (m_activeOutputBuiltIns.find(BuiltInClipDistance) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.clipDistance = false;
        }

        if (builtInUsage.cullDistance &&
            (m_activeOutputBuiltIns.find(BuiltInCullDistance) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.cullDistance = false;
        }

        if (builtInUsage.primitiveId &&
            (m_activeOutputBuiltIns.find(BuiltInPrimitiveId) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.primitiveId = false;
        }

        if (builtInUsage.layer &&
            (m_activeOutputBuiltIns.find(BuiltInLayer) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.layer = false;
        }

        if (builtInUsage.viewportIndex &&
            (m_activeOutputBuiltIns.find(BuiltInViewportIndex) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.viewportIndex = false;
        }
    }
}

// =====================================================================================================================
// Does generic input/output matching and does location mapping afterwards.
//
// NOTE: This function should be called after the cleanup work of inactive inputs is done.
void PatchResourceCollect::MatchGenericInOut()
{
    LLPC_ASSERT(m_pContext->IsGraphics());
    auto& inOutUsage = m_pContext->GetShaderResourceUsage(m_shaderStage)->inOutUsage;

    auto& inLocMap  = inOutUsage.inputLocMap;
    auto& outLocMap = inOutUsage.outputLocMap;

    auto& perPatchInLocMap  = inOutUsage.perPatchInputLocMap;
    auto& perPatchOutLocMap = inOutUsage.perPatchOutputLocMap;

    // Do input/output matching
    if (m_shaderStage != ShaderStageFragment)
    {
        const auto nextStage = m_pContext->GetNextShaderStage(m_shaderStage);

        // Do normal input/output matching
        if (nextStage != ShaderStageInvalid)
        {
            const auto pNextResUsage = m_pContext->GetShaderResourceUsage(nextStage);
            const auto& nextInLocMap = pNextResUsage->inOutUsage.inputLocMap;

            uint32_t availInMapLoc = pNextResUsage->inOutUsage.inputMapLocCount;

            // Collect locations of those outputs that are not used by next shader stage
            std::vector<uint32_t> unusedLocs;
            for (auto& locMap : outLocMap)
            {
                uint32_t loc = locMap.first;
                bool outputXfb = false;
                if (m_shaderStage == ShaderStageGeometry)
                {
                    uint32_t outLocInfo = locMap.first;
                    loc = reinterpret_cast<GsOutLocInfo*>(&outLocInfo)->location;
                    outputXfb = inOutUsage.gs.xfbOutsInfo.find(outLocInfo) != inOutUsage.gs.xfbOutsInfo.end();
                }

                if ((nextInLocMap.find(loc) == nextInLocMap.end()) && (outputXfb == false))
                {
                    if (m_hasDynIndexedOutput || (m_importedOutputLocs.find(loc) != m_importedOutputLocs.end()))
                    {
                        // NOTE: If either dynamic indexing of generic outputs exists or the generic output involve in
                        // output import, we have to mark it as active. The assigned location must not overlap with
                        // those used by inputs of next shader stage.
                        LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);
                        locMap.second = availInMapLoc++;
                    }
                    else
                    {
                        unusedLocs.push_back(loc);
                    }
                }
            }

            // Remove those collected locations
            for (auto loc : unusedLocs)
            {
                outLocMap.erase(loc);
            }
        }

        // Do per-patch input/output matching
        if (m_shaderStage == ShaderStageTessControl)
        {
            if (nextStage != ShaderStageInvalid)
            {
                const auto pNextResUsage = m_pContext->GetShaderResourceUsage(nextStage);
                const auto& nextPerPatchInLocMap = pNextResUsage->inOutUsage.perPatchInputLocMap;

                uint32_t availPerPatchInMapLoc = pNextResUsage->inOutUsage.perPatchInputMapLocCount;

                // Collect locations of those outputs that are not used by next shader stage
                std::vector<uint32_t> unusedLocs;
                for (auto& locMap : perPatchOutLocMap)
                {
                    const uint32_t loc = locMap.first;
                    if (nextPerPatchInLocMap.find(loc) == nextPerPatchInLocMap.end())
                    {
                        // NOTE: If either dynamic indexing of generic outputs exists or the generic output involve in
                        // output import, we have to mark it as active. The assigned location must not overlap with
                        // those used by inputs of next shader stage.
                        if (m_hasDynIndexedOutput || (m_importedOutputLocs.find(loc) != m_importedOutputLocs.end()))
                        {
                            LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);
                            locMap.second = availPerPatchInMapLoc++;
                        }
                        else
                        {
                            unusedLocs.push_back(loc);
                        }
                    }
                }

                // Remove those collected locations
                for (auto loc : unusedLocs)
                {
                    perPatchOutLocMap.erase(loc);
                }
            }
        }
        else
        {
            // For other stages, must be empty
            LLPC_ASSERT(perPatchOutLocMap.empty());
        }
    }

    // Do location mapping
    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC location input/output mapping results (" << GetShaderStageName(m_shaderStage)
              << " shader)\n\n");
    uint32_t nextMapLoc = 0;
    if (inLocMap.empty() == false)
    {
        LLPC_ASSERT(inOutUsage.inputMapLocCount == 0);
        for (auto& locMap : inLocMap)
        {
            LLPC_ASSERT(locMap.second == InvalidValue);
            // NOTE: For vertex shader, the input location mapping is actually trivial.
            locMap.second = (m_shaderStage == ShaderStageVertex) ? locMap.first : nextMapLoc++;
            inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, locMap.second + 1);
            LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input:  loc = "
                          << locMap.first << "  =>  Mapped = " << locMap.second << "\n");
        }
        LLPC_OUTS("\n");
    }

    if (outLocMap.empty() == false)
    {
        auto& outOrigLocs = inOutUsage.fs.outputOrigLocs;
        auto pPipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo*>(m_pContext->GetPipelineBuildInfo());
        if (m_shaderStage == ShaderStageFragment)
        {
            memset(&outOrigLocs, InvalidValue, sizeof(inOutUsage.fs.outputOrigLocs));
        }

        nextMapLoc = 0;
        LLPC_ASSERT(inOutUsage.outputMapLocCount == 0);
        for (auto locMapIt = outLocMap.begin(); locMapIt != outLocMap.end();)
        {
            auto& locMap = *locMapIt;
            if (m_shaderStage == ShaderStageFragment)
            {
                uint32_t location = locMap.first;
                if (pPipelineInfo->cbState.dualSourceBlendEnable && (location == 1))
                {
                    location = 0;
                }
                if (pPipelineInfo->cbState.target[location].format == VK_FORMAT_UNDEFINED)
                {
                    locMapIt = outLocMap.erase(locMapIt);
                    continue;
                }
            }

            if (m_shaderStage == ShaderStageGeometry)
            {
                if (locMap.second == InvalidValue)
                {
                    uint32_t outLocInfo = locMap.first;
                    MapGsGenericOutput(*(reinterpret_cast<GsOutLocInfo*>(&outLocInfo)));
                }
            }
            else
            {
                if (locMap.second == InvalidValue)
                {
                    // Only do location mapping if the output has not been mapped
                    locMap.second = nextMapLoc++;
                }
                else
                {
                    LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);
                }
                inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, locMap.second + 1);
                LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output: loc = "
                    << locMap.first << "  =>  Mapped = " << locMap.second << "\n");

                if (m_shaderStage == ShaderStageFragment)
                {
                    outOrigLocs[locMap.second] = locMap.first;
                }
            }

            ++locMapIt;
        }
        LLPC_OUTS("\n");
    }

    if (perPatchInLocMap.empty() == false)
    {
        nextMapLoc = 0;
        LLPC_ASSERT(inOutUsage.perPatchInputMapLocCount == 0);
        for (auto& locMap : perPatchInLocMap)
        {
            LLPC_ASSERT(locMap.second == InvalidValue);
            locMap.second = nextMapLoc++;
            inOutUsage.perPatchInputMapLocCount = std::max(inOutUsage.perPatchInputMapLocCount, locMap.second + 1);
            LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input (per-patch):  loc = "
                          << locMap.first << "  =>  Mapped = " << locMap.second << "\n");
        }
        LLPC_OUTS("\n");
    }

    if (perPatchOutLocMap.empty() == false)
    {
        nextMapLoc = 0;
        LLPC_ASSERT(inOutUsage.perPatchOutputMapLocCount == 0);
        for (auto& locMap : perPatchOutLocMap)
        {
            if (locMap.second == InvalidValue)
            {
                // Only do location mapping if the per-patch output has not been mapped
                locMap.second = nextMapLoc++;
            }
            else
            {
                LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);
            }
            inOutUsage.perPatchOutputMapLocCount = std::max(inOutUsage.perPatchOutputMapLocCount, locMap.second + 1);
            LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output (per-patch): loc = "
                          << locMap.first << "  =>  Mapped = " << locMap.second << "\n");
        }
        LLPC_OUTS("\n");
    }

    LLPC_OUTS("// LLPC location count results (after input/output matching) \n\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input:  loc count = "
                  << inOutUsage.inputMapLocCount << "\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output: loc count = "
                  << inOutUsage.outputMapLocCount << "\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input (per-patch):  loc count = "
                  << inOutUsage.perPatchInputMapLocCount << "\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output (per-patch): loc count = "
                  << inOutUsage.perPatchOutputMapLocCount << "\n");
    LLPC_OUTS("\n");
}

// =====================================================================================================================
// Maps special built-in input/output to generic ones.
//
// NOTE: This function should be called after generic input/output matching is done.
void PatchResourceCollect::MapBuiltInToGenericInOut()
{
    LLPC_ASSERT(m_pContext->IsGraphics());

    const auto pResUsage = m_pContext->GetShaderResourceUsage(m_shaderStage);

    auto& builtInUsage = pResUsage->builtInUsage;
    auto& inOutUsage = pResUsage->inOutUsage;

    const auto nextStage = m_pContext->GetNextShaderStage(m_shaderStage);
    auto pNextResUsage =
        (nextStage != ShaderStageInvalid) ? m_pContext->GetShaderResourceUsage(nextStage) : nullptr;

    LLPC_ASSERT(inOutUsage.builtInInputLocMap.empty()); // Should be empty
    LLPC_ASSERT(inOutUsage.builtInOutputLocMap.empty());

    // NOTE: The rules of mapping built-ins to generic inputs/outputs are as follow:
    //       (1) For built-in outputs, if next shader stager is valid and has corresponding built-in input used,
    //           get the mapped location from next shader stage inout usage and use it. If next shader stage
    //           is absent or it does not have such input used, we allocate the mapped location.
    //       (2) For built-on inputs, we always allocate the mapped location based its actual usage.
    if (m_shaderStage == ShaderStageVertex)
    {
        // VS  ==>  XXX
        uint32_t availOutMapLoc = inOutUsage.outputMapLocCount;

        // Map built-in outputs to generic ones
        if (nextStage == ShaderStageFragment)
        {
            // VS  ==>  FS
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.fs;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            if (nextBuiltInUsage.clipDistance > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
            }

            if (nextBuiltInUsage.cullDistance > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
            }

            if (nextBuiltInUsage.primitiveId)
            {
                // NOTE: The usage flag of gl_PrimitiveID must be set if fragment shader uses it.
                builtInUsage.vs.primitiveId = true;

                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPrimitiveId) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPrimitiveId];
                inOutUsage.builtInOutputLocMap[BuiltInPrimitiveId] = mapLoc;
            }

            if (nextBuiltInUsage.layer)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
                inOutUsage.builtInOutputLocMap[BuiltInLayer] = mapLoc;
            }

            if (nextBuiltInUsage.viewIndex)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewIndex) !=
                    nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewIndex];
                inOutUsage.builtInOutputLocMap[BuiltInViewIndex] = mapLoc;
            }

            if (nextBuiltInUsage.viewportIndex)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
                inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = mapLoc;
            }
        }
        else if (nextStage == ShaderStageTessControl)
        {
            // VS  ==>  TCS
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.tcs;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            if (nextBuiltInUsage.positionIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPosition) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPosition];
                inOutUsage.builtInOutputLocMap[BuiltInPosition] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                builtInUsage.vs.position = false;
            }

            if (nextBuiltInUsage.pointSizeIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
                inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                builtInUsage.vs.pointSize = false;
            }

            if (nextBuiltInUsage.clipDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.clipDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                builtInUsage.vs.clipDistance = 0;
            }

            if (nextBuiltInUsage.cullDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.cullDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                builtInUsage.vs.cullDistance = 0;
            }

            builtInUsage.vs.layer = false;
            builtInUsage.vs.viewportIndex = false;
        }
        else if (nextStage == ShaderStageGeometry)
        {
            // VS  ==>  GS
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.gs;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            if (nextBuiltInUsage.positionIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPosition) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPosition];
                inOutUsage.builtInOutputLocMap[BuiltInPosition] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                builtInUsage.vs.position = false;
            }

            if (nextBuiltInUsage.pointSizeIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
                inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                builtInUsage.vs.pointSize = false;
            }

            if (nextBuiltInUsage.clipDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.clipDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                builtInUsage.vs.clipDistance = 0;
            }

            if (nextBuiltInUsage.cullDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.cullDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                builtInUsage.vs.cullDistance = 0;
            }

            builtInUsage.vs.layer = false;
            builtInUsage.vs.viewportIndex = false;
        }
        else if (nextStage == ShaderStageInvalid)
        {
            // VS only
            if ((builtInUsage.vs.clipDistance > 0) || (builtInUsage.vs.cullDistance > 0))
            {
                uint32_t mapLoc = availOutMapLoc++;
                if (builtInUsage.vs.clipDistance + builtInUsage.vs.cullDistance > 4)
                {
                    LLPC_ASSERT(builtInUsage.vs.clipDistance +
                                builtInUsage.vs.cullDistance <= MaxClipCullDistanceCount);
                    ++availOutMapLoc; // Occupy two locations
                }

                if (builtInUsage.vs.clipDistance > 0)
                {
                    inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
                }

                if (builtInUsage.vs.cullDistance > 0)
                {
                    if (builtInUsage.vs.clipDistance >= 4)
                    {
                        ++mapLoc;
                    }
                    inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
                }
            }

            if (builtInUsage.vs.viewportIndex)
            {
                inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = availOutMapLoc++;
            }

            if (builtInUsage.vs.layer)
            {
                inOutUsage.builtInOutputLocMap[BuiltInLayer] = availOutMapLoc++;
            }

            if (builtInUsage.vs.viewIndex)
            {
                inOutUsage.builtInOutputLocMap[BuiltInViewIndex] = availOutMapLoc++;
            }
        }

        inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, availOutMapLoc);
    }
    else if (m_shaderStage == ShaderStageTessControl)
    {
        // TCS  ==>  XXX
        uint32_t availInMapLoc = inOutUsage.inputMapLocCount;
        uint32_t availOutMapLoc = inOutUsage.outputMapLocCount;

        uint32_t availPerPatchOutMapLoc = inOutUsage.perPatchOutputMapLocCount;

        // Map built-in inputs to generic ones
        if (builtInUsage.tcs.positionIn)
        {
            inOutUsage.builtInInputLocMap[BuiltInPosition] = availInMapLoc++;
        }

        if (builtInUsage.tcs.pointSizeIn)
        {
            inOutUsage.builtInInputLocMap[BuiltInPointSize] = availInMapLoc++;
        }

        if (builtInUsage.tcs.clipDistanceIn > 0)
        {
            inOutUsage.builtInInputLocMap[BuiltInClipDistance] = availInMapLoc++;
            if (builtInUsage.tcs.clipDistanceIn > 4)
            {
                ++availInMapLoc;
            }
        }

        if (builtInUsage.tcs.cullDistanceIn > 0)
        {
            inOutUsage.builtInInputLocMap[BuiltInCullDistance] = availInMapLoc++;
            if (builtInUsage.tcs.cullDistanceIn > 4)
            {
                ++availInMapLoc;
            }
        }

        // Map built-in outputs to generic ones
        if (nextStage == ShaderStageTessEval)
        {
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.tes;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            // NOTE: For tessellation control shadder, those built-in outputs that involve in output import have to
            // be mapped to generic ones even if they do not have corresponding built-in inputs used in next shader
            // stage.
            if (nextBuiltInUsage.positionIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPosition) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPosition];
                inOutUsage.builtInOutputLocMap[BuiltInPosition] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                if (m_importedOutputBuiltIns.find(BuiltInPosition) != m_importedOutputBuiltIns.end())
                {
                    inOutUsage.builtInOutputLocMap[BuiltInPosition] = InvalidValue;
                }
                else
                {
                    builtInUsage.tcs.position = false;
                }
            }

            if (nextBuiltInUsage.pointSizeIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
                inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                if (m_importedOutputBuiltIns.find(BuiltInPointSize) != m_importedOutputBuiltIns.end())
                {
                    inOutUsage.builtInOutputLocMap[BuiltInPointSize] = InvalidValue;
                }
                else
                {
                    builtInUsage.tcs.pointSize = false;
                }
            }

            if (nextBuiltInUsage.clipDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.clipDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                if (m_importedOutputBuiltIns.find(BuiltInClipDistance) != m_importedOutputBuiltIns.end())
                {
                    inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = InvalidValue;
                }
                else
                {
                    builtInUsage.tcs.clipDistance = 0;
                }
            }

            if (nextBuiltInUsage.cullDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.cullDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                if (m_importedOutputBuiltIns.find(BuiltInCullDistance) != m_importedOutputBuiltIns.end())
                {
                    inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = InvalidValue;
                }
                else
                {
                    builtInUsage.tcs.cullDistance = 0;
                }
            }

            if (nextBuiltInUsage.tessLevelOuter)
            {
                LLPC_ASSERT(nextInOutUsage.perPatchBuiltInInputLocMap.find(BuiltInTessLevelOuter) !=
                            nextInOutUsage.perPatchBuiltInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelOuter];
                inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] = mapLoc;
                availPerPatchOutMapLoc = std::max(availPerPatchOutMapLoc, mapLoc + 1);
            }
            else
            {
                // NOTE: We have to map gl_TessLevelOuter to generic per-patch output as long as it is used.
                if (builtInUsage.tcs.tessLevelOuter)
                {
                    inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] = InvalidValue;
                }
            }

            if (nextBuiltInUsage.tessLevelInner)
            {
                LLPC_ASSERT(nextInOutUsage.perPatchBuiltInInputLocMap.find(BuiltInTessLevelInner) !=
                            nextInOutUsage.perPatchBuiltInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelInner];
                inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] = mapLoc;
                availPerPatchOutMapLoc = std::max(availPerPatchOutMapLoc, mapLoc + 1);
            }
            else
            {
                // NOTE: We have to map gl_TessLevelInner to generic per-patch output as long as it is used.
                if (builtInUsage.tcs.tessLevelInner)
                {
                    inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] = InvalidValue;
                }
            }

            // Revisit built-in outputs and map those unmapped to generic ones
            if ((inOutUsage.builtInOutputLocMap.find(BuiltInPosition) != inOutUsage.builtInOutputLocMap.end()) &&
                (inOutUsage.builtInOutputLocMap[BuiltInPosition] == InvalidValue))
            {
                inOutUsage.builtInOutputLocMap[BuiltInPosition] = availOutMapLoc++;
            }

            if ((inOutUsage.builtInOutputLocMap.find(BuiltInPointSize) != inOutUsage.builtInOutputLocMap.end()) &&
                (inOutUsage.builtInOutputLocMap[BuiltInPointSize] == InvalidValue))
            {
                inOutUsage.builtInOutputLocMap[BuiltInPointSize] = availOutMapLoc++;
            }

            if ((inOutUsage.builtInOutputLocMap.find(BuiltInClipDistance) != inOutUsage.builtInOutputLocMap.end()) &&
                (inOutUsage.builtInOutputLocMap[BuiltInClipDistance] == InvalidValue))
            {
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = availOutMapLoc++;
            }

            if ((inOutUsage.builtInOutputLocMap.find(BuiltInCullDistance) != inOutUsage.builtInOutputLocMap.end()) &&
                (inOutUsage.builtInOutputLocMap[BuiltInCullDistance] == InvalidValue))
            {
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = availOutMapLoc++;
            }

            if ((inOutUsage.perPatchBuiltInOutputLocMap.find(BuiltInTessLevelOuter) !=
                 inOutUsage.perPatchBuiltInOutputLocMap.end()) &&
                (inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] == InvalidValue))
            {
                inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] = availPerPatchOutMapLoc++;
            }

            if ((inOutUsage.perPatchBuiltInOutputLocMap.find(BuiltInTessLevelInner) !=
                 inOutUsage.perPatchBuiltInOutputLocMap.end()) &&
                (inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] == InvalidValue))
            {
                inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] = availPerPatchOutMapLoc++;
            }
        }
        else if (nextStage == ShaderStageInvalid)
        {
            // TCS only
            if (builtInUsage.tcs.position)
            {
                inOutUsage.builtInOutputLocMap[BuiltInPosition] = availOutMapLoc++;
            }

            if (builtInUsage.tcs.pointSize)
            {
                inOutUsage.builtInOutputLocMap[BuiltInPointSize] = availOutMapLoc++;
            }

            if (builtInUsage.tcs.clipDistance > 0)
            {
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = availOutMapLoc++;
                if (builtInUsage.tcs.clipDistance > 4)
                {
                    ++availOutMapLoc;
                }
            }

            if (builtInUsage.tcs.cullDistance > 0)
            {
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = availOutMapLoc++;
                if (builtInUsage.tcs.cullDistance > 4)
                {
                    ++availOutMapLoc;
                }
            }

            if (builtInUsage.tcs.tessLevelOuter)
            {
                inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] = availPerPatchOutMapLoc++;
            }

            if (builtInUsage.tcs.tessLevelInner)
            {
                inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] = availPerPatchOutMapLoc++;
            }
        }

        inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, availInMapLoc);
        inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, availOutMapLoc);
        inOutUsage.perPatchOutputMapLocCount = std::max(inOutUsage.perPatchOutputMapLocCount, availPerPatchOutMapLoc);
    }
    else if (m_shaderStage == ShaderStageTessEval)
    {
        // TES  ==>  XXX
        uint32_t availInMapLoc = inOutUsage.inputMapLocCount;
        uint32_t availOutMapLoc = inOutUsage.outputMapLocCount;

        uint32_t availPerPatchInMapLoc = inOutUsage.perPatchInputMapLocCount;

        // Map built-in inputs to generic ones
        if (builtInUsage.tes.positionIn)
        {
            inOutUsage.builtInInputLocMap[BuiltInPosition] = availInMapLoc++;
        }

        if (builtInUsage.tes.pointSizeIn)
        {
            inOutUsage.builtInInputLocMap[BuiltInPointSize] = availInMapLoc++;
        }

        if (builtInUsage.tes.clipDistanceIn > 0)
        {
            uint32_t clipDistanceCount = builtInUsage.tes.clipDistanceIn;

            // NOTE: If gl_in[].gl_ClipDistance is used, we have to check the usage of gl_out[].gl_ClipDistance in
            // tessellation control shader. The clip distance is the maximum of the two. We do this to avoid
            // incorrectness of location assignment during builtin-to-generic mapping.
            const auto prevStage = m_pContext->GetPrevShaderStage(m_shaderStage);
            if (prevStage == ShaderStageTessControl)
            {
                const auto& prevBuiltInUsage = m_pContext->GetShaderResourceUsage(prevStage)->builtInUsage.tcs;
                clipDistanceCount = std::max(clipDistanceCount, prevBuiltInUsage.clipDistance);
            }

            inOutUsage.builtInInputLocMap[BuiltInClipDistance] = availInMapLoc++;
            if (clipDistanceCount > 4)
            {
                ++availInMapLoc;
            }
        }

        if (builtInUsage.tes.cullDistanceIn > 0)
        {
            uint32_t cullDistanceCount = builtInUsage.tes.cullDistanceIn;

            const auto prevStage = m_pContext->GetPrevShaderStage(m_shaderStage);
            if (prevStage == ShaderStageTessControl)
            {
                const auto& prevBuiltInUsage = m_pContext->GetShaderResourceUsage(prevStage)->builtInUsage.tcs;
                cullDistanceCount = std::max(cullDistanceCount, prevBuiltInUsage.clipDistance);
            }

            inOutUsage.builtInInputLocMap[BuiltInCullDistance] = availInMapLoc++;
            if (cullDistanceCount > 4)
            {
                ++availInMapLoc;
            }
        }

        if (builtInUsage.tes.tessLevelOuter)
        {
            inOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelOuter] = availPerPatchInMapLoc++;
        }

        if (builtInUsage.tes.tessLevelInner)
        {
            inOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelInner] = availPerPatchInMapLoc++;
        }

        // Map built-in outputs to generic ones
        if (nextStage == ShaderStageFragment)
        {
            // TES  ==>  FS
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.fs;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            if (nextBuiltInUsage.clipDistance > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
            }

            if (nextBuiltInUsage.cullDistance > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
            }

            if (nextBuiltInUsage.primitiveId)
            {
                // NOTE: The usage flag of gl_PrimitiveID must be set if fragment shader uses it.
                builtInUsage.tes.primitiveId = true;

                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPrimitiveId) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPrimitiveId];
                inOutUsage.builtInOutputLocMap[BuiltInPrimitiveId] = mapLoc;
            }

            if (nextBuiltInUsage.layer)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
                inOutUsage.builtInOutputLocMap[BuiltInLayer] = mapLoc;
            }

            if (nextBuiltInUsage.viewIndex)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewIndex) !=
                    nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewIndex];
                inOutUsage.builtInOutputLocMap[BuiltInViewIndex] = mapLoc;
            }

            if (nextBuiltInUsage.viewportIndex)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
                inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = mapLoc;
            }
        }
        else if (nextStage == ShaderStageGeometry)
        {
            // TES  ==>  GS
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.gs;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            if (nextBuiltInUsage.positionIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPosition) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPosition];
                inOutUsage.builtInOutputLocMap[BuiltInPosition] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                builtInUsage.tes.position = false;
            }

            if (nextBuiltInUsage.pointSizeIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
                inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                builtInUsage.tes.pointSize = false;
            }

            if (nextBuiltInUsage.clipDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.clipDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                builtInUsage.tes.clipDistance = 0;
            }

            if (nextBuiltInUsage.cullDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.cullDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                builtInUsage.tes.cullDistance = 0;
            }

            builtInUsage.tes.layer = false;
            builtInUsage.tes.viewportIndex = false;
        }
        else if (nextStage == ShaderStageInvalid)
        {
            // TES only
            if ((builtInUsage.tes.clipDistance > 0) || (builtInUsage.tes.cullDistance > 0))
            {
                uint32_t mapLoc = availOutMapLoc++;
                if (builtInUsage.tes.clipDistance + builtInUsage.tes.cullDistance > 4)
                {
                    LLPC_ASSERT(builtInUsage.tes.clipDistance +
                                builtInUsage.tes.cullDistance <= MaxClipCullDistanceCount);
                    ++availOutMapLoc; // Occupy two locations
                }

                if (builtInUsage.tes.clipDistance > 0)
                {
                    inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
                }

                if (builtInUsage.tes.cullDistance > 0)
                {
                    if (builtInUsage.tes.clipDistance >= 4)
                    {
                        ++mapLoc;
                    }
                    inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
                }
            }

            if (builtInUsage.tes.viewportIndex)
            {
                inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = availOutMapLoc++;
            }

            if (builtInUsage.tes.layer)
            {
                inOutUsage.builtInOutputLocMap[BuiltInLayer] = availOutMapLoc++;
            }

            if (builtInUsage.tes.viewIndex)
            {
                inOutUsage.builtInOutputLocMap[BuiltInViewIndex] = availOutMapLoc++;
            }
        }

        inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, availInMapLoc);
        inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, availOutMapLoc);

        inOutUsage.perPatchInputMapLocCount = std::max(inOutUsage.perPatchInputMapLocCount, availPerPatchInMapLoc);
    }
    else if (m_shaderStage == ShaderStageGeometry)
    {
        // GS  ==>  XXX
        uint32_t availInMapLoc  = inOutUsage.inputMapLocCount;

        // Map built-in inputs to generic ones
        if (builtInUsage.gs.positionIn)
        {
            inOutUsage.builtInInputLocMap[BuiltInPosition] = availInMapLoc++;
        }

        if (builtInUsage.gs.pointSizeIn)
        {
            inOutUsage.builtInInputLocMap[BuiltInPointSize] = availInMapLoc++;
        }

        if (builtInUsage.gs.clipDistanceIn > 0)
        {
            inOutUsage.builtInInputLocMap[BuiltInClipDistance] = availInMapLoc++;
            if (builtInUsage.gs.clipDistanceIn > 4)
            {
                ++availInMapLoc;
            }
        }

        if (builtInUsage.gs.cullDistanceIn > 0)
        {
            inOutUsage.builtInInputLocMap[BuiltInCullDistance] = availInMapLoc++;
            if (builtInUsage.gs.cullDistanceIn > 4)
            {
                ++availInMapLoc;
            }
        }

        // Map built-in outputs to generic ones (for GS)
        if (builtInUsage.gs.position)
        {
            MapGsBuiltInOutput(BuiltInPosition, 1);
        }

        if (builtInUsage.gs.pointSize)
        {
            MapGsBuiltInOutput(BuiltInPointSize, 1);
        }

        if (builtInUsage.gs.clipDistance > 0)
        {
            MapGsBuiltInOutput(BuiltInClipDistance, builtInUsage.gs.clipDistance);
        }

        if (builtInUsage.gs.cullDistance > 0)
        {
            MapGsBuiltInOutput(BuiltInCullDistance, builtInUsage.gs.cullDistance);
        }

        if (builtInUsage.gs.primitiveId)
        {
            MapGsBuiltInOutput(BuiltInPrimitiveId, 1);
        }

        if (builtInUsage.gs.layer)
        {
            MapGsBuiltInOutput(BuiltInLayer, 1);
        }

        if (builtInUsage.gs.viewIndex)
        {
            MapGsBuiltInOutput(BuiltInViewIndex, 1);
        }

        if (builtInUsage.gs.viewportIndex)
        {
            MapGsBuiltInOutput(BuiltInViewportIndex, 1);
        }

        // Map built-in outputs to generic ones (for copy shader)
        auto& builtInOutLocs = inOutUsage.gs.builtInOutLocs;

        if (nextStage == ShaderStageFragment)
        {
            // GS  ==>  FS
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.fs;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            if (nextBuiltInUsage.clipDistance > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                builtInOutLocs[BuiltInClipDistance] = mapLoc;
            }

            if (nextBuiltInUsage.cullDistance > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                builtInOutLocs[BuiltInCullDistance] = mapLoc;
            }

            if (nextBuiltInUsage.primitiveId)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPrimitiveId) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPrimitiveId];
                builtInOutLocs[BuiltInPrimitiveId] = mapLoc;
            }

            if (nextBuiltInUsage.layer)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
                builtInOutLocs[BuiltInLayer] = mapLoc;
            }

            if (nextBuiltInUsage.viewIndex)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewIndex) !=
                    nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewIndex];
                builtInOutLocs[BuiltInViewIndex] = mapLoc;
            }

            if (nextBuiltInUsage.viewportIndex)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
                builtInOutLocs[BuiltInViewportIndex] = mapLoc;
            }
        }
        else if (nextStage == ShaderStageInvalid)
        {
            // GS only
            uint32_t availOutMapLoc = inOutUsage.outputLocMap.size(); // Reset available location

            if ((builtInUsage.gs.clipDistance > 0) || (builtInUsage.gs.cullDistance > 0))
            {
                uint32_t mapLoc = availOutMapLoc++;
                if (builtInUsage.gs.clipDistance + builtInUsage.gs.cullDistance > 4)
                {
                    LLPC_ASSERT(builtInUsage.gs.clipDistance +
                                builtInUsage.gs.cullDistance <= MaxClipCullDistanceCount);
                    ++availOutMapLoc; // Occupy two locations
                }

                if (builtInUsage.gs.clipDistance > 0)
                {
                    builtInOutLocs[BuiltInClipDistance] = mapLoc;
                }

                if (builtInUsage.gs.cullDistance > 0)
                {
                    if (builtInUsage.gs.clipDistance >= 4)
                    {
                        ++mapLoc;
                    }
                    builtInOutLocs[BuiltInCullDistance] = mapLoc;
                }
            }

            if (builtInUsage.gs.primitiveId)
            {
                builtInOutLocs[BuiltInPrimitiveId] = availOutMapLoc++;
            }

            if (builtInUsage.gs.viewportIndex)
            {
                builtInOutLocs[BuiltInViewportIndex] = availOutMapLoc++;
            }

            if (builtInUsage.gs.layer)
            {
                builtInOutLocs[BuiltInLayer] = availOutMapLoc++;
            }

            if (builtInUsage.gs.viewIndex)
            {
                builtInOutLocs[BuiltInViewIndex] = availOutMapLoc++;
            }
        }

        inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, availInMapLoc);
    }
    else if (m_shaderStage == ShaderStageFragment)
    {
        // FS
        uint32_t availInMapLoc = inOutUsage.inputMapLocCount;

        if (builtInUsage.fs.pointCoord)
        {
            inOutUsage.builtInInputLocMap[BuiltInPointCoord] = availInMapLoc++;
        }

        if (builtInUsage.fs.primitiveId)
        {
            inOutUsage.builtInInputLocMap[BuiltInPrimitiveId] = availInMapLoc++;
        }

        if (builtInUsage.fs.layer)
        {
            inOutUsage.builtInInputLocMap[BuiltInLayer] = availInMapLoc++;
        }

        if (builtInUsage.fs.viewIndex)
        {
            inOutUsage.builtInInputLocMap[BuiltInViewIndex] = availInMapLoc++;
        }

        if (builtInUsage.fs.viewportIndex)
        {
            inOutUsage.builtInInputLocMap[BuiltInViewportIndex] = availInMapLoc++;
        }

        if ((builtInUsage.fs.clipDistance > 0) || (builtInUsage.fs.cullDistance > 0))
        {
            uint32_t mapLoc = availInMapLoc++;
            if (builtInUsage.fs.clipDistance + builtInUsage.fs.cullDistance > 4)
            {
                LLPC_ASSERT(builtInUsage.fs.clipDistance +
                            builtInUsage.fs.cullDistance <= MaxClipCullDistanceCount);
                ++availInMapLoc; // Occupy two locations
            }

            if (builtInUsage.fs.clipDistance > 0)
            {
                inOutUsage.builtInInputLocMap[BuiltInClipDistance] = mapLoc;
            }

            if (builtInUsage.fs.cullDistance > 0)
            {
                if (builtInUsage.fs.clipDistance >= 4)
                {
                    ++mapLoc;
                }
                inOutUsage.builtInInputLocMap[BuiltInCullDistance] = mapLoc;
            }
        }

        inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, availInMapLoc);
    }

    // Do builtin-to-generic mapping
    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC builtin-to-generic mapping results (" << GetShaderStageName(m_shaderStage)
              << " shader)\n\n");
    if (inOutUsage.builtInInputLocMap.empty() == false)
    {
        for (const auto& builtInMap : inOutUsage.builtInInputLocMap)
        {
            const BuiltIn builtInId = static_cast<BuiltIn>(builtInMap.first);
            const uint32_t loc = builtInMap.second;
            LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input:  builtin = "
                          << getNameMap(builtInId).map(builtInId).substr(strlen("BuiltIn"))
                          << "  =>  Mapped = " << loc << "\n");
        }
        LLPC_OUTS("\n");
    }

    if (inOutUsage.builtInOutputLocMap.empty() == false)
    {
        for (const auto& builtInMap : inOutUsage.builtInOutputLocMap)
        {
            const BuiltIn builtInId = static_cast<BuiltIn>(builtInMap.first);
            const uint32_t loc = builtInMap.second;

            if (m_shaderStage == ShaderStageGeometry)
            {
                LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true)
                    << ") Output: stream = " << inOutUsage.gs.rasterStream << " , "
                    << "builtin = " << getNameMap(builtInId).map(builtInId).substr(strlen("BuiltIn"))
                    << "  =>  Mapped = " << loc << "\n");
            }
            else
            {
                LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true)
                    << ") Output: builtin = "
                    << getNameMap(builtInId).map(builtInId).substr(strlen("BuiltIn"))
                    << "  =>  Mapped = " << loc << "\n");
            }
        }
        LLPC_OUTS("\n");
    }

    if (inOutUsage.perPatchBuiltInInputLocMap.empty() == false)
    {
        for (const auto& builtInMap : inOutUsage.perPatchBuiltInInputLocMap)
        {
            const BuiltIn builtInId = static_cast<BuiltIn>(builtInMap.first);
            const uint32_t loc = builtInMap.second;
            LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input (per-patch):  builtin = "
                          << getNameMap(builtInId).map(builtInId).substr(strlen("BuiltIn"))
                          << "  =>  Mapped = " << loc << "\n");
        }
        LLPC_OUTS("\n");
    }

    if (inOutUsage.perPatchBuiltInOutputLocMap.empty() == false)
    {
        for (const auto& builtInMap : inOutUsage.perPatchBuiltInOutputLocMap)
        {
            const BuiltIn builtInId = static_cast<BuiltIn>(builtInMap.first);
            const uint32_t loc = builtInMap.second;
            LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output (per-patch): builtin = "
                          << getNameMap(builtInId).map(builtInId).substr(strlen("BuiltIn"))
                          << "  =>  Mapped = " << loc << "\n");
        }
        LLPC_OUTS("\n");
    }

    LLPC_OUTS("// LLPC location count results (after builtin-to-generic mapping)\n\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input:  loc count = "
                  << inOutUsage.inputMapLocCount << "\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output: loc count = "
                  << inOutUsage.outputMapLocCount << "\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input (per-patch):  loc count = "
                  << inOutUsage.perPatchInputMapLocCount << "\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output (per-patch): loc count = "
                  << inOutUsage.perPatchOutputMapLocCount << "\n");
    LLPC_OUTS("\n");
}

// =====================================================================================================================
// Revises the usage of execution modes for tessellation shader.
void PatchResourceCollect::ReviseTessExecutionMode()
{
    LLPC_ASSERT((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval));

    // NOTE: Usually, "output vertices" is specified on tessellation control shader and "vertex spacing", "vertex
    // order", "point mode", "primitive mode" are all specified on tessellation evaluation shader according to GLSL
    // spec. However, SPIR-V spec allows those execution modes to be specified on any of tessellation shader. So we
    // have to revise the execution modes and make them follow GLSL spec.
    auto& tcsBuiltInUsage = m_pContext->GetShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
    auto& tesBuiltInUsage = m_pContext->GetShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;

    if (tcsBuiltInUsage.outputVertices == 0)
    {
        if (tesBuiltInUsage.outputVertices != 0)
        {
            tcsBuiltInUsage.outputVertices = tesBuiltInUsage.outputVertices;
            tesBuiltInUsage.outputVertices = 0;
        }
        else
        {
            tcsBuiltInUsage.outputVertices = MaxTessPatchVertices;
        }
    }

    if (tesBuiltInUsage.vertexSpacing == SpacingUnknown)
    {
        if (tcsBuiltInUsage.vertexSpacing != SpacingUnknown)
        {
            tesBuiltInUsage.vertexSpacing = tcsBuiltInUsage.vertexSpacing;
            tcsBuiltInUsage.vertexSpacing = SpacingUnknown;
        }
        else
        {
            tesBuiltInUsage.vertexSpacing = SpacingEqual;
        }
    }

    if (tesBuiltInUsage.vertexOrder == VertexOrderUnknown)
    {
        if (tcsBuiltInUsage.vertexOrder != VertexOrderUnknown)
        {
            tesBuiltInUsage.vertexOrder = tcsBuiltInUsage.vertexOrder;
            tcsBuiltInUsage.vertexOrder = VertexOrderUnknown;
        }
        else
        {
            tesBuiltInUsage.vertexOrder = VertexOrderCcw;
        }
    }

    if (tesBuiltInUsage.pointMode == false)
    {
        if (tcsBuiltInUsage.pointMode)
        {
            tesBuiltInUsage.pointMode = tcsBuiltInUsage.pointMode;
            tcsBuiltInUsage.pointMode = false;
        }
    }

    if (tesBuiltInUsage.primitiveMode == SPIRVPrimitiveModeKind::Unknown)
    {
        if (tcsBuiltInUsage.primitiveMode != SPIRVPrimitiveModeKind::Unknown)
        {
            tesBuiltInUsage.primitiveMode = tcsBuiltInUsage.primitiveMode;
            tcsBuiltInUsage.primitiveMode = SPIRVPrimitiveModeKind::Unknown;
        }
        else
        {
            tesBuiltInUsage.primitiveMode = Triangles;
        }
    }
}

// =====================================================================================================================
// Map locations of generic outputs of geometry shader to tightly packed ones.
void PatchResourceCollect::MapGsGenericOutput(
    GsOutLocInfo outLocInfo)             // GS output location info
{
    LLPC_ASSERT(m_shaderStage == ShaderStageGeometry);
    uint32_t streamId = outLocInfo.streamId;
    auto pResUsage = m_pContext->GetShaderResourceUsage(ShaderStageGeometry);
    auto& inOutUsage = pResUsage->inOutUsage.gs;

    pResUsage->inOutUsage.outputLocMap[outLocInfo.u32All] = inOutUsage.outLocCount[streamId]++;

    uint32_t assignedLocCount = inOutUsage.outLocCount[0] +
                            inOutUsage.outLocCount[1] +
                            inOutUsage.outLocCount[2] +
                            inOutUsage.outLocCount[3];

    pResUsage->inOutUsage.outputMapLocCount = std::max(pResUsage->inOutUsage.outputMapLocCount, assignedLocCount);

    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true)
                << ") Output: stream = " << outLocInfo.streamId << ", "
                << " loc = " << outLocInfo.location
                << "  =>  Mapped = "
                << pResUsage->inOutUsage.outputLocMap[outLocInfo.u32All] << "\n");
}

// =====================================================================================================================
// Map built-in outputs of geometry shader to tightly packed locations.
void PatchResourceCollect::MapGsBuiltInOutput(
    uint32_t builtInId,         // Built-in ID
    uint32_t elemCount)         // Element count of this built-in
{
    LLPC_ASSERT(m_shaderStage == ShaderStageGeometry);
    auto pResUsage = m_pContext->GetShaderResourceUsage(ShaderStageGeometry);
    auto& inOutUsage = pResUsage->inOutUsage.gs;
    uint32_t streamId = inOutUsage.rasterStream;

    pResUsage->inOutUsage.builtInOutputLocMap[builtInId] = inOutUsage.outLocCount[streamId]++;

    if (elemCount > 4)
    {
        inOutUsage.outLocCount[streamId]++;
    }

    uint32_t assignedLocCount = inOutUsage.outLocCount[0] +
                            inOutUsage.outLocCount[1] +
                            inOutUsage.outLocCount[2] +
                            inOutUsage.outLocCount[3];

    pResUsage->inOutUsage.outputMapLocCount = std::max(pResUsage->inOutUsage.outputMapLocCount, assignedLocCount);
}

// =====================================================================================================================
// Determine whether pack in/out is valid in patch phase
bool PatchResourceCollect::CheckValidityForInOutPack() const
{
    // Pack in/out requirements:
    // 1) cl::PackIo option is enable
    // 2) It is VS->FS pipeline or only VS
    if (cl::PackIo)
    {
        const bool hasNoCs = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageCompute)) == 0);
        const bool hasNoTcs = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageTessControl)) == 0);
        const bool hasNoTes = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageTessEval)) == 0);
        const bool hasNoGs = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageGeometry)) == 0);
        const bool hasVs = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageVertex)) != 0);

        return (hasNoCs && hasNoTcs && hasNoTes && hasNoGs && hasVs);
    }

    return false;
}

// =====================================================================================================================
// Process input/output call to collect InOutPackInfo for each call
void PatchResourceCollect::ProcessCallForInOutPack(CallInst* pCall)
{
    auto pCallee = pCall->getCalledFunction();
    auto mangledName = pCallee->getName();
    if (mangledName.startswith(LlpcName::InputImportGeneric))
    {
        // The key of inputLocMap is InOutPackInfo.u32All
        // The fields of compIdx, location, bitWidth, interpMode, interpLoc are used
        InOutPackInfo packInfo = {};
        packInfo.compIdx = cast<ConstantInt>(pCall->getOperand(1))->getZExtValue();
        packInfo.location = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue();

        packInfo.interpMode = cast<ConstantInt>(pCall->getOperand(2))->getZExtValue();
        LLPC_ASSERT(packInfo.interpMode < 4); // Valid range is : 0 ~ 3

        packInfo.interpLoc = cast<ConstantInt>(pCall->getOperand(3))->getZExtValue();
        LLPC_ASSERT(packInfo.interpLoc < 5);  // Valid range is : 0 ~ 4

        uint32_t bitWidth = pCallee->getReturnType()->getScalarSizeInBits();
        switch (bitWidth)
        {
        case 8:
            packInfo.bitWidth = BitWidth8;
            break;
        case 16:
            packInfo.bitWidth = BitWidth16;
            break;
        case 32:
            packInfo.bitWidth = BitWidth32;
            break;
        case 64:
            packInfo.bitWidth = BitWidth64;
            break;
        default:
            LLPC_NEVER_CALLED();
            break;
        }

        m_activeInputLocs.insert(packInfo.u32All);

        // The input scalar calls are unique due to being splitted once in lower phase
        // Generally, each generic input adds a new pair into inLocMap and stores the index of call temporarily
        // Specially, null FS initialized inLocMap with one pair <0, InvalidValue>.
        auto& inLocMap = m_pContext->GetShaderResourceUsage(m_shaderStage)->inOutUsage.inputLocMap;
        LLPC_ASSERT((inLocMap.find(packInfo.u32All) == inLocMap.end()) ||
                    (inLocMap[packInfo.u32All] == InvalidValue));
        inLocMap[packInfo.u32All] = m_importedCalls.size();

        m_importedCalls.push_back(pCall);
    }
    else if (mangledName.startswith(LlpcName::InputImportInterpolant))
    {
        auto pLocOffset = pCall->getOperand(1);
        if (isa<ConstantInt>(pLocOffset))
        {
            // Location offset is constant
            auto loc = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue();
            auto locOffset = cast<ConstantInt>(pLocOffset)->getZExtValue();
            loc += locOffset;

            LLPC_ASSERT(pCall->getType()->getPrimitiveSizeInBits() <= (8 * SizeOfVec4));

            // The key of inputLocMap is InOutPackInfo.u32All
            // The fields of compIdx, location, interpolantCompCount, isInterpolant are used
            InOutPackInfo packInfo = {};
            packInfo.compIdx = cast<ConstantInt>(pCall->getOperand(2))->getZExtValue();
            LLPC_ASSERT(packInfo.compIdx == 0);

            packInfo.location = loc;

            Type* pReturnTy = pCallee->getReturnType();
            packInfo.interpolantCompCount = pReturnTy->isVectorTy() ? pReturnTy->getVectorNumElements() : 1;

            packInfo.isInterpolant = true;

            m_activeInputLocs.insert(packInfo.u32All);

            // Interpolant input calls are kept original and need to update the matched location
            auto& inLocMap = m_pContext->GetShaderResourceUsage(m_shaderStage)->inOutUsage.inputLocMap;
            inLocMap[packInfo.u32All] = InvalidValue;

            m_importedCalls.push_back(pCall);
            m_hasInterpolantInput = true;
        }
        else
        {
            // NOTE: If location offset is not constant, we consider dynamic indexing occurs.
            m_hasDynIndexedInput = true;
        }
    }
    else
    {
        LLPC_ASSERT(mangledName.startswith(LlpcName::OutputExportGeneric) && "Invalid mangle name");

        // The key of inputLocMap is InOutPackInfo.u32All and the field of compIdx, location are used
        InOutPackInfo packInfo = {};
        packInfo.compIdx = cast<ConstantInt>(pCall->getOperand(1))->getZExtValue();
        packInfo.location = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue();

        auto& outLocMap = m_pContext->GetShaderResourceUsage(m_shaderStage)->inOutUsage.outputLocMap;
        outLocMap[packInfo.u32All] = m_exportedCalls.size();
        m_exportedCalls.push_back(pCall);
    }
}

// =====================================================================================================================
// Does generic output matching and does location mapping afterwards for only VS
void PatchResourceCollect::MatchGenericOutForOnlyVS()
{
    if (m_exportedCalls.empty())
    {
        return;
    }

    LLPC_ASSERT((m_shaderStage == ShaderStageVertex) &&
                (m_pContext->GetNextShaderStage(m_shaderStage) == ShaderStageInvalid));

    // Revert scalar calls to vector calls
    auto& outLocMap = m_pContext->GetShaderResourceUsage(m_shaderStage)->inOutUsage.outputLocMap;
    uint32_t packLoc = 0;

    IRBuilder<> builder(*m_pContext);
    CallInst* pInsertPos = m_exportedCalls.back();
    builder.SetInsertPoint(pInsertPos);

    for (auto locMapIt = outLocMap.begin(); locMapIt != outLocMap.end(); )
    {
        InOutPackInfo outPackInfo = {};
        outPackInfo.u32All = locMapIt->first;

        SmallVector<uint32_t, 4> callIndices;

        // Find the scalars belong to a same location
        for (uint32_t compIdx = 0; compIdx < 4; ++compIdx)
        {
            outPackInfo.compIdx = compIdx;
            if (outLocMap.find(outPackInfo.u32All) != outLocMap.end())
            {
                callIndices.push_back(outLocMap[outPackInfo.u32All]);
            }
        }

        const uint32_t compCount = callIndices.size();
        CallInst* pCall = m_exportedCalls[callIndices[0]];
        Type* pOutputTy = pCall->getOperand(2)->getType();
        const bool is64Bit = (pOutputTy->getScalarSizeInBits() == BitWidth64);

        SmallVector<Value*, 3> args(3, nullptr);
        Value* pOutValue = (compCount == 1) ? pCall->getOperand(2):
                           UndefValue::get(VectorType::get(pOutputTy, compCount));

        InOutPackInfo packInfo = {};
        packInfo.compIdx = cast<ConstantInt>(pCall->getOperand(1))->getZExtValue();
        packInfo.location = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue();
        // The new output will export a vector from packed location
        args[0] = ConstantInt::get(m_pContext->Int32Ty(), packInfo.u32All);
        args[1] = ConstantInt::get(m_pContext->Int32Ty(), 0);

        if (compCount > 1)
        {
            for (uint32_t compIdx = 0; compIdx < compCount; ++compIdx)
            {
                CallInst* pCall = m_exportedCalls[callIndices[compIdx]];

                InOutPackInfo outPackInfo = {};
                outPackInfo.compIdx = cast<ConstantInt>(pCall->getOperand(1))->getZExtValue();
                outPackInfo.location = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue();
                locMapIt = outLocMap.find(outPackInfo.u32All);
                locMapIt = outLocMap.erase(locMapIt);

                Value* pComp = pCall->getOperand(2);
                const uint32_t newCompIdx = is64Bit ? (compIdx * 2) : compIdx;
                Value* pCompIdx = ConstantInt::get(m_pContext->Int32Ty(), newCompIdx);
                pOutValue = builder.CreateInsertElement(pOutValue, pComp, pCompIdx);

                m_deadCalls.insert(pCall);
            }
        }
        else
        {
            m_deadCalls.insert(pCall);
            ++locMapIt;
        }

        args[2] = pOutValue;

        std::string callName = LlpcName::OutputExportGeneric;
        AddTypeMangling(m_pContext->VoidTy(), args, callName);

        EmitCall(callName,
                 m_pContext->VoidTy(),
                 args,
                 NoAttrib,
                 pInsertPos);

        outLocMap[packInfo.u32All] = packLoc++;
    }

}

// =====================================================================================================================
// Does generic input/output matching and does location mapping afterwards for packing input/output.
//
// NOTE: This function should be called after the cleanup work of inactive inputs is done.
void PatchResourceCollect::MatchGenericInOutWithPack()
{
    LLPC_ASSERT(m_pContext->IsGraphics());
    auto pCurResUsage = m_pContext->GetShaderResourceUsage(m_shaderStage);
    auto& curOutLocMap = pCurResUsage->inOutUsage.outputLocMap;

    const auto& nextStage = m_pContext->GetNextShaderStage(m_shaderStage);

    if (nextStage == ShaderStageFragment)
    {
        // Collect locations of those outputs that are used by next shader stage
        auto pNextResUsage = m_pContext->GetShaderResourceUsage(nextStage);
        auto& nextInLocMap = pNextResUsage->inOutUsage.inputLocMap;
        uint32_t availInMapLoc = pNextResUsage->inOutUsage.inputMapLocCount;

        std::set<uint32_t> usedLocs;

        for (auto locMap : nextInLocMap)
        {
            uint32_t loc = locMap.first;
            InOutPackInfo* pInPackInfo = reinterpret_cast<InOutPackInfo*>(&loc);
            InOutPackInfo  outPackInfo = {};
            outPackInfo.location = pInPackInfo->location;

            if (pInPackInfo->isInterpolant)
            {
                for (uint32_t compIdx = 0; compIdx < pInPackInfo->interpolantCompCount; ++compIdx)
                {
                    outPackInfo.compIdx = compIdx;
                    if (curOutLocMap.find(outPackInfo.u32All) != curOutLocMap.end())
                    {
                        usedLocs.insert(outPackInfo.u32All);
                    }
                }
            }
            else
            {
                outPackInfo.compIdx = pInPackInfo->compIdx;
                if (curOutLocMap.find(outPackInfo.u32All) != curOutLocMap.end())
                {
                    usedLocs.insert(outPackInfo.u32All);
                }

            }
        }
        // Remove unused locations from outputLocMap
        for (auto locMapIt = curOutLocMap.begin(); locMapIt != curOutLocMap.end();)
        {
            if (usedLocs.find(locMapIt->first) == usedLocs.end())
            {
                locMapIt = curOutLocMap.erase(locMapIt);
            }
            else
            {
                ++locMapIt;
            }
        }

        // Do pack for VS' output and FS' input
        PackGenericInOut();
    }
    else
    {
        LLPC_ASSERT(nextStage == ShaderStageInvalid);
        // Only VS
        MatchGenericOutForOnlyVS();
    }

    // Print inputLocMap and outputLocMap info and fill inputMapLocCount and outputMapLocCount
    const uint32_t stageCount = (nextStage == ShaderStageInvalid) ? 1 : 2;
    for (uint32_t stageIdx = stageCount; stageIdx > 0; --stageIdx)
    {
        const bool isNextStage = (stageIdx == 2);
        const auto& shaderStage = isNextStage ? nextStage : m_shaderStage;

        LLPC_OUTS("===============================================================================\n");
        LLPC_OUTS("// LLPC location input/output mapping results (" << GetShaderStageName(shaderStage)
                  << " shader)\n\n");

        auto& inOutUsage = isNextStage ? m_pContext->GetShaderResourceUsage(nextStage)->inOutUsage:
                            pCurResUsage->inOutUsage;
        auto& inputLocMap = inOutUsage.inputLocMap;
        if (inputLocMap.empty() == false)
        {
            LLPC_ASSERT(inOutUsage.inputMapLocCount == 0);
            for (auto& locMap : inputLocMap)
            {
                uint32_t location;
                // NOTE: For vertex shader, the input location mapping is actually trivial.
                if (shaderStage == ShaderStageVertex)
                {
                    LLPC_ASSERT(locMap.second == InvalidValue);
                    locMap.second = locMap.first;
                    location = locMap.first;
                }
                else
                {
                    // NOTE: For fragement shader, the input location mapping is done in packing process
                    InOutPackInfo inPackInfo;
                    inPackInfo.u32All = locMap.first;
                    location = inPackInfo.location;
                }

                inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, locMap.second + 1);
                LLPC_OUTS("(" << GetShaderStageAbbreviation(shaderStage, true) << ") Input:  loc = "
                              << location << "  =>  Mapped = " << locMap.second << "\n");
            }
            LLPC_OUTS("\n");
        }

        auto& outputLocMap = inOutUsage.outputLocMap;
        if (outputLocMap.empty() == false)
        {
            auto& outOrigLocs = inOutUsage.fs.outputOrigLocs;
            auto pPipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo*>(m_pContext->GetPipelineBuildInfo());
            if (shaderStage == ShaderStageFragment)
            {
                memset(&outOrigLocs, InvalidValue, sizeof(inOutUsage.fs.outputOrigLocs));
            }

            uint32_t nextMapLoc = 0;
            LLPC_ASSERT(inOutUsage.outputMapLocCount == 0);
            for (auto locMapIt = outputLocMap.begin(); locMapIt != outputLocMap.end();)
            {
                auto& locMap = *locMapIt;
                if (shaderStage == ShaderStageFragment)
                {
                    uint32_t location = locMap.first;
                    if (pPipelineInfo->cbState.dualSourceBlendEnable && (location == 1))
                    {
                        location = 0;
                    }
                    if (pPipelineInfo->cbState.target[location].format == VK_FORMAT_UNDEFINED)
                    {
                        locMapIt = outputLocMap.erase(locMapIt);
                        continue;
                    }
                }

                uint32_t loc = locMap.first;
                if (isNextStage == false)
                {
                    InOutPackInfo packInfo = {};
                    packInfo.u32All = locMap.first;
                    loc = packInfo.location;
                }
                else if (locMap.second == InvalidValue)
                {
                    // Only do location mapping if the output has not been mapped
                    locMap.second = nextMapLoc++;
                }

                inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, locMap.second + 1);
                LLPC_OUTS("(" << GetShaderStageAbbreviation(shaderStage, true) << ") Output: loc = "
                              << loc << "  =>  Mapped = " << locMap.second << "\n");

                if (shaderStage == ShaderStageFragment)
                {
                    outOrigLocs[locMap.second] = locMap.first;
                }

                ++locMapIt;
            }
            LLPC_OUTS("\n");
        }

        LLPC_OUTS("// LLPC location count results (after input/output matching) \n\n");
        LLPC_OUTS("(" << GetShaderStageAbbreviation(shaderStage, true) << ") Input:  loc count = "
                      << inOutUsage.inputMapLocCount << "\n");
        LLPC_OUTS("(" << GetShaderStageAbbreviation(shaderStage, true) << ") Output: loc count = "
                      << inOutUsage.outputMapLocCount << "\n");
        LLPC_OUTS("\n");
    }
}

// =====================================================================================================================
// Maps special built-in input/output to generic ones for packing input/output
//
// NOTE: This function should be called after generic input/output matching is done.
void PatchResourceCollect::MapBuiltInToGenericInOutWithPack()
{
    LLPC_ASSERT(m_pContext->IsGraphics());
    LLPC_ASSERT(m_shaderStage == ShaderStageVertex);

    // NOTE: The rules of mapping built-ins to generic inputs/outputs are as follow:
    //       (1) For built-in outputs, if next shader stager has corresponding built-in input used,
    //           get the mapped location from next shader stage inout usage and use it. If next shader stage
    //           is absent or it does not have such input used, we allocate the mapped location.
    //       (2) For built-in inputs, we always allocate the mapped location based its actual usage.

    const auto nextStage = m_pContext->GetNextShaderStage(m_shaderStage);

    // Map FS built-in to generic input
    if (nextStage == ShaderStageFragment)
    {
        auto pNextResUsage = m_pContext->GetShaderResourceUsage(nextStage);
        // FS built-in inputs
        const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.fs;
        auto& nextInOutUsage = pNextResUsage->inOutUsage;
        uint32_t availInMapLoc = nextInOutUsage.inputMapLocCount;
        if (nextBuiltInUsage.pointCoord)
        {
            nextInOutUsage.builtInInputLocMap[BuiltInPointCoord] = availInMapLoc++;
        }

        if (nextBuiltInUsage.primitiveId)
        {
            nextInOutUsage.builtInInputLocMap[BuiltInPrimitiveId] = availInMapLoc++;
        }

        if (nextBuiltInUsage.layer)
        {
            nextInOutUsage.builtInInputLocMap[BuiltInLayer] = availInMapLoc++;
        }

        if (nextBuiltInUsage.viewIndex)
        {
            nextInOutUsage.builtInInputLocMap[BuiltInViewIndex] = availInMapLoc++;
        }

        if (nextBuiltInUsage.viewportIndex)
        {
            nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex] = availInMapLoc++;
        }

        if ((nextBuiltInUsage.clipDistance > 0) || (nextBuiltInUsage.cullDistance > 0))
        {
            uint32_t mapLoc = availInMapLoc++;
            if (nextBuiltInUsage.clipDistance + nextBuiltInUsage.cullDistance > 4)
            {
                LLPC_ASSERT(nextBuiltInUsage.clipDistance +
                    nextBuiltInUsage.cullDistance <= MaxClipCullDistanceCount);
                ++availInMapLoc; // Occupy two locations
            }

            if (nextBuiltInUsage.clipDistance > 0)
            {
                nextInOutUsage.builtInInputLocMap[BuiltInClipDistance] = mapLoc;
            }

            if (nextBuiltInUsage.cullDistance > 0)
            {
                if (nextBuiltInUsage.clipDistance >= 4)
                {
                    ++mapLoc;
                }
                nextInOutUsage.builtInInputLocMap[BuiltInCullDistance] = mapLoc;
            }
        }
        nextInOutUsage.inputMapLocCount = std::max(nextInOutUsage.inputMapLocCount, availInMapLoc);
    }

    // Map VS built-in to generic in/out
    auto pCurResUsage = m_pContext->GetShaderResourceUsage(m_shaderStage);
    auto& curInOutUsage = pCurResUsage->inOutUsage;
    LLPC_ASSERT(curInOutUsage.builtInInputLocMap.empty()); // Should be empty
    LLPC_ASSERT(curInOutUsage.builtInOutputLocMap.empty());
    auto& curBuiltInUsage = pCurResUsage->builtInUsage;

    uint32_t availOutMapLoc = curInOutUsage.outputMapLocCount;
    if (nextStage == ShaderStageFragment)
    {
        auto pNextResUsage = m_pContext->GetShaderResourceUsage(nextStage);
        const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.fs;
        auto& nextInOutUsage = pNextResUsage->inOutUsage;

        if (nextBuiltInUsage.clipDistance > 0)
        {
            LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                nextInOutUsage.builtInInputLocMap.end());
            const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
            curInOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
        }

        if (nextBuiltInUsage.cullDistance > 0)
        {
            LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                nextInOutUsage.builtInInputLocMap.end());
            const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
            curInOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
        }

        if (nextBuiltInUsage.primitiveId)
        {
            // NOTE: The usage flag of gl_PrimitiveID must be set if fragment shader uses it.
            curBuiltInUsage.vs.primitiveId = true;

            LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPrimitiveId) !=
                nextInOutUsage.builtInInputLocMap.end());
            const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPrimitiveId];
            curInOutUsage.builtInOutputLocMap[BuiltInPrimitiveId] = mapLoc;
        }

        if (nextBuiltInUsage.layer)
        {
            LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) !=
                nextInOutUsage.builtInInputLocMap.end());
            const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
            curInOutUsage.builtInOutputLocMap[BuiltInLayer] = mapLoc;
        }

        if (nextBuiltInUsage.viewIndex)
        {
            LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewIndex) !=
                nextInOutUsage.builtInInputLocMap.end());
            const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewIndex];
            curInOutUsage.builtInOutputLocMap[BuiltInViewIndex] = mapLoc;
        }

        if (nextBuiltInUsage.viewportIndex)
        {
            LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) !=
                nextInOutUsage.builtInInputLocMap.end());
            const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
            curInOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = mapLoc;
        }
    }
    else
    {
        LLPC_ASSERT(nextStage == ShaderStageInvalid);
        // VS only
        if ((curBuiltInUsage.vs.clipDistance > 0) || (curBuiltInUsage.vs.cullDistance > 0))
        {
            uint32_t mapLoc = availOutMapLoc++;
            if (curBuiltInUsage.vs.clipDistance + curBuiltInUsage.vs.cullDistance > 4)
            {
                LLPC_ASSERT(curBuiltInUsage.vs.clipDistance +
                    curBuiltInUsage.vs.cullDistance <= MaxClipCullDistanceCount);
                ++availOutMapLoc; // Occupy two locations
            }

            if (curBuiltInUsage.vs.clipDistance > 0)
            {
                curInOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
            }

            if (curBuiltInUsage.vs.cullDistance > 0)
            {
                if (curBuiltInUsage.vs.clipDistance >= 4)
                {
                    ++mapLoc;
                }
                curInOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
            }
        }

        if (curBuiltInUsage.vs.viewportIndex)
        {
            curInOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = availOutMapLoc++;
        }

        if (curBuiltInUsage.vs.layer)
        {
            curInOutUsage.builtInOutputLocMap[BuiltInLayer] = availOutMapLoc++;
        }

        if (curBuiltInUsage.vs.viewIndex)
        {
            curInOutUsage.builtInOutputLocMap[BuiltInViewIndex] = availOutMapLoc++;
        }
    }
    curInOutUsage.outputMapLocCount = std::max(curInOutUsage.outputMapLocCount, availOutMapLoc);

    // Print builtin-to-generic mapping for current stage and next stage
    const uint32_t stageCount = (nextStage == ShaderStageInvalid) ? 1 : 2;
    for (uint32_t stageIdx = stageCount; stageIdx > 0; --stageIdx)
    {
        const bool isNextStage = (stageIdx == 2);
        const auto& shaderStage = isNextStage ? nextStage : m_shaderStage;
        const auto& inOutUsage = isNextStage ?
                    m_pContext->GetShaderResourceUsage(nextStage)->inOutUsage : curInOutUsage;

        LLPC_OUTS("===============================================================================\n");
        LLPC_OUTS("// LLPC builtin-to-generic mapping results (" << GetShaderStageName(shaderStage)
                  << " shader)\n\n");

        if (inOutUsage.builtInInputLocMap.empty() == false)
        {
            for (const auto& builtInMap : inOutUsage.builtInInputLocMap)
            {
                const BuiltIn builtInId = static_cast<BuiltIn>(builtInMap.first);
                const uint32_t loc = builtInMap.second;
                LLPC_OUTS("(" << GetShaderStageAbbreviation(shaderStage, true) << ") Input:  builtin = "
                              << getNameMap(builtInId).map(builtInId).substr(strlen("BuiltIn"))
                              << "  =>  Mapped = " << loc << "\n");
            }
            LLPC_OUTS("\n");
        }

        if (inOutUsage.builtInOutputLocMap.empty() == false)
        {
            for (const auto& builtInMap : inOutUsage.builtInOutputLocMap)
            {
                const BuiltIn builtInId = static_cast<BuiltIn>(builtInMap.first);
                const uint32_t loc = builtInMap.second;

                LLPC_OUTS("(" << GetShaderStageAbbreviation(shaderStage, true)
                              << ") Output: builtin = "
                              << getNameMap(builtInId).map(builtInId).substr(strlen("BuiltIn"))
                              << "  =>  Mapped = " << loc << "\n");

            }
            LLPC_OUTS("\n");
        }

        LLPC_OUTS("// LLPC location count results (after builtin-to-generic mapping)\n\n");
        LLPC_OUTS("(" << GetShaderStageAbbreviation(shaderStage, true) << ") Input:  loc count = "
                      << inOutUsage.inputMapLocCount << "\n");
        LLPC_OUTS("(" << GetShaderStageAbbreviation(shaderStage, true) << ") Output: loc count = "
                      << inOutUsage.outputMapLocCount << "\n");
        LLPC_OUTS("\n");
    }
}

// =====================================================================================================================
// Pack in/out will create calls with mapped location and packed component index
// Pack generic output of current stage and generic input of next stage
void PatchResourceCollect::PackGenericInOut()
{
    if (m_importedCalls.empty() && m_exportedCalls.empty())
    {
        // Skip. No need pack
        return;
    }

    std::vector<PackGroup> packGroups;
    std::vector<uint32_t>  orderedOutputCallIndices;
    PrepareForInOutPack(packGroups, orderedOutputCallIndices);

    uint32_t outputPackLoc = 0;
    uint32_t inputPackLoc = 0;

    // Create generic import calls with mapped location and component index
    CreatePackedGenericInOut(packGroups, orderedOutputCallIndices, inputPackLoc, outputPackLoc);

    // Create input and output calls for interpolant with matched location and component index
    CreateInterpolateInOut(inputPackLoc, outputPackLoc);
}

// =====================================================================================================================
// Prepare data for creating packed in/out
void PatchResourceCollect::PrepareForInOutPack(
    std::vector<PackGroup>& packGroups,                 // [in] Info of pack groups
    std::vector<uint32_t>& orderedOutputCallIndices)    // [in] A set of indices of ordered output calls
{
    const auto nextStage = m_pContext->GetNextShaderStage(m_shaderStage);
    LLPC_ASSERT(nextStage != ShaderStageInvalid);
    const auto& nextInLocMap = m_pContext->GetShaderResourceUsage(nextStage)->inOutUsage.inputLocMap;

    // nextStage's inputLocMap is used for partioning m_shaderStage's output calls in VS-FS pipeline
    if (m_exportedCalls.empty() == false)
    {
        auto& outLocMap = m_pContext->GetShaderResourceUsage(m_shaderStage)->inOutUsage.outputLocMap;

        orderedOutputCallIndices.reserve(m_exportedCalls.size());
        for (const auto& locMap : nextInLocMap)
        {
            uint32_t loc = locMap.first;
            InOutPackInfo* pInPackInfo = reinterpret_cast<InOutPackInfo*>(&loc);
            InOutPackInfo  outPackInfo = {};
            outPackInfo.compIdx = pInPackInfo->compIdx;
            outPackInfo.location = pInPackInfo->location;

            if (pInPackInfo->isInterpolant)
            {
                // Skip. Not join in packing.
                break;
            }

            LLPC_ASSERT(outLocMap.find(outPackInfo.u32All) != outLocMap.end());
            uint32_t callIndex = outLocMap[outPackInfo.u32All];
            orderedOutputCallIndices.push_back(callIndex);
        }
    }

    // A pack group has the same bitWdith, interpMode and interpLoc
    uint32_t callCount = 0;
    InOutPackInfo packInfo = {};

    for (auto locMapIt = nextInLocMap.begin(); locMapIt != nextInLocMap.end(); ++locMapIt, ++callCount)
    {
        uint32_t loc = locMapIt->first;
        InOutPackInfo* pCurPackInfo = reinterpret_cast<InOutPackInfo*>(&loc);
        // No need pack info for interpolant calls
        if (pCurPackInfo->isInterpolant)
        {
            break;
        }

        if (locMapIt != nextInLocMap.begin())
        {
            InOutPackInfo curPackInfo = {};
            curPackInfo.interpLoc  = pCurPackInfo->interpLoc;
            curPackInfo.interpMode = pCurPackInfo->interpMode;
            curPackInfo.bitWidth   = (pCurPackInfo->bitWidth == BitWidth64) ? BitWidth64 : BitWidth32;

            if (curPackInfo.u32All != packInfo.u32All)
            {
                // Update the current pack group
                auto& curPackGroup = packGroups.back();
                curPackGroup.scalarCallCount = callCount;

                // Add a new pack group
                PackGroup packGroup;
                packGroup.is64Bit = (curPackInfo.bitWidth == BitWidth64);
                packGroups.push_back(packGroup);

                packInfo.u32All = curPackInfo.u32All; // Reset packInfo
                callCount = 0; // Reset call count for a new pack group
            }
        }
        else
        {
            packInfo.interpLoc  = pCurPackInfo->interpLoc;
            packInfo.interpMode = pCurPackInfo->interpMode;
            packInfo.bitWidth   = (pCurPackInfo->bitWidth == BitWidth64) ? BitWidth64 : BitWidth32;

            // Add the first pack group
            PackGroup packGroup;
            packGroup.is64Bit = (packInfo.bitWidth == BitWidth64);
            packGroups.push_back(packGroup);
        }
    }

    if (packGroups.empty() == false)
    {
        auto& lastPackGroup = packGroups.back();
        lastPackGroup.scalarCallCount = callCount;
    }

    // All current stage's output and next stage's inputs should be added to dead calls list.
    // Because they will be recreated with new location and component index
    for (auto pCall : m_importedCalls)
    {
        m_deadCalls.insert(pCall);
    }

    for (auto pCall : m_exportedCalls)
    {
        m_deadCalls.insert(pCall);
    }
}

// =====================================================================================================================
// Create generic import and export calls with new mapped location and component index
void PatchResourceCollect::CreatePackedGenericInOut(
    const std::vector<PackGroup>& packGroups,               // [in] Info of pack groups
    const std::vector<uint32_t>& orderedOutputCallIndices,  // [in] A set of indices of ordered output calls
    uint32_t& inputPackLoc,                                 // [out] The final packed location for input
    uint32_t& outputPackLoc)                                // [Out] The final packed location for output
{
    const auto nextStage = m_pContext->GetNextShaderStage(m_shaderStage);
    LLPC_ASSERT(nextStage != ShaderStageInvalid);
    auto& nextInLocMap = m_pContext->GetShaderResourceUsage(nextStage)->inOutUsage.inputLocMap;
    auto inlocMapIt = nextInLocMap.begin();

    uint32_t callIndexPos = 0;

    for (const auto& packGroup : packGroups)
    {
        const uint32_t scalarCount = packGroup.scalarCallCount;
        if (scalarCount > 0)
        {
            inlocMapIt = CreatePackedGenericInput(packGroup.is64Bit, scalarCount, inputPackLoc, inlocMapIt);

            // NOTE: orderedOutputCallIds may be empty in case of using null FS
            if (orderedOutputCallIndices.empty() == false)
            {
                CreatePackedGenericOutput(orderedOutputCallIndices, packGroup.is64Bit, scalarCount, callIndexPos, outputPackLoc);
            }
        }
    }
}

// =====================================================================================================================
// Create input and output calls for interpolant with packed location and component index
void PatchResourceCollect::CreateInterpolateInOut(
    uint32_t& inputPackLoc,        // [int, out] The final packed output location
    uint32_t& outputPackLoc)       // [int, out] The final packed output location
{
    if (m_hasInterpolantInput == false)
    {
        return;
    }

    IRBuilder<> builder(*m_pContext);
    builder.SetInsertPoint(m_importedCalls.front());

    // FS @llpc.input.import.interpolant.% Type % (i32 location, i32 locOffset, i32 elemIdx,
    //                                            i32 interpMode, <2 x float> | i32 auxInterpValue)
    const auto nextStage = m_pContext->GetNextShaderStage(m_shaderStage);
    auto& nextInLocMap = m_pContext->GetShaderResourceUsage(nextStage)->inOutUsage.inputLocMap;
    for (auto pCall : m_importedCalls)
    {
        auto mangledName = pCall->getCalledFunction()->getName();
        if (mangledName.startswith(LlpcName::InputImportInterpolant) == false)
        {
            continue;
        }

        // Create interpolant input with matched location
        InOutPackInfo packInfo = {};
        packInfo.compIdx = cast<ConstantInt>(pCall->getOperand(2))->getZExtValue();
        LLPC_ASSERT(packInfo.compIdx == 0);

        auto loc = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue();
        auto locOffset = cast<ConstantInt>(pCall->getOperand(1))->getZExtValue();
        loc += locOffset;
        packInfo.location = loc;

        // Check if the location is proccessed by generic input
        bool found = false;
        InOutPackInfo usedPackInfo = {};
        for (auto locMap : nextInLocMap)
        {
            usedPackInfo.u32All = locMap.first;
            if (usedPackInfo.isInterpolant)
            {
                break;
            }
            if ((packInfo.compIdx == usedPackInfo.compIdx) && (packInfo.location == usedPackInfo.location))
            {
                found = true;
                break;
            }
        }

        Type* pReturnTy = pCall->getCalledFunction()->getReturnType();
        packInfo.interpolantCompCount = pReturnTy->isVectorTy() ? pReturnTy->getVectorNumElements() : 1;

        packInfo.isInterpolant = true;

        uint32_t matchedLoc = InvalidValue;

        if (found)
        {
            matchedLoc = usedPackInfo.u32All;
            nextInLocMap.erase(packInfo.u32All);
        }
        else
        {
            matchedLoc = packInfo.u32All;

            // InvalidValue means the variable is only used for interpolation, we should update it here
            if (nextInLocMap[packInfo.u32All] == InvalidValue)
            {
                nextInLocMap[packInfo.u32All] = inputPackLoc++;
            }
        }

        SmallVector<Value*, 5> args;
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), matchedLoc));
        for (uint32_t operandIdx = 1; operandIdx < pCall->getNumArgOperands(); ++operandIdx)
        {
            args.push_back(pCall->getOperand(operandIdx));
        }

        Value* pOutValue = EmitCall(mangledName,
                                    pCall->getCalledFunction()->getReturnType(),
                                    args,
                                    NoAttrib,
                                    pCall);
        pCall->replaceAllUsesWith(pOutValue);
    }

    // Restore splitted scalar-like VS generic outputs back to original vectors
    builder.SetInsertPoint(m_exportedCalls.back());

    auto& outLocMap = m_pContext->GetShaderResourceUsage(m_shaderStage)->inOutUsage.outputLocMap;

    for (auto inLocMap : nextInLocMap)
    {
        uint32_t loc = inLocMap.first;
        InOutPackInfo* pInPackInfo = reinterpret_cast<InOutPackInfo*>(&loc);
        if (pInPackInfo->isInterpolant == false)
        {
            continue;
        }

        InOutPackInfo outPackInfo = {};
        outPackInfo.location = pInPackInfo->location;
        const uint32_t compCount = pInPackInfo->interpolantCompCount;
        uint32_t callIndices[4] = {};
        for (uint32_t compIdx = 0; compIdx < compCount; ++compIdx)
        {
            outPackInfo.compIdx = compIdx;
            callIndices[compIdx] = outLocMap[outPackInfo.u32All];
        }

        SmallVector<Value*, 3> args(3, nullptr);
        CallInst* pCall = m_exportedCalls[callIndices[0]];
        LLPC_ASSERT(cast<ConstantInt>(pCall->getOperand(1))->getZExtValue() == 0);
        outPackInfo.compIdx = 0;
        // The restored output with matched Location and elem index 0
        args[0] = ConstantInt::get(m_pContext->Int32Ty(), outPackInfo.u32All);
        args[1] = ConstantInt::get(m_pContext->Int32Ty(), 0);

        std::string callName = LlpcName::OutputExportGeneric;

        if (compCount == 1)
        {
            // The restored output is scalar output
            outLocMap[outPackInfo.u32All] = outputPackLoc;

            args[2] = pCall->getOperand(2);

            AddTypeMangling(m_pContext->VoidTy(), args, callName);

            EmitCall(callName,
                     m_pContext->VoidTy(),
                     args,
                     NoAttrib,
                     m_exportedCalls.back());
        }
        else
        {
            // Construct the vector for output value
            CallInst* pCall = m_exportedCalls[callIndices[0]];

            Value* pOutValue = UndefValue::get(VectorType::get(pCall->getOperand(2)->getType(), compCount));
            for (uint32_t compIdx = 0; compIdx < compCount; ++compIdx)
            {
                outPackInfo.compIdx = compIdx;
                LLPC_ASSERT(outLocMap.find(outPackInfo.u32All) != outLocMap.end());
                // The scalar item is merged into a vector and it should be removed from the map
                outLocMap.erase(outPackInfo.u32All);

                pCall = m_exportedCalls[callIndices[compIdx]];
                Value* pCompIdx = ConstantInt::get(m_pContext->Int32Ty(), compIdx);
                pOutValue = builder.CreateInsertElement(pOutValue, pCall->getOperand(2), pCompIdx);
            }
            args[2] = pOutValue;

            AddTypeMangling(m_pContext->VoidTy(), args, callName);

            EmitCall(callName,
                     m_pContext->VoidTy(),
                     args,
                     NoAttrib,
                     m_exportedCalls.back());

            outPackInfo.compIdx = 0;
            outLocMap[outPackInfo.u32All] = outputPackLoc;
        }
        ++outputPackLoc;
    }
}

// =====================================================================================================================
// Create generic import calls with packed location and elemId
LocMapIterator PatchResourceCollect::CreatePackedGenericInput(
    bool is64Bit,                      // Whether the pack group is 64-bit
    uint32_t inputCallCount,           // The count of input calls
    uint32_t& packLoc,                 // [in, out] The packed location (continuous)
    LocMapIterator locMapIt)           // The iterator of next stage's inputLocMap
{
    //FS:  @llpc.input.import.generic.% Type % (i32 location, i32 elemIdx, i32 interpMode, i32 interpLoc)
    const uint32_t compCount = is64Bit ? 2 : 4;

    // [0]: The count of packed calls occupied four channels
    // [1]: The count of packed call is 1 when the count of (remianed) input calls is less than 4
    const uint32_t callCounts[2] = { inputCallCount / compCount, 1 };

    // [0]: The count of component for packed calls occupied four chanenls
    // [1]: The count of component for packed calls occupied partial chanels
    const uint32_t compCounts[2] = { compCount, inputCallCount - callCounts[0] * compCount };

    IRBuilder<> builder(*m_pContext);
    CallInst* pInsertPos = m_importedCalls.front();
    builder.SetInsertPoint(pInsertPos);

    const auto nextStage = m_pContext->GetNextShaderStage(m_shaderStage);
    auto& inLocMap = m_pContext->GetShaderResourceUsage(nextStage)->inOutUsage.inputLocMap;

    // FS' input scalar calls follow the merging rule of VS' output scalar calls
    for (uint32_t i = 0; i < 2; ++i)
    {
        const uint32_t callCount = callCounts[i];
        const uint32_t compCount = compCounts[i];
        if ((callCount == 0) || (compCount == 0))
        {
            continue;
        }

        for (uint32_t j = 0; j < callCount; ++j)
        {
            SmallVector<Value*, 4> args(4, nullptr);

            args[0] = ConstantInt::get(m_pContext->Int32Ty(), locMapIt->first);

            for (uint32_t compIdx = 0; compIdx < compCount; ++compIdx)
            {
                // Create generic input with matched location and elemId
                CallInst* pCall = m_importedCalls[locMapIt->second];

                // Update the component index of the new input call
                args[1] = ConstantInt::get(m_pContext->Int32Ty(), compIdx);
                args[2] = pCall->getOperand(2);
                args[3] = pCall->getOperand(3);

                Type* pOrigReturnTy = pCall->getCalledFunction()->getReturnType();
                Type* pReturnTy = is64Bit ? m_pContext->DoubleTy() : m_pContext->FloatTy();
                std::string callName = LlpcName::InputImportGeneric;
                AddTypeMangling(pReturnTy, args, callName);

                Value* pOutValue = EmitCall(callName,
                                            pReturnTy,
                                            args,
                                            NoAttrib,
                                            pInsertPos);

                // VS' packed output' type is either double or float which need to convert to original type
                if (pOrigReturnTy->isHalfTy())
                {
                    // f16
                    pOutValue = builder.CreateFPTrunc(pOutValue, pOrigReturnTy);
                }
                else if (pOrigReturnTy->isIntegerTy())
                {
                    // i8, i16, i32, i64
                    Type* pCastIntTy = is64Bit ? m_pContext->Int64Ty() : m_pContext->Int32Ty();
                    pOutValue = builder.CreateBitCast(pOutValue, pCastIntTy);
                    if (pOrigReturnTy->getScalarSizeInBits() < 32)
                    {
                        pOutValue = builder.CreateTrunc(pOutValue, pOrigReturnTy);
                    }
                }

                pCall->replaceAllUsesWith(pOutValue);

                // Update the mapped location
                locMapIt->second = packLoc;

                ++locMapIt;
            }

            ++packLoc;
        }
    }
    return locMapIt;
}

// =====================================================================================================================
// Create merged import calls with packed location and component index
void PatchResourceCollect::CreatePackedGenericOutput(
    const std::vector<uint32_t>& orderedOutputCallIndices,  // [in] A set of indices of ordered output calls
    bool is64Bit,                                           // Whether the pack group is 64-bit
    uint32_t outputCallCount,                               // The count of output calls
    uint32_t& callIndexPos,                                 // [in,out] The position of the call index
    uint32_t& packLoc)                                      // [in,out] The packed location (continuous)
{
    LLPC_ASSERT(orderedOutputCallIndices.empty() == false);
    const uint32_t compCount = is64Bit ? 2 : 4;

    // [0]: The count of packed calls occupied four channels
    // [1]: The count of packed call is 1 when the count of (remained) input calls is less than 4
    const uint32_t callCounts[2] = { outputCallCount / compCount, 1 };

    // [0]: The count of component for packed calls occupied four chanenls
    // [1]: The count of component for packed calls occupied partial chanels
    const uint32_t compCounts[2] = { compCount, outputCallCount - callCounts[0] * compCount };

    IRBuilder<> builder(*m_pContext);
    CallInst* pInsertPos = m_exportedCalls.back();
    builder.SetInsertPoint(pInsertPos);

    auto& outLocMap = m_pContext->GetShaderResourceUsage(m_shaderStage)->inOutUsage.outputLocMap;

    // Traverse the two elements of callCounts
    for (uint32_t i = 0; i < 2; ++i)
    {
        const uint32_t callCount = callCounts[i];
        const uint32_t compCount = compCounts[i];
        if ((callCount == 0) || (compCount == 0))
        {
            continue;
        }

        SmallVector<Value*, 3> args(3, nullptr);
        // VS output' type is either double or float
        Type* pOutputTy = is64Bit ? m_pContext->DoubleTy() : m_pContext->FloatTy();
        Value* pOutValue = UndefValue::get(VectorType::get(pOutputTy, compCount));

        for (uint32_t j = 0; j < callCount; ++j)
        {
            CallInst* pCall = m_exportedCalls[orderedOutputCallIndices[callIndexPos]];
            InOutPackInfo packInfo = {};
            packInfo.compIdx = cast<ConstantInt>(pCall->getOperand(1))->getZExtValue();
            packInfo.location = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue();
            // The new output will export a vector from packed location
            args[0] = ConstantInt::get(m_pContext->Int32Ty(), packInfo.u32All);
            args[1] = ConstantInt::get(m_pContext->Int32Ty(), 0);

            if (compCount == 1)
            {
                pOutValue = pCall->getOperand(2);
                outLocMap[packInfo.u32All] = packLoc;
            }
            else
            {
                // Construct the new output value from scalar calls
                for (uint32_t compIdx = 0; compIdx < compCount; ++compIdx)
                {
                    CallInst* pCall = m_exportedCalls[orderedOutputCallIndices[callIndexPos + compIdx]];

                    packInfo.compIdx = cast<ConstantInt>(pCall->getOperand(1))->getZExtValue();
                    packInfo.location = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue();
                    outLocMap[packInfo.u32All] = packLoc;

                    Value* pComp = pCall->getOperand(2);
                    Type* pCompTy = pComp->getType();
                    if (pCompTy->isHalfTy())
                    {
                        // f16 -> float
                        pComp = builder.CreateFPExt(pComp, m_pContext->FloatTy());
                    }
                    else if (pCompTy->isIntegerTy())
                    {
                        // i8, i16 -> i32
                        if (pComp->getType()->getScalarSizeInBits() < 32)
                        {
                            pComp = builder.CreateZExt(pComp, m_pContext->Int32Ty());
                        }

                        // i32 -> float, i64 -> double
                        Type* pCastTy = is64Bit ? m_pContext->DoubleTy() : m_pContext->FloatTy();
                        pComp = builder.CreateBitCast(pComp, pCastTy);
                    }
                    Value* pCompIdx = ConstantInt::get(m_pContext->Int32Ty(), compIdx);
                    pOutValue = builder.CreateInsertElement(pOutValue, pComp, pCompIdx);
                }
            }
            args[2] = pOutValue;

            std::string callName = LlpcName::OutputExportGeneric;
            AddTypeMangling(m_pContext->VoidTy(), args, callName);

            EmitCall(callName,
                     m_pContext->VoidTy(),
                     args,
                     NoAttrib,
                     pInsertPos);

            callIndexPos += compCount;

            // Update final packed location for vector calls
            ++packLoc;
        }
    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM patch operations for resource collecting.
INITIALIZE_PASS(PatchResourceCollect, DEBUG_TYPE,
                "Patch LLVM for resource collecting", false, false)
