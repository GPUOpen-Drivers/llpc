/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @brief LLPC source file: contains implementation of class Llpc::PipelineState.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-pipeline-state"

#include "llpc.h"
#include "llpcBuilderContext.h"
#include "llpcContext.h"
#include "llpcInternal.h"
#include "llpcPipelineState.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

using namespace Llpc;
using namespace llvm;

// Names for named metadata nodes when storing and reading back pipeline state
static const char* const UserDataMetadataName = "llpc.user.data.nodes";

// =====================================================================================================================
// Get LLVMContext
LLVMContext& PipelineState::GetContext() const
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
// Get GfxIpVersion
GfxIpVersion PipelineState::GetGfxIpVersion() const
{
    return GetTargetInfo().gfxIp;
}

// =====================================================================================================================
// Get GpuProperty
const GpuProperty* PipelineState::GetGpuProperty() const
{
    return &GetTargetInfo().gpuProperty;
}

// =====================================================================================================================
// Get GpuWorkarounds
const WorkaroundFlags* PipelineState::GetGpuWorkarounds() const
{
    return &GetTargetInfo().gpuWorkarounds;
}

// =====================================================================================================================
// Read shaderStageMask from IR. This consists of checking what shader stage functions are present in the IR.
void PipelineState::ReadShaderStageMask()
{
    m_stageMask = 0;
    for (auto& func : *m_pModule)
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
// Clear the pipeline state IR metadata.
void PipelineState::Clear(
    Module* pModule)    // [in/out] IR module
{
    m_userDataNodes = {};
    m_clientStateDirty = true;
    Flush(pModule);
}

// =====================================================================================================================
// Record dirty pipeline state into IR metadata of specified module. Returns true if module modified.
// Note that this takes a Module* instead of using m_pModule, because it can be called before pipeline linking.
bool PipelineState::Flush(
    Module* pModule)    // [in/out] Module to record the IR metadata in
{
    bool changed = false;
    if (m_clientStateDirty)
    {
        m_clientStateDirty = false;
        RecordUserDataNodes(pModule);
        changed = true;
    }
    return changed;
}

// =====================================================================================================================
// Set up the pipeline state from the pipeline IR module.
void PipelineState::ReadState()
{
    ReadShaderStageMask();
    ReadUserDataNodes();
}

// =====================================================================================================================
// Set the resource mapping nodes for the pipeline.
// The table entries are flattened and stored in IR metadata.
void PipelineState::SetUserDataNodes(
    ArrayRef<ResourceMappingNode>   nodes,            // The resource mapping nodes
    ArrayRef<DescriptorRangeValue>  rangeValues)      // The descriptor range values
{
    // Create a map of immutable nodes.
    ImmutableNodesMap immutableNodesMap;
    for (auto& rangeValue : rangeValues)
    {
        immutableNodesMap[{ rangeValue.set, rangeValue.binding }] = &rangeValue;
    }

    // Count how many user data nodes we have, and allocate the buffer.
    uint32_t nodeCount = nodes.size();
    for (auto& node : nodes)
    {
        if (node.type == ResourceMappingNodeType::DescriptorTableVaPtr)
        {
            nodeCount += node.tablePtr.nodeCount;
        }
    }
    LLPC_ASSERT(m_allocUserDataNodes == nullptr);
    m_allocUserDataNodes = std::make_unique<ResourceNode[]>(nodeCount);

    // Copy nodes in.
    ResourceNode* pDestTable = m_allocUserDataNodes.get();
    ResourceNode* pDestInnerTable = pDestTable + nodeCount;
    m_userDataNodes = ArrayRef<ResourceNode>(pDestTable, nodes.size());
    SetUserDataNodesTable(nodes, immutableNodesMap, pDestTable, pDestInnerTable);
    LLPC_ASSERT(pDestInnerTable == pDestTable + nodes.size());

    m_clientStateDirty = true;
}

// =====================================================================================================================
// Set one user data table, and its inner tables.
void PipelineState::SetUserDataNodesTable(
    ArrayRef<ResourceMappingNode> nodes,              // The resource mapping nodes
    const ImmutableNodesMap&      immutableNodesMap,  // [in] Map of immutable nodes
    ResourceNode*                 pDestTable,         // [out] Where to write nodes
    ResourceNode*&                pDestInnerTable)    // [in/out] End of space available for inner tables
{
    for (uint32_t idx = 0; idx != nodes.size(); ++idx)
    {
        auto& node = nodes[idx];
        auto& destNode = pDestTable[idx];

        destNode.type = node.type;
        destNode.sizeInDwords = node.sizeInDwords;
        destNode.offsetInDwords = node.offsetInDwords;

        switch (node.type)
        {
        case ResourceMappingNodeType::DescriptorTableVaPtr:
            {
                // Process an inner table.
                pDestInnerTable -= node.tablePtr.nodeCount;
                destNode.innerTable = ArrayRef<ResourceNode>(pDestInnerTable, node.tablePtr.nodeCount);
                SetUserDataNodesTable(ArrayRef<ResourceMappingNode>(node.tablePtr.pNext, node.tablePtr.nodeCount),
                                      immutableNodesMap,
                                      pDestInnerTable,
                                      pDestInnerTable);
                break;
            }
        case ResourceMappingNodeType::IndirectUserDataVaPtr:
        case ResourceMappingNodeType::StreamOutTableVaPtr:
            {
                // Process an indirect pointer.
                destNode.indirectSizeInDwords = node.userDataPtr.sizeInDwords;
                break;
            }
        default:
            {
                // Process an SRD.
                destNode.set = node.srdRange.set;
                destNode.binding = node.srdRange.binding;
                destNode.pImmutableValue = nullptr;

                auto it = immutableNodesMap.find(std::pair<uint32_t, uint32_t>(destNode.set, destNode.binding));
                if (it != immutableNodesMap.end())
                {
                    // This set/binding is (or contains) an immutable value. The value can only be a sampler, so we
                    // can assume it is four dwords.
                    auto& immutableNode = *it->second;

                    IRBuilder<> builder(GetContext());
                    SmallVector<Constant*, 4> values;

                    if (immutableNode.arraySize != 0)
                    {
                        for (uint32_t compIdx = 0; compIdx < immutableNode.arraySize; ++compIdx)
                        {
                            Constant* compValues[4] =
                            {
                                builder.getInt32(immutableNode.pValue[compIdx * 4]),
                                builder.getInt32(immutableNode.pValue[compIdx * 4 + 1]),
                                builder.getInt32(immutableNode.pValue[compIdx * 4 + 2]),
                                builder.getInt32(immutableNode.pValue[compIdx * 4 + 3])
                            };
                            values.push_back(ConstantVector::get(compValues));
                        }
                        destNode.pImmutableValue = ConstantArray::get(ArrayType::get(values[0]->getType(), values.size()),
                                                                      values);
                    }
                }
                break;
            }
        }
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
        LLPC_ASSERT(node.type < ResourceMappingNodeType::Count);
        // Operand 0: type
        operands.push_back(GetResourceTypeName(node.type));
        // Operand 1: offsetInDwords
        operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.offsetInDwords)));
        // Operand 2: sizeInDwords
        operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.sizeInDwords)));

        switch (node.type)
        {
        case ResourceMappingNodeType::DescriptorTableVaPtr:
            {
                // Operand 3: Node count in sub-table.
                operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.innerTable.size())));
                // Create the metadata node here.
                pUserDataMetaNode->addOperand(MDNode::get(GetContext(), operands));
                // Create nodes for the sub-table.
                RecordUserDataTable(node.innerTable, pUserDataMetaNode);
                continue;
            }
        case ResourceMappingNodeType::IndirectUserDataVaPtr:
        case ResourceMappingNodeType::StreamOutTableVaPtr:
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
                    // We can assume that the descriptor is <4 x i32> as an immutable descriptor is always a sampler.
                    static const uint32_t SamplerDescriptorSize = 4;
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
void PipelineState::ReadUserDataNodes()
{
    // Find the named metadata node.
    auto pUserDataMetaNode = m_pModule->getNamedMetadata(UserDataMetadataName);
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

        if (pNextNode->type == ResourceMappingNodeType::DescriptorTableVaPtr)
        {
            // Operand 3: number of nodes in inner table
            uint32_t innerNodeCount =
                  mdconst::dyn_extract<ConstantInt>(pMetadataNode->getOperand(3))->getZExtValue();
            // Go into inner table.
            LLPC_ASSERT(pEndThisInnerTable == nullptr);
            pEndThisInnerTable = pEndNextInnerTable;
            pEndNextInnerTable -= innerNodeCount;
            pNextNode = pEndNextInnerTable;
            pNextOuterNode->innerTable = ArrayRef<ResourceNode>(pNextNode, innerNodeCount);
            ++pNextOuterNode;
        }
        else
        {
            if ((pNextNode->type == ResourceMappingNodeType::IndirectUserDataVaPtr) ||
                (pNextNode->type == ResourceMappingNodeType::StreamOutTableVaPtr))
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
                    static const uint32_t SamplerDescriptorSize = 4;
                    static const uint32_t OperandStartIdx = 5;

                    uint32_t elemCount = (pMetadataNode->getNumOperands() - OperandStartIdx) / SamplerDescriptorSize;
                    SmallVector<Constant*, 4> descriptors;
                    for (uint32_t elemIdx = 0; elemIdx < elemCount; ++elemIdx)
                    {
                        Constant* compValues[SamplerDescriptorSize];
                        for (uint32_t compIdx = 0; compIdx < SamplerDescriptorSize; ++compIdx)
                        {
                            compValues[compIdx] = mdconst::dyn_extract<ConstantInt>(
                                  pMetadataNode->getOperand(OperandStartIdx + SamplerDescriptorSize * elemIdx + compIdx));
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
// Get the cached MDString for the name of a resource mapping node type, as used in IR metadata for user data nodes.
MDString* PipelineState::GetResourceTypeName(
    ResourceMappingNodeType type)   // Resource mapping node type
{
    return GetResourceTypeNames()[static_cast<uint32_t>(type)];
}

// =====================================================================================================================
// Get the resource mapping node type given its MDString name.
ResourceMappingNodeType PipelineState::GetResourceTypeFromName(
    MDString* pTypeName)  // [in] Name of resource type as MDString
{
    auto typeNames = GetResourceTypeNames();
    for (uint32_t type = 0; ; ++type)
    {
        if (typeNames[type] == pTypeName)
        {
            return static_cast<ResourceMappingNodeType>(type);
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
        for (uint32_t type = 0; type < static_cast<uint32_t>(ResourceMappingNodeType::Count); ++type)
        {
            m_resourceNodeTypeNames[type] =
               MDString::get(GetContext(), GetResourceMappingNodeTypeName(static_cast<ResourceMappingNodeType>(type)));
        }
    }
    return ArrayRef<MDString*>(m_resourceNodeTypeNames);
}

// =====================================================================================================================
// Get wave size for the specified shader stage
uint32_t PipelineState::GetShaderWaveSize(
    ShaderStage stage)  // Shader stage
{
    // TODO: Move the logic of GetShaderWaveSize into here. But first we need to pass the pipeline build info
    // into the middle-end in a clean way.
    return reinterpret_cast<Context*>(&m_pModule->getContext())->GetShaderWaveSize(stage, *GetGpuProperty());
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
ModulePass* Llpc::CreatePipelineStateClearer()
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
PipelineStateWrapper::PipelineStateWrapper()
    :
    ImmutablePass(ID)
{
    initializePipelineStateWrapperPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Clean-up of PipelineStateWrapper at end of pass manager run
bool PipelineStateWrapper::doFinalization(
    Module& module)     // [in] Module
{
    return GetPipelineState(&module)->Flush(&module);
}

// =====================================================================================================================
// Get the PipelineState from the wrapper pass.
PipelineState* PipelineStateWrapper::GetPipelineState(
    Module* pModule)   // [in] Module
{
    LLPC_ASSERT(m_pPipelineState->GetModule() == pModule);
    return &*m_pPipelineState;
}

// =====================================================================================================================
// Set the PipelineState. PipelineStateWrapper takes ownership of the PipelineState.
void PipelineStateWrapper::SetPipelineState(
    std::unique_ptr<PipelineState> pPipelineState)  // [in] PipelineState
{
    m_pPipelineState = std::move(pPipelineState);
}

// =====================================================================================================================
// Initialize the pipeline state wrapper pass
INITIALIZE_PASS(PipelineStateWrapper, DEBUG_TYPE, "LLPC pipeline state wrapper", false, true)

