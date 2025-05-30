/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerGlCompatibility.h
 * @brief LLPC header file: contains declaration of Llpc::LowerGlCompatibility
 ***********************************************************************************************************************
 */
#pragma once

#include "Lowering.h"
#include "SPIRVInternal.h"
#include "lgc/Builder.h"
#include "lgc/LgcDialect.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

namespace Llpc {

// =====================================================================================================================
// Represents the pass of FE lowering ray query post inline.
class LowerGlCompatibility : public Lowering, public llvm::PassInfoMixin<LowerGlCompatibility> {
public:
  LowerGlCompatibility();
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "Lower GL compatibility variables and operations"; }

private:
  bool needRun();
  unsigned getUniformLocation(llvm::GlobalVariable *var);
  ShaderInOutMetadata getShaderInOutMetadata(Type *elementType, Constant *elementMetadata);
  void unifyFunctionReturn(Function *func);
  void collectEmitInst();
  void collectEmulationResource();
  void buildPatchPositionInfo();

  // The function use to lower gl_ClipVertex
  bool needLowerClipVertex();
  bool needLowerFrontColor();
  bool needLowerBackColor();
  bool needLowerFrontSecondaryColor();
  bool needLowerBackSecondaryColor();
  bool needEmulateDrawPixels();
  bool needEmulateTwoSideLighting();
  bool needEmulateBitmap();
  bool needLowerFragColor();
  bool needEmulateSmoothStipple();
  bool needLowerAlphaTest();
  MDTuple *createInOutMd(const ShaderInOutMetadata &md);
  MDTuple *createBuiltInInOutMd(lgc::BuiltInKind builtIn);
  void createClipDistance();
  void createClipPlane();
  void createBackColor();
  void createBackSecondaryColor();
  void createFrontFacing();
  void createPatchTexCoord();
  void createFragDepth();
  void createFragStencilRef();
  void emulateStoreClipVertex();
  void emulationOutputColor(llvm::User *color);
  void emulateDrawPixels();
  void emulateTwoSideLighting();
  void emulateBitmap();
  void emulateSmoothStipple();
  void lowerClipVertex();
  void lowerColor(llvm::User *color);
  void lowerFrontColor();
  void lowerBackColor();
  void lowerFrontSecondaryColor();
  void lowerBackSecondaryColor();
  void lowerFragColor();
  void lowerAlphaTest();
  Value *patchAlphaScaling(Value *val, Type *valTy, Constant *metaVal, Value *alphaScaleVal);

  llvm::SmallVector<llvm::CallInst *> m_emitCalls; // "Call" instructions to emit vertex (geometry shader).
  llvm::ReturnInst *m_retInst;                     // "Return" of the entry point.

  // The resource use to lower gl_ClipVertex
  llvm::User *m_clipVertex;          // The global variable of gl_ClipVertex
  llvm::User *m_clipDistance;        // The global variable of gl_ClipDistance
  llvm::User *m_clipPlane;           // The global variable of gl_ClipPlane
  llvm::User *m_frontColor;          // The global variable of gl_FrontColor
  llvm::User *m_backColor;           // The global variable of gl_BackColor
  llvm::User *m_frontSecondaryColor; // The global variable of gl_FrontSecondaryColor
  llvm::User *m_backSecondaryColor;  // The global variable of gl_BackSecondaryColor
  llvm::User *m_color;               // The global variable of gl_Color
  llvm::User *m_secondaryColor;      // The fragment input, global variable of gl_SecondaryColor
  llvm::User *m_frontFacing;         // The fragment input, global variable of gl_FrontFacing
  llvm::User *m_patchTexCoord;       // The internal variable of patchTexCoord
  llvm::User *m_fragColor;      // The fragment output, it can be gl_FragColor or user define fragment shader output
  llvm::User *m_fragDepth;      // The fragment output, it can be gl_FragDepth or user define fragment shader output
  llvm::User *m_fragStencilRef; // The fragment output, it can be gl_FragStencilRefAMD or gl_FragStencilRefARB or user
                                // define fragment shader output
};

} // namespace Llpc
