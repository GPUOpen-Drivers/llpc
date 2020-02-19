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
 * @file  llpcPatchInOutImportExport.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchInOutImportExport.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-patch-in-out-import-export"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <unordered_set>
#include "llpcBuilderBuiltIns.h"
#include "llpcBuilderContext.h"
#include "llpcBuilderImpl.h"
#include "llpcFragColorExport.h"
#include "llpcPatchInOutImportExport.h"
#include "llpcPipelineShaders.h"
#include "llpcTargetInfo.h"
#include "llpcVertexFetch.h"

using namespace llvm;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char PatchInOutImportExport::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for input import and output export
ModulePass* CreatePatchInOutImportExport()
{
    return new PatchInOutImportExport();
}

// =====================================================================================================================
PatchInOutImportExport::PatchInOutImportExport()
    :
    Patch(ID),
    m_pLds(nullptr)
{
    memset(&m_gfxIp, 0, sizeof(m_gfxIp));
    InitPerShader();
}

// =====================================================================================================================
PatchInOutImportExport::~PatchInOutImportExport()
{
    LLPC_ASSERT(m_pFragColorExport == nullptr);
    LLPC_ASSERT(m_pVertexFetch == nullptr);
}

// =====================================================================================================================
// Initialize per-shader members
void PatchInOutImportExport::InitPerShader()
{
    m_pVertexFetch = nullptr;
    m_pFragColorExport = nullptr;
    m_pLastExport = nullptr;
    m_pClipDistance = nullptr;
    m_pCullDistance = nullptr;
    m_pPrimitiveId = nullptr;
    m_pFragDepth = nullptr;
    m_pFragStencilRef = nullptr;
    m_pSampleMask = nullptr;
    m_pViewportIndex = nullptr;
    m_pLayer = nullptr;
    m_pThreadId = nullptr;
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
bool PatchInOutImportExport::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-In-Out-Import-Export\n");

    Patch::Init(&module);

    m_pPipelineState = getAnalysis<PipelineStateWrapper>().GetPipelineState(&module);
    m_gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();
    m_pipelineSysValues.Initialize(m_pPipelineState);

    const uint32_t stageMask = m_pPipelineState->GetShaderStageMask();
    m_hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                             ShaderStageToMask(ShaderStageTessEval))) != 0);
    m_hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

    // Create the global variable that is to model LDS
    // NOTE: ES -> GS ring is always on-chip on GFX9.
    if (m_hasTs || (m_hasGs && (m_pPipelineState->IsGsOnChip() || (m_gfxIp.major >= 9))))
    {
        m_pLds = Patch::GetLdsVariable(m_pPipelineState, m_pModule);
    }

    // Process each shader in turn, in reverse order (because for example VS uses inOutUsage.tcs.calcFactor
    // set by TCS).
    auto pPipelineShaders = &getAnalysis<PipelineShaders>();
    for (int32_t shaderStage = ShaderStageCountInternal - 1; shaderStage >= 0; --shaderStage)
    {
        auto pEntryPoint = pPipelineShaders->GetEntryPoint(static_cast<ShaderStage>(shaderStage));
        if (pEntryPoint != nullptr)
        {
            InitPerShader();
            m_pEntryPoint = pEntryPoint;
            m_shaderStage = static_cast<ShaderStage>(shaderStage);
            ProcessShader();

            // Now process the call and return instructions.
            visit(*m_pEntryPoint);
        }
    }

    for (auto pCallInst : m_importCalls)
    {
        pCallInst->dropAllReferences();
        pCallInst->eraseFromParent();
    }
    m_importCalls.clear();

    for (auto pCallInst : m_exportCalls)
    {
        pCallInst->dropAllReferences();
        pCallInst->eraseFromParent();
    }
    m_exportCalls.clear();

    delete m_pFragColorExport;
    m_pFragColorExport = nullptr;

    delete m_pVertexFetch;
    m_pVertexFetch = nullptr;

    for (auto& fragColors : m_expFragColors)
    {
        fragColors.clear();
    }
    m_pipelineSysValues.Clear();

    return true;
}

// =====================================================================================================================
// Process a single shader
void PatchInOutImportExport::ProcessShader()
{
    if (m_shaderStage == ShaderStageVertex)
    {
        // Create vertex fetch manager
        m_pVertexFetch = new VertexFetch(m_pEntryPoint, m_pipelineSysValues.Get(m_pEntryPoint), m_pPipelineState);
    }
    else if (m_shaderStage == ShaderStageFragment)
    {
        // Create fragment color export manager
        m_pFragColorExport = new FragColorExport(m_pPipelineState, m_pModule);
    }

    // Initialize the output value for gl_PrimitiveID
    const auto& builtInUsage = m_pPipelineState->GetShaderResourceUsage(m_shaderStage)->builtInUsage;
    const auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(m_shaderStage)->entryArgIdxs;
    if (m_shaderStage == ShaderStageVertex)
    {
        if (builtInUsage.vs.primitiveId)
        {
            m_pPrimitiveId = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.vs.primitiveId);
        }
    }
    else if (m_shaderStage == ShaderStageTessEval)
    {
        if (builtInUsage.tes.primitiveId)
        {
            // TODO: Support tessellation shader.
            m_pPrimitiveId = UndefValue::get(Type::getInt32Ty(*m_pContext));
        }
    }

    // Thread ID will be used in on-chip GS offset calculation (ES -> GS ring is always on-chip on GFX9)
    bool useThreadId = (m_hasGs && (m_pPipelineState->IsGsOnChip() || (m_gfxIp.major >= 9)));

    // Thread ID will also be used for stream-out buffer export
    const bool enableXfb = m_pPipelineState->GetShaderResourceUsage(m_shaderStage)->inOutUsage.enableXfb;
    useThreadId = useThreadId || enableXfb;

    if (useThreadId)
    {
        // Calculate and store thread ID
        auto pInsertPos = m_pEntryPoint->begin()->getFirstInsertionPt();
        m_pThreadId = GetSubgroupLocalInvocationId(&*pInsertPos);
    }

    // Initialize calculation factors for tessellation shader
    if ((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval))
    {
        const uint32_t stageMask = m_pPipelineState->GetShaderStageMask();
        const bool hasTcs = ((stageMask & ShaderStageToMask(ShaderStageTessControl)) != 0);

        auto& calcFactor = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
        if ((calcFactor.inVertexStride              == InvalidValue) &&
            (calcFactor.outVertexStride             == InvalidValue) &&
            (calcFactor.patchCountPerThreadGroup    == InvalidValue) &&
            (calcFactor.outPatchSize                == InvalidValue) &&
            (calcFactor.patchConstSize              == InvalidValue))
        {
            // NOTE: The LDS space is divided to three parts:
            //
            //              +----------------------------------------+
            //            / | TCS Vertex (Control Point) In (VS Out) |
            //           /  +----------------------------------------+
            //   LDS Space  | TCS Vertex (Control Point) Out         |
            //           \  +----------------------------------------+
            //            \ | TCS Patch Constant                     |
            //              +----------------------------------------+
            //
            // inPatchTotalSize = inVertexCount * inVertexStride * patchCountPerThreadGroup
            // outPatchTotalSize = outVertexCount * outVertexStride * patchCountPerThreadGroup
            // patchConstTotalSize = patchConstCount * 4 * patchCountPerThreadGroup

            const auto& tcsInOutUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl)->inOutUsage;
            const auto& tesInOutUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessEval)->inOutUsage;

            const uint32_t inLocCount = std::max(tcsInOutUsage.inputMapLocCount, 1u);
            const uint32_t outLocCount =
                hasTcs ? std::max(tcsInOutUsage.outputMapLocCount, 1u) : std::max(tesInOutUsage.inputMapLocCount, 1u);

            const uint32_t inVertexCount = m_pPipelineState->GetInputAssemblyState().patchControlPoints;
            const uint32_t outVertexCount = hasTcs ?
                                            m_pPipelineState->GetShaderModes()->GetTessellationMode().outputVertices :
                                            MaxTessPatchVertices;

            uint32_t tessFactorStride = 0;
            switch (m_pPipelineState->GetShaderModes()->GetTessellationMode().primitiveMode)
            {
            case PrimitiveMode::Triangles:
                tessFactorStride = 4;
                break;
            case PrimitiveMode::Quads:
                tessFactorStride = 6;
                break;
            case PrimitiveMode::Isolines:
                tessFactorStride = 2;
                break;
            default:
                LLPC_NEVER_CALLED();
                break;
            }

            calcFactor.inVertexStride = inLocCount * 4;
            calcFactor.outVertexStride = outLocCount * 4;

            const uint32_t patchConstCount =
                hasTcs ? tcsInOutUsage.perPatchOutputMapLocCount : tesInOutUsage.perPatchInputMapLocCount;
            calcFactor.patchConstSize = patchConstCount * 4;

            calcFactor.patchCountPerThreadGroup = CalcPatchCountPerThreadGroup(inVertexCount,
                                                                               calcFactor.inVertexStride,
                                                                               outVertexCount,
                                                                               calcFactor.outVertexStride,
                                                                               patchConstCount,
                                                                               tessFactorStride);

            const uint32_t inPatchSize = inVertexCount * calcFactor.inVertexStride;
            const uint32_t inPatchTotalSize = calcFactor.patchCountPerThreadGroup * inPatchSize;

            const uint32_t outPatchSize = outVertexCount * calcFactor.outVertexStride;
            const uint32_t outPatchTotalSize = calcFactor.patchCountPerThreadGroup * outPatchSize;

            calcFactor.outPatchSize = outPatchSize;
            calcFactor.inPatchSize = inPatchSize;

            calcFactor.onChip.outPatchStart = inPatchTotalSize;
            calcFactor.onChip.patchConstStart = inPatchTotalSize + outPatchTotalSize;

            if (m_pPipelineState->IsTessOffChip())
            {
                calcFactor.offChip.outPatchStart = 0;
                calcFactor.offChip.patchConstStart = outPatchTotalSize;
            }

            calcFactor.tessFactorStride = tessFactorStride;

            LLPC_OUTS("===============================================================================\n");
            LLPC_OUTS("// LLPC tessellation calculation factor results\n\n");
            LLPC_OUTS("Patch count per thread group: " << calcFactor.patchCountPerThreadGroup << "\n");
            LLPC_OUTS("\n");
            LLPC_OUTS("Input vertex count: " << inVertexCount << "\n");
            LLPC_OUTS("Input vertex stride: " << calcFactor.inVertexStride << "\n");
            LLPC_OUTS("Input patch size: " << inPatchSize << "\n");
            LLPC_OUTS("Input patch total size: " << inPatchTotalSize << "\n");
            LLPC_OUTS("\n");
            LLPC_OUTS("Output vertex count: " << outVertexCount << "\n");
            LLPC_OUTS("Output vertex stride: " << calcFactor.outVertexStride << "\n");
            LLPC_OUTS("Output patch size: " << outPatchSize << "\n");
            LLPC_OUTS("Output patch total size: " << outPatchTotalSize << "\n");
            LLPC_OUTS("\n");
            LLPC_OUTS("Patch constant count: " << patchConstCount << "\n");
            LLPC_OUTS("Patch constant size: " << calcFactor.patchConstSize << "\n");
            LLPC_OUTS("Patch constant total size: " <<
                      calcFactor.patchConstSize * calcFactor.patchCountPerThreadGroup << "\n");
            LLPC_OUTS("\n");
            LLPC_OUTS("Tessellation factor stride: " << tessFactorStride << " (");
            switch (m_pPipelineState->GetShaderModes()->GetTessellationMode().primitiveMode)
            {
            case PrimitiveMode::Triangles:
                LLPC_OUTS("triangles");
                break;
            case PrimitiveMode::Quads:
                LLPC_OUTS("quads");
                tessFactorStride = 6;
                break;
            case PrimitiveMode::Isolines:
                LLPC_OUTS("isolines");
                tessFactorStride = 2;
                break;
            default:
                LLPC_NEVER_CALLED();
                break;
            }
            LLPC_OUTS(")\n\n");
        }
    }
}

// =====================================================================================================================
// Visits "call" instruction.
void PatchInOutImportExport::visitCallInst(
    CallInst& callInst)   // [in] "Call" instruction
{
    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(m_shaderStage);

    auto mangledName = pCallee->getName();

    auto importGenericInput     = LlpcName::InputImportGeneric;
    auto importBuiltInInput     = LlpcName::InputImportBuiltIn;
    auto importInterpolantInput = LlpcName::InputImportInterpolant;
    auto importGenericOutput    = LlpcName::OutputImportGeneric;
    auto importBuiltInOutput    = LlpcName::OutputImportBuiltIn;

    const bool isGenericInputImport     = mangledName.startswith(importGenericInput);
    const bool isBuiltInInputImport     = mangledName.startswith(importBuiltInInput);
    const bool isInterpolantInputImport = mangledName.startswith(importInterpolantInput);
    const bool isGenericOutputImport    = mangledName.startswith(importGenericOutput);
    const bool isBuiltInOutputImport    = mangledName.startswith(importBuiltInOutput);

    const bool isImport = (isGenericInputImport  || isBuiltInInputImport || isInterpolantInputImport ||
                           isGenericOutputImport || isBuiltInOutputImport);

    auto exportGenericOutput = LlpcName::OutputExportGeneric;
    auto exportBuiltInOutput = LlpcName::OutputExportBuiltIn;
    auto exportXfbOutput = LlpcName::OutputExportXfb;

    const bool isGenericOutputExport = mangledName.startswith(exportGenericOutput);
    const bool isBuiltInOutputExport = mangledName.startswith(exportBuiltInOutput);
    const bool isXfbOutputExport = mangledName.startswith(exportXfbOutput);

    const bool isExport = (isGenericOutputExport || isBuiltInOutputExport || isXfbOutputExport);

    const bool isInput  = (isGenericInputImport || isBuiltInInputImport || isInterpolantInputImport);
    const bool isOutput = (isGenericOutputImport || isBuiltInOutputImport ||
                           isGenericOutputExport || isBuiltInOutputExport || isXfbOutputExport);

    if (isImport && isInput)
    {
        // Input imports
        Value* pInput = nullptr;
        Type* pInputTy = callInst.getType();

        // Generic value (location or SPIR-V built-in ID)
        uint32_t value = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();

        LLVM_DEBUG(dbgs() << "Find input import call: builtin = " << isBuiltInInputImport
                     << " value = " << value << "\n");

        m_importCalls.push_back(&callInst);

        if (isBuiltInInputImport)
        {
            const uint32_t builtInId = value;

            switch (m_shaderStage)
            {
            case ShaderStageVertex:
                {
                    pInput = PatchVsBuiltInInputImport(pInputTy, builtInId, &callInst);
                    break;
                }
            case ShaderStageTessControl:
                {
                    // Builtin Call has different number of operands
                    Value* pElemIdx = nullptr; Value* pVertexIdx = nullptr;
                    if (callInst.getNumArgOperands() > 1)
                    {
                        pElemIdx = IsDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);
                    }

                    if (callInst.getNumArgOperands() > 2)
                    {
                        pVertexIdx = IsDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);
                    }

                    pInput = PatchTcsBuiltInInputImport(pInputTy, builtInId, pElemIdx, pVertexIdx, &callInst);
                    break;
                }
            case ShaderStageTessEval:
                {
                    // Builtin Call has different number of operands
                    Value *pElemIdx = nullptr; Value* pVertexIdx = nullptr;
                    if (callInst.getNumArgOperands() > 1)
                    {
                        pElemIdx = IsDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);
                    }

                    if (callInst.getNumArgOperands() > 2)
                    {
                        pVertexIdx = IsDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);
                    }
                    pInput = PatchTesBuiltInInputImport(pInputTy, builtInId, pElemIdx, pVertexIdx, &callInst);
                    break;
                }
            case ShaderStageGeometry:
                {
                    // Builtin Call has different number of operands
                    Value* pVertexIdx = nullptr;
                    if (callInst.getNumArgOperands() > 1)
                    {
                        pVertexIdx = IsDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);
                    }

                    pInput = PatchGsBuiltInInputImport(pInputTy, builtInId, pVertexIdx, &callInst);
                    break;
                }
            case ShaderStageFragment:
                {
                    Value* pSampleId = nullptr;
                    if (callInst.getNumArgOperands() >= 2)
                    {
                        pSampleId = callInst.getArgOperand(1);
                    }
                    pInput = PatchFsBuiltInInputImport(pInputTy, builtInId, pSampleId, &callInst);
                    break;
                }
            case ShaderStageCompute:
                {
                    pInput = PatchCsBuiltInInputImport(pInputTy, builtInId, &callInst);
                    break;
                }
            default:
                {
                    LLPC_NEVER_CALLED();
                    break;
                }
            }
        }
        else
        {
            LLPC_ASSERT(isGenericInputImport || isInterpolantInputImport);

            uint32_t loc = InvalidValue;
            Value* pLocOffset = nullptr;

            if (m_shaderStage == ShaderStageVertex)
            {
                // NOTE: For vertex shader, generic inputs are not mapped.
                loc = value;
            }
            else
            {
                if ((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval) ||
                    ((m_shaderStage == ShaderStageFragment) && isInterpolantInputImport))
                {
                    // NOTE: If location offset is present and is a constant, we have to add it to the unmapped
                    // location before querying the mapped location. Meanwhile, we have to adjust the location
                    // offset to 0 (rebase it).
                    pLocOffset = callInst.getOperand(1);
                    if (isa<ConstantInt>(pLocOffset))
                    {
                        auto locOffset = cast<ConstantInt>(pLocOffset)->getZExtValue();
                        value += locOffset;
                        pLocOffset = ConstantInt::get(Type::getInt32Ty(*m_pContext), 0);
                    }
                }

                if (m_shaderStage == ShaderStageTessEval)
                {
                    // NOTE: For generic inputs of tessellation evaluation shader, they could be per-patch ones.
                    if (pResUsage->inOutUsage.inputLocMap.find(value) !=
                        pResUsage->inOutUsage.inputLocMap.end())
                    {
                        loc = pResUsage->inOutUsage.inputLocMap[value];
                    }
                    else
                    {
                        LLPC_ASSERT(pResUsage->inOutUsage.perPatchInputLocMap.find(value) !=
                                    pResUsage->inOutUsage.perPatchInputLocMap.end());
                        loc = pResUsage->inOutUsage.perPatchInputLocMap[value];
                    }
                }
                else
                {
                    LLPC_ASSERT(pResUsage->inOutUsage.inputLocMap.find(value) !=
                                pResUsage->inOutUsage.inputLocMap.end());
                    loc = pResUsage->inOutUsage.inputLocMap[value];
                }
            }
            LLPC_ASSERT(loc != InvalidValue);

            switch (m_shaderStage)
            {
            case ShaderStageVertex:
                {
                    LLPC_ASSERT(callInst.getNumArgOperands() == 2);
                    const uint32_t compIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
                    pInput = PatchVsGenericInputImport(pInputTy, loc, compIdx, &callInst);
                    break;
                }
            case ShaderStageTessControl:
                {
                    LLPC_ASSERT(callInst.getNumArgOperands() == 4);

                    auto pElemIdx = callInst.getOperand(2);
                    LLPC_ASSERT(IsDontCareValue(pElemIdx) == false);

                    auto pVertexIdx = callInst.getOperand(3);
                    LLPC_ASSERT(IsDontCareValue(pVertexIdx) == false);

                    pInput = PatchTcsGenericInputImport(pInputTy, loc, pLocOffset, pElemIdx, pVertexIdx, &callInst);
                    break;
                }
            case ShaderStageTessEval:
                {
                    LLPC_ASSERT(callInst.getNumArgOperands() == 4);

                    auto pElemIdx = callInst.getOperand(2);
                    LLPC_ASSERT(IsDontCareValue(pElemIdx) == false);

                    auto pVertexIdx = IsDontCareValue(callInst.getOperand(3)) ? nullptr : callInst.getOperand(3);

                    pInput = PatchTesGenericInputImport(pInputTy, loc, pLocOffset, pElemIdx, pVertexIdx, &callInst);
                    break;
                }
            case ShaderStageGeometry:
                {
                    LLPC_ASSERT(callInst.getNumArgOperands() == 3);

                    const uint32_t compIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();

                    Value* pVertexIdx = callInst.getOperand(2);
                    LLPC_ASSERT(IsDontCareValue(pVertexIdx) == false);

                    pInput = PatchGsGenericInputImport(pInputTy, loc, compIdx, pVertexIdx, &callInst);
                    break;
                }
            case ShaderStageFragment:
                {
                    uint32_t interpMode = InOutInfo::InterpModeSmooth;
                    uint32_t interpLoc = InOutInfo::InterpLocCenter;

                    Value* pElemIdx = callInst.getOperand(isInterpolantInputImport ? 2 : 1);
                    LLPC_ASSERT(IsDontCareValue(pElemIdx) == false);

                    Value* pAuxInterpValue = nullptr;

                    if (isGenericInputImport)
                    {
                        LLPC_ASSERT(callInst.getNumArgOperands() == 4);

                        interpMode = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();
                        interpLoc = cast<ConstantInt>(callInst.getOperand(3))->getZExtValue();
                    }
                    else
                    {
                        LLPC_ASSERT(isInterpolantInputImport);
                        LLPC_ASSERT(callInst.getNumArgOperands() == 5);

                        interpMode = cast<ConstantInt>(callInst.getOperand(3))->getZExtValue();
                        interpLoc = InOutInfo::InterpLocUnknown;

                        pAuxInterpValue = callInst.getOperand(4);
                    }

                    pInput = PatchFsGenericInputImport(pInputTy,
                                                       loc,
                                                       pLocOffset,
                                                       pElemIdx,
                                                       pAuxInterpValue,
                                                       interpMode,
                                                       interpLoc,
                                                       &callInst);
                    break;
                }
            case ShaderStageCompute:
                {
                    LLPC_NEVER_CALLED();
                    break;
                }
            default:
                {
                    LLPC_NEVER_CALLED();
                    break;
                }
            }
        }

        callInst.replaceAllUsesWith(pInput);
    }
    else if (isImport && isOutput)
    {
        // Output imports
        LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);

        Value* pOutput = nullptr;
        Type* pOutputTy = callInst.getType();

        // Generic value (location or SPIR-V built-in ID)
        uint32_t value = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();

        LLVM_DEBUG(dbgs() << "Find output import call: builtin = " << isBuiltInOutputImport
                     << " value = " << value << "\n");

        m_importCalls.push_back(&callInst);

        if (isBuiltInOutputImport)
        {
            const uint32_t builtInId = value;

            LLPC_ASSERT(callInst.getNumArgOperands() == 3);
            Value* pElemIdx = IsDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);
            Value* pVertexIdx = IsDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);

            pOutput = PatchTcsBuiltInOutputImport(pOutputTy, builtInId, pElemIdx, pVertexIdx, &callInst);
        }
        else
        {
            LLPC_ASSERT(isGenericOutputImport);

            uint32_t loc = InvalidValue;

            // NOTE: If location offset is a constant, we have to add it to the unmapped location before querying
            // the mapped location. Meanwhile, we have to adjust the location offset to 0 (rebase it).
            Value* pLocOffset = callInst.getOperand(1);
            if (isa<ConstantInt>(pLocOffset))
            {
                auto locOffset = cast<ConstantInt>(pLocOffset)->getZExtValue();
                value += locOffset;
                pLocOffset = ConstantInt::get(Type::getInt32Ty(*m_pContext), 0);
            }

            // NOTE: For generic outputs of tessellation control shader, they could be per-patch ones.
            if (pResUsage->inOutUsage.outputLocMap.find(value) != pResUsage->inOutUsage.outputLocMap.end())
            {
                loc = pResUsage->inOutUsage.outputLocMap[value];
            }
            else
            {
                LLPC_ASSERT(pResUsage->inOutUsage.perPatchOutputLocMap.find(value) !=
                            pResUsage->inOutUsage.perPatchOutputLocMap.end());
                loc = pResUsage->inOutUsage.perPatchOutputLocMap[value];
            }
            LLPC_ASSERT(loc != InvalidValue);

            LLPC_ASSERT(callInst.getNumArgOperands() == 4);
            auto pElemIdx = callInst.getOperand(2);
            LLPC_ASSERT(IsDontCareValue(pElemIdx) == false);
            auto pVertexIdx = IsDontCareValue(callInst.getOperand(3)) ? nullptr : callInst.getOperand(3);

            pOutput = PatchTcsGenericOutputImport(pOutputTy, loc, pLocOffset, pElemIdx, pVertexIdx, &callInst);

        }

        callInst.replaceAllUsesWith(pOutput);
    }
    else if (isExport)
    {
        // Output exports
        LLPC_ASSERT(isOutput);

        Value* pOutput = callInst.getOperand(callInst.getNumArgOperands() - 1); // Last argument

        // Generic value (location or SPIR-V built-in ID or XFB buffer ID)
        uint32_t value = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();

        LLVM_DEBUG(dbgs() << "Find output export call: builtin = " << isBuiltInOutputExport
                          << " value = " << value << "\n");

        m_exportCalls.push_back(&callInst);

        if (isXfbOutputExport)
        {
            uint32_t xfbBuffer = value;
            LLPC_ASSERT(xfbBuffer < MaxTransformFeedbackBuffers);

            uint32_t xfbOffset = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
            uint32_t xfbExtraOffset = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();

            // NOTE: Transform feedback output will be done in last vertex-processing shader stage.
            switch (m_shaderStage)
            {
            case ShaderStageVertex:
                {
                    // No TS/GS pipeline, VS is the last stage
                    if ((m_hasGs == false) && (m_hasTs == false))
                    {
                        PatchXfbOutputExport(pOutput, xfbBuffer, xfbOffset, xfbExtraOffset, &callInst);
                    }
                    break;
                }
            case ShaderStageTessEval:
                {
                    // TS-only pipeline, TES is the last stage
                    if (m_hasGs == false)
                    {
                        PatchXfbOutputExport(pOutput, xfbBuffer, xfbOffset, xfbExtraOffset, &callInst);
                    }
                    break;
                }
            case ShaderStageGeometry:
                {
                    // Do nothing, transform feedback output is done in copy shader
                    break;
                }
            case ShaderStageCopyShader:
                {
                    // TS-GS or GS-only pipeline, copy shader is the last stage
                    PatchXfbOutputExport(pOutput, xfbBuffer, xfbOffset, xfbExtraOffset, &callInst);
                    break;
                }
            default:
                {
                    LLPC_NEVER_CALLED();
                    break;
                }
            }
        }
        else if (isBuiltInOutputExport)
        {
            const uint32_t builtInId = value;

            switch (m_shaderStage)
            {
            case ShaderStageVertex:
                {
                    PatchVsBuiltInOutputExport(pOutput, builtInId, &callInst);
                    break;
                }
            case ShaderStageTessControl:
                {
                    LLPC_ASSERT(callInst.getNumArgOperands() == 4);
                    Value* pElemIdx = IsDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);
                    Value* pVertexIdx = IsDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);

                    PatchTcsBuiltInOutputExport(pOutput, builtInId, pElemIdx, pVertexIdx, &callInst);
                    break;
                }
            case ShaderStageTessEval:
                {
                    PatchTesBuiltInOutputExport(pOutput, builtInId, &callInst);
                    break;
                }
            case ShaderStageGeometry:
                {
                    PatchGsBuiltInOutputExport(pOutput, builtInId, pResUsage->inOutUsage.gs.rasterStream, &callInst);
                    break;
                }
            case ShaderStageFragment:
                {
                    PatchFsBuiltInOutputExport(pOutput, builtInId, &callInst);
                    break;
                }
            case ShaderStageCopyShader:
                {
                    PatchCopyShaderBuiltInOutputExport(pOutput, builtInId, &callInst);
                    break;
                }
            case ShaderStageCompute:
                {
                    LLPC_NEVER_CALLED();
                    break;
                }
            default:
                {
                    LLPC_NEVER_CALLED();
                    break;
                }
            }
        }
        else
        {
            LLPC_ASSERT(isGenericOutputExport);

            bool exist = false;
            uint32_t loc = InvalidValue;
            Value* pLocOffset = nullptr;

            if (m_shaderStage == ShaderStageTessControl)
            {
                // NOTE: If location offset is a constant, we have to add it to the unmapped location before querying
                // the mapped location. Meanwhile, we have to adjust the location offset to 0 (rebase it).
                pLocOffset = callInst.getOperand(1);
                if (isa<ConstantInt>(pLocOffset))
                {
                    auto locOffset = cast<ConstantInt>(pLocOffset)->getZExtValue();
                    value += locOffset;
                    pLocOffset = ConstantInt::get(Type::getInt32Ty(*m_pContext), 0);
                }

                // NOTE: For generic outputs of tessellation control shader, they could be per-patch ones.
                if (pResUsage->inOutUsage.outputLocMap.find(value) != pResUsage->inOutUsage.outputLocMap.end())
                {
                    exist = true;
                    loc = pResUsage->inOutUsage.outputLocMap[value];
                }
                else if (pResUsage->inOutUsage.perPatchOutputLocMap.find(value) !=
                         pResUsage->inOutUsage.perPatchOutputLocMap.end())
                {
                    exist = true;
                    loc = pResUsage->inOutUsage.perPatchOutputLocMap[value];
                }
            }
            else if (m_shaderStage == ShaderStageCopyShader)
            {
                exist = true;
                loc = value;
            }
            else if (m_shaderStage == ShaderStageGeometry)
            {
                LLPC_ASSERT(callInst.getNumArgOperands() == 4);

                GsOutLocInfo outLocInfo = {};
                outLocInfo.location  = value;
                outLocInfo.isBuiltIn = false;
                outLocInfo.streamId  = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();

                if (pResUsage->inOutUsage.outputLocMap.find(outLocInfo.u32All) != pResUsage->inOutUsage.outputLocMap.end())
                {
                    exist = true;
                    loc = pResUsage->inOutUsage.outputLocMap[outLocInfo.u32All];
                }
            }
            else
            {
                if (pResUsage->inOutUsage.outputLocMap.find(value) != pResUsage->inOutUsage.outputLocMap.end())
                {
                    exist = true;
                    loc = pResUsage->inOutUsage.outputLocMap[value];
                }
            }

            if (exist)
            {
                // NOTE: Some outputs are not used by next shader stage. They must have been removed already.
                LLPC_ASSERT(loc != InvalidValue);

                switch (m_shaderStage)
                {
                case ShaderStageVertex:
                    {
                        LLPC_ASSERT(callInst.getNumArgOperands() == 3);
                        const uint32_t compIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
                        PatchVsGenericOutputExport(pOutput, loc, compIdx, &callInst);
                        break;
                    }
                case ShaderStageTessControl:
                    {
                        LLPC_ASSERT(callInst.getNumArgOperands() == 5);

                        auto pElemIdx = callInst.getOperand(2);
                        LLPC_ASSERT(IsDontCareValue(pElemIdx) == false);

                        auto pVertexIdx = IsDontCareValue(callInst.getOperand(3)) ? nullptr : callInst.getOperand(3);

                        PatchTcsGenericOutputExport(pOutput, loc, pLocOffset, pElemIdx, pVertexIdx, &callInst);
                        break;
                    }
                case ShaderStageTessEval:
                    {
                        LLPC_ASSERT(callInst.getNumArgOperands() == 3);
                        const uint32_t compIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
                        PatchTesGenericOutputExport(pOutput, loc, compIdx, &callInst);
                        break;
                    }
                case ShaderStageGeometry:
                    {
                        LLPC_ASSERT(callInst.getNumArgOperands() == 4);
                        const uint32_t compIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
                        const uint32_t streamId = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();
                        PatchGsGenericOutputExport(pOutput, loc, compIdx, streamId, &callInst);
                        break;
                    }
                case ShaderStageFragment:
                    {
                        LLPC_ASSERT(callInst.getNumArgOperands() == 3);
                        const uint32_t compIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
                        PatchFsGenericOutputExport(pOutput, loc, compIdx, &callInst);
                        break;
                    }
                case ShaderStageCopyShader:
                    {
                        PatchCopyShaderGenericOutputExport(pOutput, loc, &callInst);
                        break;
                    }
                case ShaderStageCompute:
                    {
                        LLPC_NEVER_CALLED();
                        break;
                    }
                default:
                    {
                        LLPC_NEVER_CALLED();
                        break;
                    }
                }
            }
        }
    }
    else
    {
        // Other calls relevant to input/output import/export
        if (pCallee->isIntrinsic() && (pCallee->getIntrinsicID() == Intrinsic::amdgcn_s_sendmsg))
        {
            // NOTE: Implicitly store the value of gl_ViewIndex to GS-VS ring buffer before emit calls.
            if (m_pPipelineState->GetInputAssemblyState().enableMultiView)
            {
                LLPC_ASSERT(m_shaderStage == ShaderStageGeometry); // Must be geometry shader

                auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
                auto pViewIndex = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.viewIndex);

                auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);
                auto& builtInOutLocMap = pResUsage->inOutUsage.builtInOutputLocMap;

                LLPC_ASSERT(builtInOutLocMap.find(BuiltInViewIndex) != builtInOutLocMap.end());
                uint32_t loc = builtInOutLocMap[BuiltInViewIndex];

                auto rasterStream = pResUsage->inOutUsage.gs.rasterStream;
                StoreValueToGsVsRing(pViewIndex, loc, 0, rasterStream, &callInst);
            }

            uint32_t emitStream = InvalidValue;

            uint64_t message = cast<ConstantInt>(callInst.getArgOperand(0))->getZExtValue();
            if ((message == GS_EMIT_STREAM0)|| (message == GS_EMIT_STREAM1) ||
                (message == GS_EMIT_STREAM2) || (message == GS_EMIT_STREAM3))
            {
                // NOTE: MSG[9:8] = STREAM_ID
                emitStream = (message & GS_EMIT_CUT_STREAM_ID_MASK) >> GS_EMIT_CUT_STREAM_ID_SHIFT;
            }

            if (emitStream != InvalidValue)
            {
                // Increment emit vertex counter
                auto pEmitCounterPtr = m_pipelineSysValues.Get(m_pEntryPoint)->GetEmitCounterPtr()[emitStream];
                Value* pEmitCounter = new LoadInst(pEmitCounterPtr, "", &callInst);
                pEmitCounter = BinaryOperator::CreateAdd(pEmitCounter,
                                                         ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                         "",
                                                         &callInst);
                new StoreInst(pEmitCounter, pEmitCounterPtr, &callInst);
            }
        }
    }
}

