/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include <map>

namespace llvm {

class MDString;
class NamedMDNode;
class Timer;

} // namespace llvm

namespace lgc {

class ElfLinker;
class PalMetadata;
class PipelineState;
class TargetInfo;

// Resource node type used to ask to find any buffer node, whether constant or not.
static constexpr ResourceNodeType DescriptorAnyBuffer = ResourceNodeType::Count;

// =====================================================================================================================
// Represents NGG (implicit primitive shader) control settings (valid for GFX10+)

// Enumerates compaction modes after culling operations for NGG primitive shader.
enum NggCompactMode : unsigned {
  NggCompactDisable,  // Compaction is disabled
  NggCompactVertices, // Compaction is based on vertices
};

// Represents NGG tuning options
struct NggControl {
  bool enableNgg;     // Enable NGG mode, use an implicit primitive shader
  bool enableGsUse;   // Enable NGG use on geometry shader
  bool compactVertex; // Enable vertex compaction after culling operations

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

  NggSubgroupSizing subgroupSizing; // NGG subgroup sizing type

  unsigned primsPerSubgroup; // Preferred number of GS primitives to pack into a primitive shader
                             // subgroup

  unsigned vertsPerSubgroup; // Preferred number of vertices consumed by a primitive shader subgroup

  bool passthroughMode; // Whether NGG passthrough mode is enabled
};

// Represents transform feedback state metadata
struct XfbStateMetadata {
  bool enableXfb;                                               // Whether transform feedback is active
  bool enablePrimStats;                                         // Whether to count generated primitives
  std::array<unsigned, MaxTransformFeedbackBuffers> xfbStrides; // The strides of each XFB buffer
  std::array<int, MaxGsStreams> streamXfbBuffers;               // The stream-out XFB buffers bit mask per stream
  std::array<bool, MaxGsStreams> streamActive;                  // Flag indicating which vertex stream is active
};

// =====================================================================================================================
// The middle-end implementation of PipelineState, a subclass of Pipeline.
class PipelineState final : public Pipeline {
public:
  PipelineState(LgcContext *builderContext, bool emitLgc = false);

  ~PipelineState() override final;

  // -----------------------------------------------------------------------------------------------------------------
  // Implementations of Pipeline methods exposed to the front-end

  // Set the resource mapping nodes for the pipeline
  void setUserDataNodes(llvm::ArrayRef<ResourceNode> nodes) override final;

  // Set whether pre-rasterization part has a geometry shader
  // NOTE: Only applicable in the part pipeline compilation mode.
  void setPreRasterHasGs(bool preRasterHasGs) override final { m_preRasterHasGs = preRasterHasGs; }

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

  // Set depth/stencil state
  void setDepthStencilState(const DepthStencilState &dsState) override final;

  // Set the finalized 128-bit cache hash that is used to find this pipeline in the cache for the given version of LLPC.
  void set128BitCacheHash(const Hash128 &finalizedCacheHash, const llvm::VersionTuple &version) override final;

  // Link the individual shader IR modules into a single pipeline module
  llvm::Module *irLink(llvm::ArrayRef<llvm::Module *> modules, PipelineLink pipelineLink) override final;

