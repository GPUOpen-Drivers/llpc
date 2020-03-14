/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPipelineState.cpp
 * @brief LLPC source file: contains implementation of class lgc::PipelineState.
 ***********************************************************************************************************************
 */
#include "llpcBuilderContext.h"
#include "llpcBuilderRecorder.h"
#include "llpcCodeGenManager.h"
#include "llpcFragColorExport.h"
#include "llpcInternal.h"
#include "llpcPassManager.h"
#include "llpcPatch.h"
#include "llpcPipelineState.h"
#include "llpcTargetInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Timer.h"
#include "llvm/Target/TargetMachine.h"

#define DEBUG_TYPE "llpc-pipeline-state"

using namespace lgc;
using namespace llvm;

// -enable-tess-offchip: enable tessellation off-chip mode
static cl::opt<bool> EnableTessOffChip("enable-tess-offchip",
                                       cl::desc("Enable tessellation off-chip mode"),
                                       cl::init(false));

// Names for named metadata nodes when storing and reading back pipeline state
static const char OptionsMetadataName[] = "llpc.options";
static const char UserDataMetadataName[] = "llpc.user.data.nodes";
static const char DeviceIndexMetadataName[] = "llpc.device.index";
static const char VertexInputsMetadataName[] = "llpc.vertex.inputs";
static const char IaStateMetadataName[] = "llpc.input.assembly.state";
static const char VpStateMetadataName[] = "llpc.viewport.state";
static const char RsStateMetadataName[] = "llpc.rasterizer.state";
static const char ColorExportFormatsMetadataName[] = "llpc.color.export.formats";
static const char ColorExportStateMetadataName[] = "llpc.color.export.state";

// =====================================================================================================================
// Get LLVMContext
LLVMContext& Pipeline::GetContext() const
{
    return GetBuilderContext()->GetContext();
}

// =====================================================================================================================
// Get TargetInfo
const TargetInfo& PipelineState::GetTargetInfo() const
{
    return GetBuilderContext()->GetTargetInfo();
}

// =====================================================================================================================
// Get PAL pipeline ABI version
uint32_t PipelineState::GetPalAbiVersion() const
{
    return GetBuilderContext()->GetPalAbiVersion();
}

// =====================================================================================================================
// Link shader modules into a pipeline module.
Module* PipelineState::Link(
    ArrayRef<Module*> modules)               // Array of modules indexed by shader stage, with nullptr entry
                                             // for any stage not present in the pipeline. Modules are freed.
{
    // Processing for each shader module before linking.
    IRBuilder<> builder(GetContext());
    uint32_t metaKindId = GetContext().getMDKindID(lgcName::ShaderStageMetadata);
    Module* pAnyModule = nullptr;
    for (uint32_t stage = 0; stage < modules.size(); ++stage)
    {
        Module* pModule = modules[stage];
        if (pModule == nullptr)
        {
            continue;
        }
        pAnyModule = pModule;

        // If this is a link of shader modules from earlier separate shader compiles, then the modes are
        // recorded in IR metadata. Read the modes here.
        GetShaderModes()->ReadModesFromShader(pModule, static_cast<ShaderStage>(stage));

        // Add IR metadata for the shader stage to each function in the shader, and rename the entrypoint to
        // ensure there is no clash on linking.
        auto pStageMetaNode = MDNode::get(GetContext(), { ConstantAsMetadata::get(builder.getInt32(stage)) });
        for (Function& func : *pModule)
        {
            if (func.isDeclaration() == false)
            {
                func.setMetadata(metaKindId, pStageMetaNode);
                if (func.getLinkage() != GlobalValue::InternalLinkage)
                {
                    func.setName(Twine(lgcName::EntryPointPrefix) +
                                 GetShaderStageAbbreviation(static_cast<ShaderStage>(stage)) +
                                 "." +
                                 func.getName());
                }
            }
        }
    }

    // If the front-end was using a BuilderRecorder, record pipeline state into IR metadata.
    if (m_noReplayer == false)
    {
        Record(pAnyModule);
    }

    // If there is only one shader, just change the name on its module and return it.
    Module* pPipelineModule = nullptr;
    for (auto pModule : modules)
    {
        if (pPipelineModule == nullptr)
        {
            pPipelineModule = pModule;
        }
        else if (pModule != nullptr)
        {
            pPipelineModule = nullptr;
            break;
        }
    }

    if (pPipelineModule != nullptr)
    {
        pPipelineModule->setModuleIdentifier("llpcPipeline");
    }
    else
    {
        // Create an empty module then link each shader module into it. We record pipeline state into IR
        // metadata before the link, to avoid problems with a Constant for an immutable descriptor value
        // disappearing when modules are deleted.
        bool result = true;
        pPipelineModule = new Module("llpcPipeline", GetContext());
        TargetMachine* pTargetMachine = GetBuilderContext()->GetTargetMachine();
        pPipelineModule->setTargetTriple(pTargetMachine->getTargetTriple().getTriple());
        pPipelineModule->setDataLayout(pTargetMachine->createDataLayout());

        Linker linker(*pPipelineModule);

        for (uint32_t shaderIndex = 0; shaderIndex < modules.size(); ++shaderIndex)
        {
            if (modules[shaderIndex] != nullptr)
            {
                // NOTE: We use unique_ptr here. The shader module will be destroyed after it is
                // linked into pipeline module.
                if (linker.linkInModule(std::unique_ptr<Module>(modules[shaderIndex])))
                {
                    result = false;
                }
            }
        }

        if (result == false)
        {
            delete pPipelineModule;
            pPipelineModule = nullptr;
        }
    }
    return pPipelineModule;
}

