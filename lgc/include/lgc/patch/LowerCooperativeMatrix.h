/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerCooperativeMatrix.h
 * @brief LLPC header file : contains declaration of class lgc::LowerCooperativeMatrix.h
 ***********************************************************************************************************************
 */
#pragma once
#include "SystemValues.h"
#include "lgc/Builder.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/Function.h"

namespace lgc {
// =====================================================================================================================
// Pass to lower coopMatrix calls
class LowerCooperativeMatrix : public Patch, public llvm::PassInfoMixin<LowerCooperativeMatrix> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  bool runImpl(llvm::Module &module, PipelineShadersResult &pipelineShaders, PipelineState *pipelineState);

  static llvm::StringRef name() { return "Patch cooperative matrix calls"; }

  void visitCallInst(llvm::CallInst &callInst);

private:
  void processCoopMatrixFunction(llvm::ArrayRef<llvm::Function *> coopMatrixCallees);

  struct TypeProperties {
    // Number of (true) elements per lane.
    unsigned numFlatElements;

    // Number of (true and unused) elements per lane when casting an LGC dialect cooperative matrix type to
    // <N x elementType>.
    unsigned numMatrixElements;

    // Number of dwords per lane in an LGC dialect cooperative matrix type.
    unsigned numMatrixWords;

    // Stride of elements.
    unsigned matrixElementStride;
  };

  struct ComputeAddressInfo {
    // The base address for the first element in each lane.
    llvm::Value *base;

    // The increasing step between the last element in preVgpr and first element in curVgpr.
    llvm::Value *macroStep;

    // It will only be set on 16bit@Accumulator@gfx10 like:{C0_0,C1_0;C4_0,C5_0}
    llvm::Value *microStep;

    // It will only be set on 16bit @Accumulator @gfx10 like : {C0_0, C1_0; C4_0, C5_0}, which value will
    // be 2.
    unsigned microCount;
  };

  unsigned getLength(Builder::CooperativeMatrixLayout layout) const;

  TypeProperties getTypeProperties(Builder::CooperativeMatrixElementType elemType,
                                   Builder::CooperativeMatrixLayout layout) const;

  ComputeAddressInfo computeAddressing(Builder::CooperativeMatrixLayout layout,
                                       Builder::CooperativeMatrixElementType elemType, int waveSize,
                                       llvm::Value *stride, bool isColMajor, llvm::Instruction *insertPos);

  llvm::Value *cooperativeMatrixLoadInternal(llvm::Value *dataPtr, llvm::Value *stride, bool colMajor,
                                             Builder::CooperativeMatrixElementType elemType,
                                             Builder::CooperativeMatrixLayout layout, unsigned memoryAccess,
                                             const llvm::Twine &instName, llvm::Instruction *insertPos);
  // Convert vector data to cooperativeMatrix vec data
  // eg. v16*data_In_Buffer-->v8*coopMatrix_data as two 16bits elements packed.
  llvm::Value *convFlatVecToCoopMatrixVec(BuilderCommon &builder, llvm::Value *vecValue,
                                          Builder::CooperativeMatrixElementType elemType,
                                          Builder::CooperativeMatrixLayout layout);

  // Convert cooperativeMatrix vec data to vec data.
  llvm::Value *convCoopMatrixVecToFlatVec(BuilderCommon &builder, llvm::Value *matrixValue,
                                          Builder::CooperativeMatrixElementType elemType,
                                          Builder::CooperativeMatrixLayout layout);

  // Create cooperative matrix store operation
  void cooperativeMatrixStoreInternal(llvm::Value *dataPtr, llvm::Value *stride, bool colMajor,
                                      Builder::CooperativeMatrixElementType elemType,
                                      Builder::CooperativeMatrixLayout layout, unsigned memoryAccess,
                                      llvm::Value *&vecVal, const llvm::Twine &instName, llvm::Instruction *insertPos);

  // Open-code cooperative matrix extract operation
  llvm::Value *cooperativeMatrixExtract(BuilderCommon &builder, llvm::Value *matrix, llvm::Value *index,
                                        Builder::CooperativeMatrixElementType elemType,
                                        Builder::CooperativeMatrixLayout layout);

  // Open-code cooperative matrix insert operation
  llvm::Value *cooperativeMatrixInsert(BuilderCommon &builder, llvm::Value *matrix, llvm::Value *value,
                                       llvm::Value *index, Builder::CooperativeMatrixElementType elemType,
                                       Builder::CooperativeMatrixLayout layout);

  // Create cooperative matrix convert operation
  llvm::Value *cooperativeMatrixConvert(llvm::CastInst::CastOps castOp, llvm::Value *source,
                                        Builder::CooperativeMatrixElementType srcElemType,
                                        Builder::CooperativeMatrixElementType dstElemType,
                                        Builder::CooperativeMatrixLayout srclayout,
                                        Builder::CooperativeMatrixLayout dstlayout, const llvm::Twine &instName,
                                        llvm::Instruction *insertPos);

  // Create cooperative matrix convert operation without reshape operation
  llvm::Value *cooperativeMatrixConvertInternal(llvm::CastInst::CastOps castOp, llvm::Value *source,
                                                Builder::CooperativeMatrixElementType srcElemType,
                                                Builder::CooperativeMatrixElementType dstElemType,
                                                const llvm::Twine &instName, llvm::Instruction *insertPos);

  // Create cooperative matrix binary operation
  llvm::Value *cooperativeMatrixBinaryOp(Builder::CooperativeMatrixArithOp coopMatArithOp, llvm::Value *lhs,
                                         llvm::Value *rhs, Builder::CooperativeMatrixElementType elemType,
                                         Builder::CooperativeMatrixLayout layout, const llvm::Twine &instName,
                                         llvm::Instruction *insertPos);

