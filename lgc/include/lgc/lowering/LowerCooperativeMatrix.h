/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerCooperativeMatrix.h
 * @brief LLPC header file : contains declaration of class lgc::LowerCooperativeMatrix.h
 ***********************************************************************************************************************
 */
#pragma once
#include "SystemValues.h"
#include "lgc/Builder.h"
#include "lgc/LgcXdlTypes.h"
#include "lgc/lowering/LgcLowering.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/Function.h"

namespace lgc {

namespace xdl {
class CooperativeRowAccLoadOp;
class CooperativeRowAccStoreOp;
class CooperativeRowAccFinalizeModeOp;
class CooperativeRowAccAccumulateModeOp;
class CooperativeRowAccSplatOp;
class CooperativeRowAccExpandOp;
class CooperativeRowAccSumAccumulateOp;
class CooperativeRowAccScalarOp;

class CooperativeMatrixLoadOp;
class CooperativeMatrixStoreOp;
class CooperativeMatrixLengthOp;
class CooperativeMatrixFillOp;
class CooperativeMatrixExtractOp;
class CooperativeMatrixInsertOp;
class CooperativeMatrixConvertOp;
class CooperativeMatrixTransposeOp;
class CooperativeMatrixBinaryOp;
class CooperativeMatrixTimesScalarOp;
class CooperativeMatrixMulAddOp;
class CooperativeMatrixPackOp;
class CooperativeMatrixUnPackOp;
class SparsityIndexLoadOp;
class SparseCooperativeMatrixMulAddOp;
} // namespace xdl

// =====================================================================================================================
// Pass to lower coopMatrix calls
class LowerCooperativeMatrix : public LgcLowering, public llvm::PassInfoMixin<LowerCooperativeMatrix> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Lower cooperative matrix operations"; }

  void visitCallInst(llvm::CallInst &callInst);

private:
  void processCoopMatrixFunction(llvm::Module &module);

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

    // It will only be set for processing packed 8-bit value
    llvm::Value *packOffset;

