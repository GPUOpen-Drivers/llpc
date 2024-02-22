/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchResourceCollect.h
 * @brief LLPC header file: contains declaration of class lgc::PatchResourceCollect.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "llvm/IR/InstVisitor.h"
#include <set>
#include <unordered_set>

namespace lgc {

class GenericLocationOp;
class OutputImportGenericOp;

class InOutLocationInfoMapManager;
typedef std::map<InOutLocationInfo, InOutLocationInfo> InOutLocationInfoMap;

// =====================================================================================================================
// Represents the pass of LLVM patching operations for resource collecting
class PatchResourceCollect : public Patch,
                             public llvm::InstVisitor<PatchResourceCollect>,
                             public llvm::PassInfoMixin<PatchResourceCollect> {
public:
  PatchResourceCollect();

  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  virtual void visitCallInst(llvm::CallInst &callInst);

  static llvm::StringRef name() { return "Patch LLVM for resource collecting"; }

private:
  // Determines whether GS on-chip mode is valid for this pipeline, also computes ES-GS/GS-VS ring item size.
  bool checkGsOnChipValidity();

  // Sets NGG control settings
  void setNggControl(llvm::Module *module);
  bool canUseNgg(llvm::Module *module);
  bool canUseNggCulling(llvm::Module *module);

  void processShader();
  void processMissingFs();

  bool isVertexReuseDisabled();
  void checkRayQueryLdsStackUsage(llvm::Module *module);

  void clearInactiveBuiltInInput();
  void clearInactiveBuiltInOutput();
  void clearUnusedOutput();
  void clearUndefinedOutput();

  void matchGenericInOut();
  void mapBuiltInToGenericInOut();

  void mapGsBuiltInOutput(unsigned builtInId, unsigned elemCount);

  void updateInputLocInfoMapWithUnpack();
  void updateOutputLocInfoMapWithUnpack();
  bool canChangeOutputLocationsForGs();
  void updateInputLocInfoMapWithPack();
  void updateOutputLocInfoMapWithPack();
  void reassembleOutputExportCalls();

  // Input/output scalarizing
  void scalarizeForInOutPacking(llvm::Module *module);
  void scalarizeGenericInput(GenericLocationOp *input);
  void scalarizeGenericOutput(llvm::CallInst *call);

  PipelineShadersResult *m_pipelineShaders; // Pipeline shaders
  PipelineState *m_pipelineState;           // Pipeline state

  std::vector<llvm::CallInst *> m_deadCalls; // Dead calls

  std::unordered_set<unsigned> m_activeInputBuiltIns;  // IDs of active built-in inputs
  std::unordered_set<unsigned> m_activeOutputBuiltIns; // IDs of active built-in outputs

  std::unordered_set<unsigned> m_importedOutputBuiltIns; // IDs of imported built-in outputs

  std::vector<OutputImportGenericOp *> m_importedOutputCalls; // The output import calls
  std::vector<GenericLocationOp *> m_inputCalls;              // The input import calls
  std::vector<llvm::CallInst *> m_outputCalls;                // The output export calls

  ResourceUsage *m_resUsage; // Pointer to shader resource usage
  std::unique_ptr<InOutLocationInfoMapManager>
      m_locationInfoMapManager; // Pointer to InOutLocationInfoMapManager instance

  bool m_tcsInputHasDynamicIndexing = false; // Whether there is a dynamically indexed TCS input.
  bool m_processMissingFs = false;           // Whether to process a missing FS (part-pipeline compilation).
};

// Represents the compatibility info of input/output
union InOutCompatibilityInfo {
  struct {
    uint16_t halfComponentCount : 9; // The number of components measured in times of 16-bits.
                                     // A single 32-bit component will be halfComponentCount=2
    uint16_t is16Bit : 1;            // 16-bit (i8/i16/f16, i8 is treated as 16-bit) or not
    uint16_t isFlat : 1;             // Flat shading or not
    uint16_t isCustom : 1;           // Custom interpolation mode or not
  };
  uint16_t u16All;
};

// =====================================================================================================================
// Represents the manager of input/output locationInfoMap generation
class InOutLocationInfoMapManager {
public:
  InOutLocationInfoMapManager() {}

  void createMap(llvm::ArrayRef<GenericLocationOp *> calls, ShaderStageEnum shaderStage, bool requireDword);
  void createMap(const std::vector<InOutLocationInfo> &locInfos, ShaderStageEnum shaderStage);
  void deserializeMap(llvm::ArrayRef<std::pair<unsigned, unsigned>> serializedMap);
  bool findMap(const InOutLocationInfo &origLocInfo, InOutLocationInfoMap::const_iterator &mapIt);
  InOutLocationInfoMap &getMap() { return m_locationInfoMap; }

  struct LocationSpan {
    uint16_t getCompatibilityKey() const { return compatibilityInfo.u16All; }

    unsigned asIndex() const { return ((getCompatibilityKey() << 16) | firstLocationInfo.getData()); }

    bool operator==(const LocationSpan &rhs) const { return this->asIndex() == rhs.asIndex(); }

    bool operator<(const LocationSpan &rhs) const { return this->asIndex() < rhs.asIndex(); }

    InOutLocationInfo firstLocationInfo;
    InOutCompatibilityInfo compatibilityInfo;
  };

private:
  InOutLocationInfoMapManager(const InOutLocationInfoMapManager &) = delete;
  InOutLocationInfoMapManager &operator=(const InOutLocationInfoMapManager &) = delete;

  void addSpan(llvm::CallInst *call, ShaderStageEnum shaderStage, bool requireDword);
  void buildMap(ShaderStageEnum shaderStage);

  bool isCompatible(const LocationSpan &rSpan, const LocationSpan &lSpan, ShaderStageEnum shaderStage) const {
    bool isCompatible = rSpan.getCompatibilityKey() == lSpan.getCompatibilityKey();
    if (isCompatible && shaderStage == ShaderStage::Geometry) {
      // Outputs with the same stream id are packed together
      isCompatible &= rSpan.firstLocationInfo.getStreamId() == lSpan.firstLocationInfo.getStreamId();
    }
    return isCompatible;
  }

  std::set<LocationSpan> m_locationSpans; // Tracks spans of contiguous components in the generic input space
  InOutLocationInfoMap m_locationInfoMap; // The map between original location and new location
};

} // namespace lgc
