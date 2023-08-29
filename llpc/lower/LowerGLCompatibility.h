/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerGLCompatibility.h
 * @brief LLPC header file: contains declaration of Llpc::LowerGLCompatibility
 ***********************************************************************************************************************
 */
#pragma once

#include "SPIRVInternal.h"
#include "llpcSpirvLower.h"
#include "llvm/IR/PassManager.h"

namespace Llpc {

// =====================================================================================================================
// Represents the pass of SPIR-V lowering ray query post inline.
class LowerGLCompatibility : public SpirvLower, public llvm::PassInfoMixin<LowerGLCompatibility> {
public:
  LowerGLCompatibility();
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "Lower GLSL compatibility variables and operations"; }

private:
  bool needRun();
  unsigned getUniformLocation(llvm::GlobalVariable *var);
  void decodeInOutMetaRecursively(llvm::Type *valueTy, llvm::Constant *mds,
                                  llvm::SmallVector<ShaderInOutMetadata> &out);
  void unifyFunctionReturn(Function *func);
  void collectEmitInst();
  void collectEmulationResource();
  void buildPatchPositionInfo();

  // The function use to lower gl_ClipVertex
  bool needLowerClipVertex();
  void createClipDistance();
  void createClipPlane();
  void emulateStoreClipVertex();
  void lowerClipVertex();

  llvm::SmallVector<llvm::CallInst *> m_emitCalls; // "Call" instructions to emit vertex (geometry shader).
  llvm::ReturnInst *m_retInst;                     // "Return" of the entry point.

  // The resource use to lower gl_ClipVertex
  llvm::GlobalVariable *m_clipVertex;   // The global variable of gl_ClipVertex
  llvm::GlobalVariable *m_clipDistance; // The global variable of gl_ClipDistance
  llvm::GlobalVariable *m_clipPlane;    // The global variable of gl_ClipPlane
};

} // namespace Llpc
