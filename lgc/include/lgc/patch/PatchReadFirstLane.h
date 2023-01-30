/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchReadFirstLane.h
 * @brief LLPC header file: contains declaration of class lgc::PatchReadFirstLane.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/util/BuilderBase.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class TargetTransformInfo;

} // namespace llvm

namespace lgc {

class PatchReadFirstLane final : public llvm::PassInfoMixin<PatchReadFirstLane> {
public:
  PatchReadFirstLane();
  llvm::PreservedAnalyses run(llvm::Function &function, llvm::FunctionAnalysisManager &analysisManager);

  // NOTE: Once the switch to the new pass manager is completed, the isDivergentUse and TTI arguments can be removed and
  // put back as class attributes.
  bool runImpl(llvm::Function &function, std::function<bool(const llvm::Use &)> isDivergentUse,
               llvm::TargetTransformInfo *targetTransformInfo);

  static llvm::StringRef name() { return "Patch LLVM for readfirstlane optimizations"; }

private:
  bool promoteEqualUniformOps(llvm::Function &function);
  bool liftReadFirstLane(llvm::Function &function);
  void collectAssumeUniforms(llvm::BasicBlock *block,
                             const llvm::SmallVectorImpl<llvm::Instruction *> &initialReadFirstLanes);
  void findBestInsertLocation(const llvm::SmallVectorImpl<llvm::Instruction *> &initialReadFirstLanes);
  bool isAllUsersAssumedUniform(llvm::Instruction *inst);
  void applyReadFirstLane(llvm::Instruction *inst, BuilderBase &builder);

  // We only support to apply amdgcn_readfirstlane on float or int type
  // TODO: Support various types when backend work is ready
  bool isSupportedType(llvm::Instruction *inst) {
    return inst->getType()->isFloatTy() || inst->getType()->isIntegerTy(32);
  }

  std::function<bool(const llvm::Use &)> m_isDivergentUse;
  llvm::TargetTransformInfo *m_targetTransformInfo; // The target transform info to determine stop propagation
  llvm::DenseMap<llvm::Instruction *, llvm::SmallVector<llvm::Instruction *, 2>>
      m_uniformDivergentUsesMap; // The map key is an instruction `I` that can be assumed uniform.
                                 // That is, we can apply readfirstlane to the result of `I` and remain
                                 // correct. If the map value vector is non-empty, it contains a list of
                                 // instructions that we can apply readfirstlane on to achieve the same effect
                                 // as a readfirstlane on `I`. An empty vector means that it is not possible to
                                 // life a readfirstlane beyond `I`.
  llvm::DenseSet<llvm::Instruction *> m_insertLocations; // The insert locations of readfirstlane
};

} // namespace lgc
