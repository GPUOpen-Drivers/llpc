/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  PalMetadata.cpp
 * @brief LLPC source file: PalMetadata class for manipulating PAL metadata
 *
 * The PalMetadata object can be retrieved using PipelineState::getPalMetadata(), and is used by various parts
 * of LGC to write information to PAL metadata at the time the information is generated. The PalMetadata object
 * is carried through the middle-end, and serialized to IR metadata at the end of the middle-end (or at the
 * point -stop-before etc stops compilation, if earlier).
 ***********************************************************************************************************************
 */
#include "lgc/state/PalMetadata.h"
#include "lgc/state/AbiMetadata.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/VersionTuple.h"

#define DEBUG_TYPE "lgc-pal-metadata"

using namespace lgc;
using namespace llvm;

namespace {

// Structure used to hold the key and the corresponding value in an ArrayMap below.
struct KeyValuePair {
  unsigned key;
  unsigned value;
};

// A type used to represent a map that is stored as a constant array.
using ArrayMap = KeyValuePair[];

// =====================================================================================================================
// Returns an ArrayDocNode associated with the given document that contains the given data.
//
// @param document : The msgpack document the returned array will be associated with.
// @param hash : The hash to place in the array node that will be built.
msgpack::ArrayDocNode buildArrayDocNode(msgpack::Document *document, Hash128 hash) {
  msgpack::ArrayDocNode childNode = document->getArrayNode();
  for (uint32_t i = 0; i < hash.size(); ++i)
    childNode[i] = hash[i];
  return childNode;
}
} // namespace

// =====================================================================================================================
// Construct empty object
//
// @param pipelineState : PipelineState
PalMetadata::PalMetadata(PipelineState *pipelineState) : m_pipelineState(pipelineState) {
  m_document = new msgpack::Document;
  initialize();
}

// =====================================================================================================================
// Constructor given blob of MsgPack PAL metadata
//
// @param pipelineState : PipelineState
// @param blob : MsgPack PAL metadata
PalMetadata::PalMetadata(PipelineState *pipelineState, StringRef blob) : m_pipelineState(pipelineState) {
  m_document = new msgpack::Document;
  bool success = m_document->readFromBlob(blob, /*multi=*/false);
  assert(success && "Bad PAL metadata format");
  ((void)success);
  initialize();
}

// =====================================================================================================================
// Constructor given pipeline IR module. This reads the already-existing PAL metadata if any.
//
// @param pipelineState : PipelineState
// @param module : Pipeline IR module
PalMetadata::PalMetadata(PipelineState *pipelineState, Module *module) : m_pipelineState(pipelineState) {
  m_document = new msgpack::Document;
  NamedMDNode *namedMd = module->getNamedMetadata(PalMetadataName);
  if (namedMd && namedMd->getNumOperands()) {
    // The IR named metadata node contains an MDTuple containing an MDString containing the msgpack data.
    auto mdTuple = dyn_cast<MDTuple>(namedMd->getOperand(0));
    if (mdTuple && mdTuple->getNumOperands()) {
      if (auto mdString = dyn_cast<MDString>(mdTuple->getOperand(0))) {
        bool success = m_document->readFromBlob(mdString->getString(), /*multi=*/false);
        assert(success && "Bad PAL metadata format");
        ((void)success);
      }
    }
  }
  initialize();
}

// =====================================================================================================================
// Destructor
PalMetadata::~PalMetadata() {
  delete m_document;
}

// =====================================================================================================================
// Initialize the PalMetadata object after reading in already-existing PAL metadata if any
void PalMetadata::initialize() {
  // Pre-find (or create) heavily used nodes.
  m_pipelineNode =
      m_document->getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Pipelines].getArray(true)[0].getMap(true);
  m_userDataLimit = &m_pipelineNode[Util::Abi::PipelineMetadataKey::UserDataLimit];
  if (m_userDataLimit->isEmpty())
    *m_userDataLimit = 0U;
  m_spillThreshold = &m_pipelineNode[Util::Abi::PipelineMetadataKey::SpillThreshold];
  if (m_spillThreshold->isEmpty())
    *m_spillThreshold = MAX_SPILL_THRESHOLD;
  for (unsigned stage = ShaderStage::Task; stage < ShaderStage::CountInternal; ++stage) {
    m_pipelineState->setSpillThreshold(static_cast<ShaderStageEnum>(stage), MAX_SPILL_THRESHOLD);
  }
}

// =====================================================================================================================
// Record the PAL metadata into IR metadata in the specified module. This is used both for passing the PAL metadata
// to the AMDGPU back-end, and when compilation stops early due to -stop-before etc.
//
// @param module : Pipeline IR module
void PalMetadata::record(Module *module) {
  // Add the metadata version number.
  auto versionNode = m_document->getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Version].getArray(true);
  versionNode[0] = Util::Abi::PipelineMetadataMajorVersionNew;
  versionNode[1] = Util::Abi::PipelineMetadataMinorVersionNew;

  // Write the MsgPack document into an IR metadata node.
  // The IR named metadata node contains an MDTuple containing an MDString containing the msgpack data.
  std::string blob;
  m_document->writeToBlob(blob);
  MDString *abiMetaString = MDString::get(module->getContext(), blob);
  MDNode *abiMetaNode = MDNode::get(module->getContext(), abiMetaString);
  NamedMDNode *namedMeta = module->getOrInsertNamedMetadata(PalMetadataName);

  if (namedMeta->getNumOperands() == 0) {
    namedMeta->addOperand(abiMetaNode);
  } else {
    // If the PAL metadata was already written, then we need to replace the previous value.
    assert(namedMeta->getNumOperands() == 1);
    namedMeta->setOperand(0, abiMetaNode);
  }
}

