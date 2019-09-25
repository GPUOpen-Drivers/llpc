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
 * @file  llpcSpirvLowerResourceCollect.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerResourceCollect.
 ***********************************************************************************************************************
 */
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcSpirvLowerResourceCollect.h"

#define DEBUG_TYPE "llpc-spirv-lower-resource-collect"

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
    initializeSpirvLowerResourceCollectPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerResourceCollect::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Resource-Collect\n");

    SpirvLower::Init(&module);

    ShaderStage shaderStage = m_shaderStage;

    m_pResUsage = m_pContext->GetShaderResourceUsage(shaderStage);

    CollectExecutionModeUsage();

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
    bool useImages = false;

    // Collect resource usages from globals
    for (auto pGlobal = m_pModule->global_begin(), pEnd = m_pModule->global_end(); pGlobal != pEnd; ++pGlobal)
    {
        const Type* pGlobalTy = pGlobal->getType()->getContainedType(0);

        auto addrSpace = pGlobal->getType()->getAddressSpace();
        switch (addrSpace)
        {
        case SPIRAS_Constant:
            {
                if (pGlobal->hasMetadata(gSPIRVMD::PushConst))
                {
                    // Push constant
                    MDNode* pMetaNode = pGlobal->getMetadata(gSPIRVMD::PushConst);
                    auto pushConstSize = mdconst::dyn_extract<ConstantInt>(pMetaNode->getOperand(0))->getZExtValue();
                    m_pResUsage->pushConstSizeInBytes = pushConstSize;
                }
                else
                {
                    useImages = true;

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
                                LLPC_ASSERT(m_shaderStage == ShaderStageFragment);
                                m_pResUsage->builtInUsage.fs.fragCoord = true;
                                useViewIndex = true;
                            }
                        }
                    }
                }
                break;
            }
        case SPIRAS_Private:
        case SPIRAS_Global:
        case SPIRAS_Local:
        case SPIRAS_Input:
        case SPIRAS_Output:
            {
                break;
            }
        case SPIRAS_Uniform:
            {
                // Buffer block
#ifndef NDEBUG
                MDNode* pMetaNode = pGlobal->getMetadata(gSPIRVMD::Resource);
                auto blockType = mdconst::dyn_extract<ConstantInt>(pMetaNode->getOperand(2))->getZExtValue();
                LLPC_ASSERT((blockType == BlockTypeUniform) || (blockType == BlockTypeShaderStorage));
#endif
                break;
            }
        default:
            {
                LLPC_NEVER_CALLED();
                break;
            }
        }
    }

    if (m_shaderStage == ShaderStageCompute)
    {
        auto& builtInUsage = m_pResUsage->builtInUsage.cs;

        bool reconfig = false;

        switch (static_cast<WorkgroupLayout>(builtInUsage.workgroupLayout))
        {
        case WorkgroupLayout::Unknown:
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 28
            // If no configuration has been specified, apply a reconfigure if the compute shader uses images and the
            // pipeline option was enabled.
            reconfig = useImages && m_pContext->GetTargetMachinePipelineOptions()->reconfigWorkgroupLayout;
#endif
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
            if (((builtInUsage.workgroupSizeX % 2) == 0) && ((builtInUsage.workgroupSizeY % 2) == 0))
            {
                if (((builtInUsage.workgroupSizeX > 8) && (builtInUsage.workgroupSizeY >= 8)) ||
                    ((builtInUsage.workgroupSizeX >= 8) && (builtInUsage.workgroupSizeY > 8)))
                {
                    // If our local size in the X & Y dimensions are greater than 8, we can reconfigure.
                    builtInUsage.workgroupLayout = static_cast<uint32_t>(WorkgroupLayout::SexagintiQuads);
                }
                else
                {
                    // If our local size in the X & Y dimensions are multiples of 2, we can reconfigure.
                    builtInUsage.workgroupLayout = static_cast<uint32_t>(WorkgroupLayout::Quads);
                }
            }
        }
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
void SpirvLowerResourceCollect::CollectExecutionModeUsage()
{
    const auto execModel = ConvertToExecModel(m_shaderStage);
    std::string execModeMetaName = gSPIRVMD::ExecutionMode + std::string(".") + getName(execModel);

    ShaderExecModeMetadata execModeMeta = {};

    auto pEntryMetaNodes = m_pModule->getNamedMetadata(gSPIRVMD::EntryPoints);
    if (pEntryMetaNodes == nullptr)
    {
        return;
    }

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

                auto fpControlFlags = execModeMeta.common.FpControlFlags;
                m_pResUsage->builtInUsage.common.denormPerserve           = fpControlFlags.DenormPerserve;
                m_pResUsage->builtInUsage.common.denormFlushToZero        = fpControlFlags.DenormFlushToZero;
                m_pResUsage->builtInUsage.common.signedZeroInfNanPreserve = fpControlFlags.SignedZeroInfNanPreserve;
                m_pResUsage->builtInUsage.common.roundingModeRTE          = fpControlFlags.RoundingModeRTE;
                m_pResUsage->builtInUsage.common.roundingModeRTZ          = fpControlFlags.RoundingModeRTZ;

                if (m_shaderStage == ShaderStageTessControl)
                {
                    LLPC_ASSERT(execModeMeta.ts.OutputVertices <= MaxTessPatchVertices);
                    m_pResUsage->builtInUsage.tcs.outputVertices = execModeMeta.ts.OutputVertices;

                    // NOTE: These execution modes belong to tessellation evaluation shader. But SPIR-V allows
                    // them to appear in tessellation control shader.
                    m_pResUsage->builtInUsage.tcs.vertexSpacing = SpacingUnknown;
                    if (execModeMeta.ts.SpacingEqual)
                    {
                        m_pResUsage->builtInUsage.tcs.vertexSpacing = SpacingEqual;
                    }
                    else if (execModeMeta.ts.SpacingFractionalEven)
                    {
                        m_pResUsage->builtInUsage.tcs.vertexSpacing = SpacingFractionalEven;
                    }
                    else if (execModeMeta.ts.SpacingFractionalOdd)
                    {
                        m_pResUsage->builtInUsage.tcs.vertexSpacing = SpacingFractionalOdd;
                    }

                    m_pResUsage->builtInUsage.tcs.vertexOrder = VertexOrderUnknown;
                    if (execModeMeta.ts.VertexOrderCw)
                    {
                        m_pResUsage->builtInUsage.tcs.vertexOrder = VertexOrderCw;
                    }
                    else if (execModeMeta.ts.VertexOrderCcw)
                    {
                        m_pResUsage->builtInUsage.tcs.vertexOrder = VertexOrderCcw;
                    }

                    m_pResUsage->builtInUsage.tcs.primitiveMode = SPIRVPrimitiveModeKind::Unknown;
                    if (execModeMeta.ts.Triangles)
                    {
                        m_pResUsage->builtInUsage.tcs.primitiveMode = Triangles;
                    }
                    else if (execModeMeta.ts.Quads)
                    {
                        m_pResUsage->builtInUsage.tcs.primitiveMode = Quads;
                    }
                    else if (execModeMeta.ts.Isolines)
                    {
                        m_pResUsage->builtInUsage.tcs.primitiveMode = Isolines;
                    }

                    m_pResUsage->builtInUsage.tcs.pointMode = false;
                    if (execModeMeta.ts.PointMode)
                    {
                        m_pResUsage->builtInUsage.tcs.pointMode = true;
                    }
                }
                else if (m_shaderStage == ShaderStageTessEval)
                {
                    m_pResUsage->builtInUsage.tes.vertexSpacing = SpacingUnknown;
                    if (execModeMeta.ts.SpacingEqual)
                    {
                        m_pResUsage->builtInUsage.tes.vertexSpacing = SpacingEqual;
                    }
                    else if (execModeMeta.ts.SpacingFractionalEven)
                    {
                        m_pResUsage->builtInUsage.tes.vertexSpacing = SpacingFractionalEven;
                    }
                    else if (execModeMeta.ts.SpacingFractionalOdd)
                    {
                        m_pResUsage->builtInUsage.tes.vertexSpacing = SpacingFractionalOdd;
                    }

                    m_pResUsage->builtInUsage.tes.vertexOrder = VertexOrderUnknown;
                    if (execModeMeta.ts.VertexOrderCw)
                    {
                        m_pResUsage->builtInUsage.tes.vertexOrder = VertexOrderCw;
                    }
                    else if (execModeMeta.ts.VertexOrderCcw)
                    {
                        m_pResUsage->builtInUsage.tes.vertexOrder = VertexOrderCcw;
                    }

                    m_pResUsage->builtInUsage.tes.primitiveMode = SPIRVPrimitiveModeKind::Unknown;
                    if (execModeMeta.ts.Triangles)
                    {
                        m_pResUsage->builtInUsage.tes.primitiveMode = Triangles;
                    }
                    else if (execModeMeta.ts.Quads)
                    {
                        m_pResUsage->builtInUsage.tes.primitiveMode = Quads;
                    }
                    else if (execModeMeta.ts.Isolines)
                    {
                        m_pResUsage->builtInUsage.tes.primitiveMode = Isolines;
                    }

                    m_pResUsage->builtInUsage.tes.pointMode = false;
                    if (execModeMeta.ts.PointMode)
                    {
                        m_pResUsage->builtInUsage.tes.pointMode = true;
                    }

                    // NOTE: This execution mode belongs to tessellation control shader. But SPIR-V allows
                    // it to appear in tessellation evaluation shader.
                    LLPC_ASSERT(execModeMeta.ts.OutputVertices <= MaxTessPatchVertices);
                    m_pResUsage->builtInUsage.tes.outputVertices = execModeMeta.ts.OutputVertices;
                }
                else if (m_shaderStage == ShaderStageGeometry)
                {
                    m_pResUsage->builtInUsage.gs.invocations = 1;
                    if (execModeMeta.gs.Invocations > 0)
                    {
                        LLPC_ASSERT(execModeMeta.gs.Invocations <= MaxGeometryInvocations);
                        m_pResUsage->builtInUsage.gs.invocations = execModeMeta.gs.Invocations;
                    }

                    LLPC_ASSERT(execModeMeta.gs.OutputVertices <= MaxGeometryOutputVertices);
                    m_pResUsage->builtInUsage.gs.outputVertices = execModeMeta.gs.OutputVertices;

                    if (execModeMeta.gs.InputPoints)
                    {
                        m_pResUsage->builtInUsage.gs.inputPrimitive = InputPoints;
                    }
                    else if (execModeMeta.gs.InputLines)
                    {
                        m_pResUsage->builtInUsage.gs.inputPrimitive = InputLines;
                    }
                    else if (execModeMeta.gs.InputLinesAdjacency)
                    {
                        m_pResUsage->builtInUsage.gs.inputPrimitive = InputLinesAdjacency;
                    }
                    else if (execModeMeta.gs.Triangles)
                    {
                        m_pResUsage->builtInUsage.gs.inputPrimitive = InputTriangles;
                    }
                    else if (execModeMeta.gs.InputTrianglesAdjacency)
                    {
                        m_pResUsage->builtInUsage.gs.inputPrimitive = InputTrianglesAdjacency;
                    }

                    if (execModeMeta.gs.OutputPoints)
                    {
                        m_pResUsage->builtInUsage.gs.outputPrimitive = OutputPoints;
                    }
                    else if (execModeMeta.gs.OutputLineStrip)
                    {
                        m_pResUsage->builtInUsage.gs.outputPrimitive = OutputLineStrip;
                    }
                    else if (execModeMeta.gs.OutputTriangleStrip)
                    {
                        m_pResUsage->builtInUsage.gs.outputPrimitive = OutputTriangleStrip;
                    }
                }
                else if (m_shaderStage == ShaderStageFragment)
                {
                    m_pResUsage->builtInUsage.fs.originUpperLeft    = execModeMeta.fs.OriginUpperLeft;
                    m_pResUsage->builtInUsage.fs.pixelCenterInteger = execModeMeta.fs.PixelCenterInteger;
                    m_pResUsage->builtInUsage.fs.earlyFragmentTests = execModeMeta.fs.EarlyFragmentTests;
                    m_pResUsage->builtInUsage.fs.postDepthCoverage  = execModeMeta.fs.PostDepthCoverage;

                    m_pResUsage->builtInUsage.fs.depthMode = DepthReplacing;
                    if (execModeMeta.fs.DepthReplacing)
                    {
                        m_pResUsage->builtInUsage.fs.depthMode = DepthReplacing;
                    }
                    else if (execModeMeta.fs.DepthGreater)
                    {
                        m_pResUsage->builtInUsage.fs.depthMode = DepthGreater;
                    }
                    else if (execModeMeta.fs.DepthLess)
                    {
                        m_pResUsage->builtInUsage.fs.depthMode = DepthLess;
                    }
                    else if (execModeMeta.fs.DepthUnchanged)
                    {
                        m_pResUsage->builtInUsage.fs.depthMode = DepthUnchanged;
                    }
                }
                else if (m_shaderStage == ShaderStageCompute)
                {
                    LLPC_ASSERT((execModeMeta.cs.LocalSizeX <= MaxComputeWorkgroupSize) &&
                                (execModeMeta.cs.LocalSizeY <= MaxComputeWorkgroupSize) &&
                                (execModeMeta.cs.LocalSizeZ <= MaxComputeWorkgroupSize));

                    m_pResUsage->builtInUsage.cs.workgroupSizeX =
                        (execModeMeta.cs.LocalSizeX > 0) ? execModeMeta.cs.LocalSizeX : 1;
                    m_pResUsage->builtInUsage.cs.workgroupSizeY =
                        (execModeMeta.cs.LocalSizeY > 0) ? execModeMeta.cs.LocalSizeY : 1;
                    m_pResUsage->builtInUsage.cs.workgroupSizeZ =
                        (execModeMeta.cs.LocalSizeZ > 0) ? execModeMeta.cs.LocalSizeZ : 1;
                }

                break;
            }
        }
    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for resource collecting.
INITIALIZE_PASS(SpirvLowerResourceCollect, DEBUG_TYPE,
                "Lower SPIR-V resource collecting", false, false)
