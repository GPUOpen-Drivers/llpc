/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PipelineState.h
 * @brief LLPC header file: contains declaration of class lgc::PipelineState
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/Pipeline.h"
#include "lgc/state/Abi.h"
#include "lgc/state/Defs.h"
#include "lgc/state/ResourceUsage.h"
#include "lgc/state/ShaderModes.h"
#include "lgc/state/ShaderStage.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Pass.h"
#include <map>

namespace llvm {

class MDString;
class ModulePass;
class NamedMDNode;
class PassRegistry;
class Timer;

void initializePipelineShadersPass(PassRegistry &);
void initializePipelineStateClearerPass(PassRegistry &);
void initializePipelineStateWrapperPass(PassRegistry &);

} // namespace llvm

namespace lgc {

class ElfLinker;
class PalMetadata;
class PipelineState;
class TargetInfo;

llvm::ModulePass *createPipelineStateClearer();

// Initialize passes in state directory
//
// @param passRegistry : Pass registry
inline static void initializeStatePasses(llvm::PassRegistry &passRegistry) {
  initializePipelineShadersPass(passRegistry);
  initializePipelineStateClearerPass(passRegistry);
  initializePipelineStateWrapperPass(passRegistry);
}

// =====================================================================================================================
// Represents NGG (implicit primitive shader) control settings (valid for GFX10+)

// Enumerates compaction modes after culling operations for NGG primitive shader.
enum NggCompactMode : unsigned {
  NggCompactDisable,  // Compaction is disabled
  NggCompactVertices, // Compaction is based on vertices
};

// Represents NGG tuning options
struct NggControl {
  bool enableNgg;                // Enable NGG mode, use an implicit primitive shader
  bool enableGsUse;              // Enable NGG use on geometry shader
  bool alwaysUsePrimShaderTable; // Always use primitive shader table to fetch culling-control registers
  NggCompactMode compactMode;    // Compaction mode after culling operations

  bool enableVertexReuse;         // Enable optimization to cull duplicate vertices
  bool enableBackfaceCulling;     // Enable culling of primitives that don't meet facing criteria
  bool enableFrustumCulling;      // Enable discarding of primitives outside of view frustum
  bool enableBoxFilterCulling;    // Enable simpler frustum culler that is less accurate
  bool enableSphereCulling;       // Enable frustum culling based on a sphere
  bool enableSmallPrimFilter;     // Enable trivial sub-sample primitive culling
  bool enableCullDistanceCulling; // Enable culling when "cull distance" exports are present

  /// Following fields are used for NGG tuning
  unsigned backfaceExponent; // Value from 1 to UINT32_MAX that will cause the backface culling
                             // algorithm to ignore area calculations that are less than
                             // (10 ^ -(backfaceExponent)) / abs(w0 * w1 * w2)
                             // Only valid if the NGG backface culler is enabled.
                             // A value of 0 will disable the threshold.

  NggSubgroupSizing subgroupSizing; // NGG sub-group sizing type

  unsigned primsPerSubgroup; // Preferred number of GS primitives to pack into a primitive shader
                             // sub-group

  unsigned vertsPerSubgroup; // Preferred number of vertices consumed by a primitive shader sub-group

  bool passthroughMode;                          // Whether NGG passthrough mode is enabled
  Util::Abi::PrimShaderCbLayout primShaderTable; // Primitive shader table (only some registers are used)
};

// =====================================================================================================================
// The middle-end implementation of PipelineState, a subclass of Pipeline.
class PipelineState final : public Pipeline {
public:
  PipelineState(LgcContext *builderContext, bool emitLgc = false) : Pipeline(builderContext), m_emitLgc(emitLgc) {}

  ~PipelineState() override final;

  // -----------------------------------------------------------------------------------------------------------------
  // Implementations of Pipeline methods exposed to the front-end

  // Set the resource mapping nodes for the pipeline
  void setUserDataNodes(llvm::ArrayRef<ResourceNode> nodes) override final;

  // Set shader stage mask
  void setShaderStageMask(unsigned mask) override final { m_stageMask = mask; }

  // Set client name
  void setClient(llvm::StringRef client) override final { m_client = client.str(); }

  // Set and get per-pipeline options
  void setOptions(const Options &options) override final { m_options = options; }
  const Options &getOptions() const override final { return m_options; }

  // Set per-shader options
  void setShaderOptions(ShaderStage stage, const ShaderOptions &options) override final;