  // Create cooperative matrixTimeScalar operation
  llvm::Value *coopMatrixTimesScalar(llvm::Value *matrix, llvm::Value *scalar,
                                     Builder::CooperativeMatrixElementType elemType,
                                     Builder::CooperativeMatrixLayout layout, const llvm::Twine &instName,
                                     llvm::Instruction *insertPos);

  // Create cooperative matrix reshape operation for 16bit on gfx10 and gfx110
  llvm::Value *cooperativeMatrixReshape16BitElementGfx1011(llvm::Value *matrix,
                                                           Builder::CooperativeMatrixElementType elemType,
                                                           Builder::CooperativeMatrixLayout srcLayout,
                                                           Builder::CooperativeMatrixLayout dstLayout,
                                                           llvm::Value *threadId, const llvm::Twine &instName,
                                                           llvm::Instruction *insertPos);

  // Create cooperative matrix reshape operation for 8bit on gfx10 and gfx11
  llvm::Value *cooperativeMatrixReshapeBetween8bitAnd32bitElementGfx1011(
      llvm::Value *matrix, Builder::CooperativeMatrixElementType srcElemType,
      Builder::CooperativeMatrixLayout srcLayout, const llvm::Twine &instName, llvm::Instruction *insertPos);

  // Adjust the layout on accumulator for gfx10
  llvm::Value *cooperativeMatrixReshapeBetween16bitAnd32bitOnAccGfx10(
      llvm::Value *source, Builder::CooperativeMatrixElementType srcElemType,
      Builder::CooperativeMatrixElementType dstElemType, Builder::CooperativeMatrixLayout layout,
      llvm::Value *isEvenGroup, const llvm::Twine &instName, llvm::Instruction *insertPos);

  // Adjust the layout before reshape operation(eg:float16->float32)
  llvm::Value *cooperativeMatrixReshapeBeforeConvert(llvm::Value *source,
                                                     Builder::CooperativeMatrixElementType srcElemType,
                                                     Builder::CooperativeMatrixElementType dstElemType,
                                                     Builder::CooperativeMatrixLayout srcLayout,
                                                     Builder::CooperativeMatrixLayout dstLayout,
                                                     const llvm::Twine &instName, llvm::Instruction *insertPos);

  // Adjust the layout before reshape operation(eg:float32->float16)
  llvm::Value *cooperativeMatrixReshapeAfterConvert(llvm::Value *source,
                                                    Builder::CooperativeMatrixElementType srcElemType,
                                                    Builder::CooperativeMatrixElementType dstElemType,
                                                    Builder::CooperativeMatrixLayout srcLayout,
                                                    Builder::CooperativeMatrixLayout dstLayout,
                                                    const llvm::Twine &instName, llvm::Instruction *insertPos);

  // Create cooperative matrix transpose operation
  llvm::Value *cooperativeMatrixTranspose(llvm::Value *matrix, Builder::CooperativeMatrixElementType elemType,
                                          Builder::CooperativeMatrixLayout srcLayout, const llvm::Twine &instName,
                                          llvm::Instruction *insertPos);

  llvm::Value *transposeCooperativeMatrixRecursively(llvm::Value *matrix, unsigned vecStride, unsigned laneStride,
                                                     llvm::Value *threadId, BuilderBase &builder);

  // Create cooperative matrix muladd operation
  llvm::Value *cooperativeMatrixMulAdd(llvm::Value *copMatrixa, llvm::Value *copMatrixb, llvm::Value *copMatrixc,
                                       bool isSignedA, bool isSignedB, bool isSat,
                                       Builder::CooperativeMatrixElementType accumElemType,
                                       Builder::CooperativeMatrixElementType factorElemType,
                                       const llvm::Twine &instName, llvm::Instruction *insertPos);

  // Simulating for WMMA
  llvm::Value *createDotProductFp16Fp16(llvm::Value *const vector1, llvm::Value *const vector2,
                                        llvm::Value *const accumulator, bool isSat, const llvm::Twine &instName,
                                        llvm::Instruction *insertPos);
  llvm::Value *createDotProductFp16Fp32(llvm::Value *const vector1, llvm::Value *const vector2,
                                        llvm::Value *const accumulator, bool isSat, const llvm::Twine &instName,
                                        llvm::Instruction *insertPos);
  llvm::Value *createDotProductInt16Int32(llvm::Value *vector1, llvm::Value *vector2, llvm::Value *accumulator,
                                          unsigned flags, bool isSat, const llvm::Twine &instName,
                                          llvm::Instruction *insertPos);
  llvm::Value *createDotProductInt8Int32(llvm::Value *vector1, llvm::Value *vector2, llvm::Value *accumulator,
                                         unsigned flags, bool isSat, const llvm::Twine &instName,
                                         llvm::Instruction *insertPos);
  llvm::Value *createDotProductInt16Int16(llvm::Value *vector1, llvm::Value *vector2, llvm::Value *accumulator,
                                          unsigned flags, bool isSat, const llvm::Twine &instName,
                                          llvm::Instruction *insertPos);

  llvm::Value *getLaneNumber(BuilderBase &builder);

  llvm::SmallVector<llvm::CallInst *, 8> m_coopMatrixCalls;
  PipelineState *m_pipelineState = nullptr;
  PipelineShadersResult *m_pipelineShaders = nullptr;
  GfxIpVersion m_gfxIp;
};

} // namespace lgc