// =====================================================================================================================
// Visits "ret" instruction.
void PatchInOutImportExport::visitReturnInst(
    ReturnInst& retInst)  // [in] "Ret" instruction
{
    // We only handle the "ret" of shader entry point
    if (m_shaderStage == ShaderStageInvalid)
    {
        return;
    }

    const auto nextStage = m_pPipelineState->GetNextShaderStage(m_shaderStage);
    const bool enableXfb = m_pPipelineState->GetShaderResourceUsage(m_shaderStage)->inOutUsage.enableXfb;

    // Whether this shader stage has to use "exp" instructions to export outputs
    const bool useExpInst = (((m_shaderStage == ShaderStageVertex) || (m_shaderStage == ShaderStageTessEval) ||
                              ((m_shaderStage == ShaderStageCopyShader) && (enableXfb == false))) &&
                             ((nextStage == ShaderStageInvalid) || (nextStage == ShaderStageFragment)));

    auto pZero  = ConstantFP::get(Type::getFloatTy(*m_pContext), 0.0);
    auto pOne   = ConstantFP::get(Type::getFloatTy(*m_pContext), 1.0);
    auto pUndef = UndefValue::get(Type::getFloatTy(*m_pContext));

    auto pInsertPos = &retInst;

    if (useExpInst)
    {
        bool usePosition = false;
        bool usePointSize = false;
        bool usePrimitiveId = false;
        bool useLayer = false;
        bool useViewportIndex = false;
        uint32_t clipDistanceCount = 0;
        uint32_t cullDistanceCount = 0;

        auto& inOutUsage = m_pPipelineState->GetShaderResourceUsage(m_shaderStage)->inOutUsage;

        const auto enableMultiView = m_pPipelineState->GetInputAssemblyState().enableMultiView;

        if (m_shaderStage == ShaderStageVertex)
        {
            auto& builtInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
            auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageVertex)->entryArgIdxs.vs;

            usePosition       = builtInUsage.position;
            usePointSize      = builtInUsage.pointSize;
            usePrimitiveId    = builtInUsage.primitiveId;
            useLayer          = builtInUsage.layer;
            useViewportIndex  = builtInUsage.viewportIndex;
            clipDistanceCount = builtInUsage.clipDistance;
            cullDistanceCount = builtInUsage.cullDistance;

            if (enableMultiView)
            {
                // NOTE: If multi-view is enabled, the exported value of gl_Layer is from gl_ViewIndex.
                m_pLayer = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.viewIndex);
            }
        }
        else if (m_shaderStage == ShaderStageTessEval)
        {
            auto& builtInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
            auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageTessEval)->entryArgIdxs.tes;

            usePosition       = builtInUsage.position;
            usePointSize      = builtInUsage.pointSize;
            usePrimitiveId    = builtInUsage.primitiveId;
            useLayer          = builtInUsage.layer;
            useViewportIndex  = builtInUsage.viewportIndex;
            clipDistanceCount = builtInUsage.clipDistance;
            cullDistanceCount = builtInUsage.cullDistance;

            if (enableMultiView)
            {
                // NOTE: If multi-view is enabled, the exported value of gl_Layer is from gl_ViewIndex.
                m_pLayer = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.viewIndex);
            }
        }
        else
        {
            LLPC_ASSERT(m_shaderStage == ShaderStageCopyShader);
            auto& builtInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageCopyShader)->builtInUsage.gs;

            usePosition       = builtInUsage.position;
            usePointSize      = builtInUsage.pointSize;
            usePrimitiveId    = builtInUsage.primitiveId;
            useLayer          = builtInUsage.layer;
            useViewportIndex  = builtInUsage.viewportIndex;
            clipDistanceCount = builtInUsage.clipDistance;
            cullDistanceCount = builtInUsage.cullDistance;
        }

        useLayer = enableMultiView || useLayer;

        // NOTE: If gl_Position is not present in this shader stage, we have to export a dummy one.
        if (usePosition == false)
        {
            Value* args[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_POS_0),  // tgt
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0xF),               // en
                pZero,                                                              // src0
                pZero,                                                              // src1
                pZero,                                                              // src2
                pOne,                                                               // src3
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false),              // done
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false)               // vm
            };
            m_pLastExport =
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
        }

        // Export gl_ClipDistance[] and gl_CullDistance[] before entry-point returns
        if ((clipDistanceCount > 0) || (cullDistanceCount > 0))
        {
            LLPC_ASSERT(clipDistanceCount + cullDistanceCount <= MaxClipCullDistanceCount);

            LLPC_ASSERT((clipDistanceCount == 0) || ((clipDistanceCount > 0) && (m_pClipDistance != nullptr)));
            LLPC_ASSERT((cullDistanceCount == 0) || ((cullDistanceCount > 0) && (m_pCullDistance != nullptr)));

            // Extract elements of gl_ClipDistance[] and gl_CullDistance[]
            std::vector<Value*> clipDistance;
            for (uint32_t i = 0; i < clipDistanceCount; ++i)
            {
                auto pClipDistance = ExtractValueInst::Create(m_pClipDistance, { i }, "", pInsertPos);
                clipDistance.push_back(pClipDistance);
            }

            std::vector<Value*> cullDistance;
            for (uint32_t i = 0; i < cullDistanceCount; ++i)
            {
                auto pCullDistance = ExtractValueInst::Create(m_pCullDistance, { i }, "", pInsertPos);
                cullDistance.push_back(pCullDistance);
            }

            // Merge gl_ClipDistance[] and gl_CullDistance[]
            std::vector<Value*> clipCullDistance;
            for (auto pClipDistance : clipDistance)
            {
                clipCullDistance.push_back(pClipDistance);
            }

            for (auto pCullDistance : cullDistance)
            {
                clipCullDistance.push_back(pCullDistance);
            }

            // Do array padding
            if (clipCullDistance.size() <= 4)
            {
                while (clipCullDistance.size() < 4) // [4 x float]
                {
                    clipCullDistance.push_back(pUndef);
                }
            }
            else
            {
                while (clipCullDistance.size() < 8) // [8 x float]
                {
                    clipCullDistance.push_back(pUndef);
                }
            }

            // NOTE: When gl_PointSize, gl_Layer, or gl_ViewportIndex is used, gl_ClipDistance[] or gl_CullDistance[]
            // should start from pos2.
            uint32_t pos = (usePointSize || useLayer || useViewportIndex) ? EXP_TARGET_POS_2 : EXP_TARGET_POS_1;
            Value* args[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), pos),   // tgt
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0xF),   // en
                clipCullDistance[0],                                    // src0
                clipCullDistance[1],                                    // src1
                clipCullDistance[2],                                    // src2
                clipCullDistance[3],                                    // src3
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false),  // done
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false)   // vm
            };

            // "Done" flag is valid for exporting position 0 ~ 3
            m_pLastExport =
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);

            if (clipCullDistance.size() > 4)
            {
                // Do the second exporting
                Value* args[] = {
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), pos + 1), // tgt
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), 0xF),     // en
                    clipCullDistance[4],                                      // src0
                    clipCullDistance[5],                                      // src1
                    clipCullDistance[6],                                      // src2
                    clipCullDistance[7],                                      // src3
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false),    // done
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false)     // vm
                };
                m_pLastExport =
                    EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
            }

            // NOTE: We have to export gl_ClipDistance[] or gl_CullDistancep[] via generic outputs as well.
            LLPC_ASSERT((nextStage == ShaderStageInvalid) || (nextStage == ShaderStageFragment));

            bool hasClipCullExport = true;
            if (nextStage == ShaderStageFragment)
            {
                const auto& nextBuiltInUsage =
                    m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;

                hasClipCullExport = ((nextBuiltInUsage.clipDistance > 0) || (nextBuiltInUsage.cullDistance > 0));

                if (hasClipCullExport)
                {
                    // NOTE: We adjust the array size of gl_ClipDistance[] and gl_CullDistance[] according to their
                    // usages in fragment shader.
                    clipDistanceCount = std::min(nextBuiltInUsage.clipDistance, clipDistanceCount);
                    cullDistanceCount = std::min(nextBuiltInUsage.cullDistance, cullDistanceCount);

                    clipCullDistance.clear();
                    for (uint32_t i = 0; i < clipDistanceCount; ++i)
                    {
                        clipCullDistance.push_back(clipDistance[i]);
                    }

                    for (uint32_t i = clipDistanceCount; i < nextBuiltInUsage.clipDistance; ++i)
                    {
                        clipCullDistance.push_back(pUndef);
                    }

                    for (uint32_t i = 0; i < cullDistanceCount; ++i)
                    {
                        clipCullDistance.push_back(cullDistance[i]);
                    }

                    // Do array padding
                    if (clipCullDistance.size() <= 4)
                    {
                        while (clipCullDistance.size() < 4) // [4 x float]
                        {
                            clipCullDistance.push_back(pUndef);
                        }
                    }
                    else
                    {
                        while (clipCullDistance.size() < 8) // [8 x float]
                        {
                            clipCullDistance.push_back(pUndef);
                        }
                    }
                }
            }

            if (hasClipCullExport)
            {
                uint32_t loc = InvalidValue;
                if (m_shaderStage == ShaderStageCopyShader)
                {
                    if (inOutUsage.gs.builtInOutLocs.find(BuiltInClipDistance) !=
                        inOutUsage.gs.builtInOutLocs.end())
                    {
                        loc = inOutUsage.gs.builtInOutLocs[BuiltInClipDistance];
                    }
                    else
                    {
                        LLPC_ASSERT(inOutUsage.gs.builtInOutLocs.find(BuiltInCullDistance) !=
                                    inOutUsage.gs.builtInOutLocs.end());
                        loc = inOutUsage.gs.builtInOutLocs[BuiltInCullDistance];
                    }
                }
                else
                {
                    if (inOutUsage.builtInOutputLocMap.find(BuiltInClipDistance) !=
                        inOutUsage.builtInOutputLocMap.end())
                    {
                        loc = inOutUsage.builtInOutputLocMap[BuiltInClipDistance];
                    }
                    else
                    {
                        LLPC_ASSERT(inOutUsage.builtInOutputLocMap.find(BuiltInCullDistance) !=
                            inOutUsage.builtInOutputLocMap.end());
                        loc = inOutUsage.builtInOutputLocMap[BuiltInCullDistance];
                    }
                }

                Value* args[] = {
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_PARAM_0 + loc),  // tgt
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), 0xF),                       // en
                    clipCullDistance[0],                                                        // src0
                    clipCullDistance[1],                                                        // src1
                    clipCullDistance[2],                                                        // src2
                    clipCullDistance[3],                                                        // src3
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false),                      // done
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false)                       // vm
                };
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
                ++inOutUsage.expCount;

                if (clipCullDistance.size() > 4)
                {
                    // Do the second exporting
                    Value* args[] = {
                        ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_PARAM_0 + loc + 1),  // tgt
                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 0xF),                           // en
                        clipCullDistance[4],                                                            // src0
                        clipCullDistance[5],                                                            // src1
                        clipCullDistance[6],                                                            // src2
                        clipCullDistance[7],                                                            // src3
                        ConstantInt::get(Type::getInt1Ty(*m_pContext), false),                          // done
                        ConstantInt::get(Type::getInt1Ty(*m_pContext), false)                           // vm
                    };
                    EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
                    ++inOutUsage.expCount;
                }
            }
        }

        // Export gl_PrimitiveID before entry-point returns
        if (usePrimitiveId)
        {
            bool hasPrimitiveIdExport = false;
            if (nextStage == ShaderStageFragment)
            {
                hasPrimitiveIdExport =
                    m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs.primitiveId;
            }
            else if (nextStage == ShaderStageInvalid)
            {
                if (m_shaderStage == ShaderStageCopyShader)
                {
                    hasPrimitiveIdExport =
                        m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs.primitiveId;
                }
            }

            if (hasPrimitiveIdExport)
            {
                uint32_t loc = InvalidValue;
                if (m_shaderStage == ShaderStageCopyShader)
                {
                    LLPC_ASSERT(inOutUsage.gs.builtInOutLocs.find(BuiltInPrimitiveId) !=
                        inOutUsage.gs.builtInOutLocs.end());
                    loc = inOutUsage.gs.builtInOutLocs[BuiltInPrimitiveId];
                }
                else
                {
                    LLPC_ASSERT(inOutUsage.builtInOutputLocMap.find(BuiltInPrimitiveId) !=
                        inOutUsage.builtInOutputLocMap.end());
                    loc = inOutUsage.builtInOutputLocMap[BuiltInPrimitiveId];
                }

                LLPC_ASSERT(m_pPrimitiveId != nullptr);
                Value* pPrimitiveId = new BitCastInst(m_pPrimitiveId, Type::getFloatTy(*m_pContext), "", pInsertPos);

                Value* args[] = {
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_PARAM_0 + loc),  // tgt
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), 0x1),                       // en
                    pPrimitiveId,                                                               // src0
                    pUndef,                                                                     // src1
                    pUndef,                                                                     // src2
                    pUndef,                                                                     // src3
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false),                      // done
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false)                       // vm
                };
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
                ++inOutUsage.expCount;
            }
        }
        // NOTE: If multi-view is enabled, always do exporting for gl_Layer.
        if ((m_gfxIp.major <= 8) && enableMultiView)
        {
            LLPC_ASSERT(m_pLayer != nullptr);
            AddExportInstForBuiltInOutput(m_pLayer, BuiltInLayer, pInsertPos);
        }

        // Export gl_Layer and gl_ViewportIndex before entry-point returns
        if ((m_gfxIp.major >= 9) && (useLayer || useViewportIndex))
        {
            Value* pViewportIndexAndLayer = ConstantInt::get(Type::getInt32Ty(*m_pContext), 0);

            if (useViewportIndex)
            {
                LLPC_ASSERT(m_pViewportIndex != nullptr);
                pViewportIndexAndLayer = BinaryOperator::CreateShl(m_pViewportIndex,
                                                                   ConstantInt::get(Type::getInt32Ty(*m_pContext), 16),
                                                                   "",
                                                                   pInsertPos);

            }

            if (useLayer)
            {
                LLPC_ASSERT(m_pLayer != nullptr);
                pViewportIndexAndLayer = BinaryOperator::CreateOr(pViewportIndexAndLayer,
                                                                  m_pLayer,
                                                                  "",
                                                                  pInsertPos);
            }

            pViewportIndexAndLayer = new BitCastInst(pViewportIndexAndLayer,
                                                     Type::getFloatTy(*m_pContext),
                                                     "",
                                                     pInsertPos);

            Value* args[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_POS_1),  // tgt
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0x4),               // en
                pUndef,                                                             // src0
                pUndef,                                                             // src1
                pViewportIndexAndLayer,                                             // src2
                pUndef,                                                             // src3
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false),              // done
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false)               // vm
            };

            // "Done" flag is valid for exporting position 0 ~ 3
            m_pLastExport =
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);

            // NOTE: We have to export gl_ViewportIndex via generic outputs as well.
            if (useViewportIndex)
            {
                bool hasViewportIndexExport = true;
                if (nextStage == ShaderStageFragment)
                {
                    const auto& nextBuiltInUsage =
                        m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;

                    hasViewportIndexExport = nextBuiltInUsage.viewportIndex;
                }

                if (hasViewportIndexExport)
                {
                    uint32_t loc = InvalidValue;
                    if (m_shaderStage == ShaderStageCopyShader)
                    {
                        LLPC_ASSERT(inOutUsage.gs.builtInOutLocs.find(BuiltInViewportIndex) !=
                                    inOutUsage.gs.builtInOutLocs.end());
                        loc = inOutUsage.gs.builtInOutLocs[BuiltInViewportIndex];
                    }
                    else
                    {
                        LLPC_ASSERT(inOutUsage.builtInOutputLocMap.find(BuiltInViewportIndex) !=
                                    inOutUsage.builtInOutputLocMap.end());
                        loc = inOutUsage.builtInOutputLocMap[BuiltInViewportIndex];
                    }

                    Value* pViewportIndex = new BitCastInst(m_pViewportIndex,
                                                            Type::getFloatTy(*m_pContext),
                                                            "",
                                                            pInsertPos);

                    Value* args[] = {
                        ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_PARAM_0 + loc),  // tgt
                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 0xF),                       // en
                        pViewportIndex,                                                             // src0
                        pUndef,                                                                     // src1
                        pUndef,                                                                     // src2
                        pUndef,                                                                     // src3
                        ConstantInt::get(Type::getInt1Ty(*m_pContext), false),                      // done
                        ConstantInt::get(Type::getInt1Ty(*m_pContext), false)                       // vm
                    };
                    EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
                    ++inOutUsage.expCount;
                }
            }

            // NOTE: We have to export gl_Layer via generic outputs as well.
            if (useLayer)
            {
                bool hasLayerExport = true;
                if (nextStage == ShaderStageFragment)
                {
                    const auto& nextBuiltInUsage =
                        m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;

                    hasLayerExport = nextBuiltInUsage.layer|| nextBuiltInUsage.viewIndex;
                }

                if (hasLayerExport)
                {
                    uint32_t loc = InvalidValue;
                    if (m_shaderStage == ShaderStageCopyShader)
                    {
                        LLPC_ASSERT(inOutUsage.gs.builtInOutLocs.find(BuiltInLayer) !=
                                    inOutUsage.gs.builtInOutLocs.end() ||
                                    inOutUsage.gs.builtInOutLocs.find(BuiltInViewIndex) !=
                                    inOutUsage.gs.builtInOutLocs.end());

                        loc = enableMultiView ? inOutUsage.gs.builtInOutLocs[BuiltInViewIndex] :
                            inOutUsage.gs.builtInOutLocs[BuiltInLayer];
                    }
                    else
                    {
                        LLPC_ASSERT(inOutUsage.builtInOutputLocMap.find(BuiltInLayer) !=
                                    inOutUsage.builtInOutputLocMap.end() ||
                                    inOutUsage.builtInOutputLocMap.find(BuiltInViewIndex) !=
                                    inOutUsage.builtInOutputLocMap.end());

                        loc = enableMultiView ? inOutUsage.builtInOutputLocMap[BuiltInViewIndex]:
                            inOutUsage.builtInOutputLocMap[BuiltInLayer];
                    }

                    Value* pLayer = new BitCastInst(m_pLayer, Type::getFloatTy(*m_pContext), "", pInsertPos);

                    Value* args[] = {
                        ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_PARAM_0 + loc),  // tgt
                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 0xF),                       // en
                        pLayer,                                                                     // src0
                        pUndef,                                                                     // src1
                        pUndef,                                                                     // src2
                        pUndef,                                                                     // src3
                        ConstantInt::get(Type::getInt1Ty(*m_pContext), false),                      // done
                        ConstantInt::get(Type::getInt1Ty(*m_pContext), false)                       // vm
                    };
                    EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
                    ++inOutUsage.expCount;
                }
            }
        }

#if LLPC_BUILD_GFX10
        // NOTE: For GFX10+, dummy generic output is no longer needed. Field NO_PC_EXPORT of SPI_VS_OUT_CONFIG
        // will control the behavior.
#endif
        if (m_gfxIp.major <= 9)
        {
            // NOTE: If no generic outputs is present in this shader, we have to export a dummy one
            if (inOutUsage.expCount == 0)
            {
                Value* args[] = {
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_PARAM_0),  // tgt
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),                   // en
                    pUndef,                                                               // src0
                    pUndef,                                                               // src1
                    pUndef,                                                               // src2
                    pUndef,                                                               // src3
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false),                // done
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false)                 // vm
                };
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
                ++inOutUsage.expCount;
            }
        }

        if (m_pPipelineState->GetBuilderContext()->BuildingRelocatableElf())
        {
            // If we are building relocatable shaders, it is possible there are
            // generic outputs that are not written to.  We need to count them in
            // the export count.
            auto pResUsage = m_pPipelineState->GetShaderResourceUsage(m_shaderStage);
            for(auto locMap : pResUsage->inOutUsage.outputLocMap)
            {
                if (m_expLocs.count(locMap.second) != 0)
                {
                    continue;
                }
                ++inOutUsage.expCount;
            }
        }
    }
    else if (m_shaderStage == ShaderStageGeometry)
    {
#if LLPC_BUILD_GFX10
        if ((m_pPipelineState->IsGsOnChip() == false) && (m_gfxIp.major >= 10))
        {
            // NOTE: This is a workaround because backend compiler does not provide s_waitcnt_vscnt intrinsic, so we
            // use fence release to generate s_waitcnt vmcnt/s_waitcnt_vscnt before s_sendmsg(MSG_GS_DONE)
            new FenceInst(*m_pContext, AtomicOrdering::Release, SyncScope::System, pInsertPos);
        }
#endif
        auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
        auto pWaveId = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.waveId);
        Value* args[] = {
            ConstantInt::get(Type::getInt32Ty(*m_pContext), GS_DONE),
            pWaveId
        };

        EmitCall("llvm.amdgcn.s.sendmsg", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
    }
    else if (m_shaderStage == ShaderStageFragment)
    {
        if ((m_pFragDepth != nullptr) || (m_pFragStencilRef != nullptr) || (m_pSampleMask != nullptr))
        {
            auto& builtInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
            Value* pFragDepth = pUndef;
            Value* pFragStencilRef = pUndef;
            Value* pSampleMask = pUndef;

            uint32_t channelMask = 0x1; // Always export gl_FragDepth
            if (m_pFragDepth != nullptr)
            {
                LLPC_ASSERT(builtInUsage.fragDepth);
                LLPC_UNUSED(builtInUsage);
                pFragDepth = m_pFragDepth;
            }

            if (m_pFragStencilRef != nullptr)
            {
                LLPC_ASSERT(builtInUsage.fragStencilRef);
                LLPC_UNUSED(builtInUsage);
                channelMask |= 2;
                pFragStencilRef = m_pFragStencilRef;
            }

            if (m_pSampleMask != nullptr)
            {
                LLPC_ASSERT(builtInUsage.sampleMask);
                LLPC_UNUSED(builtInUsage);
                channelMask |= 4;
                pSampleMask = m_pSampleMask;
            }

            Value* args[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_Z),  // tgt
                ConstantInt::get(Type::getInt32Ty(*m_pContext), channelMask),   // en
                pFragDepth,                                                     // src0
                pFragStencilRef,                                                // src1
                pSampleMask,                                                    // src2
                pUndef,                                                         // src3
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false),          // done
                ConstantInt::get(Type::getInt1Ty(*m_pContext), true)            // vm
            };
            m_pLastExport =
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
        }

        // Export fragment colors
        for (uint32_t location = 0; location < MaxColorTargets; ++location)
        {
            auto& expFragColor = m_expFragColors[location];
            if (expFragColor.size() > 0)
            {
                Value* pOutput = nullptr;
                uint32_t compCount = expFragColor.size();
                LLPC_ASSERT(compCount <= 4);

                // Set CB shader mask
                auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment);
                const uint32_t channelMask = ((1 << compCount) - 1);
                const uint32_t origLoc = pResUsage->inOutUsage.fs.outputOrigLocs[location];
                if (origLoc == InvalidValue)
                {
                    continue;
                }

                pResUsage->inOutUsage.fs.cbShaderMask |= (channelMask << (4 * origLoc));

                // Construct exported fragment colors
                if (compCount == 1)
                {
                    pOutput = expFragColor[0];
                }
                else
                {
                    const auto pCompTy = expFragColor[0]->getType();

                    pOutput = UndefValue::get(VectorType::get(pCompTy, compCount));
                    for (uint32_t i = 0; i < compCount; ++i)
                    {
                        LLPC_ASSERT(expFragColor[i]->getType() == pCompTy);
                        pOutput = InsertElementInst::Create(pOutput,
                                                            expFragColor[i],
                                                            ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                            "",
                                                            pInsertPos);
                    }
                }

                // Do fragment color exporting
                auto pExport = m_pFragColorExport->Run(pOutput, location, pInsertPos);
                if (pExport != nullptr)
                {
                    m_pLastExport = cast<CallInst>(pExport);
                }
            }
        }

        // NOTE: If outputs are present in fragment shader, we have to export a dummy one
        auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment);

#if LLPC_BUILD_GFX10
        // NOTE: GFX10 can allow no dummy export when the fragment shader does not have discard operation
        // or ROV (Raster-ordered views)
        pResUsage->inOutUsage.fs.dummyExport = ((m_gfxIp.major < 10) || pResUsage->builtInUsage.fs.discard);
#else
        pResUsage->inOutUsage.fs.dummyExport = true;
#endif
        if ((m_pLastExport == nullptr) && pResUsage->inOutUsage.fs.dummyExport)
        {
            Value* args[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_MRT_0),  // tgt
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0x1),               // en
                pZero,                                                              // src0
                pUndef,                                                             // src1
                pUndef,                                                             // src2
                pUndef,                                                             // src3
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false),              // done
                ConstantInt::get(Type::getInt1Ty(*m_pContext), true)                // vm
            };
            m_pLastExport =
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
        }
    }

    if (m_pLastExport != nullptr)
    {
        // Set "done" flag
        auto exportName = m_pLastExport->getCalledFunction()->getName();
        if (exportName == "llvm.amdgcn.exp.f32")
        {
            m_pLastExport->setOperand(6, ConstantInt::get(Type::getInt1Ty(*m_pContext), true));
        }
        else
        {
            LLPC_ASSERT(exportName == "llvm.amdgcn.exp.compr.v2f16");
            m_pLastExport->setOperand(4, ConstantInt::get(Type::getInt1Ty(*m_pContext), true));
        }
    }
}

// =====================================================================================================================
// Patches import calls for generic inputs of vertex shader.
Value* PatchInOutImportExport::PatchVsGenericInputImport(
    Type*        pInputTy,        // [in] Type of input value
    uint32_t     location,        // Location of the input
    uint32_t     compIdx,         // Index used for vector element indexing
    Instruction* pInsertPos)      // [in] Where to insert the patch instruction
{
    Value* pInput = UndefValue::get(pInputTy);

    // Do vertex fetch operations
    LLPC_ASSERT(m_pVertexFetch != nullptr);
    auto pVertex = m_pVertexFetch->Run(pInputTy, location, compIdx, pInsertPos);

    // Cast vertex fetch results if necessary
    const Type* pVertexTy = pVertex->getType();
    if (pVertexTy != pInputTy)
    {
        LLPC_ASSERT(CanBitCast(pVertexTy, pInputTy));
        pInput = new BitCastInst(pVertex, pInputTy, "", pInsertPos);
    }
    else
    {
        pInput = pVertex;
    }

    return pInput;
}

// =====================================================================================================================
// Patches import calls for generic inputs of tessellation control shader.
Value* PatchInOutImportExport::PatchTcsGenericInputImport(
    Type*        pInputTy,        // [in] Type of input value
    uint32_t     location,        // Base location of the input
    Value*       pLocOffset,      // [in] Relative location offset
    Value*       pCompIdx,        // [in] Index used for vector element indexing
    Value*       pVertexIdx,      // [in] Input array outermost index used for vertex indexing
    Instruction* pInsertPos)      // [in] Where to insert the patch instruction
{
    LLPC_ASSERT((pCompIdx != nullptr) && (pVertexIdx != nullptr));

    auto pLdsOffset = CalcLdsOffsetForTcsInput(pInputTy, location, pLocOffset, pCompIdx, pVertexIdx, pInsertPos);
    return ReadValueFromLds(false, pInputTy, pLdsOffset, pInsertPos);
}

// =====================================================================================================================
// Patches import calls for generic inputs of tessellation evaluation shader.
Value* PatchInOutImportExport::PatchTesGenericInputImport(
    Type*        pInputTy,        // [in] Type of input value
    uint32_t     location,        // Base location of the input
    Value*       pLocOffset,      // [in] Relative location offset
    Value*       pCompIdx,        // [in] Index used for vector element indexing
    Value*       pVertexIdx,      // [in] Input array outermost index used for vertex indexing (could be null)
    Instruction* pInsertPos)      // [in] Where to insert the patch instruction
{
    LLPC_ASSERT(pCompIdx != nullptr);

    auto pLdsOffset = CalcLdsOffsetForTesInput(pInputTy, location, pLocOffset, pCompIdx, pVertexIdx, pInsertPos);
    return ReadValueFromLds(false, pInputTy, pLdsOffset, pInsertPos);
}

// =====================================================================================================================
// Patches import calls for generic inputs of geometry shader.
Value* PatchInOutImportExport::PatchGsGenericInputImport(
    Type*        pInputTy,        // [in] Type of input value
    uint32_t     location,        // Location of the input
    uint32_t     compIdx,         // Index used for vector element indexing
    Value*       pVertexIdx,      // [in] Input array outermost index used for vertex indexing
    Instruction* pInsertPos)      // [in] Where to insert the patch instruction
{
    LLPC_ASSERT(pVertexIdx != nullptr);

    const uint32_t compCount = pInputTy->isVectorTy() ? pInputTy->getVectorNumElements() : 1;
    const uint32_t bitWidth = pInputTy->getScalarSizeInBits();

    Type* pOrigInputTy = pInputTy;

    if (bitWidth == 64)
    {
        compIdx *= 2; // For 64-bit data type, the component indexing must multiply by 2

        // Cast 64-bit data type to float vector
        pInputTy = VectorType::get(Type::getFloatTy(*m_pContext), compCount * 2);
    }
    else
    {
        LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32));
    }

    Value* pInput = LoadValueFromEsGsRing(pInputTy, location, compIdx, pVertexIdx, pInsertPos);

    if (pInputTy != pOrigInputTy)
    {
        // Cast back to oringinal input type
        LLPC_ASSERT(CanBitCast(pInputTy, pOrigInputTy));
        LLPC_ASSERT(pInputTy->isVectorTy());

        pInput = new BitCastInst(pInput, pOrigInputTy, "", pInsertPos);
    }

    return pInput;
}