  // Set device index
  void setDeviceIndex(unsigned deviceIndex) override final { m_deviceIndex = deviceIndex; }

  // Set vertex input descriptions
  void setVertexInputDescriptions(llvm::ArrayRef<VertexInputDescription> inputs) override final;

  // Set color export state
  void setColorExportState(llvm::ArrayRef<ColorExportFormat> formats,
                           const ColorExportState &exportState) override final;

  // Set graphics state (input-assembly, viewport, rasterizer).
  void setGraphicsState(const InputAssemblyState &iaState, const RasterizerState &rsState) override final;

  // Set the finalized 128-bit cache hash that is used to find this pipeline in the cache for the given version of LLPC.
  void set128BitCacheHash(const Hash128 &finalizedCacheHash, const llvm::VersionTuple &version) override final;

  // Link the individual shader IR modules into a single pipeline module
  llvm::Module *irLink(llvm::ArrayRef<llvm::Module *> modules, PipelineLink pipelineLink) override final;

  // Generate pipeline module
  bool generate(std::unique_ptr<llvm::Module> pipelineModule, llvm::raw_pwrite_stream &outStream,
                CheckShaderCacheFunc checkShaderCacheFunc, llvm::ArrayRef<llvm::Timer *> timers) override final;

  // Create an ELF linker object for linking unlinked shader/part-pipeline ELFs into a pipeline ELF using the
  // pipeline state
  ElfLinker *createElfLinker(llvm::ArrayRef<llvm::MemoryBufferRef> elfs) override final;

  // Do an early check for ability to use unlinked shader compilation then ELF linking.
  bool checkElfLinkable() override final;

  // Get a textual error message for the last recoverable error
  llvm::StringRef getLastError() override final;

  // Compute the ExportFormat (as an opaque int) of the specified color export location with the specified output
  // type. Only the number of elements of the type is significant.
  unsigned computeExportFormat(llvm::Type *outputTy, unsigned location) override final;

  // Set entire pipeline state from metadata in an IR module. This is used by the lgc command-line utility
  // for its link option.
  void setStateFromModule(llvm::Module *module) override final { readState(module); }

  // Set the "other part-pipeline" from the given other Pipeline object. This is used when doing a part-pipeline
  // compile of the non-FS part of the pipeline, to inherit required information from the FS part-pipeline.
  void setOtherPartPipeline(Pipeline &otherPartPipeline, llvm::Module *linkedModule = nullptr) override final;

  // -----------------------------------------------------------------------------------------------------------------
  // Other methods

  // Get the embedded ShaderModes object
  ShaderModes *getShaderModes() { return &m_shaderModes; }

  // Accessors for context information
  const TargetInfo &getTargetInfo() const;
  unsigned getPalAbiVersion() const;

  // Return whether we're generating a whole pipeline.
  bool isWholePipeline() const { return m_pipelineLink == PipelineLink::WholePipeline; }

  // Return whether we're generating a part-pipeline.
  bool isPartPipeline() const { return m_pipelineLink == PipelineLink::PartPipeline; }

  // Return whether we're generating an indepedent unlinked shader (not in the part-pipeline scheme).
  bool isUnlinked() const { return m_pipelineLink == PipelineLink::Unlinked; }

  // Clear the pipeline state IR metadata.
  void clear(llvm::Module *module);

  // Record pipeline state into IR metadata of specified module.
  void record(llvm::Module *module);

  // Accessors for shader stage mask
  unsigned getShaderStageMask() const { return m_stageMask; }
  bool hasShaderStage(ShaderStage stage) const { return (getShaderStageMask() >> stage) & 1; }
  bool isGraphics() const;
  bool isComputeLibrary() const { return m_computeLibrary; }
  ShaderStage getLastVertexProcessingStage() const;
  ShaderStage getPrevShaderStage(ShaderStage shaderStage) const;
  ShaderStage getNextShaderStage(ShaderStage shaderStage, bool fakeFs = false) const;

  // Get client name
  const char *getClient() const { return m_client.c_str(); }

  // Get per-shader options
  const ShaderOptions &getShaderOptions(ShaderStage stage);

  // Set up the pipeline state from the pipeline module.
  void readState(llvm::Module *module);

  // Get user data nodes
  llvm::ArrayRef<ResourceNode> getUserDataNodes() const { return m_userDataNodes; }