// =====================================================================================================================
// Read blob as PAL metadata and merge it into existing PAL metadata (if any)
//
// @param blob : MsgPack PAL metadata to merge
// @param isGlueCode : True if the blob was generated for glue code.
void PalMetadata::mergeFromBlob(llvm::StringRef blob, bool isGlueCode) {
  // Use msgpack::Document::readFromBlob to read the new MsgPack PAL metadata, merging it into the msgpack::Document
  // we already have. We pass it a lambda that determines how to cope with merge conflicts, which returns:
  // -1: failure
  // 0: success; *dest has been set up with the merged node. For an array, 0 means overwrite the existing array
  //    rather than appending.
  auto merger = [isGlueCode](msgpack::DocNode *destNode, msgpack::DocNode srcNode, msgpack::DocNode mapKey) {
    // Allow array and map merging.
    if (srcNode.isMap() && destNode->isMap())
      return 0;
    if (srcNode.isArray() && destNode->isArray())
      return 0;
    // Allow string merging as long as the two strings have the same value. If one string has a "_fetchless"
    // suffix, take the other one. This is for the benefit of the linker linking a fetch shader with a
    // fetchless VS.
    if (destNode->isString() && srcNode.isString()) {
      if (destNode->getString() == srcNode.getString())
        return 0;
      if (srcNode.getString().ends_with("_fetchless"))
        return 0;
      if (destNode->getString().ends_with("_fetchless")) {
        *destNode = srcNode;
        return 0;
      }
      if (srcNode.getString() == "color_export_shader")
        return 0;
      if (destNode->getString() == "color_export_shader") {
        *destNode = srcNode;
        return 0;
      }
    }
    // Disallow merging if they are not in the same type
    if (destNode->getKind() != srcNode.getKind())
      return -1;
    // Special cases of merging.
    if (mapKey.isString()) {
      // For .userdatalimit, register counts, and register limits, take the max value.
      if (mapKey.getString() == Util::Abi::PipelineMetadataKey::UserDataLimit ||
          mapKey.getString() == Util::Abi::HardwareStageMetadataKey::SgprCount ||
          mapKey.getString() == Util::Abi::HardwareStageMetadataKey::SgprLimit ||
          mapKey.getString() == Util::Abi::HardwareStageMetadataKey::VgprCount ||
          mapKey.getString() == Util::Abi::HardwareStageMetadataKey::VgprLimit) {
        *destNode = std::max(destNode->getUInt(), srcNode.getUInt());
        return 0;
      }
      // For .spillthreshold, take the min value.
      if (mapKey.getString() == Util::Abi::PipelineMetadataKey::SpillThreshold) {
        *destNode = std::min(destNode->getUInt(), srcNode.getUInt());
        return 0;
      }
      if (isGlueCode) {
        // For .spi_ps_input_addr/ena, spi_ps_in_control, vgt_shader_stages_en, and float_mode, skip merging
        if (mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::AncillaryEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::FrontFaceEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::LineStippleTexEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::LinearCenterEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::LinearCentroidEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::LinearSampleEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::PerspCenterEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::PerspCentroidEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::PerspPullModelEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::PerspSampleEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::PosFixedPtEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::PosWFloatEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::PosXFloatEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::PosYFloatEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::PosZFloatEna ||
            mapKey.getString() == Util::Abi::SpiPsInputAddrMetadataKey::SampleCoverageEna ||
            mapKey.getString() == Util::Abi::SpiPsInControlMetadataKey::NumInterps ||
            mapKey.getString() == Util::Abi::SpiPsInControlMetadataKey::ParamGen ||
            mapKey.getString() == Util::Abi::SpiPsInControlMetadataKey::NumPrimInterp ||
            mapKey.getString() == Util::Abi::SpiPsInControlMetadataKey::PsW32En ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::DynamicHs ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::EsStageEn ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::GsFastLaunch ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::GsStageEn ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::GsW32En ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::HsStageEn ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::HsW32En ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::LsStageEn ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::MaxPrimgroupInWave ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::NggWaveIdEn ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::PrimgenEn ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::PrimgenPassthruEn ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::PrimgenPassthruNoMsg ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::VsStageEn ||
            mapKey.getString() == Util::Abi::VgtShaderStagesEnMetadataKey::VsW32En ||
            mapKey.getString() == Util::Abi::HardwareStageMetadataKey::FloatMode)
          return 0;
      }
      // Default behavior for uint/bool nodes: "or" the values together.
      if (destNode->getKind() == msgpack::Type::UInt)
        *destNode = destNode->getUInt() | srcNode.getUInt();
      else if (destNode->getKind() == msgpack::Type::Boolean)
        *destNode = destNode->getBool() || srcNode.getBool();
      else
        llvm_unreachable("unsupported type to be merged!");
    }

    return 0;
  };
  bool success = m_document->readFromBlob(blob, /*multi=*/false, merger);

  assert(success && "Bad PAL metadata format");
  ((void)success);
}

// =====================================================================================================================
// Mark that the user data spill table is used at the given offset. The SpillThreshold PAL metadata entry is
// set to the minimum of any call to this function in any shader.
//
// @param dwordOffset : Dword offset that the spill table is used at
// @param shaderStage : The shaderStage for the spill table is used at
void PalMetadata::setUserDataSpillUsage(unsigned dwordOffset, std::optional<ShaderStageEnum> shaderStage) {
  // Update shaderstage spillThreshold in shader level which try to fix the cache issue
  // More details can refer on llpcElfWriter.cpp
  if (dwordOffset < m_pipelineState->getSpillThreshold(shaderStage.value())) {
    m_pipelineState->setSpillThreshold(shaderStage.value(), dwordOffset);
  }
  // Update pipeline spillThreshold
  if (dwordOffset < m_spillThreshold->getUInt())
    *m_spillThreshold = dwordOffset;
}