  // Generate pipeline module
  bool generate(std::unique_ptr<llvm::Module> pipelineModule, llvm::raw_pwrite_stream &outStream,
                CheckShaderCacheFunc checkShaderCacheFunc, llvm::ArrayRef<llvm::Timer *> timers) override final;
  bool generate(llvm::Module *pipelineModule, llvm::raw_pwrite_stream &outStream,
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

  // Set the client-defined metadata to be stored inside the ELF
  void setClientMetadata(llvm::StringRef clientMetadata) override final;

  // Set default tessellation inner/outer level from driver API
  void setTessLevel(const float *tessLevelInner, const float *tessLevelOuter) override final {
    m_tessLevel.inner[0] = tessLevelInner[0];
    m_tessLevel.inner[1] = tessLevelInner[1];
    m_tessLevel.outer[0] = tessLevelOuter[0];
    m_tessLevel.outer[1] = tessLevelOuter[1];
    m_tessLevel.outer[2] = tessLevelOuter[2];
    m_tessLevel.outer[3] = tessLevelOuter[3];
  }

  // Get default tessellation inner/outer level from driver API
  float getTessLevelInner(unsigned level) override final {
    assert(level <= 2);
    return m_tessLevel.inner[level];
  }
  float getTessLevelOuter(unsigned level) override final {
    assert(level <= 4);
    return m_tessLevel.outer[level];
  }

  // -----------------------------------------------------------------------------------------------------------------
  // Other methods

  // Set shader stage mask
  void setShaderStageMask(unsigned mask) { m_stageMask = mask; }

  // Get the embedded ShaderModes object
  const ShaderModes *getShaderModes() const { return &m_shaderModes; }
  ShaderModes *getShaderModes() { return &m_shaderModes; }

  // Accessors for context information
  const TargetInfo &getTargetInfo() const;
  unsigned getPalAbiVersion() const;

  // Return whether we're generating a whole pipeline.
  bool isWholePipeline() const { return m_pipelineLink == PipelineLink::WholePipeline; }

  // Return whether we're generating a part-pipeline.
  bool isPartPipeline() const { return m_pipelineLink == PipelineLink::PartPipeline; }

  // Return whether we're generating an independent unlinked shader (not in the part-pipeline scheme).
  bool isUnlinked() const { return m_pipelineLink == PipelineLink::Unlinked; }

  // Clear the pipeline state IR metadata.
  void clear(llvm::Module *module);

  // Record pipeline state into IR metadata of specified module.
  void record(llvm::Module *module);

  // Print pipeline state
  void print(llvm::raw_ostream &out) const;
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD void dump() const { print(llvm::dbgs()); }
#endif

  // Accessors for shader stage mask
  unsigned getShaderStageMask();
  bool getPreRasterHasGs() const { return m_preRasterHasGs; }
  bool hasShaderStage(ShaderStage stage) { return (getShaderStageMask() >> stage) & 1; }
  bool isGraphics();
  bool isComputeLibrary() const { return m_computeLibrary; }
  ShaderStage getLastVertexProcessingStage() const;
  ShaderStage getPrevShaderStage(ShaderStage shaderStage) const;
  ShaderStage getNextShaderStage(ShaderStage shaderStage) const;

  // Get client name
  const char *getClient() const { return m_client.c_str(); }

  // Get per-shader options
  const ShaderOptions &getShaderOptions(ShaderStage stage);

  // Set up the pipeline state from the pipeline module.
  void readState(llvm::Module *module);

  // Get user data nodes
  llvm::ArrayRef<ResourceNode> getUserDataNodes() const { return m_userDataNodes; }

  // Find the push constant resource node
  const ResourceNode *findPushConstantResourceNode(ShaderStage shaderStage = ShaderStageInvalid) const;

  // Find the resource node for the given set,binding
  std::pair<const ResourceNode *, const ResourceNode *>
  findResourceNode(ResourceNodeType nodeType, uint64_t descSet, unsigned binding,
                   ShaderStage shaderStage = ShaderStageInvalid) const;

  // Find the single root resource node of the given type
  const ResourceNode *findSingleRootResourceNode(ResourceNodeType nodeType, ShaderStage shaderStage) const;

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
  unsigned getNumPatchControlPoints() const;
  const RasterizerState &getRasterizerState() const { return m_rasterizerState; }
  const DepthStencilState &getDepthStencilState() const { return m_depthStencilState; }

  // Determine whether to use off-chip tessellation mode
  bool isTessOffChip();

  // Set GS on-chip mode
  void setGsOnChip(bool gsOnChip) { m_gsOnChip = gsOnChip; }

  // Checks whether GS on-chip mode is enabled
  // NOTE: GS on-chip mode has different meaning for GFX6~8 and GFX9: on GFX6~8, GS on-chip mode means ES -> GS ring
  // and GS -> VS ring are both on-chip; on GFX9, ES -> GS ring is always on-chip, GS on-chip mode means GS -> VS
  // ring is on-chip.
  bool isGsOnChip() const { return m_gsOnChip; }

  // Determine whether can use tessellation factor optimization
  bool canOptimizeTessFactor();

  // Gets wave size for the specified shader stage
  unsigned getShaderWaveSize(ShaderStage stage);
  // Gets wave size for the merged shader stage
  unsigned getMergedShaderWaveSize(ShaderStage stage);
  // Gets subgroup size for the specified shader stage
  unsigned getShaderSubgroupSize(ShaderStage stage);

  // Set the default wave size for the specified shader stage
  void setShaderDefaultWaveSize(ShaderStage stage);

  // Set the wave size for the specified shader stage
  void setShaderWaveSize(ShaderStage stage, unsigned waveSize) {
    assert(waveSize == 32 || waveSize == 64);
    m_waveSize[stage] = waveSize;
  }

  // Whether WGP mode is enabled for the given shader stage
  bool getShaderWgpMode(ShaderStage stage) const;

  // Get NGG control settings
  NggControl *getNggControl() { return &m_nggControl; }

  // Checks if SW-emulated mesh pipeline statistics is needed
  bool needSwMeshPipelineStats() const;

  // Checks if row export for mesh shader is enabled or not
  bool enableMeshRowExport() const;

  // Checks if register field value format is used or not
  bool useRegisterFieldFormat() const { return m_registerFieldFormat; }

  // Checks if SW-emulated stream-out should be enabled
  bool enableSwXfb();

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

  // Get the primitive type
  PrimitiveType getPrimitiveType();

  // -----------------------------------------------------------------------------------------------------------------
  // Utility methods

  // Translate enum "ResourceNodeType" to string
  static const char *getResourceNodeTypeName(ResourceNodeType type);

  // Get name of built-in
  static llvm::StringRef getBuiltInName(BuiltInKind builtIn);

  // Set transform feedback state metadata
  void setXfbStateMetadata(llvm::Module *module);

  // Check if transform feedback is active
  bool enableXfb() const { return m_xfbStateMetadata.enableXfb; }

  // Check if we need count primitives if XFB is disabled
  bool enablePrimStats() const { return m_xfbStateMetadata.enablePrimStats; }

  // Get transform feedback strides
  const std::array<unsigned, MaxTransformFeedbackBuffers> &getXfbBufferStrides() const {
    return m_xfbStateMetadata.xfbStrides;
  }

  // Get transform feedback strides
  std::array<unsigned, MaxTransformFeedbackBuffers> &getXfbBufferStrides() { return m_xfbStateMetadata.xfbStrides; }

  // Get transform feedback buffers used for each stream
  const std::array<int, MaxGsStreams> &getStreamXfbBuffers() const { return m_xfbStateMetadata.streamXfbBuffers; }

  // Get transform feedback buffers used for each stream
  std::array<int, MaxGsStreams> &getStreamXfbBuffers() { return m_xfbStateMetadata.streamXfbBuffers; }

  // Set the activness for a vertex stream
  void setVertexStreamActive(unsigned streamId) { m_xfbStateMetadata.streamActive[streamId] = true; }

  // Get the activeness for a vertex stream
  bool isVertexStreamActive(unsigned streamId) {
    if (getRasterizerState().rasterStream == streamId)
      return true; // Rasterization stream is always active
    return m_xfbStateMetadata.streamActive[streamId];
  }

  // Set user data for a specific shader stage
  void setUserDataMap(ShaderStage shaderStage, llvm::ArrayRef<unsigned> userDataValues) {
    m_userDataMaps[shaderStage].clear();
    m_userDataMaps[shaderStage].append(userDataValues.begin(), userDataValues.end());
  }

  // Get user data for a specific shader stage
  llvm::ArrayRef<unsigned> getUserDataMap(ShaderStage shaderStage) const { return m_userDataMaps[shaderStage]; }

  // -----------------------------------------------------------------------------------------------------------------
  // Utility method templates to read and write IR metadata, used by PipelineState and ShaderModes

  // Get a metadata node containing an array of i32 values, which can be read from any type.
  // The array is trimmed to remove trailing zero values. If the whole array would be 0, then this function
  // returns nullptr.
  //
  // @param context : LLVM context
  // @param value : Value to write as array of i32
  // @param atLeastOneValue : True to generate node with one value even if all values are zero
  template <typename T>
  static llvm::MDNode *getArrayOfInt32MetaNode(llvm::LLVMContext &context, const T &value, bool atLeastOneValue) {
    llvm::IRBuilder<> builder(context);
    static_assert(sizeof(value) % sizeof(unsigned) == 0, "Bad value type");
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
  //
  // @param [in/out] module : IR module to record into
  // @param value : Value to write as array of i32
  // @param metaName : Name for named metadata node
  template <typename T>
  static void setNamedMetadataToArrayOfInt32(llvm::Module *module, const T &value, llvm::StringRef metaName) {
    static_assert(sizeof(value) % sizeof(unsigned) == 0, "Bad value type");
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
  //
  // @param metaNode : Metadata node to read from
  // @param [out] value : Value to write into (caller must zero initialize)
  template <typename T> static unsigned readArrayOfInt32MetaNode(llvm::MDNode *metaNode, T &value) {
    static_assert(sizeof(value) % sizeof(unsigned) == 0, "Bad value type");
    llvm::MutableArrayRef<unsigned> values(reinterpret_cast<unsigned *>(&value), sizeof(value) / sizeof(unsigned));
    unsigned count = std::min(metaNode->getNumOperands(), unsigned(values.size()));
    for (unsigned index = 0; index < count; ++index)
      values[index] = llvm::mdconst::dyn_extract<llvm::ConstantInt>(metaNode->getOperand(index))->getZExtValue();
    return count;
  }

  // Read an array of i32 values out of a metadata node that is operand 0 of the named metadata node,
  // writing into any type.
  // Returns the number of i32s read.
  //
  // @param module : IR module to look in
  // @param metaName : Name for named metadata node
  // @param [out] value : Value to write into (caller must zero initialize)
  template <typename T>
  static unsigned readNamedMetadataArrayOfInt32(llvm::Module *module, llvm::StringRef metaName, T &value) {
    auto namedMetaNode = module->getNamedMetadata(metaName);
    if (!namedMetaNode || namedMetaNode->getNumOperands() == 0)
      return 0;
    return readArrayOfInt32MetaNode(namedMetaNode->getOperand(0), value);
  }

  // Set a named metadata node to point to its previous array of i32 values, with a new array of i32 ORed in.
  // The array is trimmed to remove trailing zero values. If the whole array would be 0, then this function
  // removes the named metadata node (if it existed).
  //
  // @param [in/out] module : IR module to record into
  // @param value : Value to write as array of i32
  // @param metaName : Name for named metadata node
  template <typename T>
  static void orNamedMetadataToArrayOfInt32(llvm::Module *module, const T &value, llvm::StringRef metaName) {
    static_assert(sizeof(value) % sizeof(unsigned) == 0, "Bad value type");
    llvm::ArrayRef<unsigned> values(reinterpret_cast<const unsigned *>(&value), sizeof(value) / sizeof(unsigned));
    unsigned oredValues[sizeof(value) / sizeof(unsigned)] = {};
    auto namedMetaNode = module->getOrInsertNamedMetadata(metaName);
    if (namedMetaNode->getNumOperands() >= 1)
      readArrayOfInt32MetaNode(namedMetaNode->getOperand(0), oredValues);
    for (unsigned idx = 0; idx != sizeof(value) / sizeof(unsigned); ++idx)
      oredValues[idx] |= values[idx];
    llvm::MDNode *arrayMetaNode = getArrayOfInt32MetaNode(module->getContext(), oredValues, false);
    if (!arrayMetaNode) {
      module->eraseNamedMetadata(namedMetaNode);
      return;
    }
    namedMetaNode->clearOperands();
    namedMetaNode->addOperand(arrayMetaNode);
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
  bool matchResourceNode(const ResourceNode &node, ResourceNodeType nodeType, uint64_t descSet, unsigned binding) const;

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

  std::string m_lastError; // Error to be reported by getLastError()
  bool m_emitLgc = false;  // Whether -emit-lgc is on
  // Whether generating pipeline or unlinked part-pipeline
  PipelineLink m_pipelineLink = PipelineLink::WholePipeline;
  unsigned m_stageMask = 0;                             // Mask of active shader stages
  bool m_preRasterHasGs = false;                        // Whether pre-rasterization part has a geometry shader
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
  bool m_meshRowExport = false;                                                // Enable mesh shader row export or not
  bool m_registerFieldFormat = false;                                          // Use register field format
  NggControl m_nggControl = {};                                                // NGG control settings
  ShaderModes m_shaderModes;                                                   // Shader modes for this pipeline
  unsigned m_deviceIndex = 0;                                                  // Device index
  std::vector<VertexInputDescription> m_vertexInputDescriptions;               // Vertex input descriptions
  llvm::SmallVector<ColorExportFormat, 8> m_colorExportFormats;                // Color export formats
  ColorExportState m_colorExportState = {};                                    // Color export state
  InputAssemblyState m_inputAssemblyState = {};                                // Input-assembly state
  RasterizerState m_rasterizerState = {};                                      // Rasterizer state
  DepthStencilState m_depthStencilState = {};                                  // Depth/stencil state
  std::unique_ptr<ResourceUsage> m_resourceUsage[ShaderStageCompute + 1] = {}; // Per-shader ResourceUsage
  std::unique_ptr<InterfaceData> m_interfaceData[ShaderStageCompute + 1] = {}; // Per-shader InterfaceData
  PalMetadata *m_palMetadata = nullptr;                                        // PAL metadata object
  unsigned m_waveSize[ShaderStageCountInternal] = {};                          // Per-shader wave size
  unsigned m_subgroupSize[ShaderStageCountInternal] = {};                      // Per-shader subgroup size
  bool m_inputPackState[ShaderStageGfxCount] = {};  // The input packable state per shader stage
  bool m_outputPackState[ShaderStageGfxCount] = {}; // The output packable state per shader stage
  XfbStateMetadata m_xfbStateMetadata = {};         // Transform feedback state metadata
  llvm::SmallVector<unsigned, 32> m_userDataMaps[ShaderStageCountInternal]; // The user data per-shader
  bool m_useMrt0AToMrtzA = false;                                           // Whether to copy mrt0.a to mrz.a

  struct {
    float inner[2]; // default tessellation inner level
    float outer[4]; // default tessellation outer level
  } m_tessLevel;
};

// =====================================================================================================================
// PipelineStateWrapper analysis result
class PipelineStateWrapperResult {
public:
  PipelineStateWrapperResult(PipelineState *pipelineState);
  PipelineState *getPipelineState() { return m_pipelineState; }

  bool invalidate(llvm::Module &, const llvm::PreservedAnalyses &, llvm::ModuleAnalysisManager::Invalidator &) {
    return false;
  }

private:
  PipelineState *m_pipelineState = nullptr;
};

// =====================================================================================================================
// Wrapper pass for the pipeline state in the middle-end
class PipelineStateWrapper : public llvm::AnalysisInfoMixin<PipelineStateWrapper> {
public:
  using Result = PipelineStateWrapperResult;
  PipelineStateWrapper(LgcContext *builderContext);
  PipelineStateWrapper(PipelineState *pipelineState);
  Result run(llvm::Module &module, llvm::ModuleAnalysisManager &);
  static llvm::AnalysisKey Key; // NOLINT

private:
  LgcContext *m_builderContext = nullptr;                  // LgcContext for allocating PipelineState
  PipelineState *m_pipelineState = nullptr;                // Cached pipeline state
  std::unique_ptr<PipelineState> m_allocatedPipelineState; // Pipeline state allocated by this pass
};

// =====================================================================================================================
// Pass to clear pipeline state out of the IR
class PipelineStateClearer : public llvm::PassInfoMixin<PipelineStateClearer> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  bool runImpl(llvm::Module &module, PipelineState *pipelineState);

  static llvm::StringRef name() { return "LLPC pipeline state clearer"; }
};

// =====================================================================================================================
// Pass to print the pipeline state in a human-readable way
class PipelineStatePrinter : public llvm::PassInfoMixin<PipelineStatePrinter> {
public:
  explicit PipelineStatePrinter(llvm::raw_ostream &out = llvm::dbgs()) : m_out(out) {}

  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

private:
  llvm::raw_ostream &m_out;
};

// =====================================================================================================================
// Pass to record the pipeline state back into the IR if present
class PipelineStateRecorder : public llvm::PassInfoMixin<PipelineStateRecorder> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
};

} // namespace lgc