// =====================================================================================================================
// Patches import calls for generic inputs of fragment shader.
Value* PatchInOutImportExport::PatchFsGenericInputImport(
    Type*        pInputTy,        // [in] Type of input value
    uint32_t     location,        // Base location of the input
    Value*       pLocOffset,      // [in] Relative location offset
    Value*       pCompIdx,        // [in] Index used for vector element indexing (could be null)
    Value*       pAuxInterpValue, // [in] Auxiliary value of interpolation: for non "custom" interpolation, it is the
                                  // explicitly calculated I/J; for "custom" interpolation, it is vertex no. (could be
                                  // null)
    uint32_t     interpMode,      // Interpolation mode
    uint32_t     interpLoc,       // Interpolation location
    Instruction* pInsertPos)      // [in] Where to insert the patch instruction
{
    Value* pInput = UndefValue::get(pInputTy);

    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment);
    auto& interpInfo = pResUsage->inOutUsage.fs.interpInfo;

    const uint32_t locCount = (pInputTy->getPrimitiveSizeInBits() / 8 > SizeOfVec4) ? 2 : 1;
    while (interpInfo.size() <= location + locCount - 1)
    {
        interpInfo.push_back(InvalidFsInterpInfo);
    }
    interpInfo[location] =
    {
        location,
        (interpMode == InOutInfo::InterpModeFlat),
        (interpMode == InOutInfo::InterpModeCustom),
        (pInputTy->getScalarSizeInBits() == 16),
    };

    if (locCount > 1)
    {
        // The input occupies two consecutive locations
        LLPC_ASSERT(locCount == 2);
        interpInfo[location + 1] =
        {
            location + 1,
            (interpMode == InOutInfo::InterpModeFlat),
            (interpMode == InOutInfo::InterpModeCustom),
            false,
        };
    }

    auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageFragment)->entryArgIdxs.fs;
    auto  pPrimMask    = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.primMask);
    Value* pI  = nullptr;
    Value* pJ  = nullptr;

    // Not "flat" and "custom" interpolation
    if ((interpMode != InOutInfo::InterpModeFlat) && (interpMode != InOutInfo::InterpModeCustom))
    {
        auto pIJ = pAuxInterpValue;
        if (pIJ == nullptr)
        {
            if (interpMode == InOutInfo::InterpModeSmooth)
            {
                if (interpLoc == InOutInfo::InterpLocCentroid)
                {
                    pIJ = AdjustCentroidIJ(GetFunctionArgument(m_pEntryPoint, entryArgIdxs.perspInterp.centroid),
                                           GetFunctionArgument(m_pEntryPoint, entryArgIdxs.perspInterp.center),
                                           pInsertPos);
                }
                else if (interpLoc == InOutInfo::InterpLocSample)
                {
                    pIJ = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.perspInterp.sample);
                }
                else
                {
                    LLPC_ASSERT(interpLoc == InOutInfo::InterpLocCenter);
                    pIJ = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.perspInterp.center);
                }
            }
            else
            {
                LLPC_ASSERT(interpMode == InOutInfo::InterpModeNoPersp);
                if (interpLoc == InOutInfo::InterpLocCentroid)
                {
                    pIJ = AdjustCentroidIJ(GetFunctionArgument(m_pEntryPoint, entryArgIdxs.linearInterp.centroid),
                                           GetFunctionArgument(m_pEntryPoint, entryArgIdxs.linearInterp.center),
                                           pInsertPos);
                }
                else if (interpLoc == InOutInfo::InterpLocSample)
                {
                    pIJ = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.linearInterp.sample);
                }
                else
                {
                    LLPC_ASSERT(interpLoc == InOutInfo::InterpLocCenter);
                    pIJ = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.linearInterp.center);
                }
            }
        }
        pI = ExtractElementInst::Create(pIJ, ConstantInt::get(Type::getInt32Ty(*m_pContext), 0), "", pInsertPos);
        pJ = ExtractElementInst::Create(pIJ, ConstantInt::get(Type::getInt32Ty(*m_pContext), 1), "", pInsertPos);
    }

    Attribute::AttrKind attribs[] = {
        Attribute::ReadNone
    };

    Type* pBasicTy = pInputTy->isVectorTy() ? pInputTy->getVectorElementType() : pInputTy;

    const uint32_t compCout = pInputTy->isVectorTy() ? pInputTy->getVectorNumElements() : 1;
    const uint32_t bitWidth = pInputTy->getScalarSizeInBits();
    LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32) || (bitWidth == 64));

    const uint32_t numChannels = ((bitWidth == 64) ? 2 : 1) * compCout;

    Type* pInterpTy = nullptr;
    if (bitWidth == 8)
    {
        LLPC_ASSERT(pInputTy->isIntOrIntVectorTy());
        pInterpTy = Type::getInt8Ty(*m_pContext);
    }
    else if (bitWidth == 16)
    {
        pInterpTy = Type::getHalfTy(*m_pContext);
    }
    else
    {
        pInterpTy = Type::getFloatTy(*m_pContext);
    }
    if (numChannels > 1)
    {
        pInterpTy = VectorType::get(pInterpTy, numChannels);
    }
    Value* pInterp = UndefValue::get(pInterpTy);

    uint32_t startChannel = 0;
    if (pCompIdx != nullptr)
    {
        startChannel = cast<ConstantInt>(pCompIdx)->getZExtValue();
    }

    Value* pLoc = ConstantInt::get(Type::getInt32Ty(*m_pContext), location);
    if (pLocOffset != nullptr)
    {
        pLoc = BinaryOperator::CreateAdd(pLoc, pLocOffset, "", pInsertPos);
        LLPC_ASSERT((startChannel + numChannels) <= 4);
    }

    for (uint32_t i = startChannel; i < startChannel + numChannels; ++i)
    {
        Value* pCompValue = nullptr;

        if ((interpMode != InOutInfo::InterpModeFlat) && (interpMode != InOutInfo::InterpModeCustom))
        {
            LLPC_ASSERT((pBasicTy->isHalfTy() || pBasicTy->isFloatTy()) && (numChannels <= 4));
            LLPC_UNUSED(pBasicTy);

            if (bitWidth == 16)
            {
                Value* args1[] = {
                    pI,                                                     // i
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), i),     // attr_chan
                    pLoc,                                                   // attr
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false),  // high
                    pPrimMask                                               // m0
                };
                pCompValue = EmitCall("llvm.amdgcn.interp.p1.f16",
                                      Type::getFloatTy(*m_pContext),
                                      args1,
                                      attribs,
                                      pInsertPos);

                Value* args2[] = {
                    pCompValue,                                             // p1
                    pJ,                                                     // j
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), i),     // attr_chan
                    pLoc,                                                   // attr
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false),  // high
                    pPrimMask                                               // m0
                };
                pCompValue = EmitCall("llvm.amdgcn.interp.p2.f16",
                                      Type::getHalfTy(*m_pContext),
                                      args2,
                                      attribs,
                                      pInsertPos);
            }
            else
            {
                Value* args1[] = {
                    pI,                                                   // i
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), i),   // attr_chan
                    pLoc,                                                 // attr
                    pPrimMask                                             // m0
                };
                pCompValue = EmitCall("llvm.amdgcn.interp.p1",
                                      Type::getFloatTy(*m_pContext),
                                      args1,
                                      attribs,
                                      pInsertPos);

                Value* args2[] = {
                    pCompValue,                                         // p1
                    pJ,                                                 // j
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), i), // attr_chan
                    pLoc,                                               // attr
                    pPrimMask                                           // m0
                };
                pCompValue = EmitCall("llvm.amdgcn.interp.p2",
                                      Type::getFloatTy(*m_pContext),
                                      args2,
                                      attribs,
                                      pInsertPos);
            }
        }
        else
        {
            InterpParam interpParam = INTERP_PARAM_P0;

            if (interpMode == InOutInfo::InterpModeCustom)
            {
                LLPC_ASSERT(isa<ConstantInt>(pAuxInterpValue));
                uint32_t vertexNo = cast<ConstantInt>(pAuxInterpValue)->getZExtValue();

                switch (vertexNo)
                {
                case 0:
                    interpParam = INTERP_PARAM_P0;
                    break;
                case 1:
                    interpParam = INTERP_PARAM_P10;
                    break;
                case 2:
                    interpParam = INTERP_PARAM_P20;
                    break;
                default:
                    LLPC_NEVER_CALLED();
                    break;
                }
            }
            else
            {
                LLPC_ASSERT(interpMode == InOutInfo::InterpModeFlat);
            }

            Value* args[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), interpParam),           // param
                ConstantInt::get(Type::getInt32Ty(*m_pContext), i % 4),                 // attr_chan
                (pLocOffset != nullptr) ?
                    pLoc :
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), location + i / 4),  // attr
                pPrimMask                                                               // m0
            };
            pCompValue = EmitCall("llvm.amdgcn.interp.mov",
                                  Type::getFloatTy(*m_pContext),
                                  args,
                                  attribs,
                                  pInsertPos);

            if (bitWidth == 8)
            {
                pCompValue = new BitCastInst(pCompValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
                pCompValue = new TruncInst(pCompValue, Type::getInt8Ty(*m_pContext), "", pInsertPos);
            }
            else if (bitWidth == 16)
            {
                pCompValue = new BitCastInst(pCompValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
                pCompValue = new TruncInst(pCompValue, Type::getInt16Ty(*m_pContext), "", pInsertPos);
                pCompValue = new BitCastInst(pCompValue, Type::getHalfTy(*m_pContext), "", pInsertPos);
            }
        }

        if (numChannels == 1)
        {
            pInterp = pCompValue;
        }
        else
        {
            pInterp = InsertElementInst::Create(pInterp,
                                                pCompValue,
                                                ConstantInt::get(Type::getInt32Ty(*m_pContext), i - startChannel),
                                                "",
                                                pInsertPos);
        }
    }

    // Store interpolation results to inputs
    if (pInterpTy == pInputTy)
    {
        pInput = pInterp;
    }
    else
    {
        LLPC_ASSERT(CanBitCast(pInterpTy, pInputTy));
        pInput = new BitCastInst(pInterp, pInputTy, "", pInsertPos);
    }

    return pInput;
}

// =====================================================================================================================
// Patches import calls for generic outputs of tessellation control shader.
Value* PatchInOutImportExport::PatchTcsGenericOutputImport(
    Type*        pOutputTy,       // [in] Type of output value
    uint32_t     location,        // Base location of the output
    Value*       pLocOffset,      // [in] Relative location offset
    Value*       pCompIdx,        // [in] Index used for vector element indexing
    Value*       pVertexIdx,      // [in] Input array outermost index used for vertex indexing (could be null)
    Instruction* pInsertPos)      // [in] Where to insert the patch instruction
{
    LLPC_ASSERT(pCompIdx != nullptr);

    auto pLdsOffset = CalcLdsOffsetForTcsOutput(pOutputTy, location, pLocOffset, pCompIdx, pVertexIdx, pInsertPos);
    return ReadValueFromLds(true, pOutputTy, pLdsOffset, pInsertPos);
}

// =====================================================================================================================
// Patches export calls for generic outputs of vertex shader.
void PatchInOutImportExport::PatchVsGenericOutputExport(
    Value*       pOutput,        // [in] Output value
    uint32_t     location,       // Location of the output
    uint32_t     compIdx,        // Index used for vector element indexing
    Instruction* pInsertPos)     // [in] Where to insert the patch instruction
{
    auto pOutputTy = pOutput->getType();

    m_expLocs.insert(location);

    if (m_hasTs)
    {
        auto pLdsOffset = CalcLdsOffsetForVsOutput(pOutputTy, location, compIdx, pInsertPos);
        WriteValueToLds(pOutput, pLdsOffset, pInsertPos);
    }
    else
    {
        if (m_hasGs)
        {
            LLPC_ASSERT(pOutputTy->isIntOrIntVectorTy() || pOutputTy->isFPOrFPVectorTy());

            const uint32_t bitWidth = pOutputTy->getScalarSizeInBits();
            if (bitWidth == 64)
            {
                // For 64-bit data type, the component indexing must multiply by 2
                compIdx *= 2;

                uint32_t compCount = pOutputTy->isVectorTy() ? pOutputTy->getVectorNumElements() * 2 : 2;

                pOutputTy = VectorType::get(Type::getFloatTy(*m_pContext), compCount);
                pOutput = new BitCastInst(pOutput, pOutputTy, "", pInsertPos);
            }
            else
            {
                LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32));
            }

            StoreValueToEsGsRing(pOutput, location, compIdx, pInsertPos);
        }
        else
        {
            AddExportInstForGenericOutput(pOutput, location, compIdx, pInsertPos);
        }
    }
}

// =====================================================================================================================
// Patches export calls for generic outputs of tessellation control shader.
void PatchInOutImportExport::PatchTcsGenericOutputExport(
    Value*       pOutput,        // [in] Output value
    uint32_t     location,       // Base location of the output
    Value*       pLocOffset,     // [in] Relative location offset
    Value*       pCompIdx,       // [in] Index used for vector element indexing
    Value*       pVertexIdx,     // [in] Input array outermost index used for vertex indexing (could be null)
    Instruction* pInsertPos)     // [in] Where to insert the patch instruction
{
    LLPC_ASSERT(pCompIdx != nullptr);

    Type* pOutputTy = pOutput->getType();
    auto pLdsOffset = CalcLdsOffsetForTcsOutput(pOutputTy, location, pLocOffset, pCompIdx, pVertexIdx, pInsertPos);
    WriteValueToLds(pOutput, pLdsOffset, pInsertPos);
}

// =====================================================================================================================
// Patches export calls for generic outputs of tessellation evaluation shader.
void PatchInOutImportExport::PatchTesGenericOutputExport(
    Value*       pOutput,        // [in] Output value
    uint32_t     location,       // Location of the output
    uint32_t     compIdx,        // Index used for vector element indexing
    Instruction* pInsertPos)     // [in] Where to insert the patch instruction
{
    if (m_hasGs)
    {
        auto pOutputTy = pOutput->getType();
        LLPC_ASSERT(pOutputTy->isIntOrIntVectorTy() || pOutputTy->isFPOrFPVectorTy());

        const uint32_t bitWidth = pOutputTy->getScalarSizeInBits();
        if (bitWidth == 64)
        {
            // For 64-bit data type, the component indexing must multiply by 2
            compIdx *= 2;

            uint32_t compCount = pOutputTy->isVectorTy() ? pOutputTy->getVectorNumElements() * 2 : 2;
            pOutputTy = VectorType::get(Type::getFloatTy(*m_pContext), compCount);

            pOutput = new BitCastInst(pOutput, pOutputTy, "", pInsertPos);
        }
        else
        {
            LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32));
        }

        StoreValueToEsGsRing(pOutput, location, compIdx, pInsertPos);
    }
    else
    {
        AddExportInstForGenericOutput(pOutput, location, compIdx, pInsertPos);
    }
}

// =====================================================================================================================
// Patches export calls for generic outputs of geometry shader.
void PatchInOutImportExport::PatchGsGenericOutputExport(
    Value*       pOutput,        // [in] Output value
    uint32_t     location,       // Location of the output
    uint32_t     compIdx,        // Index used for vector element indexing
    uint32_t     streamId,       // ID of output vertex stream
    Instruction* pInsertPos)     // [in] Where to insert the patch instruction
{
    auto pOutputTy = pOutput->getType();

    // Cast double or double vector to float vector.
    const uint32_t bitWidth = pOutputTy->getScalarSizeInBits();
    if (bitWidth == 64)
    {
        // For 64-bit data type, the component indexing must multiply by 2
        compIdx *= 2;

        if (pOutputTy->isVectorTy())
        {
            pOutputTy = VectorType::get(Type::getFloatTy(*m_pContext), pOutputTy->getVectorNumElements() * 2);
        }
        else
        {
            pOutputTy = VectorType::get(Type::getFloatTy(*m_pContext), 2);
        }

        pOutput = new BitCastInst(pOutput, pOutputTy, "", pInsertPos);
    }
    else
    {
        LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32));
    }

    const uint32_t compCount = pOutputTy->isVectorTy() ? pOutputTy->getVectorNumElements() : 1;
    // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend BYTE/WORD to DWORD and
    // store DWORD to GS-VS ring. So for 8-bit/16-bit data type, the actual byte size is based on number of DWORDs.
    uint32_t byteSize = (pOutputTy->getScalarSizeInBits() / 8) * compCount;
    if ((bitWidth == 8) || (bitWidth == 16))
    {
        byteSize *= (32 / bitWidth);
    }

    LLPC_ASSERT(compIdx <= 4);

    // Field "genericOutByteSizes" now gets set when generating the copy shader. Just assert that we agree on the
    // byteSize.
    auto& genericOutByteSizes =
        m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.genericOutByteSizes;
    LLPC_ASSERT(genericOutByteSizes[streamId][location][compIdx] == byteSize);
    LLPC_UNUSED(genericOutByteSizes);

    StoreValueToGsVsRing(pOutput, location, compIdx, streamId, pInsertPos);
}

// =====================================================================================================================
// Patches export calls for generic outputs of fragment shader.
void PatchInOutImportExport::PatchFsGenericOutputExport(
    Value*       pOutput,         // [in] Output value
    uint32_t     location,        // Location of the output
    uint32_t     compIdx,         // Index used for vector element indexing
    Instruction* pInsertPos)      // [in] Where to insert the patch instruction
{
    Type* pOutputTy = pOutput->getType();

    const uint32_t bitWidth = pOutputTy->getScalarSizeInBits();
    LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32));
    LLPC_UNUSED(bitWidth);

    auto pCompTy = pOutputTy->isVectorTy() ? pOutputTy->getVectorElementType() : pOutputTy;
    uint32_t compCount = pOutputTy->isVectorTy() ? pOutputTy->getVectorNumElements() : 1;

    std::vector<Value*> outputComps;
    for (uint32_t i = 0; i < compCount; ++i)
    {
        Value* pOutputComp = nullptr;
        if (compCount == 1)
        {
            pOutputComp = pOutput;
        }
        else
        {
            pOutputComp = ExtractElementInst::Create(pOutput,
                                                     ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                     "",
                                                     pInsertPos);
        }

        outputComps.push_back(pOutputComp);
    }

    LLPC_ASSERT(location < MaxColorTargets);
    auto& expFragColor = m_expFragColors[location];

    while (compIdx + compCount > expFragColor.size())
    {
        expFragColor.push_back(UndefValue::get(pCompTy));
    }

    for (uint32_t i = 0; i < compCount; ++i)
    {
        expFragColor[compIdx + i] = outputComps[i];
    }
}

// =====================================================================================================================
// Patches import calls for built-in inputs of vertex shader.
Value* PatchInOutImportExport::PatchVsBuiltInInputImport(
    Type*        pInputTy,      // [in] Type of input value
    uint32_t     builtInId,     // ID of the built-in variable
    Instruction* pInsertPos)    // [in] Where to insert the patch instruction
{
    auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageVertex)->entryArgIdxs.vs;

    switch (builtInId)
    {
    case BuiltInVertexIndex:
        return m_pVertexFetch->GetVertexIndex();
    case BuiltInInstanceIndex:
        return m_pVertexFetch->GetInstanceIndex();
    case BuiltInBaseVertex:
        return GetFunctionArgument(m_pEntryPoint, entryArgIdxs.baseVertex);
    case BuiltInBaseInstance:
        return GetFunctionArgument(m_pEntryPoint, entryArgIdxs.baseInstance);
    case BuiltInDrawIndex:
        return GetFunctionArgument(m_pEntryPoint, entryArgIdxs.drawIndex);
    case BuiltInViewIndex:
        return GetFunctionArgument(m_pEntryPoint, entryArgIdxs.viewIndex);
    case BuiltInSubgroupSize:
        return ConstantInt::get(Type::getInt32Ty(*m_pContext), m_pPipelineState->GetShaderWaveSize(m_shaderStage));
    case BuiltInSubgroupLocalInvocationId:
        return GetSubgroupLocalInvocationId(pInsertPos);
    case BuiltInDeviceIndex:
        return ConstantInt::get(Type::getInt32Ty(*m_pContext), m_pPipelineState->GetDeviceIndex());
    default:
        LLPC_NEVER_CALLED();
        return UndefValue::get(pInputTy);
    }
}

// =====================================================================================================================
// Patches import calls for built-in inputs of tessellation control shader.
Value* PatchInOutImportExport::PatchTcsBuiltInInputImport(
    Type*        pInputTy,      // [in] Type of input value
    uint32_t     builtInId,     // ID of the built-in variable
    Value*       pElemIdx,      // [in] Index used for array/vector element indexing (could be null)
    Value*       pVertexIdx,    // [in] Input array outermost index used for vertex indexing (could be null)
    Instruction* pInsertPos)    // [in] Where to insert the patch instruction
{
    Value* pInput = UndefValue::get(pInputTy);

    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl);
    auto& inoutUsage = pResUsage->inOutUsage;
    auto& builtInInLocMap = inoutUsage.builtInInputLocMap;

    switch (builtInId)
    {
    case BuiltInPosition:
        {
            LLPC_ASSERT(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
            const uint32_t loc = builtInInLocMap[builtInId];

            auto pLdsOffset = CalcLdsOffsetForTcsInput(pInputTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
            pInput = ReadValueFromLds(false, pInputTy, pLdsOffset, pInsertPos);

            break;
        }
    case BuiltInPointSize:
        {
            LLPC_ASSERT(pElemIdx == nullptr);
            LLPC_ASSERT(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
            const uint32_t loc = builtInInLocMap[builtInId];

            auto pLdsOffset = CalcLdsOffsetForTcsInput(pInputTy, loc, nullptr, nullptr, pVertexIdx, pInsertPos);
            pInput = ReadValueFromLds(false, pInputTy, pLdsOffset, pInsertPos);

            break;
        }
    case BuiltInClipDistance:
    case BuiltInCullDistance:
        {
            LLPC_ASSERT(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
            const uint32_t loc = builtInInLocMap[builtInId];

            if (pElemIdx == nullptr)
            {
                // gl_ClipDistanceIn[]/gl_CullDistanceIn[] is treated as 2 x vec4
                LLPC_ASSERT(pInputTy->isArrayTy());

                auto pElemTy = pInputTy->getArrayElementType();
                for (uint32_t i = 0; i < pInputTy->getArrayNumElements(); ++i)
                {
                    auto pElemIdx = ConstantInt::get(Type::getInt32Ty(*m_pContext), i);
                    auto pLdsOffset =
                        CalcLdsOffsetForTcsInput(pElemTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                    auto pElem = ReadValueFromLds(false, pElemTy, pLdsOffset, pInsertPos);
                    pInput = InsertValueInst::Create(pInput, pElem, { i }, "", pInsertPos);
                }
            }
            else
            {
                auto pLdsOffset = CalcLdsOffsetForTcsInput(pInputTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                pInput = ReadValueFromLds(false, pInputTy, pLdsOffset, pInsertPos);
            }

            break;
        }
    case BuiltInPatchVertices:
        {
            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext),
                                      m_pPipelineState->GetInputAssemblyState().patchControlPoints);
            break;
        }
    case BuiltInPrimitiveId:
        {
            pInput = m_pipelineSysValues.Get(m_pEntryPoint)->GetPrimitiveId();
            break;
        }
    case BuiltInInvocationId:
        {
            pInput = m_pipelineSysValues.Get(m_pEntryPoint)->GetInvocationId();
            break;
        }
    case BuiltInSubgroupSize:
        {
            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext), m_pPipelineState->GetShaderWaveSize(m_shaderStage));
            break;
        }
    case BuiltInSubgroupLocalInvocationId:
        {
            pInput = GetSubgroupLocalInvocationId(pInsertPos);
            break;
        }
    case BuiltInDeviceIndex:
        {
            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext), m_pPipelineState->GetDeviceIndex());
            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }

    return pInput;
}

// =====================================================================================================================
// Patches import calls for built-in inputs of tessellation evaluation shader.
Value* PatchInOutImportExport::PatchTesBuiltInInputImport(
    Type*        pInputTy,      // [in] Type of input value
    uint32_t     builtInId,     // ID of the built-in variable
    Value*       pElemIdx,      // [in] Index used for array/vector element indexing (could be null)
    Value*       pVertexIdx,    // [in] Input array outermost index used for vertex indexing (could be null)
    Instruction* pInsertPos)    // [in] Where to insert the patch instruction
{
    Value* pInput = UndefValue::get(pInputTy);

    auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageTessEval)->entryArgIdxs.tes;

    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessEval);
    auto& inOutUsage = pResUsage->inOutUsage;
    auto& builtInInLocMap = inOutUsage.builtInInputLocMap;
    auto& perPatchBuiltInInLocMap = inOutUsage.perPatchBuiltInInputLocMap;

    switch (builtInId)
    {
    case BuiltInPosition:
        {
            LLPC_ASSERT(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
            const uint32_t loc = builtInInLocMap[builtInId];

            auto pLdsOffset = CalcLdsOffsetForTesInput(pInputTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
            pInput = ReadValueFromLds(false, pInputTy, pLdsOffset, pInsertPos);

            break;
        }
    case BuiltInPointSize:
        {
            LLPC_ASSERT(pElemIdx == nullptr);
            LLPC_ASSERT(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
            const uint32_t loc = builtInInLocMap[builtInId];

            auto pLdsOffset = CalcLdsOffsetForTesInput(pInputTy, loc, nullptr, nullptr, pVertexIdx, pInsertPos);
            pInput = ReadValueFromLds(false, pInputTy, pLdsOffset, pInsertPos);

            break;
        }
    case BuiltInClipDistance:
    case BuiltInCullDistance:
        {
            LLPC_ASSERT(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
            const uint32_t loc = builtInInLocMap[builtInId];

            if (pElemIdx == nullptr)
            {
                // gl_ClipDistanceIn[]/gl_CullDistanceIn[] is treated as 2 x vec4
                LLPC_ASSERT(pInputTy->isArrayTy());

                auto pElemTy = pInputTy->getArrayElementType();
                for (uint32_t i = 0; i < pInputTy->getArrayNumElements(); ++i)
                {
                    auto pElemIdx = ConstantInt::get(Type::getInt32Ty(*m_pContext), i);
                    auto pLdsOffset =
                        CalcLdsOffsetForTesInput(pElemTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                    auto pElem = ReadValueFromLds(false, pElemTy, pLdsOffset, pInsertPos);
                    pInput = InsertValueInst::Create(pInput, pElem, { i }, "", pInsertPos);
                }
            }
            else
            {
                auto pLdsOffset = CalcLdsOffsetForTesInput(pInputTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                pInput = ReadValueFromLds(false, pInputTy, pLdsOffset, pInsertPos);
            }

            break;
        }
    case BuiltInPatchVertices:
        {
            uint32_t patchVertices = MaxTessPatchVertices;
            const bool hasTcs = m_pPipelineState->HasShaderStage(ShaderStageTessControl);
            if (hasTcs)
            {
                patchVertices = m_pPipelineState->GetShaderModes()->GetTessellationMode().outputVertices;
            }

            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext), patchVertices);

            break;
        }
    case BuiltInPrimitiveId:
        {
            pInput = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.patchId);
            break;
        }
    case BuiltInTessCoord:
        {
            auto pTessCoord = m_pipelineSysValues.Get(m_pEntryPoint)->GetTessCoord();

            if (pElemIdx != nullptr)
            {
                pInput = ExtractElementInst::Create(pTessCoord, pElemIdx, "", pInsertPos);
            }
            else
            {
                pInput = pTessCoord;
            }

            break;
        }
    case BuiltInTessLevelOuter:
    case BuiltInTessLevelInner:
        {
            LLPC_ASSERT(perPatchBuiltInInLocMap.find(builtInId) != perPatchBuiltInInLocMap.end());
            uint32_t loc = perPatchBuiltInInLocMap[builtInId];

            if (pElemIdx == nullptr)
            {
                // gl_TessLevelOuter[4] is treated as vec4
                // gl_TessLevelInner[2] is treated as vec2
                LLPC_ASSERT(pInputTy->isArrayTy());

                auto pElemTy = pInputTy->getArrayElementType();
                for (uint32_t i = 0; i < pInputTy->getArrayNumElements(); ++i)
                {
                    auto pElemIdx = ConstantInt::get(Type::getInt32Ty(*m_pContext), i);
                    auto pLdsOffset =
                        CalcLdsOffsetForTesInput(pElemTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                    auto pElem = ReadValueFromLds(false, pElemTy, pLdsOffset, pInsertPos);
                    pInput = InsertValueInst::Create(pInput, pElem, { i }, "", pInsertPos);
                }
            }
            else
            {
                auto pLdsOffset = CalcLdsOffsetForTesInput(pInputTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                pInput = ReadValueFromLds(false, pInputTy, pLdsOffset, pInsertPos);
            }

            break;
        }
    case BuiltInViewIndex:
        {
            pInput = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.viewIndex);
            break;
        }
    case BuiltInSubgroupSize:
        {
            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext), m_pPipelineState->GetShaderWaveSize(m_shaderStage));
            break;
        }
    case BuiltInSubgroupLocalInvocationId:
        {
            pInput = GetSubgroupLocalInvocationId(pInsertPos);
            break;
        }
    case BuiltInDeviceIndex:
        {
            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext), m_pPipelineState->GetDeviceIndex());
            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }

    return pInput;
}

// =====================================================================================================================
// Patches import calls for built-in inputs of geometry shader.
Value* PatchInOutImportExport::PatchGsBuiltInInputImport(
    Type*        pInputTy,      // [in] Type of input value
    uint32_t     builtInId,     // ID of the built-in variable
    Value*       pVertexIdx,    // [in] Input array outermost index used for vertex indexing (could be null)
    Instruction* pInsertPos)    // [in] Where to insert the patch instruction
{
    Value* pInput = nullptr;

    auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
    auto& inOutUsage   = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry)->inOutUsage;

    uint32_t loc = inOutUsage.builtInInputLocMap[builtInId];
    LLPC_ASSERT(loc != InvalidValue);

    switch (builtInId)
    {
    case BuiltInPosition:
    case BuiltInPointSize:
    case BuiltInClipDistance:
    case BuiltInCullDistance:
        {
            pInput = LoadValueFromEsGsRing(pInputTy, loc, 0, pVertexIdx, pInsertPos);
            break;
        }
    case BuiltInPrimitiveId:
        {
            pInput = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.primitiveId);
            break;
        }
    case BuiltInInvocationId:
        {
            pInput = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.invocationId);
            break;
        }
    case BuiltInViewIndex:
        {
            pInput = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.viewIndex);
            break;
        }
    case BuiltInSubgroupSize:
        {
            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext), m_pPipelineState->GetShaderWaveSize(m_shaderStage));
            break;
        }
    case BuiltInSubgroupLocalInvocationId:
        {
            pInput = GetSubgroupLocalInvocationId(pInsertPos);
            break;
        }
    case BuiltInDeviceIndex:
        {
            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext), m_pPipelineState->GetDeviceIndex());
            break;
        }
    // Handle internal-use built-ins
    case BuiltInWaveId:
        {
            pInput = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.waveId);
            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }

    return pInput;
}

