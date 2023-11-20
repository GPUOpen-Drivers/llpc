/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  BuilderCommon.h
 * @brief LLPC header file: declaration of BuilderCommon
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm-dialects/Dialect/Builder.h"
#include "llvm/IR/IRBuilder.h"

namespace lgc {

enum class ResourceNodeType : unsigned;

// =====================================================================================================================
// BuilderCommon extends llvm_dialects::Builder, which extends llvm::IRBuilder<>, and provides a few utility methods
// used in both the LLPC front-end and in LGC (the LLPC middle-end).
// This class is used directly by passes in LGC.
class BuilderCommon : public llvm_dialects::Builder {
public:
  // Constructors
  BuilderCommon(llvm::LLVMContext &context) : llvm_dialects::Builder(context) {}
  BuilderCommon(llvm::BasicBlock *block) : llvm_dialects::Builder(block) {}
  BuilderCommon(llvm::Instruction *inst) : llvm_dialects::Builder(inst) {}

  // Get the type of a descriptor
  //
  // @param descType : Descriptor type, one of the ResourceNodeType values
  llvm::VectorType *getDescTy(ResourceNodeType descType);

  // Get the type of pointer to descriptor.
  //
  // @param descType : Descriptor type, one of the ResourceNodeType values
  llvm::Type *getDescPtrTy(ResourceNodeType descType);

  // Get the type of pointer returned by CreateLoadBufferDesc.
  llvm::PointerType *getBufferDescTy();

  // Get a constant of FP or vector of FP type from the given APFloat, converting APFloat semantics where necessary
  llvm::Constant *getFpConstant(llvm::Type *ty, llvm::APFloat value);

  // Return the i64 difference between two pointers, dividing out the size of the pointed-to objects.
  // For buffer fat pointers, delays the translation to patch phase.
  //
  // @param ty : Element type of the pointers.
  // @param lhs : Left hand side of the subtraction.
  // @param rhs : Reft hand side of the subtraction.
  // @param instName : Name to give instruction(s)
  llvm::Value *CreatePtrDiff(llvm::Type *ty, llvm::Value *lhs, llvm::Value *rhs, const llvm::Twine &instName = "");

  // Create an LLVM function call to the named function. The callee is built automatically based on return
  // type and its parameters.
  //
  // @param funcName : Name of the callee
  // @param retTy : Return type of the callee
  // @param args : Arguments to pass to the callee
  // @param attribs : Function attributes
  // @param instName : Name to give instruction
  llvm::CallInst *CreateNamedCall(llvm::StringRef funcName, llvm::Type *retTy, llvm::ArrayRef<llvm::Value *> args,
                                  llvm::ArrayRef<llvm::Attribute::AttrKind> attribs, const llvm::Twine &instName = "");

  // -----------------------------------------------------------------------------------------------------------------
  // Cooperative matrix operation.

  enum CooperativeMatrixMemoryAccess {
    MemoryAccessMaskNone = 0x00,     // No mask
    MemoryAccessVolatileMask = 0x01, // Access memory in volatile
    MemoryAccessCoherentMask = 0x02, // Access memory in coherent
    MemoryAccessTemporalMask = 0x04, // Access memory in temporal
  };

  enum CooperativeMatrixElementType {
    Unknown = 0, // Unknown
    Float16,     // 16-bit floating-point
    Float32,     // 32-bit floating-point
    Int8,        // 8-bit integer
    Int16,       // 16-bit integer
    Int32        // 32 bit integer
  };

  // Layout is virtual concept, eg: 16bit and 32bit for matrixC will share the same layout initially.
  // It will be passed as the argument of getTypeProperties to calculate the more detailed layout information.
  enum CooperativeMatrixLayout {
    FactorMatrixLayout = 0,            // A/B layout on gfx10/gfx11
    AccumulatorMatrixLayout,           // C/D layout on gfx11
    Gfx10AccumulatorMatrixLayout,      // 32bit@C/D layout on gfx10
    Gfx10Accumulator16bitMatrixLayout, // 16bit@C/D layout on gfx10
    InvalidLayout
  };

  // The cooperative matrix arithmetic operations the builder can consume.
  // NOTE: We rely on casting this implicitly to an integer, so we cannot use an enum class.
  enum class CooperativeMatrixArithOp {
    IAdd = 0,
    FAdd,
    ISub,
    FSub,
    IMul,
    FMul,
    UDiv,
    SDiv,
    FDiv,
    UMod,
    SRem,
    SMod,
    FRem,
    FMod
  };

  // Convert the element type enum into the corresponding LLVM type.
  llvm::Type *transCooperativeMatrixElementType(CooperativeMatrixElementType elemType);

  // Get the LGC type of a cooperative matrix with the given element type and layout.
  llvm::Type *getCooperativeMatrixTy(CooperativeMatrixElementType elemType, CooperativeMatrixLayout layout);

  // Determine the "length" of a cooperative matrix for purposes of extract/insert operations.
  llvm::Value *CreateCooperativeMatrixLength(CooperativeMatrixElementType elemType, CooperativeMatrixLayout layout,
                                             const llvm::Twine &instName = "");

