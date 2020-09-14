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
 * @file  PipelineState.cpp
 * @brief LLPC source file: contains implementation of class lgc::PipelineState.
 ***********************************************************************************************************************
 */
#include "lgc/state/PipelineState.h"
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
// -pack-in-out: pack input/output
static cl::opt<bool> PackInOut("pack-in-out", cl::desc("Pack input/output"), cl::init(true));

// Names for named metadata nodes when storing and reading back pipeline state
static const char UnlinkedMetadataName[] = "lgc.unlinked";
static const char OptionsMetadataName[] = "lgc.options";
static const char UserDataMetadataName[] = "lgc.user.data.nodes";
static const char DeviceIndexMetadataName[] = "lgc.device.index";
static const char VertexInputsMetadataName[] = "lgc.vertex.inputs";
static const char IaStateMetadataName[] = "lgc.input.assembly.state";
static const char VpStateMetadataName[] = "lgc.viewport.state";
static const char RsStateMetadataName[] = "lgc.rasterizer.state";
static const char ColorExportFormatsMetadataName[] = "lgc.color.export.formats";
static const char ColorExportStateMetadataName[] = "lgc.color.export.state";

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

namespace lgc {
// Create BuilderReplayer pass
ModulePass *createBuilderReplayer(Pipeline *pipeline);
} // namespace lgc

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
    m_palMetadata = new PalMetadata(this);
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
// @param isGlueCode : True if the blob is was generated for glue code.
void PipelineState::mergePalMetadataFromBlob(llvm::StringRef blob, bool isGlueCode) {
  if (!m_palMetadata)
    m_palMetadata = new PalMetadata(this, blob);
  else
    m_palMetadata->mergeFromBlob(blob, isGlueCode);
}

// =====================================================================================================================
// Clear the pipeline state IR metadata.
// This does not clear PalMetadta, because we want that to persist into the back-end.
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
  m_viewportState = {};
  m_rasterizerState = {};
  record(module);
}

// =====================================================================================================================
// Record pipeline state into IR metadata of specified module.
//
// @param [in/out] module : Module to record the IR metadata in
void PipelineState::record(Module *module) {
  getShaderModes()->record(module);
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
    m_palMetadata = new PalMetadata(this, module);
}

