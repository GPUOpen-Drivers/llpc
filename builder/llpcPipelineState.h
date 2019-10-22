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
#include "llpcBuilder.h"

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
void initializePipelineStateClearerPass(PassRegistry&);

} // llvm

namespace Llpc
{

using namespace llvm;

class BuilderContext;
struct GpuProperty;
class PipelineState;
struct TargetInfo;
struct WorkaroundFlags;

ModulePass* CreatePipelineStateClearer();

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
            Constant*                       pImmutableValue;
        };

        // Info for DescriptorTableVaPtr
        ArrayRef<ResourceNode>              innerTable;

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
        : m_pBuilderContext(nullptr)
    {}

    PipelineState(BuilderContext* pBuilderContext)
        : m_pBuilderContext(pBuilderContext)
    {}

    // Get BuilderContext
    BuilderContext* GetBuilderContext() const { return m_pBuilderContext; }

    // Get LLVMContext
    LLVMContext& GetContext() const;

    // Accessors for pipeline module that this pipeline state is for.
    void SetModule(Module* pModule) { m_pModule = pModule; }
    Module* GetModule() const { return m_pModule; }

    // Accessors for TargetInfo and its contents
    const TargetInfo& GetTargetInfo() const;
    GfxIpVersion GetGfxIpVersion() const;
    const GpuProperty* GetGpuProperty() const;
    const WorkaroundFlags* GetGpuWorkarounds() const;

    // Clear the pipeline state IR metadata.
    void Clear(Module* pModule);

    // Accessors for shader stage mask
    void SetShaderStageMask(uint32_t mask) { m_stageMask = mask; }
    uint32_t GetShaderStageMask() const { return m_stageMask; }
    bool HasShaderStage(ShaderStage stage) const { return (GetShaderStageMask() >> stage) & 1; }
    bool IsGraphics() const;

    // Record dirty pipeline state into IR metadata of specified module. Returns true if module modified.
    bool Flush(Module* pModule);

    // Set up the pipeline state from the pipeline module.
    void ReadState();

    // Accessors for user data nodes
    void SetUserDataNodes(ArrayRef<ResourceMappingNode>   nodes,
                          ArrayRef<DescriptorRangeValue>  rangeValues);
    ArrayRef<ResourceNode> GetUserDataNodes() const { return m_userDataNodes; }

    // Get wave size for the specified shader stage
    uint32_t GetShaderWaveSize(ShaderStage stage);

    // Accessors for device index
    void SetDeviceIndex(uint32_t deviceIndex);
    uint32_t GetDeviceIndex() const;

    // Accessors for input-assembly state.
    void SetInputAssemblyState(const Builder::InputAssemblyState& iaState);
    const Builder::InputAssemblyState& GetInputAssemblyState();

    // Accessors for viewport state.
    void SetViewportState(const Builder::ViewportState& vpState);
    const Builder::ViewportState& GetViewportState();

    // Accessors for rasterizer state.
    void SetRasterizerState(const Builder::RasterizerState& vpState);
    const Builder::RasterizerState& GetRasterizerState();

private:
    // Type of immutable nodes map used in SetUserDataNodes
    typedef std::map<std::pair<uint32_t, uint32_t>, const DescriptorRangeValue*> ImmutableNodesMap;

    // Read shaderStageMask from IR
    void ReadShaderStageMask();

    // User data nodes handling
    void SetUserDataNodesTable(ArrayRef<ResourceMappingNode>        nodes,
                               const ImmutableNodesMap&             immutableNodesMap,
                               ResourceNode*                        pDestTable,
                               ResourceNode*&                       pDestInnerTable);
    void RecordUserDataNodes(Module* pModule);
    void RecordUserDataTable(ArrayRef<ResourceNode> nodes, NamedMDNode* pUserDataMetaNode);
    void ReadUserDataNodes();
    ArrayRef<MDString*> GetResourceTypeNames();
    MDString* GetResourceTypeName(ResourceMappingNodeType type);
    ResourceMappingNodeType GetResourceTypeFromName(MDString* pTypeName);

    // Device index handling
    void RecordDeviceIndex(Module* pModule);
    void ReadDeviceIndex();

    // Input-assembly state handling
    void RecordInputAssemblyState(Module* pModule);
    void ReadInputAssemblyState();

    // Viewport state handling
    void RecordViewportState(Module* pModule);
    void ReadViewportState();

    // Rasterizer state handling
    void RecordRasterizerState(Module* pModule);
    void ReadRasterizerState();

    // Utility functions to record and read an array of i32 values in metadata
    void SetNamedMetadataToArrayOfInt32(Module* pModule, ArrayRef<uint32_t> values, StringRef metaName);
    MDNode* GetArrayOfInt32MetaNode(ArrayRef<uint32_t> values, bool atLeastOneValue);
    uint32_t ReadNamedMetadataArrayOfInt32(StringRef metaName, MutableArrayRef<uint32_t> values);
    uint32_t ReadArrayOfInt32MetaNode(MDNode* pMetaNode, MutableArrayRef<uint32_t> values);

    // -----------------------------------------------------------------------------------------------------------------
    BuilderContext*                 m_pBuilderContext;                  // Builder context
    Module*                         m_pModule = nullptr;                // Pipeline IR module
    uint32_t                        m_stageMask = 0;                    // Mask of active shader stages
    std::unique_ptr<ResourceNode[]> m_allocUserDataNodes;               // Allocated buffer for user data
    ArrayRef<ResourceNode>          m_userDataNodes;                    // Top-level user data node table
    MDString*                       m_resourceNodeTypeNames[uint32_t(ResourceMappingNodeType::Count)] = {};
                                                                        // Cached MDString for each resource node type
    uint32_t                        m_deviceIndex = 0;                  // Device index
    Builder::InputAssemblyState     m_inputAssemblyState = {};          // Input-assembly state
    Builder::ViewportState          m_viewportState = {};               // Viewport state
    Builder::RasterizerState        m_rasterizerState = {};             // Rasterizer state
    bool                            m_clientStateDirty = false;         // Whether state provided by builder client
                                                                        //  (user data, vertex inputs, options) is dirty
                                                                        //  and needs writing to IR
};

// =====================================================================================================================
// Wrapper pass for the pipeline state in the middle-end
class PipelineStateWrapper : public ImmutablePass
{
public:
    PipelineStateWrapper();

    bool doFinalization(Module& module) override;

    // Get the PipelineState from this wrapper pass.
    PipelineState* GetPipelineState(Module* pModule);

    // Set the PipelineState. PipelineStateWrapper takes ownership of the PipelineState.
    void SetPipelineState(std::unique_ptr<PipelineState> pPipelineState);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    std::unique_ptr<PipelineState> m_pPipelineState;  // Cached pipeline state
};

} // Llpc