// =====================================================================================================================
// Patches import calls for built-in inputs of fragment shader.
Value* PatchInOutImportExport::PatchFsBuiltInInputImport(
    Type*        pInputTy,      // [in] Type of input value
    uint32_t     builtInId,     // ID of the built-in variable
    Value*       pSampleId,     // [in] Sample ID; only needed for BuiltInSamplePosOffset
    Instruction* pInsertPos)    // [in] Where to insert the patch instruction
{
    Value* pInput = UndefValue::get(pInputTy);

    auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageFragment)->entryArgIdxs.fs;
    auto& builtInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
    auto& inOutUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment)->inOutUsage;

    Attribute::AttrKind attribs[] = {
        Attribute::ReadNone
    };

    switch (builtInId)
    {
    case BuiltInSampleMask:
        {
            LLPC_ASSERT(pInputTy->isArrayTy());

            auto pSampleCoverage = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.sampleCoverage);
            auto pAncillary = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.ancillary);

            // gl_SampleID = Ancillary[11:8]
            Value* args[] = {
                pAncillary,
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 8),
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 4)
            };
            auto pSampleId =
                EmitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_pContext), args, NoAttrib, pInsertPos);

            auto pSampleMaskIn = pSampleCoverage;
            if (m_pPipelineState->GetRasterizerState().perSampleShading)
            {
                // gl_SampleMaskIn[0] = (SampleCoverage & (1 << gl_SampleID))
                pSampleMaskIn = BinaryOperator::CreateShl(ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                          pSampleId,
                                                          "",
                                                          pInsertPos);
                pSampleMaskIn = BinaryOperator::CreateAnd(pSampleCoverage, pSampleMaskIn, "", pInsertPos);
            }

            // NOTE: Only gl_SampleMaskIn[0] is valid for us.
            pInput = InsertValueInst::Create(pInput, pSampleMaskIn, { 0 }, "", pInsertPos);

            break;
        }
    case BuiltInFragCoord:
        {
            // TODO: Support layout qualifiers "pixel_center_integer" and "origin_upper_left".
            Value* fragCoord[4] =
            {
                GetFunctionArgument(m_pEntryPoint, entryArgIdxs.fragCoord.x),
                GetFunctionArgument(m_pEntryPoint, entryArgIdxs.fragCoord.y),
                GetFunctionArgument(m_pEntryPoint, entryArgIdxs.fragCoord.z),
                GetFunctionArgument(m_pEntryPoint, entryArgIdxs.fragCoord.w),
            };

            fragCoord[3] =
                EmitCall("llvm.amdgcn.rcp.f32", Type::getFloatTy(*m_pContext), { fragCoord[3] }, attribs, pInsertPos);

            for (uint32_t i = 0; i < 4; ++i)
            {
                pInput = InsertElementInst::Create(pInput,
                                                   fragCoord[i],
                                                   ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                   "",
                                                   pInsertPos);
            }

            break;
        }
    case BuiltInFrontFacing:
        {
            auto pFrontFacing = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.frontFacing);
            pInput = new ICmpInst(pInsertPos,
                                  ICmpInst::ICMP_NE,
                                  pFrontFacing,
                                  ConstantInt::get(Type::getInt32Ty(*m_pContext),
                                  0));
            pInput = CastInst::CreateIntegerCast(pInput, pInputTy, false, "", pInsertPos);
            break;
        }
    case BuiltInPointCoord:
        {
            LLPC_ASSERT(inOutUsage.builtInInputLocMap.find(BuiltInPointCoord) != inOutUsage.builtInInputLocMap.end());
            const uint32_t loc = inOutUsage.builtInInputLocMap[BuiltInPointCoord];

            auto& interpInfo = inOutUsage.fs.interpInfo;
            while (interpInfo.size() <= loc)
            {
                interpInfo.push_back(InvalidFsInterpInfo);
            }
            interpInfo[loc] = { loc, false, false, false };

            // Emulation for "in vec2 gl_PointCoord"
            const bool perSampleShading = m_pPipelineState->GetRasterizerState().perSampleShading;
            pInput = PatchFsGenericInputImport(pInputTy,
                                               loc,
                                               nullptr,
                                               nullptr,
                                               nullptr,
                                               InOutInfo::InterpModeSmooth,
                                               perSampleShading ? InOutInfo::InterpLocSample :
                                                                  InOutInfo::InterpLocCenter,
                                               pInsertPos);
            break;
        }
    case BuiltInHelperInvocation:
        {
            pInput = EmitCall("llvm.amdgcn.ps.live", Type::getInt1Ty(*m_pContext), {}, Attribute::ReadNone, pInsertPos);
            pInput = BinaryOperator::CreateNot(pInput, "", pInsertPos);
            pInput = CastInst::CreateIntegerCast(pInput, pInputTy, false, "", pInsertPos);
            break;
        }
    case BuiltInPrimitiveId:
    case BuiltInLayer:
    case BuiltInViewportIndex:
    case BuiltInViewIndex:
        {
            uint32_t loc = InvalidValue;

            if (builtInId == BuiltInPrimitiveId)
            {
                LLPC_ASSERT(inOutUsage.builtInInputLocMap.find(BuiltInPrimitiveId) !=
                            inOutUsage.builtInInputLocMap.end());
                loc = inOutUsage.builtInInputLocMap[BuiltInPrimitiveId];
            }
            else if (builtInId == BuiltInLayer)
            {
                LLPC_ASSERT(inOutUsage.builtInInputLocMap.find(BuiltInLayer) != inOutUsage.builtInInputLocMap.end());
                loc = inOutUsage.builtInInputLocMap[BuiltInLayer];
            }
            else if (builtInId == BuiltInViewIndex)
            {
                LLPC_ASSERT(inOutUsage.builtInInputLocMap.find(BuiltInViewIndex) != inOutUsage.builtInInputLocMap.end());
                loc = inOutUsage.builtInInputLocMap[BuiltInViewIndex];
            }
            else
            {
                LLPC_ASSERT(builtInId == BuiltInViewportIndex);

                LLPC_ASSERT(inOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) !=
                            inOutUsage.builtInInputLocMap.end());
                loc = inOutUsage.builtInInputLocMap[BuiltInViewportIndex];
            }

            auto& interpInfo = inOutUsage.fs.interpInfo;
            while (interpInfo.size() <= loc)
            {
                interpInfo.push_back(InvalidFsInterpInfo);
            }
            interpInfo[loc] = { loc, true, false }; // Flat interpolation

            // Emulation for "in int gl_PrimitiveID" or "in int gl_Layer" or "in int gl_ViewportIndex"
            // or "in int gl_ViewIndex"
            pInput = PatchFsGenericInputImport(pInputTy,
                                               loc,
                                               nullptr,
                                               nullptr,
                                               nullptr,
                                               InOutInfo::InterpModeFlat,
                                               InOutInfo::InterpLocCenter,
                                               pInsertPos);
            break;
        }
    case BuiltInClipDistance:
    case BuiltInCullDistance:
        {
            LLPC_ASSERT(pInputTy->isArrayTy());

            uint32_t loc = InvalidValue;
            uint32_t locCount = 0;
            uint32_t startChannel = 0;

            if (builtInId == BuiltInClipDistance)
            {
                LLPC_ASSERT(inOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            inOutUsage.builtInInputLocMap.end());
                loc = inOutUsage.builtInInputLocMap[BuiltInClipDistance];
                locCount = (builtInUsage.clipDistance > 4) ? 2 : 1;
                startChannel = 0;
            }
            else
            {
                LLPC_ASSERT(builtInId == BuiltInCullDistance);

                LLPC_ASSERT(inOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            inOutUsage.builtInInputLocMap.end());
                loc = inOutUsage.builtInInputLocMap[BuiltInCullDistance];
                locCount = (builtInUsage.clipDistance + builtInUsage.cullDistance > 4) ? 2 : 1;
                startChannel = builtInUsage.clipDistance % 4;
            }

            auto& interpInfo = inOutUsage.fs.interpInfo;
            while (interpInfo.size() <= loc + locCount -1)
            {
                interpInfo.push_back(InvalidFsInterpInfo);
            }

            interpInfo[loc] = { loc, false, false };
            if (locCount > 1)
            {
                interpInfo[loc + 1] = { loc + 1, false, false };
            }

            // Emulation for "in float gl_ClipDistance[]" or "in float gl_CullDistance[]"
            auto pPrimMask = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.primMask);
            auto pIJ = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.linearInterp.center);

            pIJ = new BitCastInst(pIJ, VectorType::get(Type::getFloatTy(*m_pContext), 2), "", pInsertPos);
            auto pI = ExtractElementInst::Create(pIJ,
                                                 ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                                 "",
                                                 pInsertPos);
            auto pJ = ExtractElementInst::Create(pIJ,
                                                 ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                 "",
                                                 pInsertPos);

            const uint32_t elemCount = pInputTy->getArrayNumElements();
            LLPC_ASSERT(elemCount <= MaxClipCullDistanceCount);

            for (uint32_t i = 0; i < elemCount; ++i)
            {
                Value* args1[] = {
                    pI,                                                                             // i
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), (startChannel + i) % 4),        // attr_chan
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), loc + (startChannel + i) / 4),  // attr
                    pPrimMask                                                                       // m0
                };
                auto pCompValue = EmitCall("llvm.amdgcn.interp.p1",
                                           Type::getFloatTy(*m_pContext),
                                           args1,
                                           attribs,
                                           pInsertPos);

                Value* args2[] = {
                    pCompValue,                                                                     // p1
                    pJ,                                                                             // j
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), (startChannel + i) % 4),        // attr_chan
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), loc + (startChannel + i) / 4),  // attr
                    pPrimMask                                                                       // m0
                };
                pCompValue = EmitCall("llvm.amdgcn.interp.p2",
                                      Type::getFloatTy(*m_pContext),
                                      args2,
                                      attribs,
                                      pInsertPos);
                pInput = InsertValueInst::Create(pInput, pCompValue, { i }, "", pInsertPos);
            }

            break;
        }
    case BuiltInSampleId:
        {
            auto pAncillary = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.ancillary);

            // gl_SampleID = Ancillary[11:8]
            Value* args[] = {
                pAncillary,
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 8),
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 4)
            };
            pInput = EmitCall("llvm.amdgcn.ubfe.i32", pInputTy, args, NoAttrib, pInsertPos);

            break;
        }
    case BuiltInSubgroupSize:
        {
            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext), m_pPipelineState->GetShaderWaveSize(m_shaderStage));
            break;
        }
    case BuiltInSubgroupLocalInvocationId:
        {
            pInput = GetSubgroupLocalInvocationId(pInsertPos);
            break;
        }
    case BuiltInDeviceIndex:
        {
            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext), m_pPipelineState->GetDeviceIndex());
            break;
        }
    // Handle internal-use built-ins for sample position emulation
    case BuiltInNumSamples:
        {
            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext), m_pPipelineState->GetRasterizerState().numSamples);
            break;
        }
    case BuiltInSamplePatternIdx:
        {
            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext),
                                      m_pPipelineState->GetRasterizerState().samplePatternIdx);
            break;
        }
    // Handle internal-use built-ins for interpolation functions and AMD extension (AMD_shader_explicit_vertex_parameter)
    case BuiltInInterpPerspSample:
    case BuiltInBaryCoordSmoothSample:
        {
            LLPC_ASSERT(entryArgIdxs.perspInterp.sample != 0);
            pInput = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.perspInterp.sample);
            break;
        }
    case BuiltInInterpPerspCenter:
    case BuiltInBaryCoordSmooth:
        {
            LLPC_ASSERT(entryArgIdxs.perspInterp.center != 0);
            pInput = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.perspInterp.center);
            break;
        }
    case BuiltInInterpPerspCentroid:
    case BuiltInBaryCoordSmoothCentroid:
        {
            LLPC_ASSERT(entryArgIdxs.perspInterp.centroid != 0);
            pInput = AdjustCentroidIJ(GetFunctionArgument(m_pEntryPoint, entryArgIdxs.perspInterp.centroid),
                                      GetFunctionArgument(m_pEntryPoint, entryArgIdxs.perspInterp.center),
                                      pInsertPos);
            break;
        }
    case BuiltInInterpPullMode:
    case BuiltInBaryCoordPullModel:
        {
            LLPC_ASSERT(entryArgIdxs.perspInterp.pullMode != 0);
            pInput = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.perspInterp.pullMode);
            break;
        }
    case BuiltInInterpLinearSample:
    case BuiltInBaryCoordNoPerspSample:
        {
            LLPC_ASSERT(entryArgIdxs.linearInterp.sample != 0);
            pInput = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.linearInterp.sample);
            break;
        }
    case BuiltInInterpLinearCenter:
    case BuiltInBaryCoordNoPersp:
        {
            LLPC_ASSERT(entryArgIdxs.linearInterp.center != 0);
            pInput = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.linearInterp.center);
            break;
        }
    case BuiltInInterpLinearCentroid:
    case BuiltInBaryCoordNoPerspCentroid:
        {
            LLPC_ASSERT(entryArgIdxs.linearInterp.centroid != 0);
            pInput = AdjustCentroidIJ(GetFunctionArgument(m_pEntryPoint, entryArgIdxs.linearInterp.centroid),
                                      GetFunctionArgument(m_pEntryPoint, entryArgIdxs.linearInterp.center),
                                      pInsertPos);
            break;
        }
    case BuiltInSamplePosOffset:
        {
            pInput = GetSamplePosOffset(pInputTy, pSampleId, pInsertPos);
            break;
        }
    case BuiltInSamplePosition:
        {
            pInput = GetSamplePosition(pInputTy, pInsertPos);
            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }

    return pInput;
}

// =====================================================================================================================
// Generate code to read BuiltInSamplePosOffset
Value* PatchInOutImportExport::GetSamplePosOffset(
    Type*         pInputTy,     // [in] Type of BuiltInSamplePosOffset
    Value*        pSampleId,    // [in] Sample ID
    Instruction*  pInsertPos)   // [in] Insert position
{
    // Gets the offset of sample position relative to the pixel center for the specified sample ID
    IRBuilder<> builder(*m_pContext);
    builder.SetInsertPoint(pInsertPos);
    Value* pNumSamples = PatchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInNumSamples, nullptr, pInsertPos);
    Value* pPatternIdx = PatchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInSamplePatternIdx, nullptr, pInsertPos);
    Value* pValidOffset = builder.CreateAdd(pPatternIdx, pSampleId);
    // offset = (sampleCount > sampleId) ? (samplePatternOffset + sampleId) : 0
    Value* pSampleValid = builder.CreateICmpUGT(pNumSamples, pSampleId);
    Value* pOffset = builder.CreateSelect(pSampleValid, pValidOffset, builder.getInt32(0));
    // Load sample position descriptor.
    auto pDesc = EmitCall(LlpcName::DescriptorLoadBuffer,
                          VectorType::get(builder.getInt32Ty(), 4),
                          {
                              builder.getInt32(InternalResourceTable),
                              builder.getInt32(SI_DRV_TABLE_SAMPLEPOS),
                              builder.getInt32(0),
                          },
                          NoAttrib,
                          pInsertPos);
    pOffset = builder.CreateShl(pOffset, builder.getInt32(4));
    return builder.CreateIntrinsic(Intrinsic::amdgcn_raw_buffer_load,
                                   pInputTy,
                                   {
                                      pDesc,
                                      pOffset,
                                      builder.getInt32(0),
                                      builder.getInt32(0)
                                   });
}

// =====================================================================================================================
// Generate code to read BuiltInSamplePosition
Value* PatchInOutImportExport::GetSamplePosition(
    Type*         pInputTy,   // [in] Type of BuiltInSamplePosition
    Instruction*  pInsertPos) // [in] Insert position
{
    IRBuilder<> builder(*m_pContext);
    builder.SetInsertPoint(pInsertPos);
    Value* pSampleId = PatchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInSampleId, nullptr, pInsertPos);
    Value* pInput = PatchFsBuiltInInputImport(pInputTy, BuiltInSamplePosOffset, pSampleId, pInsertPos);
    return builder.CreateFAdd(pInput, ConstantFP::get(pInputTy, 0.5));
}

// =====================================================================================================================
// Patches import calls for built-in inputs of compute shader.
Value* PatchInOutImportExport::PatchCsBuiltInInputImport(
    Type*        pInputTy,      // [in] Type of input value
    uint32_t     builtInId,     // ID of the built-in variable
    Instruction* pInsertPos)    // [in] Where to insert the patch instruction
{
    Value* pInput = nullptr;

    auto  pIntfData    = m_pPipelineState->GetShaderInterfaceData(ShaderStageCompute);
    auto& entryArgIdxs = pIntfData->entryArgIdxs.cs;

    switch (builtInId)
    {
    case BuiltInWorkgroupSize:
        {
            pInput = GetWorkgroupSize();
            break;
        }
    case BuiltInNumWorkgroups:
        {
            pInput = m_pipelineSysValues.Get(m_pEntryPoint)->GetNumWorkgroups();
            break;
        }
    case BuiltInWorkgroupId:
        {
            pInput = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.workgroupId);
            break;
        }
    case BuiltInLocalInvocationId:
        {
            pInput = GetInLocalInvocationId(pInsertPos);
            break;
        }
    case BuiltInSubgroupSize:
        {
            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext), m_pPipelineState->GetShaderWaveSize(m_shaderStage));
            break;
        }
    case BuiltInSubgroupLocalInvocationId:
        {
            pInput = GetSubgroupLocalInvocationId(pInsertPos);
            break;
        }
    case BuiltInDeviceIndex:
        {
            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext), m_pPipelineState->GetDeviceIndex());
            break;
        }
    case BuiltInNumSubgroups:
        {
            // workgroupSize = gl_WorkGroupSize.x * gl_WorkGroupSize.y * gl_WorkGroupSize.z
            auto& mode = m_pPipelineState->GetShaderModes()->GetComputeShaderMode();
            const uint32_t workgroupSize = mode.workgroupSizeX *
                                           mode.workgroupSizeY *
                                           mode.workgroupSizeZ;

            // gl_NumSubgroups = (workgroupSize + gl_SubGroupSize - 1) / gl_SubgroupSize
            const uint32_t subgroupSize = m_pPipelineState->GetShaderWaveSize(m_shaderStage);
            const uint32_t numSubgroups = (workgroupSize + subgroupSize - 1) / subgroupSize;

            pInput = ConstantInt::get(Type::getInt32Ty(*m_pContext), numSubgroups);
            break;
        }
    case BuiltInGlobalInvocationId:
        {
            pInput = GetGlobalInvocationId(pInputTy, pInsertPos);
            break;
        }
    case BuiltInLocalInvocationIndex:
        {
            pInput = GetLocalInvocationIndex(pInputTy, pInsertPos);
            break;
        }
    case BuiltInSubgroupId:
        {
            pInput = GetSubgroupId(pInputTy, pInsertPos);
            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }

    return pInput;
}

// =====================================================================================================================
// Get GlobalInvocationId
Value* PatchInOutImportExport::GetGlobalInvocationId(
    Type*         pInputTy,   // [in] Type of GlobalInvocationId
    Instruction*  pInsertPos) // [in] Insert position
{
    IRBuilder<> builder(*m_pContext);
    builder.SetInsertPoint(pInsertPos);
    Value* pWorkgroupSize = PatchCsBuiltInInputImport(pInputTy, BuiltInWorkgroupSize, pInsertPos);
    Value* pWorkgroupId = PatchCsBuiltInInputImport(pInputTy, BuiltInWorkgroupId, pInsertPos);
    Value* pLocalInvocationId = PatchCsBuiltInInputImport(pInputTy, BuiltInLocalInvocationId, pInsertPos);
    Value* pInput = builder.CreateMul(pWorkgroupSize, pWorkgroupId);
    pInput = builder.CreateAdd(pInput, pLocalInvocationId);
    return pInput;
}

// =====================================================================================================================
// Get LocalInvocationIndex
Value* PatchInOutImportExport::GetLocalInvocationIndex(
    Type*         pInputTy,   // [in] Type of LocalInvocationIndex
    Instruction*  pInsertPos) // [in] Insert position
{
    IRBuilder<> builder(*m_pContext);
    builder.SetInsertPoint(pInsertPos);
    Value* pWorkgroupSize = PatchCsBuiltInInputImport(pInputTy, BuiltInWorkgroupSize, pInsertPos);
    Value* pLocalInvocationId = PatchCsBuiltInInputImport(pInputTy, BuiltInLocalInvocationId, pInsertPos);
    Value* pInput = builder.CreateMul(builder.CreateExtractElement(pWorkgroupSize, 1),
                                      builder.CreateExtractElement(pLocalInvocationId, 2));
    pInput = builder.CreateAdd(pInput, builder.CreateExtractElement(pLocalInvocationId, 1));
    pInput = builder.CreateMul(builder.CreateExtractElement(pWorkgroupSize, uint64_t(0)), pInput);
    pInput = builder.CreateAdd(pInput, builder.CreateExtractElement(pLocalInvocationId, uint64_t(0)));
    return pInput;
}

// =====================================================================================================================
// Get SubgroupId
Value* PatchInOutImportExport::GetSubgroupId(
    Type*         pInputTy,   // [in] Type of LocalInvocationIndex
    Instruction*  pInsertPos) // [in] Insert position
{
    // gl_SubgroupID = gl_LocationInvocationIndex / gl_SubgroupSize
    IRBuilder<> builder(*m_pContext);
    builder.SetInsertPoint(pInsertPos);
    Value* pLocalInvocationIndex = PatchCsBuiltInInputImport(pInputTy, BuiltInLocalInvocationIndex, pInsertPos);
    uint32_t subgroupSize = m_pPipelineState->GetShaderWaveSize(m_shaderStage);
    return builder.CreateLShr(pLocalInvocationIndex, builder.getInt32(Log2_32(subgroupSize)));
}