// =====================================================================================================================
// Fix up registers. Any user data register that has one of the unlinked UserDataMapping values defined in
// AbiUnlinked.h is fixed up by looking at pipeline state; And some dynamic states also need to be fixed.
void PalMetadata::fixUpRegisters() {
  // Fix GS output primitive type (VGT_GS_OUT_PRIM_TYPE). Unlinked compiling VS + NGG, we can't determine
  // the output primitive type, we must fix up it when linking.
  // If pipeline includes GS or TS, the type is from shader, we don't need to fix it. We only must fix a case
  // which includes VS + FS + NGG.
  if (m_pipelineState->isGraphics()) {
    const bool hasTs = m_pipelineState->hasShaderStage(ShaderStage::TessControl) ||
                       m_pipelineState->hasShaderStage(ShaderStage::TessEval);
    const bool hasGs = m_pipelineState->hasShaderStage(ShaderStage::Geometry);
    const bool hasMesh = m_pipelineState->hasShaderStage(ShaderStage::Mesh);
    if (!hasTs && !hasGs && !hasMesh) {
      auto getPrimType = [&]() {
        const auto primType = m_pipelineState->getInputAssemblyState().primitiveType;
        unsigned gsOutputPrimitiveType = 0;
        switch (primType) {
        case PrimitiveType::Point:
          gsOutputPrimitiveType = 0; // POINTLIST
          break;
        case PrimitiveType::LineList:
        case PrimitiveType::LineStrip:
          gsOutputPrimitiveType = 1; // LINESTRIP
          break;
        case PrimitiveType::TriangleList:
        case PrimitiveType::TriangleStrip:
        case PrimitiveType::TriangleFan:
        case PrimitiveType::TriangleListAdjacency:
        case PrimitiveType::TriangleStripAdjacency:
          gsOutputPrimitiveType = 2; // TRISTRIP
          break;
        default:
          llvm_unreachable("Should never be called!");
          break;
        }
        return gsOutputPrimitiveType;
      };
      // Here we use register field to determine if NGG is enabled, because enabling NGG depends on other conditions.
      // see PatchResourceCollect::canUseNgg.
      auto graphicsRegisters = m_pipelineNode[Util::Abi::PipelineMetadataKey::GraphicsRegisters].getMap(true);
      if (graphicsRegisters.find(Util::Abi::GraphicsRegisterMetadataKey::VgtGsOutPrimType) != graphicsRegisters.end()) {
        auto primType = getPrimType();
        auto vgtGsOutPrimType =
            graphicsRegisters[Util::Abi::GraphicsRegisterMetadataKey::VgtGsOutPrimType].getMap(true);
        vgtGsOutPrimType[Util::Abi::VgtGsOutPrimTypeMetadataKey::OutprimType] =
            serializeEnum(Util::Abi::GsOutPrimType(primType));
      }
    }
  }
}

// =====================================================================================================================
// Get shader stage mask (only called for a link-only pipeline whose shader stage mask has not been set yet).
ShaderStageMask PalMetadata::getShaderStageMask() {
  msgpack::MapDocNode shaderStages = m_pipelineNode[Util::Abi::PipelineMetadataKey::Shaders].getMap(true);
  static const struct TableEntry {
    const char *key;
    ShaderStageEnum stage;
  } table[] = {{".compute", ShaderStage::Compute}, {".pixel", ShaderStage::Fragment},
               {".mesh", ShaderStage::Mesh},       {".geometry", ShaderStage::Geometry},
               {".domain", ShaderStage::TessEval}, {".hull", ShaderStage::TessControl},
               {".vertex", ShaderStage::Vertex},   {".task", ShaderStage::Task}};
  ShaderStageMask stageMask;
  for (const auto &entry : ArrayRef<TableEntry>(table)) {
    if (shaderStages.find(m_document->getNode(entry.key)) != shaderStages.end()) {
      msgpack::MapDocNode stageNode = shaderStages[entry.key].getMap(true);
      if (stageNode.find(m_document->getNode(Util::Abi::ShaderMetadataKey::ApiShaderHash)) != stageNode.end())
        stageMask |= ShaderStageMask(entry.stage);
    }
  }
  return stageMask;
}

// =====================================================================================================================
// Finalize PAL metadata user data limit for any compilation (shader, part-pipeline, whole pipeline)
void PalMetadata::finalizeUserDataLimit() {
  if (!m_pipelineState->getUserDataNodes().empty()) {
    // Ensure user_data_limit is set correctly if no user data used or spill is in use.
    // If the spill is used, the entire user data table is considered used.
    if (userDataNodesAreSpilled()) {
      setUserDataLimit();
    }
    // If there are root user data nodes but none of them are used, then PAL wants a non-zero user data limit.
    else if (m_userDataLimit->getUInt() == 0) {
      *m_userDataLimit = 1U;
    }
  }
}

