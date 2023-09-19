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
 * @file  PipelineState.cpp
 * @brief LLPC source file: contains implementation of class lgc::PipelineState.
 ***********************************************************************************************************************
 */
#include "lgc/state/PipelineState.h"
#include "lgc/CommonDefs.h"
#include "lgc/LgcContext.h"
#include "lgc/PassManager.h"
#include "lgc/patch/FragColorExport.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-pipeline-state"

using namespace lgc;
using namespace llvm;

// -enable-tess-offchip: enable tessellation off-chip mode
static cl::opt<bool> EnableTessOffChip("enable-tess-offchip", cl::desc("Enable tessellation off-chip mode"),
                                       cl::init(false));

// -enable-row-export: enable row export for mesh shader
static cl::opt<bool> EnableRowExport("enable-row-export", cl::desc("Enable row export for mesh shader"),
                                     cl::init(false));

cl::opt<bool> UseRegisterFieldFormat("use-register-field-format", cl::desc("Use register field format in pipeline ELF"),
                                     cl::init(true));

// Names for named metadata nodes when storing and reading back pipeline state
static const char UnlinkedMetadataName[] = "lgc.unlinked";
static const char PreRasterHasGsMetadataName[] = "lgc.prerast.has.gs";
static const char ClientMetadataName[] = "lgc.client";
static const char OptionsMetadataName[] = "lgc.options";
static const char UserDataMetadataName[] = "lgc.user.data.nodes";
static const char DeviceIndexMetadataName[] = "lgc.device.index";
static const char VertexInputsMetadataName[] = "lgc.vertex.inputs";
static const char IaStateMetadataName[] = "lgc.input.assembly.state";
static const char RsStateMetadataName[] = "lgc.rasterizer.state";
static const char ColorExportFormatsMetadataName[] = "lgc.color.export.formats";
static const char ColorExportStateMetadataName[] = "lgc.color.export.state";
static const char TessLevelMetadataName[] = "lgc.tessellation.level.state";

namespace {

// =====================================================================================================================
// Gets the maximum bit-count of any component in specified color attachment format.
//
// @param dfmt : Color attachment data format
static unsigned getMaxComponentBitCount(BufDataFormat dfmt) {
  switch (dfmt) {
  case BufDataFormatInvalid:
  case BufDataFormatReserved:
    return 0;
  case BufDataFormat4_4:
  case BufDataFormat4_4_4_4:
  case BufDataFormat4_4_4_4_Bgra:
    return 4;
  case BufDataFormat5_6_5:
  case BufDataFormat5_6_5_Bgr:
  case BufDataFormat5_6_5_1:
  case BufDataFormat5_6_5_1_Bgra:
  case BufDataFormat1_5_6_5:
    return 6;
  case BufDataFormat8:
  case BufDataFormat8_8:
  case BufDataFormat8_8_8:
  case BufDataFormat8_8_8_Bgr:
  case BufDataFormat8_8_8_8:
  case BufDataFormat8_8_8_8_Bgra:
    return 8;
  case BufDataFormat5_9_9_9:
    return 9;
  case BufDataFormat10_10_10_2:
  case BufDataFormat2_10_10_10:
  case BufDataFormat2_10_10_10_Bgra:
    return 10;
  case BufDataFormat10_11_11:
  case BufDataFormat11_11_10:
    return 11;
  case BufDataFormat16:
  case BufDataFormat16_16:
  case BufDataFormat16_16_16_16:
    return 16;
  case BufDataFormat32:
  case BufDataFormat32_32:
  case BufDataFormat32_32_32:
  case BufDataFormat32_32_32_32:
    return 32;
  case BufDataFormat64:
  case BufDataFormat64_64:
  case BufDataFormat64_64_64:
  case BufDataFormat64_64_64_64:
    return 64;
  }
  return 0;
}

// =====================================================================================================================
// Checks whether the alpha channel is present in the specified color attachment format.
//
// @param dfmt : Color attachment data format
static bool hasAlpha(BufDataFormat dfmt) {
  switch (dfmt) {
  case BufDataFormat10_10_10_2:
  case BufDataFormat2_10_10_10:
  case BufDataFormat8_8_8_8:
  case BufDataFormat16_16_16_16:
  case BufDataFormat32_32_32_32:
  case BufDataFormat8_8_8_8_Bgra:
  case BufDataFormat2_10_10_10_Bgra:
  case BufDataFormat64_64_64_64:
  case BufDataFormat4_4_4_4:
  case BufDataFormat4_4_4_4_Bgra:
  case BufDataFormat5_6_5_1:
  case BufDataFormat5_6_5_1_Bgra:
  case BufDataFormat1_5_6_5:
  case BufDataFormat5_9_9_9:
    return true;
  default:
    return false;
  }
}

// =====================================================================================================================
// Get the number of channels
//
// @param dfmt : Color attachment data format
static unsigned getNumChannels(BufDataFormat dfmt) {
  switch (dfmt) {
  case BufDataFormatInvalid:
  case BufDataFormatReserved:
  case BufDataFormat8:
  case BufDataFormat16:
  case BufDataFormat32:
  case BufDataFormat64:
    return 1;
  case BufDataFormat4_4:
  case BufDataFormat8_8:
  case BufDataFormat16_16:
  case BufDataFormat32_32:
  case BufDataFormat64_64:
    return 2;
  case BufDataFormat8_8_8:
  case BufDataFormat8_8_8_Bgr:
  case BufDataFormat10_11_11:
  case BufDataFormat11_11_10:
  case BufDataFormat32_32_32:
  case BufDataFormat64_64_64:
  case BufDataFormat5_6_5:
  case BufDataFormat5_6_5_Bgr:
    return 3;
  case BufDataFormat10_10_10_2:
  case BufDataFormat2_10_10_10:
  case BufDataFormat8_8_8_8:
  case BufDataFormat16_16_16_16:
  case BufDataFormat32_32_32_32:
  case BufDataFormat8_8_8_8_Bgra:
  case BufDataFormat2_10_10_10_Bgra:
  case BufDataFormat64_64_64_64:
  case BufDataFormat4_4_4_4:
  case BufDataFormat4_4_4_4_Bgra:
  case BufDataFormat5_6_5_1:
  case BufDataFormat5_6_5_1_Bgra:
  case BufDataFormat1_5_6_5:
  case BufDataFormat5_9_9_9:
    return 4;
  }
  return 0;
}

// =====================================================================================================================
// This is the helper function for the algorithm to determine the shader export format.
//
// @param dfmt : Color attachment data format
static CompSetting computeCompSetting(BufDataFormat dfmt) {
  CompSetting compSetting = CompSetting::Invalid;
  switch (getNumChannels(dfmt)) {
  case 1:
    compSetting = CompSetting::OneCompRed;
    break;
  case 2:
    compSetting = CompSetting::TwoCompGreenRed;
    break;
  }
  return compSetting;
}
} // namespace

// =====================================================================================================================
// Set the common shader mode for the given shader stage, containing hardware FP round and denorm modes.
// This records the mode into IR metadata in the given module.
//
// @param module : Module to record in
// @param shaderStage : Shader stage to set modes for
// @param commonShaderMode : FP round and denorm modes
void Pipeline::setCommonShaderMode(Module &module, ShaderStage shaderStage, const CommonShaderMode &commonShaderMode) {
  ShaderModes::setCommonShaderMode(module, shaderStage, commonShaderMode);
}

// =====================================================================================================================
// Get the common shader mode for the given shader stage.
// This reads the mode from IR metadata in the given module.
//
// @param module : Module to read from
// @param shaderStage : Shader stage to get modes for
CommonShaderMode Pipeline::getCommonShaderMode(Module &module, ShaderStage shaderStage) {
  return ShaderModes::getCommonShaderMode(module, shaderStage);
}

// =====================================================================================================================
// Set the tessellation mode
// This records the mode into IR metadata in the given module.
// Both TCS and TES can set tessellation mode, and the two get merged together by the middle end.
//
// @param module : Module to record in
// @param shaderStage : Shader stage to set modes for (TCS or TES)
// @param tessellationMode : Tessellation mode
void Pipeline::setTessellationMode(Module &module, ShaderStage shaderStage, const TessellationMode &tessellationMode) {
  ShaderModes::setTessellationMode(module, shaderStage, tessellationMode);
}

// =====================================================================================================================
// Get the tessellation mode for the given shader stage.
// This reads the mode from IR metadata in the given module.
TessellationMode Pipeline::getTessellationMode(Module &module, ShaderStage shaderStage) {
  return ShaderModes::getTessellationMode(module, shaderStage);
}

// =====================================================================================================================
// Set the geometry shader mode
// This records the mode into IR metadata in the given module.
//
// @param module : Module to record in
// @param geometryShaderMode : Geometry shader mode
void Pipeline::setGeometryShaderMode(Module &module, const GeometryShaderMode &geometryShaderMode) {
  ShaderModes::setGeometryShaderMode(module, geometryShaderMode);
}

// =====================================================================================================================
// Set the mesh shader mode
// This records the mode into IR metadata in the given module.
//
// @param module : Module to record in
// @param meshShaderMode : Mesh shader mode
void Pipeline::setMeshShaderMode(Module &module, const MeshShaderMode &meshShaderMode) {
  ShaderModes::setMeshShaderMode(module, meshShaderMode);
}

// =====================================================================================================================
// Set the fragment shader mode
// This records the mode into IR metadata in the given module.
//
// @param module : Module to record in
// @param fragmentShaderMode : Fragment shader mode
void Pipeline::setFragmentShaderMode(Module &module, const FragmentShaderMode &fragmentShaderMode) {
  ShaderModes::setFragmentShaderMode(module, fragmentShaderMode);
}

// =====================================================================================================================
// Set the compute shader mode (workgroup size)
// This records the mode into IR metadata in the given module.
//
// @param module : Module to record in
// @param computeShaderMode : Compute shader mode
void Pipeline::setComputeShaderMode(Module &module, const ComputeShaderMode &computeShaderMode) {
  ShaderModes::setComputeShaderMode(module, computeShaderMode);
}

// =====================================================================================================================
// Set subgroup size usage.
// This records the mode into IR metadata in the given module.
//
// @param module : Module to record in
// @param stage : Shader stage
// @param usage : Subgroup size usage
void Pipeline::setSubgroupSizeUsage(Module &module, ShaderStage stage, bool usage) {
  ShaderModes::setSubgroupSizeUsage(module, stage, usage);
}