// =====================================================================================================================
// Generate pipeline module by running patch, middle-end optimization and backend codegen passes.
// The output is normally ELF, but IR disassembly if an option is used to stop compilation early.
// Output is written to outStream.
// Like other Builder methods, on error, this calls report_fatal_error, which you can catch by setting
// a diagnostic handler with LLVMContext::setDiagnosticHandler.
void PipelineState::Generate(
    std::unique_ptr<Module>         pipelineModule,       // IR pipeline module
    raw_pwrite_stream&              outStream,            // [in/out] Stream to write ELF or IR disassembly output
    Pipeline::CheckShaderCacheFunc  checkShaderCacheFunc, // Function to check shader cache in graphics pipeline
    ArrayRef<Timer*>                timers)               // Timers for: patch passes, llvm optimizations, codegen
{
    uint32_t passIndex = 1000;
    Timer* pPatchTimer = (timers.size() >= 1) ? timers[0] : nullptr;
    Timer* pOptTimer = (timers.size() >= 2) ? timers[1] : nullptr;
    Timer* pCodeGenTimer = (timers.size() >= 3) ? timers[2] : nullptr;

    // Set up "whole pipeline" passes, where we have a single module representing the whole pipeline.
    //
    // TODO: The "whole pipeline" passes are supposed to include code generation passes. However, there is a CTS issue.
    // In the case "dEQP-VK.spirv_assembly.instruction.graphics.16bit_storage.struct_mixed_types.uniform_geom", GS gets
    // unrolled to such a size that backend compilation takes too long. Thus, we put code generation in its own pass
    // manager.
    std::unique_ptr<PassManager> patchPassMgr(PassManager::Create());
    patchPassMgr->SetPassIndex(&passIndex);
    patchPassMgr->add(createTargetTransformInfoWrapperPass(
                          GetBuilderContext()->GetTargetMachine()->getTargetIRAnalysis()));

    // Manually add a target-aware TLI pass, so optimizations do not think that we have library functions.
    GetBuilderContext()->PreparePassManager(&*patchPassMgr);

    // Manually add a PipelineStateWrapper pass.
    // If we were not using BuilderRecorder, give our PipelineState to it. (In the BuilderRecorder case,
    // the first time PipelineStateWrapper is used, it allocates its own PipelineState and populates
    // it by reading IR metadata.)
    PipelineStateWrapper* pPipelineStateWrapper = new PipelineStateWrapper(GetBuilderContext());
    patchPassMgr->add(pPipelineStateWrapper);
    if (m_noReplayer)
    {
        pPipelineStateWrapper->SetPipelineState(this);
    }

    // Get a BuilderReplayer pass if needed.
    ModulePass* pReplayerPass = nullptr;
    if (m_noReplayer == false)
    {
        pReplayerPass = CreateBuilderReplayer(this);
    }

    // Patching.
    Patch::AddPasses(this,
                     *patchPassMgr,
                     pReplayerPass,
                     pPatchTimer,
                     pOptTimer,
                     checkShaderCacheFunc);

    // Add pass to clear pipeline state from IR
    patchPassMgr->add(CreatePipelineStateClearer());

    // Run the "whole pipeline" passes, excluding the target backend.
    patchPassMgr->run(*pipelineModule);
    patchPassMgr.reset(nullptr);

    // A separate "whole pipeline" pass manager for code generation.
    std::unique_ptr<PassManager> codeGenPassMgr(PassManager::Create());
    codeGenPassMgr->SetPassIndex(&passIndex);

    // Code generation.
    GetBuilderContext()->AddTargetPasses(*codeGenPassMgr, pCodeGenTimer, outStream);

    // Run the target backend codegen passes.
    codeGenPassMgr->run(*pipelineModule);
}

// =====================================================================================================================
// Clear the pipeline state IR metadata.
void PipelineState::Clear(
    Module* pModule)    // [in/out] IR module
{
    GetShaderModes()->Clear();
    m_options = {};
    m_userDataNodes = {};
    m_deviceIndex = 0;
    m_vertexInputDescriptions.clear();
    m_colorExportFormats.clear();
    m_colorExportState = {};
    m_inputAssemblyState = {};
    m_viewportState = {};
    m_rasterizerState = {};
    Record(pModule);
}

// =====================================================================================================================
// Record pipeline state into IR metadata of specified module.
void PipelineState::Record(
    Module* pModule)    // [in/out] Module to record the IR metadata in
{
    GetShaderModes()->Record(pModule);
    RecordOptions(pModule);
    RecordUserDataNodes(pModule);
    RecordDeviceIndex(pModule);
    RecordVertexInputDescriptions(pModule);
    RecordColorExportState(pModule);
    RecordGraphicsState(pModule);
}

// =====================================================================================================================
// Set up the pipeline state from the pipeline IR module.
void PipelineState::ReadState(
    Module* pModule)    // [in] LLVM module
{
    GetShaderModes()->ReadModesFromPipeline(pModule);
    ReadShaderStageMask(pModule);
    ReadOptions(pModule);
    ReadUserDataNodes(pModule);
    ReadDeviceIndex(pModule);
    ReadVertexInputDescriptions(pModule);
    ReadColorExportState(pModule);
    ReadGraphicsState(pModule);
}

// =====================================================================================================================
// Read shaderStageMask from IR. This consists of checking what shader stage functions are present in the IR.
void PipelineState::ReadShaderStageMask(
    Module* pModule)    // [in] LLVM module
{
    m_stageMask = 0;
    for (auto& func : *pModule)
    {
        if ((func.empty() == false) && (func.getLinkage() != GlobalValue::InternalLinkage))
        {
            auto shaderStage = GetShaderStageFromFunction(&func);

            if (shaderStage != ShaderStageInvalid)
            {
                m_stageMask |= 1 << shaderStage;
            }
        }
    }
}

// =====================================================================================================================
// Get the last vertex processing shader stage in this pipeline, or ShaderStageInvalid if none.
ShaderStage PipelineState::GetLastVertexProcessingStage() const
{
    if (m_stageMask & ShaderStageToMask(ShaderStageCopyShader))
    {
        return ShaderStageCopyShader;
    }
    if (m_stageMask & ShaderStageToMask(ShaderStageGeometry))
    {
        return ShaderStageGeometry;
    }
    if (m_stageMask & ShaderStageToMask(ShaderStageTessEval))
    {
        return ShaderStageTessEval;
    }
    if (m_stageMask & ShaderStageToMask(ShaderStageVertex))
    {
        return ShaderStageVertex;
    }
    return ShaderStageInvalid;
}

// =====================================================================================================================
// Gets the previous active shader stage in this pipeline
ShaderStage PipelineState::GetPrevShaderStage(
    ShaderStage shaderStage // Current shader stage
    ) const
{
    if (shaderStage == ShaderStageCompute)
    {
        return ShaderStageInvalid;
    }

    if (shaderStage == ShaderStageCopyShader)
    {
        // Treat copy shader as part of geometry shader
        shaderStage = ShaderStageGeometry;
    }

    assert(shaderStage < ShaderStageGfxCount);

    ShaderStage prevStage = ShaderStageInvalid;

    for (int32_t stage = shaderStage - 1; stage >= 0; --stage)
    {
        if ((m_stageMask & ShaderStageToMask(static_cast<ShaderStage>(stage))) != 0)
        {
            prevStage = static_cast<ShaderStage>(stage);
            break;
        }
    }

    return prevStage;
}