// =====================================================================================================================
// Finalize PAL register settings for pipeline, part-pipeline or shader compilation.
//
// @param isWholePipeline : True if called for a whole pipeline compilation or an ELF link, false if called
//                          for part-pipeline or shader compilation.
void PalMetadata::finalizeRegisterSettings(bool isWholePipeline) {
  assert(m_pipelineState->isGraphics());
  auto graphicsRegNode = m_pipelineNode[Util::Abi::PipelineMetadataKey::GraphicsRegisters].getMap(true);

  if (m_pipelineState->getShaderStageMask().contains(ShaderStage::Fragment)) {
    // There is a divergence between Vulkan/OpenGL and D3D11/12 specs in terms of sample mask output and alpha to
    // coverage. DX spec states that "Alpha to Coverage is disabled if this register (i.e. oMask) is written in a
    // shader." In contrast, the VK spec allows (or rather, does not disallow) enabling both sample mask output and
    // alpha to coverage at the same time.
    auto dbShaderControl = graphicsRegNode[Util::Abi::GraphicsRegisterMetadataKey::DbShaderControl].getMap(true);
    bool alphaToCoverageEnable = m_pipelineState->getColorExportState().alphaToCoverageEnable;
    if (m_pipelineState->getOptions().sampleMaskExportOverridesAlphaToCoverage) {
      dbShaderControl[Util::Abi::DbShaderControlMetadataKey::AlphaToMaskDisable] =
          !alphaToCoverageEnable || dbShaderControl[Util::Abi::DbShaderControlMetadataKey::MaskExportEnable].getBool();
    } else if (alphaToCoverageEnable) {
      dbShaderControl[Util::Abi::DbShaderControlMetadataKey::AlphaToMaskDisable] = false;
    }
  }

  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major == 10) {
    WaveBreak waveBreakSize = m_pipelineState->getShaderOptions(ShaderStage::Fragment).waveBreakSize;
    auto paScShaderControl = graphicsRegNode[Util::Abi::GraphicsRegisterMetadataKey::PaScShaderControl].getMap(true);
    paScShaderControl[Util::Abi::PaScShaderControlMetadataKey::WaveBreakRegionSize] =
        static_cast<unsigned>(waveBreakSize);
  }

  if (m_pipelineState->getRasterizerState().innerCoverage)
    graphicsRegNode[Util::Abi::GraphicsRegisterMetadataKey::AaCoverageToShaderSelect] =
        serializeEnum(Util::Abi::CoverageToShaderSel(INPUT_INNER_COVERAGE));
  else
    graphicsRegNode[Util::Abi::GraphicsRegisterMetadataKey::AaCoverageToShaderSelect] =
        serializeEnum(Util::Abi::CoverageToShaderSel(INPUT_COVERAGE));
}

// =====================================================================================================================
// Finalize SPI_PS_INPUT_CNTL_0_* register setting for pipeline or part-pipeline compilation.
//
// Adjust the value if gl_ViewportIndex is not used in the pre-rasterizer stages.
void PalMetadata::finalizeInputControlRegisterSetting() {
  assert(m_pipelineState->getShaderStageMask().contains(ShaderStage::Fragment));

  unsigned viewportIndexLoc = getFragmentShaderBuiltInLoc(BuiltInViewportIndex);
  if (viewportIndexLoc == InvalidValue) {
    auto &builtInInLocMap =
        m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment)->inOutUsage.builtInInputLocMap;
    auto builtInInputLocMapIt = builtInInLocMap.find(BuiltInViewportIndex);
    if (builtInInputLocMapIt == builtInInLocMap.end())
      return;
    viewportIndexLoc = builtInInputLocMapIt->second;
  }
  assert(viewportIndexLoc != InvalidValue);

  auto usesViewportArrayIndexNode = &m_pipelineNode[Util::Abi::PipelineMetadataKey::UsesViewportArrayIndex];
  if (usesViewportArrayIndexNode->isEmpty())
    *usesViewportArrayIndexNode = false;
  const bool usesViewportArrayIndex = usesViewportArrayIndexNode->getBool();

  if (!usesViewportArrayIndex) {
    auto spiPsInputCntl = m_pipelineNode[Util::Abi::PipelineMetadataKey::GraphicsRegisters]
                              .getMap(true)[Util::Abi::GraphicsRegisterMetadataKey::SpiPsInputCntl]
                              .getArray(true);
    // Check if pointCoordLoc is not used
    auto spiPsInputCntlElem = spiPsInputCntl[viewportIndexLoc].getMap(true);
    if (!spiPsInputCntlElem[Util::Abi::SpiPsInputCntlMetadataKey::PtSpriteTex].getBool()) {
      // Use default value 0 for viewport array index if it is only used in FS (not set in other stages)
      constexpr unsigned defaultVal = (1 << 5);
      spiPsInputCntlElem[Util::Abi::SpiPsInputCntlMetadataKey::Offset] = defaultVal;
      spiPsInputCntlElem[Util::Abi::SpiPsInputCntlMetadataKey::FlatShade] = false;
    }
  }
}

// =====================================================================================================================
// Finalize PAL metadata for pipeline.
//
// @param isWholePipeline : True if called for a whole pipeline compilation or an ELF link, false if called
//                          for part-pipeline or shader compilation.
void PalMetadata::finalizePipeline(bool isWholePipeline) {
  // Ensure user_data_limit is set correctly.
  finalizeUserDataLimit();

  // Finalize register settings.
  if (m_pipelineState->isGraphics())
    finalizeRegisterSettings(isWholePipeline);

  // Set pipeline hash and resource hash.
  auto pipelineHashNode = m_pipelineNode[Util::Abi::PipelineMetadataKey::InternalPipelineHash].getArray(true);
  const auto &options = m_pipelineState->getOptions();
  pipelineHashNode[0] = options.hash[0];
  pipelineHashNode[1] = options.hash[1];
  if (options.resourceHash != 0)
    m_pipelineNode[Util::Abi::PipelineMetadataKey::ResourceHash] = options.resourceHash;

  // Set usesCps if applicable.
  bool usesCps = options.rtIndirectMode == RayTracingIndirectMode::ContinuationsContinufy ||
                 options.rtIndirectMode == RayTracingIndirectMode::Continuations;
  if (usesCps)
    m_pipelineNode[Util::Abi::PipelineMetadataKey::UsesCps] = true;

  // The rest of this function is used only for whole pipeline PAL metadata or an ELF link.
  if (!isWholePipeline)
    return;

  // In the part-pipeline compilation only at ELF link stage do we know how gl_ViewportIndex was used in all stages.
  if (m_pipelineState->getShaderStageMask().contains(ShaderStage::Fragment))
    finalizeInputControlRegisterSetting();

  // Erase the PAL metadata for FS input mappings.
  eraseFragmentInputInfo();
}