// =====================================================================================================================
// Get the compute shader mode (workgroup size)
// This reads the mode from IR metadata in the given module.
//
// @param module : Module to read from
ComputeShaderMode Pipeline::getComputeShaderMode(Module &module) {
  return ShaderModes::getComputeShaderMode(module);
}

// =====================================================================================================================
// Constructor
//
// @param builderContext : LGC builder context
// @param emitLgc : Whether the option -emit-lgc is on
PipelineState::PipelineState(LgcContext *builderContext, bool emitLgc)
    : Pipeline(builderContext), m_emitLgc(emitLgc), m_meshRowExport(EnableRowExport) {
  m_registerFieldFormat = getTargetInfo().getGfxIpVersion().major >= 9 && UseRegisterFieldFormat;
  m_tessLevel.inner[0] = -1.0f;
  m_tessLevel.inner[1] = -1.0f;
  m_tessLevel.outer[0] = -1.0f;
  m_tessLevel.outer[1] = -1.0f;
  m_tessLevel.outer[2] = -1.0f;
  m_tessLevel.outer[3] = -1.0f;
}

// =====================================================================================================================
// Destructor
PipelineState::~PipelineState() {
  delete m_palMetadata;
}

// =====================================================================================================================
// Get LLVMContext
LLVMContext &Pipeline::getContext() const {
  return getLgcContext()->getContext();
}

// =====================================================================================================================
// Get TargetInfo
const TargetInfo &PipelineState::getTargetInfo() const {
  return getLgcContext()->getTargetInfo();
}

// =====================================================================================================================
// Get PAL pipeline ABI version
unsigned PipelineState::getPalAbiVersion() const {
  return getLgcContext()->getPalAbiVersion();
}

// =====================================================================================================================
// Get PalMetadata object, creating an empty one if necessary
PalMetadata *PipelineState::getPalMetadata() {
  if (!m_palMetadata)
    m_palMetadata = new PalMetadata(this, m_registerFieldFormat);
  return m_palMetadata;
}

// =====================================================================================================================
// Clear PAL metadata object
void PipelineState::clearPalMetadata() {
  delete m_palMetadata;
  m_palMetadata = nullptr;
}

// =====================================================================================================================
// Merge blob of MsgPack data into existing PAL metadata
//
// @param blob : MsgPack PAL metadata to merge
// @param isGlueCode : True if the blob was generated for glue code.
void PipelineState::mergePalMetadataFromBlob(StringRef blob, bool isGlueCode) {
  if (!m_palMetadata)
    m_palMetadata = new PalMetadata(this, blob, m_registerFieldFormat);
  else
    m_palMetadata->mergeFromBlob(blob, isGlueCode);
}

// =====================================================================================================================
// Set the "other part-pipeline" from the given other Pipeline object. This is used when doing a part-pipeline
// compile of the non-FS part of the pipeline, to inherit required information from the FS part-pipeline.
//
// @param otherPartPipeline : The other part-pipeline, containing metadata for FS input mappings
// @param module : If called before Pipeline::irLink(), should be nullptr. If called after Pipeline::irLink(), should
//                 be the linked IR module, so the PAL metadata that needs to be inherited from otherPartPipeline
//                 can be recorded in the module. The latter is provided as a hook for the LGC tool, which does not
//                 do an irLink() at all.
void PipelineState::setOtherPartPipeline(Pipeline &otherPartPipeline, Module *linkedModule) {
  auto &otherPartPipelineState = *static_cast<PipelineState *>(&otherPartPipeline);
  getPalMetadata()->setOtherPartPipeline(*otherPartPipelineState.getPalMetadata());
  // Record the updated PAL metadata.
  if (linkedModule && m_palMetadata)
    m_palMetadata->record(linkedModule);
}

// =====================================================================================================================
// Copy client-defined metadata blob to be stored inside ELF.
//
// @param clientMetadata : StringRef representing the client metadata blob
void PipelineState::setClientMetadata(StringRef clientMetadata) {
  getPalMetadata()->setClientMetadata(clientMetadata);
}

// =====================================================================================================================
// Clear the pipeline state IR metadata.
// This does not clear PalMetadata, because we want that to persist into the back-end.
//
// @param [in/out] module : IR module
void PipelineState::clear(Module *module) {
  getShaderModes()->clear();
  m_options = {};
  m_userDataNodes = {};
  m_deviceIndex = 0;
  m_vertexInputDescriptions.clear();
  m_colorExportFormats.clear();
  m_colorExportState = {};
  m_inputAssemblyState = {};
  m_rasterizerState = {};
  record(module);
}

// =====================================================================================================================
// Record pipeline state into IR metadata of specified module.
//
// @param [in/out] module : Module to record the IR metadata in
void PipelineState::record(Module *module) {
  recordOptions(module);
  recordUserDataNodes(module);
  recordDeviceIndex(module);
  recordVertexInputDescriptions(module);
  recordColorExportState(module);
  recordGraphicsState(module);
  if (m_palMetadata)
    m_palMetadata->record(module);
}

// =====================================================================================================================
// Set up the pipeline state from the pipeline IR module.
//
// @param module : LLVM module
void PipelineState::readState(Module *module) {
  getShaderModes()->readModesFromPipeline(module);
  readShaderStageMask(module);
  readOptions(module);
  readUserDataNodes(module);
  readDeviceIndex(module);
  readVertexInputDescriptions(module);
  readColorExportState(module);
  readGraphicsState(module);
  if (!m_palMetadata)
    m_palMetadata = new PalMetadata(this, module, m_registerFieldFormat);
  setXfbStateMetadata(module);
}

// =====================================================================================================================
// Read shaderStageMask from IR. This consists of checking what shader stage functions are present in the IR.
// It also sets the m_computeLibrary flag if there are no shader entry-points.
//
// @param module : LLVM module
void PipelineState::readShaderStageMask(Module *module) {
  m_stageMask = 0;
  for (auto &func : *module) {
    if (isShaderEntryPoint(&func)) {
      auto shaderStage = getShaderStage(&func);
      if (shaderStage != ShaderStageInvalid)
        m_stageMask |= 1 << shaderStage;
    }
  }
  if (m_stageMask == 0) {
    m_stageMask = 1 << ShaderStageCompute;
    m_computeLibrary = true;
  }
}

// =====================================================================================================================
// Get the last vertex processing shader stage in this pipeline, or ShaderStageInvalid if none.
ShaderStage PipelineState::getLastVertexProcessingStage() const {
  if (m_stageMask & shaderStageToMask(ShaderStageCopyShader))
    return ShaderStageCopyShader;
  if (m_stageMask & shaderStageToMask(ShaderStageGeometry))
    return ShaderStageGeometry;
  if (m_stageMask & shaderStageToMask(ShaderStageTessEval))
    return ShaderStageTessEval;
  if (m_stageMask & shaderStageToMask(ShaderStageVertex))
    return ShaderStageVertex;
  return ShaderStageInvalid;
}

// =====================================================================================================================
// Gets the previous active shader stage in this pipeline
//
// @param shaderStage : Current shader stage
ShaderStage PipelineState::getPrevShaderStage(ShaderStage shaderStage) const {
  if (shaderStage == ShaderStageCompute)
    return ShaderStageInvalid;

  if (shaderStage == ShaderStageCopyShader) {
    // Treat copy shader as part of geometry shader
    shaderStage = ShaderStageGeometry;
  }

  assert(shaderStage < ShaderStageGfxCount);

  ShaderStage prevStage = ShaderStageInvalid;

  for (int stage = shaderStage - 1; stage >= 0; --stage) {
    if ((m_stageMask & shaderStageToMask(static_cast<ShaderStage>(stage))) != 0) {
      prevStage = static_cast<ShaderStage>(stage);
      break;
    }
  }

  return prevStage;
}

// =====================================================================================================================
// Gets the next active shader stage in this pipeline
//
// @param shaderStage : Current shader stage
ShaderStage PipelineState::getNextShaderStage(ShaderStage shaderStage) const {
  if (shaderStage == ShaderStageCompute)
    return ShaderStageInvalid;

  if (shaderStage == ShaderStageCopyShader) {
    // Treat copy shader as part of geometry shader
    shaderStage = ShaderStageGeometry;
  }

  assert(shaderStage < ShaderStageGfxCount);

  ShaderStage nextStage = ShaderStageInvalid;
  unsigned stageMask = m_stageMask;
  if (isPartPipeline())
    stageMask |= shaderStageToMask(ShaderStageFragment);

  for (unsigned stage = shaderStage + 1; stage < ShaderStageGfxCount; ++stage) {
    if ((stageMask & shaderStageToMask(static_cast<ShaderStage>(stage))) != 0) {
      nextStage = static_cast<ShaderStage>(stage);
      break;
    }
  }

  return nextStage;
}

// =====================================================================================================================
// Get the shader stage mask.
unsigned PipelineState::getShaderStageMask() {
  if (!m_stageMask && !m_computeLibrary) {
    // No shader stage mask set (and it isn't a compute library). We must be in ElfLinker; get the shader stage
    // mask from PAL metadata.
    m_stageMask = getPalMetadata()->getShaderStageMask();
  }
  return m_stageMask;
}

// =====================================================================================================================
// Check whether the pipeline is a graphics pipeline
bool PipelineState::isGraphics() {
  return (getShaderStageMask() & ((1U << ShaderStageTask) | (1U << ShaderStageVertex) | (1U << ShaderStageTessControl) |
                                  (1U << ShaderStageTessEval) | (1U << ShaderStageGeometry) | (1U << ShaderStageMesh) |
                                  (1U << ShaderStageFragment))) != 0;
}

// =====================================================================================================================
// Set per-shader options
//
// @param stage : Shader stage
// @param options : Shader options
void PipelineState::setShaderOptions(ShaderStage stage, const ShaderOptions &options) {
  if (m_shaderOptions.size() <= stage)
    m_shaderOptions.resize(stage + 1);
  m_shaderOptions[stage] = options;
}

// =====================================================================================================================
// Get per-shader options
//
// @param stage : Shader stage
const ShaderOptions &PipelineState::getShaderOptions(ShaderStage stage) {
  if (m_shaderOptions.size() <= stage)
    m_shaderOptions.resize(stage + 1);
  return m_shaderOptions[stage];
}