// =====================================================================================================================
// Gets the next active shader stage in this pipeline
ShaderStage PipelineState::GetNextShaderStage(
    ShaderStage shaderStage // Current shader stage
    ) const
{
    if (shaderStage == ShaderStageCompute)
    {
        return ShaderStageInvalid;
    }

    if (shaderStage == ShaderStageCopyShader)
    {
        // Treat copy shader as part of geometry shader
        shaderStage = ShaderStageGeometry;
    }

    assert(shaderStage < ShaderStageGfxCount);

    ShaderStage nextStage = ShaderStageInvalid;

    for (uint32_t stage = shaderStage + 1; stage < ShaderStageGfxCount; ++stage)
    {
        if ((m_stageMask & ShaderStageToMask(static_cast<ShaderStage>(stage))) != 0)
        {
            nextStage = static_cast<ShaderStage>(stage);
            break;
        }
    }

    return nextStage;
}

// =====================================================================================================================
// Check whether the pipeline is a graphics pipeline
bool PipelineState::IsGraphics() const
{
    return (GetShaderStageMask() &
            ((1U << ShaderStageVertex) |
             (1U << ShaderStageTessControl) |
             (1U << ShaderStageTessEval) |
             (1U << ShaderStageGeometry) |
             (1U << ShaderStageFragment))) != 0;
}

// =====================================================================================================================
// Set per-shader options
void PipelineState::SetShaderOptions(
    ShaderStage           stage,    // Shader stage
    const ShaderOptions&  options)  // [in] Shader options
{
    if (m_shaderOptions.size() <= stage)
    {
        m_shaderOptions.resize(stage + 1);
    }
    m_shaderOptions[stage] = options;
}

// =====================================================================================================================
// Get per-shader options
const ShaderOptions& PipelineState::GetShaderOptions(
    ShaderStage           stage)    // Shader stage
{
    if (m_shaderOptions.size() <= stage)
    {
        m_shaderOptions.resize(stage + 1);
    }
    return m_shaderOptions[stage];
}

// =====================================================================================================================
// Record pipeline and shader options into IR metadata.
// TODO: The options could be recorded in a more human-readable form, with a string for the option name for each
// option.
void PipelineState::RecordOptions(
    Module* pModule)    // [in/out] Module to record metadata into
{
    SetNamedMetadataToArrayOfInt32(pModule, m_options, OptionsMetadataName);
    for (uint32_t stage = 0; stage != m_shaderOptions.size(); ++stage)
    {
        std::string metadataName = (Twine(OptionsMetadataName) + "." +
                                    GetShaderStageAbbreviation(static_cast<ShaderStage>(stage))).str();
        SetNamedMetadataToArrayOfInt32(pModule, m_shaderOptions[stage], metadataName);
    }
}

// =====================================================================================================================
// Read pipeline and shader options from IR metadata
void PipelineState::ReadOptions(
    Module* pModule)    // [in] Module to read metadata from
{
    ReadNamedMetadataArrayOfInt32(pModule, OptionsMetadataName, m_options);
    for (uint32_t stage = 0; stage != ShaderStageCompute + 1; ++stage)
    {
        std::string metadataName = (Twine(OptionsMetadataName) + "." +
                                    GetShaderStageAbbreviation(static_cast<ShaderStage>(stage))).str();
        auto pNamedMetaNode = pModule->getNamedMetadata(metadataName);
        if ((pNamedMetaNode == nullptr) || (pNamedMetaNode->getNumOperands() == 0))
        {
            continue;
        }
        m_shaderOptions.resize(stage + 1);
        ReadArrayOfInt32MetaNode(pNamedMetaNode->getOperand(0), m_shaderOptions[stage]);
    }
}

// =====================================================================================================================
// Set the resource nodes for the pipeline.
void PipelineState::SetUserDataNodes(
    ArrayRef<ResourceNode>   nodes)     // The resource nodes. Copied, so only need to remain valid for the
                                        // duration of this call.
{
    // Count how many entries in total and allocate the buffer.
    uint32_t nodeCount = nodes.size();
    for (auto& node : nodes)
    {
        if (node.type == ResourceNodeType::DescriptorTableVaPtr)
        {
            nodeCount += node.innerTable.size();
        }
    }
    assert(m_allocUserDataNodes == nullptr);
    m_allocUserDataNodes = std::make_unique<ResourceNode[]>(nodeCount);

    // Copy nodes in.
    ResourceNode* pDestTable = m_allocUserDataNodes.get();
    ResourceNode* pDestInnerTable = pDestTable + nodeCount;
    m_userDataNodes = ArrayRef<ResourceNode>(pDestTable, nodes.size());
    SetUserDataNodesTable(nodes, pDestTable, pDestInnerTable);
    assert(pDestInnerTable == pDestTable + nodes.size());
}

// =====================================================================================================================
// Set one user data table, and its inner tables.
void PipelineState::SetUserDataNodesTable(
    ArrayRef<ResourceNode>        nodes,              // The source resource nodes to copy
    ResourceNode*                 pDestTable,         // [out] Where to write nodes
    ResourceNode*&                pDestInnerTable)    // [in/out] End of space available for inner tables
{
    for (uint32_t idx = 0; idx != nodes.size(); ++idx)
    {
        auto& node = nodes[idx];
        auto& destNode = pDestTable[idx];

        // Copy the node.
        destNode = node;
        if (node.type == ResourceNodeType::DescriptorTableVaPtr)
        {
            // Process an inner table.
            pDestInnerTable -= node.innerTable.size();
            destNode.innerTable = ArrayRef<ResourceNode>(pDestInnerTable, node.innerTable.size());
            SetUserDataNodesTable(node.innerTable,
                                  pDestInnerTable,
                                  pDestInnerTable);
        }
        m_haveConvertingSampler |= (node.type == ResourceNodeType::DescriptorYCbCrSampler);
    }
}