// =====================================================================================================================
// Set userDataLimit to maximum (the size of the root user data table, excluding vertex buffer and streamout).
// This is called if spill is in use, or if there are root user data nodes but none of them are used (PAL does
// not like userDataLimit being 0 if there are any root user data nodes).
void PalMetadata::setUserDataLimit() {
  unsigned userDataLimit = 0;
  for (auto &node : m_pipelineState->getUserDataNodes()) {
    if (node.concreteType != ResourceNodeType::IndirectUserDataVaPtr &&
        node.concreteType != ResourceNodeType::StreamOutTableVaPtr)
      userDataLimit = std::max(userDataLimit, node.offsetInDwords + node.sizeInDwords);
  }
  *m_userDataLimit = userDataLimit;
}

// =====================================================================================================================
// Store the color export information in PAL metadata for an exportless fragment shader with shader compilation.
//
// @param info : Array of ColorExportInfo structs
void PalMetadata::addColorExportInfo(ArrayRef<ColorExportInfo> exports) {
  // Each color export is an array containing {location, original location,type}.
  // .colorExports is an array containing the color exports.
  auto colorExportArray = m_pipelineNode[PipelineMetadataKey::ColorExports].getArray(true);
  m_colorExports = colorExportArray;
  for (const ColorExportInfo &exportInfo : exports) {
    msgpack::ArrayDocNode fetchNode = m_document->getArrayNode();
    fetchNode.push_back(m_document->getNode(exportInfo.hwColorTarget));
    fetchNode.push_back(m_document->getNode(exportInfo.location));
    fetchNode.push_back(m_document->getNode(exportInfo.isSigned));
    fetchNode.push_back(m_document->getNode(getTypeName(exportInfo.ty), /*copy=*/true));
    colorExportArray.push_back(fetchNode);
  }
}

// =====================================================================================================================
// Set discard state in the PAL metadata for explicitly building color export shader.
//
// @param enable : Whether this fragment shader has kill enabled.
void PalMetadata::setDiscardState(bool enable) {
  m_pipelineNode[PipelineMetadataKey::DiscardState] = enable;
}

// =====================================================================================================================
// Get the count of color exports needed by the fragment shader.
unsigned PalMetadata::getColorExportCount() {
  if (m_colorExports.isEmpty())
    return 0;
  return m_colorExports.getArray(false).size();
}

// =====================================================================================================================
// Get the color export information out of PAL metadata. Used by the linker to generate the color export shader, and to
// finalize the PS register information.
//
// @param [out] exports : Vector to store info of each export
void PalMetadata::getColorExportInfo(llvm::SmallVectorImpl<ColorExportInfo> &exports) {
  if (m_colorExports.isEmpty()) {
    auto it = m_pipelineNode.find(m_document->getNode(PipelineMetadataKey::ColorExports));
    if (it == m_pipelineNode.end() || !it->second.isArray())
      return;
    m_colorExports = it->second.getArray();
    m_pipelineNode.erase(m_document->getNode(PipelineMetadataKey::ColorExports));
  }
  assert(m_colorExports.isArray());
  auto colorExportArray = m_colorExports.getArray(false);
  unsigned numColorExports = colorExportArray.size();
  for (unsigned i = 0; i != numColorExports; ++i) {
    msgpack::ArrayDocNode fetchNode = colorExportArray[i].getArray();
    unsigned hwMrt = fetchNode[0].getUInt();
    unsigned location = fetchNode[1].getUInt();
    bool isSigned = fetchNode[2].getBool();
    StringRef tyName = fetchNode[3].getString();
    Type *ty = getLlvmType(tyName);
    exports.push_back({hwMrt, location, isSigned, ty});
  }
}

// =====================================================================================================================
// Get the color export information out of PAL metadata. Used by the linker to generate the color export shader, and to
// finalize the PS register information.
//
void PalMetadata::eraseColorExportInfo() {
  m_colorExports = m_document->getEmptyNode();
  m_pipelineNode.erase(m_document->getNode(PipelineMetadataKey::ColorExports));
}

// =====================================================================================================================
// Get the llvm type for that corresponds to tyName.  Returns nullptr if no such type exists.
//
// @param tyName : A type name that is an encoding of a type.
llvm::Type *PalMetadata::getLlvmType(StringRef tyName) const {
  unsigned vecLength = 0;
  if (tyName[0] == 'v') {
    tyName = tyName.drop_front();
    tyName.consumeInteger(10, vecLength);
  }
  Type *ty = nullptr;
  if (tyName == "i8")
    ty = Type::getInt8Ty(m_pipelineState->getContext());
  else if (tyName == "i16")
    ty = Type::getInt16Ty(m_pipelineState->getContext());
  else if (tyName == "i32")
    ty = Type::getInt32Ty(m_pipelineState->getContext());
  else if (tyName == "i64")
    ty = Type::getInt64Ty(m_pipelineState->getContext());
  else if (tyName == "f16")
    ty = Type::getHalfTy(m_pipelineState->getContext());
  else if (tyName == "f32")
    ty = Type::getFloatTy(m_pipelineState->getContext());
  else if (tyName == "f64")
    ty = Type::getDoubleTy(m_pipelineState->getContext());
  if (vecLength != 0 && ty != nullptr)
    ty = FixedVectorType::get(ty, vecLength);
  return ty;
}