// =====================================================================================================================
// Record pipeline and shader options into IR metadata.
// This also records m_client and m_pipelineLink.
// TODO: The options could be recorded in a more human-readable form, with a string for the option name for each
// option.
//
// @param [in/out] module : Module to record metadata into
void PipelineState::recordOptions(Module *module) {
  auto clientNamedMeta = module->getOrInsertNamedMetadata(ClientMetadataName);
  clientNamedMeta->clearOperands();
  clientNamedMeta->addOperand(MDNode::get(module->getContext(), MDString::get(module->getContext(), m_client)));

  if (unsigned unlinkedAsInt = unsigned(m_pipelineLink))
    setNamedMetadataToArrayOfInt32(module, unlinkedAsInt, UnlinkedMetadataName);
  if (unsigned preRasterHasGs = unsigned(m_preRasterHasGs))
    setNamedMetadataToArrayOfInt32(module, preRasterHasGs, PreRasterHasGsMetadataName);
  setNamedMetadataToArrayOfInt32(module, m_options, OptionsMetadataName);
  for (unsigned stage = 0; stage != m_shaderOptions.size(); ++stage) {
    std::string metadataName =
        (Twine(OptionsMetadataName) + "." + getShaderStageAbbreviation(static_cast<ShaderStage>(stage))).str();
    setNamedMetadataToArrayOfInt32(module, m_shaderOptions[stage], metadataName);
  }
}

// =====================================================================================================================
// Read pipeline and shader options from IR metadata.
// This also reads m_client and m_pipelineLink.
//
// @param module : Module to read metadata from
void PipelineState::readOptions(Module *module) {
  m_client.clear();
  if (auto clientNamedMeta = module->getNamedMetadata(ClientMetadataName)) {
    if (clientNamedMeta->getNumOperands() >= 1) {
      auto clientMeta = clientNamedMeta->getOperand(0);
      if (clientMeta->getNumOperands() >= 1) {
        if (MDString *mdString = dyn_cast<MDString>(clientMeta->getOperand(0)))
          m_client = mdString->getString().str();
      }
    }
  }

  unsigned unlinkedAsInt = 0;
  readNamedMetadataArrayOfInt32(module, UnlinkedMetadataName, unlinkedAsInt);
  m_pipelineLink = PipelineLink(unlinkedAsInt);
  unsigned preRasterHasGsAsInt = 0;
  readNamedMetadataArrayOfInt32(module, PreRasterHasGsMetadataName, preRasterHasGsAsInt);
  m_preRasterHasGs = preRasterHasGsAsInt;

  readNamedMetadataArrayOfInt32(module, OptionsMetadataName, m_options);
  for (unsigned stage = 0; stage != ShaderStageCompute + 1; ++stage) {
    std::string metadataName =
        (Twine(OptionsMetadataName) + "." + getShaderStageAbbreviation(static_cast<ShaderStage>(stage))).str();
    auto namedMetaNode = module->getNamedMetadata(metadataName);
    if (!namedMetaNode || namedMetaNode->getNumOperands() == 0)
      continue;
    m_shaderOptions.resize(stage + 1);
    readArrayOfInt32MetaNode(namedMetaNode->getOperand(0), m_shaderOptions[stage]);
  }
}

// =====================================================================================================================
// Set the resource nodes for the pipeline.
//
// @param nodes : The resource nodes. Copied, so only need to remain valid for the duration of this call.
void PipelineState::setUserDataNodes(ArrayRef<ResourceNode> nodes) {
  // Count how many entries in total and allocate the buffer.
  unsigned nodeCount = nodes.size();
  for (auto &node : nodes) {
    if (node.concreteType == ResourceNodeType::DescriptorTableVaPtr)
      nodeCount += node.innerTable.size();
  }
  assert(m_allocUserDataNodes == nullptr);
  m_allocUserDataNodes = std::make_unique<ResourceNode[]>(nodeCount);

  // Copy nodes in.
  ResourceNode *destTable = m_allocUserDataNodes.get();
  ResourceNode *destInnerTable = destTable + nodeCount;
  m_userDataNodes = ArrayRef<ResourceNode>(destTable, nodes.size());
  setUserDataNodesTable(nodes, destTable, destInnerTable);
  assert(destInnerTable == destTable + nodes.size());
}

// =====================================================================================================================
// Set one user data table, and its inner tables.
//
// @param nodes : The source resource nodes to copy
// @param [out] destTable : Where to write nodes
// @param [in/out] destInnerTable : End of space available for inner tables
void PipelineState::setUserDataNodesTable(ArrayRef<ResourceNode> nodes, ResourceNode *destTable,
                                          ResourceNode *&destInnerTable) {
  for (unsigned idx = 0; idx != nodes.size(); ++idx) {
    auto &node = nodes[idx];
    auto &destNode = destTable[idx];

    // Copy the node.
    destNode = node;

    switch (node.concreteType) {
    case ResourceNodeType::DescriptorTableVaPtr:
      // Process an inner table.
      destInnerTable -= node.innerTable.size();
      destNode.innerTable = ArrayRef<ResourceNode>(destInnerTable, node.innerTable.size());
      setUserDataNodesTable(node.innerTable, destInnerTable, destInnerTable);
      break;
    case ResourceNodeType::IndirectUserDataVaPtr:
    case ResourceNodeType::StreamOutTableVaPtr:
      break;
    default:
      // If there is immutable sampler data, take our own copy of it.
      if (node.immutableSize != 0) {
        unsigned sizeInDwords = node.immutableSize * DescriptorSizeSamplerInDwords;
        auto buffer = std::make_unique<uint32_t[]>(sizeInDwords);
        std::copy_n(node.immutableValue, sizeInDwords, buffer.get());
        destNode.immutableValue = buffer.get();
        m_immutableValueAllocs.push_back(std::move(buffer));
      }
      break;
    }
  }
}

// =====================================================================================================================
// Record user data nodes into IR metadata.
// Note that this takes a Module* instead of using m_pModule, because it can be called before pipeline linking.
//
// @param [in/out] module : Module to record the IR metadata in
void PipelineState::recordUserDataNodes(Module *module) {
  if (m_userDataNodes.empty()) {
    if (auto userDataMetaNode = module->getNamedMetadata(UserDataMetadataName))
      module->eraseNamedMetadata(userDataMetaNode);
    return;
  }

  auto userDataMetaNode = module->getOrInsertNamedMetadata(UserDataMetadataName);
  userDataMetaNode->clearOperands();
  recordUserDataTable(m_userDataNodes, userDataMetaNode);
}

// =====================================================================================================================
// Record one table of user data nodes into IR metadata, calling itself recursively for inner tables.
//
// @param nodes : Table of user data nodes
// @param userDataMetaNode : IR metadata node to record them into
void PipelineState::recordUserDataTable(ArrayRef<ResourceNode> nodes, NamedMDNode *userDataMetaNode) {
  IRBuilder<> builder(getContext());

  for (const ResourceNode &node : nodes) {
    SmallVector<Metadata *, 5> operands;
    assert(node.concreteType < ResourceNodeType::Count);
    // Operand 0: type
    operands.push_back(getResourceTypeName(node.concreteType));
    // Operand 1: matchType
    operands.push_back(ConstantAsMetadata::get(builder.getInt32(static_cast<uint32_t>(node.abstractType))));
    // Operand 2: visibility
    operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.visibility)));
    // Operand 3: offsetInDwords
    operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.offsetInDwords)));
    // Operand 4: sizeInDwords
    operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.sizeInDwords)));

    switch (node.concreteType) {
    case ResourceNodeType::DescriptorTableVaPtr: {
      // Operand 5: Node count in sub-table.
      operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.innerTable.size())));
      // Create the metadata node here.
      userDataMetaNode->addOperand(MDNode::get(getContext(), operands));
      // Create nodes for the sub-table.
      recordUserDataTable(node.innerTable, userDataMetaNode);
      continue;
    }
    case ResourceNodeType::IndirectUserDataVaPtr:
    case ResourceNodeType::StreamOutTableVaPtr: {
      // Operand 5: Size of the indirect data in dwords.
      operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.indirectSizeInDwords)));
      break;
    }
    default: {
      // Operand 5: set
      operands.push_back(ConstantAsMetadata::get(builder.getInt64(node.set)));
      // Operand 6: binding
      operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.binding)));
      // Operand 7: stride
      operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.stride)));
      // Operand 8 onwards: immutable descriptor constants
      for (uint32_t element :
           ArrayRef<uint32_t>(node.immutableValue, node.immutableSize * DescriptorSizeSamplerInDwords))
        operands.push_back(ConstantAsMetadata::get(builder.getInt32(element)));
      break;
    }
    }

    // Create the metadata node.
    userDataMetaNode->addOperand(MDNode::get(getContext(), operands));
  }
}

