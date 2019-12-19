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
#include "llpcShaderModes.h"
#include "palPipelineAbi.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
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

#if LLPC_BUILD_GFX10
// =====================================================================================================================
// Represents NGG (implicit primitive shader) control settings (valid for GFX10+)
struct NggControl : NggState
{
    bool                            passthroughMode; // Whether NGG passthrough mode is enabled
    Util::Abi::PrimShaderCbLayout   primShaderTable; // Primitive shader table (only some registers are used)
};
#endif

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

    // Get the embedded ShaderModes object
    ShaderModes* GetShaderModes() { return &m_shaderModes; }

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

    // Determine whether to use off-chip tessellation mode
    bool IsTessOffChip();

    // Set GS on-chip mode
    void SetGsOnChip(bool gsOnChip) { m_gsOnChip = gsOnChip; }

    // Checks whether GS on-chip mode is enabled
    // NOTE: GS on-chip mode has different meaning for GFX6~8 and GFX9: on GFX6~8, GS on-chip mode means ES -> GS ring
    // and GS -> VS ring are both on-chip; on GFX9, ES -> GS ring is always on-chip, GS on-chip mode means GS -> VS
    // ring is on-chip.
    bool IsGsOnChip() const { return m_gsOnChip; }

#if LLPC_BUILD_GFX10
    // Get NGG control settings
    NggControl* GetNggControl() { return &m_nggControl; }
#endif

    // -----------------------------------------------------------------------------------------------------------------
    // Utility method templates to read and write IR metadata, used by PipelineState and ShaderModes

    // Get a metadata node containing an array of i32 values, which can be read from any type.
    // The array is trimmed to remove trailing zero values. If the whole array would be 0, then this function
    // returns nullptr.
    template<typename T>
    static MDNode* GetArrayOfInt32MetaNode(
        LLVMContext&        context,          // [in] LLVM context
        const T&            value,            // [in] Value to write as array of i32
        bool                atLeastOneValue)  // True to generate node with one value even if all values are zero
    {
        IRBuilder<> builder(context);
        ArrayRef<uint32_t> values(reinterpret_cast<const uint32_t*>(&value), sizeof(value) / sizeof(uint32_t));

        while ((values.empty() == false) && (values.back() == 0))
        {
            if ((values.size() == 1) && atLeastOneValue)
            {
                break;
            }
            values = values.slice(0, values.size() - 1);
        }
        if (values.empty())
        {
            return nullptr;
        }

        SmallVector<Metadata*, 8> operands;
        for (uint32_t value : values)
        {
            operands.push_back(ConstantAsMetadata::get(builder.getInt32(value)));
        }
        return MDNode::get(context, operands);
    }

    // Set a named metadata node to point to an array of i32 values, which can be read from any type.
    // The array is trimmed to remove trailing zero values. If the whole array would be 0, then this function
    // removes the named metadata node (if it existed).
    template<typename T>
    static void SetNamedMetadataToArrayOfInt32(
        Module*             pModule,    // [in/out] IR module to record into
        const T&            value,      // [in] Value to write as array of i32
        StringRef           metaName)   // Name for named metadata node
    {
        MDNode* pArrayMetaNode = GetArrayOfInt32MetaNode(pModule->getContext(), value, false);
        if (pArrayMetaNode == nullptr)
        {
            if (auto pNamedMetaNode = pModule->getNamedMetadata(metaName))
            {
                pModule->eraseNamedMetadata(pNamedMetaNode);
            }
            return;
        }

        auto pNamedMetaNode = pModule->getOrInsertNamedMetadata(metaName);
        pNamedMetaNode->clearOperands();
        pNamedMetaNode->addOperand(pArrayMetaNode);
    }

    // Read an array of i32 values out of a metadata node, writing into any type.
    // Returns the number of i32s read.
    template<typename T>
    static uint32_t ReadArrayOfInt32MetaNode(
        MDNode*                   pMetaNode,  // Metadata node to read from
        T&                        value)      // [out] Value to write into (caller must zero initialize)
    {
        MutableArrayRef<uint32_t> values(reinterpret_cast<uint32_t*>(&value), sizeof(value) / sizeof(uint32_t));
        uint32_t count = std::min(pMetaNode->getNumOperands(), unsigned(values.size()));
        for (uint32_t index = 0; index < count; ++index)
        {
            values[index] = mdconst::dyn_extract<ConstantInt>(pMetaNode->getOperand(index))->getZExtValue();
        }
        return count;
    }

    // Read an array of i32 values out of a metadata node that is operand 0 of the named metadata node,
    // writing into any type.
    // Returns the number of i32s read.
    template<typename T>
    static uint32_t ReadNamedMetadataArrayOfInt32(
        Module*                   pModule,    // [in] IR module to look in
        StringRef                 metaName,   // Name for named metadata node
        T&                        value)      // [out] Value to write into (caller must zero initialize)
    {
        auto pNamedMetaNode = pModule->getNamedMetadata(metaName);
        if ((pNamedMetaNode == nullptr) || (pNamedMetaNode->getNumOperands() == 0))
        {
            return 0;
        }
        return ReadArrayOfInt32MetaNode(pNamedMetaNode->getOperand(0), value);
    }

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

    bool                            m_gsOnChip = false;                 // Whether to use GS on-chip mode
#if LLPC_BUILD_GFX10
    NggControl                      m_nggControl = {};                  // NGG control settings
#endif
    ShaderModes                     m_shaderModes;                      // Shader modes for this pipeline
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