// =====================================================================================================================
// Updates the SPI_SHADER_COL_FORMAT entry.
//
void PalMetadata::updateSpiShaderColFormat(ArrayRef<ExportFormat> expFormats) {
  unsigned spiShaderColFormat = 0;
  for (auto [i, expFormat] : enumerate(expFormats))
    spiShaderColFormat |= expFormat << (4 * i);

  auto spiShaderColFormatNode = m_pipelineNode[Util::Abi::PipelineMetadataKey::GraphicsRegisters]
                                    .getMap(true)[Util::Abi::GraphicsRegisterMetadataKey::SpiShaderColFormat]
                                    .getMap(true);
  spiShaderColFormatNode[Util::Abi::SpiShaderColFormatMetadataKey::Col_0ExportFormat] = spiShaderColFormat & 0xF;
  spiShaderColFormatNode[Util::Abi::SpiShaderColFormatMetadataKey::Col_1ExportFormat] = (spiShaderColFormat >> 4) & 0xF;
  spiShaderColFormatNode[Util::Abi::SpiShaderColFormatMetadataKey::Col_2ExportFormat] = (spiShaderColFormat >> 8) & 0xF;
  spiShaderColFormatNode[Util::Abi::SpiShaderColFormatMetadataKey::Col_3ExportFormat] =
      (spiShaderColFormat >> 12) & 0xF;
  spiShaderColFormatNode[Util::Abi::SpiShaderColFormatMetadataKey::Col_4ExportFormat] =
      (spiShaderColFormat >> 16) & 0xF;
  spiShaderColFormatNode[Util::Abi::SpiShaderColFormatMetadataKey::Col_5ExportFormat] =
      (spiShaderColFormat >> 20) & 0xF;
  spiShaderColFormatNode[Util::Abi::SpiShaderColFormatMetadataKey::Col_6ExportFormat] =
      (spiShaderColFormat >> 24) & 0xF;
  spiShaderColFormatNode[Util::Abi::SpiShaderColFormatMetadataKey::Col_7ExportFormat] =
      (spiShaderColFormat >> 28) & 0xF;
}

// =====================================================================================================================
// Updates the CB_SHADER_MASK entry.
//
void PalMetadata::updateCbShaderMask(unsigned cbShaderMask) {
  auto cbShaderMaskNode = m_pipelineNode[Util::Abi::PipelineMetadataKey::GraphicsRegisters]
                              .getMap(true)[Util::Abi::GraphicsRegisterMetadataKey::CbShaderMask]
                              .getMap(true);
  cbShaderMaskNode[Util::Abi::CbShaderMaskMetadataKey::Output0Enable] = cbShaderMask & 0xF;
  cbShaderMaskNode[Util::Abi::CbShaderMaskMetadataKey::Output1Enable] = (cbShaderMask >> 4) & 0xF;
  cbShaderMaskNode[Util::Abi::CbShaderMaskMetadataKey::Output2Enable] = (cbShaderMask >> 8) & 0xF;
  cbShaderMaskNode[Util::Abi::CbShaderMaskMetadataKey::Output3Enable] = (cbShaderMask >> 12) & 0xF;
  cbShaderMaskNode[Util::Abi::CbShaderMaskMetadataKey::Output4Enable] = (cbShaderMask >> 16) & 0xF;
  cbShaderMaskNode[Util::Abi::CbShaderMaskMetadataKey::Output5Enable] = (cbShaderMask >> 20) & 0xF;
  cbShaderMaskNode[Util::Abi::CbShaderMaskMetadataKey::Output6Enable] = (cbShaderMask >> 24) & 0xF;
  cbShaderMaskNode[Util::Abi::CbShaderMaskMetadataKey::Output7Enable] = (cbShaderMask >> 28) & 0xF;
}

// =====================================================================================================================
//  Updates the DB shader control that depends on the CB state.
//
void PalMetadata::updateDbShaderControl() {
  auto graphicsRegNode = m_pipelineNode[Util::Abi::PipelineMetadataKey::GraphicsRegisters].getMap(true);
  auto dbShaderControl = graphicsRegNode[Util::Abi::GraphicsRegisterMetadataKey::DbShaderControl].getMap(true);
  bool alphaToCoverageEnable = m_pipelineState->getColorExportState().alphaToCoverageEnable;
  if (m_pipelineState->getOptions().sampleMaskExportOverridesAlphaToCoverage) {
    dbShaderControl[Util::Abi::DbShaderControlMetadataKey::AlphaToMaskDisable] =
        !alphaToCoverageEnable || dbShaderControl[Util::Abi::DbShaderControlMetadataKey::MaskExportEnable].getBool();
  } else {
    dbShaderControl[Util::Abi::DbShaderControlMetadataKey::AlphaToMaskDisable] = !alphaToCoverageEnable;
  }
}

// =====================================================================================================================
// Sets the SPI_SHADER_Z_FORMAT entry.
//
// @param zExportFormat : new z-export-format
void PalMetadata::setSpiShaderZFormat(unsigned zExportFormat) {
  auto graphicsRegNode = m_pipelineNode[Util::Abi::PipelineMetadataKey::GraphicsRegisters].getMap(true);
  graphicsRegNode[Util::Abi::GraphicsRegisterMetadataKey::SpiShaderZFormat] = zExportFormat;
}

// =====================================================================================================================
// Fills the xglCacheInfo section of the PAL metadata with the given data.
//
// @param finalizedCacheHash : The finalized hash that will be used to look for this pipeline in caches.
// @param version : The version of llpc that generated the hash.
void PalMetadata::setFinalized128BitCacheHash(const Hash128 &finalizedCacheHash, const VersionTuple &version) {
  msgpack::MapDocNode &xglCacheInfo = m_pipelineNode[Util::Abi::PipelineMetadataKey::XglCacheInfo].getMap(true);
  xglCacheInfo[Util::Abi::PipelineMetadataKey::CacheHash128Bits] = buildArrayDocNode(m_document, finalizedCacheHash);
  xglCacheInfo[Util::Abi::PipelineMetadataKey::LlpcVersion] = m_document->getNode(version.getAsString(),
                                                                                  /* copy = */ true);
}