// =====================================================================================================================
// Record user data nodes into IR metadata.
// Note that this takes a Module* instead of using m_pModule, because it can be called before pipeline linking.
void PipelineState::RecordUserDataNodes(
    Module* pModule)    // [in/out] Module to record the IR metadata in
{
    if (m_userDataNodes.empty())
    {
        if (auto pUserDataMetaNode = pModule->getNamedMetadata(UserDataMetadataName))
        {
            pModule->eraseNamedMetadata(pUserDataMetaNode);
        }
        return;
    }

    auto pUserDataMetaNode = pModule->getOrInsertNamedMetadata(UserDataMetadataName);
    pUserDataMetaNode->clearOperands();
    RecordUserDataTable(m_userDataNodes, pUserDataMetaNode);
}

// =====================================================================================================================
// Record one table of user data nodes into IR metadata, calling itself recursively for inner tables.
void PipelineState::RecordUserDataTable(
    ArrayRef<ResourceNode>  nodes,              // Table of user data nodes
    NamedMDNode*            pUserDataMetaNode)  // IR metadata node to record them into
{
    IRBuilder<> builder(GetContext());

    for (const ResourceNode& node : nodes)
    {
        SmallVector<Metadata*, 5> operands;
        assert(node.type < ResourceNodeType::Count);
        // Operand 0: type
        operands.push_back(GetResourceTypeName(node.type));
        // Operand 1: offsetInDwords
        operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.offsetInDwords)));
        // Operand 2: sizeInDwords
        operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.sizeInDwords)));

        switch (node.type)
        {
        case ResourceNodeType::DescriptorTableVaPtr:
            {
                // Operand 3: Node count in sub-table.
                operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.innerTable.size())));
                // Create the metadata node here.
                pUserDataMetaNode->addOperand(MDNode::get(GetContext(), operands));
                // Create nodes for the sub-table.
                RecordUserDataTable(node.innerTable, pUserDataMetaNode);
                continue;
            }
        case ResourceNodeType::IndirectUserDataVaPtr:
        case ResourceNodeType::StreamOutTableVaPtr:
            {
                // Operand 3: Size of the indirect data in dwords.
                operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.indirectSizeInDwords)));
                break;
            }
        default:
            {
                // Operand 3: set
                operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.set)));
                // Operand 4: binding
                operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.binding)));
                if (node.pImmutableValue != nullptr)
                {
                    // Operand 5 onwards: immutable descriptor constant.
                    // Writing the constant array directly does not seem to work, as it does not survive IR linking.
                    // Maybe it is a problem with the IR linker when metadata contains a non-ConstantData constant.
                    // So we write the individual ConstantInts instead.
                    // The descriptor is either a sampler (<4 x i32>) or converting sampler (<8 x i32>).
                    uint32_t SamplerDescriptorSize = 4;
                    if (node.type == ResourceNodeType::DescriptorYCbCrSampler)
                    {
                        SamplerDescriptorSize = 8;
                    }
                    uint32_t elemCount = node.pImmutableValue->getType()->getArrayNumElements();
                    for (uint32_t elemIdx = 0; elemIdx != elemCount; ++elemIdx)
                    {
                        Constant* pVectorValue = ConstantExpr::getExtractValue(node.pImmutableValue, elemIdx);
                        for (uint32_t compIdx = 0; compIdx != SamplerDescriptorSize; ++compIdx)
                        {
                            operands.push_back(ConstantAsMetadata::get(
                                                      ConstantExpr::getExtractElement(pVectorValue,
                                                                                      builder.getInt32(compIdx))));
                        }
                    }
                }
                break;
            }
        }

        // Create the metadata node.
        pUserDataMetaNode->addOperand(MDNode::get(GetContext(), operands));
    }
}

// =====================================================================================================================
// Read user data nodes for the pipeline from IR metadata
void PipelineState::ReadUserDataNodes(
    Module* pModule)  // [in] LLVM module
{
    // Find the named metadata node.
    auto pUserDataMetaNode = pModule->getNamedMetadata(UserDataMetadataName);
    if (pUserDataMetaNode == nullptr)
    {
        return;
    }

    // Prepare to read the resource nodes from the named MD node. We allocate a single buffer, with the
    // outer table at the start, and inner tables allocated from the end backwards.
    uint32_t totalNodeCount = pUserDataMetaNode->getNumOperands();
    m_allocUserDataNodes = std::make_unique<ResourceNode[]>(totalNodeCount);

    ResourceNode* pNextOuterNode = m_allocUserDataNodes.get();
    ResourceNode* pNextNode = pNextOuterNode;
    ResourceNode* pEndNextInnerTable = pNextOuterNode + totalNodeCount;
    ResourceNode* pEndThisInnerTable = nullptr;

    // Read the nodes.
    for (uint32_t nodeIndex = 0; nodeIndex < totalNodeCount; ++nodeIndex)
    {
        MDNode* pMetadataNode = pUserDataMetaNode->getOperand(nodeIndex);
        // Operand 0: node type
        pNextNode->type = GetResourceTypeFromName(cast<MDString>(pMetadataNode->getOperand(0)));
        // Operand 1: offsetInDwords
        pNextNode->offsetInDwords =
              mdconst::dyn_extract<ConstantInt>(pMetadataNode->getOperand(1))->getZExtValue();
        // Operand 2: sizeInDwords
        pNextNode->sizeInDwords =
              mdconst::dyn_extract<ConstantInt>(pMetadataNode->getOperand(2))->getZExtValue();

        if (pNextNode->type == ResourceNodeType::DescriptorTableVaPtr)
        {
            // Operand 3: number of nodes in inner table
            uint32_t innerNodeCount =
                  mdconst::dyn_extract<ConstantInt>(pMetadataNode->getOperand(3))->getZExtValue();
            // Go into inner table.
            assert(pEndThisInnerTable == nullptr);
            pEndThisInnerTable = pEndNextInnerTable;
            pEndNextInnerTable -= innerNodeCount;
            pNextNode = pEndNextInnerTable;
            pNextOuterNode->innerTable = ArrayRef<ResourceNode>(pNextNode, innerNodeCount);
            ++pNextOuterNode;
        }
        else
        {
            if ((pNextNode->type == ResourceNodeType::IndirectUserDataVaPtr) ||
                (pNextNode->type == ResourceNodeType::StreamOutTableVaPtr))
            {
                // Operand 3: Size of the indirect data in dwords
                pNextNode->indirectSizeInDwords =
                    mdconst::dyn_extract<ConstantInt>(pMetadataNode->getOperand(3))->getZExtValue();
            }
            else
            {
                // Operand 3: set
                pNextNode->set =
                    mdconst::dyn_extract<ConstantInt>(pMetadataNode->getOperand(3))->getZExtValue();
                // Operand 4: binding
                pNextNode->binding =
                    mdconst::dyn_extract<ConstantInt>(pMetadataNode->getOperand(4))->getZExtValue();
                pNextNode->pImmutableValue = nullptr;
                if (pMetadataNode->getNumOperands() >= 6)
                {
                    // Operand 5 onward: immutable descriptor constant
                    // The descriptor is either a sampler (<4 x i32>) or converting sampler (<8 x i32>).
                    static const uint32_t OperandStartIdx = 5;
                    uint32_t SamplerDescriptorSize = 4;
                    if (pNextNode->type == ResourceNodeType::DescriptorYCbCrSampler)
                    {
                        SamplerDescriptorSize = 8;
                        m_haveConvertingSampler = true;
                    }

                    uint32_t elemCount = (pMetadataNode->getNumOperands() - OperandStartIdx) / SamplerDescriptorSize;
                    SmallVector<Constant*, 8> descriptors;
                    for (uint32_t elemIdx = 0; elemIdx < elemCount; ++elemIdx)
                    {
                        SmallVector<Constant*, 8> compValues;
                        for (uint32_t compIdx = 0; compIdx < SamplerDescriptorSize; ++compIdx)
                        {
                            compValues.push_back(mdconst::dyn_extract<ConstantInt>(
                                  pMetadataNode->getOperand(
                                        OperandStartIdx + SamplerDescriptorSize * elemIdx + compIdx)));
                        }
                        descriptors.push_back(ConstantVector::get(compValues));
                    }
                    pNextNode->pImmutableValue = ConstantArray::get(ArrayType::get(descriptors[0]->getType(),
                                                                                   elemCount),
                                                                    descriptors);
                }
            }
            // Move on to next node to write in table.
            ++pNextNode;
            if (pEndThisInnerTable == nullptr)
            {
                pNextOuterNode = pNextNode;
            }
        }
        // See if we have reached the end of the inner table.
        if (pNextNode == pEndThisInnerTable)
        {
            pEndThisInnerTable = nullptr;
            pNextNode = pNextOuterNode;
        }
    }
    m_userDataNodes = ArrayRef<ResourceNode>(m_allocUserDataNodes.get(), pNextOuterNode);
}