  // Find the push constant resource node
  const ResourceNode *findPushConstantResourceNode() const;

  // Find the resource node for the given set,binding
  std::pair<const ResourceNode *, const ResourceNode *> findResourceNode(ResourceNodeType nodeType, unsigned descSet,
                                                                         unsigned binding) const;

  // Find the single root resource node of the given type
  const ResourceNode *findSingleRootResourceNode(ResourceNodeType nodeType) const;

  // Set "no replayer" flag, saying that this pipeline is being compiled with a BuilderImpl so does not
  // need a BuilderReplayer pass.
  void setNoReplayer() { m_noReplayer = true; }

  // Accessors for vertex input descriptions.
  llvm::ArrayRef<VertexInputDescription> getVertexInputDescriptions() const { return m_vertexInputDescriptions; }
  const VertexInputDescription *findVertexInputDescription(unsigned location) const;

  // Accessors for color export state
  const ColorExportFormat &getColorExportFormat(unsigned location);
  const bool hasColorExportFormats() { return !m_colorExportFormats.empty(); }
  const ColorExportState &getColorExportState() { return m_colorExportState; }

  // Accessors for pipeline state
  unsigned getDeviceIndex() const { return m_deviceIndex; }
  const InputAssemblyState &getInputAssemblyState() const { return m_inputAssemblyState; }
  const RasterizerState &getRasterizerState() const { return m_rasterizerState; }

  // Determine whether to use off-chip tessellation mode
  bool isTessOffChip();

  // Set GS on-chip mode
  void setGsOnChip(bool gsOnChip) { m_gsOnChip = gsOnChip; }

  // Checks whether GS on-chip mode is enabled
  // NOTE: GS on-chip mode has different meaning for GFX6~8 and GFX9: on GFX6~8, GS on-chip mode means ES -> GS ring
  // and GS -> VS ring are both on-chip; on GFX9, ES -> GS ring is always on-chip, GS on-chip mode means GS -> VS
  // ring is on-chip.
  bool isGsOnChip() const { return m_gsOnChip; }

  // Gets wave size for the specified shader stage
  unsigned getShaderWaveSize(ShaderStage stage);

  // Set the default wave size for the specified shader stage
  void setShaderDefaultWaveSize(ShaderStage stage);

  // Set the wave size for the specified shader stage
  void setShaderWaveSize(ShaderStage stage, unsigned waveSize) {
    assert(waveSize == 32 || waveSize == 64);
    m_waveSize[stage] = waveSize;
  }

  // Get NGG control settings
  NggControl *getNggControl() { return &m_nggControl; }

  // Gets resource usage of the specified shader stage
  ResourceUsage *getShaderResourceUsage(ShaderStage shaderStage);

  // Gets interface data of the specified shader stage
  InterfaceData *getShaderInterfaceData(ShaderStage shaderStage);

  // Accessor for PAL metadata
  PalMetadata *getPalMetadata();

  // Clear PAL metadata object from PipelineState
  void clearPalMetadata();

  // Merge blob of MsgPack data into existing PAL metadata
  void mergePalMetadataFromBlob(llvm::StringRef blob, bool isGlueCode);

  // Set error message to be returned to the client by it calling getLastError
  void setError(const llvm::Twine &message);

  // Initialize the packable state of generic input/output
  void initializeInOutPackState();

  // Get whether the input locations of the specified shader stage can be packed
  bool canPackInput(ShaderStage shaderStage);

  // Get whether the output locations of the specified shader stage can be packed
  bool canPackOutput(ShaderStage shaderStage);

  // Set the flag to pack the input locations of the specified shader stage
  void setPackInput(ShaderStage shaderStage, bool pack) { m_inputPackState[shaderStage] = pack; }

  // Set the flag to pack the output locations of the specified shader stage
  void setPackOutput(ShaderStage shaderStage, bool pack) { m_outputPackState[shaderStage] = pack; }

  // Get the count of vertices per primitive
  unsigned getVerticesPerPrimitive();

  // -----------------------------------------------------------------------------------------------------------------
  // Utility methods

  // Translate enum "ResourceNodeType" to string
  static const char *getResourceNodeTypeName(ResourceNodeType type);

  // Get name of built-in
  static llvm::StringRef getBuiltInName(BuiltInKind builtIn);

  // -----------------------------------------------------------------------------------------------------------------
  // Utility method templates to read and write IR metadata, used by PipelineState and ShaderModes