// =====================================================================================================================
// Store the fragment shader input mapping information for a fragment shader being compiled by itself (part-
// pipeline compilation).
//
// @param fsInputMappings : FS input mapping information
void PalMetadata::addFragmentInputInfo(const FsInputMappings &fsInputMappings) {
  auto fragInputMappingArray1 = m_pipelineNode[PipelineMetadataKey::FragInputMapping1].getArray(true);
  for (std::pair<unsigned, unsigned> item : fsInputMappings.locationInfo) {
    fragInputMappingArray1.push_back(m_document->getNode(item.first));
    fragInputMappingArray1.push_back(m_document->getNode(item.second));
  }
  auto fragInputMappingArray2 = m_pipelineNode[PipelineMetadataKey::FragInputMapping2].getArray(true);
  for (std::pair<unsigned, unsigned> item : fsInputMappings.builtInLocationInfo) {
    fragInputMappingArray2.push_back(m_document->getNode(item.first));
    fragInputMappingArray2.push_back(m_document->getNode(item.second));
  }
  auto fragInputMappingArray3 = m_pipelineNode[PipelineMetadataKey::FragInputMapping3].getArray(true);
  fragInputMappingArray3.push_back(m_document->getNode(fsInputMappings.clipDistanceCount));
  fragInputMappingArray3.push_back(m_document->getNode(fsInputMappings.cullDistanceCount));
}

// =====================================================================================================================
// In part-pipeline compilation, copy any metadata needed from the "other" pipeline's PAL metadata into ours.
// Currently this just copies the fragment shader input mapping information.
// Storing it in our PAL metadata, even though it will be erased before the PAL metadata is written into the ELF,
// means that it persists in the IR if compilation is interrupted by e.g. -emit-lgc.
//
// @param other : PAL metadata object for "other" pipeline
void PalMetadata::setOtherPartPipeline(PalMetadata &other) {
  m_pipelineNode[PipelineMetadataKey::FragInputMapping1] = other.m_pipelineNode[PipelineMetadataKey::FragInputMapping1];
  m_pipelineNode[PipelineMetadataKey::FragInputMapping2] = other.m_pipelineNode[PipelineMetadataKey::FragInputMapping2];
  m_pipelineNode[PipelineMetadataKey::FragInputMapping3] = other.m_pipelineNode[PipelineMetadataKey::FragInputMapping3];
}

// =====================================================================================================================
// Store client-defined metadata blob in a buffer node.
//
// @param clientMetadata : StringRef representing the client metadata blob
void PalMetadata::setClientMetadata(StringRef clientMetadata) {
  if (!clientMetadata.empty())
    m_pipelineNode[Util::Abi::PipelineMetadataKey::ApiCreateInfo] = MemoryBufferRef(clientMetadata, "");
}

// =====================================================================================================================
// Check whether we have FS input mappings, and thus whether we're doing part-pipeline compilation of the
// pre-FS part of the pipeline.
bool PalMetadata::haveFsInputMappings() {
  return m_pipelineNode.find(m_document->getNode(PipelineMetadataKey::FragInputMapping1)) != m_pipelineNode.end();
}

// =====================================================================================================================
// In part-pipeline compilation, get a blob of data representing the FS input mappings that can be used by the
// client in a hash. The resulting data is owned by the PalMetadata object, and lives until the PalMetadata
// object is destroyed or another call is made to getFsInputMappings.
StringRef PalMetadata::getFsInputMappings() {
  assert(haveFsInputMappings());
  m_fsInputMappingsBlob.clear();
  auto fragInputMappingArray1 = m_pipelineNode[PipelineMetadataKey::FragInputMapping1].getArray(true);
  auto fragInputMappingArray2 = m_pipelineNode[PipelineMetadataKey::FragInputMapping2].getArray(true);
  auto fragInputMappingArray3 = m_pipelineNode[PipelineMetadataKey::FragInputMapping3].getArray(true);
  for (auto &element : fragInputMappingArray1) {
    unsigned elementVal = element.getUInt();
    m_fsInputMappingsBlob.append(StringRef(reinterpret_cast<const char *>(&elementVal), sizeof(elementVal)));
  }
  for (auto &element : fragInputMappingArray2) {
    unsigned elementVal = element.getUInt();
    m_fsInputMappingsBlob.append(StringRef(reinterpret_cast<const char *>(&elementVal), sizeof(elementVal)));
  }
  for (auto &element : fragInputMappingArray3) {
    unsigned elementVal = element.getUInt();
    m_fsInputMappingsBlob.append(StringRef(reinterpret_cast<const char *>(&elementVal), sizeof(elementVal)));
  }
  return m_fsInputMappingsBlob;
}