// =====================================================================================================================
// Patches import calls for built-in outputs of tessellation control shader.
Value* PatchInOutImportExport::PatchTcsBuiltInOutputImport(
    Type*        pOutputTy,     // [in] Type of output value
    uint32_t     builtInId,     // ID of the built-in variable
    Value*       pElemIdx,      // [in] Index used for array/vector element indexing (could be null)
    Value*       pVertexIdx,    // [in] Input array outermost index used for vertex indexing (could be null)
    Instruction* pInsertPos)    // [in] Where to insert the patch instruction
{
    Value* pOutput = UndefValue::get(pOutputTy);

    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl);
    auto& builtInUsage = pResUsage->builtInUsage.tcs;
    auto& builtInOutLocMap = pResUsage->inOutUsage.builtInOutputLocMap;
    auto& perPatchBuiltInOutLocMap = pResUsage->inOutUsage.perPatchBuiltInOutputLocMap;

    switch (builtInId)
    {
    case BuiltInPosition:
        {
            LLPC_ASSERT(builtInUsage.position);
            LLPC_UNUSED(builtInUsage);

            LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
            uint32_t loc = builtInOutLocMap[builtInId];

            auto pLdsOffset = CalcLdsOffsetForTcsOutput(pOutputTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
            pOutput = ReadValueFromLds(true, pOutputTy, pLdsOffset, pInsertPos);

            break;
        }
    case BuiltInPointSize:
        {
            LLPC_ASSERT(builtInUsage.pointSize);
            LLPC_UNUSED(builtInUsage);

            LLPC_ASSERT(pElemIdx == nullptr);
            LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
            uint32_t loc = builtInOutLocMap[builtInId];

            auto pLdsOffset = CalcLdsOffsetForTcsOutput(pOutputTy, loc, nullptr, nullptr, pVertexIdx, pInsertPos);
            pOutput = ReadValueFromLds(true, pOutputTy, pLdsOffset, pInsertPos);

            break;
        }
    case BuiltInClipDistance:
    case BuiltInCullDistance:
        {
            if (builtInId == BuiltInClipDistance)
            {
                LLPC_ASSERT(builtInUsage.clipDistance > 0);
                LLPC_UNUSED(builtInUsage);
            }
            else
            {
                LLPC_ASSERT(builtInId == BuiltInCullDistance);
                LLPC_ASSERT(builtInUsage.cullDistance > 0);
                LLPC_UNUSED(builtInUsage);
            }

            LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
            uint32_t loc = builtInOutLocMap[builtInId];

            if (pElemIdx == nullptr)
            {
                // gl_ClipDistance[]/gl_CullDistance[] is treated as 2 x vec4
                LLPC_ASSERT(pOutputTy->isArrayTy());

                auto pElemTy = pOutputTy->getArrayElementType();
                for (uint32_t i = 0; i < pOutputTy->getArrayNumElements(); ++i)
                {
                    auto pElemIdx = ConstantInt::get(Type::getInt32Ty(*m_pContext), i);
                    auto pLdsOffset =
                        CalcLdsOffsetForTcsOutput(pElemTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                    auto pElem = ReadValueFromLds(true, pElemTy, pLdsOffset, pInsertPos);
                    pOutput = InsertValueInst::Create(pOutput, pElem, { i }, "", pInsertPos);
                }
            }
            else
            {
                auto pLdsOffset = CalcLdsOffsetForTcsOutput(pOutputTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                pOutput = ReadValueFromLds(true, pOutputTy, pLdsOffset, pInsertPos);
            }

            break;
        }
    case BuiltInTessLevelOuter:
    case BuiltInTessLevelInner:
        {
            if (builtInId == BuiltInTessLevelOuter)
            {
                LLPC_ASSERT(builtInUsage.tessLevelOuter);
                LLPC_UNUSED(builtInUsage);
            }
            else
            {
                LLPC_ASSERT(builtInId == BuiltInTessLevelInner);
                LLPC_ASSERT(builtInUsage.tessLevelInner);
                LLPC_UNUSED(builtInUsage);
            }

            LLPC_ASSERT(perPatchBuiltInOutLocMap.find(builtInId) != perPatchBuiltInOutLocMap.end());
            uint32_t loc = perPatchBuiltInOutLocMap[builtInId];

            if (pElemIdx == nullptr)
            {
                // gl_TessLevelOuter[4] is treated as vec4
                // gl_TessLevelInner[2] is treated as vec2
                LLPC_ASSERT(pOutputTy->isArrayTy());

                auto pElemTy = pOutputTy->getArrayElementType();
                for (uint32_t i = 0; i < pOutputTy->getArrayNumElements(); ++i)
                {
                    auto pElemIdx = ConstantInt::get(Type::getInt32Ty(*m_pContext), i);
                    auto pLdsOffset =
                        CalcLdsOffsetForTcsOutput(pElemTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                    auto pElem = ReadValueFromLds(true, pElemTy, pLdsOffset, pInsertPos);
                    pOutput = InsertValueInst::Create(pOutput, pElem, { i }, "", pInsertPos);
                }
            }
            else
            {
                auto pLdsOffset = CalcLdsOffsetForTcsOutput(pOutputTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                pOutput = ReadValueFromLds(true, pOutputTy, pLdsOffset, pInsertPos);
            }

            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }

    return pOutput;
}

// =====================================================================================================================
// Patches export calls for built-in outputs of vertex shader.
void PatchInOutImportExport::PatchVsBuiltInOutputExport(
    Value*       pOutput,       // [in] Output value
    uint32_t     builtInId,     // ID of the built-in variable
    Instruction* pInsertPos)    // [in] Where to insert the patch instruction
{
    auto pOutputTy = pOutput->getType();

    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageVertex);
    auto& builtInUsage = pResUsage->builtInUsage.vs;
    auto& builtInOutLocMap = pResUsage->inOutUsage.builtInOutputLocMap;

    switch (builtInId)
    {
    case BuiltInPosition:
        {
            if (builtInUsage.position == false)
            {
                return;
            }

            if (m_hasTs)
            {
                uint32_t loc = builtInOutLocMap[builtInId];
                auto pLdsOffset = CalcLdsOffsetForVsOutput(pOutputTy, loc, 0, pInsertPos);
                WriteValueToLds(pOutput, pLdsOffset, pInsertPos);
            }
            else
            {
                if (m_hasGs)
                {
                    LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
                    uint32_t loc = builtInOutLocMap[builtInId];

                    StoreValueToEsGsRing(pOutput, loc, 0, pInsertPos);
                }
                else
                {
                    AddExportInstForBuiltInOutput(pOutput, builtInId, pInsertPos);
                }
            }

            break;
        }
    case BuiltInPointSize:
        {
            if (builtInUsage.pointSize == false)
            {
                return;
            }

            if (isa<UndefValue>(pOutput))
            {
                // NOTE: gl_PointSize is always declared as a field of gl_PerVertex. We have to check the output
                // value to determine if it is actually referenced in shader.
                builtInUsage.pointSize = false;
                return;
            }

            if (m_hasTs)
            {
                uint32_t loc = builtInOutLocMap[builtInId];
                auto pLdsOffset = CalcLdsOffsetForVsOutput(pOutputTy, loc, 0, pInsertPos);
                WriteValueToLds(pOutput, pLdsOffset, pInsertPos);
            }
            else
            {
                if (m_hasGs)
                {
                    LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
                    uint32_t loc = builtInOutLocMap[builtInId];

                    StoreValueToEsGsRing(pOutput, loc, 0, pInsertPos);
                }
                else
                {
                    AddExportInstForBuiltInOutput(pOutput, builtInId, pInsertPos);
                }
            }

            break;
        }
    case BuiltInClipDistance:
        {
            if (builtInUsage.clipDistance == 0)
            {
                return;
            }

            if (isa<UndefValue>(pOutput))
            {
                // NOTE: gl_ClipDistance[] is always declared as a field of gl_PerVertex. We have to check the output
                // value to determine if it is actually referenced in shader.
                builtInUsage.clipDistance = 0;
                return;
            }

            if (m_hasTs)
            {
                LLPC_ASSERT(pOutputTy->isArrayTy());

                uint32_t loc = builtInOutLocMap[builtInId];
                auto pLdsOffset = CalcLdsOffsetForVsOutput(pOutputTy->getArrayElementType(), loc, 0, pInsertPos);

                for (uint32_t i = 0; i < pOutputTy->getArrayNumElements(); ++i)
                {
                    auto pElem = ExtractValueInst::Create(pOutput, { i }, "", pInsertPos);
                    WriteValueToLds(pElem, pLdsOffset, pInsertPos);

                    pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                                           ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                           "",
                                                           pInsertPos);
                }
            }
            else
            {
                if (m_hasGs)
                {
                    LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
                    uint32_t loc = builtInOutLocMap[builtInId];

                    StoreValueToEsGsRing(pOutput, loc, 0, pInsertPos);
                }
                else
                {
                    // NOTE: The export of gl_ClipDistance[] is delayed and is done before entry-point returns.
                    m_pClipDistance = pOutput;
                }
            }

            break;
        }
    case BuiltInCullDistance:
        {
            if (builtInUsage.cullDistance == 0)
            {
                return;
            }

            if (isa<UndefValue>(pOutput))
            {
                // NOTE: gl_CullDistance[] is always declared as a field of gl_PerVertex. We have to check the output
                // value to determine if it is actually referenced in shader.
                builtInUsage.cullDistance = 0;
                return;
            }

            if (m_hasTs)
            {
                LLPC_ASSERT(pOutputTy->isArrayTy());

                uint32_t loc = builtInOutLocMap[builtInId];
                auto pLdsOffset = CalcLdsOffsetForVsOutput(pOutputTy->getArrayElementType(), loc, 0, pInsertPos);

                for (uint32_t i = 0; i < pOutputTy->getArrayNumElements(); ++i)
                {
                    auto pElem = ExtractValueInst::Create(pOutput, { i }, "", pInsertPos);
                    WriteValueToLds(pElem, pLdsOffset, pInsertPos);

                    pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                                           ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                           "",
                                                           pInsertPos);
                }
            }
            else
            {
                if (m_hasGs)
                {
                    LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
                    uint32_t loc = builtInOutLocMap[builtInId];

                    StoreValueToEsGsRing(pOutput, loc, 0, pInsertPos);
                }
                else
                {
                    // NOTE: The export of gl_CullDistance[] is delayed and is done before entry-point returns.
                    m_pCullDistance = pOutput;
                }
            }

            break;
        }
    case BuiltInLayer:
        {
            if (builtInUsage.layer == false)
            {
                return;
            }

            const auto enableMultiView = m_pPipelineState->GetInputAssemblyState().enableMultiView;

            // NOTE: Only last non-fragment shader stage has to export the value of gl_Layer.
            if ((m_hasTs == false) && (m_hasGs == false) && (enableMultiView == false))
            {
                if (m_gfxIp.major <= 8)
                {
                    AddExportInstForBuiltInOutput(pOutput, builtInId, pInsertPos);
                }
                else
                {
                    // NOTE: The export of gl_Layer is delayed and is done before entry-point returns.
                    m_pLayer = pOutput;
                }
            }

            break;
        }
    case BuiltInViewportIndex:
        {
            if (builtInUsage.viewportIndex == false)
            {
                return;
            }

            // NOTE: Only last non-fragment shader stage has to export the value of gl_ViewportIndex.
            if ((m_hasTs == false) && (m_hasGs == false))
            {
                if (m_gfxIp.major <= 8)
                {
                    AddExportInstForBuiltInOutput(pOutput, builtInId, pInsertPos);
                }
                else
                {
                    // NOTE: The export of gl_ViewportIndex is delayed and is done before entry-point returns.
                    m_pViewportIndex = pOutput;
                }
            }

            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }
}

// =====================================================================================================================
// Patches export calls for built-in outputs of tessellation control shader.
void PatchInOutImportExport::PatchTcsBuiltInOutputExport(
    Value*       pOutput,       // [in] Output value
    uint32_t     builtInId,     // ID of the built-in variable
    Value*       pElemIdx,      // [in] Index used for array/vector element indexing (could be null)
    Value*       pVertexIdx,    // [in] Input array outermost index used for vertex indexing (could be null)
    Instruction* pInsertPos)    // [in] Where to insert the patch instruction
{
    auto pOutputTy = pOutput->getType();

    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl);
    auto& builtInUsage = pResUsage->builtInUsage.tcs;
    auto& builtInOutLocMap = pResUsage->inOutUsage.builtInOutputLocMap;
    auto& perPatchBuiltInOutLocMap = pResUsage->inOutUsage.perPatchBuiltInOutputLocMap;

    switch (builtInId)
    {
    case BuiltInPosition:
        {
            if (builtInUsage.position == false)
            {
                return;
            }

            LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
            uint32_t loc = builtInOutLocMap[builtInId];

            auto pLdsOffset = CalcLdsOffsetForTcsOutput(pOutputTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
            WriteValueToLds(pOutput, pLdsOffset, pInsertPos);

            break;
        }
    case BuiltInPointSize:
        {
            if (builtInUsage.pointSize == false)
            {
                return;
            }

            LLPC_ASSERT(pElemIdx == nullptr);
            LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
            uint32_t loc = builtInOutLocMap[builtInId];

            auto pLdsOffset = CalcLdsOffsetForTcsOutput(pOutputTy, loc, nullptr, nullptr, pVertexIdx, pInsertPos);
            WriteValueToLds(pOutput, pLdsOffset, pInsertPos);

            break;
        }
    case BuiltInClipDistance:
    case BuiltInCullDistance:
        {
            if (((builtInId == BuiltInClipDistance) && (builtInUsage.clipDistance == 0)) ||
                ((builtInId == BuiltInCullDistance) && (builtInUsage.cullDistance == 0)))
            {
                return;
            }

            LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
            uint32_t loc = builtInOutLocMap[builtInId];

            if (pElemIdx == nullptr)
            {
                // gl_ClipDistance[]/gl_CullDistance[] is treated as 2 x vec4
                LLPC_ASSERT(pOutputTy->isArrayTy());

                for (uint32_t i = 0; i < pOutputTy->getArrayNumElements(); ++i)
                {
                    auto pElem = ExtractValueInst::Create(pOutput, { i }, "", pInsertPos);
                    auto pElemIdx = ConstantInt::get(Type::getInt32Ty(*m_pContext), i);
                    auto pLdsOffset =
                        CalcLdsOffsetForTcsOutput(pElem->getType(), loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                    WriteValueToLds(pElem, pLdsOffset, pInsertPos);
                }
            }
            else
            {
                auto pLdsOffset = CalcLdsOffsetForTcsOutput(pOutputTy, loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                WriteValueToLds(pOutput, pLdsOffset, pInsertPos);
            }

            break;
        }
    case BuiltInTessLevelOuter:
        {
            if (builtInUsage.tessLevelOuter)
            {
                // Extract tessellation factors
                std::vector<Value*> tessFactors;
                if (pElemIdx == nullptr)
                {
                    LLPC_ASSERT(pOutputTy->isArrayTy());

                    uint32_t tessFactorCount = 0;

                    auto primitiveMode = m_pPipelineState->GetShaderModes()->GetTessellationMode().primitiveMode;
                    switch (primitiveMode)
                    {
                    case PrimitiveMode::Isolines:
                        tessFactorCount = 2;
                        break;
                    case PrimitiveMode::Triangles:
                        tessFactorCount = 3;
                        break;
                    case PrimitiveMode::Quads:
                        tessFactorCount = 4;
                        break;
                    default:
                        LLPC_NEVER_CALLED();
                        break;
                    }

                    for (uint32_t i = 0; i < tessFactorCount; ++i)
                    {
                        Value* pElem = ExtractValueInst::Create(pOutput, { i }, "", pInsertPos);
                        tessFactors.push_back(pElem);
                    }

                    if (primitiveMode == PrimitiveMode::Isolines)
                    {
                        LLPC_ASSERT(tessFactorCount == 2);
                        std::swap(tessFactors[0], tessFactors[1]);
                    }
                }
                else
                {
                    LLPC_ASSERT(pOutputTy->isFloatTy());
                    tessFactors.push_back(pOutput);
                }

                Value* pTessFactorOffset = CalcTessFactorOffset(true, pElemIdx, pInsertPos);
                StoreTessFactorToBuffer(tessFactors, pTessFactorOffset, pInsertPos);

                LLPC_ASSERT(perPatchBuiltInOutLocMap.find(builtInId) != perPatchBuiltInOutLocMap.end());
                uint32_t loc = perPatchBuiltInOutLocMap[builtInId];

                if (pElemIdx == nullptr)
                {
                    // gl_TessLevelOuter[4] is treated as vec4
                    LLPC_ASSERT(pOutputTy->isArrayTy());

                    for (uint32_t i = 0; i < pOutputTy->getArrayNumElements(); ++i)
                    {
                        auto pElem = ExtractValueInst::Create(pOutput, { i }, "",  pInsertPos);
                        auto pElemIdx = ConstantInt::get(Type::getInt32Ty(*m_pContext), i);
                        auto pLdsOffset =
                            CalcLdsOffsetForTcsOutput(pElem->getType(), loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                        WriteValueToLds(pElem, pLdsOffset,  pInsertPos);
                    }
                }
                else
                {
                    auto pLdsOffset = CalcLdsOffsetForTcsOutput(pOutputTy, loc, nullptr, pElemIdx, nullptr, pInsertPos);
                    WriteValueToLds(pOutput, pLdsOffset,  pInsertPos);
                }
            }
            break;
        }
    case BuiltInTessLevelInner:
        {
            if (builtInUsage.tessLevelInner)
            {
                // Extract tessellation factors
                std::vector<Value*> tessFactors;
                if (pElemIdx == nullptr)
                {
                    uint32_t tessFactorCount = 0;

                    switch (m_pPipelineState->GetShaderModes()->GetTessellationMode().primitiveMode)
                    {
                    case PrimitiveMode::Isolines:
                        tessFactorCount = 0;
                        break;
                    case PrimitiveMode::Triangles:
                        tessFactorCount = 1;
                        break;
                    case PrimitiveMode::Quads:
                        tessFactorCount = 2;
                        break;
                    default:
                        LLPC_NEVER_CALLED();
                        break;
                    }

                    for (uint32_t i = 0; i < tessFactorCount; ++i)
                    {
                        Value* pElem = ExtractValueInst::Create(pOutput, { i }, "", pInsertPos);
                        tessFactors.push_back(pElem);
                    }
                }
                else
                {
                    LLPC_ASSERT(pOutputTy->isFloatTy());
                    tessFactors.push_back(pOutput);
                }

                Value* pTessFactorOffset = CalcTessFactorOffset(false, pElemIdx, pInsertPos);
                StoreTessFactorToBuffer(tessFactors, pTessFactorOffset, pInsertPos);

                LLPC_ASSERT(perPatchBuiltInOutLocMap.find(builtInId) != perPatchBuiltInOutLocMap.end());
                uint32_t loc = perPatchBuiltInOutLocMap[builtInId];

                if (pElemIdx == nullptr)
                {
                    // gl_TessLevelInner[2] is treated as vec2
                    LLPC_ASSERT(pOutputTy->isArrayTy());

                    for (uint32_t i = 0; i < pOutputTy->getArrayNumElements(); ++i)
                    {
                        auto pElem = ExtractValueInst::Create(pOutput, { i }, "", pInsertPos);
                        auto pElemIdx = ConstantInt::get(Type::getInt32Ty(*m_pContext), i);
                        auto pLdsOffset =
                            CalcLdsOffsetForTcsOutput(pElem->getType(), loc, nullptr, pElemIdx, pVertexIdx, pInsertPos);
                        WriteValueToLds(pElem, pLdsOffset, pInsertPos);
                    }
                }
                else
                {
                    auto pLdsOffset = CalcLdsOffsetForTcsOutput(pOutputTy, loc, nullptr, pElemIdx, nullptr, pInsertPos);
                    WriteValueToLds(pOutput, pLdsOffset, pInsertPos);
                }
            }

            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }
}

// =====================================================================================================================
// Patches export calls for built-in outputs of tessellation evaluation shader.
void PatchInOutImportExport::PatchTesBuiltInOutputExport(
    Value*       pOutput,       // [in] Output value
    uint32_t     builtInId,     // ID of the built-in variable
    Instruction* pInsertPos)    // [in] Where to insert the patch instruction
{
    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessEval);
    auto& builtInUsage = pResUsage->builtInUsage.tes;
    auto& builtInOutLocMap = pResUsage->inOutUsage.builtInOutputLocMap;

    switch (builtInId)
    {
    case BuiltInPosition:
        {
            if (builtInUsage.position == false)
            {
                return;
            }

            if (m_hasGs)
            {
                LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
                uint32_t loc = builtInOutLocMap[builtInId];

                StoreValueToEsGsRing(pOutput, loc, 0, pInsertPos);
            }
            else
            {
                AddExportInstForBuiltInOutput(pOutput, builtInId, pInsertPos);
            }

            break;
        }
    case BuiltInPointSize:
        {
            if (builtInUsage.pointSize == false)
            {
                return;
            }

            if (isa<UndefValue>(pOutput))
            {
                // NOTE: gl_PointSize is always declared as a field of gl_PerVertex. We have to check the output
                // value to determine if it is actually referenced in shader.
                builtInUsage.pointSize = false;
                return;
            }

            if (m_hasGs)
            {
                LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
                uint32_t loc = builtInOutLocMap[builtInId];

                StoreValueToEsGsRing(pOutput, loc, 0, pInsertPos);
            }
            else
            {
                AddExportInstForBuiltInOutput(pOutput, builtInId, pInsertPos);
            }

            break;
        }
    case BuiltInClipDistance:
        {
            if (builtInUsage.clipDistance == 0)
            {
                return;
            }

            if (isa<UndefValue>(pOutput))
            {
                // NOTE: gl_ClipDistance[] is always declared as a field of gl_PerVertex. We have to check the output
                // value to determine if it is actually referenced in shader.
                builtInUsage.clipDistance = 0;
                return;
            }

            if (m_hasGs)
            {
                LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
                uint32_t loc = builtInOutLocMap[builtInId];

                StoreValueToEsGsRing(pOutput, loc, 0, pInsertPos);
            }
            else
            {
                // NOTE: The export of gl_ClipDistance[] is delayed and is done before entry-point returns.
                m_pClipDistance = pOutput;
            }

            break;
        }
    case BuiltInCullDistance:
        {
            if (builtInUsage.cullDistance == 0)
            {
                return;
            }

            if (isa<UndefValue>(pOutput))
            {
                // NOTE: gl_CullDistance[] is always declared as a field of gl_PerVertex. We have to check the output
                // value to determine if it is actually referenced in shader.
                builtInUsage.cullDistance = 0;
                return;
            }

            if (m_hasGs)
            {
                LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
                uint32_t loc = builtInOutLocMap[builtInId];

                StoreValueToEsGsRing(pOutput, loc, 0, pInsertPos);
            }
            else
            {
                // NOTE: The export of gl_CullDistance[] is delayed and is done before entry-point returns.
                m_pCullDistance = pOutput;
            }

            break;
        }
    case BuiltInLayer:
        {
            if (builtInUsage.layer == false)
            {
                return;
            }

            const auto enableMultiView = m_pPipelineState->GetInputAssemblyState().enableMultiView;

            // NOTE: Only last non-fragment shader stage has to export the value of gl_Layer.
            if ((m_hasGs == false) && (enableMultiView == false))
            {
                if (m_gfxIp.major <= 8)
                {
                    AddExportInstForBuiltInOutput(pOutput, builtInId, pInsertPos);
                }
                else
                {
                    // NOTE: The export of gl_Layer is delayed and is done before entry-point returns.
                    m_pLayer = pOutput;
                }
            }

            break;
        }
    case BuiltInViewportIndex:
        {
            if (builtInUsage.viewportIndex == false)
            {
                return;
            }

            // NOTE: Only last non-fragment shader stage has to export the value of gl_ViewportIndex.
            if (m_hasGs == false)
            {
                if (m_gfxIp.major <= 8)
                {
                    AddExportInstForBuiltInOutput(pOutput, builtInId, pInsertPos);
                }
                else
                {
                    // NOTE: The export of gl_ViewportIndex is delayed and is done before entry-point returns.
                    m_pViewportIndex = pOutput;
                }
            }

            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }
}

// =====================================================================================================================
// Patches export calls for built-in outputs of geometry shader.
void PatchInOutImportExport::PatchGsBuiltInOutputExport(
    Value*       pOutput,       // [in] Output value
    uint32_t     builtInId,     // ID of the built-in variable
    uint32_t     streamId,      // ID of output vertex stream
    Instruction* pInsertPos)    // [in] Where to insert the patch instruction
{
    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);
    auto& builtInUsage = pResUsage->builtInUsage.gs;
    auto& builtInOutLocMap = pResUsage->inOutUsage.builtInOutputLocMap;

    LLPC_ASSERT(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    uint32_t loc = builtInOutLocMap[builtInId];

    switch (builtInId)
    {
    case BuiltInPosition:
        LLPC_ASSERT(builtInUsage.position);
        break;
    case BuiltInPointSize:
        LLPC_ASSERT(builtInUsage.pointSize);
        break;
    case BuiltInClipDistance:
        LLPC_ASSERT(builtInUsage.clipDistance);
        break;
    case BuiltInCullDistance:
        LLPC_ASSERT(builtInUsage.cullDistance);
        break;
    case BuiltInPrimitiveId:
        LLPC_ASSERT(builtInUsage.primitiveId);
        break;
    case BuiltInLayer:
        LLPC_ASSERT(builtInUsage.layer);
        break;
    case BuiltInViewportIndex:
        LLPC_ASSERT(builtInUsage.viewportIndex);
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    LLPC_UNUSED(builtInUsage);
    StoreValueToGsVsRing(pOutput, loc, 0, streamId, pInsertPos);
}

// =====================================================================================================================
// Patches export calls for built-in outputs of fragment shader.
void PatchInOutImportExport::PatchFsBuiltInOutputExport(
    Value*       pOutput,       // [in] Output value
    uint32_t     builtInId,     // ID of the built-in variable
    Instruction* pInsertPos)    // [in] Where to insert the patch instruction
{
    switch (builtInId)
    {
    case BuiltInFragDepth:
        {
            m_pFragDepth = pOutput;
            break;
        }
    case BuiltInSampleMask:
        {
            LLPC_ASSERT(pOutput->getType()->isArrayTy());

            // NOTE: Only gl_SampleMask[0] is valid for us.
            m_pSampleMask = ExtractValueInst::Create(pOutput, { 0 }, "", pInsertPos);
            m_pSampleMask = new BitCastInst(m_pSampleMask, Type::getFloatTy(*m_pContext), "", pInsertPos);
            break;
        }
    case BuiltInFragStencilRef:
        {
            m_pFragStencilRef = new BitCastInst(pOutput, Type::getFloatTy(*m_pContext), "", pInsertPos);
            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }
}

// =====================================================================================================================
// Patches export calls for generic outputs of copy shader.
void PatchInOutImportExport::PatchCopyShaderGenericOutputExport(
    Value*       pOutput,        // [in] Output value
    uint32_t     location,       // Location of the output
    Instruction* pInsertPos)     // [in] Where to insert the patch instruction
{
    AddExportInstForGenericOutput(pOutput, location, 0, pInsertPos);
}

// =====================================================================================================================
// Patches export calls for built-in outputs of copy shader.
void PatchInOutImportExport::PatchCopyShaderBuiltInOutputExport(
    Value*       pOutput,       // [in] Output value
    uint32_t     builtInId,     // ID of the built-in variable
    Instruction* pInsertPos)    // [in] Where to insert the patch instruction
{
    switch (builtInId)
    {
    case BuiltInPosition:
    case BuiltInPointSize:
        {
            AddExportInstForBuiltInOutput(pOutput, builtInId, pInsertPos);
            break;
        }
    case BuiltInClipDistance:
        {
            // NOTE: The export of gl_ClipDistance[] is delayed and is done before entry-point returns.
            m_pClipDistance = pOutput;
            break;
        }
    case BuiltInCullDistance:
        {
            // NOTE: The export of gl_CullDistance[] is delayed and is done before entry-point returns.
            m_pCullDistance = pOutput;
            break;
        }
     case BuiltInPrimitiveId:
        {
            // NOTE: The export of gl_PrimitiveID is delayed and is done before entry-point returns.
            m_pPrimitiveId = pOutput;
            break;
        }
    case BuiltInLayer:
        {
            const auto enableMultiView = m_pPipelineState->GetInputAssemblyState().enableMultiView;

            if ((m_gfxIp.major <= 8) && (enableMultiView == false))
            {
                AddExportInstForBuiltInOutput(pOutput, builtInId, pInsertPos);
            }
            else
            {
                // NOTE: The export of gl_Layer is delayed and is done before entry-point returns.
                m_pLayer = pOutput;
            }

            break;
        }
    case BuiltInViewportIndex:
        {
            if (m_gfxIp.major <= 8)
            {
                AddExportInstForBuiltInOutput(pOutput, builtInId, pInsertPos);
            }
            else
            {
                // NOTE: The export of gl_ViewportIndex is delayed and is done before entry-point returns.
                m_pViewportIndex = pOutput;
            }

            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }
}

// =====================================================================================================================
// Patch export calls for transform feedback outputs of vertex shader and tessellation evaluation shader.
void PatchInOutImportExport::PatchXfbOutputExport(
    Value*        pOutput,            // [in] Output value
    uint32_t      xfbBuffer,          // Transform feedback buffer ID
    uint32_t      xfbOffset,          // Transform feedback offset
    uint32_t      xfbExtraOffset,     // Transform feedback extra offset, passed from aggregate type
    Instruction*  pInsertPos)         // [in] Where to insert the store instruction
{
    LLPC_ASSERT((m_shaderStage == ShaderStageVertex) ||
                (m_shaderStage == ShaderStageTessEval) ||
                (m_shaderStage == ShaderStageCopyShader));

    Value* pStreamOutBufDesc = m_pipelineSysValues.Get(m_pEntryPoint)->GetStreamOutBufDesc(xfbBuffer);

    const auto& xfbStrides = m_pPipelineState->GetShaderResourceUsage(m_shaderStage)->inOutUsage.xfbStrides;
    uint32_t xfbStride = xfbStrides[xfbBuffer];

    auto pOutputTy = pOutput->getType();
    uint32_t compCount = pOutputTy->isVectorTy() ? pOutputTy->getVectorNumElements() : 1;
    uint32_t bitWidth = pOutputTy->getScalarSizeInBits();

    xfbOffset = xfbOffset + xfbExtraOffset;

    if (bitWidth == 64)
    {
        // Cast 64-bit output to 32-bit
        compCount *= 2;
        bitWidth = 32;
        pOutputTy = VectorType::get(Type::getFloatTy(*m_pContext), compCount);
        pOutput = new BitCastInst(pOutput, pOutputTy, "", pInsertPos);
    }
    LLPC_ASSERT((bitWidth == 16) || (bitWidth == 32));

    if (compCount == 8)
    {
        // vec8 -> vec4 + vec4
        LLPC_ASSERT(bitWidth == 32);

        Constant* shuffleMask0123[] = {
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 2),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 3)
        };
        Value* pCompX4 = new ShuffleVectorInst(pOutput, pOutput, ConstantVector::get(shuffleMask0123), "", pInsertPos);

        StoreValueToStreamOutBuffer(pCompX4, xfbBuffer, xfbOffset, xfbStride, pStreamOutBufDesc, pInsertPos);

        Constant* shuffleMask4567[] = {
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 5),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 6),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 7)
        };
        pCompX4 = new ShuffleVectorInst(pOutput, pOutput, ConstantVector::get(shuffleMask4567), "", pInsertPos);

        xfbOffset += 4 * (bitWidth / 8);
        StoreValueToStreamOutBuffer(pCompX4, xfbBuffer, xfbOffset, xfbStride, pStreamOutBufDesc, pInsertPos);
    }
    else if (compCount == 6)
    {
        // vec6 -> vec4 + vec2
        LLPC_ASSERT(bitWidth == 32);

        // NOTE: This case is generated by copy shader, which casts 64-bit outputs to float.
        Constant* shuffleMask0123[] = {
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 2),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 3)
        };
        Value* pCompX4 = new ShuffleVectorInst(pOutput, pOutput, ConstantVector::get(shuffleMask0123), "", pInsertPos);

        StoreValueToStreamOutBuffer(pCompX4, xfbBuffer, xfbOffset, xfbStride, pStreamOutBufDesc, pInsertPos);

        Constant* shuffleMask45[] = {
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 5)
        };
        Value* pCompX2 = new ShuffleVectorInst(pOutput, pOutput, ConstantVector::get(shuffleMask45), "", pInsertPos);

        xfbOffset += 4 * (bitWidth / 8);
        StoreValueToStreamOutBuffer(pCompX2, xfbBuffer, xfbOffset, xfbStride, pStreamOutBufDesc, pInsertPos);
    }
    else if (compCount == 3)
    {
        // 16vec3 -> 16vec2 + 16scalar
        // vec3 -> vec2 + scalar
        Constant* shuffleMask01[] = {
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), 1)
        };
        Value* pCompX2 = new ShuffleVectorInst(pOutput, pOutput, ConstantVector::get(shuffleMask01), "", pInsertPos);

        StoreValueToStreamOutBuffer(pCompX2, xfbBuffer, xfbOffset, xfbStride, pStreamOutBufDesc, pInsertPos);

        Value* pComp = ExtractElementInst::Create(pOutput,
                                                    ConstantInt::get(Type::getInt32Ty(*m_pContext), 2),
                                                    "",
                                                    pInsertPos);

        xfbOffset += 2 * (bitWidth / 8);
        StoreValueToStreamOutBuffer(pComp, xfbBuffer, xfbOffset, xfbStride, pStreamOutBufDesc, pInsertPos);
    }
    else
    {
        // 16vec4, 16vec2, 16scalar
        // vec4, vec2, scalar
        if (pOutputTy->isVectorTy() && (compCount == 1))
        {
            // NOTE: We translate vec1 to scalar. SPIR-V translated from DX has such usage.
            pOutput = ExtractElementInst::Create(pOutput,
                                                    ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                                    "",
                                                    pInsertPos);
        }

        StoreValueToStreamOutBuffer(pOutput, xfbBuffer, xfbOffset, xfbStride, pStreamOutBufDesc, pInsertPos);
    }
}