// =====================================================================================================================
// Read user data nodes for the pipeline from IR metadata
//
// @param module : LLVM module
void PipelineState::readUserDataNodes(Module *module) {
  // Find the named metadata node.
  auto userDataMetaNode = module->getNamedMetadata(UserDataMetadataName);
  if (!userDataMetaNode)
    return;

  // Prepare to read the resource nodes from the named MD node. We allocate a single buffer, with the
  // outer table at the start, and inner tables allocated from the end backwards.
  unsigned totalNodeCount = userDataMetaNode->getNumOperands();
  m_allocUserDataNodes = std::make_unique<ResourceNode[]>(totalNodeCount);

  ResourceNode *nextOuterNode = m_allocUserDataNodes.get();
  ResourceNode *nextNode = nextOuterNode;
  ResourceNode *endNextInnerTable = nextOuterNode + totalNodeCount;
  ResourceNode *endThisInnerTable = nullptr;

  // Read the nodes.
  for (unsigned nodeIndex = 0; nodeIndex < totalNodeCount; ++nodeIndex) {
    MDNode *metadataNode = userDataMetaNode->getOperand(nodeIndex);
    // Operand 0: node type
    nextNode->concreteType = getResourceTypeFromName(cast<MDString>(metadataNode->getOperand(0)));
    // Operand 1: matchType
    nextNode->abstractType =
        static_cast<ResourceNodeType>(mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(1))->getZExtValue());
    // Operand 2: visibility
    nextNode->visibility = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(2))->getZExtValue();
    // Operand 3: offsetInDwords
    nextNode->offsetInDwords = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(3))->getZExtValue();
    // Operand 4: sizeInDwords
    nextNode->sizeInDwords = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(4))->getZExtValue();

    if (nextNode->concreteType == ResourceNodeType::DescriptorTableVaPtr) {
      // Operand 5: number of nodes in inner table
      unsigned innerNodeCount = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(5))->getZExtValue();
      // Go into inner table.
      assert(!endThisInnerTable);
      endThisInnerTable = endNextInnerTable;
      endNextInnerTable -= innerNodeCount;
      nextNode = endNextInnerTable;
      nextOuterNode->innerTable = ArrayRef<ResourceNode>(nextNode, innerNodeCount);
      ++nextOuterNode;
    } else {
      if (nextNode->concreteType == ResourceNodeType::IndirectUserDataVaPtr ||
          nextNode->concreteType == ResourceNodeType::StreamOutTableVaPtr) {
        // Operand 5: Size of the indirect data in dwords
        nextNode->indirectSizeInDwords = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(5))->getZExtValue();
      } else {
        // Operand 5: set
        nextNode->set = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(5))->getZExtValue();
        // Operand 6: binding
        nextNode->binding = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(6))->getZExtValue();
        // Operand 7: stride
        nextNode->stride = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(7))->getZExtValue();
        nextNode->immutableValue = nullptr;
        // Operand 8 onward: immutable descriptor constants
        constexpr unsigned ImmutableStartOperand = 8;
        unsigned immutableSizeInDwords = metadataNode->getNumOperands() - ImmutableStartOperand;
        nextNode->immutableSize = immutableSizeInDwords / DescriptorSizeSamplerInDwords;
        if (nextNode->immutableSize) {
          m_immutableValueAllocs.push_back(std::make_unique<uint32_t[]>(immutableSizeInDwords));
          nextNode->immutableValue = m_immutableValueAllocs.back().get();
          for (unsigned i = 0; i != immutableSizeInDwords; ++i)
            m_immutableValueAllocs.back()[i] =
                mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(ImmutableStartOperand + i))->getZExtValue();
        }
      }
      // Move on to next node to write in table.
      ++nextNode;
      if (!endThisInnerTable)
        nextOuterNode = nextNode;
    }
    // See if we have reached the end of the inner table.
    if (nextNode == endThisInnerTable) {
      endThisInnerTable = nullptr;
      nextNode = nextOuterNode;
    }
  }
  m_userDataNodes = ArrayRef<ResourceNode>(m_allocUserDataNodes.get(), nextOuterNode);
}

// =====================================================================================================================
// Returns the resource node for the push constant.
//
// @param stage : Shader stage to check against nodes' visibility field, or ShaderStageInvalid for any
const ResourceNode *PipelineState::findPushConstantResourceNode(ShaderStage stage) const {
  unsigned visibilityMask = UINT_MAX;
  if (stage != ShaderStageInvalid)
    visibilityMask = 1 << std::min(unsigned(stage), unsigned(ShaderStageCompute));

  for (const ResourceNode &node : getUserDataNodes()) {
    if (node.visibility != 0 && (node.visibility & visibilityMask) == 0)
      continue;
    if (node.concreteType == ResourceNodeType::PushConst)
      return &node;
    if (node.concreteType == ResourceNodeType::DescriptorTableVaPtr) {
      if (!node.innerTable.empty() && node.innerTable[0].concreteType == ResourceNodeType::PushConst) {
        if (node.innerTable[0].visibility != 0 && (node.innerTable[0].visibility & visibilityMask) == 0)
          continue;
        assert(ResourceLayoutScheme::Indirect == m_options.resourceLayoutScheme);
        return &node;
      }
    }
  }
  return nullptr;
}

// =====================================================================================================================
// Returns true when type nodeType is compatible with candidateType.
// A node type is compatible with a candidate type iff (nodeType) <= (candidateType) in the ResourceNodeType lattice:
//
// DescriptorBufferCompact
//        +                                               DescriptorCombinedTexture
//        |         DescriptorConstBufferCompact                     +
//        |             +                                            |
//        |             |    InlineBuffer                            |
//        |             |      +                 +-------------------+--------------------+
//        |             |      |                 |                   |                    |
//        v             v      v                 v                   v                    v
// DescriptorBuffer  DescriptorConstBuffer  DescriptorResource  DescriptorTexelBuffer  DescriptorSampler
//          +            +                       +                   +                    +
//          |            |                       |                   |                    |
//          v            v                       |                   |                    |
//       DescriptorAnyBuffer                     v                   |                    |
//                +-------------------------> Unknown <--------------+--------------------+
//
// @param nodeType : Resource node type
// @param candidateType : Resource node candidate type
static bool isNodeTypeCompatible(ResourceNodeType nodeType, ResourceNodeType candidateType) {
  if (nodeType == ResourceNodeType::Unknown || candidateType == nodeType ||
      candidateType == ResourceNodeType::DescriptorMutable)
    return true;

  if ((nodeType == ResourceNodeType::DescriptorConstBuffer || nodeType == DescriptorAnyBuffer) &&
      (candidateType == ResourceNodeType::DescriptorConstBufferCompact ||
       candidateType == ResourceNodeType::DescriptorConstBuffer || candidateType == ResourceNodeType::InlineBuffer))
    return true;

  if ((nodeType == ResourceNodeType::DescriptorBuffer || nodeType == DescriptorAnyBuffer) &&
      (candidateType == ResourceNodeType::DescriptorBufferCompact ||
       candidateType == ResourceNodeType::DescriptorBuffer))
    return true;

  if ((nodeType == ResourceNodeType::DescriptorResource || nodeType == ResourceNodeType::DescriptorTexelBuffer ||
       nodeType == ResourceNodeType::DescriptorSampler) &&
      candidateType == ResourceNodeType::DescriptorCombinedTexture)
    return true;

  return false;
}

// =====================================================================================================================
// Returns true when type is one that has a binding.
// @param nodeType : Resource node type
static bool nodeTypeHasBinding(ResourceNodeType nodeType) {
  switch (nodeType) {
  case ResourceNodeType::DescriptorResource:
  case ResourceNodeType::DescriptorSampler:
  case ResourceNodeType::DescriptorCombinedTexture:
  case ResourceNodeType::DescriptorTexelBuffer:
  case ResourceNodeType::DescriptorFmask:
  case ResourceNodeType::DescriptorBuffer:
  case ResourceNodeType::DescriptorTableVaPtr:
  case ResourceNodeType::DescriptorBufferCompact:
  case ResourceNodeType::InlineBuffer:
  case ResourceNodeType::DescriptorConstBuffer:
  case ResourceNodeType::DescriptorConstBufferCompact:
    return true;
  case ResourceNodeType::IndirectUserDataVaPtr:
  case ResourceNodeType::PushConst:
  case ResourceNodeType::StreamOutTableVaPtr:
    return false;
  default:
    LLVM_BUILTIN_UNREACHABLE;
  }
  return false;
}

// =====================================================================================================================
// Check whether a (non-table) resource node matches the given {set,binding} compatible with nodeType
// If pipeline option useResourceBindingRange is set, then a node matches a range of bindings of size
// sizeInDwords/stride.
//
// @param node : Node to try and match
// @param nodeType : Resource node type being searched for
// @param descSet : Descriptor set being searched for
// @param binding : Descriptor binding being searched for
bool PipelineState::matchResourceNode(const ResourceNode &node, ResourceNodeType nodeType, uint64_t descSet,
                                      unsigned binding) const {
  if (node.set != descSet || !isNodeTypeCompatible(nodeType, node.abstractType))
    return false;
  if (node.binding == binding)
    return true;
  if (getOptions().useResourceBindingRange)
    return node.binding <= binding && (binding - node.binding) * node.stride < node.sizeInDwords;
  return false;
}

// =====================================================================================================================
// Find the resource node for the given {set,binding} compatible with nodeType.
//
// For nodeType == DescriptorTableVaPtr, the node whose first child matches descSet is returned.
// Returns {topNode, node} where "node" is the found user data node, and "topNode" is the top-level user data
// node that contains it (or is equal to it).
//
// If the node is not found and nodeType == Fmask, then a search will be done for a DescriptorResource at the given
// descriptor set and binding.
//
// @param nodeType : Type of the resource mapping node
// @param descSet : ID of descriptor set
// @param binding : ID of descriptor binding
// @param stage : Shader stage to check against nodes' visibility field, or ShaderStageInvalid for any
std::pair<const ResourceNode *, const ResourceNode *> PipelineState::findResourceNode(ResourceNodeType nodeType,
                                                                                      uint64_t descSet,
                                                                                      unsigned binding,
                                                                                      ShaderStage stage) const {
  unsigned visibilityMask = UINT_MAX;
  if (stage != ShaderStageInvalid)
    visibilityMask = 1 << std::min(unsigned(stage), unsigned(ShaderStageCompute));

  for (const ResourceNode &node : getUserDataNodes()) {
    if (!nodeTypeHasBinding(node.concreteType))
      continue;
    if (node.visibility != 0 && (node.visibility & visibilityMask) == 0)
      continue;

    if (node.concreteType == ResourceNodeType::DescriptorTableVaPtr) {
      if (nodeType == ResourceNodeType::DescriptorTableVaPtr) {
        assert(!node.innerTable.empty());

        if (node.innerTable[0].set == descSet)
          return {&node, &node};
        continue;
      }

      // Check inner nodes.
      for (const ResourceNode &innerNode : node.innerTable) {
        if (innerNode.visibility != 0 && (innerNode.visibility & visibilityMask) == 0)
          continue;
        if (matchResourceNode(innerNode, nodeType, descSet, binding))
          return {&node, &innerNode};
      }
    } else if (matchResourceNode(node, nodeType, descSet, binding))
      return {&node, &node};
  }

  if (nodeType == ResourceNodeType::DescriptorFmask && getOptions().enableFmask) {
#if defined(__GNUC__) && !defined(__clang__)
    // FIXME Newer gcc versions optimize out this if statement. The reason is either undefined behavior in lgc or a bug
    // in gcc. The following inline assembly prevents the gcc optimization.
    // See https://github.com/GPUOpen-Drivers/llpc/issues/1096 for more information.
    asm volatile("" : "+m,r"(nodeType) : : "memory");
#endif

    // If use fmask and no fmask descriptor is found, look for a resource (image) one instead.
    return findResourceNode(ResourceNodeType::DescriptorResource, descSet, binding, stage);
  }
  return {nullptr, nullptr};
}

