/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerSubgroupOps.h
 * @brief LLPC header file: contains declaration of class lgc::LowerSubgroupOps.
 ***********************************************************************************************************************
 */
#pragma once

#include "compilerutils/TypeLowering.h"
#include "llvmraytracing/CpsStackLowering.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcDialect.h"
#include "lgc/patch/Patch.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include <memory>

namespace lgc {

class SubgroupBuilder;

// =====================================================================================================================
// The lower subgroup ops pass
class LowerSubgroupOps : public Patch, public llvm::PassInfoMixin<LowerSubgroupOps> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Lower subgroup ops"; }

private:
  void replace(llvm::CallInst &old, llvm::Value *op);

  void visitElect(SubgroupElectOp &op);
  void visitAny(SubgroupAnyOp &op);
  void visitAll(SubgroupAllOp &op);
  void visitAllEqual(SubgroupAllEqualOp &op);
  void visitRotate(SubgroupRotateOp &op);

  PipelineState *m_pipelineState = nullptr;
  SubgroupBuilder *m_builder = nullptr;
};

} // namespace lgc