// =====================================================================================================================
// Creates the LLPC intrinsic "llpc.streamoutbuffer.store.f32" to store value to to stream-out buffer.
void PatchInOutImportExport::CreateStreamOutBufferStoreFunction(
    Value*         pStoreValue,   // [in] Value to store
    uint32_t       xfbStride,     // Transform feedback stride
    std::string&   funcName)      // [out] Function name to add mangling to
{
    AddTypeMangling(nullptr, { pStoreValue }, funcName);

    // define void @llpc.streamoutbuffer.store.f32(
    //      float %storeValue, <4 x i32> %streamOutBufDesc, i32 %writeIndex, i32 %threadId,
    //      i32 %vertexCount, i32 %xfbOffset, i32 %streamOffset)
    // {
    // .entry
    //     %1 = icmp ult i32 %threadId, %vtxCount
    //     br i1 %1, label %.store, label %.end
    //
    // .store:
    //     call void llvm.amdgcn.struct.tbuffer.store.f32(
    //         float %storeValue, <4 x i32> %streamOutBufDesc, i32 %writeIndex,
    //         i32 %xfbOffset, i32 %streamOffset, i32 %format, i32 %coherent)
    //     br label %.end
    //
    // .end:
    //     ret void
    // }

    Type* argTys[] = {
        pStoreValue->getType(),                             // %storeValue
        VectorType::get(Type::getInt32Ty(*m_pContext), 4),  // %streamOutBufDesc
        Type::getInt32Ty(*m_pContext),                      // %writeIndex
        Type::getInt32Ty(*m_pContext),                      // %threadId
        Type::getInt32Ty(*m_pContext),                      // %vertexCount
        Type::getInt32Ty(*m_pContext),                      // %xfbOffset
        Type::getInt32Ty(*m_pContext)                       // %streamOffset
    };
    auto pFuncTy = FunctionType::get(Type::getVoidTy(*m_pContext), argTys, false);
    auto pFunc = Function::Create(pFuncTy, GlobalValue::InternalLinkage, funcName, m_pModule);

    pFunc->setCallingConv(CallingConv::C);
    pFunc->addFnAttr(Attribute::NoUnwind);
    pFunc->addFnAttr(Attribute::AlwaysInline);

    auto argIt = pFunc->arg_begin();
    Value* pStoredValue = argIt++;
    Value* pStreamOutBufDesc = argIt++;
    Value* pWriteIndex = argIt++;
    Value* pThreadId = argIt++;
    Value* pVertexCount = argIt++;
    Value* pXfbOffset = argIt++;
    Value* pStreamOffset = argIt;

    // Create ".end" block
    BasicBlock* pEndBlock = BasicBlock::Create(*m_pContext, ".end", pFunc);
    ReturnInst::Create(*m_pContext, pEndBlock);

    // Create ".store" block
    BasicBlock* pStoreBlock = BasicBlock::Create(*m_pContext, ".store", pFunc, pEndBlock);

    // Create entry block
    BasicBlock* pEntryBlock = BasicBlock::Create(*m_pContext, "", pFunc, pStoreBlock);
    auto pThreadValid = new ICmpInst(*pEntryBlock, ICmpInst::ICMP_ULT, pThreadId, pVertexCount);

    if (m_shaderStage != ShaderStageCopyShader)
    {
        // Setup out-of-range value. GPU will drop stream-out buffer writing when the thread is invalid.
        uint32_t outofRangeValue = 0xFFFFFFFF;
        // Divide outofRangeValue by xfbStride only for GFX8.
        if (m_gfxIp.major == 8)
        {
            outofRangeValue /= xfbStride;
        }
        outofRangeValue -= (m_pPipelineState->GetShaderWaveSize(m_shaderStage) - 1);
        Value* pOutofRangeValue = ConstantInt::get(Type::getInt32Ty(*m_pContext), outofRangeValue);
        pWriteIndex = SelectInst::Create(pThreadValid, pWriteIndex, pOutofRangeValue, "", pEntryBlock);
        BranchInst::Create(pStoreBlock, pEntryBlock);
    }
    else
    {
        BranchInst::Create(pStoreBlock, pEndBlock, pThreadValid, pEntryBlock);
    }

    auto pStoreTy = pStoreValue->getType();

    uint32_t compCount = pStoreTy->isVectorTy() ? pStoreTy->getVectorNumElements() : 1;
    LLPC_ASSERT(compCount <= 4);

    const uint64_t bitWidth = pStoreTy->getScalarSizeInBits();
    LLPC_ASSERT((bitWidth == 16) || (bitWidth == 32));

    uint32_t format = 0;
    std::string callName = "llvm.amdgcn.struct.tbuffer.store.";

    CombineFormat formatOprd = {};
    formatOprd.bits.nfmt = BUF_NUM_FORMAT_FLOAT;
    switch (compCount)
    {
    case 1:
        {
            formatOprd.bits.dfmt = (bitWidth == 32) ? BUF_DATA_FORMAT_32 : BUF_DATA_FORMAT_16;
            callName += (bitWidth == 32) ? "f32" : "f16";
            break;
        }
    case 2:
        {
            formatOprd.bits.dfmt = (bitWidth == 32) ? BUF_DATA_FORMAT_32_32 : BUF_DATA_FORMAT_16_16;
            callName += (bitWidth == 32) ? "v2f32" : "v2f16";
            break;
        }
    case 4:
        {
            formatOprd.bits.dfmt = (bitWidth == 32) ? BUF_DATA_FORMAT_32_32_32_32 : BUF_DATA_FORMAT_16_16_16_16;
            callName += (bitWidth == 32) ? "v4f32" : "v4f16";
            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }

    format = formatOprd.u32All;

#if LLPC_BUILD_GFX10
    if (m_gfxIp.major >= 10)
    {
        if (compCount == 4)
        {
            format = (bitWidth == 32) ? BUF_FORMAT_32_32_32_32_FLOAT : BUF_FORMAT_16_16_16_16_FLOAT;
        }
        else if (compCount == 2)
        {
            format = (bitWidth == 32) ? BUF_FORMAT_32_32_FLOAT : BUF_FORMAT_16_16_FLOAT;
        }
        else if (compCount == 1)
        {
            format = (bitWidth == 32) ? BUF_FORMAT_32_FLOAT : BUF_FORMAT_16_FLOAT;
        }
        else
        {
            LLPC_NEVER_CALLED();
        }
    }
#endif

    // byteOffset = streamOffsets[xfbBuffer] * 4 +
    //              (writeIndex + threadId) * bufferStride[bufferId] +
    //              xfbOffset
    CoherentFlag coherent = {};
    coherent.bits.glc = true;
    coherent.bits.slc = true;
    Value* args[] = {
        pStoredValue,                                                     // value
        pStreamOutBufDesc,                                                // desc
        pWriteIndex,                                                      // vindex
        pXfbOffset,                                                       // offset
        pStreamOffset,                                                    // soffset
        ConstantInt::get(Type::getInt32Ty(*m_pContext), format),          // format
        ConstantInt::get(Type::getInt32Ty(*m_pContext), coherent.u32All)  // glc, slc
    };
    EmitCall(callName, Type::getVoidTy(*m_pContext), args, NoAttrib, pStoreBlock);
    BranchInst::Create(pEndBlock, pStoreBlock);
}

// =====================================================================================================================
// Combines scalar values store to vector store
uint32_t PatchInOutImportExport::CombineBufferStore(
    const std::vector<Value*>& storeValues,   // [in] Values to store
    uint32_t                   startIdx,      // Starting index for load operation in the load value array
    uint32_t                   valueOffset,   // Value offset as a bias of buffer store offset
    Value*                     pBufDesc,      // [in] Buffer descriptor
    Value*                     pStoreOffset,  // [in] Buffer store offset
    Value*                     pBufBase,      // [in] Buffer base offset
    CoherentFlag               coherent,      // Buffer coherency
    Instruction*               pInsertPos)    // [in] Where to insert write instructions
{

    std::vector<uint32_t> formats;

    if (m_gfxIp.major <= 9)
    {
        formats =
        {
            ((BUF_NUM_FORMAT_FLOAT<<4) | (BUF_DATA_FORMAT_32)),
            ((BUF_NUM_FORMAT_FLOAT<<4) | (BUF_DATA_FORMAT_32_32)),
            ((BUF_NUM_FORMAT_FLOAT<<4) | (BUF_DATA_FORMAT_32_32_32)),
            ((BUF_NUM_FORMAT_FLOAT<<4) | (BUF_DATA_FORMAT_32_32_32_32)),
        };
    }

 #if LLPC_BUILD_GFX10
    else if (m_gfxIp.major == 10)
    {
        formats =
        {
            BUF_FORMAT_32_FLOAT,
            BUF_FORMAT_32_32_FLOAT,
            BUF_FORMAT_32_32_32_FLOAT,
            BUF_FORMAT_32_32_32_32_FLOAT
        };
    }
#endif
    else
    {
        LLPC_NOT_IMPLEMENTED();
    }

    Type* storeTys[4] =
    {
        Type::getInt32Ty(*m_pContext),
        VectorType::get(Type::getInt32Ty(*m_pContext), 2),
        VectorType::get(Type::getInt32Ty(*m_pContext), 3),
        VectorType::get(Type::getInt32Ty(*m_pContext), 4),
    };

    std::string funcName = "llvm.amdgcn.raw.tbuffer.store.";

    // Start from 4-component combination
    uint32_t compCount = 4;
    for (; compCount > 0; compCount--)
    {
        // GFX6 does not support 3-component combination
        if ((m_gfxIp.major == 6) && (compCount == 3))
            continue;

        if (startIdx + compCount <= storeValues.size())
        {
            funcName += GetTypeName(storeTys[compCount - 1]);
            Value* pStoreValue = nullptr;
            if (compCount > 1)
            {
                auto pStoreTy = VectorType::get(Type::getInt32Ty(*m_pContext), compCount);
                pStoreValue = UndefValue::get(pStoreTy);

                for (uint32_t i = 0; i < compCount; ++i)
                {
                    pStoreValue = InsertElementInst::Create(pStoreValue,
                                                            storeValues[startIdx + i],
                                                            ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                            "",
                                                            pInsertPos);
                }
            }
            else
            {
                pStoreValue = storeValues[startIdx];
            }

            auto pWriteOffset = BinaryOperator::CreateAdd(pStoreOffset,
                                                          ConstantInt::get(Type::getInt32Ty(*m_pContext),
                                                                           valueOffset * 4),
                                                          "",
                                                          pInsertPos);
            Value* args[] = {
                pStoreValue,                                                              // vdata
                pBufDesc,                                                                 // rsrc
                pWriteOffset,                                                             // voffset
                pBufBase,                                                                 // soffset
                ConstantInt::get(Type::getInt32Ty(*m_pContext), formats[compCount - 1]),  // format
                ConstantInt::get(Type::getInt32Ty(*m_pContext), coherent.u32All)          // glc
            };
            EmitCall(funcName, Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);

            break;
        }
    }

    return compCount;
}

// =====================================================================================================================
// Combines scalar values load to vector load
uint32_t PatchInOutImportExport::CombineBufferLoad(
    std::vector<Value*>& loadValues,    // [in/out] Values to load
    uint32_t             startIdx,      // Starting index for load operation in the load value array
    Value *              pBufDesc,      // [in] Buffer descriptor
    Value *              pLoadOffset,   // [in] Buffer load offset
    Value *              pBufBase,      // [in] Buffer base offset
    CoherentFlag         coherent,      // Buffer coherency
    Instruction*         pInsertPos)    // [in] Where to insert write instructions
{
    std::vector<uint32_t> formats;

    if (m_gfxIp.major <= 9)
    {
        formats =
        {
            ((BUF_NUM_FORMAT_FLOAT<<4) | (BUF_DATA_FORMAT_32)),
            ((BUF_NUM_FORMAT_FLOAT<<4) | (BUF_DATA_FORMAT_32_32)),
            ((BUF_NUM_FORMAT_FLOAT<<4) | (BUF_DATA_FORMAT_32_32_32)),
            ((BUF_NUM_FORMAT_FLOAT<<4) | (BUF_DATA_FORMAT_32_32_32_32)),
        };
    }

 #if LLPC_BUILD_GFX10
    else if (m_gfxIp.major == 10)
    {
        formats =
        {
            BUF_FORMAT_32_FLOAT,
            BUF_FORMAT_32_32_FLOAT,
            BUF_FORMAT_32_32_32_FLOAT,
            BUF_FORMAT_32_32_32_32_FLOAT
        };
    }
#endif
    else
    {
        LLPC_NOT_IMPLEMENTED();
    }

    Type* loadTyps[4] =
    {
        Type::getInt32Ty(*m_pContext),
        VectorType::get(Type::getInt32Ty(*m_pContext), 2),
        VectorType::get(Type::getInt32Ty(*m_pContext), 3),
        VectorType::get(Type::getInt32Ty(*m_pContext), 4),
    };

    std::string funcName = "llvm.amdgcn.raw.tbuffer.load.";
    LLPC_ASSERT(loadValues.size() > 0);

    // 4-component combination
    uint32_t compCount = 4;
    for (; compCount > 0; compCount--)
    {
        // GFX6 does not support 3-component combination
        if ((m_gfxIp.major == 6) && (compCount == 3))
            continue;

        if (startIdx + compCount <= loadValues.size())
        {
            funcName += GetTypeName(loadTyps[compCount - 1]);

            Value* pLoadValue = nullptr;
            auto pWriteOffset = BinaryOperator::CreateAdd(pLoadOffset,
                                                          ConstantInt::get(Type::getInt32Ty(*m_pContext),
                                                                           startIdx * 4),
                                                          "",
                                                          pInsertPos);
            Value* args[] = {
                pBufDesc,                                                                 // rsrc
                pWriteOffset,                                                             // voffset
                pBufBase,                                                                 // soffset
                ConstantInt::get(Type::getInt32Ty(*m_pContext), formats[compCount - 1]),  // format
                ConstantInt::get(Type::getInt32Ty(*m_pContext), coherent.u32All)          // glc
            };
            pLoadValue = EmitCall(funcName, loadTyps[compCount - 1], args, NoAttrib, pInsertPos);
            LLPC_ASSERT(pLoadValue != nullptr);
            if (compCount > 1)
            {
                for (uint32_t i = 0; i < compCount; i++)
                {
                    loadValues[startIdx + i] = ExtractElementInst::Create(
                                                      pLoadValue,
                                                      ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                      "",
                                                      pInsertPos);
                }
            }
            else
            {
                loadValues[startIdx] = pLoadValue;
            }

            break;
        }
    }

    return compCount;
}

// =====================================================================================================================
// Store value to stream-out buffer
void PatchInOutImportExport::StoreValueToStreamOutBuffer(
    Value*        pStoreValue,        // [in] Value to store
    uint32_t      xfbBuffer,          // Transform feedback buffer
    uint32_t      xfbOffset,          // Offset of the store value within transform feedback buffer
    uint32_t      xfbStride,          // Transform feedback stride
    Value*        pStreamOutBufDesc,  // [in] Transform feedback buffer descriptor
    Instruction * pInsertPos)         // [in] Where to insert the store instruction
{
    auto pStoreTy = pStoreValue->getType();

    uint32_t compCount = pStoreTy->isVectorTy() ? pStoreTy->getVectorNumElements() : 1;
    LLPC_ASSERT(compCount <= 4);

    const uint64_t bitWidth = pStoreTy->getScalarSizeInBits();
    LLPC_ASSERT((bitWidth == 16) || (bitWidth == 32));

    if (pStoreTy->isIntOrIntVectorTy())
    {
        Type* pBitCastTy = (bitWidth == 32) ? Type::getFloatTy(*m_pContext) : Type::getHalfTy(*m_pContext);
        if (compCount > 1)
        {
            pBitCastTy = VectorType::get(pBitCastTy, compCount);
        }
        pStoreValue = new BitCastInst(pStoreValue, pBitCastTy, "", pInsertPos);
    }

    const auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(m_shaderStage)->entryArgIdxs;

    uint32_t streamOffsets[MaxTransformFeedbackBuffers] = {};
    uint32_t writeIndex = 0;
    uint32_t streamInfo = 0;

    if (m_shaderStage == ShaderStageVertex)
    {
        memcpy(streamOffsets, entryArgIdxs.vs.streamOutData.streamOffsets, sizeof(streamOffsets));
        writeIndex = entryArgIdxs.vs.streamOutData.writeIndex;
        streamInfo = entryArgIdxs.vs.streamOutData.streamInfo;
    }
    else if (m_shaderStage == ShaderStageTessEval)
    {
        memcpy(streamOffsets, entryArgIdxs.tes.streamOutData.streamOffsets, sizeof(streamOffsets));
        writeIndex = entryArgIdxs.tes.streamOutData.writeIndex;
        streamInfo = entryArgIdxs.tes.streamOutData.streamInfo;
    }
    else
    {
        LLPC_ASSERT(m_shaderStage == ShaderStageCopyShader);

        writeIndex = CopyShaderUserSgprIdxWriteIndex;
        streamInfo = CopyShaderUserSgprIdxStreamInfo;

        auto& inoutUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry)->inOutUsage;
        uint32_t streamOffset = CopyShaderUserSgprIdxStreamOffset;

        for (uint32_t i = 0; i < MaxTransformFeedbackBuffers; ++i)
        {
            if (inoutUsage.xfbStrides[i] > 0)
            {
                streamOffsets[i] = streamOffset++;
            }
        }
    }

    LLPC_ASSERT(xfbBuffer < MaxTransformFeedbackBuffers);
    LLPC_ASSERT(streamOffsets[xfbBuffer] != 0);

    auto pStreamOffset = GetFunctionArgument(m_pEntryPoint, streamOffsets[xfbBuffer]);

    pStreamOffset = BinaryOperator::CreateMul(pStreamOffset,
                                              ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
                                              "",
                                              pInsertPos);

    auto pStreamInfo = GetFunctionArgument(m_pEntryPoint, streamInfo);

    // vertexCount = streamInfo[22:16]
    Value* ubfeArgs[] = {
        pStreamInfo,
        ConstantInt::get(Type::getInt32Ty(*m_pContext), 16),
        ConstantInt::get(Type::getInt32Ty(*m_pContext), 7)
    };
    Value* pVertexCount = EmitCall("llvm.amdgcn.ubfe.i32",
                                   Type::getInt32Ty(*m_pContext),
                                   ubfeArgs,
                                   NoAttrib,
                                   &*pInsertPos);

    // Setup write index for stream-out
    auto pWriteIndex = GetFunctionArgument(m_pEntryPoint, writeIndex);

    if (m_gfxIp.major >= 9)
    {
        pWriteIndex = BinaryOperator::CreateAdd(pWriteIndex, m_pThreadId, "", pInsertPos);
    }

    std::string funcName = LlpcName::StreamOutBufferStore;
    CreateStreamOutBufferStoreFunction(pStoreValue, xfbStride, funcName);

    Value* args[] = {
        pStoreValue,
        pStreamOutBufDesc,
        pWriteIndex,
        m_pThreadId,
        pVertexCount,
        ConstantInt::get(Type::getInt32Ty(*m_pContext), xfbOffset),
        pStreamOffset
    };
    EmitCall(funcName, Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
}

// =====================================================================================================================
// Stores value to ES-GS ring (buffer or LDS).
void PatchInOutImportExport::StoreValueToEsGsRing(
    Value*       pStoreValue,   // [in] Value to store
    uint32_t     location,      // Output location
    uint32_t     compIdx,       // Output component index
    Instruction* pInsertPos)    // [in] Where to insert the store instruction
{
    auto pStoreTy = pStoreValue->getType();

    Type* pElemTy = pStoreTy;
    if (pStoreTy->isArrayTy())
    {
        pElemTy = pStoreTy->getArrayElementType();
    }
    else if (pStoreTy->isVectorTy())
    {
        pElemTy = pStoreTy->getVectorElementType();
    }

    const uint64_t bitWidth = pElemTy->getScalarSizeInBits();
    LLPC_ASSERT((pElemTy->isFloatingPointTy() || pElemTy->isIntegerTy()) &&
                ((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32)));

    if (pStoreTy->isArrayTy() || pStoreTy->isVectorTy())
    {
        const uint32_t elemCount =
            pStoreTy->isArrayTy() ? pStoreTy->getArrayNumElements() : pStoreTy->getVectorNumElements();

        for (uint32_t i = 0; i < elemCount; ++i)
        {
            Value* pStoreElem = nullptr;
            if (pStoreTy->isArrayTy())
            {
                pStoreElem = ExtractValueInst::Create(pStoreValue, { i }, "", pInsertPos);
            }
            else
            {
                pStoreElem = ExtractElementInst::Create(pStoreValue,
                                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                        "",
                                                        pInsertPos);
            }

            StoreValueToEsGsRing(pStoreElem, location + (compIdx + i) / 4, (compIdx + i) % 4, pInsertPos);
        }
    }
    else
    {
        if ((bitWidth == 8) || (bitWidth == 16))
        {
            if (pStoreTy->isFloatingPointTy())
            {
                LLPC_ASSERT(bitWidth == 16);
                pStoreValue = new BitCastInst(pStoreValue, Type::getInt16Ty(*m_pContext), "", pInsertPos);
            }

            pStoreValue = new ZExtInst(pStoreValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
        }
        else
        {
            LLPC_ASSERT(bitWidth == 32);
            if (pStoreTy->isFloatingPointTy())
            {
                pStoreValue = new BitCastInst(pStoreValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
            }
        }

        // Call buffer store intrinsic or LDS store
        const auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(m_shaderStage)->entryArgIdxs;
        Value* pEsGsOffset = nullptr;
        if (m_shaderStage == ShaderStageVertex)
        {
            pEsGsOffset = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.vs.esGsOffset);
        }
        else
        {
            LLPC_ASSERT(m_shaderStage == ShaderStageTessEval);
            pEsGsOffset = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.tes.esGsOffset);
        }

        auto pRingOffset = CalcEsGsRingOffsetForOutput(location, compIdx, pEsGsOffset, pInsertPos);

        if (m_pPipelineState->IsGsOnChip() || (m_gfxIp.major >= 9))   // ES -> GS ring is always on-chip on GFX9+
        {
            Value* idxs[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                pRingOffset
            };
            Value* pStorePtr = GetElementPtrInst::Create(nullptr, m_pLds, idxs, "", pInsertPos);
            auto pStoreInst = new StoreInst(pStoreValue, pStorePtr, false, pInsertPos);
            pStoreInst->setAlignment(MaybeAlign(m_pLds->getAlignment()));
        }
        else
        {
            Value* pEsGsRingBufDesc = m_pipelineSysValues.Get(m_pEntryPoint)->GetEsGsRingBufDesc();

            // NOTE: Here we use tbuffer_store instruction instead of buffer_store because we have to do explicit control
            // of soffset. This is required by swizzle enabled mode when address range checking should be complied with.
            CombineFormat combineFormat = {};
            combineFormat.bits.dfmt = BUF_DATA_FORMAT_32;
            combineFormat.bits.nfmt = BUF_NUM_FORMAT_UINT;
            CoherentFlag coherent = {};
            coherent.bits.glc = true;
            coherent.bits.slc = true;
            coherent.bits.swz = true;
            Value* args[] = {
                pStoreValue,                                                            // vdata
                pEsGsRingBufDesc,                                                       // rsrc
                pRingOffset,                                                            // voffset
                pEsGsOffset,                                                            // soffset
                ConstantInt::get(Type::getInt32Ty(*m_pContext), combineFormat.u32All),
                ConstantInt::get(Type::getInt32Ty(*m_pContext), coherent.u32All)        // glc, slc, swz
            };
            EmitCall("llvm.amdgcn.raw.tbuffer.store.i32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
        }
    }
}

// =====================================================================================================================
// Loads value from ES-GS ring (buffer or LDS).
Value* PatchInOutImportExport::LoadValueFromEsGsRing(
    Type*        pLoadTy,       // [in] Load value type
    uint32_t     location,      // Input location
    uint32_t     compIdx,       // Input component index
    Value*       pVertexIdx,    // [in] Vertex index
    Instruction* pInsertPos)    // [in] Where to insert the load instruction
{
    Type* pElemTy = pLoadTy;
    if (pLoadTy->isArrayTy())
    {
        pElemTy = pLoadTy->getArrayElementType();
    }
    else if (pLoadTy->isVectorTy())
    {
        pElemTy = pLoadTy->getVectorElementType();
    }

    const uint64_t bitWidth = pElemTy->getScalarSizeInBits();
    LLPC_ASSERT((pElemTy->isFloatingPointTy() || pElemTy->isIntegerTy()) &&
                ((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32)));

    Value* pLoadValue = UndefValue::get(pLoadTy);

    if (pLoadTy->isArrayTy() || pLoadTy->isVectorTy())
    {
        const uint32_t elemCount =
            pLoadTy->isArrayTy() ? pLoadTy->getArrayNumElements() : pLoadTy->getVectorNumElements();

        for (uint32_t i = 0; i < elemCount; ++i)
        {
            auto pLoadElem = LoadValueFromEsGsRing(pElemTy,
                                                   location + (compIdx + i) / 4,
                                                   (compIdx + i) % 4,
                                                   pVertexIdx,
                                                   pInsertPos);

            if (pLoadTy->isArrayTy())
            {
                pLoadValue = InsertValueInst::Create(pLoadValue, pLoadElem, { i }, "", pInsertPos);
            }
            else
            {
                pLoadValue = InsertElementInst::Create(pLoadValue,
                                                       pLoadElem,
                                                       ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                       "",
                                                       pInsertPos);
            }
        }
    }
    else
    {
        Value* pRingOffset = CalcEsGsRingOffsetForInput(location, compIdx, pVertexIdx, pInsertPos);
        if (m_pPipelineState->IsGsOnChip() || (m_gfxIp.major >= 9))   // ES -> GS ring is always on-chip on GFX9
        {
            Value* idxs[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                pRingOffset
            };
            Value* pLoadPtr = GetElementPtrInst::Create(nullptr, m_pLds, idxs, "", pInsertPos);
            auto pLoadInst = new LoadInst(pLoadPtr, "", false, pInsertPos);
            pLoadInst->setAlignment(MaybeAlign(m_pLds->getAlignment()));
            pLoadValue = pLoadInst;

            if (bitWidth == 8)
            {
                pLoadValue = new TruncInst(pLoadValue, Type::getInt8Ty(*m_pContext), "", pInsertPos);
            }
            else if (bitWidth == 16)
            {
                pLoadValue = new TruncInst(pLoadValue, Type::getInt16Ty(*m_pContext), "", pInsertPos);
            }

            if (pLoadTy->isFloatingPointTy())
            {
                pLoadValue = new BitCastInst(pLoadValue, pLoadTy, "", pInsertPos);
            }
        }
        else
        {
            Value* pEsGsRingBufDesc = m_pipelineSysValues.Get(m_pEntryPoint)->GetEsGsRingBufDesc();
            CoherentFlag coherent = {};
            coherent.bits.glc = true;
            coherent.bits.slc = true;
            Value* args[] = {
                pEsGsRingBufDesc,                                                 // rsrc
                pRingOffset,                                                      // offset
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),               // soffset
                ConstantInt::get(Type::getInt32Ty(*m_pContext), coherent.u32All)  // glc slc
            };
            pLoadValue = EmitCall("llvm.amdgcn.raw.buffer.load.f32",
                                  Type::getFloatTy(*m_pContext),
                                  args,
                                  NoAttrib,
                                  pInsertPos);

            if (bitWidth == 8)
            {
                LLPC_ASSERT(pLoadTy->isIntegerTy());

                pLoadValue = new BitCastInst(pLoadValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
                pLoadValue = new TruncInst(pLoadValue, Type::getInt8Ty(*m_pContext), "", pInsertPos);
            }
            else if (bitWidth == 16)
            {
                pLoadValue = new BitCastInst(pLoadValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
                pLoadValue = new TruncInst(pLoadValue, Type::getInt16Ty(*m_pContext), "", pInsertPos);

                if (pLoadTy->isFloatingPointTy())
                {
                    pLoadValue = new BitCastInst(pLoadValue, pLoadTy, "", pInsertPos);
                }
            }
            else
            {
                LLPC_ASSERT(bitWidth == 32);
                if (pLoadTy->isIntegerTy())
                {
                    pLoadValue = new BitCastInst(pLoadValue, pLoadTy, "", pInsertPos);
                }
            }
        }
    }

    return pLoadValue;
}

// =====================================================================================================================
// Stores value to GS-VS ring (buffer or LDS).
void PatchInOutImportExport::StoreValueToGsVsRing(
    Value*       pStoreValue,   // [in] Value to store
    uint32_t     location,      // Output location
    uint32_t     compIdx,       // Output component index
    uint32_t     streamId,      // Output stream ID
    Instruction* pInsertPos)    // [in] Where to insert the store instruction
{
    auto pStoreTy = pStoreValue->getType();

    Type* pElemTy = pStoreTy;
    if (pStoreTy->isArrayTy())
    {
        pElemTy = pStoreTy->getArrayElementType();
    }
    else if (pStoreTy->isVectorTy())
    {
        pElemTy = pStoreTy->getVectorElementType();
    }

    const uint32_t bitWidth = pElemTy->getScalarSizeInBits();
    LLPC_ASSERT((pElemTy->isFloatingPointTy() || pElemTy->isIntegerTy()) &&
                ((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32)));

#if LLPC_BUILD_GFX10
    if (m_pPipelineState->GetNggControl()->enableNgg)
    {
        // NOTE: For NGG, exporting GS output to GS-VS ring is represented by a call and the call is replaced with
        // real instructions when when NGG primitive shader is generated.
        Value* args[] = {
            ConstantInt::get(Type::getInt32Ty(*m_pContext), location),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), compIdx),
            ConstantInt::get(Type::getInt32Ty(*m_pContext), streamId),
            pStoreValue
        };
        std::string callName = LlpcName::NggGsOutputExport + GetTypeName(pStoreTy);
        EmitCall(callName, Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
        return;
    }
#endif

    if (pStoreTy->isArrayTy() || pStoreTy->isVectorTy())
    {
        const uint32_t elemCount =
            pStoreTy->isArrayTy() ? pStoreTy->getArrayNumElements() : pStoreTy->getVectorNumElements();

        for (uint32_t i = 0; i < elemCount; ++i)
        {
            Value* pStoreElem = nullptr;
            if (pStoreTy->isArrayTy())
            {
                pStoreElem = ExtractValueInst::Create(pStoreValue, { i }, "", pInsertPos);
            }
            else
            {
                pStoreElem = ExtractElementInst::Create(pStoreValue,
                                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                        "",
                                                        pInsertPos);
            }

            StoreValueToGsVsRing(pStoreElem, location + (compIdx + i) / 4, (compIdx + i) % 4, streamId, pInsertPos);
        }
    }
    else
    {
        if ((bitWidth == 8) || (bitWidth == 16))
        {
            // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend BYTE/WORD
            // to DWORD. This is because copy shader does not know the actual data type. It only generates output
            // export calls based on number of DWORDs.
            if (pStoreTy->isFloatingPointTy())
            {
                LLPC_ASSERT(bitWidth == 16);
                pStoreValue = new BitCastInst(pStoreValue, Type::getInt16Ty(*m_pContext), "", pInsertPos);
            }

            pStoreValue = new ZExtInst(pStoreValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
        }
        else
        {
            LLPC_ASSERT(bitWidth == 32);
            if (pStoreTy->isFloatingPointTy())
            {
                pStoreValue = new BitCastInst(pStoreValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
            }
        }

        const auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(m_shaderStage)->entryArgIdxs;
        Value* pGsVsOffset = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.gs.gsVsOffset);

        auto pEmitCounterPtr = m_pipelineSysValues.Get(m_pEntryPoint)->GetEmitCounterPtr()[streamId];
        auto pEmitCounter = new LoadInst(pEmitCounterPtr, "", pInsertPos);

        auto pRingOffset =
            CalcGsVsRingOffsetForOutput(location, compIdx, streamId, pEmitCounter, pGsVsOffset, pInsertPos);

        if (m_pPipelineState->IsGsOnChip())
        {
            Value* idxs[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                pRingOffset
            };
            Value* pStorePtr = GetElementPtrInst::Create(nullptr, m_pLds, idxs, "", pInsertPos);
            auto pStoreInst = new StoreInst(pStoreValue, pStorePtr, false, pInsertPos);
            pStoreInst->setAlignment(MaybeAlign(m_pLds->getAlignment()));
        }
        else
        {
            // NOTE: Here we use tbuffer_store instruction instead of buffer_store because we have to do explicit
            // control of soffset. This is required by swizzle enabled mode when address range checking should be
            // complied with.
            if (m_gfxIp.major <= 9)
            {
                CombineFormat combineFormat = {};
                combineFormat.bits.dfmt = BUF_DATA_FORMAT_32;
                combineFormat.bits.nfmt = BUF_NUM_FORMAT_UINT;
                CoherentFlag coherent = {};
                coherent.bits.glc = true;
                coherent.bits.slc = true;
                coherent.bits.swz = true;
                Value* args[] = {
                    pStoreValue,                                                          // vdata
                    m_pipelineSysValues.Get(m_pEntryPoint)->GetGsVsRingBufDesc(streamId), // rsrc
                    pRingOffset,                                                          // voffset
                    pGsVsOffset,                                                          // soffset
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), combineFormat.u32All),
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), coherent.u32All)      // glc, slc, swz
                };
                EmitCall("llvm.amdgcn.raw.tbuffer.store.i32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
            }
#if LLPC_BUILD_GFX10
            else if (m_gfxIp.major == 10)
            {
                CoherentFlag coherent = {};
                coherent.bits.glc = true;
                coherent.bits.slc = true;
                coherent.bits.swz = true;
                Value* args[] = {
                    pStoreValue,                                                          // vdata
                    m_pipelineSysValues.Get(m_pEntryPoint)->GetGsVsRingBufDesc(streamId), // rsrc
                    pRingOffset,                                                          // voffset
                    pGsVsOffset,                                                          // soffset
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), BUF_FORMAT_32_UINT),  // format
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), coherent.u32All)      // glc, slc, swz
                };
                EmitCall("llvm.amdgcn.raw.tbuffer.store.i32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
            }
#endif
            else
            {
                LLPC_NOT_IMPLEMENTED();
            }
        }
    }
}

// =====================================================================================================================
// Calculates the byte offset to store the output value to ES-GS ring based on the specified output info.
Value* PatchInOutImportExport::CalcEsGsRingOffsetForOutput(
    uint32_t        location,    // Output location
    uint32_t        compIdx,     // Output component index
    Value*          pEsGsOffset, // [in] ES-GS ring offset in bytes
    Instruction*    pInsertPos)  // [in] Where to insert the instruction
{
    Value* pRingOffset = nullptr;
    if (m_pPipelineState->IsGsOnChip() || (m_gfxIp.major >= 9))   // ES -> GS ring is always on-chip on GFX9
    {
        // ringOffset = esGsOffset + threadId * esGsRingItemSize + location * 4 + compIdx

        LLPC_ASSERT(m_pPipelineState->HasShaderStage(ShaderStageGeometry));
        const auto& calcFactor = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry)->
                                                                    inOutUsage.gs.calcFactor;

        pEsGsOffset = BinaryOperator::CreateLShr(pEsGsOffset,
                                                 ConstantInt::get(Type::getInt32Ty(*m_pContext), 2),
                                                 "",
                                                 pInsertPos);

        pRingOffset = BinaryOperator::CreateMul(m_pThreadId,
                                                ConstantInt::get(Type::getInt32Ty(*m_pContext),
                                                                 calcFactor.esGsRingItemSize),
                                                "",
                                                pInsertPos);

        pRingOffset = BinaryOperator::CreateAdd(pRingOffset, pEsGsOffset, "", pInsertPos);

        pRingOffset = BinaryOperator::CreateAdd(pRingOffset,
                                                ConstantInt::get(Type::getInt32Ty(*m_pContext),
                                                                 (location * 4 + compIdx)),
                                                "",
                                                pInsertPos);
    }
    else
    {
        // ringOffset = (location * 4 + compIdx) * 4
        pRingOffset = ConstantInt::get(Type::getInt32Ty(*m_pContext), (location * 4 + compIdx) * 4);
    }
    return pRingOffset;
}

// =====================================================================================================================
// Calculates the byte offset to load the input value from ES-GS ring based on the specified input info.
Value* PatchInOutImportExport::CalcEsGsRingOffsetForInput(
    uint32_t        location,    // Input location
    uint32_t        compIdx,     // Input Component index
    Value*          pVertexIdx,  // [in] Vertex index
    Instruction*    pInsertPos)  // [in] Where to insert the instruction
{
    Value* pRingOffset = nullptr;
    auto pEsGsOffsets = m_pipelineSysValues.Get(m_pEntryPoint)->GetEsGsOffsets();

    if (m_pPipelineState->IsGsOnChip() || (m_gfxIp.major >= 9))   // ES -> GS ring is always on-chip on GFX9
    {
        Value* pVertexOffset = ExtractElementInst::Create(pEsGsOffsets,
                                                          pVertexIdx,
                                                          "",
                                                          pInsertPos);

        // ringOffset = vertexOffset[N] + (location * 4 + compIdx);
        pRingOffset =
            BinaryOperator::CreateAdd(pVertexOffset,
                                      ConstantInt::get(Type::getInt32Ty(*m_pContext), (location * 4 + compIdx)),
                                      "",
                                      pInsertPos);
    }
    else
    {
        Value* pVertexOffset = ExtractElementInst::Create(pEsGsOffsets,
                                                          pVertexIdx,
                                                          "",
                                                          pInsertPos);

        // ringOffset = vertexOffset[N] * 4 + (location * 4 + compIdx) * 64 * 4;
        pRingOffset = BinaryOperator::CreateMul(pVertexOffset,
                                                ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
                                                "",
                                                pInsertPos);

        pRingOffset =
            BinaryOperator::CreateAdd(pRingOffset,
                                      ConstantInt::get(Type::getInt32Ty(*m_pContext), (location * 4 + compIdx) * 64 * 4),
                                      "",
                                      pInsertPos);
    }

    return pRingOffset;
}

// =====================================================================================================================
// Calculates the offset to store the output value to GS-VS ring based on the specified output info.
Value* PatchInOutImportExport::CalcGsVsRingOffsetForOutput(
    uint32_t        location,    // Output location
    uint32_t        compIdx,     // Output component
    uint32_t        streamId,    // Output stream ID
    Value*          pVertexIdx,  // [in] Vertex index
    Value*          pGsVsOffset, // [in] ES-GS ring offset in bytes
    Instruction*    pInsertPos)  // [in] Where to insert the instruction
{
    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);

    Value* pRingOffset = nullptr;

    uint32_t streamBases[MaxGsStreams];
    uint32_t streamBase = 0;
    for (int i = 0; i < MaxGsStreams; ++i)
    {
        streamBases[i] = streamBase;
        streamBase += ( pResUsage->inOutUsage.gs.outLocCount[i] *
            m_pPipelineState->GetShaderModes()->GetGeometryShaderMode().outputVertices * 4);
    }

    if (m_pPipelineState->IsGsOnChip())
    {
        // ringOffset = esGsLdsSize +
        //              gsVsOffset +
        //              threadId * gsVsRingItemSize +
        //              (vertexIdx * vertexSizePerStream) + location * 4 + compIdx + streamBase (in DWORDS)

        auto pEsGsLdsSize = ConstantInt::get(Type::getInt32Ty(*m_pContext),
                                             pResUsage->inOutUsage.gs.calcFactor.esGsLdsSize);

        pGsVsOffset = BinaryOperator::CreateExact(Instruction::LShr,
                                                  pGsVsOffset,
                                                  ConstantInt::get(Type::getInt32Ty(*m_pContext), 2),
                                                  "",
                                                  pInsertPos);

        auto pRingItemOffset =
            BinaryOperator::CreateMul(m_pThreadId,
                                      ConstantInt::get(Type::getInt32Ty(*m_pContext),
                                                       pResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize),
                                      "",
                                      pInsertPos);

        // VertexSize is stream output vertexSize x 4 (in DWORDS)
        uint32_t vertexSize = pResUsage->inOutUsage.gs.outLocCount[streamId] * 4;
        auto pVertexItemOffset = BinaryOperator::CreateMul(pVertexIdx,
                                                           ConstantInt::get(Type::getInt32Ty(*m_pContext), vertexSize),
                                                           "",
                                                           pInsertPos);

        pRingOffset = BinaryOperator::CreateAdd(pEsGsLdsSize, pGsVsOffset, "", pInsertPos);

        pRingOffset = BinaryOperator::CreateAdd(pRingOffset, pRingItemOffset, "", pInsertPos);

        pRingOffset = BinaryOperator::CreateAdd(pRingOffset, pVertexItemOffset, "", pInsertPos);

        uint32_t attribOffset = (location * 4) + compIdx + streamBases[streamId];
        pRingOffset = BinaryOperator::CreateAdd(pRingOffset,
                                                ConstantInt::get(Type::getInt32Ty(*m_pContext), attribOffset),
                                                "",
                                                pInsertPos);
    }
    else
    {
        // ringOffset = ((location * 4 + compIdx) * maxVertices + vertexIdx) * 4 (in bytes);

        uint32_t outputVertices = m_pPipelineState->GetShaderModes()->GetGeometryShaderMode().outputVertices;

        pRingOffset = BinaryOperator::CreateAdd(ConstantInt::get(Type::getInt32Ty(*m_pContext),
                                                                 (location * 4 + compIdx) * outputVertices),
                                                pVertexIdx,
                                                "",
                                                pInsertPos);

        pRingOffset = BinaryOperator::CreateMul(pRingOffset,
                                                ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
                                                "",
                                                pInsertPos);
    }

    return pRingOffset;
}

// =====================================================================================================================
// Reads value from LDS.
Value* PatchInOutImportExport::ReadValueFromLds(
    bool         isOutput,    // is the value from output variable
    Type*        pReadTy,     // [in] Type of value read from LDS
    Value*       pLdsOffset,  // [in] Start offset to do LDS read operations
    Instruction* pInsertPos)  // [in] Where to insert read instructions
{
    LLPC_ASSERT(m_pLds != nullptr);
    LLPC_ASSERT(pReadTy->isSingleValueType());

    // Read DWORDs from LDS
    const uint32_t compCount = pReadTy->isVectorTy() ? pReadTy->getVectorNumElements() : 1;
    const uint32_t bitWidth = pReadTy->getScalarSizeInBits();
    LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32) || (bitWidth == 64));
    const uint32_t numChannels = compCount * ((bitWidth == 64) ? 2 : 1);

    std::vector<Value*> loadValues(numChannels);

    const bool isTcsOutput = (isOutput && (m_shaderStage == ShaderStageTessControl));
    const bool isTesInput = ((isOutput == false) && (m_shaderStage == ShaderStageTessEval));

    if (m_pPipelineState->IsTessOffChip() && (isTcsOutput || isTesInput)) // Read from off-chip LDS buffer
    {
        const auto& offChipLdsBase = (m_shaderStage == ShaderStageTessEval) ?
            m_pPipelineState->GetShaderInterfaceData(m_shaderStage)->entryArgIdxs.tes.offChipLdsBase :
            m_pPipelineState->GetShaderInterfaceData(m_shaderStage)->entryArgIdxs.tcs.offChipLdsBase;

        auto pOffChipLdsDesc = m_pipelineSysValues.Get(m_pEntryPoint)->GetOffChipLdsDesc();

        auto pOffChipLdsBase = GetFunctionArgument(m_pEntryPoint, offChipLdsBase);

        // Convert DWORD off-chip LDS offset to byte offset
        pLdsOffset = BinaryOperator::CreateMul(pLdsOffset,
                                               ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
                                               "",
                                               pInsertPos);

        CoherentFlag coherent = {};
        if (m_gfxIp.major <= 9)
        {
            coherent.bits.glc = true;
        }

    #if LLPC_BUILD_GFX10
        else if (m_gfxIp.major == 10)
        {
            coherent.bits.glc = true;
            coherent.bits.dlc = true;
        }
    #endif
        else
        {
            LLPC_NOT_IMPLEMENTED();
        }

        for (uint32_t i = 0, combineCount = 0; i < numChannels; i += combineCount)
        {
            combineCount = CombineBufferLoad(loadValues,
                                        i,
                                        pOffChipLdsDesc,
                                        pLdsOffset,
                                        pOffChipLdsBase,
                                        coherent,
                                        pInsertPos);

            for (uint32_t j = i; j < i + combineCount; ++j)
            {
                if (bitWidth == 8)
                {
                    loadValues[j] = new TruncInst(loadValues[j], Type::getInt8Ty(*m_pContext), "", pInsertPos);
                }
                else if (bitWidth == 16)
                {
                    loadValues[j] = new TruncInst(loadValues[j], Type::getInt16Ty(*m_pContext), "", pInsertPos);
                }
            }
        }
    }
    else // Read from on-chip LDS
    {
        for (uint32_t i = 0; i < numChannels; ++i)
        {
            Value* idxs[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                pLdsOffset
            };
            Value* pLoadPtr = GetElementPtrInst::Create(nullptr, m_pLds, idxs, "", pInsertPos);
            auto pLoadInst = new LoadInst(pLoadPtr, "", false, pInsertPos);
            pLoadInst->setAlignment(MaybeAlign(m_pLds->getAlignment()));
            loadValues[i]=pLoadInst;

            if (bitWidth == 8)
            {
                loadValues[i] = new TruncInst(loadValues[i], Type::getInt8Ty(*m_pContext), "", pInsertPos);
            }
            else if (bitWidth == 16)
            {
                loadValues[i] = new TruncInst(loadValues[i], Type::getInt16Ty(*m_pContext), "", pInsertPos);
            }

            pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                                   ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                   "",
                                                   pInsertPos);
        }
    }

    // Construct <n x i8>, <n x i16>, or <n x i32> vector from load values (DWORDs)
    Value* pCastValue = nullptr;
    if (numChannels > 1)
    {
        auto pIntTy = ((bitWidth == 32) || (bitWidth == 64)) ?
                          Type::getInt32Ty(*m_pContext) :
                          ((bitWidth == 16) ? Type::getInt16Ty(*m_pContext) : Type::getInt8Ty(*m_pContext));
        auto pCastTy = VectorType::get(pIntTy, numChannels);
        pCastValue = UndefValue::get(pCastTy);

        for (uint32_t i = 0; i < numChannels; ++i)
        {
            pCastValue = InsertElementInst::Create(pCastValue,
                                                   loadValues[i],
                                                   ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                   "",
                                                   pInsertPos);
        }
    }
    else
    {
        pCastValue = loadValues[0];
    }

    // Cast <n x i8>, <n x i16> or <n x i32> vector to read value
    return new BitCastInst(pCastValue, pReadTy, "", pInsertPos);
}