    // It will only be set on 16bit @Accumulator @gfx10 like : {C0_0, C1_0; C4_0, C5_0}, which value will
    // be 2.
    unsigned microCount;
  };

  TypeProperties getTypeProperties(xdl::CooperativeMatrixElementType elemType, xdl::CooperativeMatrixLayout layout,
                                   unsigned kSize) const;

  ComputeAddressInfo computeAddressing(xdl::CooperativeMatrixLayout layout, xdl::CooperativeMatrixElementType elemType,
                                       int waveSize, llvm::Value *stride, bool isColMajor,
                                       llvm::Instruction *insertPos);
  llvm::Value *computeLoadtrBaseAddressing(xdl::CooperativeMatrixLayout layout,
                                           xdl::CooperativeMatrixElementType elemType, int waveSize,
                                           llvm::Value *stride, llvm::Instruction *insertPos);
  llvm::Value *getLoadTrIntrinsic(llvm::Value *dataptr, xdl::CooperativeMatrixLayout layout,
                                  xdl::CooperativeMatrixElementType elemType, int numElements, bool isColMajor,
                                  int waveSize, llvm::Value *stride, const llvm::Twine &instName,
                                  llvm::Instruction *insertPos);

  void visitCooperativeMatrixLengthOp(xdl::CooperativeMatrixLengthOp &matrixlength);
  void visitCooperativeMatrixLoadOp(xdl::CooperativeMatrixLoadOp &load);
  void visitCooperativeMatrixStoreOp(xdl::CooperativeMatrixStoreOp &store);
  void visitCooperativeMatrixFillOp(xdl::CooperativeMatrixFillOp &fill);
  void visitCooperativeMatrixExtractOp(xdl::CooperativeMatrixExtractOp &extract);
  void visitCooperativeMatrixInsertOp(xdl::CooperativeMatrixInsertOp &insert);
  void visitCooperativeMatrixConvertOp(xdl::CooperativeMatrixConvertOp &convert);
  void visitCooperativeMatrixTransposeOp(xdl::CooperativeMatrixTransposeOp &transpose);
  void visitCooperativeMatrixBinaryOp(xdl::CooperativeMatrixBinaryOp &binary);
  void visitCooperativeMatrixTimesScalarOp(xdl::CooperativeMatrixTimesScalarOp &timesscalar);
  void visitCooperativeMatrixMulAddOp(xdl::CooperativeMatrixMulAddOp &muladd);
  void visitCooperativeMatrixPackOp(xdl::CooperativeMatrixPackOp &pack);
  void visitCooperativeMatrixUnPackOp(xdl::CooperativeMatrixUnPackOp &unpack);

  // Convert vector data to cooperativeMatrix vec data
  // eg. v16*data_In_Buffer-->v8*coopMatrix_data as two 16bits elements packed.
  llvm::Value *convFlatVecToCoopMatrixVec(BuilderCommon &builder, llvm::Value *vecValue,
                                          xdl::CooperativeMatrixElementType elemType,
                                          xdl::CooperativeMatrixLayout layout, unsigned kSize = 16);

  // Convert cooperativeMatrix vec data to vec data.
  llvm::Value *convCoopMatrixVecToFlatVec(BuilderCommon &builder, llvm::Value *matrixValue,
                                          xdl::CooperativeMatrixElementType elemType,
                                          xdl::CooperativeMatrixLayout layout, unsigned kSize = 16);

  // Create cooperative matrix convert operation without reshape operation
  llvm::Value *cooperativeMatrixConvertInternal(llvm::CastInst::CastOps castOp, llvm::Value *source,
                                                xdl::CooperativeMatrixElementType srcElemType,
                                                xdl::CooperativeMatrixElementType dstElemType,
                                                const llvm::Twine &instName, llvm::Instruction *insertPos);

  // Create cooperative matrix binary operation
  llvm::Value *cooperativeMatrixBinaryOp(xdl::CooperativeMatrixArithOp coopMatArithOp, llvm::Value *lhs,
                                         llvm::Value *rhs, xdl::CooperativeMatrixElementType elemType,
                                         xdl::CooperativeMatrixLayout layout, const llvm::Twine &instName,
                                         llvm::Instruction *insertPos);

  // Create cooperative matrixTimeScalar operation
  llvm::Value *coopMatrixTimesScalar(llvm::Value *matrix, llvm::Value *scalar,
                                     xdl::CooperativeMatrixElementType elemType, xdl::CooperativeMatrixLayout layout,
                                     const llvm::Twine &instName, llvm::Instruction *insertPos);

  // Create cooperative matrix reshape operation for 16bit on gfx10 and gfx110
  llvm::Value *cooperativeMatrixReshape16BitElementGfx1011(llvm::Value *matrix,
                                                           xdl::CooperativeMatrixElementType elemType,
                                                           xdl::CooperativeMatrixLayout srcLayout,
                                                           xdl::CooperativeMatrixLayout dstLayout,
                                                           llvm::Value *threadId, const llvm::Twine &instName,
                                                           llvm::Instruction *insertPos);

  // Create cooperative matrix reshape operation for 8bit on gfx10 and gfx11
  llvm::Value *cooperativeMatrixReshapeBetween8bitAnd32bitElementGfx1011(llvm::Value *matrix,
                                                                         xdl::CooperativeMatrixElementType srcElemType,
                                                                         xdl::CooperativeMatrixLayout srcLayout,
                                                                         const llvm::Twine &instName,
                                                                         llvm::Instruction *insertPos);

  // Adjust the layout on accumulator for gfx10
  llvm::Value *cooperativeMatrixReshapeBetween16bitAnd32bitOnAccGfx10(
      llvm::Value *source, xdl::CooperativeMatrixElementType srcElemType, xdl::CooperativeMatrixElementType dstElemType,
      xdl::CooperativeMatrixLayout layout, llvm::Value *isEvenGroup, const llvm::Twine &instName,
      llvm::Instruction *insertPos);

  // Adjust the layout before reshape operation(eg:float16->float32)
  llvm::Value *cooperativeMatrixReshapeBeforeConvert(llvm::Value *source, xdl::CooperativeMatrixElementType srcElemType,
                                                     xdl::CooperativeMatrixElementType dstElemType,
                                                     xdl::CooperativeMatrixLayout srcLayout,
                                                     xdl::CooperativeMatrixLayout dstLayout,
                                                     const llvm::Twine &instName, llvm::Instruction *insertPos);

  // Adjust the layout before reshape operation(eg:float32->float16)
  llvm::Value *cooperativeMatrixReshapeAfterConvert(llvm::Value *source, xdl::CooperativeMatrixElementType srcElemType,
                                                    xdl::CooperativeMatrixElementType dstElemType,
                                                    xdl::CooperativeMatrixLayout srcLayout,
                                                    xdl::CooperativeMatrixLayout dstLayout, const llvm::Twine &instName,
                                                    llvm::Instruction *insertPos);

  llvm::Value *transposeCooperativeMatrixRecursively(llvm::Value *matrix, unsigned vecStride, unsigned laneStride,
                                                     llvm::Value *threadId, BuilderBase &builder);
  llvm::Value *swapCooperativeMatrixGfx12(llvm::Value *matrix, llvm::Value *resultValue,
                                          xdl::CooperativeMatrixElementType elemType, BuilderBase &builder);

  // Create cooperative matrix muladd operation
  llvm::Value *cooperativeMatrixMulAdd(llvm::Value *copMatrixa, llvm::Value *copMatrixb, llvm::Value *copMatrixc,
                                       bool isSignedA, bool isSignedB, bool isSatOrOpsel, bool isTied,
                                       xdl::CooperativeMatrixElementType accumElemType,
                                       xdl::CooperativeMatrixElementType factorElemType, const llvm::Twine &instName,
                                       llvm::Instruction *insertPos);

  llvm::Value *cooperativeMatrixPack(llvm::Value *matrixCLo, llvm::Value *matrixCHi, const llvm::Twine &instName,
                                     llvm::Instruction *insertPos);
  llvm::Value *cooperativeMatrixUnpack(llvm::Value *matrixPacked, bool getUpperHalf, const llvm::Twine &instName,
                                       llvm::Instruction *insertPos);

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
  llvm::Value *createDotProductInt(llvm::Value *vector1, llvm::Value *vector2, llvm::Value *accumulator, unsigned flags,
                                   bool isSat, const llvm::Twine &instName, llvm::Instruction *insertPos);

  void visitSparsityIndexLoadOp(xdl::SparsityIndexLoadOp &sparsityIndexload);
  void visitSparseCooperativeMatrixMulAddOp(xdl::SparseCooperativeMatrixMulAddOp &sparseMatrixMulAdd);

  llvm::Value *getLaneNumber(BuilderBase &builder);

  // Cooperative row acc operations.
  // Cooperative row acc data have two state: accumulate mode and finalize mode.
  // accumulate mode is matching the hardware accumulate matrix which benefit for accumulate operate.
  // finalize mode is general layout which benefit for load/store/splat operate.
  // For gfx12 row acc layout:
  // - finalize_lane[0:15] = accumulate_lane[0:15] + accumulate_lane[16:31]
  // - finalize_lane[16:31] = finalize_lane[0:15]

  // load the row acc from memory. The return row acc data is in finalize mode.
  void visitCooperativeRowAccLoadOp(xdl::CooperativeRowAccLoadOp &load);
  // store the row acc to memory. The input row acc data must be in finalize mode.
  void visitCooperativeRowAccStoreOp(xdl::CooperativeRowAccStoreOp &store);

  // change row acc data from finalize mode to accumulate mode.
  void visitCooperativeRowAccAccumulateModeOp(xdl::CooperativeRowAccAccumulateModeOp &accumulateMode);
  // change row acc data from accumulate mode to finalize mode.
  void visitCooperativeRowAccFinalizeModeOp(xdl::CooperativeRowAccFinalizeModeOp &finalizeMode);

  // fill the row acc with a scalar value. The return row acc data is in finalize mode.
  void visitCooperativeRowAccSplatOp(xdl::CooperativeRowAccSplatOp &splat);
  // expand cooperative row accumulate data into cooperative matrix.
  void visitCooperativeRowAccExpandOp(xdl::CooperativeRowAccExpandOp &expand);
  // sum and accumulate a cooperative matrix to cooperative row acc.
  // the input/output row acc data must be in accumulate mode.
  void visitCooperativeRowAccSumAccumulateOp(xdl::CooperativeRowAccSumAccumulateOp &sumAccumulate);
  // operate the row acc with a scalar value. The return row acc data is same mode as input.
  void visitCooperativeRowAccScalarOp(xdl::CooperativeRowAccScalarOp &scalar);

  // Helper functions for row acc operstions.
  llvm::Value *cooperativeRowAccConvertToAccumulateMode(BuilderBase &builder, llvm::Value *rowAccVal,
                                                        llvm::Value *threadId,
                                                        xdl::CooperativeMatrixElementType elemType);
  llvm::Value *cooperativeRowAccConvertToFinalizeMode(BuilderBase &builder, llvm::Value *rowAccVal,
                                                      xdl::CooperativeMatrixElementType elemType);

  // process cooperative row acc operations.
  void processCoopRowAccFunction(llvm::Module &module);

  llvm::SmallVector<llvm::CallInst *, 8> m_coopMatrixCalls;
  llvm::SmallVector<llvm::CallInst *, 8> m_coopRowAccCalls;
  PipelineState *m_pipelineState = nullptr;
  PipelineShadersResult *m_pipelineShaders = nullptr;
  GfxIpVersion m_gfxIp;
  llvm::DenseSet<llvm::Value *> m_valPackedInMatrixes;
};

} // namespace lgc