// =====================================================================================================================
// Find the single root resource node of the given type
//
// @param nodeType : Type of the resource mapping node
// @param stage : Shader stage to check against nodes' visibility field, or ShaderStageInvalid for any
const ResourceNode *PipelineState::findSingleRootResourceNode(ResourceNodeType nodeType, ShaderStage stage) const {
  unsigned visibilityMask = UINT_MAX;
  if (stage != ShaderStageInvalid)
    visibilityMask = 1 << std::min(unsigned(stage), unsigned(ShaderStageCompute));

  for (const ResourceNode &node : getUserDataNodes()) {
    if (node.visibility != 0 && (node.visibility & visibilityMask) == 0)
      continue;
    if (node.concreteType == nodeType)
      return &node;
  }
  return nullptr;
}

// =====================================================================================================================
// Get the cached MDString for the name of a resource mapping node type, as used in IR metadata for user data nodes.
//
// @param type : Resource mapping node type
MDString *PipelineState::getResourceTypeName(ResourceNodeType type) {
  return getResourceTypeNames()[static_cast<unsigned>(type)];
}

// =====================================================================================================================
// Get the resource mapping node type given its MDString name.
//
// @param typeName : Name of resource type as MDString
ResourceNodeType PipelineState::getResourceTypeFromName(MDString *typeName) {
  auto typeNames = getResourceTypeNames();
  for (unsigned type = 0;; ++type) {
    if (typeNames[type] == typeName)
      return static_cast<ResourceNodeType>(type);
  }
}

// =====================================================================================================================
// Get the array of cached MDStrings for names of resource mapping node type, as used in IR metadata for user
// data nodes.
ArrayRef<MDString *> PipelineState::getResourceTypeNames() {
  if (!m_resourceNodeTypeNames[0]) {
    for (ResourceNodeType type : enumRange<ResourceNodeType>()) {
      m_resourceNodeTypeNames[toUnderlying(type)] = MDString::get(getContext(), getResourceNodeTypeName(type));
    }
  }
  return ArrayRef<MDString *>(m_resourceNodeTypeNames);
}

// =====================================================================================================================
// Set vertex input descriptions. Each location referenced in a call to CreateReadGenericInput in the
// vertex shader must have a corresponding description provided here.
//
// @param inputs : Array of vertex input descriptions
void PipelineState::setVertexInputDescriptions(ArrayRef<VertexInputDescription> inputs) {
  m_vertexInputDescriptions.clear();
  m_vertexInputDescriptions.insert(m_vertexInputDescriptions.end(), inputs.begin(), inputs.end());
}

// =====================================================================================================================
// Find vertex input description for the given location.
// Returns nullptr if location not found.
//
// @param location : Location
const VertexInputDescription *PipelineState::findVertexInputDescription(unsigned location) const {
  for (auto &inputDesc : m_vertexInputDescriptions) {
    if (inputDesc.location == location)
      return &inputDesc;
  }
  return nullptr;
}

// =====================================================================================================================
// Record vertex input descriptions into IR metadata.
//
// @param [in/out] module : Module to record the IR metadata in
void PipelineState::recordVertexInputDescriptions(Module *module) {
  if (m_vertexInputDescriptions.empty()) {
    if (auto vertexInputsMetaNode = module->getNamedMetadata(VertexInputsMetadataName))
      module->eraseNamedMetadata(vertexInputsMetaNode);
    return;
  }

  auto vertexInputsMetaNode = module->getOrInsertNamedMetadata(VertexInputsMetadataName);
  IRBuilder<> builder(getContext());
  vertexInputsMetaNode->clearOperands();

  for (const VertexInputDescription &input : m_vertexInputDescriptions)
    vertexInputsMetaNode->addOperand(getArrayOfInt32MetaNode(getContext(), input, /*atLeastOneValue=*/true));
}

// =====================================================================================================================
// Read vertex input descriptions for the pipeline from IR metadata
//
// @param module : Module to read
void PipelineState::readVertexInputDescriptions(Module *module) {
  m_vertexInputDescriptions.clear();

  // Find the named metadata node.
  auto vertexInputsMetaNode = module->getNamedMetadata(VertexInputsMetadataName);
  if (!vertexInputsMetaNode)
    return;

  // Read the nodes.
  unsigned nodeCount = vertexInputsMetaNode->getNumOperands();
  for (unsigned nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
    m_vertexInputDescriptions.push_back({});
    readArrayOfInt32MetaNode(vertexInputsMetaNode->getOperand(nodeIndex), m_vertexInputDescriptions.back());
  }
}

// =====================================================================================================================
// Set color export state.
//
// @param formats : Array of ColorExportFormat structs
// @param exportState : Color export flags
void PipelineState::setColorExportState(ArrayRef<ColorExportFormat> formats, const ColorExportState &exportState) {
  // Trim null entries off the end.
  constexpr ColorExportFormat nullFormat = {};
  while (!formats.empty() && memcmp(&formats.back(), &nullFormat, sizeof(nullFormat)) == 0)
    formats = formats.drop_back(1);
  // Copy into our state.
  m_colorExportFormats.clear();
  m_colorExportFormats.insert(m_colorExportFormats.end(), formats.begin(), formats.end());
  m_colorExportState = exportState;
}

// =====================================================================================================================
// Get format for one color export
//
// @param location : Export location
const ColorExportFormat &PipelineState::getColorExportFormat(unsigned location) {
  if (getColorExportState().dualSourceBlendEnable)
    location = 0;

  if (location >= m_colorExportFormats.size()) {
    static const ColorExportFormat EmptyFormat = {};
    return EmptyFormat;
  }
  return m_colorExportFormats[location];
}

// =====================================================================================================================
// Record color export state (including formats) into IR metadata
//
// @param [in/out] module : IR module
void PipelineState::recordColorExportState(Module *module) {
  if (m_colorExportFormats.empty()) {
    if (auto exportFormatsMetaNode = module->getNamedMetadata(ColorExportFormatsMetadataName))
      module->eraseNamedMetadata(exportFormatsMetaNode);
  } else {
    auto exportFormatsMetaNode = module->getOrInsertNamedMetadata(ColorExportFormatsMetadataName);
    IRBuilder<> builder(getContext());
    exportFormatsMetaNode->clearOperands();

    // The color export formats named metadata node's operands are:
    // - N metadata nodes for N color targets, each one containing
    // { dfmt, nfmt, blendEnable, blendSrcAlphaToColor }
    for (const ColorExportFormat &target : m_colorExportFormats)
      exportFormatsMetaNode->addOperand(getArrayOfInt32MetaNode(getContext(), target, /*atLeastOneValue=*/true));
  }

  setNamedMetadataToArrayOfInt32(module, m_colorExportState, ColorExportStateMetadataName);
}

// =====================================================================================================================
// Read color targets state from IR metadata
//
// @param module : IR module
void PipelineState::readColorExportState(Module *module) {
  m_colorExportFormats.clear();

  auto exportFormatsMetaNode = module->getNamedMetadata(ColorExportFormatsMetadataName);
  if (exportFormatsMetaNode) {
    // Read the color target nodes.
    for (unsigned nodeIndex = 0; nodeIndex < exportFormatsMetaNode->getNumOperands(); ++nodeIndex) {
      m_colorExportFormats.push_back({});
      readArrayOfInt32MetaNode(exportFormatsMetaNode->getOperand(nodeIndex), m_colorExportFormats.back());
    }
  }

  readNamedMetadataArrayOfInt32(module, ColorExportStateMetadataName, m_colorExportState);
}

// =====================================================================================================================
// Set graphics state (input-assembly, viewport, rasterizer).
//
// @param iaState : Input assembly state
// @param rsState : Rasterizer state
void PipelineState::setGraphicsState(const InputAssemblyState &iaState, const RasterizerState &rsState) {
  m_inputAssemblyState = iaState;
  m_rasterizerState = rsState;
}

// =====================================================================================================================
// Set depth/stencil state.
//
// @param dsState : Depth/stencil state
void PipelineState::setDepthStencilState(const DepthStencilState &dsState) {
  m_depthStencilState = dsState;
}

// =====================================================================================================================
// Set the finalized 128-bit cache hash that is used to find this pipeline in the cache.
//
// @param finalizedCacheHash: The 128-bit hash value.
// @param version: The version of LLPC used to compute the hash.  This will let other tools know if the hashes are
//                 comparable.
void PipelineState::set128BitCacheHash(const Hash128 &finalizedCacheHash, const VersionTuple &version) {
  getPalMetadata()->setFinalized128BitCacheHash(finalizedCacheHash, version);
}

// =====================================================================================================================
// Record device index into the IR metadata
//
// @param [in/out] module : IR module to record into
void PipelineState::recordDeviceIndex(Module *module) {
  setNamedMetadataToArrayOfInt32(module, m_deviceIndex, DeviceIndexMetadataName);
}

// =====================================================================================================================
// Read device index from the IR metadata
//
// @param [in/out] module : IR module to read from
void PipelineState::readDeviceIndex(Module *module) {
  readNamedMetadataArrayOfInt32(module, DeviceIndexMetadataName, m_deviceIndex);
}

// =====================================================================================================================
// Record graphics state (iastate, vpstate, rsstate) into the IR metadata
//
// @param [in/out] module : IR module to record into
void PipelineState::recordGraphicsState(Module *module) {
  setNamedMetadataToArrayOfInt32(module, m_inputAssemblyState, IaStateMetadataName);
  setNamedMetadataToArrayOfInt32(module, m_rasterizerState, RsStateMetadataName);
  if (m_tessLevel.inner[0] >= 0 || m_tessLevel.inner[1] >= 0 || m_tessLevel.outer[0] >= 0 ||
      m_tessLevel.outer[1] >= 0 || m_tessLevel.outer[2] >= 0 || m_tessLevel.outer[3] >= 0)
    setNamedMetadataToArrayOfInt32(module, m_tessLevel, TessLevelMetadataName);
}