// =====================================================================================================================
// Find the resource node for the given {set,binding}.
// For nodeType == Unknown, the function finds any node of the given set,binding.
// For nodeType == Resource, it matches Resource or CombinedTexture.
// For nodeType == Sampler, it matches Sampler or CombinedTexture.
// For other nodeType, only a node of the specified type is returned.
// Returns {topNode, node} where "node" is the found user data node, and "topNode" is the top-level user data
// node that contains it (or is equal to it).
std::pair<const ResourceNode*, const ResourceNode*> PipelineState::FindResourceNode(
    ResourceNodeType   nodeType,   // Type of the resource mapping node
    uint32_t           descSet,    // ID of descriptor set
    uint32_t           binding     // ID of descriptor binding
    ) const
{
    for (const ResourceNode& node : GetUserDataNodes())
    {
        if (node.type == ResourceNodeType::DescriptorTableVaPtr)
        {
            for (const ResourceNode& innerNode : node.innerTable)
            {
                if ((innerNode.set == descSet) && (innerNode.binding == binding))
                {
                    if ((nodeType == ResourceNodeType::Unknown) || (nodeType == innerNode.type) ||
                        (innerNode.type == ResourceNodeType::DescriptorCombinedTexture &&
                         (nodeType == ResourceNodeType::DescriptorResource ||
                          nodeType == ResourceNodeType::DescriptorTexelBuffer ||
                          nodeType == ResourceNodeType::DescriptorSampler)))
                    {
                        return { &node, &innerNode };
                    }
                }
            }
        }
        else if ((node.set == descSet) && (node.binding == binding))
        {
            if ((nodeType == ResourceNodeType::Unknown) || (nodeType == node.type) ||
                (node.type == ResourceNodeType::DescriptorCombinedTexture &&
                 (nodeType == ResourceNodeType::DescriptorResource ||
                  nodeType == ResourceNodeType::DescriptorTexelBuffer ||
                  nodeType == ResourceNodeType::DescriptorSampler)))
            {
                return { &node, &node };
            }
        }
    }
    return { nullptr, nullptr };
}

// =====================================================================================================================
// Get the cached MDString for the name of a resource mapping node type, as used in IR metadata for user data nodes.
MDString* PipelineState::GetResourceTypeName(
    ResourceNodeType type)   // Resource mapping node type
{
    return GetResourceTypeNames()[static_cast<uint32_t>(type)];
}

// =====================================================================================================================
// Get the resource mapping node type given its MDString name.
ResourceNodeType PipelineState::GetResourceTypeFromName(
    MDString* pTypeName)  // [in] Name of resource type as MDString
{
    auto typeNames = GetResourceTypeNames();
    for (uint32_t type = 0; ; ++type)
    {
        if (typeNames[type] == pTypeName)
        {
            return static_cast<ResourceNodeType>(type);
        }
    }
}

// =====================================================================================================================
// Get the array of cached MDStrings for names of resource mapping node type, as used in IR metadata for user
// data nodes.
ArrayRef<MDString*> PipelineState::GetResourceTypeNames()
{
    if (m_resourceNodeTypeNames[0] == nullptr)
    {
        for (uint32_t type = 0; type < static_cast<uint32_t>(ResourceNodeType::Count); ++type)
        {
            m_resourceNodeTypeNames[type] =
               MDString::get(GetContext(), GetResourceNodeTypeName(static_cast<ResourceNodeType>(type)));
        }
    }
    return ArrayRef<MDString*>(m_resourceNodeTypeNames);
}

// =====================================================================================================================
// Set vertex input descriptions. Each location referenced in a call to CreateReadGenericInput in the
// vertex shader must have a corresponding description provided here.
void PipelineState::SetVertexInputDescriptions(
    ArrayRef<VertexInputDescription>  inputs)   // Array of vertex input descriptions
{
    m_vertexInputDescriptions.clear();
    m_vertexInputDescriptions.insert(m_vertexInputDescriptions.end(), inputs.begin(), inputs.end());
}

// =====================================================================================================================
// Find vertex input description for the given location.
// Returns nullptr if location not found.
const VertexInputDescription* PipelineState::FindVertexInputDescription(
    uint32_t location    // Location
) const
{
    for (auto& inputDesc : m_vertexInputDescriptions)
    {
        if (inputDesc.location == location)
        {
            return &inputDesc;
        }
    }
    return nullptr;
}