// =====================================================================================================================
// In part-pipeline compilation, retrieve the FS input mappings into the provided vectors.
//
// This is used when compiling the non-FS part pipeline.
// The function copes with that metadata not being present, as it can be called when doing a compile of a single
// shader in the command-line tool.
//
// @param [out] fsInputMappings : Struct to populate with FS input mapping information
void PalMetadata::retrieveFragmentInputInfo(FsInputMappings &fsInputMappings) {
  auto array1It = m_pipelineNode.find(m_document->getNode(PipelineMetadataKey::FragInputMapping1));
  if (array1It != m_pipelineNode.end()) {
    auto fragInputMappingArray1 = array1It->second.getArray(true);
    for (unsigned idx = 0; idx < fragInputMappingArray1.size() / 2; ++idx)
      fsInputMappings.locationInfo.push_back(
          {fragInputMappingArray1[idx * 2].getUInt(), fragInputMappingArray1[idx * 2 + 1].getUInt()});
  }

  auto array2It = m_pipelineNode.find(m_document->getNode(PipelineMetadataKey::FragInputMapping2));
  if (array2It != m_pipelineNode.end()) {
    auto fragInputMappingArray2 = array2It->second.getArray(true);
    for (unsigned idx = 0; idx < fragInputMappingArray2.size() / 2; ++idx)
      fsInputMappings.builtInLocationInfo.push_back(
          {fragInputMappingArray2[idx * 2].getUInt(), fragInputMappingArray2[idx * 2 + 1].getUInt()});
  }

  auto array3It = m_pipelineNode.find(m_document->getNode(PipelineMetadataKey::FragInputMapping3));
  if (array3It != m_pipelineNode.end()) {
    auto fragInputMappingArray3 = array3It->second.getArray(true);
    if (fragInputMappingArray3.size() >= 1) {
      fsInputMappings.clipDistanceCount = fragInputMappingArray3[0].getUInt();
      if (fragInputMappingArray3.size() >= 2)
        fsInputMappings.cullDistanceCount = fragInputMappingArray3[1].getUInt();
    }
  }
}

// =====================================================================================================================
// Erase the PAL metadata for FS input mappings. Used when finalizing the PAL metadata in the link.
void PalMetadata::eraseFragmentInputInfo() {
  auto array1It = m_pipelineNode.find(m_document->getNode(PipelineMetadataKey::FragInputMapping1));
  if (array1It != m_pipelineNode.end())
    m_pipelineNode.erase(array1It);

  auto array2It = m_pipelineNode.find(m_document->getNode(PipelineMetadataKey::FragInputMapping2));
  if (array2It != m_pipelineNode.end())
    m_pipelineNode.erase(array2It);

  auto array3It = m_pipelineNode.find(m_document->getNode(PipelineMetadataKey::FragInputMapping3));
  if (array3It != m_pipelineNode.end())
    m_pipelineNode.erase(array3It);
}

// =====================================================================================================================
// Returns the location of the fragment builtin or InvalidValue if the builtin is not found.
//
// @param builtin : Fragment builtin to check.
unsigned PalMetadata::getFragmentShaderBuiltInLoc(unsigned builtin) {
  auto array2It = m_pipelineNode.find(m_document->getNode(PipelineMetadataKey::FragInputMapping2));
  if (array2It != m_pipelineNode.end()) {
    auto fragInputMappingArray2 = array2It->second.getArray(true);
    for (unsigned idx = 0, e = fragInputMappingArray2.size() / 2; idx != e; ++idx) {
      if (fragInputMappingArray2[idx * 2].getUInt() == builtin)
        return fragInputMappingArray2[idx * 2 + 1].getUInt();
    }
  }
  return InvalidValue;
}

// =====================================================================================================================
// Serialize Util::Abi::CoverageToShaderSel to a string.
//
// @param value : The input enum of Util::Abi::CoverageToShaderSel
llvm::StringRef PalMetadata::serializeEnum(Util::Abi::CoverageToShaderSel value) {
  switch (value) {
  case Util::Abi::CoverageToShaderSel::InputCoverage:
    return "InputCoverage";
  case Util::Abi::CoverageToShaderSel::InputInnerCoverage:
    return "InputInnerCoverage";
  case Util::Abi::CoverageToShaderSel::InputDepthCoverage:
    return "InputDepthCoverage";
  case Util::Abi::CoverageToShaderSel::Raw:
    return "Raw";
  default:
    llvm_unreachable("Unexpected Util::Abi::CoverageToShaderSel enum");
    return "";
  }
}

// =====================================================================================================================
// Serialize Util::Abi::PointSpriteSelect to a string.
//
// @param value : The input enum of Util::Abi::PointSpriteSelect
llvm::StringRef PalMetadata::serializeEnum(Util::Abi::PointSpriteSelect value) {
  switch (value) {
  case Util::Abi::PointSpriteSelect::Zero:
    return "Zero";
  case Util::Abi::PointSpriteSelect::One:
    return "One";
  case Util::Abi::PointSpriteSelect::S:
    return "S";
  case Util::Abi::PointSpriteSelect::T:
    return "T";
  case Util::Abi::PointSpriteSelect::None:
    return "None";
  default:
    llvm_unreachable("Unexpected Util::Abi::PointSpriteSelect");
    return "";
  }
}

// =====================================================================================================================
// Serialize Util::Abi::GsOutPrimType to a string.
//
// @param value : The input enum of Util::Abi::GsOutPrimType
llvm::StringRef PalMetadata::serializeEnum(Util::Abi::GsOutPrimType value) {
  switch (value) {
  case Util::Abi::GsOutPrimType::PointList:
    return "PointList";
  case Util::Abi::GsOutPrimType::LineStrip:
    return "LineStrip";
  case Util::Abi::GsOutPrimType::TriStrip:
    return "TriStrip";
  case Util::Abi::GsOutPrimType::Rect2d:
    return "Rect2d";
  case Util::Abi::GsOutPrimType::RectList:
    return "RectList";
  default:
    llvm_unreachable("Unexpected Util::Abi::GsOutPrimType");
    return "";
  }
}

// =====================================================================================================================
// Set userDataLimit to the given value if it is bigger than m_userDataLimit
//
// @param value : The given value to update m_userDataLimit
void PalMetadata::setUserDataLimit(unsigned value) {
  if (value > m_userDataLimit->getUInt())
    *m_userDataLimit = value;
}

// =====================================================================================================================
// Set Util::Abi::PipelineMetadataKey::PsDummyExport to true
void PalMetadata::setPsDummyExport() {
  m_pipelineNode[Util::Abi::PipelineMetadataKey::PsDummyExport] = true;
}