// =====================================================================================================================
// Writes value to LDS.
void PatchInOutImportExport::WriteValueToLds(
    Value*        pWriteValue,   // [in] Value written to LDS
    Value*        pLdsOffset,    // [in] Start offset to do LDS write operations
    Instruction*  pInsertPos)    // [in] Where to insert write instructions
{
    LLPC_ASSERT(m_pLds != nullptr);

    auto pWriteTy = pWriteValue->getType();
    LLPC_ASSERT(pWriteTy->isSingleValueType());

    const uint32_t compCout = pWriteTy->isVectorTy() ? pWriteTy->getVectorNumElements() : 1;
    const uint32_t bitWidth = pWriteTy->getScalarSizeInBits();
    LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32) || (bitWidth == 64));
    const uint32_t numChannels = compCout * ((bitWidth == 64) ? 2 : 1);

    // Cast write value to <n x i32> vector
    Type* pIntTy = ((bitWidth == 32) || (bitWidth == 64)) ?
                      Type::getInt32Ty(*m_pContext) :
                      ((bitWidth == 16) ? Type::getInt16Ty(*m_pContext) : Type::getInt8Ty(*m_pContext));
    Type* pCastTy = (numChannels > 1) ? cast<Type>(VectorType::get(pIntTy, numChannels)) : pIntTy;
    Value* pCastValue = new BitCastInst(pWriteValue, pCastTy, "", pInsertPos);

    // Extract store values (DWORDs) from <n x i8>, <n x i16> or <n x i32> vector
    std::vector<Value*> storeValues(numChannels);
    if (numChannels > 1)
    {
        for (uint32_t i = 0; i < numChannels; ++i)
        {
            storeValues[i] = ExtractElementInst::Create(pCastValue,
                                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                        "",
                                                        pInsertPos);

            if ((bitWidth == 8) || (bitWidth == 16))
            {
                storeValues[i] = new ZExtInst(storeValues[i], Type::getInt32Ty(*m_pContext), "", pInsertPos);
            }
        }
    }
    else
    {
        storeValues[0] = pCastValue;

        if ((bitWidth == 8) || (bitWidth == 16))
        {
            storeValues[0] = new ZExtInst(storeValues[0], Type::getInt32Ty(*m_pContext), "", pInsertPos);
        }
    }

    if (m_pPipelineState->IsTessOffChip() && (m_shaderStage == ShaderStageTessControl))     // Write to off-chip LDS buffer
    {
        auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(m_shaderStage)->entryArgIdxs.tcs;

        auto pOffChipLdsBase = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.offChipLdsBase);
        // Convert DWORD off-chip LDS offset to byte offset
        pLdsOffset = BinaryOperator::CreateMul(pLdsOffset,
                                               ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
                                               "",
                                               pInsertPos);

        auto pOffChipLdsDesc = m_pipelineSysValues.Get(m_pEntryPoint)->GetOffChipLdsDesc();

        CoherentFlag coherent = {};
        coherent.bits.glc = true;

        for (uint32_t i = 0, combineCount = 0; i < numChannels; i += combineCount)
        {
            combineCount = CombineBufferStore(storeValues,
                                                i,
                                                i,
                                                pOffChipLdsDesc,
                                                pLdsOffset,
                                                pOffChipLdsBase,
                                                coherent,
                                                pInsertPos);
        }
    }
    else // Write to on-chip LDS
    {
        for (uint32_t i = 0; i < numChannels; ++i)
        {
            Value* idxs[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                pLdsOffset
            };
            Value* pStorePtr = GetElementPtrInst::Create(nullptr, m_pLds, idxs, "", pInsertPos);
            auto pStoreInst = new StoreInst(storeValues[i], pStorePtr, false, pInsertPos);
            pStoreInst->setAlignment(MaybeAlign(m_pLds->getAlignment()));

            pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                                   ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                   "",
                                                   pInsertPos);
        }
    }
}

// =====================================================================================================================
// Calculates start offset of tessellation factors in the TF buffer.
Value* PatchInOutImportExport::CalcTessFactorOffset(
    bool         isOuter,     // Whether the calculation is for tessellation outer factors
    Value*       pElemIdx,    // [in] Index used for array element indexing (could be null)
    Instruction* pInsertPos)  // [in] Where to insert store instructions
{
    LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);

    // NOTE: Tessellation factors are from tessellation level array and we have:
    //   (1) Isoline
    //      tessFactor[0] = gl_TessLevelOuter[1]
    //      tessFactor[1] = gl_TessLevelOuter[0]
    //   (2) Triangle
    //      tessFactor[0] = gl_TessLevelOuter[0]
    //      tessFactor[1] = gl_TessLevelOuter[1]
    //      tessFactor[2] = gl_TessLevelOuter[2]
    //      tessFactor[3] = gl_TessLevelInner[0]
    //   (3) Quad
    //      tessFactor[0] = gl_TessLevelOuter[0]
    //      tessFactor[1] = gl_TessLevelOuter[1]
    //      tessFactor[2] = gl_TessLevelOuter[2]
    //      tessFactor[3] = gl_TessLevelOuter[3]
    //      tessFactor[4] = gl_TessLevelInner[0]
    //      tessFactor[5] = gl_TessLevelInner[1]

    uint32_t tessFactorCount = 0;
    uint32_t tessFactorStart = 0;
    auto primitiveMode = m_pPipelineState->GetShaderModes()->GetTessellationMode().primitiveMode;
    switch (primitiveMode)
    {
    case PrimitiveMode::Isolines:
        tessFactorCount = isOuter ? 2 : 0;
        tessFactorStart = isOuter ? 0 : 2;
        break;
    case PrimitiveMode::Triangles:
        tessFactorCount = isOuter ? 3 : 1;
        tessFactorStart = isOuter ? 0 : 3;
        break;
    case PrimitiveMode::Quads:
        tessFactorCount = isOuter ? 4 : 2;
        tessFactorStart = isOuter ? 0 : 4;
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    Value* pTessFactorOffset = ConstantInt::get(Type::getInt32Ty(*m_pContext), tessFactorStart);
    if (pElemIdx != nullptr)
    {
        if (isa<ConstantInt>(pElemIdx))
        {
            // Constant element indexing
            uint32_t elemIdx = cast<ConstantInt>(pElemIdx)->getZExtValue();
            if (elemIdx < tessFactorCount)
            {
                if ((primitiveMode == PrimitiveMode::Isolines) && isOuter)
                {
                    // NOTE: In case of the isoline,  hardware wants two tessellation factor: the first is detail
                    // TF, the second is density TF. The order is reversed, different from GLSL spec.
                    LLPC_ASSERT(tessFactorCount == 2);
                    elemIdx = 1 - elemIdx;
                }

                pTessFactorOffset = ConstantInt::get(Type::getInt32Ty(*m_pContext), tessFactorStart + elemIdx);
            }
            else
            {
                // Out of range, drop it
                pTessFactorOffset = ConstantInt::get(Type::getInt32Ty(*m_pContext), InvalidValue);
            }
        }
        else
        {
            // Dynamic element indexing
            if ((primitiveMode == PrimitiveMode::Isolines) && isOuter)
            {
                // NOTE: In case of the isoline,  hardware wants two tessellation factor: the first is detail
                // TF, the second is density TF. The order is reversed, different from GLSL spec.
                LLPC_ASSERT(tessFactorCount == 2);

                // elemIdx = (elemIdx <= 1) ? 1 - elemIdx : elemIdx
                auto pCond = new ICmpInst(pInsertPos,
                                          ICmpInst::ICMP_ULE,
                                          pElemIdx,
                                          ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                          "");

                auto pSwapElemIdx = BinaryOperator::CreateSub(ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                              pElemIdx,
                                                              "",
                                                              pInsertPos);

                pElemIdx = SelectInst::Create(pCond, pSwapElemIdx, pElemIdx, "", pInsertPos);
            }

            // tessFactorOffset = (elemIdx < tessFactorCount) ? (tessFactorStart + elemIdx) : invalidValue
            pTessFactorOffset = BinaryOperator::CreateAdd(pTessFactorOffset,
                                                          pElemIdx,
                                                          "",
                                                          pInsertPos);

            auto pCond = new ICmpInst(pInsertPos,
                                      ICmpInst::ICMP_ULT,
                                      pElemIdx,
                                      ConstantInt::get(Type::getInt32Ty(*m_pContext), tessFactorCount),
                                      "");

            pTessFactorOffset = SelectInst::Create(pCond,
                                                   pTessFactorOffset,
                                                   ConstantInt::get(Type::getInt32Ty(*m_pContext), InvalidValue),
                                                   "",
                                                   pInsertPos);
        }
    }

    return pTessFactorOffset;
}

// =====================================================================================================================
// Stores tessellation factors (outer/inner) to corresponding tessellation factor (TF) buffer.
void PatchInOutImportExport::StoreTessFactorToBuffer(
    const std::vector<Value*>& tessFactors,         // [in] Tessellation factors to be stored
    Value*                     pTessFactorOffset,   // [in] Start offset to store the specified tessellation factors
    Instruction*               pInsertPos)          // [in] Where to insert store instructions
{
    LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);

    if (tessFactors.size() == 0)
    {
        // No tessellation factor should be stored
        return;
    }

    const auto& inOutUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs;
    const auto& calcFactor = inOutUsage.calcFactor;

    auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageTessControl)->entryArgIdxs.tcs;
    auto pTfBufferBase = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.tfBufferBase);

    auto pTessFactorStride = ConstantInt::get(Type::getInt32Ty(*m_pContext), calcFactor.tessFactorStride);

    if (isa<ConstantInt>(pTessFactorOffset))
    {
        uint32_t tessFactorOffset = cast<ConstantInt>(pTessFactorOffset)->getZExtValue();
        if (tessFactorOffset == InvalidValue)
        {
            // Out of range, drop it
            return;
        }

        auto pRelativeId = m_pipelineSysValues.Get(m_pEntryPoint)->GetRelativeId();
        Value* pTfBufferOffset = BinaryOperator::CreateMul(pRelativeId, pTessFactorStride, "", pInsertPos);
        pTfBufferOffset = BinaryOperator::CreateMul(pTfBufferOffset,
                                                    ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
                                                    "",
                                                    pInsertPos);

        auto pTfBufDesc = m_pipelineSysValues.Get(m_pEntryPoint)->GetTessFactorBufDesc();
        std::vector<Value*> tfValues(tessFactors.size());
        for (uint32_t i = 0; i < tessFactors.size(); i++)
        {
            tfValues[i] = new BitCastInst(tessFactors[i], Type::getInt32Ty(*m_pContext), "", pInsertPos);
        }

        CoherentFlag coherent = {};
        coherent.bits.glc = true;

        for (uint32_t i = 0, combineCount = 0; i < tessFactors.size(); i+= combineCount)
        {
            uint32_t  tfValueOffset = i + tessFactorOffset;
            if (m_gfxIp.major <= 8)
            {
                // NOTE: Additional 4-byte offset is required for tessellation off-chip mode (pre-GFX9).
                tfValueOffset += (m_pPipelineState->IsTessOffChip() ? 1 : 0);
            }
            combineCount = CombineBufferStore(tfValues,
                                              i,
                                              tfValueOffset,
                                              pTfBufDesc,
                                              pTfBufferOffset,
                                              pTfBufferBase,
                                              coherent,
                                              pInsertPos);
        }
    }
    else
    {
        // Must be element indexing of tessellation level array
        LLPC_ASSERT(tessFactors.size() == 1);

        if (m_pModule->getFunction(LlpcName::TfBufferStore) == nullptr)
        {
            CreateTessBufferStoreFunction();
        }

        if (m_pPipelineState->IsTessOffChip())
        {
            if (m_gfxIp.major <= 8)
            {
                // NOTE: Additional 4-byte offset is required for tessellation off-chip mode (pre-GFX9).
                pTfBufferBase = BinaryOperator::CreateAdd(pTfBufferBase,
                                                          ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
                                                          "",
                                                          pInsertPos);
            }

        }

        Value* args[] = {
            m_pipelineSysValues.Get(m_pEntryPoint)->GetTessFactorBufDesc(), // tfBufferDesc
            pTfBufferBase,                                                  // tfBufferBase
            m_pipelineSysValues.Get(m_pEntryPoint)->GetRelativeId(),        // relPatchId
            pTessFactorStride,                                              // tfStride
            pTessFactorOffset,                                              // tfOffset
            tessFactors[0]                                                  // tfValue
        };
        EmitCall(LlpcName::TfBufferStore, Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
    }
}

// =====================================================================================================================
// Creates the LLPC intrinsic "llpc.tfbuffer.store.f32" to store tessellation factor (dynamic element indexing for
// tessellation level array).
void PatchInOutImportExport::CreateTessBufferStoreFunction()
{
    // define void @llpc.tfbuffer.store.f32(
    //     <4 x i32> %tfBufferDesc, i32 %tfBufferBase, i32 %relPatchId, i32 %tfStride, i32 %tfOffset, float %tfValue)
    // {
    //     %1 = icmp ne i32 %tfOffset, -1 (invalidValue)
    //     br i1 %1, label %.tfstore, label %.end
    //
    // .tfstore:
    //     %2 = mul i32 %tfStride, 4
    //     %3 = mul i32 %relPatchId, %2
    //     %4 = mul i32 %tfOffset, 4
    //     %5 = add i32 %3, %4
    //     %6 = add i32 %tfBufferBase, %5
    //     call void @llvm.amdgcn.raw.buffer.store.f32(
    //         float %tfValue, <4 x i32> %tfBufferDesc, i32 %6, i32 0, i32 1)
    //     br label %.end
    //
    // .end:
    //     ret void
    // }
    Type* argTys[] = {
        VectorType::get(Type::getInt32Ty(*m_pContext), 4),  // TF buffer descriptor
        Type::getInt32Ty(*m_pContext),                      // TF buffer base
        Type::getInt32Ty(*m_pContext),                      // Relative patch ID
        Type::getInt32Ty(*m_pContext),                      // TF stride
        Type::getInt32Ty(*m_pContext),                      // TF offset
        Type::getFloatTy(*m_pContext)                       // TF value
    };
    auto pFuncTy = FunctionType::get(Type::getVoidTy(*m_pContext), argTys, false);
    auto pFunc = Function::Create(pFuncTy, GlobalValue::InternalLinkage, LlpcName::TfBufferStore, m_pModule);

    pFunc->setCallingConv(CallingConv::C);
    pFunc->addFnAttr(Attribute::NoUnwind);
    pFunc->addFnAttr(Attribute::AlwaysInline);

    auto argIt = pFunc->arg_begin();

    Value* pTfBufferDesc = argIt++;
    pTfBufferDesc->setName("tfBufferDesc");

    Value* pTfBufferBase = argIt++;
    pTfBufferBase->setName("tfBufferBase");

    Value* pRelPatchId = argIt++;
    pRelPatchId->setName("relPatchId");

    Value* pTfStride = argIt++;
    pTfStride->setName("tfStride");

    Value* pTfOffset = argIt++;
    pTfOffset->setName("tfOffset");

    Value* pTfValue = argIt++;
    pTfValue->setName("tfValue");

    // Create ".end" block
    BasicBlock* pEndBlock = BasicBlock::Create(*m_pContext, ".end", pFunc);
    ReturnInst::Create(*m_pContext, pEndBlock);

    // Create ".tfstore" block
    BasicBlock* pTfStoreBlock = BasicBlock::Create(*m_pContext, ".tfstore", pFunc, pEndBlock);

    Value *pTfByteOffset = BinaryOperator::CreateMul(pTfOffset,
                                                     ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
                                                     "",
                                                     pTfStoreBlock);

    Value* pTfByteStride = BinaryOperator::CreateMul(pTfStride,
                                                     ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
                                                     "",
                                                     pTfStoreBlock);
    Value* pTfBufferOffset = BinaryOperator::CreateMul(pRelPatchId, pTfByteStride, "", pTfStoreBlock);

    pTfBufferOffset = BinaryOperator::CreateAdd(pTfBufferOffset, pTfByteOffset, "", pTfStoreBlock);
    pTfBufferOffset = BinaryOperator::CreateAdd(pTfBufferOffset, pTfBufferBase, "", pTfStoreBlock);

    auto pBranch = BranchInst::Create(pEndBlock, pTfStoreBlock);

    Value* args[] = {
        pTfValue,                                           // vdata
        pTfBufferDesc,                                      // rsrc
        pTfBufferOffset,                                    // offset
        ConstantInt::get(Type::getInt32Ty(*m_pContext), 0), // soffset
        ConstantInt::get(Type::getInt32Ty(*m_pContext), 1)  // cachepolicy: glc = 1
    };
    EmitCall("llvm.amdgcn.raw.buffer.store.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pBranch);

    // Create entry block
    BasicBlock* pEntryBlock = BasicBlock::Create(*m_pContext, "", pFunc, pTfStoreBlock);
    Value* pCond = new ICmpInst(*pEntryBlock,
                                ICmpInst::ICMP_NE,
                                pTfOffset,
                                ConstantInt::get(Type::getInt32Ty(*m_pContext), InvalidValue),
                                "");
    BranchInst::Create(pTfStoreBlock, pEndBlock, pCond, pEntryBlock);
}

// =====================================================================================================================
// Calculates the DWORD offset to write value to LDS based on the specified VS output info.
Value* PatchInOutImportExport::CalcLdsOffsetForVsOutput(
    Type*        pOutputTy,     // [in] Type of the output
    uint32_t     location,      // Base location of the output
    uint32_t     compIdx,       // Index used for vector element indexing
    Instruction* pInsertPos)    // [in] Where to insert calculation instructions
{
    LLPC_ASSERT(m_shaderStage == ShaderStageVertex);

    // attribOffset = location * 4 + compIdx
    Value* pAttribOffset = ConstantInt::get(Type::getInt32Ty(*m_pContext), location * 4);

    const uint32_t bitWidth = pOutputTy->getScalarSizeInBits();
    LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32) || (bitWidth == 64));

    if (bitWidth == 64)
    {
        // For 64-bit data type, the component indexing must multiply by 2
        compIdx *= 2;
    }

    pAttribOffset = BinaryOperator::CreateAdd(pAttribOffset,
                                              ConstantInt::get(Type::getInt32Ty(*m_pContext), compIdx),
                                              "",
                                              pInsertPos);

    const auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageVertex)->entryArgIdxs.vs;
    auto pRelVertexId = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.relVertexId);

    const auto& calcFactor = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
    auto pVertexStride = ConstantInt::get(Type::getInt32Ty(*m_pContext), calcFactor.inVertexStride);

    // dwordOffset = relVertexId * vertexStride + attribOffset
    auto pLdsOffset = BinaryOperator::CreateMul(pRelVertexId, pVertexStride, "", pInsertPos);
    pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pAttribOffset, "", pInsertPos);

    return pLdsOffset;
}

// =====================================================================================================================
// Calculates the DWORD offset to read value from LDS based on the specified TCS input info.
Value* PatchInOutImportExport::CalcLdsOffsetForTcsInput(
    Type*        pInputTy,      // [in] Type of the input
    uint32_t     location,      // Base location of the input
    Value*       pLocOffset,    // [in] Relative location offset
    Value*       pCompIdx,      // [in] Index used for vector element indexing (could be null)
    Value*       pVertexIdx,    // [in] Vertex indexing
    Instruction* pInsertPos)    // [in] Where to insert calculation instructions
{
    LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);

    const auto& inOutUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs;
    const auto& calcFactor = inOutUsage.calcFactor;

    // attribOffset = (location + locOffset) * 4 + compIdx
    Value* pAttribOffset = ConstantInt::get(Type::getInt32Ty(*m_pContext), location);

    if (pLocOffset != nullptr)
    {
        pAttribOffset = BinaryOperator::CreateAdd(pAttribOffset, pLocOffset, "", pInsertPos);
    }

    pAttribOffset = BinaryOperator::CreateMul(pAttribOffset,
                                              ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
                                              "",
                                              pInsertPos);

    if (pCompIdx != nullptr)
    {
        const uint32_t bitWidth = pInputTy->getScalarSizeInBits();
        LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32) || (bitWidth == 64));

        if (bitWidth == 64)
        {
            // For 64-bit data type, the component indexing must multiply by 2
            pCompIdx = BinaryOperator::CreateMul(pCompIdx,
                                                 ConstantInt::get(Type::getInt32Ty(*m_pContext), 2),
                                                 "",
                                                 pInsertPos);
        }

        pAttribOffset = BinaryOperator::CreateAdd(pAttribOffset, pCompIdx, "", pInsertPos);
    }

    // dwordOffset = (relativeId * inVertexCount + vertexId) * inVertexStride + attribOffset
    auto inVertexCount = m_pPipelineState->GetInputAssemblyState().patchControlPoints;
    auto pInVertexCount = ConstantInt::get(Type::getInt32Ty(*m_pContext), inVertexCount);
    auto pRelativeId = m_pipelineSysValues.Get(m_pEntryPoint)->GetRelativeId();

    Value* pLdsOffset = BinaryOperator::CreateMul(pRelativeId, pInVertexCount, "", pInsertPos);
    pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pVertexIdx, "", pInsertPos);

    auto pInVertexStride = ConstantInt::get(Type::getInt32Ty(*m_pContext), calcFactor.inVertexStride);
    pLdsOffset = BinaryOperator::CreateMul(pLdsOffset, pInVertexStride, "", pInsertPos);

    pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pAttribOffset, "", pInsertPos);

    return pLdsOffset;
}

// =====================================================================================================================
// Calculates the DWORD offset to read/write value from/to LDS based on the specified TCS output info.
Value* PatchInOutImportExport::CalcLdsOffsetForTcsOutput(
    Type*        pOutputTy,     // [in] Type of the output
    uint32_t     location,      // Base location of the output
    Value*       pLocOffset,    // [in] Relative location offset (could be null)
    Value*       pCompIdx,      // [in] Index used for vector element indexing (could be null)
    Value*       pVertexIdx,    // [in] Vertex indexing
    Instruction* pInsertPos)    // [in] Where to insert calculation instructions
{
    LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);

    const auto& inOutUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs;
    const auto& calcFactor = inOutUsage.calcFactor;

    auto outPatchStart = m_pPipelineState->IsTessOffChip() ? calcFactor.offChip.outPatchStart :
        calcFactor.onChip.outPatchStart;

    auto patchConstStart = m_pPipelineState->IsTessOffChip() ? calcFactor.offChip.patchConstStart :
        calcFactor.onChip.patchConstStart;

    // attribOffset = (location + locOffset) * 4 + compIdx * bitWidth / 32
    Value* pAttibOffset = ConstantInt::get(Type::getInt32Ty(*m_pContext), location);

    if (pLocOffset != nullptr)
    {
        pAttibOffset = BinaryOperator::CreateAdd(pAttibOffset, pLocOffset, "", pInsertPos);
    }

    pAttibOffset = BinaryOperator::CreateMul(pAttibOffset,
                                             ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
                                             "",
                                             pInsertPos);

    if (pCompIdx != nullptr)
    {
        const uint32_t bitWidth = pOutputTy->getScalarSizeInBits();
        LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32) || (bitWidth == 64));

        if (bitWidth == 64)
        {
            // For 64-bit data type, the component indexing must multiply by 2
            pCompIdx = BinaryOperator::CreateMul(pCompIdx,
                                                 ConstantInt::get(Type::getInt32Ty(*m_pContext), 2),
                                                 "",
                                                 pInsertPos);
        }

        pAttibOffset = BinaryOperator::CreateAdd(pAttibOffset, pCompIdx, "", pInsertPos);
    }

    Value* pLdsOffset = nullptr;

    const bool perPatch = (pVertexIdx == nullptr); // Vertex indexing is unavailable for per-patch output
    auto pRelativeId = m_pipelineSysValues.Get(m_pEntryPoint)->GetRelativeId();
    if (perPatch)
    {
        // dwordOffset = patchConstStart + relativeId * patchConstSize + attribOffset
        auto pPatchConstSize = ConstantInt::get(Type::getInt32Ty(*m_pContext), calcFactor.patchConstSize);
        pLdsOffset = BinaryOperator::CreateMul(pRelativeId, pPatchConstSize, "", pInsertPos);

        auto pPatchConstStart = ConstantInt::get(Type::getInt32Ty(*m_pContext), patchConstStart);
        pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pPatchConstStart, "", pInsertPos);

        pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pAttibOffset, "", pInsertPos);
    }
    else
    {
        // dwordOffset = outPatchStart + (relativeId * outVertexCount + vertexId) * outVertexStride + attribOffset
        //             = outPatchStart + relativeId * outPatchSize + vertexId  * outVertexStride + attribOffset
        auto pOutPatchSize = ConstantInt::get(Type::getInt32Ty(*m_pContext), calcFactor.outPatchSize);
        pLdsOffset = BinaryOperator::CreateMul(pRelativeId, pOutPatchSize, "", pInsertPos);

        auto pOutPatchStart = ConstantInt::get(Type::getInt32Ty(*m_pContext), outPatchStart);
        pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pOutPatchStart, "", pInsertPos);

        auto pOutVertexStride = ConstantInt::get(Type::getInt32Ty(*m_pContext), calcFactor.outVertexStride);
        pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                               BinaryOperator::CreateMul(pVertexIdx, pOutVertexStride, "", pInsertPos),
                                               "",
                                               pInsertPos);

        pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pAttibOffset, "", pInsertPos);
    }

    return pLdsOffset;
}

// =====================================================================================================================
// Calculates the DWORD offset to read/write value from/to LDS based on the specified TES input info.
Value* PatchInOutImportExport::CalcLdsOffsetForTesInput(
    Type*        pInputTy,      // [in] Type of the input
    uint32_t     location,      // Base location of the input
    Value*       pLocOffset,    // [in] Relative location offset
    Value*       pCompIdx,      // [in] Index used for vector element indexing (could be null)
    Value*       pVertexIdx,    // [in] Vertex indexing
    Instruction* pInsertPos)    // [in] Where to insert calculation instructions
{
    LLPC_ASSERT(m_shaderStage == ShaderStageTessEval);

    const auto& calcFactor = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;

    auto outPatchStart = m_pPipelineState->IsTessOffChip() ? calcFactor.offChip.outPatchStart :
        calcFactor.onChip.outPatchStart;

    auto patchConstStart = m_pPipelineState->IsTessOffChip() ? calcFactor.offChip.patchConstStart :
        calcFactor.onChip.patchConstStart;

    const auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(m_shaderStage)->entryArgIdxs.tes;

    auto pRelPatchId = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.relPatchId);

    // attribOffset = (location + locOffset) * 4 + compIdx
    Value* pAttibOffset = ConstantInt::get(Type::getInt32Ty(*m_pContext), location);

    if (pLocOffset != nullptr)
    {
        pAttibOffset = BinaryOperator::CreateAdd(pAttibOffset, pLocOffset, "", pInsertPos);
    }

    pAttibOffset = BinaryOperator::CreateMul(pAttibOffset,
                                             ConstantInt::get(Type::getInt32Ty(*m_pContext), 4),
                                             "",
                                             pInsertPos);

    if (pCompIdx != nullptr)
    {
        const uint32_t bitWidth = pInputTy->getScalarSizeInBits();
        LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32) || (bitWidth == 64));

        if (bitWidth == 64)
        {
            // For 64-bit data type, the component indexing must multiply by 2
            pCompIdx = BinaryOperator::CreateMul(pCompIdx,
                                                 ConstantInt::get(Type::getInt32Ty(*m_pContext), 2),
                                                 "",
                                                 pInsertPos);
        }

        pAttibOffset = BinaryOperator::CreateAdd(pAttibOffset, pCompIdx, "", pInsertPos);
    }

    Value* pLdsOffset = nullptr;

    const bool perPatch = (pVertexIdx == nullptr); // Vertex indexing is unavailable for per-patch input
    if (perPatch)
    {
        // dwordOffset = patchConstStart + relPatchId * patchConstSize + attribOffset
        auto pPatchConstSize = ConstantInt::get(Type::getInt32Ty(*m_pContext), calcFactor.patchConstSize);
        pLdsOffset = BinaryOperator::CreateMul(pRelPatchId, pPatchConstSize, "", pInsertPos);

        auto pPatchConstStart = ConstantInt::get(Type::getInt32Ty(*m_pContext), patchConstStart);
        pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pPatchConstStart, "", pInsertPos);

        pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pAttibOffset, "", pInsertPos);
    }
    else
    {
        // dwordOffset = patchStart + (relPatchId * vertexCount + vertexId) * vertexStride + attribOffset
        //             = patchStart + relPatchId * patchSize + vertexId  * vertexStride + attribOffset
        auto pPatchSize = ConstantInt::get(Type::getInt32Ty(*m_pContext), calcFactor.outPatchSize);
        pLdsOffset = BinaryOperator::CreateMul(pRelPatchId, pPatchSize, "", pInsertPos);

        auto pPatchStart = ConstantInt::get(Type::getInt32Ty(*m_pContext), outPatchStart);
        pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pPatchStart, "", pInsertPos);

        auto pVertexStride = ConstantInt::get(Type::getInt32Ty(*m_pContext), calcFactor.outVertexStride);
        pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                               BinaryOperator::CreateMul(pVertexIdx, pVertexStride, "", pInsertPos),
                                               "",
                                               pInsertPos);

        pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pAttibOffset, "", pInsertPos);
    }

    return pLdsOffset;
}

// =====================================================================================================================
// Calculates the patch count for per-thread group.
uint32_t PatchInOutImportExport::CalcPatchCountPerThreadGroup(
    uint32_t inVertexCount,     // Count of vertices of input patch
    uint32_t inVertexStride,    // Vertex stride of input patch in (DWORDs)
    uint32_t outVertexCount,    // Count of vertices of output patch
    uint32_t outVertexStride,   // Vertex stride of output patch in (DWORDs)
    uint32_t patchConstCount,   // Count of output patch constants
    uint32_t tessFactorStride   // Stride of tessellation factors (DWORDs)
    ) const
{
    const uint32_t waveSize = m_pPipelineState->GetShaderWaveSize(m_shaderStage);

    // NOTE: The limit of thread count for tessellation control shader is 4 wavefronts per thread group.
    const uint32_t maxThreadCountPerThreadGroup = (4 * waveSize);
    const uint32_t maxThreadCountPerPatch = std::max(inVertexCount, outVertexCount);
    const uint32_t patchCountLimitedByThread = maxThreadCountPerThreadGroup / maxThreadCountPerPatch;

    const uint32_t inPatchSize = (inVertexCount * inVertexStride);
    const uint32_t outPatchSize = (outVertexCount * outVertexStride);
    const uint32_t patchConstSize = patchConstCount * 4;

    // Compute the required LDS size per patch, always include the space for VS vertex out
    uint32_t ldsSizePerPatch = inPatchSize;
    uint32_t patchCountLimitedByLds = (m_pPipelineState->GetTargetInfo().GetGpuProperty().ldsSizePerThreadGroup /
                                       ldsSizePerPatch);

    uint32_t patchCountPerThreadGroup = std::min(patchCountLimitedByThread, patchCountLimitedByLds);

    // NOTE: Performance analysis shows that 16 patches per thread group is an optimal upper-bound. The value is only
    // an experimental number. For GFX9. 64 is an optimal number instead.
    const uint32_t optimalPatchCountPerThreadGroup = (m_gfxIp.major >= 9) ? 64 : 16;

    patchCountPerThreadGroup = std::min(patchCountPerThreadGroup, optimalPatchCountPerThreadGroup);

    if (m_pPipelineState->IsTessOffChip())
    {
        auto outPatchLdsBufferSize = (outPatchSize + patchConstSize) * 4;
        auto tessOffChipPatchCountPerThreadGroup =
            m_pPipelineState->GetTargetInfo().GetGpuProperty().tessOffChipLdsBufferSize / outPatchLdsBufferSize;
        patchCountPerThreadGroup = std::min(patchCountPerThreadGroup, tessOffChipPatchCountPerThreadGroup);
    }

    // TF-Buffer-based limit for Patchers per Thread Group:
    // ---------------------------------------------------------------------------------------------

    // There is one TF Buffer per shader engine. We can do the below calculation on a per-SE basis.  It is also safe to
    // assume that one thread-group could at most utilize all of the TF Buffer.
    const uint32_t tfBufferSizeInBytes = sizeof(uint32_t) *
                                         m_pPipelineState->GetTargetInfo().GetGpuProperty().tessFactorBufferSizePerSe;
    uint32_t       tfBufferPatchCountLimit = tfBufferSizeInBytes / (tessFactorStride * sizeof(uint32_t));

#if LLPC_BUILD_GFX10
    const auto pWorkarounds = &m_pPipelineState->GetTargetInfo().GetGpuWorkarounds();
    if (pWorkarounds->gfx10.waTessFactorBufferSizeLimitGeUtcl1Underflow)
    {
        tfBufferPatchCountLimit /= 2;
    }
#endif
    patchCountPerThreadGroup = std::min(patchCountPerThreadGroup, tfBufferPatchCountLimit);

    if (m_pPipelineState->IsTessOffChip())
    {
        // For all-offchip tessellation, we need to write an additional 4-byte TCS control word to the TF buffer whenever
        // the patch-ID is zero.
        const uint32_t offChipTfBufferPatchCountLimit =
            (tfBufferSizeInBytes - (patchCountPerThreadGroup * sizeof(uint32_t))) / (tessFactorStride * sizeof(uint32_t));
        patchCountPerThreadGroup = std::min(patchCountPerThreadGroup, offChipTfBufferPatchCountLimit);
    }

    // Adjust the patches-per-thread-group based on hardware workarounds.
    if (m_pPipelineState->GetTargetInfo().GetGpuWorkarounds().gfx6.miscLoadBalancePerWatt != 0)
    {
        const uint32_t waveSize = m_pPipelineState->GetTargetInfo().GetGpuProperty().waveSize;
        // Load balance per watt is a mechanism which monitors HW utilization (num waves active, instructions issued
        // per cycle, etc.) to determine if the HW can handle the workload with fewer CUs enabled.  The SPI_LB_CU_MASK
        // register directs the SPI to stop launching waves to a CU so it will be clock-gated.  There is a bug in the
        // SPI which where that register setting is applied immediately, which causes any pending LS/HS/CS waves on
        // that CU to never be launched.
        //
        // The workaround is to limit each LS/HS threadgroup to a single wavefront: if there's only one wave, then the
        // CU can safely be turned off afterwards.  A microcode fix exists for CS but for GFX it was decided that the
        // cost in power efficiency wasn't worthwhile.
        //
        // Clamping to threads-per-wavefront / max(input control points, threads-per-patch) will make the hardware
        // launch a single LS/HS wave per thread-group.
        // For vulkan, threads-per-patch is always equal with outVertexCount.
        const uint32_t maxThreadCountPerPatch = std::max(inVertexCount, outVertexCount);
        const uint32_t maxPatchCount = waveSize / maxThreadCountPerPatch;

        patchCountPerThreadGroup = std::min(patchCountPerThreadGroup, maxPatchCount);
    }

    return patchCountPerThreadGroup;
}