// =====================================================================================================================
// Record vertex input descriptions into IR metadata.
void PipelineState::RecordVertexInputDescriptions(
    Module* pModule)    // [in/out] Module to record the IR metadata in
{
    if (m_vertexInputDescriptions.empty())
    {
        if (auto pVertexInputsMetaNode = pModule->getNamedMetadata(VertexInputsMetadataName))
        {
            pModule->eraseNamedMetadata(pVertexInputsMetaNode);
        }
        return;
    }

    auto pVertexInputsMetaNode = pModule->getOrInsertNamedMetadata(VertexInputsMetadataName);
    IRBuilder<> builder(GetContext());
    pVertexInputsMetaNode->clearOperands();

    for (const VertexInputDescription& input : m_vertexInputDescriptions)
    {
        pVertexInputsMetaNode->addOperand(GetArrayOfInt32MetaNode(GetContext(), input, /*atLeastOneValue=*/true));
    }
}

// =====================================================================================================================
// Read vertex input descriptions for the pipeline from IR metadata
void PipelineState::ReadVertexInputDescriptions(
    Module* pModule)    // [in] Module to read
{
    m_vertexInputDescriptions.clear();

    // Find the named metadata node.
    auto pVertexInputsMetaNode = pModule->getNamedMetadata(VertexInputsMetadataName);
    if (pVertexInputsMetaNode == nullptr)
    {
        return;
    }

    // Read the nodes.
    uint32_t nodeCount = pVertexInputsMetaNode->getNumOperands();
    for (uint32_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
    {
        m_vertexInputDescriptions.push_back({});
        ReadArrayOfInt32MetaNode(pVertexInputsMetaNode->getOperand(nodeIndex), m_vertexInputDescriptions.back());
    }
}

// =====================================================================================================================
// Set color export state.
void PipelineState::SetColorExportState(
    ArrayRef<ColorExportFormat> formats,      // Array of ColorExportFormat structs
    const ColorExportState&     exportState)  // [in] Color export flags
{
    m_colorExportFormats.clear();
    m_colorExportFormats.insert(m_colorExportFormats.end(), formats.begin(), formats.end());
    m_colorExportState = exportState;
}

// =====================================================================================================================
// Get format for one color export
const ColorExportFormat& PipelineState::GetColorExportFormat(
    uint32_t location)    // Export location
{
    if (location >= m_colorExportFormats.size())
    {
        static const ColorExportFormat emptyFormat = {};
        return emptyFormat;
    }
    return m_colorExportFormats[location];
}

// =====================================================================================================================
// Record color export state (including formats) into IR metadata
void PipelineState::RecordColorExportState(
    Module* pModule)  // [in/out] IR module
{
    if (m_colorExportFormats.empty())
    {
        if (auto pExportFormatsMetaNode = pModule->getNamedMetadata(ColorExportFormatsMetadataName))
        {
            pModule->eraseNamedMetadata(pExportFormatsMetaNode);
        }
    }
    else
    {
        auto pExportFormatsMetaNode = pModule->getOrInsertNamedMetadata(ColorExportFormatsMetadataName);
        IRBuilder<> builder(GetContext());
        pExportFormatsMetaNode->clearOperands();

        // The color export formats named metadata node's operands are:
        // - N metadata nodes for N color targets, each one containing
        // { dfmt, nfmt, blendEnable, blendSrcAlphaToColor }
        for (const ColorExportFormat& target : m_colorExportFormats)
        {
            pExportFormatsMetaNode->addOperand(GetArrayOfInt32MetaNode(GetContext(), target, /*atLeastOneValue=*/true));
        }
    }

    SetNamedMetadataToArrayOfInt32(pModule, m_colorExportState, ColorExportStateMetadataName);
}

// =====================================================================================================================
// Read color targets state from IR metadata
void PipelineState::ReadColorExportState(
    Module* pModule)  // [in] IR module
{
    m_colorExportFormats.clear();

    auto pExportFormatsMetaNode = pModule->getNamedMetadata(ColorExportFormatsMetadataName);
    if (pExportFormatsMetaNode != nullptr)
    {
        // Read the color target nodes.
        for (uint32_t nodeIndex = 0; nodeIndex < pExportFormatsMetaNode->getNumOperands(); ++nodeIndex)
        {
            m_colorExportFormats.push_back({});
            ReadArrayOfInt32MetaNode(pExportFormatsMetaNode->getOperand(nodeIndex), m_colorExportFormats.back());
        }
    }

    ReadNamedMetadataArrayOfInt32(pModule, ColorExportStateMetadataName, m_colorExportState);
}

// =====================================================================================================================
// Set graphics state (input-assembly, viewport, rasterizer).
void PipelineState::SetGraphicsState(
    const InputAssemblyState& iaState,    // [in] Input assembly state
    const ViewportState&      vpState,    // [in] Viewport state
    const RasterizerState&    rsState)    // [in] Rasterizer state
{
    m_inputAssemblyState = iaState;
    m_viewportState = vpState;
    m_rasterizerState = rsState;
}

// =====================================================================================================================
// Record device index into the IR metadata
void PipelineState::RecordDeviceIndex(
    Module* pModule)    // [in/out] IR module to record into
{
    SetNamedMetadataToArrayOfInt32(pModule, m_deviceIndex, DeviceIndexMetadataName);
}

// =====================================================================================================================
// Read device index from the IR metadata
void PipelineState::ReadDeviceIndex(
    Module* pModule)    // [in/out] IR module to read from
{
    ReadNamedMetadataArrayOfInt32(pModule, DeviceIndexMetadataName, m_deviceIndex);
}

// =====================================================================================================================
// Record graphics state (iastate, vpstate, rsstate) into the IR metadata
void PipelineState::RecordGraphicsState(
    Module* pModule)    // [in/out] IR module to record into
{
    SetNamedMetadataToArrayOfInt32(pModule, m_inputAssemblyState, IaStateMetadataName);
    SetNamedMetadataToArrayOfInt32(pModule, m_viewportState, VpStateMetadataName);
    SetNamedMetadataToArrayOfInt32(pModule, m_rasterizerState, RsStateMetadataName);
}

// =====================================================================================================================
// Read graphics state (device index, iastate, vpstate, rsstate) from the IR metadata
void PipelineState::ReadGraphicsState(
    Module* pModule)    // [in/out] IR module to read from
{
    ReadNamedMetadataArrayOfInt32(pModule, IaStateMetadataName, m_inputAssemblyState);
    ReadNamedMetadataArrayOfInt32(pModule, VpStateMetadataName, m_viewportState);
    ReadNamedMetadataArrayOfInt32(pModule, RsStateMetadataName, m_rasterizerState);
}

// =====================================================================================================================
// Determine whether to use off-chip tessellation mode
bool PipelineState::IsTessOffChip()
{
    // For GFX9+, always enable tessellation off-chip mode
    return EnableTessOffChip || (GetBuilderContext()->GetTargetInfo().GetGfxIpVersion().major >= 9);
}

// =====================================================================================================================
// Gets wave size for the specified shader stage
//
// NOTE: Need to be called after PatchResourceCollect pass, so usage of subgroupSize is confirmed.
uint32_t PipelineState::GetShaderWaveSize(
    ShaderStage stage)  // Shader stage
{
    if (stage == ShaderStageCopyShader)
    {
       // Treat copy shader as part of geometry shader
       stage = ShaderStageGeometry;
    }

    assert(stage <= ShaderStageCompute);

    uint32_t waveSize = GetTargetInfo().GetGpuProperty().waveSize;

    if (GetTargetInfo().GetGfxIpVersion().major >= 10)
    {
        // NOTE: GPU property wave size is used in shader, unless:
        //  1) A stage-specific default is preferred.
        //  2) If specified by tuning option, use the specified wave size.
        //  3) If gl_SubgroupSize is used in shader, use the specified subgroup size when required.

        if (stage == ShaderStageFragment)
        {
            // Per programming guide, it's recommended to use wave64 for fragment shader.
            waveSize = 64;
        }
        else if (HasShaderStage(ShaderStageGeometry))
        {
            // Legacy (non-NGG) hardware path for GS does not support wave32.
            waveSize = 64;
        }

        uint32_t waveSizeOption = GetShaderOptions(stage).waveSize;
        if (waveSizeOption != 0)
        {
            waveSize = waveSizeOption;
        }

        if ((stage == ShaderStageGeometry) && (HasShaderStage(ShaderStageGeometry) == false))
        {
            // NOTE: For NGG, GS could be absent and VS/TES acts as part of it in the merged shader.
            // In such cases, we check the property of VS or TES.
            if (HasShaderStage(ShaderStageTessEval))
            {
                return GetShaderWaveSize(ShaderStageTessEval);
            }
            return GetShaderWaveSize(ShaderStageVertex);
        }

        // If subgroup size is used in any shader in the pipeline, use the specified subgroup size as wave size.
        if (GetShaderModes()->GetAnyUseSubgroupSize())
        {
            uint32_t subgroupSize = GetShaderOptions(stage).subgroupSize;
            if (subgroupSize != 0)
            {
                waveSize = subgroupSize;
            }
        }

        assert((waveSize == 32) || (waveSize == 64));
    }

    return waveSize;
}

// =====================================================================================================================
// Gets resource usage of the specified shader stage
ResourceUsage* PipelineState::GetShaderResourceUsage(
    ShaderStage shaderStage)  // Shader stage
{
    if (shaderStage == ShaderStageCopyShader)
    {
        shaderStage = ShaderStageGeometry;
    }

    auto& resUsage = MutableArrayRef<std::unique_ptr<ResourceUsage>>(m_resourceUsage)[shaderStage];
    if (!resUsage)
    {
        resUsage.reset(new ResourceUsage);
        InitShaderResourceUsage(shaderStage, &*resUsage);
    }
    return &*resUsage;
}

// =====================================================================================================================
// Gets interface data of the specified shader stage
InterfaceData* PipelineState::GetShaderInterfaceData(
    ShaderStage shaderStage)  // Shader stage
{
    if (shaderStage == ShaderStageCopyShader)
    {
        shaderStage = ShaderStageGeometry;
    }

    auto& intfData = MutableArrayRef<std::unique_ptr<InterfaceData>>(m_interfaceData)[shaderStage];
    if (!intfData)
    {
        intfData.reset(new InterfaceData);
        InitShaderInterfaceData(&*intfData);
    }
    return &*intfData;
}

// =====================================================================================================================
// Initializes resource usage of the specified shader stage.
void PipelineState::InitShaderResourceUsage(
    ShaderStage    shaderStage,      // Shader stage
    ResourceUsage* pResUsage)        // [out] Resource usage
{
    memset(&pResUsage->builtInUsage, 0, sizeof(pResUsage->builtInUsage));

    pResUsage->pushConstSizeInBytes = 0;
    pResUsage->resourceWrite = false;
    pResUsage->resourceRead = false;
    pResUsage->perShaderTable = false;

    pResUsage->numSgprsAvailable = UINT32_MAX;
    pResUsage->numVgprsAvailable = UINT32_MAX;

    pResUsage->inOutUsage.inputMapLocCount = 0;
    pResUsage->inOutUsage.outputMapLocCount = 0;
    memset(pResUsage->inOutUsage.gs.outLocCount, 0, sizeof(pResUsage->inOutUsage.gs.outLocCount));
    pResUsage->inOutUsage.perPatchInputMapLocCount = 0;
    pResUsage->inOutUsage.perPatchOutputMapLocCount = 0;

    pResUsage->inOutUsage.expCount = 0;

    memset(pResUsage->inOutUsage.xfbStrides, 0, sizeof(pResUsage->inOutUsage.xfbStrides));
    pResUsage->inOutUsage.enableXfb = false;

    memset(pResUsage->inOutUsage.streamXfbBuffers, 0, sizeof(pResUsage->inOutUsage.streamXfbBuffers));

    if (shaderStage == ShaderStageVertex)
    {
        // NOTE: For vertex shader, PAL expects base vertex and base instance in user data,
        // even if they are not used in shader.
        pResUsage->builtInUsage.vs.baseVertex = true;
        pResUsage->builtInUsage.vs.baseInstance = true;
    }
    else if (shaderStage == ShaderStageTessControl)
    {
        auto& calcFactor = pResUsage->inOutUsage.tcs.calcFactor;

        calcFactor.inVertexStride           = InvalidValue;
        calcFactor.outVertexStride          = InvalidValue;
        calcFactor.patchCountPerThreadGroup = InvalidValue;
        calcFactor.offChip.outPatchStart    = InvalidValue;
        calcFactor.offChip.patchConstStart  = InvalidValue;
        calcFactor.onChip.outPatchStart     = InvalidValue;
        calcFactor.onChip.patchConstStart   = InvalidValue;
        calcFactor.outPatchSize             = InvalidValue;
        calcFactor.patchConstSize           = InvalidValue;
    }
    else if (shaderStage == ShaderStageGeometry)
    {
        pResUsage->inOutUsage.gs.rasterStream        = 0;

        auto& calcFactor = pResUsage->inOutUsage.gs.calcFactor;
        memset(&calcFactor, 0, sizeof(calcFactor));
    }
    else if (shaderStage == ShaderStageFragment)
    {
        for (uint32_t i = 0; i < MaxColorTargets; ++i)
        {
            pResUsage->inOutUsage.fs.expFmts[i] = EXP_FORMAT_ZERO;
            pResUsage->inOutUsage.fs.outputTypes[i] = BasicType::Unknown;
        }

        pResUsage->inOutUsage.fs.cbShaderMask = 0;
        pResUsage->inOutUsage.fs.dummyExport = true;
        pResUsage->inOutUsage.fs.isNullFs = false;
    }
}

// =====================================================================================================================
// Initializes interface data of the specified shader stage.
void PipelineState::InitShaderInterfaceData(
    InterfaceData* pIntfData)  // [out] Interface data
{
    pIntfData->userDataCount = 0;
    memset(pIntfData->userDataMap, InterfaceData::UserDataUnmapped, sizeof(pIntfData->userDataMap));

    memset(&pIntfData->pushConst, 0, sizeof(pIntfData->pushConst));
    pIntfData->pushConst.resNodeIdx = InvalidValue;

    memset(&pIntfData->spillTable, 0, sizeof(pIntfData->spillTable));
    pIntfData->spillTable.offsetInDwords = InvalidValue;

    memset(&pIntfData->userDataUsage, 0, sizeof(pIntfData->userDataUsage));

    memset(&pIntfData->entryArgIdxs, 0, sizeof(pIntfData->entryArgIdxs));
    pIntfData->entryArgIdxs.spillTable = InvalidValue;
}

// =====================================================================================================================
// Compute the ExportFormat (as an opaque int) of the specified color export location with the specified output
// type. Only the number of elements of the type is significant.
// This is not used in a normal compile; it is only used by amdllpc's -check-auto-layout-compatible option.
uint32_t PipelineState::ComputeExportFormat(
    Type*     pOutputTy,  // [in] Color output type
    uint32_t  location)   // Location
{
    std::unique_ptr<FragColorExport> fragColorExport(new FragColorExport(this, nullptr));
    return fragColorExport->ComputeExportFormat(pOutputTy, location);
}

// =====================================================================================================================
// Gets name string of the abbreviation for the specified shader stage
const char* PipelineState::GetShaderStageAbbreviation(
    ShaderStage shaderStage)  // Shader stage
{
    if (shaderStage == ShaderStageCopyShader)
    {
        return "COPY";
    }
    if (shaderStage > ShaderStageCompute)
    {
        return "Bad";
    }

    static const char* ShaderStageAbbrs[] = { "VS", "TCS", "TES", "GS", "FS", "CS" };
    return ShaderStageAbbrs[static_cast<uint32_t>(shaderStage)];
}

// =====================================================================================================================
// Helper macro
#define CASE_CLASSENUM_TO_STRING(TYPE, ENUM) \
    case TYPE::ENUM: pString = #ENUM; break;

// =====================================================================================================================
// Translate enum "ResourceNodeType" to string
const char* PipelineState::GetResourceNodeTypeName(
    ResourceNodeType type)  // Resource map node type
{
    const char* pString = nullptr;
    switch (type)
    {
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, Unknown)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorResource)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorSampler)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorYCbCrSampler)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorCombinedTexture)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorTexelBuffer)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorFmask)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorBuffer)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorTableVaPtr)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, IndirectUserDataVaPtr)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, PushConst)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorBufferCompact)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, StreamOutTableVaPtr)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorReserved12)
        break;
    default:
        llvm_unreachable("Should never be called!");
        break;
    }
    return pString;
}