// =====================================================================================================================
// Read shaderStageMask from IR. This consists of checking what shader stage functions are present in the IR.
//
// @param module : LLVM module
void PipelineState::readShaderStageMask(Module *module) {
  m_stageMask = 0;
  for (auto &func : *module) {
    if (!func.empty() && func.getLinkage() != GlobalValue::InternalLinkage) {
      auto shaderStage = getShaderStage(&func);
      if (shaderStage != ShaderStageInvalid)
        m_stageMask |= 1 << shaderStage;
    }
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

  for (unsigned stage = shaderStage + 1; stage < ShaderStageGfxCount; ++stage) {
    if ((m_stageMask & shaderStageToMask(static_cast<ShaderStage>(stage))) != 0) {
      nextStage = static_cast<ShaderStage>(stage);
      break;
    }
  }

  return nextStage;
}

// =====================================================================================================================
// Check whether the pipeline is a graphics pipeline
bool PipelineState::isGraphics() const {
  return (getShaderStageMask() &
          ((1U << ShaderStageVertex) | (1U << ShaderStageTessControl) | (1U << ShaderStageTessEval) |
           (1U << ShaderStageGeometry) | (1U << ShaderStageFragment))) != 0;
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
// This also records m_unlinked.
// TODO: The options could be recorded in a more human-readable form, with a string for the option name for each
// option.
//
// @param [in/out] module : Module to record metadata into
void PipelineState::recordOptions(Module *module) {
  if (m_unlinked) {
    unsigned unlinkedAsInt = m_unlinked;
    setNamedMetadataToArrayOfInt32(module, unlinkedAsInt, UnlinkedMetadataName);
  }
  setNamedMetadataToArrayOfInt32(module, m_options, OptionsMetadataName);
  for (unsigned stage = 0; stage != m_shaderOptions.size(); ++stage) {
    std::string metadataName =
        (Twine(OptionsMetadataName) + "." + getShaderStageAbbreviation(static_cast<ShaderStage>(stage))).str();
    setNamedMetadataToArrayOfInt32(module, m_shaderOptions[stage], metadataName);
  }
}

// =====================================================================================================================
// Read pipeline and shader options from IR metadata.
// This also reads m_unlinked.
//
// @param module : Module to read metadata from
void PipelineState::readOptions(Module *module) {
  unsigned unlinkedAsInt = 0;
  readNamedMetadataArrayOfInt32(module, UnlinkedMetadataName, unlinkedAsInt);
  m_unlinked = unlinkedAsInt != 0;

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
    if (node.type == ResourceNodeType::DescriptorTableVaPtr)
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
    if (node.type == ResourceNodeType::DescriptorTableVaPtr) {
      // Process an inner table.
      destInnerTable -= node.innerTable.size();
      destNode.innerTable = ArrayRef<ResourceNode>(destInnerTable, node.innerTable.size());
      setUserDataNodesTable(node.innerTable, destInnerTable, destInnerTable);
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
    assert(node.type < ResourceNodeType::Count);
    // Operand 0: type
    operands.push_back(getResourceTypeName(node.type));
    // Operand 1: offsetInDwords
    operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.offsetInDwords)));
    // Operand 2: sizeInDwords
    operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.sizeInDwords)));

    switch (node.type) {
    case ResourceNodeType::DescriptorTableVaPtr: {
      // Operand 3: Node count in sub-table.
      operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.innerTable.size())));
      // Create the metadata node here.
      userDataMetaNode->addOperand(MDNode::get(getContext(), operands));
      // Create nodes for the sub-table.
      recordUserDataTable(node.innerTable, userDataMetaNode);
      continue;
    }
    case ResourceNodeType::IndirectUserDataVaPtr:
    case ResourceNodeType::StreamOutTableVaPtr: {
      // Operand 3: Size of the indirect data in dwords.
      operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.indirectSizeInDwords)));
      break;
    }
    default: {
      // Operand 3: set
      operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.set)));
      // Operand 4: binding
      operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.binding)));
      // Operand 5: stride
      operands.push_back(ConstantAsMetadata::get(builder.getInt32(node.stride)));
      if (node.immutableValue) {
        // Operand 6 onwards: immutable descriptor constant.
        // Writing the constant array directly does not seem to work, as it does not survive IR linking.
        // Maybe it is a problem with the IR linker when metadata contains a non-ConstantData constant.
        // So we write the individual constant vectors instead.
        // The descriptor is either a sampler (<4 x i32>) or converting sampler (<8 x i32>).
        unsigned elemCount = node.immutableValue->getType()->getArrayNumElements();
        for (unsigned elemIdx = 0; elemIdx != elemCount; ++elemIdx)
          operands.push_back(ConstantAsMetadata::get(ConstantExpr::getExtractValue(node.immutableValue, elemIdx)));
      }
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
    nextNode->type = getResourceTypeFromName(cast<MDString>(metadataNode->getOperand(0)));
    // Operand 1: offsetInDwords
    nextNode->offsetInDwords = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(1))->getZExtValue();
    // Operand 2: sizeInDwords
    nextNode->sizeInDwords = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(2))->getZExtValue();

    if (nextNode->type == ResourceNodeType::DescriptorTableVaPtr) {
      // Operand 3: number of nodes in inner table
      unsigned innerNodeCount = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(3))->getZExtValue();
      // Go into inner table.
      assert(!endThisInnerTable);
      endThisInnerTable = endNextInnerTable;
      endNextInnerTable -= innerNodeCount;
      nextNode = endNextInnerTable;
      nextOuterNode->innerTable = ArrayRef<ResourceNode>(nextNode, innerNodeCount);
      ++nextOuterNode;
    } else {
      if (nextNode->type == ResourceNodeType::IndirectUserDataVaPtr ||
          nextNode->type == ResourceNodeType::StreamOutTableVaPtr) {
        // Operand 3: Size of the indirect data in dwords
        nextNode->indirectSizeInDwords = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(3))->getZExtValue();
      } else {
        // Operand 3: set
        nextNode->set = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(3))->getZExtValue();
        // Operand 4: binding
        nextNode->binding = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(4))->getZExtValue();
        // Operand 5: stride
        nextNode->stride = mdconst::dyn_extract<ConstantInt>(metadataNode->getOperand(5))->getZExtValue();
        nextNode->immutableValue = nullptr;
        if (metadataNode->getNumOperands() >= 7) {
          // Operand 6 onward: immutable descriptor constant
          // The descriptor is either a sampler (<4 x i32>) or converting sampler (<8 x i32>).
          static const unsigned OperandStartIdx = 6;
          unsigned elemCount = metadataNode->getNumOperands() - OperandStartIdx;
          SmallVector<Constant *, 8> descriptors;
          for (unsigned elemIdx = 0; elemIdx != elemCount; ++elemIdx) {
            descriptors.push_back(
                dyn_cast<ConstantAsMetadata>(metadataNode->getOperand(OperandStartIdx + elemIdx))->getValue());
          }
          nextNode->immutableValue =
              ConstantArray::get(ArrayType::get(descriptors[0]->getType(), elemCount), descriptors);
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
const ResourceNode *PipelineState::findPushConstantResourceNode() const {
  for (const ResourceNode &node : getUserDataNodes()) {
    if (node.type == ResourceNodeType::PushConst) {
      return &node;
    }
  }
  return nullptr;
}

// =====================================================================================================================
// Returns true when type nodeType is compatible with candidateType.
// A node type is compatible with a candidate type iff (nodeType) <= (candidateType) in the ResourceNodeType lattice:
//
//                                                        DescriptorCombinedTexture
//                                                                   +
// DescriptorBufferCompact   InlineBuffer                            |
//                   +         +                 +-------------------+--------------------+
//                   |         |                 |                   |                    |
//                   v         v                 v                   v                    v
//                 DescriptorBuffer     DescriptorResource  DescriptorTexelBuffer  DescriptorSampler
//                         +                     +                   +                    +
//                         |                     |                   |                    |
//                         |                     v                   |                    |
//                         +----------------> Unknown <--------------+--------------------+
//
// @param nodeType : Resource node type
// @param candidateType : Resource node candidate type
static bool IsNodeTypeCompatible(ResourceNodeType nodeType, ResourceNodeType candidateType) {
  if (nodeType == ResourceNodeType::Unknown || candidateType == nodeType)
    return true;

  if (nodeType == ResourceNodeType::DescriptorBuffer &&
      (candidateType == ResourceNodeType::DescriptorBufferCompact || candidateType == ResourceNodeType::InlineBuffer))
    return true;

  if ((nodeType == ResourceNodeType::DescriptorResource || nodeType == ResourceNodeType::DescriptorTexelBuffer ||
       nodeType == ResourceNodeType::DescriptorSampler) &&
      candidateType == ResourceNodeType::DescriptorCombinedTexture)
    return true;

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
std::pair<const ResourceNode *, const ResourceNode *>
PipelineState::findResourceNode(ResourceNodeType nodeType, unsigned descSet, unsigned binding) const {
  for (const ResourceNode &node : getUserDataNodes()) {
    if (node.type == ResourceNodeType::DescriptorTableVaPtr) {
      if (nodeType == ResourceNodeType::DescriptorTableVaPtr) {
        assert(!node.innerTable.empty());

        if (node.innerTable[0].set == descSet)
          return {&node, &node};
        continue;
      }

      // Check inner nodes.
      for (const ResourceNode &innerNode : node.innerTable)
        if (innerNode.set == descSet && innerNode.binding == binding && IsNodeTypeCompatible(nodeType, innerNode.type))
          return {&node, &innerNode};
    } else if (node.set == descSet && node.binding == binding && IsNodeTypeCompatible(nodeType, node.type)) {
      return {&node, &node};
    }
  }

  if (nodeType == ResourceNodeType::DescriptorFmask &&
      getOptions().shadowDescriptorTable != ShadowDescriptorTableDisable) {
    // For fmask with -enable-shadow-descriptor-table, if no fmask descriptor is found, look for a resource
    // (image) one instead.
    return findResourceNode(ResourceNodeType::DescriptorResource, descSet, binding);
  }
  return {nullptr, nullptr};
}

// =====================================================================================================================
// Find the single root resource node of the given type
//
// @param nodeType : Type of the resource mapping node
const ResourceNode *PipelineState::findSingleRootResourceNode(ResourceNodeType nodeType) const {
  for (const ResourceNode &node : getUserDataNodes()) {
    if (node.type == nodeType)
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
    for (unsigned type = 0; type < static_cast<unsigned>(ResourceNodeType::Count); ++type) {
      m_resourceNodeTypeNames[type] =
          MDString::get(getContext(), getResourceNodeTypeName(static_cast<ResourceNodeType>(type)));
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
// @param vpState : Viewport state
// @param rsState : Rasterizer state
void PipelineState::setGraphicsState(const InputAssemblyState &iaState, const ViewportState &vpState,
                                     const RasterizerState &rsState) {
  m_inputAssemblyState = iaState;
  m_viewportState = vpState;
  m_rasterizerState = rsState;
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
  setNamedMetadataToArrayOfInt32(module, m_viewportState, VpStateMetadataName);
  setNamedMetadataToArrayOfInt32(module, m_rasterizerState, RsStateMetadataName);
}

// =====================================================================================================================
// Read graphics state (device index, iastate, vpstate, rsstate) from the IR metadata
//
// @param [in/out] module : IR module to read from
void PipelineState::readGraphicsState(Module *module) {
  readNamedMetadataArrayOfInt32(module, IaStateMetadataName, m_inputAssemblyState);
  readNamedMetadataArrayOfInt32(module, VpStateMetadataName, m_viewportState);
  readNamedMetadataArrayOfInt32(module, RsStateMetadataName, m_rasterizerState);
}

// =====================================================================================================================
// Determine whether to use off-chip tessellation mode
bool PipelineState::isTessOffChip() {
  // For GFX9+, always enable tessellation off-chip mode
  return EnableTessOffChip || getLgcContext()->getTargetInfo().getGfxIpVersion().major >= 9;
}

// =====================================================================================================================
// Determine whether to use input/output packing
bool PipelineState::isPackInOut() {
  // Pack input/output requirements:
  // 1) -pack-in-out option is on
  // 2) It supports VS-FS, VS-TCS-TES-(FS)
  if (!PackInOut)
    return false;

  if (hasShaderStage(ShaderStageVertex) && !hasShaderStage(ShaderStageGeometry)) {
    const unsigned nextStage = getNextShaderStage(ShaderStageVertex);
    return nextStage == ShaderStageFragment || nextStage == ShaderStageTessControl;
  }
  return false;
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

  unsigned waveSize = getTargetInfo().getGpuProperty().waveSize;

  if (getTargetInfo().getGfxIpVersion().major >= 10) {
    // NOTE: GPU property wave size is used in shader, unless:
    //  1) A stage-specific default is preferred.
    //  2) If specified by tuning option, use the specified wave size.
    //  3) If gl_SubgroupSize is used in shader, use the specified subgroup size when required.

    if (stage == ShaderStageFragment) {
      // Per programming guide, it's recommended to use wave64 for fragment shader.
      waveSize = 64;
    } else if (hasShaderStage(ShaderStageGeometry)) {
      // Legacy (non-NGG) hardware path for GS does not support wave32.
      waveSize = 64;
    }

    unsigned waveSizeOption = getShaderOptions(stage).waveSize;
    if (waveSizeOption != 0)
      waveSize = waveSizeOption;

    if (stage == ShaderStageGeometry && !hasShaderStage(ShaderStageGeometry)) {
      // NOTE: For NGG, GS could be absent and VS/TES acts as part of it in the merged shader.
      // In such cases, we check the property of VS or TES.
      if (hasShaderStage(ShaderStageTessEval))
        return getShaderWaveSize(ShaderStageTessEval);
      return getShaderWaveSize(ShaderStageVertex);
    }

    // If subgroup size is used in any shader in the pipeline, use the specified subgroup size as wave size.
    if (getShaderModes()->getAnyUseSubgroupSize()) {
      unsigned subgroupSize = getShaderOptions(stage).subgroupSize;
      if (subgroupSize != 0)
        waveSize = subgroupSize;
    }

    assert(waveSize == 32 || waveSize == 64);
  }

  return waveSize;
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
    resUsage.reset(new ResourceUsage(shaderStage));
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
    intfData.reset(new InterfaceData());
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
  GfxIpVersion gfxIp = getTargetInfo().getGfxIpVersion();
  auto gpuWorkarounds = &getTargetInfo().getGpuWorkarounds();
  unsigned outputMask = outputTy->isVectorTy() ? (1 << cast<FixedVectorType>(outputTy)->getNumElements()) - 1 : 1;
  const auto cbState = &getColorExportState();
  // NOTE: Alpha-to-coverage only takes effect for outputs from color target 0.
  const bool enableAlphaToCoverage = (cbState->alphaToCoverageEnable && location == 0);

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

  bool gfx8RbPlusEnable = false;
  if (gfxIp.major == 8 && gfxIp.minor == 1)
    gfx8RbPlusEnable = true;

  if (colorExportFormat->dfmt == BufDataFormatInvalid)
    expFmt = EXP_FORMAT_ZERO;
  else if (compSetting == CompSetting::OneCompRed && !alphaExport && !isSrgbFormat &&
           (!gfx8RbPlusEnable || maxCompBitCount == 32)) {
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
    CASE_CLASSENUM_TO_STRING(ResourceNodeType, InlineBuffer)
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

  // Internal built-ins.
  case BuiltInSamplePosOffset:
    return "SamplePosOffset";
  case BuiltInInterpLinearCenter:
    return "InterpLinearCenter";
  case BuiltInInterpPullMode:
    return "InterpPullMode";

  default:
    llvm_unreachable("Should never be called!");
    return "unknown";
  }
}

// =====================================================================================================================
// Get (create if necessary) the PipelineState from this wrapper pass.
//
// @param module : IR module
PipelineState *PipelineStateWrapper::getPipelineState(Module *module) {
  if (!m_pipelineState) {
    m_allocatedPipelineState.reset(new PipelineState(m_builderContext));
    m_pipelineState = &*m_allocatedPipelineState;
    m_pipelineState->readState(module);
  }
  return m_pipelineState;
}

// =====================================================================================================================
// Pass to clear pipeline state out of the IR
class PipelineStateClearer : public ModulePass {
public:
  PipelineStateClearer() : ModulePass(ID) {}

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineStateWrapper>();
  }

  bool runOnModule(Module &module) override;

  static char ID; // ID of this pass
};

char PipelineStateClearer::ID = 0;

// =====================================================================================================================
// Create pipeline state clearer pass
ModulePass *lgc::createPipelineStateClearer() {
  return new PipelineStateClearer();
}

// =====================================================================================================================
// Run PipelineStateClearer pass to clear the pipeline state out of the IR
//
// @param [in/out] module : IR module
bool PipelineStateClearer::runOnModule(Module &module) {
  auto pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);
  pipelineState->clear(&module);
  return true;
}

// =====================================================================================================================
// Initialize the pipeline state clearer pass
INITIALIZE_PASS(PipelineStateClearer, "llpc-pipeline-state-clearer", "LLPC pipeline state clearer", false, true)

// =====================================================================================================================
char PipelineStateWrapper::ID = 0;

// =====================================================================================================================
//
// @param builderContext : LgcContext
PipelineStateWrapper::PipelineStateWrapper(LgcContext *builderContext)
    : ImmutablePass(ID), m_builderContext(builderContext) {
}

// =====================================================================================================================
// Clean-up of PipelineStateWrapper at end of pass manager run
//
// @param module : Module
bool PipelineStateWrapper::doFinalization(Module &module) {
  return false;
}

// =====================================================================================================================
// Initialize the pipeline state wrapper pass
INITIALIZE_PASS(PipelineStateWrapper, DEBUG_TYPE, "LLPC pipeline state wrapper", false, true)
