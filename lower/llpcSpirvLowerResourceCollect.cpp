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
 * @file  llpcSpirvLowerResourceCollect.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerResourceCollect.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-spirv-lower-resource-collect"

#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <queue>
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcPipelineShaders.h"
#include "llpcSpirvLowerResourceCollect.h"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char SpirvLowerResourceCollect::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering opertions for resource collecting
ModulePass* CreateSpirvLowerResourceCollect()
{
    return new SpirvLowerResourceCollect();
}

// =====================================================================================================================
SpirvLowerResourceCollect::SpirvLowerResourceCollect()
    :
    SpirvLower(ID)
{
    initializeCallGraphWrapperPassPass(*PassRegistry::getPassRegistry());
    initializePipelineShadersPass(*PassRegistry::getPassRegistry());
    initializeSpirvLowerResourceCollectPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerResourceCollect::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Resource-Collect\n");

    SpirvLower::Init(&module);
    m_pPipelineShaders = &getAnalysis<PipelineShaders>();
    SetFunctionShaderUse();

    for (uint32_t stage = 0; stage < ShaderStageCount; ++stage)
    {
        auto shaderStage = ShaderStage(stage);
        if (m_pPipelineShaders->GetEntryPoint(shaderStage) != nullptr)
        {
            CollectExecutionModeUsage(shaderStage);
        }
    }

    const uint32_t stageMask = m_pContext->GetShaderStageMask();

    if ((stageMask & ShaderStageToMask(ShaderStageVertex)) != 0)
    {
        // Collect resource usages from vertex input create info
        auto pVsResUsage = m_pContext->GetShaderResourceUsage(ShaderStageVertex);
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
                    pVsResUsage->builtInUsage.vs.vertexIndex = true;
                    pVsResUsage->builtInUsage.vs.baseVertex = true;
                }
                else
                {
                    LLPC_ASSERT(pBinding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE);
                    pVsResUsage->builtInUsage.vs.instanceIndex = true;
                    pVsResUsage->builtInUsage.vs.baseInstance = true;
                }
            }
        }
    }

    if ((stageMask & ShaderStageToMask(ShaderStageFragment)) != 0)
    {
        for (auto& func : *m_pModule)
        {
            if (func.getName() == "_Z4Killv")
            {
                auto pFsResUsage = m_pContext->GetShaderResourceUsage(ShaderStageFragment);
                pFsResUsage->builtInUsage.fs.discard = true;
            }
        }
    }

    // Collect unused globals and remove them
    std::unordered_set<GlobalVariable*> removedGlobals;
    for (auto pGlobal = m_pModule->global_begin(), pEnd = m_pModule->global_end(); pGlobal != pEnd; ++pGlobal)
    {
        if (pGlobal->user_empty())
        {
            Value* pInitializer = nullptr;
            if (pGlobal->hasInitializer())
            {
                pInitializer = pGlobal->getInitializer();
            }

            if ((pInitializer == nullptr) || isa<UndefValue>(pInitializer))
            {
                removedGlobals.insert(&*pGlobal);
            }
        }
    }

    for (auto pGlobal : removedGlobals)
    {
        pGlobal->dropAllReferences();
        pGlobal->eraseFromParent();
    }

    bool useViewIndex = false;

    // Collect resource usages from globals.
    for (auto pGlobal = m_pModule->global_begin(), pEnd = m_pModule->global_end(); pGlobal != pEnd; ++pGlobal)
    {
        // Currently, there can never be more than one shader using a global, because each shader comes from
        // a separate SPIR-V module. The code here allows for a global to be used in more than one shader,
        // which could happen if we do an optimization in the future where the same SPIR-V module containing
        // more than one shader stage is translated only once. For now, we assert that the global is used
        // in only one shader stage.
        auto shaderUseMask = GetGlobalShaderUse(&*pGlobal);
        LLPC_ASSERT(!shaderUseMask || isPowerOf2_32(shaderUseMask));

        const Type* pGlobalTy = pGlobal->getType()->getContainedType(0);

        auto addrSpace = pGlobal->getType()->getAddressSpace();
        switch (addrSpace)
        {
        case SPIRAS_Constant:
            {
                MDNode* pMetaNode = pGlobal->getMetadata(gSPIRVMD::Resource);

                auto descSet = mdconst::dyn_extract<ConstantInt>(pMetaNode->getOperand(0))->getZExtValue();
                auto binding = mdconst::dyn_extract<ConstantInt>(pMetaNode->getOperand(1))->getZExtValue();

                // TODO: Will support separated texture resource/sampler.
                DescriptorType descType = DescriptorType::Texture;

                // NOTE: For texture buffer and image buffer, the descriptor type should be set to "TexelBuffer".
                if (pGlobalTy->isPointerTy())
                {
                    Type* pImageType = pGlobalTy->getPointerElementType();
                    std::string imageTypeName = pImageType->getStructName();
                    // Format of image opaque type: ...[.SampledImage.<date type><dim>]...
                    if (imageTypeName.find(".SampledImage") != std::string::npos)
                    {
                        auto pos = imageTypeName.find("_");
                        LLPC_ASSERT(pos != std::string::npos);

                        ++pos;
                        Dim dim = static_cast<Dim>(imageTypeName[pos] - '0');
                        if (dim == DimBuffer)
                        {
                            descType = DescriptorType::TexelBuffer;
                        }
                        else if (dim == DimSubpassData)
                        {
                            LLPC_ASSERT(shaderUseMask == 1U << ShaderStageFragment);
                            auto pFsResUsage = m_pContext->GetShaderResourceUsage(ShaderStageFragment);
                            pFsResUsage->builtInUsage.fs.fragCoord = true;
                            useViewIndex = true;
                        }
                    }
                }

                DescriptorBinding bindingInfo = {};
                bindingInfo.descType  = descType;
                bindingInfo.arraySize = GetFlattenArrayElementCount(pGlobalTy);

                for (uint32_t stage = 0; stage < ShaderStageCount; ++stage)
                {
                    auto shaderStage = ShaderStage(stage);
                    if ((shaderUseMask >> shaderStage) & 1)
                    {
                        CollectDescriptorUsage(shaderStage, descSet, binding, &bindingInfo);
                    }
                }
                break;
            }
        case SPIRAS_Private:
        case SPIRAS_Global:
        case SPIRAS_Local:
            {
                // TODO: Will be implemented.
                break;
            }
        case SPIRAS_Input:
        case SPIRAS_Output:
            {
                MDNode* pMetaNode = pGlobal->getMetadata(gSPIRVMD::InOut);

                for (uint32_t stage = 0; stage < ShaderStageCount; ++stage)
                {
                    auto shaderStage = ShaderStage(stage);
                    if ((shaderUseMask >> shaderStage) & 1)
                    {
                        auto pMeta = mdconst::dyn_extract<Constant>(pMetaNode->getOperand(0));

                        auto pInOutTy = pGlobalTy;
                        if (pInOutTy->isArrayTy())
                        {
                            // NOTE: For tessellation shader and geometry shader, the outermost array index might be
                            // used for vertex indexing. Thus, it should be counted out when collect input/output usage
                            // info.
                            const bool isGsInput   = ((shaderStage == ShaderStageGeometry) &&
                                                      (addrSpace == SPIRAS_Input));
                            const bool isTcsInput  = ((shaderStage == ShaderStageTessControl) &&
                                                      (addrSpace == SPIRAS_Input));
                            const bool isTcsOutput = ((shaderStage == ShaderStageTessControl) &&
                                                      (addrSpace == SPIRAS_Output));
                            const bool isTesInput  = ((shaderStage == ShaderStageTessEval) &&
                                                      (addrSpace == SPIRAS_Input));

                            bool isVertexIdx = false;

                            if (isGsInput || isTcsInput || isTcsOutput || isTesInput)
                            {
                                ShaderInOutMetadata inOutMeta = {};
                                inOutMeta.U64All = cast<ConstantInt>(pMeta->getOperand(1))->getZExtValue();

                                if (inOutMeta.IsBuiltIn)
                                {
                                    const BuiltIn builtInId = static_cast<BuiltIn>(inOutMeta.Value);
                                    isVertexIdx = ((builtInId == BuiltInPerVertex)    || // GLSL style per-vertex data
                                                   (builtInId == BuiltInPosition)     || // HLSL style per-vertex data
                                                   (builtInId == BuiltInPointSize)    ||
                                                   (builtInId == BuiltInClipDistance) ||
                                                   (builtInId == BuiltInCullDistance));
                                }
                                else
                                {
                                    isVertexIdx = (isGsInput || isTcsInput ||
                                                   ((isTcsOutput || isTesInput) && (inOutMeta.PerPatch == false)));
                                }
                            }

                            if (isVertexIdx)
                            {
                                // The outermost array index is for vertex indexing
                                pInOutTy = pInOutTy->getArrayElementType();
                                pMeta = cast<Constant>(pMeta->getOperand(2));
                            }
                        }

                        CollectInOutUsage(shaderStage, pInOutTy, pMeta, static_cast<SPIRAddressSpace>(addrSpace));
                    }
                }
                break;
            }
        case SPIRAS_Uniform:
            {
                // Buffer block
                MDNode* pMetaNode = pGlobal->getMetadata(gSPIRVMD::Resource);
                auto descSet   = mdconst::dyn_extract<ConstantInt>(pMetaNode->getOperand(0))->getZExtValue();
                auto binding   = mdconst::dyn_extract<ConstantInt>(pMetaNode->getOperand(1))->getZExtValue();
                auto blockType = mdconst::dyn_extract<ConstantInt>(pMetaNode->getOperand(2))->getZExtValue();
                LLPC_ASSERT((blockType == BlockTypeUniform) || (blockType == BlockTypeShaderStorage));

                DescriptorBinding bindingInfo = {};
                bindingInfo.descType  = (blockType == BlockTypeUniform) ? DescriptorType::UniformBlock :
                                                                          DescriptorType::ShaderStorageBlock;
                bindingInfo.arraySize = GetFlattenArrayElementCount(pGlobalTy);

                for (uint32_t stage = 0; stage < ShaderStageCount; ++stage)
                {
                    auto shaderStage = ShaderStage(stage);
                    if ((shaderUseMask >> shaderStage) & 1)
                    {
                        CollectDescriptorUsage(shaderStage, descSet, binding, &bindingInfo);
                    }
                }
                break;
            }
        case SPIRAS_PushConst:
            {
                // Push constant
                MDNode* pMetaNode = pGlobal->getMetadata(gSPIRVMD::PushConst);
                auto pushConstSize = mdconst::dyn_extract<ConstantInt>(pMetaNode->getOperand(0))->getZExtValue();
                for (uint32_t stage = 0; stage < ShaderStageCount; ++stage)
                {
                    auto shaderStage = ShaderStage(stage);
                    if ((shaderUseMask >> shaderStage) & 1)
                    {
                        m_pContext->GetShaderResourceUsage(shaderStage)->pushConstSizeInBytes = pushConstSize;
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

    if (useViewIndex)
    {
        // NOTE: Here, we add a global variable to emulate gl_ViewIndex. If subpassLoad() is invoked while multi-view is enabled, gl_ViewIndex is used implicitly.

        auto pViewIndex = new GlobalVariable(*m_pModule,
                                             m_pContext->Int32Ty(),
                                             false,
                                             GlobalValue::ExternalLinkage,
                                             nullptr,
                                             "gl_ViewIndex",
                                             0,
                                             GlobalVariable::NotThreadLocal,
                                             SPIRAS_Input);

        std::vector<Metadata*> viewIndexMeta;

        ShaderInOutMetadata viewIndexMetaValue = {};
        viewIndexMetaValue.U64All = 0;
        viewIndexMetaValue.IsBuiltIn = true;
        viewIndexMetaValue.Value = BuiltInViewIndex;
        viewIndexMetaValue.InterpMode = InterpModeSmooth;
        viewIndexMetaValue.InterpLoc = InterpLocCenter;

        auto pViewIndexMetaValue = ConstantInt::get(m_pContext->Int32Ty(), viewIndexMetaValue.U64All);
        viewIndexMeta.push_back(ConstantAsMetadata::get(pViewIndexMetaValue));
        auto pViewIndexMetaNode = MDNode::get(*m_pContext, viewIndexMeta);
        pViewIndex->addMetadata(gSPIRVMD::InOut, *pViewIndexMetaNode);
        CollectInOutUsage(ShaderStageFragment, m_pContext->Int32Ty(), pViewIndexMetaValue, SPIRAS_Input);
    }

    return true;
}

// =====================================================================================================================
// Gets element count if the specified type is an array (flattened for multi-dimension array).
uint32_t SpirvLowerResourceCollect::GetFlattenArrayElementCount(
    const Type* pTy // [in] Type to check
    ) const
{
    uint32_t elemCount = 1;

    auto pArrayTy = dyn_cast<ArrayType>(pTy);
    while (pArrayTy != nullptr)
    {
        elemCount *= pArrayTy->getArrayNumElements();
        pArrayTy = dyn_cast<ArrayType>(pArrayTy->getArrayElementType());
    }
    return elemCount;
}

// =====================================================================================================================
// Gets element type if the specified type is an array (flattened for multi-dimension array).
const Type* SpirvLowerResourceCollect::GetFlattenArrayElementType(
    const Type* pTy // [in] Type to check
    ) const
{
    const Type* pElemType = pTy;

    auto pArrayTy = dyn_cast<ArrayType>(pTy);
    while (pArrayTy != nullptr)
    {
        pElemType = pArrayTy->getArrayElementType();
        pArrayTy = dyn_cast<ArrayType>(pElemType);
    }
    return pElemType;
}

// =====================================================================================================================
// Collects the usage of execution modes from entry-point metadata.
void SpirvLowerResourceCollect::CollectExecutionModeUsage(
    ShaderStage shaderStage)  // API shader stage
{
    auto pResUsage = m_pContext->GetShaderResourceUsage(shaderStage);
    const auto execModel = static_cast<ExecutionModel>(shaderStage);
    std::string execModeMetaName = gSPIRVMD::ExecutionMode + std::string(".") + getName(execModel);

    ShaderExecModeMetadata execModeMeta = {};

    auto pEntryMetaNodes = m_pModule->getNamedMetadata(gSPIRVMD::EntryPoints);
    LLPC_ASSERT(pEntryMetaNodes != nullptr);

    for (uint32_t entryIdx = 0, entryCount = pEntryMetaNodes->getNumOperands(); entryIdx < entryCount; ++entryIdx)
    {
        auto pEntryMetaNode = pEntryMetaNodes->getOperand(entryIdx);
        if (pEntryMetaNode->getNumOperands() == 0)
        {
            continue;
        }

        for (uint32_t argIdx = 1, argCount = pEntryMetaNode->getNumOperands(); argIdx < argCount; ++argIdx)
        {
            auto pArgMetaNode = dyn_cast<MDNode>(pEntryMetaNode->getOperand(argIdx));
            if (pArgMetaNode == nullptr)
            {
                continue;
            }

            auto argName = dyn_cast<MDString>(pArgMetaNode->getOperand(0))->getString();
            if (argName == execModeMetaName)
            {
                execModeMeta.U32All[0] =
                    mdconst::dyn_extract<ConstantInt>(pArgMetaNode->getOperand(1))->getZExtValue();
                execModeMeta.U32All[1] =
                    mdconst::dyn_extract<ConstantInt>(pArgMetaNode->getOperand(2))->getZExtValue();
                execModeMeta.U32All[2] =
                    mdconst::dyn_extract<ConstantInt>(pArgMetaNode->getOperand(3))->getZExtValue();
                execModeMeta.U32All[3] =
                    mdconst::dyn_extract<ConstantInt>(pArgMetaNode->getOperand(4))->getZExtValue();

#if VKI_KHR_SHADER_FLOAT_CONTROLS
                auto fpControlFlags = execModeMeta.common.FpControlFlags;
                pResUsage->builtInUsage.common.denormPerserve           = fpControlFlags.DenormPerserve;
                pResUsage->builtInUsage.common.denormFlushToZero        = fpControlFlags.DenormFlushToZero;
                pResUsage->builtInUsage.common.signedZeroInfNanPreserve = fpControlFlags.SignedZeroInfNanPreserve;
                pResUsage->builtInUsage.common.roundingModeRTE          = fpControlFlags.RoundingModeRTE;
                pResUsage->builtInUsage.common.roundingModeRTZ          = fpControlFlags.RoundingModeRTZ;
#endif

                if (shaderStage == ShaderStageTessControl)
                {
                    LLPC_ASSERT(execModeMeta.ts.OutputVertices <= MaxTessPatchVertices);
                    pResUsage->builtInUsage.tcs.outputVertices = execModeMeta.ts.OutputVertices;

                    // NOTE: These execution modes belong to tessellation evaluation shader. But SPIR-V allows
                    // them to appear in tessellation control shader.
                    pResUsage->builtInUsage.tcs.vertexSpacing = SpacingUnknown;
                    if (execModeMeta.ts.SpacingEqual)
                    {
                        pResUsage->builtInUsage.tcs.vertexSpacing = SpacingEqual;
                    }
                    else if (execModeMeta.ts.SpacingFractionalEven)
                    {
                        pResUsage->builtInUsage.tcs.vertexSpacing = SpacingFractionalEven;
                    }
                    else if (execModeMeta.ts.SpacingFractionalOdd)
                    {
                        pResUsage->builtInUsage.tcs.vertexSpacing = SpacingFractionalOdd;
                    }

                    pResUsage->builtInUsage.tcs.vertexOrder = VertexOrderUnknown;
                    if (execModeMeta.ts.VertexOrderCw)
                    {
                        pResUsage->builtInUsage.tcs.vertexOrder = VertexOrderCw;
                    }
                    else if (execModeMeta.ts.VertexOrderCcw)
                    {
                        pResUsage->builtInUsage.tcs.vertexOrder = VertexOrderCcw;
                    }

                    pResUsage->builtInUsage.tcs.primitiveMode = SPIRVPrimitiveModeKind::Unknown;
                    if (execModeMeta.ts.Triangles)
                    {
                        pResUsage->builtInUsage.tcs.primitiveMode = Triangles;
                    }
                    else if (execModeMeta.ts.Quads)
                    {
                        pResUsage->builtInUsage.tcs.primitiveMode = Quads;
                    }
                    else if (execModeMeta.ts.Isolines)
                    {
                        pResUsage->builtInUsage.tcs.primitiveMode = Isolines;
                    }

                    pResUsage->builtInUsage.tcs.pointMode = false;
                    if (execModeMeta.ts.PointMode)
                    {
                        pResUsage->builtInUsage.tcs.pointMode = true;
                    }
                }
                else if (shaderStage == ShaderStageTessEval)
                {
                    pResUsage->builtInUsage.tes.vertexSpacing = SpacingUnknown;
                    if (execModeMeta.ts.SpacingEqual)
                    {
                        pResUsage->builtInUsage.tes.vertexSpacing = SpacingEqual;
                    }
                    else if (execModeMeta.ts.SpacingFractionalEven)
                    {
                        pResUsage->builtInUsage.tes.vertexSpacing = SpacingFractionalEven;
                    }
                    else if (execModeMeta.ts.SpacingFractionalOdd)
                    {
                        pResUsage->builtInUsage.tes.vertexSpacing = SpacingFractionalOdd;
                    }

                    pResUsage->builtInUsage.tes.vertexOrder = VertexOrderUnknown;
                    if (execModeMeta.ts.VertexOrderCw)
                    {
                        pResUsage->builtInUsage.tes.vertexOrder = VertexOrderCw;
                    }
                    else if (execModeMeta.ts.VertexOrderCcw)
                    {
                        pResUsage->builtInUsage.tes.vertexOrder = VertexOrderCcw;
                    }

                    pResUsage->builtInUsage.tes.primitiveMode = SPIRVPrimitiveModeKind::Unknown;
                    if (execModeMeta.ts.Triangles)
                    {
                        pResUsage->builtInUsage.tes.primitiveMode = Triangles;
                    }
                    else if (execModeMeta.ts.Quads)
                    {
                        pResUsage->builtInUsage.tes.primitiveMode = Quads;
                    }
                    else if (execModeMeta.ts.Isolines)
                    {
                        pResUsage->builtInUsage.tes.primitiveMode = Isolines;
                    }

                    pResUsage->builtInUsage.tes.pointMode = false;
                    if (execModeMeta.ts.PointMode)
                    {
                        pResUsage->builtInUsage.tes.pointMode = true;
                    }

                    // NOTE: This execution mode belongs to tessellation control shader. But SPIR-V allows
                    // it to appear in tessellation evaluation shader.
                    LLPC_ASSERT(execModeMeta.ts.OutputVertices <= MaxTessPatchVertices);
                    pResUsage->builtInUsage.tes.outputVertices = execModeMeta.ts.OutputVertices;
                }
                else if (shaderStage == ShaderStageGeometry)
                {
                    pResUsage->builtInUsage.gs.invocations = 1;
                    if (execModeMeta.gs.Invocations > 0)
                    {
                        LLPC_ASSERT(execModeMeta.gs.Invocations <= MaxGeometryInvocations);
                        pResUsage->builtInUsage.gs.invocations = execModeMeta.gs.Invocations;
                    }

                    LLPC_ASSERT(execModeMeta.gs.OutputVertices <= MaxGeometryOutputVertices);
                    pResUsage->builtInUsage.gs.outputVertices = execModeMeta.gs.OutputVertices;

                    if (execModeMeta.gs.InputPoints)
                    {
                        pResUsage->builtInUsage.gs.inputPrimitive = InputPoints;
                    }
                    else if (execModeMeta.gs.InputLines)
                    {
                        pResUsage->builtInUsage.gs.inputPrimitive = InputLines;
                    }
                    else if (execModeMeta.gs.InputLinesAdjacency)
                    {
                        pResUsage->builtInUsage.gs.inputPrimitive = InputLinesAdjacency;
                    }
                    else if (execModeMeta.gs.Triangles)
                    {
                        pResUsage->builtInUsage.gs.inputPrimitive = InputTriangles;
                    }
                    else if (execModeMeta.gs.InputTrianglesAdjacency)
                    {
                        pResUsage->builtInUsage.gs.inputPrimitive = InputTrianglesAdjacency;
                    }

                    if (execModeMeta.gs.OutputPoints)
                    {
                        pResUsage->builtInUsage.gs.outputPrimitive = OutputPoints;
                    }
                    else if (execModeMeta.gs.OutputLineStrip)
                    {
                        pResUsage->builtInUsage.gs.outputPrimitive = OutputLineStrip;
                    }
                    else if (execModeMeta.gs.OutputTriangleStrip)
                    {
                        pResUsage->builtInUsage.gs.outputPrimitive = OutputTriangleStrip;
                    }
                }
                else if (shaderStage == ShaderStageFragment)
                {
                    pResUsage->builtInUsage.fs.originUpperLeft    = execModeMeta.fs.OriginUpperLeft;
                    pResUsage->builtInUsage.fs.pixelCenterInteger = execModeMeta.fs.PixelCenterInteger;
                    pResUsage->builtInUsage.fs.earlyFragmentTests = execModeMeta.fs.EarlyFragmentTests;

                    pResUsage->builtInUsage.fs.depthMode = DepthReplacing;
                    if (execModeMeta.fs.DepthReplacing)
                    {
                        pResUsage->builtInUsage.fs.depthMode = DepthReplacing;
                    }
                    else if (execModeMeta.fs.DepthGreater)
                    {
                        pResUsage->builtInUsage.fs.depthMode = DepthGreater;
                    }
                    else if (execModeMeta.fs.DepthLess)
                    {
                        pResUsage->builtInUsage.fs.depthMode = DepthLess;
                    }
                    else if (execModeMeta.fs.DepthUnchanged)
                    {
                        pResUsage->builtInUsage.fs.depthMode = DepthUnchanged;
                    }
                }
                else if (shaderStage == ShaderStageCompute)
                {
                    LLPC_ASSERT((execModeMeta.cs.LocalSizeX <= MaxComputeWorkgroupSize) &&
                                (execModeMeta.cs.LocalSizeY <= MaxComputeWorkgroupSize) &&
                                (execModeMeta.cs.LocalSizeZ <= MaxComputeWorkgroupSize));

                    pResUsage->builtInUsage.cs.workgroupSizeX =
                        (execModeMeta.cs.LocalSizeX > 0) ? execModeMeta.cs.LocalSizeX : 1;
                    pResUsage->builtInUsage.cs.workgroupSizeY =
                        (execModeMeta.cs.LocalSizeY > 0) ? execModeMeta.cs.LocalSizeY : 1;
                    pResUsage->builtInUsage.cs.workgroupSizeZ =
                        (execModeMeta.cs.LocalSizeZ > 0) ? execModeMeta.cs.LocalSizeZ : 1;
                }

                break;
            }
        }
    }
}

// =====================================================================================================================
// Collects the usage info of descriptor sets and their bindings.
void SpirvLowerResourceCollect::CollectDescriptorUsage(
    ShaderStage              shaderStage,   // API shader stage
    uint32_t                 descSet,       // ID of descriptor set
    uint32_t                 binding,       // ID of descriptor binding
    const DescriptorBinding* pBindingInfo)  // [in] Descriptor binding info
{
    auto pResUsage = m_pContext->GetShaderResourceUsage(shaderStage);

    // The set ID is somewhat larger than expected
    if ((descSet + 1) > pResUsage->descSets.size())
    {
        pResUsage->descSets.resize(descSet + 1);
    }

    auto pDescSet = &pResUsage->descSets[descSet];
    static const DescriptorBinding DummyBinding = {};
    while ((binding + 1) > pDescSet->size())
    {
        // Insert dummy bindings till the binding ID is reached
        pDescSet->push_back(DummyBinding);
    }

    (*pDescSet)[binding] = *pBindingInfo;
}

// =====================================================================================================================
// Collects the usage info of inputs and outputs.
void SpirvLowerResourceCollect::CollectInOutUsage(
    ShaderStage      shaderStage,   // API shader stage
    const Type*      pInOutTy,      // [in] Type of this input/output
    Constant*        pInOutMeta,    // [in] Metadata of this input/output
    SPIRAddressSpace addrSpace)     // Address space
{
    LLPC_ASSERT((addrSpace == SPIRAS_Input) || (addrSpace == SPIRAS_Output));

    auto pResUsage = m_pContext->GetShaderResourceUsage(shaderStage);
    ShaderInOutMetadata inOutMeta = {};
    uint32_t locCount = 0;

    const Type* pBaseTy = nullptr;
    if (pInOutTy->isArrayTy())
    {
        // Input/output is array type
        inOutMeta.U64All = cast<ConstantInt>(pInOutMeta->getOperand(1))->getZExtValue();

        if (inOutMeta.IsBuiltIn)
        {
            // Built-in arrayed input/output
            const uint32_t builtInId = inOutMeta.Value;

            if (shaderStage == ShaderStageVertex)
            {
                switch (builtInId)
                {
                case BuiltInClipDistance:
                    {
                        const uint32_t elemCount = pInOutTy->getArrayNumElements();
                        LLPC_ASSERT(elemCount <= MaxClipCullDistanceCount);
                        pResUsage->builtInUsage.vs.clipDistance = elemCount;
                        break;
                    }
                case BuiltInCullDistance:
                    {
                        const uint32_t elemCount = pInOutTy->getArrayNumElements();
                        LLPC_ASSERT(elemCount <= MaxClipCullDistanceCount);
                        pResUsage->builtInUsage.vs.cullDistance = elemCount;
                        break;
                    }
                default:
                    {
                        LLPC_NEVER_CALLED();
                        break;
                    }
                }
            }
            else if (shaderStage == ShaderStageTessControl)
            {
                switch (builtInId)
                {
                case BuiltInClipDistance:
                    {
                        const uint32_t elemCount = pInOutTy->getArrayNumElements();
                        LLPC_ASSERT(elemCount <= MaxClipCullDistanceCount);

                        if (addrSpace == SPIRAS_Input)
                        {
                            pResUsage->builtInUsage.tcs.clipDistanceIn = elemCount;
                        }
                        else
                        {
                            LLPC_ASSERT(addrSpace == SPIRAS_Output);
                            pResUsage->builtInUsage.tcs.clipDistance = elemCount;
                        }
                        break;
                    }
                case BuiltInCullDistance:
                    {
                        const uint32_t elemCount = pInOutTy->getArrayNumElements();
                        LLPC_ASSERT(elemCount <= MaxClipCullDistanceCount);

                        if (addrSpace == SPIRAS_Input)
                        {
                            pResUsage->builtInUsage.tcs.cullDistanceIn = elemCount;
                        }
                        else
                        {
                            LLPC_ASSERT(addrSpace == SPIRAS_Output);
                            pResUsage->builtInUsage.tcs.cullDistance = elemCount;
                        }
                        break;
                    }
                case BuiltInTessLevelOuter:
                    {
                        pResUsage->builtInUsage.tcs.tessLevelOuter = true;
                        break;
                    }
                case BuiltInTessLevelInner:
                    {
                        pResUsage->builtInUsage.tcs.tessLevelInner = true;
                        break;
                    }
                case BuiltInPerVertex:
                    {
                        // Do nothing
                        break;
                    }
                default:
                    {
                        LLPC_NEVER_CALLED();
                        break;
                    }
                }
            }
            else if (shaderStage == ShaderStageTessEval)
            {
                switch (builtInId)
                {
                case BuiltInClipDistance:
                    {
                        const uint32_t elemCount = pInOutTy->getArrayNumElements();
                        LLPC_ASSERT(elemCount <= MaxClipCullDistanceCount);

                        if (addrSpace == SPIRAS_Input)
                        {
                            pResUsage->builtInUsage.tes.clipDistanceIn = elemCount;
                        }
                        else
                        {
                            LLPC_ASSERT(addrSpace == SPIRAS_Output);
                            pResUsage->builtInUsage.tes.clipDistance = elemCount;
                        }
                        break;
                    }
                case BuiltInCullDistance:
                    {
                        const uint32_t elemCount = pInOutTy->getArrayNumElements();
                        LLPC_ASSERT(elemCount <= MaxClipCullDistanceCount);

                        if (addrSpace == SPIRAS_Input)
                        {
                            pResUsage->builtInUsage.tes.cullDistanceIn = elemCount;
                        }
                        else
                        {
                            LLPC_ASSERT(addrSpace == SPIRAS_Output);
                            pResUsage->builtInUsage.tes.cullDistance = elemCount;
                        }
                        break;
                    }
                case BuiltInTessLevelOuter:
                    {
                        pResUsage->builtInUsage.tes.tessLevelOuter = true;
                        break;
                    }
                case BuiltInTessLevelInner:
                    {
                        pResUsage->builtInUsage.tes.tessLevelInner = true;
                        break;
                    }
                case BuiltInPerVertex:
                    {
                        if (addrSpace == SPIRAS_Input)
                        {
                            // Do nothing
                        }
                        else
                        {
                            LLPC_ASSERT(addrSpace == SPIRAS_Output);
                            LLPC_NEVER_CALLED();
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
            else if (shaderStage == ShaderStageGeometry)
            {
                switch (builtInId)
                {
                case BuiltInClipDistance:
                    {
                        const uint32_t elemCount = pInOutTy->getArrayNumElements();
                        LLPC_ASSERT(elemCount <= MaxClipCullDistanceCount);

                        if (addrSpace == SPIRAS_Input)
                        {
                            pResUsage->builtInUsage.gs.clipDistanceIn = elemCount;
                        }
                        else
                        {
                            LLPC_ASSERT(addrSpace == SPIRAS_Output);
                            pResUsage->builtInUsage.gs.clipDistance = elemCount;
                        }
                        break;
                    }
                case BuiltInCullDistance:
                    {
                        const uint32_t elemCount = pInOutTy->getArrayNumElements();
                        LLPC_ASSERT(elemCount <= MaxClipCullDistanceCount);

                        if (addrSpace == SPIRAS_Input)
                        {
                            pResUsage->builtInUsage.gs.cullDistanceIn = elemCount;
                        }
                        else
                        {
                            LLPC_ASSERT(addrSpace == SPIRAS_Output);
                            pResUsage->builtInUsage.gs.cullDistance = elemCount;
                        }
                        break;
                    }
                case BuiltInPerVertex:
                    {
                        if (addrSpace == SPIRAS_Input)
                        {
                            // Do nothing
                        }
                        else
                        {
                            LLPC_ASSERT(addrSpace == SPIRAS_Output);
                            LLPC_NEVER_CALLED();
                        }
                        break;
                    }
                default:
                    {
                        LLPC_NEVER_CALLED();
                        break;
                    }
                }

                if (addrSpace == SPIRAS_Output)
                {
                     CollectGsOutputInfo(pInOutTy, InvalidValue, inOutMeta);
                }
            }
            else if (shaderStage == ShaderStageFragment)
            {
                switch (builtInId)
                {
                case BuiltInClipDistance:
                    {
                        const uint32_t elemCount = pInOutTy->getArrayNumElements();
                        LLPC_ASSERT(elemCount <= MaxClipCullDistanceCount);
                        pResUsage->builtInUsage.fs.clipDistance = elemCount;

                        // NOTE: gl_ClipDistance[] is emulated via general inputs. Those qualifiers therefore have to
                        // be marked as used.
                        pResUsage->builtInUsage.fs.noperspective = true;
                        pResUsage->builtInUsage.fs.center = true;
                        break;
                    }
                case BuiltInCullDistance:
                    {
                        const uint32_t elemCount = pInOutTy->getArrayNumElements();
                        LLPC_ASSERT(elemCount <= MaxClipCullDistanceCount);
                        pResUsage->builtInUsage.fs.cullDistance = elemCount;

                        // NOTE: gl_CullDistance[] is emulated via general inputs. Those qualifiers therefore have to
                        // be marked as used.
                        pResUsage->builtInUsage.fs.noperspective = true;
                        pResUsage->builtInUsage.fs.center = true;
                        break;
                    }
                case BuiltInSampleMask:
                    {
                        if (addrSpace == SPIRAS_Input)
                        {
                            pResUsage->builtInUsage.fs.sampleMaskIn = true;
                        }
                        else
                        {
                            LLPC_ASSERT(addrSpace == SPIRAS_Output);
                            pResUsage->builtInUsage.fs.sampleMask = true;
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
        }
        else
        {
            // Generic arrayed input/output
            uint32_t stride  = cast<ConstantInt>(pInOutMeta->getOperand(0))->getZExtValue();

            const uint32_t startLoc = inOutMeta.Value;

            pBaseTy = GetFlattenArrayElementType(pInOutTy);
            locCount = (pInOutTy->getPrimitiveSizeInBits() > SizeOfVec4) ? 2 : 1;
            locCount *= (stride * cast<ArrayType>(pInOutTy)->getNumElements());

            // Prepare for location mapping
            if (addrSpace == SPIRAS_Input)
            {
                if (inOutMeta.PerPatch)
                {
                    LLPC_ASSERT(shaderStage == ShaderStageTessEval);
                    for (uint32_t i = 0; i < locCount; ++i)
                    {
                        pResUsage->inOutUsage.perPatchInputLocMap[startLoc + i] = InvalidValue;
                    }
                }
                else
                {
                    for (uint32_t i = 0; i < locCount; ++i)
                    {
                        pResUsage->inOutUsage.inputLocMap[startLoc + i] = InvalidValue;
                    }
                }
            }
            else
            {
                LLPC_ASSERT(addrSpace == SPIRAS_Output);

                if (inOutMeta.PerPatch)
                {
                    LLPC_ASSERT(shaderStage == ShaderStageTessControl);
                    for (uint32_t i = 0; i < locCount; ++i)
                    {
                        pResUsage->inOutUsage.perPatchOutputLocMap[startLoc + i] = InvalidValue;
                    }
                }
                else
                {
                    if (shaderStage == ShaderStageGeometry)
                    {
                        for (uint32_t i = 0; i < locCount; ++i)
                        {
                            CollectGsOutputInfo(pBaseTy, startLoc + i, inOutMeta);
                        }
                    }
                    else
                    {
                        for (uint32_t i = 0; i < locCount; ++i)
                        {
                            pResUsage->inOutUsage.outputLocMap[startLoc + i] = InvalidValue;
                        }
                    }
                }
            }

            // Special stage-specific processing
            if (shaderStage == ShaderStageVertex)
            {
                if (addrSpace == SPIRAS_Input)
                {
                    CollectVertexInputUsage(pBaseTy, (inOutMeta.Signedness != 0), startLoc, locCount);
                }
            }
            else if (shaderStage == ShaderStageFragment)
            {
                if (addrSpace == SPIRAS_Input)
                {
                    // Collect interpolation info
                    if (inOutMeta.InterpMode == InterpModeSmooth)
                    {
                        pResUsage->builtInUsage.fs.smooth = true;
                    }
                    else if (inOutMeta.InterpMode == InterpModeFlat)
                    {
                        pResUsage->builtInUsage.fs.flat = true;
                    }
                    else
                    {
                        LLPC_ASSERT(inOutMeta.InterpMode == InterpModeNoPersp);
                        pResUsage->builtInUsage.fs.noperspective = true;
                    }

                    if (inOutMeta.InterpLoc == InterpLocCenter)
                    {
                        pResUsage->builtInUsage.fs.center = true;
                    }
                    else if (inOutMeta.InterpLoc == InterpLocCentroid)
                    {
                        pResUsage->builtInUsage.fs.centroid = true;
                    }
                    else
                    {
                        LLPC_ASSERT(inOutMeta.InterpLoc == InterpLocSample);
                        pResUsage->builtInUsage.fs.sample = true;
                        pResUsage->builtInUsage.fs.runAtSampleRate = true;
                    }
                }
            }
        }
    }
    else if (pInOutTy->isStructTy())
    {
        // Input/output is structure type
        const uint32_t memberCount = pInOutTy->getStructNumElements();
        for (uint32_t memberIdx = 0; memberIdx < memberCount; ++memberIdx)
        {
            auto pMemberTy = pInOutTy->getStructElementType(memberIdx);
            auto pMemberMeta = cast<Constant>(pInOutMeta->getOperand(memberIdx));
            CollectInOutUsage(shaderStage, pMemberTy, pMemberMeta, addrSpace); // Collect usages for structure member
        }
    }
    else
    {
        // Input/output is scalar or vector type
        LLPC_ASSERT(pInOutTy->isSingleValueType());

        inOutMeta.U64All = cast<ConstantInt>(pInOutMeta)->getZExtValue();

        // Transform feedback input/output
        if (inOutMeta.IsXfb)
        {
            LLPC_ASSERT(inOutMeta.XfbBuffer < MaxTransformFeedbackBuffers);
            pResUsage->inOutUsage.xfbStrides[inOutMeta.XfbBuffer] = inOutMeta.XfbStride;

            if (inOutMeta.XfbStride <= inOutMeta.XfbOffset)
            {
                uint32_t elemCount = pInOutTy->isVectorTy() ? pInOutTy->getVectorNumElements() : 1;
                uint32_t inOutTySize = elemCount * pInOutTy->getScalarSizeInBits() / 8;
                pResUsage->inOutUsage.xfbStrides[inOutMeta.XfbBuffer]
                    = RoundUpToMultiple(uint32_t(inOutMeta.XfbOffset) + inOutTySize, 4u);
            }

            pResUsage->inOutUsage.enableXfb = (pResUsage->inOutUsage.enableXfb || (inOutMeta.XfbStride > 0));

            LLPC_ASSERT(inOutMeta.StreamId < MaxGsStreams);
            pResUsage->inOutUsage.streamXfbBuffers[inOutMeta.StreamId] = 1 << (inOutMeta.XfbBuffer);
        }

        if (inOutMeta.IsBuiltIn)
        {
            // Built-in input/output
            const uint32_t builtInId = inOutMeta.Value;

            if (shaderStage == ShaderStageVertex)
            {
                switch (builtInId)
                {
                case BuiltInVertexIndex:
                    pResUsage->builtInUsage.vs.vertexIndex = true;
                    pResUsage->builtInUsage.vs.baseVertex = true;
                    break;
                case BuiltInInstanceIndex:
                    pResUsage->builtInUsage.vs.instanceIndex = true;
                    pResUsage->builtInUsage.vs.baseInstance = true;
                    break;
                case BuiltInBaseVertex:
                    pResUsage->builtInUsage.vs.baseVertex = true;
                    break;
                case BuiltInBaseInstance:
                    pResUsage->builtInUsage.vs.baseInstance = true;
                    break;
                case BuiltInDrawIndex:
                    pResUsage->builtInUsage.vs.drawIndex = true;
                    break;
                case BuiltInPosition:
                    pResUsage->builtInUsage.vs.position = true;
                    break;
                case BuiltInPointSize:
                    pResUsage->builtInUsage.vs.pointSize = true;
                    break;
                case BuiltInViewportIndex:
                    pResUsage->builtInUsage.vs.viewportIndex = true;
                    break;
                case BuiltInLayer:
                    pResUsage->builtInUsage.vs.layer = true;
                    break;
                case BuiltInViewIndex:
                    pResUsage->builtInUsage.vs.viewIndex = true;
                    break;
                case BuiltInSubgroupSize:
                    pResUsage->builtInUsage.common.subgroupSize = true;
                    break;
                case BuiltInSubgroupLocalInvocationId:
                    pResUsage->builtInUsage.common.subgroupLocalInvocationId = true;
                    break;
                case BuiltInSubgroupEqMaskKHR:
                    pResUsage->builtInUsage.common.subgroupEqMask = true;
                    break;
                case BuiltInSubgroupGeMaskKHR:
                    pResUsage->builtInUsage.common.subgroupGeMask = true;
                    break;
                case BuiltInSubgroupGtMaskKHR:
                    pResUsage->builtInUsage.common.subgroupGtMask = true;
                    break;
                case BuiltInSubgroupLeMaskKHR:
                    pResUsage->builtInUsage.common.subgroupLeMask = true;
                    break;
                case BuiltInSubgroupLtMaskKHR:
                    pResUsage->builtInUsage.common.subgroupLtMask = true;
                    break;
                case BuiltInDeviceIndex:
                    pResUsage->builtInUsage.common.deviceIndex = true;
                    break;
                default:
                    LLPC_NEVER_CALLED();
                    break;
                }
            }
            else if (shaderStage == ShaderStageTessControl)
            {
                switch (builtInId)
                {
                case BuiltInPosition:
                    if (addrSpace == SPIRAS_Input)
                    {
                        pResUsage->builtInUsage.tcs.positionIn = true;
                    }
                    else
                    {
                        LLPC_ASSERT(addrSpace == SPIRAS_Output);
                        pResUsage->builtInUsage.tcs.position = true;
                    }
                    break;
                case BuiltInPointSize:
                    if (addrSpace == SPIRAS_Input)
                    {
                        pResUsage->builtInUsage.tcs.pointSizeIn = true;
                    }
                    else
                    {
                        LLPC_ASSERT(addrSpace == SPIRAS_Output);
                        pResUsage->builtInUsage.tcs.pointSize = true;
                    }
                    break;
                case BuiltInPatchVertices:
                    pResUsage->builtInUsage.tcs.patchVertices = true;
                    break;
                case BuiltInInvocationId:
                    pResUsage->builtInUsage.tcs.invocationId = true;
                    break;
                case BuiltInPrimitiveId:
                    pResUsage->builtInUsage.tcs.primitiveId = true;
                    break;
                case BuiltInSubgroupSize:
                    pResUsage->builtInUsage.common.subgroupSize = true;
                    break;
                case BuiltInSubgroupLocalInvocationId:
                    pResUsage->builtInUsage.common.subgroupLocalInvocationId = true;
                    break;
                case BuiltInSubgroupEqMaskKHR:
                    pResUsage->builtInUsage.common.subgroupEqMask = true;
                    break;
                case BuiltInSubgroupGeMaskKHR:
                    pResUsage->builtInUsage.common.subgroupGeMask = true;
                    break;
                case BuiltInSubgroupGtMaskKHR:
                    pResUsage->builtInUsage.common.subgroupGtMask = true;
                    break;
                case BuiltInSubgroupLeMaskKHR:
                    pResUsage->builtInUsage.common.subgroupLeMask = true;
                    break;
                case BuiltInSubgroupLtMaskKHR:
                    pResUsage->builtInUsage.common.subgroupLtMask = true;
                    break;
                case BuiltInDeviceIndex:
                    pResUsage->builtInUsage.common.deviceIndex = true;
                    break;
                default:
                    LLPC_NEVER_CALLED();
                    break;
                }
            }
            else if (shaderStage == ShaderStageTessEval)
            {
                switch (builtInId)
                {
                case BuiltInPosition:
                    if (addrSpace == SPIRAS_Input)
                    {
                        pResUsage->builtInUsage.tes.positionIn = true;
                    }
                    else
                    {
                        LLPC_ASSERT(addrSpace == SPIRAS_Output);
                        pResUsage->builtInUsage.tes.position = true;
                    }
                    break;
                case BuiltInPointSize:
                    if (addrSpace == SPIRAS_Input)
                    {
                        pResUsage->builtInUsage.tes.pointSizeIn = true;
                    }
                    else
                    {
                        LLPC_ASSERT(addrSpace == SPIRAS_Output);
                        pResUsage->builtInUsage.tes.pointSize = true;
                    }
                    break;
                case BuiltInPatchVertices:
                    pResUsage->builtInUsage.tes.patchVertices = true;
                    break;
                case BuiltInPrimitiveId:
                    pResUsage->builtInUsage.tes.primitiveId = true;
                    break;
                case BuiltInTessCoord:
                    pResUsage->builtInUsage.tes.tessCoord = true;
                    break;
                case BuiltInViewportIndex:
                    pResUsage->builtInUsage.tes.viewportIndex = true;
                    break;
                case BuiltInLayer:
                    pResUsage->builtInUsage.tes.layer = true;
                    break;
                case BuiltInViewIndex:
                    pResUsage->builtInUsage.tes.viewIndex = true;
                    break;
                case BuiltInSubgroupSize:
                    pResUsage->builtInUsage.common.subgroupSize = true;
                    break;
                case BuiltInSubgroupLocalInvocationId:
                    pResUsage->builtInUsage.common.subgroupLocalInvocationId = true;
                    break;
                case BuiltInSubgroupEqMaskKHR:
                    pResUsage->builtInUsage.common.subgroupEqMask = true;
                    break;
                case BuiltInSubgroupGeMaskKHR:
                    pResUsage->builtInUsage.common.subgroupGeMask = true;
                    break;
                case BuiltInSubgroupGtMaskKHR:
                    pResUsage->builtInUsage.common.subgroupGtMask = true;
                    break;
                case BuiltInSubgroupLeMaskKHR:
                    pResUsage->builtInUsage.common.subgroupLeMask = true;
                    break;
                case BuiltInSubgroupLtMaskKHR:
                    pResUsage->builtInUsage.common.subgroupLtMask = true;
                    break;
                case BuiltInDeviceIndex:
                    pResUsage->builtInUsage.common.deviceIndex = true;
                    break;
                default:
                    LLPC_NEVER_CALLED();
                    break;
                }
            }
            else if (shaderStage == ShaderStageGeometry)
            {
                switch (builtInId)
                {
                case BuiltInPosition:
                    if (addrSpace == SPIRAS_Input)
                    {
                        pResUsage->builtInUsage.gs.positionIn = true;
                    }
                    else
                    {
                        LLPC_ASSERT(addrSpace == SPIRAS_Output);
                        pResUsage->builtInUsage.gs.position = true;
                    }
                    break;
                case BuiltInPointSize:
                    if (addrSpace == SPIRAS_Input)
                    {
                        pResUsage->builtInUsage.gs.pointSizeIn = true;
                    }
                    else
                    {
                        LLPC_ASSERT(addrSpace == SPIRAS_Output);
                        pResUsage->builtInUsage.gs.pointSize = true;
                    }
                    break;
                case BuiltInInvocationId:
                    pResUsage->builtInUsage.gs.invocationId = true;
                    break;
                case BuiltInViewportIndex:
                    pResUsage->builtInUsage.gs.viewportIndex = true;
                    break;
                case BuiltInLayer:
                    pResUsage->builtInUsage.gs.layer = true;
                    break;
                case BuiltInViewIndex:
                    pResUsage->builtInUsage.gs.viewIndex = true;
                    break;
                case BuiltInPrimitiveId:
                    if (addrSpace == SPIRAS_Input)
                    {
                        pResUsage->builtInUsage.gs.primitiveIdIn = true;
                    }
                    else
                    {
                        LLPC_ASSERT(addrSpace == SPIRAS_Output);
                        pResUsage->builtInUsage.gs.primitiveId = true;
                    }
                    break;
                case BuiltInSubgroupSize:
                    pResUsage->builtInUsage.common.subgroupSize = true;
                    break;
                case BuiltInSubgroupLocalInvocationId:
                    pResUsage->builtInUsage.common.subgroupLocalInvocationId = true;
                    break;
                case BuiltInSubgroupEqMaskKHR:
                    pResUsage->builtInUsage.common.subgroupEqMask = true;
                    break;
                case BuiltInSubgroupGeMaskKHR:
                    pResUsage->builtInUsage.common.subgroupGeMask = true;
                    break;
                case BuiltInSubgroupGtMaskKHR:
                    pResUsage->builtInUsage.common.subgroupGtMask = true;
                    break;
                case BuiltInSubgroupLeMaskKHR:
                    pResUsage->builtInUsage.common.subgroupLeMask = true;
                    break;
                case BuiltInSubgroupLtMaskKHR:
                    pResUsage->builtInUsage.common.subgroupLtMask = true;
                    break;
                case BuiltInDeviceIndex:
                    pResUsage->builtInUsage.common.deviceIndex = true;
                    break;
                default:
                    LLPC_NEVER_CALLED();
                    break;
                }

                if (addrSpace == SPIRAS_Output)
                {
                    CollectGsOutputInfo(pInOutTy, InvalidValue, inOutMeta);
                }
            }
            else if (shaderStage == ShaderStageFragment)
            {
                switch (builtInId)
                {
                case BuiltInFragCoord:
                    pResUsage->builtInUsage.fs.fragCoord = true;
                    break;
                case BuiltInFrontFacing:
                    pResUsage->builtInUsage.fs.frontFacing = true;
                    break;
                case BuiltInPointCoord:
                    pResUsage->builtInUsage.fs.pointCoord = true;

                    // NOTE: gl_PointCoord is emulated via a general input. Those qualifiers therefore have to
                    // be marked as used.
                    pResUsage->builtInUsage.fs.smooth = true;
                    pResUsage->builtInUsage.fs.center = true;
                    break;
                case BuiltInPrimitiveId:
                    pResUsage->builtInUsage.fs.primitiveId = true;
                    break;
                case BuiltInSampleId:
                    pResUsage->builtInUsage.fs.sampleId = true;
                    pResUsage->builtInUsage.fs.runAtSampleRate = true;
                    break;
                case BuiltInSamplePosition:
                    pResUsage->builtInUsage.fs.samplePosition = true;
                    // NOTE: gl_SamplePostion is derived from gl_SampleID
                    pResUsage->builtInUsage.fs.sampleId = true;
                    pResUsage->builtInUsage.fs.runAtSampleRate = true;
                    break;
                case BuiltInLayer:
                    pResUsage->builtInUsage.fs.layer = true;
                    break;
                case BuiltInViewportIndex:
                    pResUsage->builtInUsage.fs.viewportIndex = true;
                    break;
                case BuiltInHelperInvocation:
                    pResUsage->builtInUsage.fs.helperInvocation = true;
                    break;
                case BuiltInFragDepth:
                    pResUsage->builtInUsage.fs.fragDepth = true;
                    break;
                case BuiltInFragStencilRefEXT:
                    pResUsage->builtInUsage.fs.fragStencilRef = true;
                    break;
                case BuiltInViewIndex:
                    pResUsage->builtInUsage.fs.viewIndex = true;
                    break;
                case BuiltInBaryCoordNoPerspAMD:
                    pResUsage->builtInUsage.fs.baryCoordNoPersp = true;
                    break;
                case BuiltInBaryCoordNoPerspCentroidAMD:
                    pResUsage->builtInUsage.fs.baryCoordNoPerspCentroid = true;
                    break;
                case BuiltInBaryCoordNoPerspSampleAMD:
                    pResUsage->builtInUsage.fs.baryCoordNoPerspSample = true;
                    break;
                case BuiltInBaryCoordSmoothAMD:
                    pResUsage->builtInUsage.fs.baryCoordSmooth = true;
                    break;
                case BuiltInBaryCoordSmoothCentroidAMD:
                    pResUsage->builtInUsage.fs.baryCoordSmoothCentroid = true;
                    break;
                case BuiltInBaryCoordSmoothSampleAMD:
                    pResUsage->builtInUsage.fs.baryCoordSmoothSample = true;
                    break;
                case BuiltInBaryCoordPullModelAMD:
                    pResUsage->builtInUsage.fs.baryCoordPullModel = true;
                    break;
                case BuiltInSubgroupSize:
                    pResUsage->builtInUsage.common.subgroupSize = true;
                    break;
                case BuiltInSubgroupLocalInvocationId:
                    pResUsage->builtInUsage.common.subgroupLocalInvocationId = true;
                    break;
                case BuiltInSubgroupEqMaskKHR:
                    pResUsage->builtInUsage.common.subgroupEqMask = true;
                    break;
                case BuiltInSubgroupGeMaskKHR:
                    pResUsage->builtInUsage.common.subgroupGeMask = true;
                    break;
                case BuiltInSubgroupGtMaskKHR:
                    pResUsage->builtInUsage.common.subgroupGtMask = true;
                    break;
                case BuiltInSubgroupLeMaskKHR:
                    pResUsage->builtInUsage.common.subgroupLeMask = true;
                    break;
                case BuiltInSubgroupLtMaskKHR:
                    pResUsage->builtInUsage.common.subgroupLtMask = true;
                    break;
                case BuiltInDeviceIndex:
                    pResUsage->builtInUsage.common.deviceIndex = true;
                    break;
                default:
                    LLPC_NEVER_CALLED();
                    break;
                }
            }
            else if (shaderStage == ShaderStageCompute)
            {
                switch (builtInId)
                {
                case BuiltInLocalInvocationId:
                    pResUsage->builtInUsage.cs.localInvocationId = true;
                    break;
                case BuiltInWorkgroupId:
                    pResUsage->builtInUsage.cs.workgroupId = true;
                    break;
                case BuiltInNumWorkgroups:
                    pResUsage->builtInUsage.cs.numWorkgroups = true;
                    break;
                case BuiltInGlobalInvocationId:
                    pResUsage->builtInUsage.cs.workgroupId = true;
                    pResUsage->builtInUsage.cs.localInvocationId = true;
                    break;
                case BuiltInLocalInvocationIndex:
                    pResUsage->builtInUsage.cs.workgroupId = true;
                    pResUsage->builtInUsage.cs.localInvocationId = true;
                    break;
                case BuiltInNumSubgroups:
                    pResUsage->builtInUsage.cs.numSubgroups = true;
                    pResUsage->builtInUsage.common.subgroupSize = true;
                    break;
                case BuiltInSubgroupId:
                    pResUsage->builtInUsage.cs.workgroupId = true;
                    pResUsage->builtInUsage.cs.localInvocationId = true;
                    pResUsage->builtInUsage.common.subgroupSize = true;
                    break;
                case BuiltInSubgroupSize:
                    pResUsage->builtInUsage.common.subgroupSize = true;
                    break;
                case BuiltInSubgroupLocalInvocationId:
                    pResUsage->builtInUsage.common.subgroupLocalInvocationId = true;
                    break;
                case BuiltInSubgroupEqMaskKHR:
                    pResUsage->builtInUsage.common.subgroupEqMask = true;
                    break;
                case BuiltInSubgroupGeMaskKHR:
                    pResUsage->builtInUsage.common.subgroupGeMask = true;
                    break;
                case BuiltInSubgroupGtMaskKHR:
                    pResUsage->builtInUsage.common.subgroupGtMask = true;
                    break;
                case BuiltInSubgroupLeMaskKHR:
                    pResUsage->builtInUsage.common.subgroupLeMask = true;
                    break;
                case BuiltInSubgroupLtMaskKHR:
                    pResUsage->builtInUsage.common.subgroupLtMask = true;
                    break;
                case BuiltInDeviceIndex:
                    pResUsage->builtInUsage.common.deviceIndex = true;
                    break;
                default:
                    LLPC_NEVER_CALLED();
                    break;
                }
            }
            else
            {
                LLPC_NOT_IMPLEMENTED();
            }
        }
        else
        {
            // Generic input/output
            const uint32_t startLoc = inOutMeta.Value + inOutMeta.Index;

            pBaseTy = pInOutTy;
            locCount = (pInOutTy->getPrimitiveSizeInBits() / 8 > SizeOfVec4) ? 2 : 1;

            // Prepare for location mapping
            if (addrSpace == SPIRAS_Input)
            {
                if (inOutMeta.PerPatch)
                {
                    LLPC_ASSERT(shaderStage == ShaderStageTessEval);
                    for (uint32_t i = 0; i < locCount; ++i)
                    {
                        pResUsage->inOutUsage.perPatchInputLocMap[startLoc + i] = InvalidValue;
                    }
                }
                else
                {
                    for (uint32_t i = 0; i < locCount; ++i)
                    {
                        pResUsage->inOutUsage.inputLocMap[startLoc + i] = InvalidValue;
                    }
                }
            }
            else
            {
                LLPC_ASSERT(addrSpace == SPIRAS_Output);

                if (inOutMeta.PerPatch)
                {
                    LLPC_ASSERT(shaderStage == ShaderStageTessControl);
                    for (uint32_t i = 0; i < locCount; ++i)
                    {
                        pResUsage->inOutUsage.perPatchOutputLocMap[startLoc + i] = InvalidValue;
                    }
                }
                else
                {
                    if (shaderStage == ShaderStageGeometry)
                    {
                        for (uint32_t i = 0; i < locCount; ++i)
                        {
                            CollectGsOutputInfo(pBaseTy, startLoc + i, inOutMeta);
                        }
                    }
                    else
                    {
                        for (uint32_t i = 0; i < locCount; ++i)
                        {
                            pResUsage->inOutUsage.outputLocMap[startLoc + i] = InvalidValue;
                        }
                    }
                }
            }

            // Special stage-specific processing
            if (shaderStage == ShaderStageVertex)
            {
                if (addrSpace == SPIRAS_Input)
                {
                    CollectVertexInputUsage(pBaseTy, (inOutMeta.Signedness != 0), startLoc, locCount);
                }
            }
            else if (shaderStage == ShaderStageFragment)
            {
                if (addrSpace == SPIRAS_Input)
                {
                    // Collect interpolation info
                    if (inOutMeta.InterpMode == InterpModeSmooth)
                    {
                        pResUsage->builtInUsage.fs.smooth = true;
                    }
                    else if (inOutMeta.InterpMode == InterpModeFlat)
                    {
                        pResUsage->builtInUsage.fs.flat = true;
                    }
                    else if (inOutMeta.InterpMode == InterpModeNoPersp)
                    {
                        pResUsage->builtInUsage.fs.noperspective = true;
                    }
                    else
                    {
                        LLPC_ASSERT(inOutMeta.InterpMode == InterpModeCustom);
                        pResUsage->builtInUsage.fs.custom = true;
                    }

                    if (inOutMeta.InterpLoc == InterpLocCenter)
                    {
                        pResUsage->builtInUsage.fs.center = true;
                    }
                    else if (inOutMeta.InterpLoc == InterpLocCentroid)
                    {
                        pResUsage->builtInUsage.fs.centroid = true;
                    }
                    else if (inOutMeta.InterpLoc == InterpLocSample)
                    {
                        pResUsage->builtInUsage.fs.sample = true;
                        pResUsage->builtInUsage.fs.runAtSampleRate = true;
                    }
                    else
                    {
                        LLPC_ASSERT(inOutMeta.InterpLoc == InterpLocCustom);
                        pResUsage->builtInUsage.fs.custom = true;
                    }
                }
                else
                {
                    LLPC_ASSERT(addrSpace == SPIRAS_Output);

                    LLPC_ASSERT((startLoc < MaxColorTargets) && (locCount == 1)); // Should not be 64-bit data type

                    // Collect basic types of fragment outputs
                    BasicType basicTy = BasicType::Unknown;

                    const auto pCompTy = pBaseTy->isVectorTy() ? pBaseTy->getVectorElementType() : pBaseTy;
                    const uint32_t bitWidth = pCompTy->getScalarSizeInBits();
                    const bool signedness = (inOutMeta.Signedness != 0);

                    if (pCompTy->isIntegerTy())
                    {
                        // Integer type
                        if (bitWidth == 16)
                        {
                            basicTy = signedness ? BasicType::Int16 : BasicType::Uint16;
                        }
                        else
                        {
                            LLPC_ASSERT(bitWidth == 32);
                            basicTy = signedness ? BasicType::Int : BasicType::Uint;
                        }
                    }
                    else if (pCompTy->isFloatingPointTy())
                    {
                        // Floating-point type
                        if (bitWidth == 16)
                        {
                            basicTy = BasicType::Float16;
                        }
                        else
                        {
                            LLPC_ASSERT(bitWidth == 32);
                            basicTy = BasicType::Float;
                        }
                    }
                    else
                    {
                        LLPC_NEVER_CALLED();
                    }

                    pResUsage->inOutUsage.fs.outputTypes[startLoc] = basicTy;

                    if (m_pContext->NeedAutoLayoutDesc())
                    {
                        // Collect CB shader mask (will be revised in LLVM patching operations)
                        LLPC_ASSERT(pBaseTy->isSingleValueType());
                        const uint32_t compCount = pBaseTy->isVectorTy() ? pBaseTy->getVectorNumElements() : 1;
                        const uint32_t compIdx = inOutMeta.Component;
                        LLPC_ASSERT(compIdx + compCount <= 4);

                        const uint32_t channelMask = (((1 << compCount) - 1) << compIdx);
                        pResUsage->inOutUsage.fs.cbShaderMask |= (channelMask << 4 * startLoc);
                    }
                }
            }
        }
    }
}

// =====================================================================================================================
// Collects the usage info of vertex inputs (particularly for the map from vertex input location to vertex basic type).
void SpirvLowerResourceCollect::CollectVertexInputUsage(
    const Type* pVertexTy,  // [in] Vertex input type
    bool        signedness, // Whether the type is signed (valid for integer type)
    uint32_t    startLoc,   // Start location
    uint32_t    locCount)   // Count of locations
{
    auto pResUsage = m_pContext->GetShaderResourceUsage(ShaderStageVertex);
    auto bitWidth = pVertexTy->getScalarSizeInBits();
    auto pCompTy  = pVertexTy->isVectorTy() ? pVertexTy->getVectorElementType() : pVertexTy;

    // Get basic type of vertex input
    BasicType basicTy = BasicType::Unknown;
    if (pCompTy->isIntegerTy())
    {
        // Integer type
        if (bitWidth == 16)
        {
            basicTy = signedness ? BasicType::Int16 : BasicType::Uint16;
        }
        else if (bitWidth == 32)
        {
            basicTy = signedness ? BasicType::Int : BasicType::Uint;
        }
        else
        {
            LLPC_ASSERT(bitWidth == 64);
            basicTy = signedness ? BasicType::Int64 : BasicType::Uint64;
        }
    }
    else if (pCompTy->isFloatingPointTy())
    {
        // Floating-point type
        if (bitWidth == 16)
        {
            basicTy = BasicType::Float16;
        }
        else if (bitWidth == 32)
        {
            basicTy = BasicType::Float;
        }
        else
        {
            LLPC_ASSERT(bitWidth == 64);
            basicTy = BasicType::Double;
        }
    }
    else
    {
        LLPC_NEVER_CALLED();
    }

    auto& vsInputTypes = pResUsage->inOutUsage.vs.inputTypes;
    while ((startLoc + locCount) > vsInputTypes.size())
    {
        vsInputTypes.push_back(BasicType::Unknown);
    }

    for (uint32_t i = 0; i < locCount; ++i)
    {
        vsInputTypes[startLoc + i] = basicTy;
    }
}

// =====================================================================================================================
// Collects output info for geometry shader.
void SpirvLowerResourceCollect::CollectGsOutputInfo(
    const Type*                pOutputTy,   // [in] Type of this output
    uint32_t                   location,    // Location of this output
    const ShaderInOutMetadata& outputMeta)  // [in] Metadata of this output
{
    auto pResUsage = m_pContext->GetShaderResourceUsage(ShaderStageGeometry);

    Type* pBaseTy = const_cast<Type*>(pOutputTy);
    if (pBaseTy->isArrayTy())
    {
        pBaseTy = pBaseTy->getArrayElementType();
    }
    LLPC_ASSERT(pBaseTy->isSingleValueType());

    XfbOutInfo xfbOutInfo = {};
    xfbOutInfo.xfbBuffer = outputMeta.XfbBuffer;
    xfbOutInfo.xfbOffset = outputMeta.XfbOffset;
    xfbOutInfo.is16bit   = (pBaseTy->getScalarSizeInBits() == 16);

    GsOutLocInfo outLocInfo = {};
    outLocInfo.location     = (outputMeta.IsBuiltIn ? outputMeta.Value : location);
    outLocInfo.isBuiltIn    = outputMeta.IsBuiltIn;
    outLocInfo.streamId     = outputMeta.StreamId;

    pResUsage->inOutUsage.gs.xfbOutsInfo[outLocInfo.u32All] = xfbOutInfo.u32All;

    if (outputMeta.IsBuiltIn)
    {
        // Collect raster stream ID for the export of built-ins
        pResUsage->inOutUsage.gs.rasterStream = outputMeta.StreamId;
    }
    else
    {
        pResUsage->inOutUsage.outputLocMap[outLocInfo.u32All] = InvalidValue;
    }
}

// =====================================================================================================================
// Calculate a mask of which shader stages reference the specified global
uint32_t SpirvLowerResourceCollect::GetGlobalShaderUse(
    GlobalValue* pGlobal)   // [in] Global variable
{
    uint32_t shaderUseMask = 0;

    // Calculate which functions reference the specified global. We need to iteratively follow constant exprs.
    std::set<Constant*> seenConstExprs;
    SmallVector<Constant*, 8> constants;
    constants.push_back(pGlobal);

    for (uint32_t i = 0; i < constants.size(); ++i)
    {
        auto pConstant = constants[i];
        for (auto pUser : pConstant->users())
        {
            auto pConstExprUser = dyn_cast<ConstantExpr>(pUser);
            if (pConstExprUser != nullptr)
            {
                if (seenConstExprs.insert(pConstExprUser).second)
                {
                    constants.push_back(pConstExprUser);
                }
            }
            else
            {
                shaderUseMask |= m_funcShaderUseMap[cast<Instruction>(pUser)->getParent()->getParent()];
            }
        }
    }

    LLVM_DEBUG(dbgs() << "Global " << pGlobal->getName() << " shader use: " << shaderUseMask << "\n");

    return shaderUseMask;
}

// =====================================================================================================================
// For each function, create a mask of which shader(s) it is used in
//
// This allows us to cope with the situation where a subfunction is used by more than one shader stage.
void SpirvLowerResourceCollect::SetFunctionShaderUse()
{
    m_funcShaderUseMap.clear();

    auto pCallGraph = &getAnalysis<CallGraphWrapperPass>().getCallGraph();

    std::map<CallGraphNode*, uint32_t> callGraphNodeShaderUseMap;
    std::queue<CallGraphNode*> callGraphNodes;
    std::set<CallGraphNode*> callGraphNodeList;

    // Add the shader entrypoints to the worklist, and initialize their masks.
    for (uint32_t stage = 0; stage < ShaderStageCount; ++stage)
    {
        auto shaderStage = ShaderStage(stage);
        auto pEntryPoint = m_pPipelineShaders->GetEntryPoint(shaderStage);
        if (pEntryPoint != nullptr)
        {
            auto pCallGraphNode = (*pCallGraph)[m_pPipelineShaders->GetEntryPoint(shaderStage)];
            callGraphNodeShaderUseMap[pCallGraphNode] = 1U << shaderStage;
            callGraphNodes.push(pCallGraphNode);
            callGraphNodeList.insert(pCallGraphNode);
        }
    }

    // Iterate until the queue is empty.
    while (callGraphNodes.empty() == false)
    {
        auto pCallGraphNode = callGraphNodes.front();
        callGraphNodes.pop();
        callGraphNodeList.erase(pCallGraphNode);
        // Visit other functions that this function calls.
        auto calleeShaderUseMask = callGraphNodeShaderUseMap[pCallGraphNode];
        for (auto calleeNodeIt : *pCallGraphNode)
        {
            CallGraphNode* pCalleeNode = calleeNodeIt.second;
            auto& calleeMask = callGraphNodeShaderUseMap[pCalleeNode];
            if ((calleeMask & calleeShaderUseMask) != calleeShaderUseMask)
            {
                // Propagate more shader use into callee node, and add it to the worklist if not already there.
                calleeMask |= calleeShaderUseMask;
                if (callGraphNodeList.insert(pCalleeNode).second)
                {
                    callGraphNodes.push(pCalleeNode);
                }
            }
        }
    }

    // Set up funcShaderUse from callGraphNodeShaderUseMap.
    for (auto shaderUseMapIt : callGraphNodeShaderUseMap)
    {
        auto pFunc = shaderUseMapIt.first->getFunction();
        if ((pFunc != nullptr) && (pFunc->empty() == false))
        {
            LLVM_DEBUG(dbgs() << "Function: " << pFunc->getName() << ", shader use: " << shaderUseMapIt.second << "\n");
            m_funcShaderUseMap[pFunc] = shaderUseMapIt.second;
        }
    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for resource collecting.
INITIALIZE_PASS(SpirvLowerResourceCollect, DEBUG_TYPE,
                "Lower SPIR-V resource collecting", false, false)