// =====================================================================================================================
// Get (create if necessary) the PipelineState from this wrapper pass.
PipelineState* PipelineStateWrapper::GetPipelineState(
    Module* pModule)  // [in] IR module
{
    if (m_pPipelineState == nullptr)
    {
        m_allocatedPipelineState.reset(new PipelineState(m_pBuilderContext));
        m_pPipelineState = &*m_allocatedPipelineState;
        m_pPipelineState->ReadState(pModule);
    }
    return m_pPipelineState;
}

// =====================================================================================================================
// Pass to clear pipeline state out of the IR
class PipelineStateClearer : public ModulePass
{
public:
    PipelineStateClearer() : ModulePass(ID) {}

    void getAnalysisUsage(llvm::AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineStateWrapper>();
    }

    bool runOnModule(Module& module) override;

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass
};

char PipelineStateClearer::ID = 0;

// =====================================================================================================================
// Create pipeline state clearer pass
ModulePass* lgc::CreatePipelineStateClearer()
{
    return new PipelineStateClearer();
}

// =====================================================================================================================
// Run PipelineStateClearer pass to clear the pipeline state out of the IR
bool PipelineStateClearer::runOnModule(
    Module& module)   // [in/out] IR module
{
    auto pPipelineState = getAnalysis<PipelineStateWrapper>().GetPipelineState(&module);
    pPipelineState->Clear(&module);
    return true;
}

// =====================================================================================================================
// Initialize the pipeline state clearer pass
INITIALIZE_PASS(PipelineStateClearer, "llpc-pipeline-state-clearer", "LLPC pipeline state clearer", false, true)

// =====================================================================================================================
char PipelineStateWrapper::ID = 0;

// =====================================================================================================================
PipelineStateWrapper::PipelineStateWrapper(
    BuilderContext* pBuilderContext)  // [in] BuilderContext
    :
    ImmutablePass(ID),
    m_pBuilderContext(pBuilderContext)
{
}

// =====================================================================================================================
// Clean-up of PipelineStateWrapper at end of pass manager run
bool PipelineStateWrapper::doFinalization(
    Module& module)     // [in] Module
{
    return false;
}

// =====================================================================================================================
// Initialize the pipeline state wrapper pass
INITIALIZE_PASS(PipelineStateWrapper, DEBUG_TYPE, "LLPC pipeline state wrapper", false, true)
