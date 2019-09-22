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

#include "llpcPipeline.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Pass.h"
#include <map>

namespace llvm
{

class MDString;
class NamedMDNode;
class PassRegistry;
class Timer;

void initializePipelineStateWrapperPass(PassRegistry&);
void initializePipelineStateClearerPass(PassRegistry&);

} // llvm

namespace Llpc
{

using namespace llvm;

ModulePass* CreatePipelineStateClearer();

// =====================================================================================================================
// The representation of a user data resource node in PipelineState
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
            Constant*                       pImmutableValue;
        };

        // Info for DescriptorTableVaPtr
        ArrayRef<ResourceNode>              innerTable;

        // Info for indirect data nodes (IndirectUserDataVaPtr, StreamOutVaTablePtr)
        uint32_t                            indirectSizeInDwords;
    };
};

// =====================================================================================================================
// The middle-end implementation of PipelineState, a subclass of Pipeline.
class PipelineState : public Pipeline
{
public:
    PipelineState(BuilderContext* pBuilderContext)
        : Pipeline(pBuilderContext)
    {}

    ~PipelineState() override final {}

    // -----------------------------------------------------------------------------------------------------------------
    // Implementations of Pipeline methods exposed to the front-end

    // Set the resource mapping nodes for the pipeline
    void SetUserDataNodes(ArrayRef<ResourceMappingNode>   nodes,
                          ArrayRef<DescriptorRangeValue>  rangeValues) override final;

    // Link the individual shader modules into a single pipeline module
    Module* Link(ArrayRef<Module*> modules) override final;

    // Generate pipeline module
    void Generate(std::unique_ptr<Module>   pipelineModule,
                  raw_pwrite_stream&        outStream,
                  CheckShaderCacheFunc      checkShaderCacheFunc,
                  ArrayRef<Timer*>          timers) override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Other methods

    // Clear the pipeline state IR metadata.
    void Clear(Module* pModule);

    // Record pipeline state into IR metadata of specified module.
    void Record(Module* pModule);

    // Set up the pipeline state from the pipeline module.
    void ReadState(Module* pModule);

    // Get user data nodes
    ArrayRef<ResourceNode> GetUserDataNodes() const { return m_userDataNodes; }

    // Set "no replayer" flag, saying that this pipeline is being compiled with a BuilderImpl so does not
    // need a BuilderReplayer pass.
    void SetNoReplayer() { m_noReplayer = true; }

private:
    // Type of immutable nodes map used in SetUserDataNodes
    typedef std::map<std::pair<uint32_t, uint32_t>, const DescriptorRangeValue*> ImmutableNodesMap;

    // User data nodes handling
    void SetUserDataNodesTable(ArrayRef<ResourceMappingNode>        nodes,
                               const ImmutableNodesMap&             immutableNodesMap,
                               ResourceNode*                        pDestTable,
                               ResourceNode*&                       pDestInnerTable);
    void RecordUserDataNodes(Module* pModule);
    void RecordUserDataTable(ArrayRef<ResourceNode> nodes, NamedMDNode* pUserDataMetaNode);
    void ReadUserDataNodes(Module* pModule);
    ArrayRef<MDString*> GetResourceTypeNames();
    MDString* GetResourceTypeName(ResourceMappingNodeType type);
    ResourceMappingNodeType GetResourceTypeFromName(MDString* pTypeName);

    // -----------------------------------------------------------------------------------------------------------------
    bool                            m_noReplayer = false;               // True if no BuilderReplayer needed
    std::unique_ptr<ResourceNode[]> m_allocUserDataNodes;               // Allocated buffer for user data
    ArrayRef<ResourceNode>          m_userDataNodes;                    // Top-level user data node table
    MDString*                       m_resourceNodeTypeNames[uint32_t(ResourceMappingNodeType::Count)] = {};
                                                                        // Cached MDString for each resource node type
};

// =====================================================================================================================
// Wrapper pass for the pipeline state in the middle-end
class PipelineStateWrapper : public ImmutablePass
{
public:
    PipelineStateWrapper(BuilderContext* pBuilderContext = nullptr);

    bool doFinalization(Module& module) override;

    // Get (create if necessary) the PipelineState from this wrapper pass.
    PipelineState* GetPipelineState(Module* pModule);

    // Set the PipelineState.
    void SetPipelineState(PipelineState* pPipelineState) { m_pPipelineState = pPipelineState; }

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    BuilderContext*                     m_pBuilderContext = nullptr;  // BuilderContext for allocating PipelineState
    PipelineState*                  m_pPipelineState = nullptr;   // Cached pipeline state
    std::unique_ptr<PipelineState>  m_allocatedPipelineState;     // Pipeline state allocated by this pass
};

} // Llpc