  // Get a metadata node containing an array of i32 values, which can be read from any type.
  // The array is trimmed to remove trailing zero values. If the whole array would be 0, then this function
  // returns nullptr.
  template <typename T>
  //
  // @param context : LLVM context
  // @param value : Value to write as array of i32
  // @param atLeastOneValue : True to generate node with one value even if all values are zero
  static llvm::MDNode *getArrayOfInt32MetaNode(llvm::LLVMContext &context, const T &value, bool atLeastOneValue) {
    llvm::IRBuilder<> builder(context);
    llvm::ArrayRef<unsigned> values(reinterpret_cast<const unsigned *>(&value), sizeof(value) / sizeof(unsigned));

    while (!values.empty() && values.back() == 0) {
      if (values.size() == 1 && atLeastOneValue)
        break;
      values = values.slice(0, values.size() - 1);
    }
    if (values.empty())
      return nullptr;

    llvm::SmallVector<llvm::Metadata *, 8> operands;
    for (unsigned value : values)
      operands.push_back(llvm::ConstantAsMetadata::get(builder.getInt32(value)));
    return llvm::MDNode::get(context, operands);
  }

  // Set a named metadata node to point to an array of i32 values, which can be read from any type.
  // The array is trimmed to remove trailing zero values. If the whole array would be 0, then this function
  // removes the named metadata node (if it existed).
  template <typename T>
  //
  // @param [in/out] module : IR module to record into
  // @param value : Value to write as array of i32
  // @param metaName : Name for named metadata node
  static void setNamedMetadataToArrayOfInt32(llvm::Module *module, const T &value, llvm::StringRef metaName) {
    llvm::MDNode *arrayMetaNode = getArrayOfInt32MetaNode(module->getContext(), value, false);
    if (!arrayMetaNode) {
      if (auto namedMetaNode = module->getNamedMetadata(metaName))
        module->eraseNamedMetadata(namedMetaNode);
      return;
    }

    auto namedMetaNode = module->getOrInsertNamedMetadata(metaName);
    namedMetaNode->clearOperands();
    namedMetaNode->addOperand(arrayMetaNode);
  }

  // Read an array of i32 values out of a metadata node, writing into any type.
  // Returns the number of i32s read.
  template <typename T>
  //
  // @param metaNode : Metadata node to read from
  // @param [out] value : Value to write into (caller must zero initialize)
  static unsigned readArrayOfInt32MetaNode(llvm::MDNode *metaNode, T &value) {
    llvm::MutableArrayRef<unsigned> values(reinterpret_cast<unsigned *>(&value), sizeof(value) / sizeof(unsigned));
    unsigned count = std::min(metaNode->getNumOperands(), unsigned(values.size()));
    for (unsigned index = 0; index < count; ++index)
      values[index] = llvm::mdconst::dyn_extract<llvm::ConstantInt>(metaNode->getOperand(index))->getZExtValue();
    return count;
  }

  // Read an array of i32 values out of a metadata node that is operand 0 of the named metadata node,
  // writing into any type.
  // Returns the number of i32s read.
  template <typename T>
  //
  // @param module : IR module to look in
  // @param metaName : Name for named metadata node
  // @param [out] value : Value to write into (caller must zero initialize)
  static unsigned readNamedMetadataArrayOfInt32(llvm::Module *module, llvm::StringRef metaName, T &value) {
    auto namedMetaNode = module->getNamedMetadata(metaName);
    if (!namedMetaNode || namedMetaNode->getNumOperands() == 0)
      return 0;
    return readArrayOfInt32MetaNode(namedMetaNode->getOperand(0), value);
  }

private:
  // Read shaderStageMask from IR
  void readShaderStageMask(llvm::Module *module);

  // Options handling
  void recordOptions(llvm::Module *module);
  void readOptions(llvm::Module *module);

  // User data nodes handling
  void setUserDataNodesTable(llvm::ArrayRef<ResourceNode> nodes, ResourceNode *destTable,
                             ResourceNode *&destInnerTable);
  void recordUserDataNodes(llvm::Module *module);
  void recordUserDataTable(llvm::ArrayRef<ResourceNode> nodes, llvm::NamedMDNode *userDataMetaNode);
  void readUserDataNodes(llvm::Module *module);
  llvm::ArrayRef<llvm::MDString *> getResourceTypeNames();
  llvm::MDString *getResourceTypeName(ResourceNodeType type);
  ResourceNodeType getResourceTypeFromName(llvm::MDString *typeName);
  bool matchResourceNode(const ResourceNode &node, ResourceNodeType nodeType, unsigned descSet, unsigned binding) const;