// =====================================================================================================================
// Read graphics state (device index, iastate, vpstate, rsstate) from the IR metadata
//
// @param [in/out] module : IR module to read from
void PipelineState::readGraphicsState(Module *module) {
  readNamedMetadataArrayOfInt32(module, IaStateMetadataName, m_inputAssemblyState);
  readNamedMetadataArrayOfInt32(module, RsStateMetadataName, m_rasterizerState);
  readNamedMetadataArrayOfInt32(module, TessLevelMetadataName, m_tessLevel);

  auto nameMeta = module->getNamedMetadata(SampleShadingMetaName);
  if (nameMeta)
    m_rasterizerState.perSampleShading |= 1;
}

// =====================================================================================================================
// Get number of patch control points. The front-end supplies this as TessellationMode::inputVertices.
unsigned PipelineState::getNumPatchControlPoints() const {
  return getShaderModes()->getTessellationMode().inputVertices;
}

// =====================================================================================================================
// Determine whether to use off-chip tessellation mode
bool PipelineState::isTessOffChip() {
  // For GFX9+, always enable tessellation off-chip mode
  return EnableTessOffChip || getLgcContext()->getTargetInfo().getGfxIpVersion().major >= 9;
}

// =====================================================================================================================
// Gets wave size for the specified shader stage
//
// NOTE: Need to be called after PatchResourceCollect pass, so usage of subgroupSize is confirmed.
//
// @param stage : Shader stage
unsigned PipelineState::getShaderWaveSize(ShaderStage stage) {
  if (stage == ShaderStageCopyShader) {
    // Treat copy shader as part of geometry shader
    stage = ShaderStageGeometry;
  }

  assert(stage <= ShaderStageCompute);
  if (!m_waveSize[stage])
    setShaderDefaultWaveSize(stage);

  if (getTargetInfo().getGfxIpVersion().major >= 9) {
    return getMergedShaderWaveSize(stage);
  }

  return m_waveSize[stage];
}

// =====================================================================================================================
// Gets wave size for the merged shader stage
//
// NOTE: For GFX9+, two shaders are merged as a shader pair. The wave size is determined by the larger one.
//
// @param stage : Shader stage
unsigned PipelineState::getMergedShaderWaveSize(ShaderStage stage) {
  assert(getTargetInfo().getGfxIpVersion().major >= 9);
  unsigned waveSize = m_waveSize[stage];

  // NOTE: For GFX9+, two shaders are merged as a shader pair. The wave size is determined by the larger one. That is
  // to say:
  // - VS + TCS -> HW HS
  // - VS + GS -> HW GS (no tessellation)
  // - TES + GS -> HW GS
  // - VS/TES -> HW GS (NGG, no geometry)
  switch (stage) {
  case ShaderStageVertex:
    if (hasShaderStage(ShaderStageTessControl)) {
      return std::max(waveSize, m_waveSize[ShaderStageTessControl]);
    }
    if (hasShaderStage(ShaderStageGeometry)) {
      return std::max(waveSize, m_waveSize[ShaderStageGeometry]);
    }
    return waveSize;

  case ShaderStageTessControl:
    return std::max(waveSize, m_waveSize[ShaderStageVertex]);

  case ShaderStageTessEval:
    if (hasShaderStage(ShaderStageGeometry)) {
      return std::max(waveSize, m_waveSize[ShaderStageGeometry]);
    }
    return waveSize;

  case ShaderStageGeometry:
    if (!hasShaderStage(ShaderStageGeometry)) {
      // NGG, no geometry
      return std::max(waveSize,
                      m_waveSize[hasShaderStage(ShaderStageTessEval) ? ShaderStageTessEval : ShaderStageVertex]);
    }
    if (hasShaderStage(ShaderStageTessEval)) {
      return std::max(waveSize, m_waveSize[ShaderStageTessEval]);
    }
    return std::max(waveSize, m_waveSize[ShaderStageVertex]);

  default:
    return waveSize;
  }
}

// =====================================================================================================================
// Get subgroup size for the specified shader stage.
//
// @param stage : Shader stage
// @returns : Subgroup size of the specified shader stage
unsigned PipelineState::getShaderSubgroupSize(ShaderStage stage) {
  if (stage == ShaderStageCopyShader) {
    // Treat copy shader as part of geometry shader
    stage = ShaderStageGeometry;
  }

  assert(stage <= ShaderStageCompute);
  if (!m_subgroupSize[stage])
    setShaderDefaultWaveSize(stage);

  return m_subgroupSize[stage];
}

// =====================================================================================================================
// Set the default wave size for the specified shader stage
//
// @param stage : Shader stage
void PipelineState::setShaderDefaultWaveSize(ShaderStage stage) {
  ShaderStage checkingStage = stage;
  const bool isGfx10Plus = getTargetInfo().getGfxIpVersion().major >= 10;
  if (isGfx10Plus && stage == ShaderStageGeometry && !hasShaderStage(ShaderStageGeometry)) {
    // NOTE: For NGG, GS could be absent and VS/TES acts as part of it in the merged shader.
    // In such cases, we check the property of VS or TES.
    checkingStage = hasShaderStage(ShaderStageTessEval) ? ShaderStageTessEval : ShaderStageVertex;
  }

  if (!m_waveSize[checkingStage]) {
    unsigned waveSize = getTargetInfo().getGpuProperty().waveSize;
    unsigned subgroupSize = waveSize;

    if (isGfx10Plus) {
      // NOTE: GPU property wave size is used in shader, unless:
      //  1) A stage-specific default is preferred.
      //  2) If specified by tuning option, use the specified wave size.
      //  3) If gl_SubgroupSize is used in shader, use the specified subgroup size when required.
      //  4) If gl_SubgroupSize is not used in the (mesh/task/compute) shader, and the workgroup size is
      //     not larger than 32, use wave size 32.

      if (checkingStage == ShaderStageFragment) {
        // Per programming guide, it's recommended to use wave64 for fragment shader.
        waveSize = 64;
      } else if (hasShaderStage(ShaderStageGeometry)) {
        // Legacy (non-NGG) hardware path for GS does not support wave32.
        waveSize = 64;
        if (getTargetInfo().getGfxIpVersion().major >= 11)
          waveSize = 32;
      }

      // Experimental data from performance tuning show that wave64 is more efficient than wave32 in most cases for CS
      // on post-GFX10.3. Hence, set the wave size to wave64 by default.
      if (getTargetInfo().getGfxIpVersion() >= GfxIpVersion({10, 3}) && stage == ShaderStageCompute)
        waveSize = 64;

      // Prefer wave64 on GFX11+
      if (getTargetInfo().getGfxIpVersion() >= GfxIpVersion({11}))
        waveSize = 64;

      unsigned waveSizeOption = getShaderOptions(checkingStage).waveSize;
      if (waveSizeOption != 0)
        waveSize = waveSizeOption;

      // Note: the conditions below override the tuning option.
      // If workgroup size is not larger than 32, use wave size 32.
      if (checkingStage == ShaderStageMesh || checkingStage == ShaderStageTask || checkingStage == ShaderStageCompute) {
        unsigned workGroupSize;
        if (checkingStage == ShaderStageMesh) {
          auto &mode = m_shaderModes.getMeshShaderMode();
          workGroupSize = mode.workgroupSizeX * mode.workgroupSizeY * mode.workgroupSizeZ;
        } else {
          assert(checkingStage == ShaderStageTask || checkingStage == ShaderStageCompute);
          auto &mode = m_shaderModes.getComputeShaderMode();
          workGroupSize = mode.workgroupSizeX * mode.workgroupSizeY * mode.workgroupSizeZ;
        }

        if (workGroupSize <= 32)
          waveSize = 32;
      }

      // If subgroup size is used in any shader in the pipeline, use the specified subgroup size.
      if (m_shaderModes.getAnyUseSubgroupSize()) {
        // If allowVaryWaveSize is enabled, subgroupSize is default as zero, initialized as waveSize
        subgroupSize = getShaderOptions(checkingStage).subgroupSize;
        // The driver only sets waveSize if a size is requested by an app. We may want to change that in the driver to
        // set subgroupSize instead.
        if (subgroupSize == 0)
          subgroupSize = getShaderOptions(checkingStage).waveSize;
        if (subgroupSize == 0)
          subgroupSize = waveSize;

        if ((subgroupSize < waveSize) || getOptions().fullSubgroups)
          waveSize = subgroupSize;
      } else {
        // The subgroup size cannot be observed, use the wave size.
        subgroupSize = waveSize;
      }

      assert(waveSize == 32 || waveSize == 64);
      assert(waveSize <= subgroupSize);
    }
    m_waveSize[checkingStage] = waveSize;
    m_subgroupSize[checkingStage] = subgroupSize;
  }
  if (stage != checkingStage) {
    m_waveSize[stage] = m_waveSize[checkingStage];
    m_subgroupSize[stage] = m_subgroupSize[checkingStage];
  }
}

// =====================================================================================================================
// Whether WGP mode is enabled for the given shader stage
//
// @param stage : Shader stage
bool PipelineState::getShaderWgpMode(ShaderStage stage) const {
  if (stage == ShaderStageCopyShader) {
    // Treat copy shader as part of geometry shader
    stage = ShaderStageGeometry;
  }

  assert(stage <= ShaderStageCompute);
  assert(stage < m_shaderOptions.size());

  return m_shaderOptions[stage].wgpMode;
}

// =====================================================================================================================
// Checks if SW-emulated mesh pipeline statistics is needed
bool PipelineState::needSwMeshPipelineStats() const {
  return getTargetInfo().getGfxIpVersion().major < 11;
}

// =====================================================================================================================
// Checks if row export for mesh shader is enabled or not
bool PipelineState::enableMeshRowExport() const {
  if (getTargetInfo().getGfxIpVersion().major < 11)
    return false; // Row export is not supported by HW

  return m_meshRowExport;
}

// =====================================================================================================================
// Checks if SW-emulated stream-out should be enabled.
bool PipelineState::enableSwXfb() {
  // Not graphics pipeline
  if (!isGraphics())
    return false;

  // SW-emulated stream-out is enabled on GFX11+
  if (getTargetInfo().getGfxIpVersion().major < 11)
    return false;

  // Mesh pipeline doesn't support stream-out
  if (hasShaderStage(ShaderStageTask) || hasShaderStage(ShaderStageMesh))
    return false;

  auto lastVertexStage = getLastVertexProcessingStage();
  lastVertexStage = lastVertexStage == ShaderStageCopyShader ? ShaderStageGeometry : lastVertexStage;

  if (lastVertexStage == ShaderStageInvalid) {
    assert(isUnlinked()); // Unlinked pipeline only having fragment shader.
    return false;
  }

  return enableXfb();
}

