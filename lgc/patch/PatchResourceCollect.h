/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchResourceCollect.h
 * @brief LLPC header file: contains declaration of class lgc::PatchResourceCollect.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "llvm/IR/InstVisitor.h"
#include <unordered_set>

namespace lgc {

class InOutLocationMapManager;

// =====================================================================================================================
// Represents the pass of LLVM patching opertions for resource collecting
class PatchResourceCollect : public Patch, public llvm::InstVisitor<PatchResourceCollect> {
public:
  PatchResourceCollect();

  void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineStateWrapper>();
    analysisUsage.addRequired<PipelineShaders>();
    analysisUsage.addPreserved<PipelineShaders>();
  }

  virtual bool runOnModule(llvm::Module &module) override;
  virtual void visitCallInst(llvm::CallInst &callInst);

  static char ID; // ID of this pass

private:
  PatchResourceCollect(const PatchResourceCollect &) = delete;
  PatchResourceCollect &operator=(const PatchResourceCollect &) = delete;

  // Determines whether GS on-chip mode is valid for this pipeline, also computes ES-GS/GS-VS ring item size.
  bool checkGsOnChipValidity();

  // Sets NGG control settings
  void setNggControl(llvm::Module *module);
  bool canUseNgg(llvm::Module *module);
  bool canUseNggCulling(llvm::Module *module);
  void buildNggCullingControlRegister(NggControl &nggControl);
  unsigned getVerticesPerPrimitive() const;

  void processShader();

  bool isVertexReuseDisabled();

  void clearInactiveInput();
  void clearInactiveOutput();

  void matchGenericInOut();
  void mapBuiltInToGenericInOut();

  void mapGsBuiltInOutput(unsigned builtInId, unsigned elemCount);

  void packInOutLocation();
  void fillInOutLocInfoMap();
  void reassembleOutputExportCalls();

  // Input/output scalarizing
  void scalarizeForInOutPacking(llvm::Module *module);
  void scalarizeGenericInput(llvm::CallInst *call);
  void scalarizeGenericOutput(llvm::CallInst *call);

  PipelineShaders *m_pipelineShaders; // Pipeline shaders
  PipelineState *m_pipelineState;     // Pipeline state

  std::vector<llvm::CallInst *> m_deadCalls; // Dead calls

  std::unordered_set<unsigned> m_activeInputLocs;      // Locations of active generic inputs
  std::unordered_set<unsigned> m_activeInputBuiltIns;  // IDs of active built-in inputs
  std::unordered_set<unsigned> m_activeOutputBuiltIns; // IDs of active built-in outputs

  std::unordered_set<unsigned> m_importedOutputLocs;     // Locations of imported generic outputs
  std::unordered_set<unsigned> m_importedOutputBuiltIns; // IDs of imported built-in outputs

  std::vector<llvm::CallInst *> m_inputCalls;  // The scalarzied input import calls
  std::vector<llvm::CallInst *> m_outputCalls; // The scalarized output export calls

  bool m_hasDynIndexedInput;  // Whether dynamic indices are used in generic input addressing (valid
                              // for tessellation shader)
  bool m_hasDynIndexedOutput; // Whether dynamic indices are used in generic output addressing (valid
                              // for tessellation control shader)
  ResourceUsage *m_resUsage;  // Pointer to shader resource usage
  std::unique_ptr<InOutLocationMapManager> m_locationMapManager; // Pointer to InOutLocationMapManager instance
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

// Represents the wrapper of input/output locatoin info, along with handlers
struct InOutLocation {
  uint16_t asIndex() const { return locationInfo.u16All; }

  bool operator<(const InOutLocation &rhs) const { return this->asIndex() < rhs.asIndex(); }

  InOutLocationInfo locationInfo; // The location info of an input or output
};

// =====================================================================================================================
// Represents the manager of input/output locationMap generation
class InOutLocationMapManager {
public:
  InOutLocationMapManager() {}

  void addSpan(llvm::CallInst *call, ShaderStage shaderStage);
  void buildLocationMap(bool checkCompatibility);

  bool findMap(const InOutLocation &originalLocation, const InOutLocation *&newLocation);
  void clearMap() { m_locationMap.clear(); }

  struct LocationSpan {
    uint16_t getCompatibilityKey() const { return compatibilityInfo.u16All; }

    unsigned asIndex() const { return ((getCompatibilityKey() << 16) | firstLocation.asIndex()); }

    bool operator==(const LocationSpan &rhs) const { return this->asIndex() == rhs.asIndex(); }

    bool operator<(const LocationSpan &rhs) const { return this->asIndex() < rhs.asIndex(); }

    InOutLocation firstLocation;
    InOutCompatibilityInfo compatibilityInfo;
  };

private:
  InOutLocationMapManager(const InOutLocationMapManager &) = delete;
  InOutLocationMapManager &operator=(const InOutLocationMapManager &) = delete;

  bool isCompatible(const LocationSpan &rSpan, const LocationSpan &lSpan) const {
    return rSpan.getCompatibilityKey() == lSpan.getCompatibilityKey();
  }

  std::vector<LocationSpan> m_locationSpans; // Tracks spans of contiguous components in the generic input space
  std::map<InOutLocation, InOutLocation> m_locationMap; // The map between original location and new location
};

} // namespace lgc