  // Device index handling
  void recordDeviceIndex(llvm::Module *module);
  void readDeviceIndex(llvm::Module *module);

  // Vertex input descriptions handling
  void recordVertexInputDescriptions(llvm::Module *module);
  void readVertexInputDescriptions(llvm::Module *module);

  // Color export state handling
  void recordColorExportState(llvm::Module *module);
  void readColorExportState(llvm::Module *module);

  // Graphics state (iastate, vpstate, rsstate) handling
  void recordGraphicsState(llvm::Module *module);
  void readGraphicsState(llvm::Module *module);

  std::string m_lastError;                              // Error to be reported by getLastError()
  bool m_noReplayer = false;                            // True if no BuilderReplayer needed
  bool m_emitLgc = false;                               // Whether -emit-lgc is on
  // Whether generating pipeline or unlinked part-pipeline
  PipelineLink m_pipelineLink = PipelineLink::WholePipeline;
  unsigned m_stageMask = 0;                             // Mask of active shader stages
  bool m_computeLibrary = false;                        // Whether pipeline is in fact a compute library
  std::string m_client;                                 // Client name for PAL metadata
  Options m_options = {};                               // Per-pipeline options
  std::vector<ShaderOptions> m_shaderOptions;           // Per-shader options
  std::unique_ptr<ResourceNode[]> m_allocUserDataNodes; // Allocated buffer for user data
  llvm::ArrayRef<ResourceNode> m_userDataNodes;         // Top-level user data node table
  // Cached MDString for each resource node type
  llvm::MDString *m_resourceNodeTypeNames[unsigned(ResourceNodeType::Count)] = {};
  // Allocated buffers for immutable sampler data
  llvm::SmallVector<std::unique_ptr<uint32_t[]>, 4> m_immutableValueAllocs;

  bool m_gsOnChip = false;                                                     // Whether to use GS on-chip mode
  NggControl m_nggControl = {};                                                // NGG control settings
  ShaderModes m_shaderModes;                                                   // Shader modes for this pipeline
  unsigned m_deviceIndex = 0;                                                  // Device index
  std::vector<VertexInputDescription> m_vertexInputDescriptions;               // Vertex input descriptions
  llvm::SmallVector<ColorExportFormat, 8> m_colorExportFormats;                // Color export formats
  ColorExportState m_colorExportState = {};                                    // Color export state
  InputAssemblyState m_inputAssemblyState = {};                                // Input-assembly state
  RasterizerState m_rasterizerState = {};                                      // Rasterizer state
  std::unique_ptr<ResourceUsage> m_resourceUsage[ShaderStageCompute + 1] = {}; // Per-shader ResourceUsage
  std::unique_ptr<InterfaceData> m_interfaceData[ShaderStageCompute + 1] = {}; // Per-shader InterfaceData
  PalMetadata *m_palMetadata = nullptr;                                        // PAL metadata object
  unsigned m_waveSize[ShaderStageCountInternal] = {};                          // Per-shader wave size
  bool m_inputPackState[ShaderStageGfxCount] = {};  // The input packable state per shader stage
  bool m_outputPackState[ShaderStageGfxCount] = {}; // The output packable state per shader stage
};

// =====================================================================================================================
// Wrapper pass for the pipeline state in the middle-end
class PipelineStateWrapper : public llvm::ImmutablePass {
public:
  PipelineStateWrapper(LgcContext *builderContext = nullptr);

  bool doFinalization(llvm::Module &module) override;

  // Get (create if necessary) the PipelineState from this wrapper pass.
  PipelineState *getPipelineState(llvm::Module *module);

  // Set the PipelineState.
  void setPipelineState(PipelineState *pipelineState) { m_pipelineState = pipelineState; }

  static char ID; // ID of this pass

private:
  LgcContext *m_builderContext = nullptr;              // LgcContext for allocating PipelineState
  PipelineState *m_pipelineState = nullptr;                // Cached pipeline state
  std::unique_ptr<PipelineState> m_allocatedPipelineState; // Pipeline state allocated by this pass
};

} // namespace lgc