// =====================================================================================================================
// Gets resource usage of the specified shader stage
//
// @param shaderStage : Shader stage
ResourceUsage *PipelineState::getShaderResourceUsage(ShaderStage shaderStage) {
  if (shaderStage == ShaderStageCopyShader)
    shaderStage = ShaderStageGeometry;

  auto &resUsage = MutableArrayRef<std::unique_ptr<ResourceUsage>>(m_resourceUsage)[shaderStage];
  if (!resUsage) {
    resUsage = std::make_unique<ResourceUsage>(shaderStage);
  }
  return &*resUsage;
}

// =====================================================================================================================
// Gets interface data of the specified shader stage
//
// @param shaderStage : Shader stage
InterfaceData *PipelineState::getShaderInterfaceData(ShaderStage shaderStage) {
  if (shaderStage == ShaderStageCopyShader)
    shaderStage = ShaderStageGeometry;

  auto &intfData = MutableArrayRef<std::unique_ptr<InterfaceData>>(m_interfaceData)[shaderStage];
  if (!intfData) {
    intfData = std::make_unique<InterfaceData>();
  }
  return &*intfData;
}

// =====================================================================================================================
// Compute the ExportFormat (as an opaque int) of the specified color export location with the specified output
// type. Only the number of elements of the type is significant.
//
// @param outputTy : Color output type
// @param location : Location
unsigned PipelineState::computeExportFormat(Type *outputTy, unsigned location) {
  const ColorExportFormat *colorExportFormat = &getColorExportFormat(location);
  auto gpuWorkarounds = &getTargetInfo().getGpuWorkarounds();
  unsigned outputMask = outputTy->isVectorTy() ? (1 << cast<FixedVectorType>(outputTy)->getNumElements()) - 1 : 1;
  const auto cbState = &getColorExportState();
  // NOTE: Alpha-to-coverage only takes effect for outputs from color target 0.
  // When dual source blend is enabled, location 1 is location 0 index 1 in shader source. we need generate same export
  // format.
  const bool enableAlphaToCoverage =
      (cbState->alphaToCoverageEnable && ((location == 0) || ((location == 1) && cbState->dualSourceBlendEnable)));

  const bool blendEnabled = colorExportFormat->blendEnable;

  const bool isUnormFormat = (colorExportFormat->nfmt == BufNumFormatUnorm);
  const bool isSnormFormat = (colorExportFormat->nfmt == BufNumFormatSnorm);
  bool isFloatFormat = (colorExportFormat->nfmt == BufNumFormatFloat);
  const bool isUintFormat = (colorExportFormat->nfmt == BufNumFormatUint);
  const bool isSintFormat = (colorExportFormat->nfmt == BufNumFormatSint);
  const bool isSrgbFormat = (colorExportFormat->nfmt == BufNumFormatSrgb);

  if (colorExportFormat->dfmt == BufDataFormat8_8_8 || colorExportFormat->dfmt == BufDataFormat8_8_8_Bgr) {
    // These three-byte formats are handled by pretending they are float.
    isFloatFormat = true;
  }

  const unsigned maxCompBitCount = getMaxComponentBitCount(colorExportFormat->dfmt);

  const bool formatHasAlpha = hasAlpha(colorExportFormat->dfmt);
  const bool alphaExport =
      (outputMask == 0xF && (formatHasAlpha || colorExportFormat->blendSrcAlphaToColor || enableAlphaToCoverage));

  const CompSetting compSetting = computeCompSetting(colorExportFormat->dfmt);

  // Start by assuming EXP_FORMAT_ZERO (no exports)
  ExportFormat expFmt = EXP_FORMAT_ZERO;

  bool supportRbPlus = getTargetInfo().getGpuProperty().supportsRbPlus;

  if (colorExportFormat->dfmt == BufDataFormatInvalid)
    expFmt = EXP_FORMAT_ZERO;
  else if (compSetting == CompSetting::OneCompRed && !alphaExport && !isSrgbFormat &&
           (!supportRbPlus || maxCompBitCount == 32)) {
    // NOTE: When Rb+ is enabled, "R8 UNORM" and "R16 UNORM" shouldn't use "EXP_FORMAT_32_R", instead
    // "EXP_FORMAT_FP16_ABGR" and "EXP_FORMAT_UNORM16_ABGR" should be used for 2X exporting performance.
    expFmt = EXP_FORMAT_32_R;
  } else if (((isUnormFormat || isSnormFormat) && maxCompBitCount <= 10) || (isFloatFormat && maxCompBitCount <= 16) ||
             (isSrgbFormat && maxCompBitCount == 8))
    expFmt = EXP_FORMAT_FP16_ABGR;
  else if (isSintFormat &&
           (maxCompBitCount == 16 ||
            (!static_cast<bool>(gpuWorkarounds->gfx6.cbNoLt16BitIntClamp) && maxCompBitCount < 16)) &&
           !enableAlphaToCoverage) {
    // NOTE: On some hardware, the CB will not properly clamp its input if the shader export format is "UINT16"
    // "SINT16" and the CB format is less than 16 bits per channel. On such hardware, the workaround is picking
    // an appropriate 32-bit export format. If this workaround isn't necessary, then we can choose this higher
    // performance 16-bit export format in this case.
    expFmt = EXP_FORMAT_SINT16_ABGR;
  } else if (isSnormFormat && maxCompBitCount == 16 && !blendEnabled)
    expFmt = EXP_FORMAT_SNORM16_ABGR;
  else if (isUintFormat &&
           (maxCompBitCount == 16 ||
            (!static_cast<bool>(gpuWorkarounds->gfx6.cbNoLt16BitIntClamp) && maxCompBitCount < 16)) &&
           !enableAlphaToCoverage) {
    // NOTE: On some hardware, the CB will not properly clamp its input if the shader export format is "UINT16"
    // "SINT16" and the CB format is less than 16 bits per channel. On such hardware, the workaround is picking
    // an appropriate 32-bit export format. If this workaround isn't necessary, then we can choose this higher
    // performance 16-bit export format in this case.
    expFmt = EXP_FORMAT_UINT16_ABGR;
  } else if (isUnormFormat && maxCompBitCount == 16 && !blendEnabled)
    expFmt = EXP_FORMAT_UNORM16_ABGR;
  else if (((isUintFormat || isSintFormat) || (isFloatFormat && maxCompBitCount > 16) ||
            ((isUnormFormat || isSnormFormat) && maxCompBitCount == 16)) &&
           (compSetting == CompSetting::OneCompRed || compSetting == CompSetting::OneCompAlpha ||
            compSetting == CompSetting::TwoCompAlphaRed))
    expFmt = EXP_FORMAT_32_AR;
  else if (((isUintFormat || isSintFormat) || (isFloatFormat && maxCompBitCount > 16) ||
            ((isUnormFormat || isSnormFormat) && maxCompBitCount == 16)) &&
           compSetting == CompSetting::TwoCompGreenRed && !alphaExport)
    expFmt = EXP_FORMAT_32_GR;
  else if (((isUnormFormat || isSnormFormat) && maxCompBitCount == 16) || (isUintFormat || isSintFormat) ||
           (isFloatFormat && maxCompBitCount > 16))
    expFmt = EXP_FORMAT_32_ABGR;

  return expFmt;
}

// =====================================================================================================================
// Helper macro
#define CASE_CLASSENUM_TO_STRING(TYPE, ENUM)                                                                           \
  case TYPE::ENUM:                                                                                                     \
    string = #ENUM;                                                                                                    \
    break;

// =====================================================================================================================
// Translate enum "ResourceNodeType" to string
//
// @param type : Resource map node type
const char *PipelineState::getResourceNodeTypeName(ResourceNodeType type) {
  const char *string = nullptr;
  switch (type) {
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, Unknown)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorResource)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorSampler)
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
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorReserved13)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, InlineBuffer)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorConstBuffer)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorConstBufferCompact)
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, DescriptorMutable)
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }
  return string;
}

// =====================================================================================================================
// Get name of built-in
//
// @param builtIn : Built-in type, one of the BuiltIn* constants
StringRef PipelineState::getBuiltInName(BuiltInKind builtIn) {
  switch (static_cast<unsigned>(builtIn)) {
#define BUILTIN(name, number, out, in, type)                                                                           \
  case BuiltIn##name:                                                                                                  \
    return #name;
#include "lgc/BuiltInDefs.h"
#undef BUILTIN
  default:
    llvm_unreachable("Should never be called!");
    return "unknown";
  }
}

// =====================================================================================================================
// Determine whether can use tessellation factor optimization
bool PipelineState::canOptimizeTessFactor() {
  if (getTargetInfo().getGfxIpVersion().major < 11)
    return false;
  auto resUsage = getShaderResourceUsage(ShaderStageTessControl);
  auto &perPatchBuiltInOutLocMap = resUsage->inOutUsage.perPatchBuiltInOutputLocMap;
  // Disable tessellation factor optimization if TFs are read in TES or TCS
  if (perPatchBuiltInOutLocMap.count(BuiltInTessLevelOuter) || perPatchBuiltInOutLocMap.count(BuiltInTessLevelInner))
    return false;
  return getOptions().optimizeTessFactor;
}

