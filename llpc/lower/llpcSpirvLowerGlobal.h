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
 * @file  llpcSpirvLowerGlobal.h
 * @brief LLPC header file: contains declaration of class Llpc::SpirvLowerGlobal.
 ***********************************************************************************************************************
 */
#pragma once

#include "SPIRVInternal.h"
#include "llpcSpirvLower.h"
#include "vkgcDefs.h"
#include "lgc/Builder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/PassManager.h"
#include <list>
#include <unordered_map>
#include <unordered_set>

namespace Llpc {

// =====================================================================================================================
// Represents the pass of SPIR-V lowering operations for globals (global variables, inputs, and outputs).
class SpirvLowerGlobal : public SpirvLower, public llvm::PassInfoMixin<SpirvLowerGlobal> {
public:
  SpirvLowerGlobal();
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  void handleCallInst(bool checkEmitCall, bool checkInterpCall);

  void handleLoadInst();
  void handleLoadInstGEP(GlobalVariable *inOut, ArrayRef<Value *> indexOperands, LoadInst &loadInst);

  void handleStoreInst();
  void handleStoreInstGEP(GlobalVariable *output, ArrayRef<Value *> indexOperands, StoreInst &storeInst);

  static llvm::StringRef name() { return "Lower SPIR-V globals (global variables, inputs, and outputs)"; }

private:
  void mapGlobalVariableToProxy(llvm::GlobalVariable *globalVar);
  void lowerInOut(llvm::GlobalVariable *globalVar);
  void lowerInOutUsersInPlace(llvm::GlobalVariable *globalVar, llvm::Value *current,
                              SmallVectorImpl<llvm::Value *> &indexStack);

  void ensureUnifiedReturn();

  void lowerBufferBlock();
  void lowerTaskPayload();
  void lowerPushConsts();
  void lowerUniformConstants();
  void lowerAliasedVal();
  void lowerEdgeFlag();
  void lowerShaderRecordBuffer();

  void handleVolatileInput(llvm::GlobalVariable *input, llvm::Value *proxy);

  void changeRtFunctionSignature();

  llvm::Value *addCallInstForInOutImport(llvm::Type *inOutTy, unsigned addrSpace, llvm::Constant *inOutMeta,
                                         llvm::Value *startLoc, unsigned maxLocOffset, llvm::Value *compIdx,
                                         llvm::Value *vertexIdx, unsigned interpLoc, llvm::Value *interpInfo,
                                         bool isPerVertexDimension);

  llvm::Value *createRaytracingBuiltIn(BuiltIn builtIn);
  void addCallInstForOutputExport(llvm::Value *outputValue, llvm::Constant *outputMeta, llvm::Value *locOffset,
                                  unsigned maxLocOffset, unsigned xfbOffsetAdjust, unsigned xfbBufferAdjust,
                                  llvm::Value *elemIdx, llvm::Value *vertexIdx, unsigned emitStreamId);

  Value *loadDynamicIndexedMembers(Type *inOutTy, unsigned addrSpace, llvm::ArrayRef<llvm::Value *> indexOperands,
                                   Constant *inOutMetaVal, Value *locOffset, unsigned interpLoc, Value *auxInterpValue,
                                   bool isPerVertexDimension);

  llvm::Value *loadInOutMember(llvm::Type *inOutTy, llvm::Type *loadType, unsigned addrSpace,
                               llvm::ArrayRef<llvm::Value *> indexOperands, unsigned maxLocOffset,
                               llvm::Constant *inOutMeta, llvm::Value *locOffset, llvm::Value *vertexIdx,
                               unsigned interpLoc, llvm::Value *interpInfo, bool isPerVertexDimension);

  void storeOutputMember(llvm::Type *outputTy, llvm::Type *storeTy, llvm::Value *storeValue,
                         llvm::ArrayRef<llvm::Value *> indexOperands, unsigned maxLocOffset, llvm::Constant *outputMeta,
                         llvm::Value *locOffset, llvm::Value *vertexOrPrimitiveIdx);

  llvm::Value *interpolateInputElement(llvm::Type *returnTy, unsigned interpLoc, llvm::Value *interpInfo,
                                       GlobalVariable *gv, ArrayRef<Value *> indexOperands);

  void buildApiXfbMap();

  void addCallInstForXfbOutput(const ShaderInOutMetadata &outputMeta, Value *outputValue, unsigned xfbBufferAdjust,
                               unsigned xfbOffsetAdjust, unsigned locOffset, lgc::InOutInfo outputInfo);

  llvm::ReturnInst *m_unifiedReturn = nullptr;
  std::unordered_set<llvm::CallInst *> m_emitCalls; // "Call" instructions to emit vertex (geometry shader)
  ShaderStage m_lastVertexProcessingStage;          // The last vertex processing stage
  llvm::DenseMap<unsigned, Vkgc::XfbOutInfo>
      m_builtInXfbMap; // Map built-in to XFB output info specified by API interface
  llvm::DenseMap<unsigned, Vkgc::XfbOutInfo>
      m_genericXfbMap;           // Map generic location to XFB output info specified by API interface
  bool m_printedXfbInfo = false; // It marks if the XFB info has not been printed yet
};

} // namespace Llpc