// =====================================================================================================================
// Inserts "exp" instruction to export generic output.
void PatchInOutImportExport::AddExportInstForGenericOutput(
    Value*       pOutput,        // [in] Output value
    uint32_t     location,       // Location of the output
    uint32_t     compIdx,        // Index used for vector element indexing
    Instruction* pInsertPos)     // [in] Where to insert the "exp" instruction
{
    // Check if the shader stage is valid to use "exp" instruction to export output
    const auto nextStage = m_pPipelineState->GetNextShaderStage(m_shaderStage);
    const bool useExpInst = (((m_shaderStage == ShaderStageVertex) || (m_shaderStage == ShaderStageTessEval) ||
                              (m_shaderStage == ShaderStageCopyShader)) &&
                             ((nextStage == ShaderStageInvalid) || (nextStage == ShaderStageFragment)));
    LLPC_ASSERT(useExpInst);
    LLPC_UNUSED(useExpInst);

    auto pOutputTy = pOutput->getType();

    auto& inOutUsage = m_pPipelineState->GetShaderResourceUsage(m_shaderStage)->inOutUsage;

    const uint32_t compCount = pOutputTy->isVectorTy() ? pOutputTy->getVectorNumElements() : 1;
    const uint32_t bitWidth  = pOutputTy->getScalarSizeInBits();
    LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32) || (bitWidth == 64));

    // Convert the output value to floating-point export value
    Value* pExport = nullptr;
    const uint32_t numChannels = (bitWidth == 64) ? compCount * 2 : compCount;
    uint32_t startChannel = (bitWidth == 64) ? compIdx * 2 : compIdx;
    Type* pExportTy = (numChannels > 1) ? VectorType::get(Type::getFloatTy(*m_pContext), numChannels) :
                                          Type::getFloatTy(*m_pContext);

    if (pOutputTy != pExportTy)
    {
        if (bitWidth == 8)
        {
            // NOTE: For 16-bit output export, we have to cast the 8-bit value to 32-bit floating-point value.
            LLPC_ASSERT(pOutputTy->isIntOrIntVectorTy());
            Type* pZExtTy = Type::getInt32Ty(*m_pContext);
            pZExtTy = pOutputTy->isVectorTy() ? cast<Type>(VectorType::get(pZExtTy, compCount)) : pZExtTy;
            pExport = new ZExtInst(pOutput,
                                   pZExtTy,
                                   "",
                                   pInsertPos);
            pExport = new BitCastInst(pExport, pExportTy, "", pInsertPos);
        }
        else if (bitWidth == 16)
        {
            // NOTE: For 16-bit output export, we have to cast the 16-bit value to 32-bit floating-point value.
            if (pOutputTy->isFPOrFPVectorTy())
            {
                Type* pBitCastTy = Type::getInt16Ty(*m_pContext);
                pBitCastTy = pOutputTy->isVectorTy() ? cast<Type>(VectorType::get(pBitCastTy, compCount)) : pBitCastTy;
                pExport = new BitCastInst(pOutput,
                                          pBitCastTy,
                                          "",
                                          pInsertPos);
            }
            else
            {
                LLPC_ASSERT(pOutputTy->isIntOrIntVectorTy());
                pExport = pOutput;
            }

            Type* pZExtTy = Type::getInt32Ty(*m_pContext);
            pZExtTy = pOutputTy->isVectorTy() ? cast<Type>(VectorType::get(pZExtTy, compCount)) : pZExtTy;
            pExport = new ZExtInst(pExport,
                                   pZExtTy,
                                   "",
                                   pInsertPos);
            pExport = new BitCastInst(pExport, pExportTy, "", pInsertPos);
        }
        else
        {
            LLPC_ASSERT(CanBitCast(pOutputTy, pExportTy));
            pExport = new BitCastInst(pOutput, pExportTy, "", pInsertPos);
        }
    }
    else
    {
        pExport = pOutput;
    }

    LLPC_ASSERT(numChannels <= 8);
    Value* exportValues[8] = { nullptr };

    if (numChannels == 1)
    {
        exportValues[0] = pExport;
    }
    else
    {
        for (uint32_t i = 0; i < numChannels; ++i)
        {
            exportValues[i] = ExtractElementInst::Create(pExport,
                                                         ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                         "",
                                                         pInsertPos);
        }
    }

    std::vector<Value*> args;

    if (numChannels <= 4)
    {
        LLPC_ASSERT(startChannel + numChannels <= 4);
        const uint32_t channelMask = ((1 << (startChannel + numChannels)) - 1) - ((1 << startChannel) - 1);

        args.clear();
        args.push_back(ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_PARAM_0 + location)); // tgt
        args.push_back(ConstantInt::get(Type::getInt32Ty(*m_pContext), channelMask));                   // en

        // src0 ~ src3
        for (uint32_t i = 0; i < startChannel; ++i)
        {
            // Inactive components (dummy)
            args.push_back(UndefValue::get(Type::getFloatTy(*m_pContext)));
        }

        for (uint32_t i = startChannel; i < startChannel + numChannels; ++i)
        {
            args.push_back(exportValues[i - startChannel]);
        }

        for (uint32_t i = startChannel + numChannels; i < 4; ++i)
        {
            // Inactive components (dummy)
            args.push_back(UndefValue::get(Type::getFloatTy(*m_pContext)));
        }

        args.push_back(ConstantInt::get(Type::getInt1Ty(*m_pContext), false));  // done
        args.push_back(ConstantInt::get(Type::getInt1Ty(*m_pContext), false));  // vm

        EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
        ++inOutUsage.expCount;
    }
    else
    {
        // We have to do exporting twice for this output
        LLPC_ASSERT(startChannel == 0); // Other values are disallowed according to GLSL spec
        LLPC_ASSERT((numChannels == 6) || (numChannels == 8));

        // Do the first exporting
        args.clear();
        args.push_back(ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_PARAM_0 + location)); // tgt
        args.push_back(ConstantInt::get(Type::getInt32Ty(*m_pContext), 0xF));                           // en

        // src0 ~ src3
        for (uint32_t i = 0; i < 4; ++i)
        {
            args.push_back(exportValues[i]);
        }

        args.push_back(ConstantInt::get(Type::getInt1Ty(*m_pContext), false));  // done
        args.push_back(ConstantInt::get(Type::getInt1Ty(*m_pContext), false));  // vm

        EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
        ++inOutUsage.expCount;

        // Do the second exporting
        const uint32_t channelMask = ((1 << (numChannels - 4)) - 1);

        args.clear();
        args.push_back(ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_PARAM_0 + location + 1)); // tgt
        args.push_back(ConstantInt::get(Type::getInt32Ty(*m_pContext), channelMask));                       // en

        // src0 ~ src3
        for (uint32_t i = 4; i < numChannels; ++i)
        {
            args.push_back(exportValues[i]);
        }

        for (uint32_t i = numChannels; i < 8; ++i)
        {
            // Inactive components (dummy)
            args.push_back(UndefValue::get(Type::getFloatTy(*m_pContext)));
        }

        args.push_back(ConstantInt::get(Type::getInt1Ty(*m_pContext), false)); // done
        args.push_back(ConstantInt::get(Type::getInt1Ty(*m_pContext), false)); // vm

        EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
        ++inOutUsage.expCount;
    }
}

// =====================================================================================================================
// Inserts "exp" instruction to export built-in output.
void PatchInOutImportExport::AddExportInstForBuiltInOutput(
    Value*       pOutput,       // [in] Output value
    uint32_t     builtInId,     // ID of the built-in variable
    Instruction* pInsertPos)    // [in] Where to insert the "exp" instruction
{
    // Check if the shader stage is valid to use "exp" instruction to export output
    const auto nextStage = m_pPipelineState->GetNextShaderStage(m_shaderStage);
    const bool useExpInst = (((m_shaderStage == ShaderStageVertex) || (m_shaderStage == ShaderStageTessEval) ||
                              (m_shaderStage == ShaderStageCopyShader)) &&
                             ((nextStage == ShaderStageInvalid) || (nextStage == ShaderStageFragment)));
    LLPC_ASSERT(useExpInst);
    LLPC_UNUSED(useExpInst);

    auto& inOutUsage = m_pPipelineState->GetShaderResourceUsage(m_shaderStage)->inOutUsage;

    const auto pUndef = UndefValue::get(Type::getFloatTy(*m_pContext));

    switch (builtInId)
    {
    case BuiltInPosition:
        {
            Value* args[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_POS_0),  // tgt
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0xF),               // en
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false),              // done
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false)               // vm
            };

            // src0 ~ src3
            for (uint32_t i = 0; i < 4; ++i)
            {
                auto pCompValue = ExtractElementInst::Create(pOutput,
                                                             ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                             "",
                                                             pInsertPos);
                args[2 + i] = pCompValue;
            }

            // "Done" flag is valid for exporting position 0 ~ 3
            m_pLastExport =
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
            break;
        }
    case BuiltInPointSize:
        {
            Value* args[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_POS_1),  // tgt
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0x1),               // en
                pOutput,                                                            // src0
                pUndef,                                                             // src1
                pUndef,                                                             // src2
                pUndef,                                                             // src3
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false),              // done
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false)               // vm
            };
            // "Done" flag is valid for exporting position 0 ~ 3
            m_pLastExport =
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
            break;
        }
    case BuiltInLayer:
        {
            LLPC_ASSERT(m_gfxIp.major <= 8); // For GFX9, gl_ViewportIndex and gl_Layer are packed

            const auto enableMultiView = m_pPipelineState->GetInputAssemblyState().enableMultiView;

            Value* pLayer = new BitCastInst(pOutput, Type::getFloatTy(*m_pContext), "", pInsertPos);

            Value* args[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_POS_1),  // tgt
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0x4),               // en
                pUndef,                                                             // src0
                pUndef,                                                             // src1
                pLayer,                                                             // src2
                pUndef,                                                             // src3
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false),              // done
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false)               // vm
            };
            // "Done" flag is valid for exporting position 0 ~ 3
            m_pLastExport =
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);

            // NOTE: We have to export gl_Layer via generic outputs as well.
            bool hasLayerExport = true;
            if (nextStage == ShaderStageFragment)
            {
                const auto& nextBuiltInUsage =
                    m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;

                hasLayerExport = nextBuiltInUsage.layer || nextBuiltInUsage.viewIndex;
            }

            if (hasLayerExport)
            {
                uint32_t loc = InvalidValue;
                if (m_shaderStage == ShaderStageCopyShader)
                {
                    LLPC_ASSERT(inOutUsage.gs.builtInOutLocs.find(BuiltInLayer) !=
                                inOutUsage.gs.builtInOutLocs.end() ||
                                inOutUsage.gs.builtInOutLocs.find(BuiltInViewIndex) !=
                                inOutUsage.gs.builtInOutLocs.end());
                    loc = enableMultiView ? inOutUsage.gs.builtInOutLocs[BuiltInViewIndex]:
                        inOutUsage.gs.builtInOutLocs[BuiltInLayer];
                }
                else
                {
                    LLPC_ASSERT(inOutUsage.builtInOutputLocMap.find(BuiltInLayer) !=
                                inOutUsage.builtInOutputLocMap.end() ||
                                inOutUsage.builtInOutputLocMap.find(BuiltInViewIndex) !=
                                inOutUsage.builtInOutputLocMap.end());

                    loc = enableMultiView ? inOutUsage.builtInOutputLocMap[BuiltInViewIndex] :
                        inOutUsage.builtInOutputLocMap[BuiltInLayer];
                }

                Value* args[] = {
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_PARAM_0 + loc),  // tgt
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), 0xF),                       // en
                    pLayer,                                                             // src0
                    pUndef,                                                             // src1
                    pUndef,                                                             // src2
                    pUndef,                                                             // src3
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false),                      // done
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false)                       // vm
                };
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
                ++inOutUsage.expCount;
            }

            break;
        }
    case BuiltInViewportIndex:
        {
            LLPC_ASSERT(m_gfxIp.major <= 8); // For GFX9, gl_ViewportIndex and gl_Layer are packed
            Value* pViewportIndex = new BitCastInst(pOutput, Type::getFloatTy(*m_pContext), "", pInsertPos);

            Value* args[] = {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_POS_1),  // tgt
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0x8),               // en
                pUndef,                                                             // src0
                pUndef,                                                             // src1
                pUndef,                                                             // src2
                pViewportIndex,                                                     // src3
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false),              // done
                ConstantInt::get(Type::getInt1Ty(*m_pContext), false)               // vm
            };
            // "Done" flag is valid for exporting position 0 ~ 3
            m_pLastExport =
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);

            // NOTE: We have to export gl_ViewportIndex via generic outputs as well.
            bool hasViewportIndexExport = true;
            if (nextStage == ShaderStageFragment)
            {
                const auto& nextBuiltInUsage =
                    m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;

                hasViewportIndexExport = nextBuiltInUsage.viewportIndex;
            }

            if (hasViewportIndexExport)
            {
                uint32_t loc = InvalidValue;
                if (m_shaderStage == ShaderStageCopyShader)
                {
                    LLPC_ASSERT(inOutUsage.gs.builtInOutLocs.find(BuiltInViewportIndex) !=
                                inOutUsage.gs.builtInOutLocs.end());
                    loc = inOutUsage.gs.builtInOutLocs[BuiltInViewportIndex];
                }
                else
                {
                    LLPC_ASSERT(inOutUsage.builtInOutputLocMap.find(BuiltInViewportIndex) !=
                                inOutUsage.builtInOutputLocMap.end());
                    loc = inOutUsage.builtInOutputLocMap[BuiltInViewportIndex];
                }

                Value* args[] = {
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_PARAM_0 + loc),  // tgt
                    ConstantInt::get(Type::getInt32Ty(*m_pContext), 0xF),                       // en
                    pViewportIndex,                                                             // src0
                    pUndef,                                                                     // src1
                    pUndef,                                                                     // src2
                    pUndef,                                                                     // src3
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false),                      // done
                    ConstantInt::get(Type::getInt1Ty(*m_pContext), false)                       // vm
                };
                EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
                ++inOutUsage.expCount;
            }

            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }
}

// =====================================================================================================================
// Adjusts I/J calculation for "centroid" interpolation mode by taking "center" mode into account.
Value* PatchInOutImportExport::AdjustCentroidIJ(
    Value*       pCentroidIJ,   // [in] Centroid I/J provided by hardware natively
    Value*       pCenterIJ,     // [in] Center I/J provided by hardware natively
    Instruction* pInsertPos)    // [in] Where to insert this call
{
    auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageFragment)->entryArgIdxs.fs;
    auto pPrimMask = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.primMask);
    auto& builtInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
    Value* pIJ = nullptr;

    if (builtInUsage.centroid && builtInUsage.center)
    {
        // NOTE: If both centroid and center are enabled, centroid I/J provided by hardware natively may be invalid. We have to
        // adjust it with center I/J on condition of bc_optimize flag.
        // bc_optimize = pPrimMask[31], when bc_optimize is on, pPrimMask is less than zero
        auto pCond = new ICmpInst(pInsertPos,
                                 ICmpInst::ICMP_SLT,
                                 pPrimMask,
                                 ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                 "");
        pIJ = SelectInst::Create(pCond, pCenterIJ, pCentroidIJ, "", pInsertPos);
    }
    else
    {
        pIJ = pCentroidIJ;
    }

    return pIJ;
}

// =====================================================================================================================
// Get Subgroup local invocation Id
Value* PatchInOutImportExport::GetSubgroupLocalInvocationId(
    Instruction* pInsertPos)  // [in] Where to insert this call
{
    Value* args[] = {
        ConstantInt::get(Type::getInt32Ty(*m_pContext), -1),
        ConstantInt::get(Type::getInt32Ty(*m_pContext), 0)
    };
    Value* pSubgroupLocalInvocationId = EmitCall("llvm.amdgcn.mbcnt.lo",
                                                 Type::getInt32Ty(*m_pContext),
                                                 args,
                                                 NoAttrib,
                                                 &*pInsertPos);

#if LLPC_BUILD_GFX10
    uint32_t waveSize = m_pPipelineState->GetShaderWaveSize(m_shaderStage);
    if (waveSize == 64)
#endif
    {
        Value* args[] = {
            ConstantInt::get(Type::getInt32Ty(*m_pContext), -1),
            pSubgroupLocalInvocationId
        };
        pSubgroupLocalInvocationId = EmitCall("llvm.amdgcn.mbcnt.hi",
                                              Type::getInt32Ty(*m_pContext),
                                              args,
                                              NoAttrib,
                                              &*pInsertPos);
    }

    return pSubgroupLocalInvocationId;
}

// =====================================================================================================================
// Do automatic workgroup size reconfiguration in a compute shader, to allow ReconfigWorkgroup
// to apply optimizations.
WorkgroupLayout PatchInOutImportExport::CalculateWorkgroupLayout()
{
    auto& resUsage = *m_pPipelineState->GetShaderResourceUsage(ShaderStageCompute);
    if (m_shaderStage == ShaderStageCompute)
    {
        bool reconfig = false;

        switch (static_cast<WorkgroupLayout>(resUsage.builtInUsage.cs.workgroupLayout))
        {
        case WorkgroupLayout::Unknown:
            // If no configuration has been specified, apply a reconfigure if the compute shader uses images and the
            // pipeline option was enabled.
            if (resUsage.useImages)
            {
                reconfig = m_pPipelineState->GetOptions().reconfigWorkgroupLayout;
            }
            break;
        case WorkgroupLayout::Linear:
            // The hardware by default applies the linear rules, so just ban reconfigure and we're done.
            reconfig = false;
            break;
        case WorkgroupLayout::Quads:
            // 2x2 requested.
            reconfig = true;
            break;
        case WorkgroupLayout::SexagintiQuads:
            // 8x8 requested.
            reconfig = true;
            break;
        }

        if (reconfig)
        {
            auto& mode = m_pPipelineState->GetShaderModes()->GetComputeShaderMode();
            if (((mode.workgroupSizeX % 2) == 0) && ((mode.workgroupSizeY % 2) == 0))
            {
                if (((mode.workgroupSizeX > 8) && (mode.workgroupSizeY >= 8)) ||
                    ((mode.workgroupSizeX >= 8) && (mode.workgroupSizeY > 8)))
                {
                    // If our local size in the X & Y dimensions are greater than 8, we can reconfigure.
                    resUsage.builtInUsage.cs.workgroupLayout = static_cast<uint32_t>(WorkgroupLayout::SexagintiQuads);
                }
                else
                {
                    // If our local size in the X & Y dimensions are multiples of 2, we can reconfigure.
                    resUsage.builtInUsage.cs.workgroupLayout = static_cast<uint32_t>(WorkgroupLayout::Quads);
                }
            }
        }
    }
    return static_cast<WorkgroupLayout>(resUsage.builtInUsage.cs.workgroupLayout);
}

// =====================================================================================================================
// Reconfigure the workgroup for optimization purposes.
Value* PatchInOutImportExport::ReconfigWorkgroup(
    Value*       pLocalInvocationId, // [in] The original workgroup ID.
    Instruction* pInsertPos)         // [in] Where to insert instructions.
{
    auto& builtInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageCompute)->builtInUsage.cs;
    auto workgroupLayout = static_cast<WorkgroupLayout>(builtInUsage.workgroupLayout);
    auto& mode = m_pPipelineState->GetShaderModes()->GetComputeShaderMode();

    // NOTE: Here, we implement "GDC 2018 Engine Optimization Hot Lap Workgroup Optimization " (slides 40-45, by
    // Timothy Lottes).
    // uvec2 Remap(uint a) {
    //   uint y = bitfieldExtract(a,3,4); // v_bfe_u32 ---> {...0,y3,y2,y1,x2}
    //   y = bitfieldInsert(y,a,0,1);     // v_bfi_b32 ---> {...0,y3,y2,y1,y0}
    //   uint x = bitfieldExtract(a,1,3); // v_bfe_u32 ---> {...0,x2,x1,x0}
    //   a = bitfieldExtract(a,4,5);      // v_bfe_u32 ---> {...0,x4,x3,y3,y2,y1}
    //   x = bitfieldInsert(a,x,0,3);     // v_bfi_b32 ---> {...0,x4,x3,x2,x1,x0}
    //   return uvec2(x, y);
    // }
    // usage in shader
    //   uvec2 xy = Remap(gl_LocalInvocationID.x);
    //   xy.x += gl_WorkGroupID.x << 5; // v_lshl_add_u32
    //   xy.y += gl_WorkGroupID.y << 4; // v_lshl_add_u32

    Type* const pInt16Ty = Type::getInt16Ty(*m_pContext);
    Type* const pInt32Ty = Type::getInt32Ty(*m_pContext);

    Value* pRemappedId = pLocalInvocationId;

    // For a reconfigured workgroup, we map Y -> Z
    if (mode.workgroupSizeZ > 1)
    {
        Constant* shuffleMask[] = {
            ConstantInt::get(pInt32Ty, 0),
            UndefValue::get(pInt32Ty),
            ConstantInt::get(pInt32Ty, 1)
        };

        pRemappedId = new ShuffleVectorInst(pRemappedId,
                                            UndefValue::get(pRemappedId->getType()),
                                            ConstantVector::get(shuffleMask),
                                            "",
                                            pInsertPos);
    }
    else
    {
        pRemappedId = InsertElementInst::Create(pRemappedId,
                                                ConstantInt::get(pInt32Ty, 0),
                                                ConstantInt::get(pInt32Ty, 2),
                                                "",
                                                pInsertPos);
    }

    Instruction* const pX = ExtractElementInst::Create(pRemappedId,
                                                       ConstantInt::get(pInt32Ty, 0),
                                                       "",
                                                       pInsertPos);

    Instruction* const pBit0 = BinaryOperator::CreateAnd(pX,
                                                         ConstantInt::get(pInt32Ty, 0x1),
                                                         "",
                                                         pInsertPos);

    Instruction* pBit1 = BinaryOperator::CreateAnd(pX,
                                                   ConstantInt::get(pInt32Ty, 0x2),
                                                   "",
                                                   pInsertPos);
    pBit1 = BinaryOperator::CreateLShr(pBit1,
                                       ConstantInt::get(pInt32Ty, 1),
                                       "",
                                       pInsertPos);

    Instruction* pOffset = nullptr;
    Instruction* pMaskedX = pX;

    // Check if we are doing 8x8, as we need to calculate an offset and mask out the top bits of X if so.
    if (workgroupLayout == WorkgroupLayout::SexagintiQuads)
    {
        const uint32_t workgroupSizeYMul8 = mode.workgroupSizeY * 8;
        Constant* const pWorkgroupSizeYMul8 = ConstantInt::get(pInt32Ty, workgroupSizeYMul8);

        if (IsPowerOfTwo(workgroupSizeYMul8))
        {
            // If we have a power of two, we can use a right shift to compute the division more efficiently.
            pOffset = BinaryOperator::CreateLShr(pX,
                                                 ConstantInt::get(pInt32Ty, Log2(workgroupSizeYMul8)),
                                                 "",
                                                 pInsertPos);
        }
        else
        {
            // Otherwise we truncate down to a 16-bit integer, do the division, and zero extend. This will
            // result in significantly less instructions to do the divide.
            pOffset = CastInst::CreateIntegerCast(pX,
                                                  pInt16Ty,
                                                  false,
                                                  "",
                                                  pInsertPos);

            pOffset = BinaryOperator::CreateUDiv(pOffset,
                                                 ConstantInt::get(pInt16Ty, workgroupSizeYMul8),
                                                 "",
                                                 pInsertPos);

            pOffset = CastInst::CreateIntegerCast(pOffset,
                                                  pInt32Ty,
                                                  false,
                                                  "",
                                                  pInsertPos);
        }

        Instruction* const pMulOffset = BinaryOperator::CreateMul(pOffset,
                                                                  pWorkgroupSizeYMul8,
                                                                  "",
                                                                  pInsertPos);

        pMaskedX = BinaryOperator::CreateSub(pX,
                                             pMulOffset,
                                             "",
                                             pInsertPos);
    }

    Instruction* const pRemainingBits = BinaryOperator::CreateAnd(pMaskedX,
                                                                  ConstantInt::get(pInt32Ty, ~0x3),
                                                                  "",
                                                                  pInsertPos);

    Instruction* pDiv = nullptr;
    Instruction* pRem = nullptr;

    if (pOffset != nullptr)
    {
        if (((mode.workgroupSizeX % 8) == 0) && ((mode.workgroupSizeY % 8) == 0))
        {
            // Divide by 16.
            pDiv = BinaryOperator::CreateLShr(pRemainingBits,
                                              ConstantInt::get(pInt32Ty, 4),
                                              "",
                                              pInsertPos);

            // Multiply by 16.
            pRem = BinaryOperator::CreateShl(pDiv,
                                             ConstantInt::get(pInt32Ty, 4),
                                             "",
                                             pInsertPos);

            // Subtract to get remainder.
            pRem = BinaryOperator::CreateSub(pRemainingBits,
                                             pRem,
                                             "",
                                             pInsertPos);
        }
        else
        {
            // Multiply by 8.
            Instruction* pDivideBy = BinaryOperator::CreateShl(pOffset,
                                                               ConstantInt::get(pInt32Ty, 3),
                                                               "",
                                                               pInsertPos);

            pDivideBy = BinaryOperator::CreateSub(ConstantInt::get(pInt32Ty, mode.workgroupSizeX),
                                                  pDivideBy,
                                                  "",
                                                  pInsertPos);

            Instruction* const pCond = new ICmpInst(pInsertPos,
                                                    ICmpInst::ICMP_ULT,
                                                    pDivideBy,
                                                    ConstantInt::get(pInt32Ty, 8),
                                                    "");

            // We do a minimum operation to ensure that we never divide by more than 8, which forces our
            // workgroup layout into 8x8 tiles.
            pDivideBy = SelectInst::Create(pCond,
                                           pDivideBy,
                                           ConstantInt::get(pInt32Ty, 8),
                                           "",
                                           pInsertPos);

            // Multiply by 2.
            pDivideBy = BinaryOperator::CreateShl(pDivideBy,
                                                  ConstantInt::get(pInt32Ty, 1),
                                                  "",
                                                  pInsertPos);

            Instruction* const pDivideByTrunc = CastInst::CreateIntegerCast(pDivideBy,
                                                                            pInt16Ty,
                                                                            false,
                                                                            "",
                                                                            pInsertPos);

            // Truncate down to a 16-bit integer, do the division, and zero extend.
            pDiv = CastInst::CreateIntegerCast(pMaskedX,
                                               pInt16Ty,
                                               false,
                                               "",
                                               pInsertPos);

            pDiv = BinaryOperator::CreateUDiv(pDiv,
                                              pDivideByTrunc,
                                              "",
                                              pInsertPos);

            pDiv = CastInst::CreateIntegerCast(pDiv,
                                               pInt32Ty,
                                               false,
                                               "",
                                               pInsertPos);

            Instruction* const pMulDiv = BinaryOperator::CreateMul(pDiv,
                                                                   pDivideBy,
                                                                   "",
                                                                   pInsertPos);

            pRem = BinaryOperator::CreateSub(pRemainingBits,
                                             pMulDiv,
                                             "",
                                             pInsertPos);
        }
    }
    else
    {
        const uint32_t workgroupSizeXMul2 = mode.workgroupSizeX * 2;
        Constant* const pWorkgroupSizeXMul2 = ConstantInt::get(pInt32Ty, workgroupSizeXMul2);

        if (IsPowerOfTwo(workgroupSizeXMul2))
        {
            // If we have a power of two, we can use a right shift to compute the division more efficiently.
            pDiv = BinaryOperator::CreateLShr(pMaskedX,
                                              ConstantInt::get(pInt32Ty, Log2(workgroupSizeXMul2)),
                                              "",
                                              pInsertPos);
        }
        else
        {
            // Otherwise we truncate down to a 16-bit integer, do the division, and zero extend. This will
            // result in significantly less instructions to do the divide.
            pDiv = CastInst::CreateIntegerCast(pMaskedX,
                                               pInt16Ty,
                                               false,
                                               "",
                                               pInsertPos);

            pDiv = BinaryOperator::CreateUDiv(pDiv,
                                              ConstantInt::get(pInt16Ty, workgroupSizeXMul2),
                                              "",
                                              pInsertPos);

            pDiv = CastInst::CreateIntegerCast(pDiv,
                                               pInt32Ty,
                                               false,
                                               "",
                                               pInsertPos);
        }

        Instruction* const pMulDiv = BinaryOperator::CreateMul(pDiv,
                                                               pWorkgroupSizeXMul2,
                                                               "",
                                                               pInsertPos);

        pRem = BinaryOperator::CreateSub(pRemainingBits,
                                         pMulDiv,
                                         "",
                                         pInsertPos);
    }

    // Now we have all the components to reconstruct X & Y!
    Instruction* pNewX = BinaryOperator::CreateLShr(pRem,
                                                    ConstantInt::get(pInt32Ty, 1),
                                                    "",
                                                    pInsertPos);

    pNewX = BinaryOperator::CreateAdd(pNewX, pBit0, "", pInsertPos);

    // If we have an offset, we need to incorporate this into X.
    if (pOffset != nullptr)
    {
        const uint32_t workgroupSizeYMin8 = std::min(mode.workgroupSizeY, 8u);
        Constant* const pWorkgroupSizeYMin8 = ConstantInt::get(pInt32Ty, workgroupSizeYMin8);
        Instruction* const pMul = BinaryOperator::CreateMul(pOffset,
                                                            pWorkgroupSizeYMin8,
                                                            "",
                                                            pInsertPos);

        pNewX = BinaryOperator::CreateAdd(pNewX, pMul, "", pInsertPos);
    }

    pRemappedId = InsertElementInst::Create(pRemappedId,
                                            pNewX,
                                            ConstantInt::get(pInt32Ty, 0),
                                            "",
                                            pInsertPos);

    Instruction* pNewY = BinaryOperator::CreateShl(pDiv,
                                                   ConstantInt::get(pInt32Ty, 1),
                                                   "",
                                                   pInsertPos);

    pNewY = BinaryOperator::CreateAdd(pNewY, pBit1, "", pInsertPos);

    pRemappedId = InsertElementInst::Create(pRemappedId,
                                            pNewY,
                                            ConstantInt::get(pInt32Ty, 1),
                                            "",
                                            pInsertPos);

    return pRemappedId;
}

// =====================================================================================================================
// Get the value of compute shader built-in WorkgroupSize
Value* PatchInOutImportExport::GetWorkgroupSize()
{
    LLPC_ASSERT(m_shaderStage == ShaderStageCompute);

    auto& builtInUsage = m_pPipelineState->GetShaderModes()->GetComputeShaderMode();
    auto pWorkgroupSizeX = ConstantInt::get(Type::getInt32Ty(*m_pContext), builtInUsage.workgroupSizeX);
    auto pWorkgroupSizeY = ConstantInt::get(Type::getInt32Ty(*m_pContext), builtInUsage.workgroupSizeY);
    auto pWorkgroupSizeZ = ConstantInt::get(Type::getInt32Ty(*m_pContext), builtInUsage.workgroupSizeZ);

    return ConstantVector::get({ pWorkgroupSizeX, pWorkgroupSizeY, pWorkgroupSizeZ });
}

// =====================================================================================================================
// Get the value of compute shader built-in LocalInvocationId
Value* PatchInOutImportExport::GetInLocalInvocationId(
    Instruction* pInsertPos) // [in] Where to insert instructions.
{
    LLPC_ASSERT(m_shaderStage == ShaderStageCompute);

    auto& builtInUsage = m_pPipelineState->GetShaderModes()->GetComputeShaderMode();
    auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageCompute)->entryArgIdxs.cs;
    Value* pLocaInvocatioId = GetFunctionArgument(m_pEntryPoint, entryArgIdxs.localInvocationId);

    WorkgroupLayout workgroupLayout = CalculateWorkgroupLayout();

    // If we do not need to configure our workgroup in linear layout and the layout info is not specified, we
    // do the reconfiguration for this workgroup.
    if ((workgroupLayout != WorkgroupLayout::Unknown) &&
        (workgroupLayout != WorkgroupLayout::Linear))
    {
        pLocaInvocatioId = ReconfigWorkgroup(pLocaInvocatioId, pInsertPos);
    }
    else
    {
        if (builtInUsage.workgroupSizeZ > 1)
        {
            // XYZ, do nothing
        }
        else if (builtInUsage.workgroupSizeY > 1)
        {
            // XY
            pLocaInvocatioId = InsertElementInst::Create(pLocaInvocatioId,
                                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 2),
                                                        "",
                                                        pInsertPos);
        }
        else
        {
            // X
            pLocaInvocatioId = InsertElementInst::Create(pLocaInvocatioId,
                                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                        "",
                                                        pInsertPos);

            pLocaInvocatioId = InsertElementInst::Create(pLocaInvocatioId,
                                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 2),
                                                        "",
                                                        pInsertPos);
        }
    }
    return pLocaInvocatioId;
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM patching opertions for input import and output export.
INITIALIZE_PASS(PatchInOutImportExport, DEBUG_TYPE,
                "Patch LLVM for input import and output export operations", false, false)
