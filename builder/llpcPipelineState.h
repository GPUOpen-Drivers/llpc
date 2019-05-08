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
 * @file  llpcPipelineState.h
 * @brief LLPC header file: contains declaration of class Llpc::PipelineState
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include <map>

namespace llvm
{

class LLVMContext;
class Module;
class MDString;
class NamedMDNode;
class PassRegistry;

void initializePipelineStateWrapperPass(PassRegistry&);

} // llvm

namespace Llpc
{

class PipelineState;

// =====================================================================================================================
// The representation of a user data resource node in builder and patching
struct ResourceNode
{
    ResourceNode() {}

    ResourceMappingNodeType                 type;
    uint32_t                                sizeInDwords;
    uint32_t                                offsetInDwords;

    union
    {
        // Info for generic descriptor nodes.
        struct
        {
            uint32_t                        set;
            uint32_t                        binding;
            llvm::Constant*                 pImmutableValue;
        };

        // Info for DescriptorTableVaPtr
        llvm::ArrayRef<ResourceNode>        innerTable;

        // Info for indirect data nodes (IndirectUserDataVaPtr, StreamOutVaTablePtr)
        uint32_t                            indirectSizeInDwords;
    };
};

// =====================================================================================================================
// The pipeline state in the middle-end (Builder and Patch)
class PipelineState
{
public:
    PipelineState()
        : m_pContext(nullptr)
    {}

    PipelineState(llvm::LLVMContext* pContext)
        : m_pContext(pContext)
    {}

    // Set the resource mapping nodes for the pipeline.
    void SetUserDataNodes(llvm::ArrayRef<ResourceMappingNode>   nodes,
                          llvm::ArrayRef<DescriptorRangeValue>  rangeValues);

    // Record pipeline state into IR metadata.
    void RecordState(llvm::Module* pModule);

    // Set up the pipeline state from the specified linked IR module.
    void ReadStateFromModule(llvm::Module* pModule);

    // Get user data nodes
    llvm::ArrayRef<ResourceNode> GetUserDataNodes() const { return m_userDataNodes; }

private:
    // Type of immutable nodes map used in SetUserDataNodes
    typedef std::map<std::pair<uint32_t, uint32_t>, const DescriptorRangeValue*> ImmutableNodesMap;

    void SetUserDataNodesTable(llvm::ArrayRef<ResourceMappingNode>  nodes,
                               const ImmutableNodesMap&             immutableNodesMap,
                               ResourceNode*                        pDestTable,
                               ResourceNode*&                       pDestInnerTable);
    void RecordUserDataNodes(llvm::Module* pModule);
    void RecordUserDataTable(llvm::ArrayRef<ResourceNode> nodes, llvm::NamedMDNode* pUserDataMetaNode);

    // Read user data nodes for each shader stage from IR metadata
    void ReadUserDataNodes(llvm::Module* pModule);

    // Get the array of cached MDStrings for names of resource mapping node type, as used in IR metadata for user
    // data nodes.
    llvm::ArrayRef<llvm::MDString*> GetResourceTypeNames();

    // Get the cached MDString for the name of a resource mapping node type, as used in IR metadata for user data nodes.
    llvm::MDString* GetResourceTypeName(ResourceMappingNodeType type);

    // Get the resource mapping node type given its MDString name.
    ResourceMappingNodeType GetResourceTypeFromName(llvm::MDString* pTypeName);

    // -----------------------------------------------------------------------------------------------------------------
    llvm::LLVMContext*              m_pContext;                         // LLVM context
    std::unique_ptr<ResourceNode[]> m_allocUserDataNodes;               // Allocated buffer for user data
    llvm::ArrayRef<ResourceNode>    m_userDataNodes;                    // Top-level user data node table
    llvm::MDString*                 m_resourceNodeTypeNames[uint32_t(ResourceMappingNodeType::Count)] = {};
                                                                        // Cached MDString for each resource node type
};

// =====================================================================================================================
// Wrapper pass for the pipeline state in the middle-end
class PipelineStateWrapper : public llvm::ImmutablePass
{
public:
    PipelineStateWrapper();

    bool doFinalization(llvm::Module& module) override;

    // Get the PipelineState from this wrapper pass.
    PipelineState* GetPipelineState(llvm::Module* pModule);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    PipelineState* m_pPipelineState = nullptr;  // Cached pipeline state
};

} // Llpc