  // Create an "extractelement"-equivalent operation for a cooperative matrix value.
  llvm::Value *CreateCooperativeMatrixExtract(llvm::Value *matrix, llvm::Value *index,
                                              CooperativeMatrixElementType elemType, CooperativeMatrixLayout layout,
                                              const llvm::Twine &instName = "");

  // Create an "insertelement"-equivalent operation for a cooperative matrix value.
  llvm::Value *CreateCooperativeMatrixInsert(llvm::Value *matrix, llvm::Value *value, llvm::Value *index,
                                             CooperativeMatrixElementType elemType, CooperativeMatrixLayout layout,
                                             const llvm::Twine &instName = "");

  // Create cooperative matrix load.
  //
  // @param pointer : The pointer to a data array.
  // @param stride : The number of elements in the array in memory between the first component of consecutive rows (or
  // columns) in the result.
  // @param colMaj : Whether the values loaded from memory are arrayed in column-major or row-major.
  // @param layout : Identify it's factor or accumulator
  // @param memoryAccess : Parsed from Memory operands in SPIRV-reader
  // @param instName : Name to give instruction(s)
  llvm::Value *CreateCooperativeMatrixLoad(llvm::Value *pointer, llvm::Value *stride, bool colMajor,
                                           CooperativeMatrixElementType elemType, CooperativeMatrixLayout layout,
                                           unsigned memoryAccess, const llvm::Twine &instName = "");

  // Create cooperative matrix store.
  //
  // @param pointer : The pointer to a data array.
  // @param matrix : The cooperative matrix to store.
  // @param stride : The number of elements in the array in memory between the first component of consecutive rows (or
  // columns) in the result.
  // @param colMaj : Whether the values loaded from memory are arrayed in column-major or row-major.
  // @param layout : Identify it's factor or accumulator
  // @param memoryAccess : Parsed from Memory operands in SPIRV-reader
  // @param instName : Name to give instruction(s).
  llvm::Value *CreateCooperativeMatrixStore(llvm::Value *pointer, llvm::Value *matrix, llvm::Value *stride,
                                            bool colMajor, CooperativeMatrixElementType elemType,
                                            CooperativeMatrixLayout layout, unsigned memoryAccess,
                                            const llvm::Twine &instName = "");

  // Create cooperative matrix conversion.
  // @param opCode : The convert opCode.
  // @param source : The source cooperative matrix.
  // @param dest : The conversion target.
  // @param instName : Name to give instruction(s).
  llvm::CallInst *CreateCooperativeMatrixConvert(llvm::CastInst::CastOps opCode, llvm::Value *source,
                                                 CooperativeMatrixElementType srcElemType,
                                                 CooperativeMatrixElementType dstElemType,
                                                 CooperativeMatrixLayout srcLayout, CooperativeMatrixLayout dstLayout,
                                                 const llvm::Twine &instName = "");

  // Create cooperative matrix binary operation
  //
  // @param coopMatArithOp : The cooperative matrix arithmetic operation to perform.
  // @param operand1 : The first operand.
  // @param operand2 : The second operand.
  // @param instName : Name to give instruction(s).
  llvm::Value *CreateCooperativeMatrixBinaryOp(CooperativeMatrixArithOp coopMatArithOp, llvm::Value *lhs,
                                               llvm::Value *rhs, CooperativeMatrixElementType elemType,
                                               CooperativeMatrixLayout layout, const llvm::Twine &instName = "");

  // Create cooperative MatrixTimesScalar binary operation
  //
  // @param matrix : It should be cooperative matrix.
  // @param scalar : It should be scalar type.
  // @param elemType : Name to give instruction(s).
  // @param layout : Identify A/B matrices or C/D matrices.
  llvm::Value *CreateCoopMatrixTimesScalar(llvm::Value *matrix, llvm::Value *scalar,
                                           CooperativeMatrixElementType elemType, CooperativeMatrixLayout layout,
                                           const llvm::Twine &instName = "");

  // =====================================================================================================================
  // Create cooperative matrix transpose operation
  //
  // @param matrix : The first operand and it should be a cooperative matrix.
  // @param elemType : The component type of the matrix.
  // @param srcLayout : Identify whether it's A/B or C/D
  llvm::CallInst *CreateCooperativeMatrixTranspose(llvm::Value *matrix, CooperativeMatrixElementType elemType,
                                                   CooperativeMatrixLayout srcLayout, const llvm::Twine &instName = "");

  // Create cooperative matrix muladd operation
  // @param coopMatrixa : Factor cooperative matrix.
  // @param coopMatrixb : Factor cooperative matrix.
  // @param coopMatrixc : Accumulator cooperative matrix.
  // @param isSignedA : Identify the signess for matrix A's element type
  // @param isSignedB : Identify the signess for matrix B's element type
  // @param isSat : SaturatingAccumulation for calculation
  // @param accumElemType : The component type of the matrix c
  // @param factorElemType : The component type of the matrix a
  llvm::Value *CreateCooperativeMatrixMulAdd(llvm::Value *coopMatrixa, llvm::Value *coopMatrixb,
                                             llvm::Value *coopMatrixc, bool isSignedA, bool isSignedB, bool isSat,
                                             CooperativeMatrixElementType accumElemType,
                                             CooperativeMatrixElementType factorElemType,
                                             const llvm::Twine &instName = "");
};

} // namespace lgc