// =====================================================================================================================
// Set the packable state of generic input/output
//
void PipelineState::initializeInOutPackState() {
  // If the pipeline is not unlinked, the state of input/output pack in specified shader stages is enabled
  if (!isUnlinked()) {
    // The generic input imports of {TCS, GS, FS} are packed by default
    m_inputPackState[ShaderStageTessControl] = true;
    m_inputPackState[ShaderStageGeometry] = true;
    m_inputPackState[ShaderStageFragment] = true;
    // The generic output exports of {VS, TES, GS} are packed by default
    m_outputPackState[ShaderStageVertex] = true;
    m_outputPackState[ShaderStageTessEval] = true;
    m_outputPackState[ShaderStageGeometry] = true;

    // NOTE: For mesh shader, we don't do in-out packing currently in that mesh shader could emit per-vertex outputs
    // and per-primitive outputs, which introduces additional complexity and this complexity increases with the
    // involvement of dynamic indexing.
    if (hasShaderStage(ShaderStageMesh)) {
      m_outputPackState[ShaderStageMesh] = false;
      m_inputPackState[ShaderStageFragment] = false;
    }
  } else {
    // For unlinked shaders, we can do in-out packing if the pipeline has two adjacent shaders.
    // We are assuming that if any of the vertex processing, then the vertex processing stages are complete.  For
    // example, if we see a vertex shader and geometry shader with no tessellation shaders, then we will assume we can
    // pack the vertex outputs and geometry inputs because no tessellation shader will be added later.
    for (ShaderStage stage : lgc::enumRange(ShaderStage::ShaderStageGfxCount)) {
      if ((m_stageMask & shaderStageToMask(stage)) == 0)
        continue;
      if (stage == ShaderStageTessEval)
        continue;
      ShaderStage preStage = getPrevShaderStage(stage);
      if (preStage == ShaderStageInvalid)
        continue;
      m_inputPackState[stage] = true;
      m_outputPackState[preStage] = true;
    }
  }
}

// =====================================================================================================================
// Get whether the input locations of the specified shader stage can be packed
//
// @param shaderStage : The given shader stage
bool PipelineState::canPackInput(ShaderStage shaderStage) {
  ShaderStage preStage = getPrevShaderStage(shaderStage);
  // The input packable state of the current stage should match the output packable state of the previous stage, except
  // that the current stage has no previous and it is a null FS.
  if (preStage != ShaderStageInvalid &&
      !(shaderStage == ShaderStageFragment && getShaderResourceUsage(shaderStage)->inOutUsage.fs.isNullFs))
    assert(m_inputPackState[shaderStage] == m_outputPackState[preStage]);
  return m_inputPackState[shaderStage];
}

// =====================================================================================================================
// Get whether the output locations of the specified shader stage can be packed
//
// @param shaderStage : The given shader stage
bool PipelineState::canPackOutput(ShaderStage shaderStage) {
  ShaderStage nextStage = getNextShaderStage(shaderStage);
  // The output packable state of the current stage should match the input packable state of the next stage, except that
  // the current stage has no next stage or a null FS.
  if (nextStage != ShaderStageInvalid &&
      !(nextStage == ShaderStageFragment && getShaderResourceUsage(nextStage)->inOutUsage.fs.isNullFs))
    assert(m_outputPackState[shaderStage] == m_inputPackState[nextStage]);
  return m_outputPackState[shaderStage];
}

// =====================================================================================================================
// Get the count of vertices per primitive. For GS, the count is for output primitive.
unsigned PipelineState::getVerticesPerPrimitive() {
  if (hasShaderStage(ShaderStageGeometry)) {
    const auto &geometryMode = getShaderModes()->getGeometryShaderMode();
    switch (geometryMode.outputPrimitive) {
    case OutputPrimitives::Points:
      return 1;
    case OutputPrimitives::LineStrip:
      return 2;
    case OutputPrimitives::TriangleStrip:
      return 3;
    default:
      llvm_unreachable("Unexpected output primitive type!");
      return 0;
    }
  } else if (hasShaderStage(ShaderStageTessControl) || hasShaderStage(ShaderStageTessEval)) {
    assert(getInputAssemblyState().primitiveType == PrimitiveType::Patch);
    const auto &tessMode = getShaderModes()->getTessellationMode();
    if (tessMode.pointMode)
      return 1;
    if (tessMode.primitiveMode == PrimitiveMode::Isolines)
      return 2;
    if (tessMode.primitiveMode == PrimitiveMode::Triangles || tessMode.primitiveMode == PrimitiveMode::Quads)
      return 3;
  } else {
    auto primType = getInputAssemblyState().primitiveType;
    switch (primType) {
    case lgc::PrimitiveType::Point:
      return 1;
    case lgc::PrimitiveType::LineList:
    case lgc::PrimitiveType::LineStrip:
      return 2;
    case lgc::PrimitiveType::TriangleList:
    case lgc::PrimitiveType::TriangleStrip:
    case lgc::PrimitiveType::TriangleFan:
    case lgc::PrimitiveType::TriangleListAdjacency:
    case lgc::PrimitiveType::TriangleStripAdjacency:
      return 3;
    default:
      break;
    }
  }

  llvm_unreachable("Unable to get vertices per primitive!");
  return 0;
}

// =====================================================================================================================
// Get the primitive type. For GS, the type is for output primitive.
PrimitiveType PipelineState::getPrimitiveType() {
  if (hasShaderStage(ShaderStageGeometry)) {
    const auto &geometryMode = getShaderModes()->getGeometryShaderMode();
    switch (geometryMode.outputPrimitive) {
    case OutputPrimitives::Points:
      return PrimitiveType::Point;
    case OutputPrimitives::LineStrip:
      return PrimitiveType::LineStrip;
    case OutputPrimitives::TriangleStrip:
      return PrimitiveType::TriangleStrip;
    default:
      llvm_unreachable("Unexpected output primitive type!");
    }
  } else if (hasShaderStage(ShaderStageTessControl) || hasShaderStage(ShaderStageTessEval)) {
    assert(getInputAssemblyState().primitiveType == PrimitiveType::Patch);
    const auto &tessMode = getShaderModes()->getTessellationMode();
    if (tessMode.pointMode)
      return PrimitiveType::Point;
    if (tessMode.primitiveMode == PrimitiveMode::Isolines)
      return PrimitiveType::LineStrip;
    if (tessMode.primitiveMode == PrimitiveMode::Triangles || tessMode.primitiveMode == PrimitiveMode::Quads)
      return PrimitiveType::TriangleStrip;
  } else {
    return getInputAssemblyState().primitiveType;
  }
  llvm_unreachable("Unable to get primitive type!");
  return PrimitiveType::TriangleStrip;
}

// =====================================================================================================================
// Set transform feedback state metadata
//
// @param xfbStateMetadata : XFB state metadata
void PipelineState::setXfbStateMetadata(Module *module) {
  // Read XFB state metadata
  for (auto &func : *module) {
    if (!isShaderEntryPoint(&func))
      continue;
    if (getShaderStage(&func) != getLastVertexProcessingStage())
      continue;
    MDNode *xfbStateMetaNode = func.getMetadata(XfbStateMetadataName);
    if (xfbStateMetaNode) {
      auto &streamXfbBuffers = m_xfbStateMetadata.streamXfbBuffers;
      auto &xfbStrides = m_xfbStateMetadata.xfbStrides;
      for (unsigned xfbBuffer = 0; xfbBuffer < MaxTransformFeedbackBuffers; ++xfbBuffer) {
        // Get the vertex streamId from metadata
        auto metaOp = cast<ConstantAsMetadata>(xfbStateMetaNode->getOperand(2 * xfbBuffer));
        int streamId = cast<ConstantInt>(metaOp->getValue())->getSExtValue();
        if (streamId == InvalidValue)
          continue;
        streamXfbBuffers[streamId] |= 1 << xfbBuffer; // Bit mask of used xfbBuffers in a stream
        // Get the stride from metadata
        metaOp = cast<ConstantAsMetadata>(xfbStateMetaNode->getOperand(2 * xfbBuffer + 1));
        xfbStrides[xfbBuffer] = cast<ConstantInt>(metaOp->getValue())->getZExtValue();
        m_xfbStateMetadata.enableXfb = true;
      }
      m_xfbStateMetadata.enablePrimStats = !m_xfbStateMetadata.enableXfb;
    }
  }
}

// =====================================================================================================================
// Print the pipeline state
//
// @param out : output stream
void PipelineState::print(llvm::raw_ostream &out) const {
  if (m_palMetadata) {
    out << "PAL metadata:\n";
    m_palMetadata->getDocument()->toYAML(out);
  }
  out << "(pipeline state printing is incomplete)\n";
}

// =====================================================================================================================
// Execute this analysis on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this analysis
// @returns : PipelineStateWrapper result object
PipelineStateWrapper::Result PipelineStateWrapper::run(Module &module, ModuleAnalysisManager &analysisManager) {
  if (!m_pipelineState) {
    m_allocatedPipelineState = std::make_unique<PipelineState>(m_builderContext);
    m_pipelineState = &*m_allocatedPipelineState;
    m_pipelineState->readState(&module);
    m_pipelineState->initializeInOutPackState();
  }
  return Result(m_pipelineState);
}

// =====================================================================================================================
// Run PipelineStateClearer pass to clear the pipeline state out of the IR
//
// @param [in/out] module : IR module
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PipelineStateClearer::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  runImpl(module, pipelineState);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Run PipelineStateClearer pass to clear the pipeline state out of the IR
//
// @param [in/out] module : IR module
// @param : PipelineState object to clear
// @returns : True if the module was modified by the transformation and false otherwise
bool PipelineStateClearer::runImpl(Module &module, PipelineState *pipelineState) {
  pipelineState->clear(&module);
  return true;
}

// =====================================================================================================================
AnalysisKey PipelineStateWrapper::Key;

// =====================================================================================================================
//
// @param pipelineState : Pipeline state to wrap
PipelineStateWrapperResult::PipelineStateWrapperResult(PipelineState *pipelineState) : m_pipelineState(pipelineState) {
}

// =====================================================================================================================
//
// @param builderContext : LgcContext
PipelineStateWrapper::PipelineStateWrapper(LgcContext *builderContext) : m_builderContext(builderContext) {
}

// =====================================================================================================================
//
// @param pipelineState : Pipeline state to wrap
PipelineStateWrapper::PipelineStateWrapper(PipelineState *pipelineState) : m_pipelineState(pipelineState) {
}

// =====================================================================================================================
// Print the pipeline state in a human-readable way
//
// @param [in/out] module : IR module
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PipelineStatePrinter::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  pipelineState->print(m_out);
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Record the PipelineState back into the IR, if present
//
// @param [in/out] module : IR module
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PipelineStateRecorder::run(Module &module, ModuleAnalysisManager &analysisManager) {
  if (auto *psw = analysisManager.getCachedResult<PipelineStateWrapper>(module)) {
    PipelineState *pipelineState = psw->getPipelineState();
    pipelineState->record(&module);
  }
  return PreservedAnalyses::none();
}
