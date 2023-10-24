//===- SPIRVReader.cpp - Converts SPIR-V to LLVM ----------------*- C++ -*-===//
//
//                     The LLVM/SPIR-V Translator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright (c) 2014 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimers in the documentation
// and/or other materials provided with the distribution.
// Neither the names of Advanced Micro Devices, Inc., nor the names of its
// contributors may be used to endorse or promote products derived from this
// Software without specific prior written permission.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file implements conversion of SPIR-V binary to LLVM IR.
///
//===----------------------------------------------------------------------===//
#include "SPIRVReader.h"
#include "SPIRVBasicBlock.h"
#include "SPIRVExtInst.h"
#include "SPIRVFunction.h"
#include "SPIRVInstruction.h"
#include "SPIRVModule.h"
#include "SPIRVType.h"
#include "SPIRVUtil.h"
#include "SPIRVValue.h"
#include "llpcCompiler.h"
#include "llpcContext.h"
#include "llpcDialect.h"
#include "llpcPipelineContext.h"
#include "llpcRayTracingContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/LgcRtDialect.h"
#include "lgc/Pipeline.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

#define DEBUG_TYPE "spirv"

using namespace std;
using namespace lgc;
using namespace llvm;
using namespace SPIRV;
using namespace Llpc;
using namespace lgc::rt;

namespace Llpc {
Type *getRayQueryInternalTy(lgc::Builder *builder);
unsigned getTraceRayParamPayloadIdx(void);
} // namespace Llpc

namespace SPIRV {

cl::opt<bool> SPIRVWorkaroundBadSPIRV("spirv-workaround-bad-spirv", cl::init(true),
                                      cl::desc("Enable workarounds for bad SPIR-V"));

cl::opt<bool> SPIRVConvertNonUniformBroadcastToShuffle(
    "spirv-convert-non-uniform-broadcast-to-shuffle", cl::init(false),
    cl::desc("Convert OpGroupNonUniformBroadcast instructions to OpGroupNonUniformShuffle instructions."));

cl::opt<bool>
    SPIRVInstNamer("spirv-instnamer", cl::init(false),
                   cl::desc("Choose IR value names corresponding to SPIR-V IDs if no other name is available"));

cl::opt<Vkgc::DenormalMode> Fp32DenormalModeOpt(
    "fp32-denormal-mode", cl::init(Vkgc::DenormalMode::Auto), cl::desc("Override denormal mode for FP32"),
    cl::values(clEnumValN(Vkgc::DenormalMode::Auto, "auto", "No override (default behaviour)"),
               clEnumValN(Vkgc::DenormalMode::FlushToZero, "ftz", "Denormal input/output flushed to zero"),
               clEnumValN(Vkgc::DenormalMode::Preserve, "preserve", "Denormal input/output preserved")));

cl::opt<bool> SpirvOverrideWorkaroundStorageImageFormats(
    "spirv-override-workaround-storage-image-formats",
    cl::desc("Override the workaroundStorageImageFormats setting. When set to true, the Image Format operand of "
             "OpTypeImage is ignored and always treated as if it were Unknown. The default is to take the setting from "
             "the pipeline shader options (which itself defaults to false)"),
    cl::init(false));

cl::opt<bool> DisableFMA("disable-fma", cl::desc("Disable FMA intrinsic and use 'FMUL + FADD' instead"),
                         cl::init(false));

// Prefix for placeholder global variable name.
const char *KPlaceholderPrefix = "placeholder.";

const char *MetaNameSpirvOp = "spirv.op";

// Prefix for row major matrix helpers.
static const char SpirvLaunderRowMajor[] = "spirv.launder.row_major";

static const SPIRVWord SpvVersion10 = 0x00010000;

// Save the translated LLVM before validation for debugging purpose.
static bool DbgSaveTmpLLVM = false;
static const char *DbgTmpLLVMFileName = "_tmp_llvmbil.ll";

typedef std::pair<unsigned, AttributeList> AttributeWithIndex;

static void appendTypeMangling(Type *retTy, ArrayRef<Value *> args, std::string &name);

static void dumpLLVM(Module *m, const std::string &fName) {
  std::error_code ec;
  static int DumpIdx = 0;
  std::string uniqueFName = fName + "_" + std::to_string(DumpIdx++) + ".ll";
  raw_fd_ostream fs(uniqueFName, ec, sys::fs::OF_None);
  if (!ec) {
    fs << *m;
    fs.close();
  }
}

bool isAccelerationStructureType(SPIRVType *type) {
  return type->isTypeAccelerationStructureKHR();
}

SPIRVWord getStd430TypeAlignment(SPIRVType *const spvType) {
  switch (spvType->getOpCode()) {
  case OpTypeInt:
  case OpTypeBool:
  case OpTypeFloat:
    return spvType->getSizeInBytes();
  case OpTypeVector: {
    auto Components = spvType->getVectorComponentCount();
    if (Components == 3)
      Components = 4;
    return Components * spvType->getVectorComponentType()->getSizeInBytes();
  }
  case OpTypeStruct: {
    SPIRVWord maxAlign = 0;
    SPIRVTypeStruct *const spvStructType = static_cast<SPIRVTypeStruct *>(spvType);
    for (SPIRVWord i = 0, memberCount = spvStructType->getMemberCount(); i < memberCount; i++) {
      SPIRVType *const memberTy = spvStructType->getMemberType(i);
      auto memberAlign = getStd430TypeAlignment(memberTy);
      if (memberAlign > maxAlign)
        maxAlign = memberAlign;
    }
    return maxAlign;
  }
  case OpTypeArray: {
    SPIRVTypeArray *const spvArrayType = static_cast<SPIRVTypeArray *>(spvType);
    return getStd430TypeAlignment(spvArrayType->getArrayElementType());
  }
  case OpTypeMatrix: {
    auto *columnTy = spvType->getMatrixColumnType();
    return getStd430TypeAlignment(columnTy);
  }
  default:
    llvm_unreachable("unexpected type");
    break;
  }
  return 0;
}

// Return the aligned size, considering the padding between sub-members.
SPIRVWord getStd430AlignedTypeSize(SPIRVType *const spvType) {
  switch (spvType->getOpCode()) {
  case OpTypeInt:
  case OpTypeBool:
  case OpTypeFloat:
  case OpTypeVector:
    return spvType->getSizeInBytes();
  case OpTypeStruct: {
    SPIRVTypeStruct *const spvStructType = static_cast<SPIRVTypeStruct *>(spvType);
    SPIRVWord accumulatedOffset = 0;
    for (SPIRVWord i = 0, memberCount = spvStructType->getMemberCount(); i < memberCount; i++) {
      SPIRVType *const memberTy = spvStructType->getMemberType(i);
      auto memberAlign = getStd430TypeAlignment(memberTy);
      auto size = getStd430AlignedTypeSize(memberTy);
      accumulatedOffset = roundUpToMultiple(accumulatedOffset, memberAlign);
      accumulatedOffset += size;
    }
    return accumulatedOffset;
  }
  case OpTypeArray: {
    SPIRVTypeArray *const spvArrayType = static_cast<SPIRVTypeArray *>(spvType);
    SPIRVType *elemTy = spvArrayType->getArrayElementType();
    SPIRVWord elemAlign = getStd430TypeAlignment(elemTy);
    auto elemSize = getStd430AlignedTypeSize(elemTy);
    // We need tail-padding if element size is not multiple of element alignment
    SPIRVWord stride = elemSize;
    if (elemSize % elemAlign != 0)
      stride = roundUpToMultiple(elemSize, elemAlign);
    return stride * spvArrayType->getLength()->getZExtIntValue();
  }
  case OpTypeMatrix: {
    auto *columnTy = spvType->getMatrixColumnType();
    SPIRVWord columnCount = spvType->getMatrixColumnCount();
    return getStd430TypeAlignment(columnTy) * columnCount;
  }
  default:
    llvm_unreachable("unexpected type");
    break;
  }
  return 0;
}

static bool isStorageClassExplicitlyLaidOut(SPIRVModule *m_bm, SPIRVStorageClassKind storageClass) {
  return llvm::is_contained({StorageClassStorageBuffer, StorageClassUniform, StorageClassPushConstant,
                             StorageClassPhysicalStorageBufferEXT},
                            storageClass) ||
         storageClass == StorageClassShaderRecordBufferKHR ||
         (storageClass == StorageClassWorkgroup && m_bm->hasCapability(CapabilityWorkgroupMemoryExplicitLayoutKHR));
}

SPIRVToLLVM::SPIRVToLLVM(Module *llvmModule, SPIRVModule *theSpirvModule, const SPIRVSpecConstMap &theSpecConstMap,
                         ArrayRef<ConvertingSampler> convertingSamplers, lgc::Builder *builder,
                         const Vkgc::ShaderModuleUsage *moduleUsage, const Vkgc::PipelineShaderOptions *shaderOptions)
    : m_m(llvmModule), m_builder(builder), m_bm(theSpirvModule), m_entryTarget(nullptr),
      m_specConstMap(theSpecConstMap), m_convertingSamplers(convertingSamplers), m_dbgTran(m_bm, m_m, this),
      m_moduleUsage(reinterpret_cast<const Vkgc::ShaderModuleUsage *>(moduleUsage)), m_debugOutputBuffer(nullptr),
      m_shaderOptions(reinterpret_cast<const Vkgc::PipelineShaderOptions *>(shaderOptions)) {
  assert(m_m);
  m_context = &m_m->getContext();
  m_spirvOpMetaKindId = m_context->getMDKindID(MetaNameSpirvOp);
  m_scratchBoundsChecksEnabled = scratchBoundsChecksEnabled();

  m_workaroundStorageImageFormats = shaderOptions->workaroundStorageImageFormats;
  if (SpirvOverrideWorkaroundStorageImageFormats.getNumOccurrences() > 0)
    m_workaroundStorageImageFormats = SpirvOverrideWorkaroundStorageImageFormats.getValue();
}

void SPIRVToLLVM::recordRemappedTypeElements(SPIRVType *bt, unsigned from, unsigned to) {
  auto &elements = m_remappedTypeElements[bt];

  if (elements.size() <= from)
    elements.resize(from + 1, 0);

  elements[from] = to;
}

uint64_t SPIRVToLLVM::getTypeStoreSize(Type *const t) {
  auto it = m_typeToStoreSize.find(t);
  if (it != m_typeToStoreSize.end())
    return it->second;

  const uint64_t calculatedSize = m_m->getDataLayout().getTypeStoreSize(t);
  m_typeToStoreSize[t] = calculatedSize;
  return calculatedSize;
}

Value *SPIRVToLLVM::mapValue(SPIRVValue *bv, Value *v) {
  auto loc = m_valueMap.find(bv);
  if (loc != m_valueMap.end()) {
    if (loc->second == v)
      return v;
    auto ld = dyn_cast<LoadInst>(loc->second);
    auto placeholder = cast<GlobalVariable>(ld->getPointerOperand());
    assert(ld && placeholder->getName().startswith(KPlaceholderPrefix) && "A value is translated twice");
    // Replaces placeholders for PHI nodes
    ld->replaceAllUsesWith(v);
    ld->eraseFromParent();
    placeholder->eraseFromParent();
  }
  m_valueMap[bv] = v;
  return v;
}

Value *SPIRVToLLVM::mapEntry(const SPIRVEntry *be, Value *v) {
  if (m_entryMap.find(be) == m_entryMap.end()) {
    assert(v != nullptr);
    m_entryMap[be] = v;
    return v;
  } else
    return m_entryMap[be];
}

unsigned SPIRVToLLVM::getBlockPredecessorCounts(BasicBlock *block, BasicBlock *predecessor) {
  assert(block);
  // This will create the map entry if it does not already exist.
  auto it = m_blockPredecessorToCount.find({block, predecessor});
  if (it != m_blockPredecessorToCount.end())
    return it->second;

  return 0;
}

bool SPIRVToLLVM::isSPIRVBuiltinVariable(GlobalVariable *gv, SPIRVBuiltinVariableKind *kind) {
  auto loc = m_builtinGvMap.find(gv);
  if (loc == m_builtinGvMap.end())
    return false;
  if (kind)
    *kind = loc->second;
  return true;
}

Value *SPIRVToLLVM::getTranslatedValue(SPIRVValue *bv) {
  auto loc = m_valueMap.find(bv);
  if (loc != m_valueMap.end())
    return loc->second;
  return nullptr;
}

void SPIRVToLLVM::setAttrByCalledFunc(CallInst *call) {
  Function *f = call->getCalledFunction();
  assert(f);
  if (f->isIntrinsic())
    return;
  call->setCallingConv(f->getCallingConv());
  call->setAttributes(f->getAttributes());
}

Type *SPIRVToLLVM::transFPType(SPIRVType *t) {
  switch (t->getFloatBitWidth()) {
  case 16:
    return Type::getHalfTy(*m_context);
  case 32:
    return Type::getFloatTy(*m_context);
  case 64:
    return Type::getDoubleTy(*m_context);
  default:
    llvm_unreachable("Invalid type");
    return nullptr;
  }
}

// =====================================================================================================================
// Translate an "OpTypeArray". This contains special handling for arrays in interface storage classes which are
// explicitly laid out and may contain manually placed padding bytes. If the array needs padding, we map an array like
// '<element>[length]' -> 'struct { <element>, <padding bytes> }[length]'.
//
// @param spvType : The type.
// @param matrixStride : The matrix stride (can be 0).
// @param isColumnMajor : Whether the matrix is column major.
// @param isParentPointer : If the parent is a pointer type.
// @param layout : The layout mode will be used for the type translation.
template <>
Type *SPIRVToLLVM::transTypeWithOpcode<spv::OpTypeArray>(SPIRVType *const spvType, const unsigned matrixStride,
                                                         const bool isColumnMajor, const bool isParentPointer,
                                                         LayoutMode layout) {
  Type *elementType = transType(spvType->getArrayElementType(), matrixStride, isColumnMajor, isParentPointer, layout);

  SPIRVWord arrayStride = 0;
  const bool hasArrayStride = spvType->hasDecorate(DecorationArrayStride, 0, &arrayStride);
  assert((hasArrayStride ^ (arrayStride == 0)) || layout == LayoutMode::Std430);

  const uint64_t storeSize = getTypeStoreSize(elementType);
  if (layout == LayoutMode::Std430) {
    SPIRVWord elementAlign = getStd430TypeAlignment(spvType->getArrayElementType());
    arrayStride = roundUpToMultiple((SPIRVWord)storeSize, elementAlign);
  }

  bool maybePadded = (layout == LayoutMode::Explicit && hasArrayStride) || layout == LayoutMode::Std430;
  bool paddedArray = false;

  if (maybePadded) {
    assert(arrayStride >= storeSize);

    const unsigned padding = static_cast<unsigned>(arrayStride - storeSize);

    paddedArray = padding > 0;

    if (paddedArray) {
      // Record that the array was remapped, even though we don't record a useful mapping for arrays.
      recordRemappedTypeElements(spvType, 0, 0);

      elementType = StructType::create({elementType, getPadType(padding)}, "llpc.array.element", true);
    }
  }

  Type *const arrayType = ArrayType::get(elementType, spvType->getArrayLength());
  return paddedArray ? recordTypeWithPad(arrayType) : arrayType;
}

// =====================================================================================================================
// Translate an "OpTypeBool". This contains special handling for bools in pointers, which we need to map separately
// because boolean values in memory are represented as i32.
//
// @param spvType : The type.
// @param matrixStride : The matrix stride (can be 0).
// @param isColumnMajor : Whether the matrix is column major.
// @param isParentPointer : If the parent is a pointer type.
// @param layout : The layout mode will be used for the type translation.
template <>
Type *SPIRVToLLVM::transTypeWithOpcode<OpTypeBool>(SPIRVType *const spvType, const unsigned matrixStride,
                                                   const bool isColumnMajor, const bool isParentPointer,
                                                   LayoutMode layout) {
  if (isParentPointer)
    return getBuilder()->getInt32Ty();
  return getBuilder()->getInt1Ty();
}

// =====================================================================================================================
// Translate an "OpTypeForwardPointer".
//
// @param spvType : The type.
// @param matrixStride : The matrix stride (can be 0).
// @param isColumnMajor : Whether the matrix is column major.
// @param isParentPointer : If the parent is a pointer type.
// @param layout : The layout mode will be used for the type translation.
template <>
Type *SPIRVToLLVM::transTypeWithOpcode<OpTypeForwardPointer>(SPIRVType *const spvType, const unsigned matrixStride,
                                                             const bool isColumnMajor, const bool isParentPointer,
                                                             LayoutMode layout) {
  SPIRVTypeForwardPointer *const spvForwardPointerType = static_cast<SPIRVTypeForwardPointer *>(spvType);
  const SPIRVStorageClassKind storageClass = spvForwardPointerType->getPointerStorageClass();

  // Forward pointers must always point to structs.
  assert(spvForwardPointerType->getPointerElementType()->isTypeStruct());

  // We first have to map the pointed-to-struct to an opaque struct so we can have a forward reference to the struct.
  StructType *const pointeeType = StructType::create(*m_context);

  // Then we need to map our forward pointer itself, because the struct we are pointing to could use the pointer.
  const unsigned addrSpace = SPIRSPIRVAddrSpaceMap::rmap(storageClass);
  Type *const type = mapType(spvType, PointerType::get(pointeeType, addrSpace));

  const bool isBufferBlockPointer = storageClass == StorageClassStorageBuffer || storageClass == StorageClassUniform ||
                                    storageClass == StorageClassPushConstant ||
                                    storageClass == StorageClassShaderRecordBufferKHR ||
                                    storageClass == StorageClassPhysicalStorageBufferEXT;
  LayoutMode structLayout = isBufferBlockPointer ? LayoutMode::Explicit : LayoutMode::Native;

  // Finally we translate the struct we are pointing to create it.
  StructType *const structType =
      cast<StructType>(transType(spvType->getPointerElementType(), matrixStride, isColumnMajor, true, structLayout));

  pointeeType->setBody(structType->elements(), structType->isPacked());

  return type;
}

// =====================================================================================================================
// Translate an "OpTypeMatrix". This contains special handling for matrices in interface storage classes which are
// explicitly laid out and may contain manually placed padding bytes after the column elements.
//
// @param spvType : The type.
// @param matrixStride : The matrix stride (can be 0).
// @param isColumnMajor : Whether the matrix is column major.
// @param isParentPointer : If the parent is a pointer type.
// @param layout : The layout mode will be used for the type translation.
template <>
Type *SPIRVToLLVM::transTypeWithOpcode<OpTypeMatrix>(SPIRVType *const spvType, unsigned matrixStride,
                                                     const bool isColumnMajor, const bool isParentPointer,
                                                     LayoutMode layout) {
  const auto spvColumnType = spvType->getMatrixColumnType();
  const auto spvElementType = spvColumnType->getVectorComponentType();
  const unsigned spvColumnCount = spvType->getMatrixColumnCount();
  const unsigned spvRowCount = spvColumnType->getVectorComponentCount();
  Type *columnType = nullptr;
  unsigned columnCount = 0;

  if (!isParentPointer || isColumnMajor) {
    // If the matrix is not explicitly laid out or is column major, just translate the column type.
    columnType = transType(spvColumnType, matrixStride, isColumnMajor, isParentPointer, layout);
    columnCount = spvColumnCount;
  } else {
    // We need to transpose the matrix type to represent its layout in memory.
    Type *const elementType = transType(spvElementType, matrixStride, isColumnMajor, isParentPointer, layout);

    // NOTE: The new column after transposition is actually the original SPIR-V row vector and the column count is the
    // original SPIR-V row vector count.
    columnType = ArrayType::get(elementType, spvColumnCount);
    columnCount = spvRowCount;

    // NOTE: This is a workaround. SPIR-V translated from HLSL might not have MatrixStride decoration. In such cases,
    // we will calculate it for RowMajor matrix.
    if (!isColumnMajor && matrixStride == 0) {
      // Targeted for std430 layout with those rules:
      //   - A three- or four-component vector, with components of size N, has a base alignment of 4N.
      //   - A row-major matrix of C columns has a base alignment equal to the base alignment of a vector of C matrix
      //     components.
      //   - Any ArrayStride or MatrixStride decoration must be an integer multiple of the base alignment of the array
      //     or matrix.
      assert(spvColumnCount >= 2);
      matrixStride = alignTo(spvColumnCount, 2) * (elementType->getPrimitiveSizeInBits() / 8);
    }
  }

  if (layout == LayoutMode::Std430) {
    // For std430 layout, set matrix stride to the alignment of column vector to make every column vector properly
    // aligned.
    matrixStride = getStd430TypeAlignment(spvType->getMatrixColumnType());
  }

  const bool isPaddedMatrix = matrixStride > 0;
  const bool usePadding = (layout == LayoutMode::Explicit || layout == LayoutMode::Std430) && isPaddedMatrix;

  if (usePadding) {
    SmallVector<Type *, 2> memberTypes;

    memberTypes.push_back(columnType);

    const uint64_t storeSize = getTypeStoreSize(columnType);
    assert(matrixStride >= storeSize);

    const unsigned padding = static_cast<unsigned>(matrixStride - storeSize);

    if (padding > 0)
      memberTypes.push_back(getPadType(padding));

    const StringRef typeName = isColumnMajor ? "llpc.matrix.column" : "llpc.matrix.row";

    columnType = StructType::create(memberTypes, typeName, true);
  }

  Type *const matrixType = ArrayType::get(columnType, columnCount);
  return usePadding ? recordTypeWithPad(matrixType, !isColumnMajor) : matrixType;
}

// =====================================================================================================================
// Translate an "OpTypePointer". This contains special handling for pointers to bool, which we need to map separately
// because boolean values in memory are represented as i32, and special handling for images and samplers.
//
// @param spvType : The type.
// @param matrixStride : The matrix stride (can be 0).
// @param isColumnMajor : Whether the matrix is column major.
// @param isParentPointer : If the parent is a pointer type.
// @param layout : The layout mode will be used for the type translation.
template <>
Type *SPIRVToLLVM::transTypeWithOpcode<OpTypePointer>(SPIRVType *const spvType, const unsigned matrixStride,
                                                      const bool isColumnMajor, const bool isParentPointer,
                                                      LayoutMode layout) {
  SPIRVStorageClassKind storageClass = spvType->getPointerStorageClass();
  LayoutMode pointeeLayout =
      isStorageClassExplicitlyLaidOut(m_bm, storageClass) ? LayoutMode::Explicit : LayoutMode::Native;
  auto addrSpace = SPIRSPIRVAddrSpaceMap::rmap(storageClass);

  // Handle image etc types first, if in UniformConstant memory.
  if (storageClass == StorageClassUniformConstant) {
    auto spvElementType = spvType->getPointerElementType();
    while (spvElementType->getOpCode() == OpTypeArray || spvElementType->getOpCode() == OpTypeRuntimeArray) {
      // Pointer to array (or runtime array) of image/sampler/sampledimage has the same representation as
      // a simple pointer to same image/sampler/sampledimage.
      spvElementType = spvElementType->getArrayElementType();
    }

    if (spvElementType->getOpCode() == OpTypeImage || spvElementType->getOpCode() == OpTypeSampler ||
        spvElementType->getOpCode() == OpTypeSampledImage) {
      // Pointer to image/sampler/sampledimage type.
      Type *imagePtrTy = nullptr;
      SPIRVTypeImage *spvImageTy = nullptr;

      if (spvElementType->getOpCode() != OpTypeSampler) {
        // Image or sampledimage: get the image pointer type.
        if (spvElementType->getOpCode() == OpTypeSampledImage)
          spvImageTy = static_cast<SPIRVTypeSampledImage *>(spvElementType)->getImageType();
        else
          spvImageTy = static_cast<SPIRVTypeImage *>(spvElementType);
        if (spvImageTy->getDescriptor().Dim == DimBuffer) {
          // Texel buffer.
          imagePtrTy = getBuilder()->getDescPtrTy(ResourceNodeType::DescriptorTexelBuffer);
        } else {
          // Image descriptor.
          imagePtrTy = getBuilder()->getDescPtrTy(ResourceNodeType::DescriptorResource);
        }
        // Pointer to image is represented as a struct containing {pointer, stride, planeStride, isResource}.
        imagePtrTy = StructType::get(*m_context, {imagePtrTy, getBuilder()->getInt32Ty(), getBuilder()->getInt32Ty(),
                                                  getBuilder()->getInt32Ty()});

        if (spvImageTy->getDescriptor().MS) {
          // Pointer to multisampled image is represented as two image pointers, the second one for the fmask.
          imagePtrTy = StructType::get(*m_context, {imagePtrTy, imagePtrTy});
        }
      }

      // For an image (not sampler or sampledimage), just return the pointer-to-image type.
      if (spvElementType->getOpCode() == OpTypeImage)
        return imagePtrTy;

      // Sampler or sampledimage: get the sampler pointer type.
      Type *samplerPtrTy = getBuilder()->getDescPtrTy(ResourceNodeType::DescriptorSampler);
      // Pointer to sampler is represented as a struct containing {pointer,stride,convertingSamplerIdx}
      samplerPtrTy =
          StructType::get(*m_context, {samplerPtrTy, getBuilder()->getInt32Ty(), getBuilder()->getInt32Ty()});

      // For a sampler, just return that. For a sampledimage, return a struct type containing both pointers.
      if (!imagePtrTy)
        return samplerPtrTy;
      return StructType::get(*m_context, {imagePtrTy, samplerPtrTy});
    } else {
      // Uniform constant variable outside of a block use std430 layout.
      pointeeLayout = isAccelerationStructureType(spvElementType) ? LayoutMode::Explicit : LayoutMode::Std430;
      // From now on (GPURT major version >= 34), AS header may start at a non-zero offset, GPURT now request base
      // offset of the resource, and it will calculate the actual GPUVA, instead of compiler providing one loaded from
      // offset 0. Here we use SPIRAS_Constant because later in llpcSpirvLowerGlobal the AS will be lowered to
      // get.desc.ptr which returns SPIRAS_Constant ptr.
      addrSpace = isAccelerationStructureType(spvElementType) ? SPIRAS_Constant : addrSpace;
    }
  }

  Type *const pointeeType =
      transType(spvType->getPointerElementType(), matrixStride, isColumnMajor, true, pointeeLayout);

  return PointerType::get(pointeeType, addrSpace);
}

// =====================================================================================================================
// Translate an "OpTypeRuntimeArray". This contains special handling for arrays in interface storage classes which are
// explicitly laid out and may contain manually placed padding bytes. If the array needs padding, we map an array like
// '<element>[length]' -> 'struct { <element>, <padding bytes> }[length]'.
//
// @param spvType : The type.
// @param matrixStride : The matrix stride (can be 0).
// @param isColumnMajor : Whether the matrix is column major.
// @param isParentPointer : If the parent is a pointer type.
// @param layout : The layout mode will be used for the type translation.
template <>
Type *SPIRVToLLVM::transTypeWithOpcode<OpTypeRuntimeArray>(SPIRVType *const spvType, const unsigned matrixStride,
                                                           const bool isColumnMajor, const bool isParentPointer,
                                                           LayoutMode layout) {
  Type *elementType = transType(spvType->getArrayElementType(), matrixStride, isColumnMajor, isParentPointer, layout);

  SPIRVWord arrayStride = 0;
  const bool hasArrayStride = spvType->hasDecorate(DecorationArrayStride, 0, &arrayStride);
  assert(hasArrayStride ^ (arrayStride == 0));

  const uint64_t storeSize = getTypeStoreSize(elementType);

  // NOTE: Padding isn't allowed for a case that the array element is a structure with array-type member in HLSL.
  bool paddedArray = arrayStride > storeSize;

  if (layout == LayoutMode::Explicit && hasArrayStride && paddedArray) {
    const unsigned padding = static_cast<unsigned>(arrayStride - storeSize);

    // Record that the array was remapped, even though we don't record a useful mapping for arrays.
    recordRemappedTypeElements(spvType, 0, 0);

    elementType = StructType::create({elementType, getPadType(padding)}, "llpc.runtime.array.element", true);
  }

  Type *const runtimeArrayType = ArrayType::get(elementType, SPIRVWORD_MAX);
  return paddedArray ? recordTypeWithPad(runtimeArrayType) : runtimeArrayType;
}

// =====================================================================================================================
// Translate an "OpTypeStruct". This contains special handling for structures in interface storage classes which are
// explicitly laid out and may contain manually placed padding bytes between any struct elements (including perhaps
// before the first struct element!).
//
// @param spvType : The type.
// @param matrixStride : The matrix stride (can be 0).
// @param isColumnMajor : Whether the matrix is column major.
// @param isParentPointer : If the parent is a pointer type.
// @param layout : The layout mode will be used for the type translation.
template <>
Type *SPIRVToLLVM::transTypeWithOpcode<spv::OpTypeStruct>(SPIRVType *const spvType, const unsigned matrixStride,
                                                          const bool isColumnMajor, const bool isParentPointer,
                                                          LayoutMode layout) {
  SPIRVTypeStruct *const spvStructType = static_cast<SPIRVTypeStruct *>(spvType);

  bool isPacked = false;
  bool isExplicitlyLaidOut = layout == LayoutMode::Explicit;
  bool hasMemberOffset = false;

  using StructMember = std::tuple<SPIRVWord, SPIRVWord>;

  SmallVector<StructMember, 8> structMembers;
  // For uniform constants, we need to calculate the alignment of each member first. Based on the alignment,
  // we get the offset.
  SPIRVWord accumulatedOffset = 0;
  for (SPIRVWord i = 0, memberCount = spvStructType->getMemberCount(); i < memberCount; i++) {
    SPIRVWord offset = 0;

    if (layout == LayoutMode::Std430) {
      auto align = getStd430TypeAlignment(spvStructType->getMemberType(i));
      auto size = getStd430AlignedTypeSize(spvStructType->getMemberType(i));
      accumulatedOffset = roundUpToMultiple(accumulatedOffset, align);
      structMembers.push_back(StructMember(i, accumulatedOffset));
      accumulatedOffset += size;
      continue;
    }
    // If we have a member decorate, we need to handle the struct carefully. To do this we use a packed LLVM struct
    // type with manually added byte array pads. We record all the remappings of original index -> new index that
    // have occurred so that we can fixup GEPs and insert/extract's later.
    if (isExplicitlyLaidOut) {
      const bool nextHasMemberOffset = spvStructType->hasMemberDecorate(i, DecorationOffset, 0, &offset);

      // If we did not find a member offset, check that we did not see any member offsets on other members.
      assert(hasMemberOffset == false || nextHasMemberOffset);

      hasMemberOffset = nextHasMemberOffset;
    }

    structMembers.push_back(StructMember(i, offset));
  }

  // Sort the members by the offsets they have into the struct.
  llvm::sort(structMembers, [](const StructMember &left, const StructMember &right) {
    // First order by offsets.
    if (std::get<1>(left) < std::get<1>(right))
      return true;
    if (std::get<1>(left) > std::get<1>(right))
      return false;
    return std::get<0>(left) < std::get<0>(right);
  });

  SPIRVWord lastIndex = 0;
  uint64_t lastValidByte = 0;
  bool usePadding = (isExplicitlyLaidOut && hasMemberOffset) || layout == LayoutMode::Std430;
  SmallVector<Type *, 16> memberTypes;

  for (const StructMember &structMember : structMembers) {
    const SPIRVWord index = std::get<0>(structMember);
    const SPIRVWord offset = std::get<1>(structMember);

    if (usePadding) {
      // HLSL-derived shaders contain some (entirely valid) strange mappings for arrays that cannot be represented
      // in LLVM. This manifests as an offset for a struct member that overlaps the previous data in the struct.
      // To workaround this, we need to change the previous member in the struct to a pad array that we'll sort
      // out during access-chain and load/stores later.
      if (offset < lastValidByte) {
        // Get the previous last member in the struct.
        Type *const lastMemberType = memberTypes.back();

        // Pop it from the member types.
        memberTypes.pop_back();

        // Get the size of the last member.
        const uint64_t bytes = getTypeStoreSize(lastMemberType);

        // Push a pad type into the struct for the member we are having to remap.
        memberTypes.push_back(getPadType(offset - (lastValidByte - bytes)));

        // Remember the original type of the struct member which we need later.
        m_overlappingStructTypeWorkaroundMap[std::make_pair(spvType, lastIndex)] = lastMemberType;

        // And set the last valid byte to the offset since we've worked around this.
        lastValidByte = offset;
      } else {
        const unsigned padding = static_cast<unsigned>(offset - lastValidByte);

        if (padding > 0)
          memberTypes.push_back(getPadType(padding));
      }

      recordRemappedTypeElements(spvStructType, index, memberTypes.size());

      // We always pack structs with explicit offsets.
      isPacked = true;
    }

    SPIRVType *const spvMemberType = spvStructType->getMemberType(index);

    SPIRVWord memberMatrixStride = 0;
    spvStructType->hasMemberDecorate(index, DecorationMatrixStride, 0, &memberMatrixStride);

    const bool memberIsColumnMajor = !spvStructType->hasMemberDecorate(index, DecorationRowMajor);

    // If our member is a matrix, check that only one of the specifiers is declared.
    if (isExplicitlyLaidOut && memberMatrixStride > 0)
      assert(memberIsColumnMajor ^ spvStructType->hasMemberDecorate(index, DecorationRowMajor));

    Type *const memberType = transType(spvMemberType, memberMatrixStride, memberIsColumnMajor, isParentPointer, layout);

    lastValidByte = offset + getTypeStoreSize(memberType);

    memberTypes.push_back(memberType);

    lastIndex = index;
  }

  StructType *structType = nullptr;
  if (spvStructType->isLiteral())
    structType = StructType::get(*m_context, memberTypes, isPacked);
  else {
    structType = StructType::create(*m_context, spvStructType->getName());
    structType->setBody(memberTypes, isPacked);
  }

  return usePadding ? recordTypeWithPad(structType) : structType;
}

// =====================================================================================================================
// Translate an "OpTypeVector". Vectors in interface storage classes are laid out using arrays because vectors in our
// target triple have implicit padding bytes for 3-element vector types, which does not work with relaxed block layout
// or scalar block layout. We translate these arrays back to vectors before load/store operations.
//
// @param spvType : The type.
// @param matrixStride : The matrix stride (can be 0).
// @param isColumnMajor : Whether the matrix is column major.
// @param isParentPointer : If the parent is a pointer type.
// @param layout : The layout mode will be used for the type translation.
template <>
Type *SPIRVToLLVM::transTypeWithOpcode<OpTypeVector>(SPIRVType *const spvType, const unsigned matrixStride,
                                                     const bool isColumnMajor, const bool isParentPointer,
                                                     LayoutMode layout) {
  Type *const compType =
      transType(spvType->getVectorComponentType(), matrixStride, isColumnMajor, isParentPointer, layout);

  // If the vector needs explicit/std430 layout, we need to use an array to represent it because of LLVM's data layout
  // rules.
  if (layout == LayoutMode::Explicit || layout == LayoutMode::Std430)
    return ArrayType::get(compType, spvType->getVectorComponentCount());
  return FixedVectorType::get(compType, spvType->getVectorComponentCount());
}

// =====================================================================================================================
// Get pointee type from SPIRV Value.
//
// @param v : SPIRV Value
// @param layout : The layout mode will be used for the type translation.
Type *SPIRVToLLVM::getPointeeType(SPIRVValue *v, LayoutMode layout) {
  auto opCode = v->getOpCode();
  if (isAccessChainOpCode(opCode)) {
    // If the Base of the AccessChain is a structure then additional padding may be added (depending on the structure
    // elements). This additional padding is changing the return type of GetElementPointer (created from
    // OpAccessChain). Right now the easiest way is to store this type (type with padding) in the HashMap (while
    // creating GEP) and get this type here.
    //
    // Example which is showing why this is needed:
    //
    // %Uniforms = OpTypeStruct %v3double %mat2v3double %v2double
    // %_ptr_Uniform_Uniforms = OpTypePointer Uniform %Uniforms
    // %_ = OpVariable %_ptr_Uniform_Uniforms Uniform
    // %1 = OpAccessChain %_ptr_Uniform_mat2v3double %_ %int_1
    //
    // Created GetElementPointer (from %1 = OpAccessChain) will have a dereferenced return type = [2 x
    // %llpc.matrix.column], where %llpc.matrix.column = type <{ [3 x double], [8 x i8] }>  (8 x i8) is an additional
    // padding. If we just do a transType on the OpAccessChain->getType()->getPointerElementType() then we will get
    // 'only' type = [2x[3xdouble]] (without padding).
    //
    Type *pointeeType = tryGetAccessChainRetType(v);
    if (pointeeType)
      return pointeeType;
  }
  if (isStorageClassExplicitlyLaidOut(m_bm, v->getType()->getPointerStorageClass()))
    layout = LayoutMode::Explicit;

  return transType(v->getType()->getPointerElementType(), 0, true, true, layout);
}

Type *SPIRVToLLVM::transType(SPIRVType *t, unsigned matrixStride, bool columnMajor, bool parentIsPointer,
                             LayoutMode layout) {
  SPIRVTypeContext ctx(t, matrixStride, columnMajor, parentIsPointer, layout);
  auto it = m_fullTypeMap.find(ctx.asTuple());
  if (it != m_fullTypeMap.end())
    return it->second;

  auto res = transTypeImpl(t, matrixStride, columnMajor, parentIsPointer, layout);
  m_fullTypeMap[ctx.asTuple()] = res;
  return res;
}

Type *SPIRVToLLVM::transTypeImpl(SPIRVType *t, unsigned matrixStride, bool columnMajor, bool parentIsPointer,
                                 LayoutMode layout) {
  // If the type is not a sub-part of a pointer or it is a forward pointer, we can look in the map.
  if (!parentIsPointer || t->isTypeForwardPointer()) {
    auto loc = m_typeMap.find(t);
    if (loc != m_typeMap.end())
      return loc->second;
  }

  t->validate();
  switch (t->getOpCode()) {
  case OpTypeVoid:
    return mapType(t, Type::getVoidTy(*m_context));
  case OpTypeInt:
    return mapType(t, Type::getIntNTy(*m_context, t->getIntegerBitWidth()));
  case OpTypeFloat:
    return mapType(t, transFPType(t));
  case OpTypeFunction: {
    auto ft = static_cast<SPIRVTypeFunction *>(t);
    auto rt = transType(ft->getReturnType());
    std::vector<Type *> pt;
    for (size_t i = 0, e = ft->getNumParameters(); i != e; ++i)
      pt.push_back(transType(ft->getParameterType(i)));
    return mapType(t, FunctionType::get(rt, pt, false));
  }
  case OpTypeImage: {
    auto st = static_cast<SPIRVTypeImage *>(t);
    // A buffer image is represented by a texel buffer descriptor. Any other image is represented by an array
    // of three image descriptors, to allow for multi-plane YCbCr conversion. (The f-mask part of a multi-sampled
    // image is not an array of three.)
    Type *imageTy = nullptr;
    if (st->getDescriptor().Dim == DimBuffer) {
      imageTy = getBuilder()->getDescTy(ResourceNodeType::DescriptorTexelBuffer);
    } else {
      Type *singleImageTy = getBuilder()->getDescTy(ResourceNodeType::DescriptorResource);
      imageTy = ArrayType::get(singleImageTy, 3);
      if (st->getDescriptor().MS) {
        // A multisampled image is represented by a struct containing both the
        // image descriptor and the fmask descriptor.
        imageTy = StructType::get(*m_context, {imageTy, singleImageTy});
      }
    }
    return mapType(t, imageTy);
  }
  case OpTypeSampler:
  case OpTypeSampledImage: {
    // Get sampler type.
    // A sampler is represented by a struct containing the sampler itself, and the convertingSamplerIdx, an i32
    // that is either 0 or the 1-based index into the converting samplers.
    Type *ty = getBuilder()->getDescTy(ResourceNodeType::DescriptorSampler);
    ty = StructType::get(*m_context, {ty, getBuilder()->getInt32Ty()});
    if (t->getOpCode() == OpTypeSampledImage) {
      // A sampledimage is represented by a struct containing the image descriptor
      // and the sampler descriptor.
      Type *imageTy = transType(static_cast<SPIRVTypeSampledImage *>(t)->getImageType());
      ty = StructType::get(*m_context, {imageTy, ty});
    }
    return mapType(t, ty);
  }
  case OpTypeAccelerationStructureKHR: {
    auto int32x2Ty = FixedVectorType::get(Type::getInt32Ty(*m_context), 2);
    return mapType(t, int32x2Ty);
  }
  case OpTypeRayQueryKHR:
    return mapType(t, getRayQueryInternalTy(m_builder));
  case OpTypeArray: {
    Type *newTy = transTypeWithOpcode<OpTypeArray>(t, matrixStride, columnMajor, parentIsPointer, layout);
    return parentIsPointer ? newTy : mapType(t, newTy);
  }
  case OpTypeBool: {
    Type *newTy = transTypeWithOpcode<OpTypeBool>(t, matrixStride, columnMajor, parentIsPointer, layout);
    return parentIsPointer ? newTy : mapType(t, newTy);
  }
  case OpTypeForwardPointer: {
    Type *newTy = transTypeWithOpcode<OpTypeForwardPointer>(t, matrixStride, columnMajor, parentIsPointer, layout);
    return parentIsPointer ? newTy : mapType(t, newTy);
  }
  case OpTypeMatrix: {
    Type *newTy = transTypeWithOpcode<OpTypeMatrix>(t, matrixStride, columnMajor, parentIsPointer, layout);
    return parentIsPointer ? newTy : mapType(t, newTy);
  }
  case OpTypePointer: {
    Type *newTy = transTypeWithOpcode<OpTypePointer>(t, matrixStride, columnMajor, parentIsPointer, layout);
    return parentIsPointer ? newTy : mapType(t, newTy);
  }
  case OpTypeRuntimeArray: {
    Type *newTy = transTypeWithOpcode<OpTypeRuntimeArray>(t, matrixStride, columnMajor, parentIsPointer, layout);
    return parentIsPointer ? newTy : mapType(t, newTy);
  }
  case OpTypeStruct: {
    Type *newTy = transTypeWithOpcode<OpTypeStruct>(t, matrixStride, columnMajor, parentIsPointer, layout);
    return parentIsPointer ? newTy : mapType(t, newTy);
  }
  case OpTypeVector: {
    Type *newTy = transTypeWithOpcode<OpTypeVector>(t, matrixStride, columnMajor, parentIsPointer, layout);
    return parentIsPointer ? newTy : mapType(t, newTy);
  }
  default: {
    llvm_unreachable("Not implemented");
  }
  }
  return 0;
}

std::vector<Type *> SPIRVToLLVM::transTypeVector(const std::vector<SPIRVType *> &bt) {
  std::vector<Type *> t;
  t.reserve(bt.size());
  for (auto i : bt)
    t.push_back(transType(i));
  return t;
}

std::vector<Value *> SPIRVToLLVM::transValue(const std::vector<SPIRVValue *> &bv, Function *f, BasicBlock *bb) {
  std::vector<Value *> v;
  v.reserve(bv.size());
  for (auto i : bv)
    v.push_back(transValue(i, f, bb));
  return v;
}

bool SPIRVToLLVM::isSPIRVCmpInstTransToLLVMInst(SPIRVInstruction *bi) const {
  auto oc = bi->getOpCode();
  return isCmpOpCode(oc);
}

void SPIRVToLLVM::setName(llvm::Value *v, SPIRVValue *bv) {
  const auto &name = bv->getName();

  if (v->hasName())
    return;

  if (v->getType()->isVoidTy())
    return;

  if (!name.empty())
    v->setName(name);
  else if (SPIRVInstNamer && isa<Instruction>(v) && bv->hasId())
    v->setName(Twine("spv") + Twine(bv->getId()));
}

void SPIRVToLLVM::setLLVMLoopMetadata(SPIRVLoopMerge *lm, BranchInst *bi) {
  if (!lm)
    return;
  llvm::MDString *name = nullptr;
  auto temp = MDNode::getTemporary(*m_context, {});
  auto self = MDNode::get(*m_context, temp.get());
  self->replaceOperandWith(0, self);
  std::vector<llvm::Metadata *> mDs;
  if (lm->getLoopControl() == LoopControlMaskNone) {
    bi->setMetadata("llvm.loop", self);
    return;
  }
  if (lm->getLoopControl() == LoopControlUnrollMask) {
    name = llvm::MDString::get(*m_context, "llvm.loop.unroll.full");
    mDs.push_back(name);
  } else if (lm->getLoopControl() == LoopControlDontUnrollMask) {
    name = llvm::MDString::get(*m_context, "llvm.loop.unroll.disable");
    mDs.push_back(name);
  } else if (lm->getLoopControl() & LoopControlPartialCountMask) {
    name = llvm::MDString::get(*m_context, "llvm.loop.unroll.count");
    mDs.push_back(name);

    auto partialCount = ConstantInt::get(Type::getInt32Ty(*m_context), lm->getLoopControlParameters().at(0));
    mDs.push_back(ConstantAsMetadata::get(partialCount));
  }

  if (lm->getLoopControl() & LoopControlDependencyInfiniteMask ||
      (lm->getLoopControl() & LoopControlDependencyLengthMask)) {
    // TODO: DependencyInfinite probably mapped to llvm.loop.parallel_accesses with llvm.access.group
    // DependencyLength potentially useful but without llvm mappings
    return;
  }

  if (lm->getLoopControl() & LoopControlIterationMultipleMask) {
    // TODO: Potentially useful but without llvm mappings
    return;
  }
  if ((lm->getLoopControl() & LoopControlMaxIterationsMask) || (lm->getLoopControl() & LoopControlMinIterationsMask) ||
      (lm->getLoopControl() & LoopControlPeelCountMask)) {
    // No LLVM mapping and not too important
    return;
  }

  if (mDs.empty())
    return;

  // We disable all nonforced loop transformations to ensure our transformation is not blocked
  std::vector<llvm::Metadata *> mDnf;
  mDnf.push_back(llvm::MDString::get(*m_context, "llvm.loop.disable_nonforced"));

  SmallVector<llvm::Metadata *, 2> metadata;
  metadata.push_back(llvm::MDNode::get(*m_context, self));
  metadata.push_back(llvm::MDNode::get(*m_context, mDs));
  metadata.push_back(llvm::MDNode::get(*m_context, mDnf));

  llvm::MDNode *node = llvm::MDNode::get(*m_context, metadata);
  node->replaceOperandWith(0, node);
  bi->setMetadata("llvm.loop", node);
}

Value *SPIRVToLLVM::transValue(SPIRVValue *bv, Function *f, BasicBlock *bb, bool createPlaceHolder) {
  SPIRVToLLVMValueMap::iterator loc = m_valueMap.find(bv);

  if (loc != m_valueMap.end() && (!m_placeholderMap.count(bv) || createPlaceHolder))
    return loc->second;

  bv->validate();

  auto v = transValueWithoutDecoration(bv, f, bb, createPlaceHolder);
  if (!v)
    return nullptr;
  setName(v, bv);
  if (!transDecoration(bv, v)) {
    assert(0 && "trans decoration fail");
    return nullptr;
  }
  m_dbgTran.recordsValue(bv, v);

  return v;
}

Value *SPIRVToLLVM::transConvertInst(SPIRVValue *bv, Function *f, BasicBlock *bb) {
  SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
  auto src = transValue(bc->getOperand(0), f, bb, bb != nullptr);
  auto srcType = src->getType();
  auto dstType = transType(bc->getType());
  CastInst::CastOps co = Instruction::BitCast;

  bool isExt = dstType->getScalarSizeInBits() > srcType->getScalarSizeInBits();
  switch (bc->getOpCode()) {
  case OpSConvert:
    co = isExt ? Instruction::SExt : Instruction::Trunc;
    break;
  case OpUConvert:
    co = isExt ? Instruction::ZExt : Instruction::Trunc;
    break;
  case OpFConvert:
    co = isExt ? Instruction::FPExt : Instruction::FPTrunc;
    break;
  default:
    co = static_cast<CastInst::CastOps>(OpCodeMap::rmap(bc->getOpCode()));
  }

  if (dstType == srcType)
    return src;
  assert(CastInst::isCast(co) && "Invalid cast op code");
  if (bb) {
    bool srcIsPtr = srcType->isPtrOrPtrVectorTy();
    bool dstIsPtr = dstType->isPtrOrPtrVectorTy();
    // OpBitcast in SPIR-V allows casting between pointers and integers (and integer vectors),
    // but LLVM BitCast does not allow converting pointers to other types, PtrToInt and IntToPtr
    // should be used instead.
    if (co == Instruction::BitCast && srcIsPtr != dstIsPtr) {
      auto int64Ty = Type::getInt64Ty(*m_context);
      if (srcIsPtr) {
        assert(dstType->isIntOrIntVectorTy());
        Instruction *ret = new PtrToIntInst(src, int64Ty, bv->getName(), bb);
        if (dstType != int64Ty)
          ret = new BitCastInst(ret, dstType, bv->getName(), bb);
        return ret;
      }

      if (dstIsPtr) {
        assert(srcType->isIntOrIntVectorTy());
        if (srcType != int64Ty)
          src = new BitCastInst(src, int64Ty, bv->getName(), bb);
        return new IntToPtrInst(src, dstType, bv->getName(), bb);
      }
    } else {
      return CastInst::Create(co, src, dstType, bv->getName(), bb);
    }
  }
  return ConstantExpr::getCast(co, dyn_cast<Constant>(src), dstType);
}

static FastMathFlags buildFastMathFlags(unsigned flags) {
  FastMathFlags fmf;

  if (flags & FastMathFlags::AllowReassoc)
    fmf.setAllowReassoc();

  if (flags & FastMathFlags::NoNaNs)
    fmf.setNoNaNs();

  if (flags & FastMathFlags::NoInfs)
    fmf.setNoInfs();

  if (flags & FastMathFlags::NoSignedZeros)
    fmf.setNoSignedZeros();

  if (flags & FastMathFlags::AllowReciprocal)
    fmf.setAllowReciprocal();

  if (flags & FastMathFlags::AllowContract)
    fmf.setAllowContract();

  if (flags & FastMathFlags::ApproxFunc)
    fmf.setApproxFunc();

  return fmf;
}

// Decide what fast math flags to set in Builder, just before generating
// code for BV. Decorations on BV may prevent us from setting some flags.
FastMathFlags SPIRVToLLVM::getFastMathFlags(SPIRVValue *bv) {
  // For floating-point operations, if "FastMath" is enabled, set the "FastMath"
  // flags on the handled instruction
  if (const unsigned flags = m_shaderOptions->fastMathFlags)
    return buildFastMathFlags(flags);

  // Only do this for operations with floating point type.
  if (!bv->hasType())
    return {};
  SPIRVType *ty = bv->getType();
  if (ty->isTypeVector())
    ty = ty->getVectorComponentType();
  else if (ty->isTypeMatrix())
    ty = ty->getMatrixColumnType()->getVectorComponentType();
  if (!ty->isTypeFloat())
    return {};

  FastMathFlags fmf;

  fmf.setAllowReciprocal();
  if (!ty->isTypeFloat(64)) {
    // Only do this for half and float, not double, to avoid problems with Vulkan CTS precision_double tests.
    fmf.setApproxFunc();
  }
  // Enable contraction when "NoContraction" decoration is not specified
  bool allowContract = !bv->hasDecorate(DecorationNoContraction);
  // If invariant variable is used, we cannot enable contraction
  allowContract &= !m_moduleUsage->useInvariant;
  // Do not set AllowContract or AllowReassoc if DenormFlushToZero is on, to
  // avoid an FP operation being simplified to a move that does not flush
  // denorms.
  if (m_fpControlFlags.DenormFlushToZero == 0) {
    // Allow contract
    fmf.setAllowContract(allowContract);
    // AllowRessociation should be same with AllowContract
    fmf.setAllowReassoc(allowContract);
  }
  // Enable "no NaN" and "no signed zeros" only if there isn't any floating point control flags
  if (m_fpControlFlags.U32All == 0) {
    if (!m_moduleUsage->useIsNan)
      fmf.setNoNaNs();

    fmf.setNoSignedZeros();
  }

  if (m_shaderOptions->disableFastMathFlags)
    fmf &= buildFastMathFlags(~m_shaderOptions->disableFastMathFlags);

  return fmf;
}

// Set fast math flags in Builder, just before generating
// code for BV.
void SPIRVToLLVM::setFastMathFlags(SPIRVValue *bv) {
  getBuilder()->setFastMathFlags(getFastMathFlags(bv));
}

// Set fast math flags on just-generated instruction Val.
// This is only needed if the instruction was not generated by Builder, or using
// a Builder method that does not honor FMF such as CreateMinNum.
void SPIRVToLLVM::setFastMathFlags(Value *val) {
  if (auto inst = dyn_cast<Instruction>(val)) {
    if (isa<FPMathOperator>(inst))
      inst->setFastMathFlags(getBuilder()->getFastMathFlags());
  }
}

Value *SPIRVToLLVM::transShiftLogicalBitwiseInst(SPIRVValue *bv, BasicBlock *bb, Function *f) {
  SPIRVBinary *bbn = static_cast<SPIRVBinary *>(bv);
  assert(bb && "Invalid BB");
  Instruction::BinaryOps bo;
  auto op = bbn->getOpCode();
  if (isLogicalOpCode(op))
    op = IntBoolOpMap::rmap(op);
  bo = static_cast<Instruction::BinaryOps>(OpCodeMap::rmap(op));
  auto base = transValue(bbn->getOperand(0), f, bb);
  auto shift = transValue(bbn->getOperand(1), f, bb);

  // NOTE: SPIR-V spec allows the operands "base" and "shift" to have different
  // bit width.
  if (shift->getType()->isIntOrIntVectorTy())
    shift = getBuilder()->CreateZExtOrTrunc(shift, base->getType());

  if (isShiftOpCode(op)) {
    // An out-of-range shift amount returns an undefined value in SPIR-V but poison in LLVM IR. The least disruptive way
    // of fixing this is to mask the shift amount so it is always in-range. This generates AND instructions which can
    // usually be folded away in IR or elided during instruction selection.
    unsigned baseWidth = base->getType()->getScalarSizeInBits();
    assert(isPowerOf2_32(baseWidth));
    Constant *rhs = getBuilder()->getIntN(baseWidth, baseWidth - 1);
    if (base->getType()->isVectorTy())
      rhs = ConstantVector::getSplat(cast<VectorType>(base->getType())->getElementCount(), rhs);
    shift = getBuilder()->CreateAnd(shift, rhs);
  }

  auto inst = BinaryOperator::Create(bo, base, shift, bv->getName(), bb);
  setFastMathFlags(inst);

  return inst;
}

Value *SPIRVToLLVM::transCmpInst(SPIRVValue *bv, BasicBlock *bb, Function *f) {
  SPIRVCompare *bc = static_cast<SPIRVCompare *>(bv);
  assert(bb && "Invalid BB");
  auto op = bc->getOpCode();

  if (isLogicalOpCode(op))
    op = IntBoolOpMap::rmap(op);
  return getBuilder()->CreateCmp(CmpMap::rmap(op), transValue(bc->getOperand(0), f, bb),
                                 transValue(bc->getOperand(1), f, bb));
}

// =====================================================================================================================
// Perform matrix transpose on given input type.
//
// @param t : type which will be transposed.
static Type *getTransposedType(Type *t) {
  assert(t->isArrayTy() && t->getArrayElementType()->isStructTy());
  Type *const rowVectorType = t->getArrayElementType()->getStructElementType(0);
  assert(rowVectorType->isArrayTy());

  Type *const newColumnVectorType = ArrayType::get(rowVectorType->getArrayElementType(), t->getArrayNumElements());
  Type *const transposedType = ArrayType::get(newColumnVectorType, rowVectorType->getArrayNumElements());

  return transposedType;
}

// =====================================================================================================================
// Given the LLVM IR type of a part of SPIR-V matrix, return the part's "dimensionality" (i.e.: 0 for an element,
// 1 for a column, 2 for the entire matrix)
//
static unsigned getMatrixPartDim(Type *type) {
  if (auto *arrayType = dyn_cast<ArrayType>(type)) {
    if (isa<ArrayType>(arrayType->getArrayElementType()))
      return 2;
    else
      return 1;
  }
  if (isa<FixedVectorType>(type))
    return 1;
  assert(type->isIntegerTy() || type->isFloatingPointTy());
  return 0;
}

// =====================================================================================================================
// Create a GEP to an element of a row-major matrix. Return the pointer to the element and its alignment, assuming the
// given matrix alignment.
std::pair<Value *, Align> SPIRVToLLVM::createGepIntoRowMajorMatrix(Type *matrixType, Value *matrixPtr,
                                                                   Align matrixAlign, Value *row, Value *col) {
  // The matrix type is [nrows x {[ncols x T], pad}]
  Value *zero = m_builder->getInt32(0);
  Value *indices[] = {zero, row ? row : zero, zero, col ? col : zero};
  Value *pointer = m_builder->CreateGEP(matrixType, matrixPtr, indices);
  Align align = matrixAlign;
  if (auto *gep = dyn_cast<GEPOperator>(pointer)) {
    const DataLayout &dl = m_m->getDataLayout();
    unsigned bitWidth = dl.getIndexSizeInBits(gep->getPointerAddressSpace());
    MapVector<Value *, APInt> variableOffsets;
    APInt constantOffset{bitWidth, 0};
    bool success = gep->collectOffset(dl, bitWidth, variableOffsets, constantOffset);
    (void)success;
    assert(success);
    align = commonAlignment(align, constantOffset.getZExtValue());
    for (const auto &item : variableOffsets) {
      align = commonAlignment(align, item.second.getZExtValue());
    }
  }
  return {pointer, align};
}

// =====================================================================================================================
// Post process the module to remove row major matrix uses.
bool SPIRVToLLVM::postProcessRowMajorMatrix() {
  SmallVector<Instruction *, 8> valuesToRemove;
  SmallVector<Function *> functionsToRemove;

  for (Function &func : m_m->functions()) {
    if (!func.getName().startswith(SpirvLaunderRowMajor))
      continue;

    // Remember to remove the function later.
    functionsToRemove.push_back(&func);

    for (User *const user : func.users()) {
      CallInst *const call = dyn_cast<CallInst>(user);

      assert(call);

      // Remember to remove the call later.
      valuesToRemove.push_back(call);

      Value *const matrix = call->getArgOperand(0);
      Type *const matrixType = func.getArg(1)->getType();
      // Get return type of the function by transposing matrix type.
      Type *const destType = getTransposedType(func.getArg(1)->getType());

      assert(destType->isArrayTy());

      const unsigned colCount = destType->getArrayNumElements();
      const unsigned rowCount = destType->getArrayElementType()->getArrayNumElements();

      Type *const matrixElementType = destType->getArrayElementType()->getArrayElementType();

      // Map a pointer to its corresponding (row, column) index (as i32) into the matrix. Either index may be null,
      // indicating a constant 0.
      DenseMap<Value *, std::pair<Value *, Value *>> valueMap;

      valueMap.try_emplace(call, nullptr, nullptr);

      SmallVector<Value *, 8> workList(call->user_begin(), call->user_end());

      while (!workList.empty()) {
        auto *const inst = cast<Instruction>(workList.pop_back_val());

        // Remember to remove the instruction later.
        valuesToRemove.push_back(inst);

        m_builder->SetInsertPoint(inst);

        if (auto *const bitcast = dyn_cast<BitCastInst>(inst)) {
          // Bitcasts should generally disappear with opaque pointers, but being able to look through them is correct
          // and shouldn't hurt.
          valueMap[bitcast] = valueMap.lookup(bitcast->getOperand(0));
        } else if (auto *const gep = dyn_cast<GetElementPtrInst>(inst)) {
          auto [row, col] = valueMap.lookup(gep->getPointerOperand());
          Type *gepType = gep->getSourceElementType();
          unsigned dim = getMatrixPartDim(gepType);
          assert(gep->getNumIndices() <= dim + 1);

          auto idx_it = gep->idx_begin();
          if (dim == 2) {
            assert(cast<Constant>(*idx_it)->isNullValue());
            idx_it++;
            dim--;
          }

          if (dim == 1 && idx_it != gep->idx_end()) {
            Value *idx = *idx_it++;
            idx = m_builder->CreateZExtOrTrunc(idx, m_builder->getInt32Ty());
            if (col)
              col = m_builder->CreateAdd(col, idx);
            else
              col = idx;
            dim--;
          }

          if (dim == 0 && idx_it != gep->idx_end()) {
            Value *idx = *idx_it++;
            idx = m_builder->CreateZExtOrTrunc(idx, m_builder->getInt32Ty());
            if (row)
              row = m_builder->CreateAdd(row, idx);
            else
              row = idx;
            dim--;
          }

          assert(idx_it == gep->idx_end());

          valueMap.try_emplace(gep, row, col);
        } else if (auto *const load = dyn_cast<LoadInst>(inst)) {
          // For loads we have to handle three cases:
          // 1. We are loading a full matrix, so do a load + transpose.
          // 2. We are loading a column of a matrix, and since this is represented as a vector of GEPs we need
          //    to issue a load for each element of this vector and recombine the result.
          // 3. We are loading a single scalar element, do a simple load.

          auto [ptrRow, ptrCol] = valueMap.lookup(load->getPointerOperand());
          unsigned dim = getMatrixPartDim(load->getType());

          assert(dim <= 1 || (!ptrCol || cast<Constant>(ptrCol)->isNullValue()));
          assert(dim == 0 || (!ptrRow || cast<Constant>(ptrRow)->isNullValue()));

          Type *columnType = nullptr;
          if (dim == 2)
            columnType = load->getType()->getArrayElementType();
          else if (dim == 1)
            columnType = load->getType();

          Value *result = dim >= 1 ? PoisonValue::get(load->getType()) : nullptr;
          Value *const columnInitial = dim >= 1 ? PoisonValue::get(columnType) : nullptr;

          unsigned loadCol = 0;
          do {
            Value *col = ptrCol;
            if (col && loadCol != 0)
              col = m_builder->CreateAdd(col, m_builder->getInt32(loadCol));
            if (!col)
              col = m_builder->getInt32(loadCol);

            Value *columnResult = columnInitial;

            unsigned loadRow = 0;
            do {
              Value *row = ptrRow;
              if (row && loadRow != 0)
                row = m_builder->CreateAdd(row, m_builder->getInt32(loadRow));
              if (!row)
                row = m_builder->getInt32(loadRow);

              auto [ptr, align] = createGepIntoRowMajorMatrix(matrixType, matrix, load->getAlign(), row, col);
              LoadInst *const newLoad = m_builder->CreateAlignedLoad(matrixElementType, ptr, align, load->isVolatile());
              newLoad->setOrdering(load->getOrdering());
              newLoad->setSyncScopeID(load->getSyncScopeID());

              if (load->getMetadata(LLVMContext::MD_nontemporal))
                transNonTemporalMetadata(newLoad);

              if (dim == 0) {
                columnResult = newLoad;
              } else {
                if (isa<ArrayType>(columnType))
                  columnResult = m_builder->CreateInsertValue(columnResult, newLoad, loadRow);
                else
                  columnResult = m_builder->CreateInsertElement(columnResult, newLoad, loadRow);
              }

              ++loadRow;
            } while (dim >= 1 && loadRow < rowCount);

            if (dim <= 1)
              result = columnResult;
            else
              result = m_builder->CreateInsertValue(result, columnResult, loadCol);

            ++loadCol;
          } while (dim == 2 && loadCol < colCount);

          load->replaceAllUsesWith(result);

          // don't add users to worklist
          continue;
        } else if (auto *const store = dyn_cast<StoreInst>(inst)) {
          // For stores we have to handle three cases:
          // 1. We are storing a full matrix, so do a transpose + store.
          // 2. We are storing a column of a matrix, and since this is represented as a vector of GEPs we need
          //    to extract each element and issue a store.
          // 3. We are storing a single scalar element, do a simple store.

          auto [ptrRow, ptrCol] = valueMap.lookup(store->getPointerOperand());
          unsigned dim = getMatrixPartDim(store->getValueOperand()->getType());

          assert(dim <= 1 || (!ptrCol || cast<Constant>(ptrCol)->isNullValue()));
          assert(dim == 0 || (!ptrRow || cast<Constant>(ptrRow)->isNullValue()));

          unsigned storeCol = 0;
          do {
            Value *col = ptrCol;
            if (col && storeCol != 0)
              col = m_builder->CreateAdd(col, m_builder->getInt32(storeCol));
            if (!col)
              col = m_builder->getInt32(storeCol);

            Value *column;
            if (dim <= 1)
              column = store->getValueOperand();
            else
              column = m_builder->CreateExtractValue(store->getValueOperand(), storeCol);

            unsigned storeRow = 0;
            do {
              Value *row = ptrRow;
              if (row && storeRow != 0)
                row = m_builder->CreateAdd(row, m_builder->getInt32(storeRow));
              if (!row)
                row = m_builder->getInt32(storeRow);

              Value *element;
              if (dim == 0) {
                element = column;
              } else {
                if (isa<ArrayType>(column->getType()))
                  element = m_builder->CreateExtractValue(column, storeRow);
                else
                  element = m_builder->CreateExtractElement(column, storeRow);
              }

              auto [ptr, align] = createGepIntoRowMajorMatrix(matrixType, matrix, store->getAlign(), row, col);
              StoreInst *const newStore = m_builder->CreateAlignedStore(element, ptr, align, store->isVolatile());
              newStore->setOrdering(store->getOrdering());
              newStore->setSyncScopeID(store->getSyncScopeID());

              if (store->getMetadata(LLVMContext::MD_nontemporal))
                transNonTemporalMetadata(newStore);

              ++storeRow;
            } while (dim >= 1 && storeRow < rowCount);
            ++storeCol;
          } while (dim == 2 && storeCol < colCount);

          // don't add users to worklist
          continue;
        } else if (auto *const callInst = dyn_cast<CallInst>(inst)) {
          if (callInst->getCalledFunction()->getName().startswith(gSPIRVMD::NonUniform)) {
            // don't add users to worklist
            continue;
          }

          llvm_unreachable("laundered row-major matrix pointer should not be used by call");
        } else if (auto *const cmpInst = dyn_cast<CmpInst>(inst)) {
          if (cmpInst->use_empty())
            continue;

          Value *lhs = cmpInst->getOperand(0);
          Value *rhs = cmpInst->getOperand(1);
          auto [lhsRow, lhsCol] = valueMap.lookup(lhs);
          if (lhsRow || lhsCol)
            lhs = createGepIntoRowMajorMatrix(matrixType, matrix, {}, lhsRow, lhsCol).first;
          auto [rhsRow, rhsCol] = valueMap.lookup(rhs);
          if (rhsRow || rhsCol)
            rhs = createGepIntoRowMajorMatrix(matrixType, matrix, {}, rhsRow, rhsCol).first;

          Value *newCmpInst = m_builder->CreateCmp(cmpInst->getPredicate(), lhs, rhs, cmpInst->getName());
          cmpInst->replaceAllUsesWith(newCmpInst);
          // Drop all references to eliminate double add to remove list.
          // This can happen when we are comparing pointers of two matrices of row major.
          cmpInst->dropAllReferences();

          // don't add users to worklist
          continue;
        } else {
          llvm_unreachable("Should never be called!");
        }

        for (User *const user : inst->users()) {
          // Add only unique instructions to the list. The same pointer may be used twice due to compare instructions.
          if (std::find(workList.begin(), workList.end(), user) == workList.end())
            workList.push_back(user);
        }
      }
    }
  }

  const bool changed = !valuesToRemove.empty() || !functionsToRemove.empty();

  while (!valuesToRemove.empty()) {
    auto *const inst = valuesToRemove.pop_back_val();
    inst->dropAllReferences();
    inst->eraseFromParent();
  }
  while (!functionsToRemove.empty()) {
    auto *const func = functionsToRemove.pop_back_val();
    func->dropAllReferences();
    func->eraseFromParent();
  }

  return changed;
}

/// Construct a DebugLoc for the given SPIRVInstruction.
DebugLoc SPIRVToLLVM::getDebugLoc(SPIRVInstruction *bi, Function *f) {
  if (!f || !bi->hasLine())
    return DebugLoc();
  auto line = bi->getLine();
  SPIRVFunction *sf = bi->getParent()->getParent();
  assert(sf);
  DISubprogram *sp = m_dbgTran.getDISubprogram(sf);
  if (!sp) {
    return DebugLoc();
  }
  return DILocation::get(sp->getContext(), line->getLine(), line->getColumn(), sp);
}

void SPIRVToLLVM::updateDebugLoc(SPIRVValue *bv, Function *f) {
  if (bv->isInst()) {
    SPIRVInstruction *bi = static_cast<SPIRVInstruction *>(bv);
    getBuilder()->SetCurrentDebugLocation(getDebugLoc(bi, f));
  }
}

// =====================================================================================================================
// Create a call to launder a row major matrix.
//
// @param pointerToMatrix : The pointer to matrix to launder.
std::pair<Type *, Value *> SPIRVToLLVM::createLaunderRowMajorMatrix(Type *const matrixType,
                                                                    Value *const pointerToMatrix) {
  Type *const matrixPointerType = pointerToMatrix->getType();

  Type *const newMatrixType = getTransposedType(matrixType);
  Type *const newMatrixPointerType = newMatrixType->getPointerTo(matrixPointerType->getPointerAddressSpace());

  // Dummy value used to remember matrixType which will be used in postProcessRowMajorMatrix.
  Value *dummyValue = Constant::getNullValue(matrixType);
  Value *args[] = {pointerToMatrix, dummyValue};
  FunctionType *const rowMajorFuncType =
      FunctionType::get(newMatrixPointerType, {matrixPointerType, matrixType}, false);
  string mangledName(SpirvLaunderRowMajor);
  appendTypeMangling(newMatrixPointerType, args, mangledName);
  Function *rowMajorFunc = m_m->getFunction(mangledName);
  if (!rowMajorFunc) {
    rowMajorFunc = Function::Create(rowMajorFuncType, GlobalValue::ExternalLinkage, SpirvLaunderRowMajor, m_m);
  }
  Value *call = getBuilder()->CreateCall(rowMajorFunc, args);
  return std::make_pair(newMatrixType, call);
}

// =====================================================================================================================
// Creates a load, taking care for types where we have had to add in explicit pads (structs with offset, arrays, and
// matrices) to only load the data that is being used. This will recursively step through the pointer to load from.
//
// @param spvType : The SPIR-V type of the load.
// @param loadPointer : The LLVM pointer to load from.
// @param loadType : The LLVM load type.
// @param isVolatile : Is the load volatile?
// @param isCoherent : Is the load coherent?
// @param isNonTemporal : Is the load non-temporal?
Value *SPIRVToLLVM::addLoadInstRecursively(SPIRVType *const spvType, Value *loadPointer, Type *loadType,
                                           bool isVolatile, bool isCoherent, bool isNonTemporal) {
  assert(loadPointer->getType()->isPointerTy());

  if (isTypeWithPadRowMajorMatrix(loadType)) {
    auto loadPair = createLaunderRowMajorMatrix(loadType, loadPointer);
    loadType = loadPair.first;
    loadPointer = loadPair.second;
  }

  Constant *const zero = getBuilder()->getInt32(0);
  if (loadType->isStructTy() && !spvType->isTypeSampledImage() && !spvType->isTypeImage() &&
      !spvType->isTypeSampler() && spvType->getOpCode() != OpTypeRayQueryKHR) {
    // For structs we lookup the mapping of the elements and use it to reverse map the values.
    const bool needsPad = isRemappedTypeElements(spvType);

    SmallVector<Value *, 8> memberLoads;
    SmallVector<Type *, 8> memberTypes;

    for (unsigned i = 0, memberCount = spvType->getStructMemberCount(); i < memberCount; i++) {
      const unsigned memberIndex = needsPad ? lookupRemappedTypeElements(spvType, i) : i;

      SmallVector<Value *, 2> indices = {zero, getBuilder()->getInt32(memberIndex)};

      Value *memberLoadPointer = getBuilder()->CreateGEP(loadType, loadPointer, indices);

      Type *memberLoadType = nullptr;

      // If the struct member was one which overlapped another member (as is common with HLSL cbuffer layout), we
      // need to handle the struct member carefully.
      auto pair = std::make_pair(spvType, i);
      if (m_overlappingStructTypeWorkaroundMap.count(pair) > 0) {
        memberLoadType = m_overlappingStructTypeWorkaroundMap[pair];
        Type *const type = memberLoadType->getPointerTo(memberLoadPointer->getType()->getPointerAddressSpace());
        memberLoadPointer = getBuilder()->CreateBitCast(memberLoadPointer, type);
      }

      if (!memberLoadType)
        memberLoadType = GetElementPtrInst::getIndexedType(loadType, indices);

      Value *const memberLoad = addLoadInstRecursively(spvType->getStructMemberType(i), memberLoadPointer,
                                                       memberLoadType, isVolatile, isCoherent, isNonTemporal);

      memberLoads.push_back(memberLoad);
      memberTypes.push_back(memberLoad->getType());
    }

    Value *load = PoisonValue::get(StructType::get(m_m->getContext(), memberTypes));

    for (unsigned i = 0, memberCount = spvType->getStructMemberCount(); i < memberCount; i++)
      load = getBuilder()->CreateInsertValue(load, memberLoads[i], i);

    return load;
  }
  if (loadType->isArrayTy() && !spvType->isTypeVector() && !spvType->isTypeImage()) {
    // Matrix and arrays both get here. For both we need to turn [<{element-type, pad}>] into [element-type].
    const bool needsPad = isTypeWithPad(loadType);

    SPIRVType *const spvElementType =
        spvType->isTypeArray() ? spvType->getArrayElementType() : spvType->getMatrixColumnType();

    Type *elementType = transType(spvElementType);

    Value *load = PoisonValue::get(ArrayType::get(elementType, loadType->getArrayNumElements()));

    for (unsigned i = 0, elementCount = loadType->getArrayNumElements(); i < elementCount; i++) {
      SmallVector<Value *, 3> indices;
      indices.push_back(zero);
      indices.push_back(getBuilder()->getInt32(i));

      if (needsPad)
        indices.push_back(zero);

      Value *elementLoadPointer = getBuilder()->CreateGEP(loadType, loadPointer, indices);
      Type *const elementLoadType = GetElementPtrInst::getIndexedType(loadType, indices);
      Value *const elementLoad = addLoadInstRecursively(spvElementType, elementLoadPointer, elementLoadType, isVolatile,
                                                        isCoherent, isNonTemporal);
      load = getBuilder()->CreateInsertValue(load, elementLoad, i);
    }

    return load;
  }
  if (spvType->isTypeVector() && isCoherent) {
    // Coherent load operand must be integer, pointer, or floating point type, so need to spilte vector.
    SPIRVType *spvElementType = spvType->getVectorComponentType();
    Type *elementType = transType(spvElementType);
    Value *load = PoisonValue::get(VectorType::get(elementType, spvType->getVectorComponentCount(), false));
    for (unsigned i = 0, elementCount = spvType->getVectorComponentCount(); i < elementCount; i++) {
      Value *const elementLoadPointer =
          getBuilder()->CreateGEP(loadType, loadPointer, {zero, getBuilder()->getInt32(i)});
      Value *const elementLoad = addLoadInstRecursively(spvElementType, elementLoadPointer, elementType, isVolatile,
                                                        isCoherent, isNonTemporal);
      load = getBuilder()->CreateInsertElement(load, elementLoad, i);
    }

    return load;
  }
  Type *alignmentType = loadType;

  // Vectors are represented as arrays in memory, so we need to cast the array to a vector before loading.
  if (spvType->isTypeVector()) {
    Type *const vectorType = transType(spvType, 0, false, true, LayoutMode::Native);
    Type *const castType = vectorType->getPointerTo(loadPointer->getType()->getPointerAddressSpace());
    loadPointer = getBuilder()->CreateBitCast(loadPointer, castType);
    loadType = vectorType;

    const bool scalarBlockLayout = getPipelineOptions()->scalarBlockLayout;

    if (!scalarBlockLayout)
      alignmentType = vectorType;
  }

  LoadInst *const load = getBuilder()->CreateAlignedLoad(
      loadType, loadPointer, m_m->getDataLayout().getABITypeAlign(alignmentType), isVolatile);

  if (isCoherent)
    load->setAtomic(AtomicOrdering::Unordered);

  if (isNonTemporal)
    transNonTemporalMetadata(load);

  // If the load was a bool or vector of bool, need to truncate the result.
  if (spvType->isTypeBool() || (spvType->isTypeVector() && spvType->getVectorComponentType()->isTypeBool()))
    return getBuilder()->CreateTruncOrBitCast(load, transType(spvType));
  return load;
}

// =====================================================================================================================
// Creates a store, taking care for types where we have had to add in explicit pads (structs with offset, arrays, and
// matrices) to only store the data that is being used. This will recursively step through the value to store.
//
// @param spvType : The SPIR-V type of the store.
// @param storePointer : The LLVM pointer to store to.
// @param storeType : The LLVM type of the store.
// @param storeValue : The LLVM value to store into the pointer.
// @param isVolatile : Is the store volatile?
// @param isCoherent : Is the store coherent?
// @param isNonTemporal : Is the store non-temporal?
void SPIRVToLLVM::addStoreInstRecursively(SPIRVType *const spvType, Value *storePointer, Type *storeType,
                                          Value *storeValue, bool isVolatile, bool isCoherent, bool isNonTemporal) {
  assert(storePointer->getType()->isPointerTy());

  if (isTypeWithPadRowMajorMatrix(storeType)) {
    auto storePair = createLaunderRowMajorMatrix(storeType, storePointer);
    storePointer = storePair.second;
    storeType = storePair.first;
  }

  const Align alignment = m_m->getDataLayout().getABITypeAlign(storeType);

  // Special case if we are storing a constant value, we build up a modified constant, and store that - but only if
  // the alignment is greater than 1 (if the constant is storing an entire structure, because we have to use packed
  // structs to encoded layout information from SPIR-V into LLVM, we can very easily output large stores with align 1
  // that causes problems with the load/store vectorizer and DAG combining).
  if (isa<Constant>(storeValue) && alignment > 1) {
    Constant *const constStoreValue =
        buildConstStoreRecursively(spvType, storePointer->getType(), storeType, cast<Constant>(storeValue));

    StoreInst *const store = getBuilder()->CreateAlignedStore(constStoreValue, storePointer, alignment, isVolatile);

    if (isCoherent)
      store->setAtomic(AtomicOrdering::Unordered);

    if (isNonTemporal)
      transNonTemporalMetadata(store);

    return;
  }

  Value *const zero = getBuilder()->getInt32(0);
  if (storeType->isStructTy() && !spvType->isTypeSampledImage() && !spvType->isTypeImage() &&
      !spvType->isTypeSampler() && spvType->getOpCode() != OpTypeRayQueryKHR) {
    // For structs we lookup the mapping of the elements and use it to map the values.
    const bool needsPad = isRemappedTypeElements(spvType);

    for (unsigned i = 0, memberCount = spvType->getStructMemberCount(); i < memberCount; i++) {
      const unsigned memberIndex = needsPad ? lookupRemappedTypeElements(spvType, i) : i;
      Value *indices[] = {zero, getBuilder()->getInt32(memberIndex)};
      Value *const memberStorePointer = getBuilder()->CreateGEP(storeType, storePointer, indices);
      Type *const memberStoreType = GetElementPtrInst::getIndexedType(storeType, indices);
      Value *const memberStoreValue = getBuilder()->CreateExtractValue(storeValue, i);
      addStoreInstRecursively(spvType->getStructMemberType(i), memberStorePointer, memberStoreType, memberStoreValue,
                              isVolatile, isCoherent, isNonTemporal);
    }
  } else if (storeType->isArrayTy() && !spvType->isTypeVector() && !spvType->isTypeImage()) {
    // Matrix and arrays both get here. For both we need to turn [element-type] into [<{element-type, pad}>].
    const bool needsPad = isTypeWithPad(storeType);

    SPIRVType *const spvElementType =
        spvType->isTypeArray() ? spvType->getArrayElementType() : spvType->getMatrixColumnType();

    for (unsigned i = 0, elementCount = storeType->getArrayNumElements(); i < elementCount; i++) {
      SmallVector<Value *, 3> indices;
      indices.push_back(zero);
      indices.push_back(getBuilder()->getInt32(i));

      if (needsPad)
        indices.push_back(zero);

      Value *const elementStorePointer = getBuilder()->CreateGEP(storeType, storePointer, indices);
      Type *const elementStoreType = GetElementPtrInst::getIndexedType(storeType, indices);
      Value *const elementStoreValue = getBuilder()->CreateExtractValue(storeValue, i);
      addStoreInstRecursively(spvElementType, elementStorePointer, elementStoreType, elementStoreValue, isVolatile,
                              isCoherent, isNonTemporal);
    }
  } else if (spvType->isTypeVector() && isCoherent) {
    // Coherent store operand must be integer, pointer, or floating point type, so need to spilte vector.
    SPIRVType *spvElementType = spvType->getVectorComponentType();

    for (unsigned i = 0, elementCount = spvType->getVectorComponentCount(); i < elementCount; i++) {
      Value *indices[] = {zero, getBuilder()->getInt32(i)};
      Value *const elementStorePointer = getBuilder()->CreateGEP(storeType, storePointer, indices);
      Type *const elementStoreType = GetElementPtrInst::getIndexedType(storeType, indices);
      Value *const elementStoreValue = getBuilder()->CreateExtractElement(storeValue, i);
      addStoreInstRecursively(spvElementType, elementStorePointer, elementStoreType, elementStoreValue, isVolatile,
                              isCoherent, isNonTemporal);
    }
  } else {
    Type *alignmentType = storeType;

    Type *storeType = nullptr;

    // If the store was a bool or vector of bool, need to zext the storing value.
    if (spvType->isTypeBool() || (spvType->isTypeVector() && spvType->getVectorComponentType()->isTypeBool())) {
      storeValue = getBuilder()->CreateZExtOrBitCast(storeValue, alignmentType);
      storeType = storeValue->getType();
    } else
      storeType = transType(spvType);

    // Vectors are represented as arrays in memory, so we need to cast the array to a vector before storing.
    if (spvType->isTypeVector()) {
      Type *const castType = storeType->getPointerTo(storePointer->getType()->getPointerAddressSpace());
      storePointer = getBuilder()->CreateBitCast(storePointer, castType);

      const bool scalarBlockLayout = getPipelineOptions()->scalarBlockLayout;

      if (!scalarBlockLayout)
        alignmentType = storeType;
    }

    StoreInst *const store = getBuilder()->CreateAlignedStore(
        storeValue, storePointer, m_m->getDataLayout().getABITypeAlign(alignmentType), isVolatile);

    if (isCoherent)
      store->setAtomic(AtomicOrdering::Unordered);

    if (isNonTemporal)
      transNonTemporalMetadata(store);
  }
}

// =====================================================================================================================
// Build a modified constant to store.
//
// @param spvType : The SPIR-V type of the store.
// @param storePointerType : The LLVM pointer to store to.
// @param storeType : The LLVM type of store.
// @param constStoreValue : The LLVM constant to store into the pointer.
Constant *SPIRVToLLVM::buildConstStoreRecursively(SPIRVType *const spvType, Type *const storePointerType,
                                                  Type *const storeType, Constant *constStoreValue) {
  assert(storePointerType->isPointerTy());

  const unsigned addrSpace = storePointerType->getPointerAddressSpace();

  Constant *const zero = getBuilder()->getInt32(0);

  if (storeType->isStructTy() && spvType->getOpCode() != OpTypeSampledImage && spvType->getOpCode() != OpTypeImage) {
    // For structs we lookup the mapping of the elements and use it to map the values.
    const bool needsPad = isRemappedTypeElements(spvType);

    SmallVector<Constant *, 8> constMembers(storeType->getStructNumElements(), nullptr);

    // First run through the final LLVM type and create undef's for the members
    for (unsigned i = 0, memberCount = constMembers.size(); i < memberCount; i++)
      constMembers[i] = PoisonValue::get(storeType->getStructElementType(i));

    // Then run through the SPIR-V type and set the non-undef members to actual constants.
    for (unsigned i = 0, memberCount = spvType->getStructMemberCount(); i < memberCount; i++) {
      const unsigned memberIndex = needsPad ? lookupRemappedTypeElements(spvType, i) : i;
      Constant *indices[] = {zero, getBuilder()->getInt32(memberIndex)};
      Type *const memberStoreType = GetElementPtrInst::getIndexedType(storeType, indices);
      constMembers[memberIndex] =
          buildConstStoreRecursively(spvType->getStructMemberType(i), memberStoreType->getPointerTo(addrSpace),
                                     memberStoreType, constStoreValue->getAggregateElement(i));
    }

    return ConstantStruct::get(cast<StructType>(storeType), constMembers);
  }
  if (storeType->isArrayTy() && !spvType->isTypeVector()) {
    // Matrix and arrays both get here. For both we need to turn [element-type] into [<{element-type, pad}>].
    const bool needsPad = isTypeWithPad(storeType);

    SmallVector<Constant *, 8> constElements(storeType->getArrayNumElements(),
                                             PoisonValue::get(storeType->getArrayElementType()));

    SPIRVType *const spvElementType =
        spvType->isTypeArray() ? spvType->getArrayElementType() : spvType->getMatrixColumnType();

    for (unsigned i = 0, elementCount = storeType->getArrayNumElements(); i < elementCount; i++) {
      SmallVector<Value *, 3> indices;
      indices.push_back(zero);
      indices.push_back(getBuilder()->getInt32(i));

      if (needsPad)
        indices.push_back(zero);

      Type *const elementStoreType = GetElementPtrInst::getIndexedType(storeType, indices);
      Constant *const constElement =
          buildConstStoreRecursively(spvElementType, elementStoreType->getPointerTo(addrSpace), elementStoreType,
                                     constStoreValue->getAggregateElement(i));

      if (needsPad) {
        constElements[i] = llvm::ConstantFoldInsertValueInstruction(constElements[i], constElement, 0);
        assert(constElements[i] && "unexpected error creating aggregate initializer, malformed aggregate?");
      } else {
        constElements[i] = constElement;
      }
    }

    return ConstantArray::get(cast<ArrayType>(storeType), constElements);
  }
  // If the store was a bool or vector of bool, need to zext the storing value.
  if (spvType->isTypeBool() || (spvType->isTypeVector() && spvType->getVectorComponentType()->isTypeBool()))
    constStoreValue = ConstantExpr::getZExtOrBitCast(constStoreValue, storeType);

  // If the LLVM type is a not a vector, we need to change the constant into an array.
  if (spvType->isTypeVector() && !storeType->isVectorTy()) {
    assert(storeType->isArrayTy());

    SmallVector<Constant *, 8> constElements(storeType->getArrayNumElements(), nullptr);

    for (unsigned i = 0, compCount = spvType->getVectorComponentCount(); i < compCount; i++)
      constElements[i] = constStoreValue->getAggregateElement(i);

    return ConstantArray::get(cast<ArrayType>(storeType), constElements);
  }

  return constStoreValue;
}

// =====================================================================================================================
// Translate scope from SPIR-V to LLVM.
//
// @param context : The LLVM context.
// @param spvScope : The scope to translate.
static SyncScope::ID transScope(LLVMContext &context, const SPIRVConstant *const spvScope) {
  const unsigned scope = static_cast<unsigned>(spvScope->getZExtIntValue());

  switch (scope) {
  case ScopeCrossDevice:
  case ScopeDevice:
  case ScopeQueueFamilyKHR:
    return SyncScope::System;
  case ScopeInvocation:
    return SyncScope::SingleThread;
  case ScopeWorkgroup:
    return context.getOrInsertSyncScopeID("workgroup");
  case ScopeSubgroup:
    return context.getOrInsertSyncScopeID("wavefront");
  default:
    llvm_unreachable("Should never be called!");
    return SyncScope::System;
  }
}

// =====================================================================================================================
// Translate memory semantics from SPIR-V to LLVM.
//
// @param spvMemorySemantics : The semantics to translate.
// @param isAtomicRMW : Is the memory semantic from an atomic rmw operation.
static AtomicOrdering transMemorySemantics(const SPIRVConstant *const spvMemorySemantics, const bool isAtomicRMW) {
  const unsigned semantics = static_cast<unsigned>(spvMemorySemantics->getZExtIntValue());

  if (semantics & MemorySemanticsSequentiallyConsistentMask)
    return AtomicOrdering::SequentiallyConsistent;
  if (semantics & MemorySemanticsAcquireReleaseMask)
    return AtomicOrdering::AcquireRelease;
  if (semantics & MemorySemanticsAcquireMask)
    return AtomicOrdering::Acquire;
  if (semantics & MemorySemanticsReleaseMask)
    return AtomicOrdering::Release;
  if (semantics & (MemorySemanticsMakeAvailableKHRMask | MemorySemanticsMakeVisibleKHRMask))
    return AtomicOrdering::Monotonic;

  return AtomicOrdering::Monotonic;
}

// =====================================================================================================================
// Translate any read-modify-write atomics.
//
// @param spvValue : A SPIR-V value.
// @param binOp : The binary operator.
Value *SPIRVToLLVM::transAtomicRMW(SPIRVValue *const spvValue, const AtomicRMWInst::BinOp binOp) {
  SPIRVAtomicInstBase *const spvAtomicInst = static_cast<SPIRVAtomicInstBase *>(spvValue);

  const SyncScope::ID scope = transScope(*m_context, static_cast<SPIRVConstant *>(spvAtomicInst->getOpValue(1)));
  const AtomicOrdering ordering =
      transMemorySemantics(static_cast<SPIRVConstant *>(spvAtomicInst->getOpValue(2)), true);

  Value *const atomicPointer = transValue(spvAtomicInst->getOpValue(0), getBuilder()->GetInsertBlock()->getParent(),
                                          getBuilder()->GetInsertBlock());
  Value *const atomicValue = transValue(spvAtomicInst->getOpValue(3), getBuilder()->GetInsertBlock()->getParent(),
                                        getBuilder()->GetInsertBlock());

  // NOTE: This is a workaround. atomic.swap.f64 is not supported in LLVM backend, so we convert double to int64.
  if (binOp == AtomicRMWInst::Xchg && atomicValue->getType()->isDoubleTy()) {
    Value *const int64Value = getBuilder()->CreateBitCast(atomicValue, getBuilder()->getInt64Ty());

    Value *const int64AtomicPointer = getBuilder()->CreateBitCast(
        atomicPointer, PointerType::get(getBuilder()->getInt64Ty(),
                                        dyn_cast<PointerType>(atomicPointer->getType())->getAddressSpace()));
    Value *const atomicRes =
        getBuilder()->CreateAtomicRMW(binOp, int64AtomicPointer, int64Value, MaybeAlign(), ordering, scope);

    return getBuilder()->CreateBitCast(atomicRes, getBuilder()->getDoubleTy());
  }

  return getBuilder()->CreateAtomicRMW(binOp, atomicPointer, atomicValue, MaybeAlign(), ordering, scope);
}

// =====================================================================================================================
// Handle OpAtomicLoad.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicLoad>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  SPIRVAtomicLoad *const spvAtomicLoad = static_cast<SPIRVAtomicLoad *>(spvValue);

  const SyncScope::ID scope = transScope(*m_context, static_cast<SPIRVConstant *>(spvAtomicLoad->getOpValue(1)));
  const AtomicOrdering ordering =
      transMemorySemantics(static_cast<SPIRVConstant *>(spvAtomicLoad->getOpValue(2)), false);

  Value *const loadPointer = transValue(spvAtomicLoad->getOpValue(0), getBuilder()->GetInsertBlock()->getParent(),
                                        getBuilder()->GetInsertBlock());
  Type *const loadType = transType(spvAtomicLoad->getType());

  const unsigned loadAlignment = static_cast<unsigned>(m_m->getDataLayout().getTypeSizeInBits(loadType) / 8);
  LoadInst *const load = getBuilder()->CreateAlignedLoad(loadType, loadPointer, Align(loadAlignment));

  load->setAtomic(ordering, scope);

  return load;
}

// =====================================================================================================================
// Handle OpAtomicStore.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicStore>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  SPIRVAtomicStore *const spvAtomicStore = static_cast<SPIRVAtomicStore *>(spvValue);

  const SyncScope::ID scope = transScope(*m_context, static_cast<SPIRVConstant *>(spvAtomicStore->getOpValue(1)));
  const AtomicOrdering ordering =
      transMemorySemantics(static_cast<SPIRVConstant *>(spvAtomicStore->getOpValue(2)), false);

  Value *const storePointer = transValue(spvAtomicStore->getOpValue(0), getBuilder()->GetInsertBlock()->getParent(),
                                         getBuilder()->GetInsertBlock());
  Value *const storeValue = transValue(spvAtomicStore->getOpValue(3), getBuilder()->GetInsertBlock()->getParent(),
                                       getBuilder()->GetInsertBlock());

  const uint64_t storeSizeInBits = m_m->getDataLayout().getTypeSizeInBits(storeValue->getType());
  const unsigned storeAlignment = static_cast<unsigned>(storeSizeInBits / 8);
  StoreInst *const store = getBuilder()->CreateAlignedStore(storeValue, storePointer, Align(storeAlignment));

  store->setAtomic(ordering, scope);

  return store;
}

// =====================================================================================================================
// Handle OpAtomicExchange.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicExchange>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  return transAtomicRMW(spvValue, AtomicRMWInst::Xchg);
}

// =====================================================================================================================
// Handle OpAtomicIAdd.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicIAdd>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  return transAtomicRMW(spvValue, AtomicRMWInst::Add);
}

// =====================================================================================================================
// Handle OpAtomicISub.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicISub>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  return transAtomicRMW(spvValue, AtomicRMWInst::Sub);
}

// =====================================================================================================================
// Handle OpAtomicSMin.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicSMin>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  return transAtomicRMW(spvValue, AtomicRMWInst::Min);
}

// =====================================================================================================================
// Handle OpAtomicUMin.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicUMin>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  return transAtomicRMW(spvValue, AtomicRMWInst::UMin);
}

// =====================================================================================================================
// Handle OpAtomicSMax.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicSMax>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  return transAtomicRMW(spvValue, AtomicRMWInst::Max);
}

// =====================================================================================================================
// Handle OpAtomicUMax.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicUMax>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  return transAtomicRMW(spvValue, AtomicRMWInst::UMax);
}

// =====================================================================================================================
// Handle OpAtomicAnd.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicAnd>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  return transAtomicRMW(spvValue, AtomicRMWInst::And);
}

// =====================================================================================================================
// Handle OpAtomicOr.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicOr>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  return transAtomicRMW(spvValue, AtomicRMWInst::Or);
}

// =====================================================================================================================
// Handle OpAtomicXor.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicXor>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  return transAtomicRMW(spvValue, AtomicRMWInst::Xor);
}

// =====================================================================================================================
// Handle OpAtomicFMinEXT.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicFMinEXT>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  return transAtomicRMW(spvValue, AtomicRMWInst::FMin);
}

// =====================================================================================================================
// Handle OpAtomicFMaxEXT.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicFMaxEXT>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  return transAtomicRMW(spvValue, AtomicRMWInst::FMax);
}

// =====================================================================================================================
// Handle OpAtomicFAddEXT.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicFAddEXT>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  return transAtomicRMW(spvValue, AtomicRMWInst::FAdd);
}

// =====================================================================================================================
// Handle OpAtomicIIncrement.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicIIncrement>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  SPIRVAtomicInstBase *const spvAtomicInst = static_cast<SPIRVAtomicInstBase *>(spvValue);

  const SyncScope::ID scope = transScope(*m_context, static_cast<SPIRVConstant *>(spvAtomicInst->getOpValue(1)));
  const AtomicOrdering ordering =
      transMemorySemantics(static_cast<SPIRVConstant *>(spvAtomicInst->getOpValue(2)), true);

  Value *const atomicPointer = transValue(spvAtomicInst->getOpValue(0), getBuilder()->GetInsertBlock()->getParent(),
                                          getBuilder()->GetInsertBlock());

  Value *const one = ConstantInt::get(transType(spvAtomicInst->getOpValue(0)->getType()->getPointerElementType()), 1);

  return getBuilder()->CreateAtomicRMW(AtomicRMWInst::Add, atomicPointer, one, MaybeAlign(), ordering, scope);
}

// =====================================================================================================================
// Handle OpAtomicIDecrement.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicIDecrement>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  SPIRVAtomicInstBase *const spvAtomicInst = static_cast<SPIRVAtomicInstBase *>(spvValue);

  const SyncScope::ID scope = transScope(*m_context, static_cast<SPIRVConstant *>(spvAtomicInst->getOpValue(1)));
  const AtomicOrdering ordering =
      transMemorySemantics(static_cast<SPIRVConstant *>(spvAtomicInst->getOpValue(2)), true);

  Value *const atomicPointer = transValue(spvAtomicInst->getOpValue(0), getBuilder()->GetInsertBlock()->getParent(),
                                          getBuilder()->GetInsertBlock());

  Value *const one = ConstantInt::get(transType(spvAtomicInst->getOpValue(0)->getType()->getPointerElementType()), 1);

  return getBuilder()->CreateAtomicRMW(AtomicRMWInst::Sub, atomicPointer, one, MaybeAlign(), ordering, scope);
}

// =====================================================================================================================
// Handle OpAtomicCompareExchange.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAtomicCompareExchange>(SPIRVValue *const spvValue) {
  // Image texel atomic operations use the older path for now.
  if (static_cast<SPIRVInstruction *>(spvValue)->getOperands()[0]->getOpCode() == OpImageTexelPointer) {
    return transSPIRVImageAtomicOpFromInst(static_cast<SPIRVInstruction *>(spvValue), getBuilder()->GetInsertBlock());
  }

  SPIRVAtomicInstBase *const spvAtomicInst = static_cast<SPIRVAtomicInstBase *>(spvValue);

  const SyncScope::ID scope = transScope(*m_context, static_cast<SPIRVConstant *>(spvAtomicInst->getOpValue(1)));
  const AtomicOrdering successOrdering =
      transMemorySemantics(static_cast<SPIRVConstant *>(spvAtomicInst->getOpValue(2)), true);
  const AtomicOrdering failureOrdering =
      transMemorySemantics(static_cast<SPIRVConstant *>(spvAtomicInst->getOpValue(3)), true);

  Value *const atomicPointer = transValue(spvAtomicInst->getOpValue(0), getBuilder()->GetInsertBlock()->getParent(),
                                          getBuilder()->GetInsertBlock());
  Value *const exchangeValue = transValue(spvAtomicInst->getOpValue(4), getBuilder()->GetInsertBlock()->getParent(),
                                          getBuilder()->GetInsertBlock());
  Value *const compareValue = transValue(spvAtomicInst->getOpValue(5), getBuilder()->GetInsertBlock()->getParent(),
                                         getBuilder()->GetInsertBlock());

  AtomicCmpXchgInst *const atomicCmpXchg = getBuilder()->CreateAtomicCmpXchg(
      atomicPointer, compareValue, exchangeValue, MaybeAlign(), successOrdering, failureOrdering, scope);

  // LLVM cmpxchg returns { <ty>, i1 }, for SPIR-V we only care about the <ty>.
  return getBuilder()->CreateExtractValue(atomicCmpXchg, 0);
}

// =====================================================================================================================
// Handle OpCopyMemory.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpCopyMemory>(SPIRVValue *const spvValue) {
  SPIRVCopyMemory *const spvCopyMemory = static_cast<SPIRVCopyMemory *>(spvValue);

  bool isSrcVolatile = spvCopyMemory->SPIRVMemoryAccess::isVolatile(true);

  // We don't require volatile on address spaces that become non-pointers.
  switch (spvCopyMemory->getSource()->getType()->getPointerStorageClass()) {
  case StorageClassInput:
  case StorageClassOutput:
  case StorageClassPrivate:
  case StorageClassFunction:
    isSrcVolatile = false;
    break;
  default:
    break;
  }

  bool isDestVolatile = spvCopyMemory->SPIRVMemoryAccess::isVolatile(false);

  // We don't require volatile on address spaces that become non-pointers.
  switch (spvCopyMemory->getTarget()->getType()->getPointerStorageClass()) {
  case StorageClassInput:
  case StorageClassOutput:
  case StorageClassPrivate:
  case StorageClassFunction:
    isDestVolatile = false;
    break;
  default:
    break;
  }

  bool isCoherent = false;

  if (spvCopyMemory->getMemoryAccessMask(true) & MemoryAccessMakePointerVisibleKHRMask) {
    SPIRVWord spvId = spvCopyMemory->getMakeVisibleScope(true);
    SPIRVConstant *const spvScope = static_cast<SPIRVConstant *>(m_bm->getValue(spvId));
    const unsigned scope = spvScope->getZExtIntValue();

    const bool isSystemScope = (scope <= ScopeDevice || scope == ScopeQueueFamilyKHR);

    if (isSystemScope)
      isCoherent = true;
  }
  if (spvCopyMemory->getMemoryAccessMask(true) & MemoryAccessNonPrivatePointerKHRMask)
    isCoherent = true;

  if (spvCopyMemory->getMemoryAccessMask(false) & MemoryAccessMakePointerAvailableKHRMask) {
    SPIRVWord spvId = spvCopyMemory->getMakeAvailableScope(false);
    SPIRVConstant *const spvScope = static_cast<SPIRVConstant *>(m_bm->getValue(spvId));
    const unsigned scope = spvScope->getZExtIntValue();

    const bool isSystemScope = (scope <= ScopeDevice || scope == ScopeQueueFamilyKHR);

    if (isSystemScope)
      isCoherent = true;
  }
  if (spvCopyMemory->getMemoryAccessMask(false) & MemoryAccessNonPrivatePointerKHRMask)
    isCoherent = true;

  bool isNonTemporal = spvCopyMemory->SPIRVMemoryAccess::isNonTemporal(true);

  Value *const loadPointer = transValue(spvCopyMemory->getSource(), getBuilder()->GetInsertBlock()->getParent(),
                                        getBuilder()->GetInsertBlock());

  SPIRVType *const spvLoadType = spvCopyMemory->getSource()->getType();
  LayoutMode loadLayout = isStorageClassExplicitlyLaidOut(m_bm, spvLoadType->getPointerStorageClass())
                              ? LayoutMode::Explicit
                              : LayoutMode::Native;

  Type *const loadType = transType(spvLoadType->getPointerElementType(), 0, true, true, loadLayout);

  Value *const load = addLoadInstRecursively(spvLoadType->getPointerElementType(), loadPointer, loadType, isSrcVolatile,
                                             isCoherent, isNonTemporal);

  Value *const storePointer = transValue(spvCopyMemory->getTarget(), getBuilder()->GetInsertBlock()->getParent(),
                                         getBuilder()->GetInsertBlock());

  SPIRVType *const spvStoreType = spvCopyMemory->getTarget()->getType();
  LayoutMode storeLayout = isStorageClassExplicitlyLaidOut(m_bm, spvStoreType->getPointerStorageClass())
                               ? LayoutMode::Explicit
                               : LayoutMode::Native;
  Type *const storeType = transType(spvStoreType->getPointerElementType(), 0, true, true, storeLayout);
  isNonTemporal = spvCopyMemory->SPIRVMemoryAccess::isNonTemporal(false);

  addStoreInstRecursively(spvStoreType->getPointerElementType(), storePointer, storeType, load, isDestVolatile,
                          isCoherent, isNonTemporal);
  return nullptr;
}

// =====================================================================================================================
// Handle OpLoad.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpLoad>(SPIRVValue *const spvValue) {
  SPIRVLoad *const spvLoad = static_cast<SPIRVLoad *>(spvValue);
  LayoutMode layout = LayoutMode::Native;

  // Handle UniformConstant image/sampler/sampledimage load.
  if (static_cast<SPIRVTypePointer *>(spvLoad->getSrc()->getType())->getStorageClass() == StorageClassUniformConstant) {
    switch (spvLoad->getType()->getOpCode()) {
    case OpTypeImage:
    case OpTypeSampler:
    case OpTypeSampledImage:
      return transLoadImage(spvLoad->getSrc());
    case OpTypeAccelerationStructureKHR: {
      auto pipelineContext = (static_cast<Llpc::Context *>(m_context))->getPipelineContext();
      if (pipelineContext->getRayTracingState()->forceInvalidAccelStruct) {
        // Always return invalid AS address (0x0, 0x0) if the option is set.
        auto loadType = cast_or_null<FixedVectorType>(getPointeeType(spvLoad->getSrc()));
        assert(loadType && (loadType->getNumElements() == 2));
        (void(loadType)); // unused
        return ConstantVector::get({m_builder->getInt32(0), m_builder->getInt32(0)});
      }
      break;
    }
    default:
      layout = LayoutMode::Std430;
      break;
    }
  }

  Value *const loadPointer =
      transValue(spvLoad->getSrc(), getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());

  bool isVolatile = spvLoad->SPIRVMemoryAccess::isVolatile(true);
  const Vkgc::ExtendedRobustness &extendedRobustness = getPipelineOptions()->extendedRobustness;
  if (extendedRobustness.nullDescriptor || extendedRobustness.robustBufferAccess)
    isVolatile |= spvLoad->getSrc()->isVolatile();

  // Translate a volatile load of BuiltInHelperInvocation to a call to IsHelperInvocation.
  if ((isVolatile || spvLoad->getSrc()->isVolatile()) &&
      spvLoad->getSrc()->getType()->getPointerStorageClass() == StorageClassInput) {
    if (GlobalVariable *gv = dyn_cast<GlobalVariable>(loadPointer)) {
      SPIRVBuiltinVariableKind kind;
      if (isSPIRVBuiltinVariable(gv, &kind)) {
        if (kind == spv::BuiltInHelperInvocation)
          return getBuilder()->CreateIsHelperInvocation();
      }
    }
  }

  // We don't require volatile on address spaces that become non-pointers.
  switch (spvLoad->getSrc()->getType()->getPointerStorageClass()) {
  case StorageClassInput:
  case StorageClassOutput:
  case StorageClassPrivate:
  case StorageClassFunction:
    isVolatile = false;
    break;
  default:
    break;
  }

  bool isCoherent = spvLoad->getSrc()->isCoherent();

  // MakePointerVisibleKHR is valid with OpLoad
  if (spvLoad->getMemoryAccessMask(true) & MemoryAccessMakePointerVisibleKHRMask) {
    SPIRVWord spvId = spvLoad->getMakeVisibleScope(true);
    SPIRVConstant *const spvScope = static_cast<SPIRVConstant *>(m_bm->getValue(spvId));
    const unsigned scope = spvScope->getZExtIntValue();

    const bool isSystemScope = (scope <= ScopeDevice || scope == ScopeQueueFamilyKHR);

    if (isSystemScope)
      isCoherent = true;
  }

  if (spvLoad->getMemoryAccessMask(true) & MemoryAccessNonPrivatePointerKHRMask)
    isCoherent = true;

  if (spvLoad->getSrc()->getType()->getPointerStorageClass() == StorageClassTaskPayloadWorkgroupEXT)
    isCoherent = true;

  const bool isNonTemporal = spvLoad->SPIRVMemoryAccess::isNonTemporal(true);

  SPIRVType *const spvLoadType = spvLoad->getSrc()->getType();

  Instruction *baseNode = getLastInsertedValue();
  BasicBlock *currentBlock = getBuilder()->GetInsertBlock();

  Type *loadType = getPointeeType(spvLoad->getSrc(), layout);

  auto loadInst = addLoadInstRecursively(spvLoadType->getPointerElementType(), loadPointer, loadType, isVolatile,
                                         isCoherent, isNonTemporal);

  // Record all load instructions inserted by addLoadInstRecursively.
  if (m_scratchBoundsChecksEnabled) {
    gatherScratchBoundsChecksMemOps(spvLoad, baseNode, currentBlock, LlvmMemOpType::IS_LOAD, currentBlock->getParent());
  }

  if (spvLoad->getType()->getOpCode() == OpTypeAccelerationStructureKHR) {
    // From now on (GPURT major version >= 34), AS header may start at a non-zero offset, GPURT now request base offset
    // of the resource, and it will calculate the actual GPUVA, instead of compiler providing one loaded from offset 0.
    // We'll lower the AS global value later in llpcSpirvLowerGlobal, here we clamp the loaded GPUVA to first 48 bits.
    assert(loadInst->getType()->isVectorTy());
    loadInst = getBuilder()->CreateInsertElement(
        loadInst, getBuilder()->CreateAnd(getBuilder()->CreateExtractElement(loadInst, 1), 0xFFFF), 1);
  }

  return loadInst;
}

// =====================================================================================================================
// Translate a load for UniformConstant that is image/sampler/sampledimage
//
// @param spvImageLoadPtr : The image/sampler/sampledimage pointer
Value *SPIRVToLLVM::transLoadImage(SPIRVValue *spvImageLoadPtr) {
  SPIRVType *spvElementTy = spvImageLoadPtr->getType()->getPointerElementType();
  Type *elementTy = transType(spvElementTy, 0, false, false, LayoutMode::Native);
  Value *base = transImagePointer(spvImageLoadPtr);
  return loadImageSampler(elementTy, base);
}

// =====================================================================================================================
// Generate a load of an image, sampler or sampledimage
//
// @param elementTy : Element type being loaded
// @param base : Pointer to load from
Value *SPIRVToLLVM::loadImageSampler(Type *elementTy, Value *base) {
  if (auto structTy = dyn_cast<StructType>(elementTy)) {
    if (!structTy->getElementType(1)->isIntegerTy()) {
      // The item being loaded is a struct of two items that need loading separately (excluding the case below that
      // is it a struct with an i32, which is a sampler with its convertingSamplerIdx). There are two cases
      // of that:
      // 1. A sampledimage is an image plus a sampler.
      // 2. An image that is multisampled is an image plus an fmask.
      Value *ptr1 = getBuilder()->CreateExtractValue(base, 1);
      Value *element1 = loadImageSampler(structTy->getElementType(1), ptr1);
      Value *ptr0 = getBuilder()->CreateExtractValue(base, 0);
      Value *element0 = loadImageSampler(structTy->getElementType(0), ptr0);
      Value *result = getBuilder()->CreateInsertValue(PoisonValue::get(structTy), element0, 0);
      result = getBuilder()->CreateInsertValue(result, element1, 1);
      return result;
    }

    // The item being loaded is a struct where element 1 is integer. That must be a sampler with its i32
    // convertingSamplerIdx. The loaded value inherits the convertingSamplerIdx from the
    // {pointer,stride,convertingSamplerIdx} struct that represents the descriptor pointer.
    Value *convertingSamplerIdx = getBuilder()->CreateExtractValue(base, 2);
    Value *loadedVal = loadImageSampler(structTy->getElementType(0), base);
    loadedVal = getBuilder()->CreateInsertValue(PoisonValue::get(structTy), loadedVal, 0);
    return getBuilder()->CreateInsertValue(loadedVal, convertingSamplerIdx, 1);
  }

  // The image or sampler "descriptor" is in fact a struct containing the pointer and stride. We only
  // need the pointer here.
  Value *ptr = getBuilder()->CreateExtractValue(base, 0);

  if (auto arrayTy = dyn_cast<ArrayType>(elementTy)) {
    // The element type being loaded is an array. That must be where a non-texel-buffer image is represented as
    // an array of three image descriptors, to allow for multiple planes in YCbCr conversion. Normally we only
    // load one descriptor; if there are any converting samplers, we load all three, and rely on later optimizations
    // to remove the unused ones (and thus stop us reading off the end of the descriptor table).
    elementTy = arrayTy->getElementType();
    auto *oneVal = getBuilder()->CreateLoad(elementTy, ptr);
    oneVal->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(*m_context, {}));

    Value *result = getBuilder()->CreateInsertValue(PoisonValue::get(arrayTy), oneVal, 0);
    // Pointer to image is represented as a struct containing {pointer, stride, planeStride, isResource}.
    if (!m_convertingSamplers.empty() && base->getType()->getStructNumElements() >= 4) {
      Value *planeStride = getBuilder()->CreateExtractValue(base, 2);
      Type *ptrTy = ptr->getType();

      for (unsigned planeIdx = 1; planeIdx != arrayTy->getNumElements(); ++planeIdx) {
        ptr = getBuilder()->CreateBitCast(
            ptr, getBuilder()->getInt8Ty()->getPointerTo(ptr->getType()->getPointerAddressSpace()));
        ptr = getBuilder()->CreateGEP(getBuilder()->getInt8Ty(), ptr, planeStride);
        ptr = getBuilder()->CreateBitCast(ptr, ptrTy);
        oneVal = getBuilder()->CreateLoad(elementTy, ptr);
        oneVal->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(*m_context, {}));
        result = getBuilder()->CreateInsertValue(result, oneVal, planeIdx);
      }
    }
    return result;
  }

  // Other cases: Just load the element from the pointer.
  auto load = getBuilder()->CreateLoad(elementTy, ptr);
  load->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(*m_context, {}));
  return load;
}

// =====================================================================================================================
// Translate image/sampler/sampledimage pointer to IR value
//
// @param spvImagePtr : The image/sampler/sampledimage pointer
Value *SPIRVToLLVM::transImagePointer(SPIRVValue *spvImagePtr) {
  if (spvImagePtr->getOpCode() != OpVariable ||
      static_cast<SPIRVTypePointer *>(spvImagePtr->getType())->getStorageClass() != StorageClassUniformConstant)
    return transValue(spvImagePtr, getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());

  // For an image/sampler/sampledimage pointer that is a UniformConstant OpVariable, we need to materialize it by
  // generating the code to get the descriptor pointer(s).
  SPIRVWord binding = 0;
  unsigned descriptorSet = 0;

  spvImagePtr->hasDecorate(DecorationBinding, 0, &binding);
  bool hasDescriptorSet = spvImagePtr->hasDecorate(DecorationDescriptorSet, 0, &descriptorSet);
  assert(!getPipelineOptions()->replaceSetWithResourceType || !hasDescriptorSet ||
         static_cast<SPIRVTypePointer *>(spvImagePtr->getType())->getStorageClass() == StorageClassUniformConstant);
  (void)hasDescriptorSet;

  SPIRVType *spvTy = spvImagePtr->getType()->getPointerElementType();
  while (spvTy->getOpCode() == OpTypeArray || spvTy->getOpCode() == OpTypeRuntimeArray)
    spvTy = spvTy->getArrayElementType();

  Value *imageDescPtr = nullptr;
  Value *samplerDescPtr = nullptr;

  if (getPipelineOptions()->replaceSetWithResourceType)
    assert(spvTy->getOpCode() != OpTypeSampler);

  if (spvTy->getOpCode() != OpTypeSampler) {
    // Image or sampledimage -- need to get the image pointer-and-stride.
    SPIRVType *spvImageTy = spvTy;
    if (spvTy->getOpCode() == OpTypeSampledImage)
      spvImageTy = static_cast<SPIRVTypeSampledImage *>(spvTy)->getImageType();
    assert(spvImageTy->getOpCode() == OpTypeImage);

    auto desc = &static_cast<SPIRVTypeImage *>(spvImageTy)->getDescriptor();
    auto resType =
        desc->Dim == DimBuffer ? ResourceNodeType::DescriptorTexelBuffer : ResourceNodeType::DescriptorResource;

    if (getPipelineOptions()->replaceSetWithResourceType) {
      if (spvTy->getOpCode() == OpTypeImage) {
        descriptorSet = PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorImage);
      } else if (spvTy->getOpCode() == OpTypeSampledImage) {
        if (getPipelineOptions()->enableCombinedTexture) {
          descriptorSet =
              PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorCombinedTexture);
        } else {
          descriptorSet =
              PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorResource);
        }
      }
    }

    imageDescPtr = getDescPointerAndStride(resType, descriptorSet, binding, resType);

    if (desc->MS) {
      if (getPipelineOptions()->replaceSetWithResourceType && spvTy->getOpCode() != OpTypeImage)
        descriptorSet = PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorFmask);
      // A multisampled image pointer is a struct containing an image desc pointer and an fmask desc pointer.
      Value *fmaskDescPtr = getDescPointerAndStride(ResourceNodeType::DescriptorFmask, descriptorSet, binding,
                                                    ResourceNodeType::DescriptorFmask);
      imageDescPtr = getBuilder()->CreateInsertValue(
          PoisonValue::get(StructType::get(*m_context, {imageDescPtr->getType(), fmaskDescPtr->getType()})),
          imageDescPtr, 0);
      imageDescPtr = getBuilder()->CreateInsertValue(imageDescPtr, fmaskDescPtr, 1);
    }
  }

  if (spvTy->getOpCode() != OpTypeImage) {
    if (getPipelineOptions()->replaceSetWithResourceType && !getPipelineOptions()->enableCombinedTexture)
      descriptorSet = PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorSampler);
    // Sampler or sampledimage -- need to get the sampler {pointer,stride,convertingSamplerIdx}
    samplerDescPtr = getDescPointerAndStride(ResourceNodeType::DescriptorSampler, descriptorSet, binding,
                                             ResourceNodeType::DescriptorSampler);

    if (spvTy->getOpCode() == OpTypeSampler)
      return samplerDescPtr;
  }

  if (imageDescPtr) {
    if (samplerDescPtr) {
      Value *descPtr =
          PoisonValue::get(StructType::get(*m_context, {imageDescPtr->getType(), samplerDescPtr->getType()}));
      descPtr = getBuilder()->CreateInsertValue(descPtr, imageDescPtr, 0);
      descPtr = getBuilder()->CreateInsertValue(descPtr, samplerDescPtr, 1);
      return descPtr;
    }
    return imageDescPtr;
  }
  return samplerDescPtr;
}

// =====================================================================================================================
// Get an image/sampler descriptor pointer-and-stride struct
//
// @param resType : ResourceNodeType value
// @param descriptorSet : Descriptor set
// @param binding : Binding
// @param searchType : ResourceNodeType to find user resource node
Value *SPIRVToLLVM::getDescPointerAndStride(ResourceNodeType resType, unsigned descriptorSet, unsigned binding,
                                            ResourceNodeType searchType) {
  if (resType != ResourceNodeType::DescriptorSampler) {
    // f-mask/texel buffer, where a pointer is represented by a struct {pointer,stride}.
    Value *descPtr = getBuilder()->CreateGetDescPtr(resType, searchType, descriptorSet, binding);
    Value *descStride = getBuilder()->CreateGetDescStride(resType, searchType, descriptorSet, binding);
    descPtr = getBuilder()->CreateInsertValue(
        PoisonValue::get(StructType::get(*m_context, {descPtr->getType(), descStride->getType(), descStride->getType(),
                                                      getBuilder()->getInt32Ty()})),
        descPtr, 0);
    descPtr = getBuilder()->CreateInsertValue(descPtr, descStride, 1);

    if (resType == ResourceNodeType::DescriptorResource) {
      // Image, where a pointer is represented by a struct {pointer, stride, planeStride, isResource}
      unsigned convertingSamplerIdx = 0;
      unsigned nextIdx = 1;
      for (const ConvertingSampler &convertingSampler : m_convertingSamplers) {
        if (convertingSampler.set == descriptorSet && convertingSampler.binding == binding) {
          convertingSamplerIdx = nextIdx;
          break;
        }
        nextIdx += convertingSampler.values.size() / ConvertingSamplerDwordCount;
      }
      if (convertingSamplerIdx == 0) {
        descPtr = getBuilder()->CreateInsertValue(descPtr, getBuilder()->getInt32(DescriptorSizeResource), 2);
      } else {
        // Sampler Descriptor includes {sampler, YCbCrMetaDta}
        auto samplerMetadata =
            m_convertingSamplers[convertingSamplerIdx - 1].values.data() + DescriptorSizeSamplerInDwords;
        Value *planes = getBuilder()->getInt32(
            reinterpret_cast<const Vkgc::SamplerYCbCrConversionMetaData *>(samplerMetadata)->word1.planes);
        Value *planeStride = getBuilder()->CreateUDiv(descStride, planes);
        descPtr = getBuilder()->CreateInsertValue(descPtr, planeStride, 2);
      }
      descPtr = getBuilder()->CreateInsertValue(descPtr, getBuilder()->getInt32(1), 3);
    }
    return descPtr;
  }

  // A sampler pointer is represented by a struct {pointer,stride,convertingSamplerIdx}, where
  // convertingSamplerIdx is 0 or the 1-based converting sampler index. Here we use descriptorSet and binding
  // to detect whether it is a converting sampler, and set up the converting sampler index.
  unsigned convertingSamplerIdx = 0;
  unsigned nextIdx = 1;
  for (const ConvertingSampler &convertingSampler : m_convertingSamplers) {
    if (convertingSampler.set == descriptorSet && convertingSampler.binding == binding) {
      convertingSamplerIdx = nextIdx;
      break;
    }
    nextIdx += convertingSampler.values.size() / ConvertingSamplerDwordCount;
  }
  Type *samplerPtrTy = StructType::get(*m_context, {getBuilder()->getDescPtrTy(ResourceNodeType::DescriptorSampler),
                                                    getBuilder()->getInt32Ty(), getBuilder()->getInt32Ty()});
  Value *samplerDescPtr = Constant::getNullValue(samplerPtrTy);

  if (convertingSamplerIdx == 0) {
    // Not a converting sampler. Get a normal sampler pointer and stride and put it in the struct.
    samplerDescPtr = getBuilder()->CreateInsertValue(
        samplerDescPtr, getBuilder()->CreateGetDescPtr(resType, searchType, descriptorSet, binding), 0);
    samplerDescPtr = getBuilder()->CreateInsertValue(
        samplerDescPtr, getBuilder()->CreateGetDescStride(resType, searchType, descriptorSet, binding), 1);
  } else {
    // It is a converting sampler. Return the struct with just the converting sampler index.
    samplerDescPtr = getBuilder()->CreateInsertValue(samplerDescPtr, getBuilder()->getInt32(convertingSamplerIdx), 2);
  }
  return samplerDescPtr;
}

// =====================================================================================================================
// Handle OpStore.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpStore>(SPIRVValue *const spvValue) {
  SPIRVStore *const spvStore = static_cast<SPIRVStore *>(spvValue);

  if (spvStore->getDst()->hasDecorate(DecorationNonWritable)) {
    // Discard any writes to non-writable memory as these are not permitted.
    // For debugging purposes assert that these are only duplicate stores of the initializer of an OpVariable.
    assert(spvStore->getDst()->getOpCode() == OpVariable);
    assert(static_cast<SPIRVVariable *>(spvStore->getDst())->getInitializer() == spvStore->getSrc());
    return nullptr;
  }

  bool isVolatile = spvStore->SPIRVMemoryAccess::isVolatile(false);
  const Vkgc::ExtendedRobustness &extendedRobustness = getPipelineOptions()->extendedRobustness;
  if (extendedRobustness.nullDescriptor || extendedRobustness.robustBufferAccess)
    isVolatile |= spvStore->getDst()->isVolatile();

  // We don't require volatile on address spaces that become non-pointers.
  const auto pointerStorageClass = spvStore->getDst()->getType()->getPointerStorageClass();
  switch (pointerStorageClass) {
  case StorageClassInput:
  case StorageClassOutput:
  case StorageClassPrivate:
  case StorageClassFunction:
    isVolatile = false;
    break;
  default:
    break;
  }

  bool isCoherent = spvStore->getDst()->isCoherent();

  // MakePointerAvailableKHR is valid with OpStore
  if (spvStore->getMemoryAccessMask(false) & MemoryAccessMakePointerAvailableKHRMask) {
    SPIRVWord spvId = spvStore->getMakeAvailableScope(false);
    SPIRVConstant *const spvScope = static_cast<SPIRVConstant *>(m_bm->getValue(spvId));
    const unsigned scope = spvScope->getZExtIntValue();

    const bool isSystemScope = (scope <= ScopeDevice || scope == ScopeQueueFamilyKHR);

    if (isSystemScope)
      isCoherent = true;
  }

  if (spvStore->getMemoryAccessMask(false) & MemoryAccessNonPrivatePointerKHRMask)
    isCoherent = true;

  if (pointerStorageClass == StorageClassTaskPayloadWorkgroupEXT)
    isCoherent = true;

  const bool isNonTemporal = spvStore->SPIRVMemoryAccess::isNonTemporal(false);

  Value *const storePointer =
      transValue(spvStore->getDst(), getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());

  Value *const storeValue =
      transValue(spvStore->getSrc(), getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());

  SPIRVType *const spvStoreType = spvStore->getDst()->getType();
  SPIRVType *pointerElementType = spvStoreType->getPointerElementType();

  Type *storeType = getPointeeType(spvStore->getDst());

  Instruction *baseNode = getLastInsertedValue();
  BasicBlock *currentBlock = getBuilder()->GetInsertBlock();

  addStoreInstRecursively(pointerElementType, storePointer, storeType, storeValue, isVolatile, isCoherent,
                          isNonTemporal);

  // Record all store instructions inserted by addStoreInstRecursively.
  if (m_scratchBoundsChecksEnabled) {
    gatherScratchBoundsChecksMemOps(spvStore, baseNode, currentBlock, LlvmMemOpType::IS_STORE,
                                    currentBlock->getParent());
  }

  // For stores, we don't really have a thing to map to, so we just return nullptr here.
  return nullptr;
}

// =====================================================================================================================
// Handle OpEndPrimitive
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpEndPrimitive>(SPIRVValue *const spvValue) {
  return getBuilder()->CreateEndPrimitive(0);
}

// =====================================================================================================================
// Handle OpEndStreamPrimitive
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpEndStreamPrimitive>(SPIRVValue *const spvValue) {
  unsigned streamId =
      static_cast<SPIRVConstant *>(static_cast<SPIRVInstTemplateBase *>(spvValue)->getOpValue(0))->getZExtIntValue();
  return getBuilder()->CreateEndPrimitive(streamId);
}

// =====================================================================================================================
// Handle OpArrayLength.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpArrayLength>(SPIRVValue *const spvValue) {
  SPIRVArrayLength *const spvArrayLength = static_cast<SPIRVArrayLength *>(spvValue);
  SPIRVValue *const spvStruct = spvArrayLength->getStruct();
  assert(spvStruct->getType()->isTypePointer());

  Value *const structure =
      transValue(spvStruct, getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());
  assert(structure->getType()->isPointerTy());

  const unsigned memberIndex = spvArrayLength->getMemberIndex();
  const unsigned remappedMemberIndex =
      lookupRemappedTypeElements(spvStruct->getType()->getPointerElementType(), memberIndex);

  StructType *const structType = cast<StructType>(getPointeeType(spvStruct));
  const StructLayout *const structLayout = m_m->getDataLayout().getStructLayout(structType);
  const unsigned offset = static_cast<unsigned>(structLayout->getElementOffset(remappedMemberIndex));
  Value *const offsetVal = getBuilder()->getInt32(offset);
  Value *const arrayBytes = getBuilder()->create<BufferLengthOp>(structure, offsetVal);

  Type *const memberType = structType->getStructElementType(remappedMemberIndex)->getArrayElementType();
  const unsigned stride = static_cast<unsigned>(m_m->getDataLayout().getTypeSizeInBits(memberType) / 8);

  return getBuilder()->CreateUDiv(arrayBytes, getBuilder()->getInt32(stride));
}

// =====================================================================================================================
// Handle OpAccessChain.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpAccessChain>(SPIRVValue *const spvValue) {
  SPIRVAccessChainBase *const spvAccessChain = static_cast<SPIRVAccessChainBase *>(spvValue);
  LayoutMode layout = LayoutMode::Native;

  // Special handling for UniformConstant if the ultimate element type is image/sampler/sampledimage.
  if (static_cast<SPIRVTypePointer *>(spvAccessChain->getBase()->getType())->getStorageClass() ==
      StorageClassUniformConstant) {
    SPIRVType *spvUltimateElementType = spvAccessChain->getBase()->getType()->getPointerElementType();
    while (spvUltimateElementType->getOpCode() == OpTypeArray ||
           spvUltimateElementType->getOpCode() == OpTypeRuntimeArray)
      spvUltimateElementType = spvUltimateElementType->getArrayElementType();

    switch (spvUltimateElementType->getOpCode()) {
    case OpTypeImage:
    case OpTypeSampler:
    case OpTypeSampledImage:
      return transOpAccessChainForImage(spvAccessChain);
    default:
      layout = isAccelerationStructureType(spvUltimateElementType) ? LayoutMode::Explicit : LayoutMode::Std430;
      break;
    }
  }

  // Non-image-related handling.
  Value *base = transValue(spvAccessChain->getBase(), getBuilder()->GetInsertBlock()->getParent(),
                           getBuilder()->GetInsertBlock());
  auto srcIndices = transValue(spvAccessChain->getIndices(), getBuilder()->GetInsertBlock()->getParent(),
                               getBuilder()->GetInsertBlock());

  truncConstantIndex(srcIndices, getBuilder()->GetInsertBlock());

  if (!spvAccessChain->hasPtrIndex())
    srcIndices.insert(srcIndices.begin(), getBuilder()->getInt32(0));

  SPIRVType *spvAccessType = spvAccessChain->getBase()->getType();
  const SPIRVStorageClassKind storageClass = spvAccessType->getPointerStorageClass();
  const bool typeMaybeRemapped =
      isStorageClassExplicitlyLaidOut(m_bm, storageClass) || storageClass == StorageClassUniformConstant;

  Type *basePointeeType = getPointeeType(spvAccessChain->getBase(), layout);

  SmallVector<Value *, 8> gepIndices;

  if (spvAccessType->isTypeForwardPointer())
    spvAccessType = static_cast<SPIRVTypeForwardPointer *>(spvAccessType)->getPointer();
  assert(spvAccessType->isTypePointer());
  gepIndices.push_back(srcIndices[0]);
  spvAccessType = spvAccessType->getPointerElementType();

  auto flushGep = [&]() {
    if (gepIndices.size() == 1) {
      if (auto *constant = dyn_cast<ConstantInt>(gepIndices[0])) {
        if (constant->getZExtValue() == 0)
          return; // no-op
      }
    }

    if (spvAccessChain->isInBounds())
      base = getBuilder()->CreateInBoundsGEP(basePointeeType, base, gepIndices);
    else
      base = getBuilder()->CreateGEP(basePointeeType, base, gepIndices);

    gepIndices.clear();
    gepIndices.push_back(getBuilder()->getInt32(0));
  };

  // Run over the indices and map the SPIR-V level indices to LLVM indices, which may be different because the LLVM
  // types may contain manual padding fields to model the power of Vulkan's layout options.
  // Additionally, break up the GEP sequence to handle some special cases like row major matrices.
  for (Value *index : ArrayRef(srcIndices).drop_front()) {
    switch (spvAccessType->getOpCode()) {
    case OpTypeStruct: {
      ConstantInt *constIndex = cast<ConstantInt>(index);
      const uint64_t origMemberIndex = constIndex->getZExtValue();
      Type *castType = nullptr;

      if (typeMaybeRemapped) {
        if (isRemappedTypeElements(spvAccessType)) {
          const uint64_t remappedMemberIndex = lookupRemappedTypeElements(spvAccessType, origMemberIndex);
          constIndex = getBuilder()->getInt32(remappedMemberIndex);
        }

        // If the struct member was actually overlapping another struct member, we need a split here.
        const auto structIndexPair = std::make_pair(spvAccessType, origMemberIndex);

        if (m_overlappingStructTypeWorkaroundMap.count(structIndexPair) > 0)
          castType = m_overlappingStructTypeWorkaroundMap[structIndexPair];
      }

      gepIndices.push_back(constIndex);

      if (castType) {
        flushGep();
        basePointeeType = castType;
        base =
            getBuilder()->CreateBitCast(base, basePointeeType->getPointerTo(base->getType()->getPointerAddressSpace()));
      }

      spvAccessType = spvAccessType->getStructMemberType(origMemberIndex);
      break;
    }
    case OpTypeArray:
    case OpTypeRuntimeArray: {
      gepIndices.push_back(index);

      if (typeMaybeRemapped && isRemappedTypeElements(spvAccessType)) {
        // If we have padding in an array, we inserted a struct to add that
        // padding, and so we need an extra constant 0 index.
        gepIndices.push_back(getBuilder()->getInt32(0));
      }

      spvAccessType = spvAccessType->getArrayElementType();
      break;
    }
    case OpTypeMatrix: {
      // Matrices are represented as an array of columns.
      Type *const matrixType = GetElementPtrInst::getIndexedType(basePointeeType, gepIndices);
      assert(matrixType && matrixType->isArrayTy());

      if (typeMaybeRemapped && isTypeWithPadRowMajorMatrix(matrixType)) {
        // We have a row major matrix, we need to split the access chain here to handle it.
        flushGep();

        auto pair = createLaunderRowMajorMatrix(matrixType, base);
        basePointeeType = pair.first;
        base = pair.second;

        gepIndices.push_back(index);
      } else {
        gepIndices.push_back(index);
        if (matrixType->getArrayElementType()->isStructTy()) {
          // If the type of the column is a struct we had to add padding to align, so need a further index.
          gepIndices.push_back(getBuilder()->getInt32(0));
        }
      }

      spvAccessType = spvAccessType->getMatrixColumnType();
      break;
    }
    case OpTypeVector: {
      gepIndices.push_back(index);
      spvAccessType = spvAccessType->getVectorComponentType();
      break;
    }
    default:
      llvm_unreachable("unhandled type in access chain");
    }
  }

  Type *finalPointeeType = GetElementPtrInst::getIndexedType(basePointeeType, gepIndices);
  flushGep();

  tryAddAccessChainRetType(spvAccessChain, finalPointeeType);
  return base;
}

// =====================================================================================================================
// Handle OpAccessChain for pointer to (array of) image/sampler/sampledimage
//
// @param spvAccessChain : The OpAccessChain
Value *SPIRVToLLVM::transOpAccessChainForImage(SPIRVAccessChainBase *spvAccessChain) {
  SPIRVType *spvElementType = spvAccessChain->getBase()->getType()->getPointerElementType();
  std::vector<SPIRVValue *> spvIndicesVec = spvAccessChain->getIndices();
  ArrayRef<SPIRVValue *> spvIndices = spvIndicesVec;
  Value *base = transImagePointer(spvAccessChain->getBase());

  if (spvIndices.empty())
    return base;

  Value *index = transValue(spvIndices[0], getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());
  spvIndices = spvIndices.slice(1);
  spvElementType = spvElementType->getArrayElementType();

  while (spvElementType->getOpCode() == OpTypeArray) {
    index = getBuilder()->CreateMul(
        index, getBuilder()->getInt32(static_cast<SPIRVTypeArray *>(spvElementType)->getLength()->getZExtIntValue()));
    if (!spvIndices.empty()) {
      index = getBuilder()->CreateAdd(index, transValue(spvIndices[0], getBuilder()->GetInsertBlock()->getParent(),
                                                        getBuilder()->GetInsertBlock()));
      spvIndices = spvIndices.slice(1);
    }
    spvElementType = spvElementType->getArrayElementType();
  }

  Type *elementTy = transType(spvElementType, 0, false, false, LayoutMode::Native);
  return indexDescPtr(elementTy, base, index);
}

// =====================================================================================================================
// Apply an array index to a pointer to array of image/sampler/sampledimage.
// A pointer to sampledimage is in fact a structure containing pointer to image and pointer to sampler.
// A pointer to image when the image is multisampled is in fact a structure containing pointer to image
// and pointer to fmask descriptor.
//
// @param elementTy : Ultimate non-array element type
// @param base : Base pointer to add index to
// @param index : Index value
Value *SPIRVToLLVM::indexDescPtr(Type *elementTy, Value *base, Value *index) {
  auto structTy = dyn_cast<StructType>(elementTy);
  if (structTy && !structTy->getElementType(structTy->getNumElements() - 1)->isIntegerTy()) {
    // The element type is a struct containing two image/sampler elements. The cases where this happens are:
    // 1. A sampledimage is a struct containing image and sampler.
    // 2. An image that is multisampled is a struct containing image and fmask.
    // In both cases, the pointer type is also a struct containing the corresponding two pointer-and-samples.
    // Index them separately.
    assert(structTy->getNumElements() == 2);
    Value *ptr0 = getBuilder()->CreateExtractValue(base, 0);
    Value *ptr1 = getBuilder()->CreateExtractValue(base, 1);
    ptr0 = indexDescPtr(structTy->getElementType(0), ptr0, index);
    ptr1 = indexDescPtr(structTy->getElementType(1), ptr1, index);
    base = getBuilder()->CreateInsertValue(PoisonValue::get(base->getType()), ptr0, 0);
    base = getBuilder()->CreateInsertValue(base, ptr1, 1);
    return base;
  }

  // A sampler pointer is represented by a {pointer,stride,convertingSamplerIdx} struct. If the converting sampler
  // index is non-zero (i.e. it is actually a converting sampler), we also want to modify that index. That can only
  // happen if there are any converting samplers at all.
  if (!m_convertingSamplers.empty() && base->getType()->getStructNumElements() == 3) {
    Value *convertingSamplerIdx = getBuilder()->CreateExtractValue(base, 2);
    Value *modifiedIdx = getBuilder()->CreateAdd(convertingSamplerIdx, index);
    Value *isConvertingSampler = getBuilder()->CreateICmpNE(convertingSamplerIdx, getBuilder()->getInt32(0));
    modifiedIdx = getBuilder()->CreateSelect(isConvertingSampler, modifiedIdx, getBuilder()->getInt32(0));
    base = getBuilder()->CreateInsertValue(base, modifiedIdx, 2);
  }

  // The descriptor "pointer" is in fact a struct containing the pointer and stride.
  Value *ptr = getBuilder()->CreateExtractValue(base, 0);
  Value *stride = getBuilder()->CreateExtractValue(base, 1);
  index = getBuilder()->CreateZExtOrTrunc(index, getBuilder()->getInt32Ty());
  index = getBuilder()->CreateMul(index, stride);

  // Do the indexing operation by GEPping as a byte pointer.
  Type *ptrTy = ptr->getType();
  ptr = getBuilder()->CreateBitCast(ptr,
                                    getBuilder()->getInt8Ty()->getPointerTo(ptr->getType()->getPointerAddressSpace()));
  ptr = getBuilder()->CreateGEP(getBuilder()->getInt8Ty(), ptr, index);
  ptr = getBuilder()->CreateBitCast(ptr, ptrTy);
  base = getBuilder()->CreateInsertValue(base, ptr, 0);

  return base;
}

// =====================================================================================================================
// Handle OpInBoundsAccessChain.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpInBoundsAccessChain>(SPIRVValue *const spvValue) {
  return transValueWithOpcode<OpAccessChain>(spvValue);
}

// =====================================================================================================================
// Handle OpPtrAccessChain.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpPtrAccessChain>(SPIRVValue *const spvValue) {
  return transValueWithOpcode<OpAccessChain>(spvValue);
}

// =====================================================================================================================
// Handle OpInBoundsPtrAccessChain.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpInBoundsPtrAccessChain>(SPIRVValue *const spvValue) {
  return transValueWithOpcode<OpAccessChain>(spvValue);
}

// =====================================================================================================================
// Handle OpImage (extract image from sampledimage)
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpImage>(SPIRVValue *const spvValue) {
  Value *sampledImage = transValue(static_cast<SPIRVInstTemplateBase *>(spvValue)->getOpValue(0),
                                   getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());
  return getBuilder()->CreateExtractValue(sampledImage, uint64_t(0));
}

// =====================================================================================================================
// Handle OpSampledImage (combine image and sampler to create sampledimage)
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpSampledImage>(SPIRVValue *const spvValue) {
  Value *image = transValue(static_cast<SPIRVInstTemplateBase *>(spvValue)->getOpValue(0),
                            getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());
  Value *sampler = transValue(static_cast<SPIRVInstTemplateBase *>(spvValue)->getOpValue(1),
                              getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());

  Value *result = PoisonValue::get(StructType::get(*m_context, {image->getType(), sampler->getType()}));
  result = getBuilder()->CreateInsertValue(result, image, uint64_t(0));
  result = getBuilder()->CreateInsertValue(result, sampler, 1);
  return result;
}

// =====================================================================================================================
// Handle OpKill.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpKill>(SPIRVValue *const spvValue) {
  Value *const kill = getBuilder()->CreateKill();

  // NOTE: In SPIR-V, "OpKill" is considered as a valid instruction to terminate blocks. But in LLVM, we have to
  // insert a dummy "return" instruction as block terminator.
  if (getBuilder()->getCurrentFunctionReturnType()->isVoidTy()) {
    // No return value
    getBuilder()->CreateRetVoid();
  } else {
    // Function returns value
    getBuilder()->CreateRet(PoisonValue::get(getBuilder()->getCurrentFunctionReturnType()));
  }

  return kill;
}

// =====================================================================================================================
// Handle OpDemoteToHelperInvocationEXT.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpDemoteToHelperInvocationEXT>(SPIRVValue *const spvValue) {
  return getBuilder()->CreateDemoteToHelperInvocation();
}

// =====================================================================================================================
// Handle OpIsHelperInvocationEXT.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpIsHelperInvocationEXT>(SPIRVValue *const spvValue) {
  return getBuilder()->CreateIsHelperInvocation();
}

// =====================================================================================================================
// Handle OpReadClockKHR.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<spv::OpReadClockKHR>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  SPIRVConstant *const spvScope = static_cast<SPIRVConstant *>(spvInst->getOperands()[0]);
  const spv::Scope scope = static_cast<spv::Scope>(spvScope->getZExtIntValue());
  assert(scope == spv::ScopeDevice || scope == spv::ScopeSubgroup);

  Value *const readClock = getBuilder()->CreateReadClock(scope == spv::ScopeDevice);

  SPIRVType *const spvType = spvInst->getType();
  if (spvType->isTypeVectorInt(32)) {
    assert(spvType->getVectorComponentCount() == 2);                   // Must be uvec2
    return getBuilder()->CreateBitCast(readClock, transType(spvType)); // uint64 -> uvec2
  }
  assert(spvType->isTypeInt(64));
  return readClock;
}

// =====================================================================================================================
// Handle OpGroupAll.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupAll>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const predicate = transValue(spvOperands[1], func, block);
  return getBuilder()->CreateSubgroupAll(predicate);
}

// =====================================================================================================================
// Handle OpGroupAny.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupAny>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const predicate = transValue(spvOperands[1], func, block);
  return getBuilder()->CreateSubgroupAny(predicate);
}

// =====================================================================================================================
// Handle OpGroupBroadcast.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupBroadcast>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);
  Value *const id = transValue(spvOperands[2], func, block);
  return getBuilder()->CreateSubgroupBroadcast(value, id);
}

// =====================================================================================================================
// Handle OpGroupIAdd.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupIAdd>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::IAdd, spvValue);
}

// =====================================================================================================================
// Handle OpGroupFAdd.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupFAdd>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::FAdd, spvValue);
}

// =====================================================================================================================
// Handle OpGroupSMin.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupSMin>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::SMin, spvValue);
}

// =====================================================================================================================
// Handle OpGroupUMin.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupUMin>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::UMin, spvValue);
}

// =====================================================================================================================
// Handle OpGroupFMin.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupFMin>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::FMin, spvValue);
}

// =====================================================================================================================
// Handle OpGroupSMax.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupSMax>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::SMax, spvValue);
}

// =====================================================================================================================
// Handle OpGroupUMax.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupUMax>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::UMax, spvValue);
}

// =====================================================================================================================
// Handle OpGroupFMax.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupFMax>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::FMax, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformElect.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformElect>(SPIRVValue *const spvValue) {
  Value *result = nullptr;

  result = getBuilder()->CreateSubgroupElect();
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformAll.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformAll>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const predicate = transValue(spvOperands[1], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupAll(predicate);
  }
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformAny.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformAny>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const predicate = transValue(spvOperands[1], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupAny(predicate);
  }
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformAllEqual.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformAllEqual>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupAllEqual(value);
  }
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformBroadcast.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBroadcast>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);
  Value *const index = transValue(spvOperands[2], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupBroadcastWaterfall(value, index);
  }
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformBroadcastFirst.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBroadcastFirst>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupBroadcastFirst(value);
  }
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformBallot.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBallot>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const predicate = transValue(spvOperands[1], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupBallot(predicate);
  }
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformInverseBallot.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformInverseBallot>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupInverseBallot(value);
  }
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformBallotBitExtract.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBallotBitExtract>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);
  Value *const index = transValue(spvOperands[2], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupBallotBitExtract(value, index);
  }
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformBallotBitCount.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBallotBitCount>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[2], func, block);

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    switch (static_cast<SPIRVConstant *>(spvOperands[1])->getZExtIntValue()) {
    case GroupOperationReduce:
      return getBuilder()->CreateSubgroupBallotBitCount(value);
    case GroupOperationInclusiveScan:
      return getBuilder()->CreateSubgroupBallotInclusiveBitCount(value);
    case GroupOperationExclusiveScan:
      return getBuilder()->CreateSubgroupBallotExclusiveBitCount(value);
    default:
      llvm_unreachable("Should never be called!");
      return nullptr;
    }
  }
}

// =====================================================================================================================
// Handle OpGroupNonUniformBallotFindLSB.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBallotFindLSB>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupBallotFindLsb(value);
  }
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformBallotFindMSB.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBallotFindMSB>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupBallotFindMsb(value);
  }
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformShuffle.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformShuffle>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);
  Value *const index = transValue(spvOperands[2], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupShuffle(value, index);
  }
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformShuffleXor.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformShuffleXor>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);
  Value *const mask = transValue(spvOperands[2], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupShuffleXor(value, mask);
  }
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformShuffleUp.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformShuffleUp>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);
  Value *const delta = transValue(spvOperands[2], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupShuffleUp(value, delta);
  }
  return result;
}

// =====================================================================================================================
// Handle OpGroupNonUniformShuffleDown.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformShuffleDown>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);
  Value *const delta = transValue(spvOperands[2], func, block);
  Value *result = nullptr;

  {
    assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);
    result = getBuilder()->CreateSubgroupShuffleDown(value, delta);
  }
  return result;
}

// =====================================================================================================================
// Create TraceRay dialect Op.
//
// @param spvValue : A SPIR-V value.
Value *SPIRV::SPIRVToLLVM::createTraceRayDialectOp(SPIRVValue *const spvValue) {
  if (m_execModule != ExecutionModelRayGenerationKHR) {
    Llpc::Context *llpcContext = static_cast<Llpc::Context *>(m_context);
    auto *pipelineContext = static_cast<Llpc::RayTracingContext *>(llpcContext->getPipelineContext());
    pipelineContext->setIndirectPipeline();
  }

  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const accelStruct = transValue(spvOperands[0], func, block);
  Value *const rayFlags = transValue(spvOperands[1], func, block);
  Value *const cullMask = transValue(spvOperands[2], func, block);
  Value *const sbtOffset = transValue(spvOperands[3], func, block);
  Value *const sbtStride = transValue(spvOperands[4], func, block);
  Value *const missIndex = transValue(spvOperands[5], func, block);
  Value *const rayOrigin = transValue(spvOperands[6], func, block);
  Value *const rayTMin = transValue(spvOperands[7], func, block);
  Value *const rayDir = transValue(spvOperands[8], func, block);
  Value *const rayTMax = transValue(spvOperands[9], func, block);
  Value *const payload = transValue(spvOperands[10], func, block);

  auto accelStructAsI64 = getBuilder()->CreateBitCast(accelStruct, getBuilder()->getInt64Ty());

  Type *payloadTy = transType(spvOperands[10]->getType()->getPointerElementType());
  auto paq = getPaqFromSize(getBuilder()->getContext(), alignTo(m_m->getDataLayout().getTypeAllocSize(payloadTy), 4));

  return getBuilder()->create<TraceRayOp>(accelStructAsI64, rayFlags, cullMask, sbtOffset, sbtStride, missIndex,
                                          rayOrigin, rayTMin, rayDir, rayTMax, payload, paq);
}

// =====================================================================================================================
// Handle OpExecuteCallableKHR.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpExecuteCallableKHR>(SPIRVValue *const spvValue) {
  if (m_execModule == ExecutionModelCallableKHR) {
    Llpc::Context *llpcContext = static_cast<Llpc::Context *>(m_context);
    auto *pipelineContext = static_cast<Llpc::RayTracingContext *>(llpcContext->getPipelineContext());
    pipelineContext->setIndirectPipeline();
  }
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const shaderIndex = transValue(spvOperands[0], func, block);
  Value *const callableData = transValue(spvOperands[1], func, block);
  Type *callableDataTy = transType(spvOperands[1]->getType()->getPointerElementType());
  unsigned dataByteSize = alignTo(m_m->getDataLayout().getTypeAllocSize(callableDataTy), 4);

  return getBuilder()->create<CallCallableShaderOp>(shaderIndex, callableData, dataByteSize);
}

// =====================================================================================================================
// Handle OpConvertUToAccelerationStructureKHR.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpConvertUToAccelerationStructureKHR>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *gpuAddr = transValue(spvInst->getOperands()[0], func, block);
  Type *gpuAddrTy = gpuAddr->getType();
  // NOTE: The input is allowed to be i64 or <2 x i32>, but we always assume acceleration structure is <2 x i32>.
  if (gpuAddrTy->isIntegerTy()) {
    assert(gpuAddrTy->getScalarSizeInBits() == 64);
    gpuAddr = getBuilder()->CreateBitCast(gpuAddr, FixedVectorType::get(getBuilder()->getInt32Ty(), 2));
  }
  return mapValue(spvValue, gpuAddr);
}

// =====================================================================================================================
// Handle OpTerminateRayKHR.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpTerminateRayKHR>(SPIRVValue *const spvValue) {
  getBuilder()->create<AcceptHitAndEndSearchOp>();
  return getBuilder()->CreateUnreachable();
}

// =====================================================================================================================
// Handle OpIgnoreIntersectionKHR.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpIgnoreIntersectionKHR>(SPIRVValue *const spvValue) {
  getBuilder()->create<IgnoreHitOp>();
  return getBuilder()->CreateUnreachable();
}

// =====================================================================================================================
// Handle OpReportIntersectionKHR.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpReportIntersectionKHR>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const hitT = transValue(spvOperands[0], func, block);
  Value *const hitKind = transValue(spvOperands[1], func, block);

  // We don't have attribute for this Op in SPIR-V
  // TODO: Pass the pointer of hit attribute global variable here after SpirvLowerRayTracing is moved after
  // SpirvLowerGlobal
  auto dummyPtr = ConstantPointerNull::get(PointerType::get(getBuilder()->getContext(), SPIRAS_Private));

  return getBuilder()->create<ReportHitOp>(hitT, hitKind, dummyPtr, 0);
}

// =====================================================================================================================
// Handle OpEmitMeshTasksEXT.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpEmitMeshTasksEXT>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const groupCountX = transValue(spvOperands[0], func, block);
  Value *const groupCountY = transValue(spvOperands[1], func, block);
  Value *const groupCountZ = transValue(spvOperands[2], func, block);
  getBuilder()->create<lgc::EmitMeshTasksOp>(groupCountX, groupCountY, groupCountZ);
  // NOTE: According to SPIR-V spec, OpEmitMeshTasksEXT is a terminator. Hence, we have to insert
  // a return instruction to implement this semantics.
  return getBuilder()->CreateRetVoid();
}

// =====================================================================================================================
// Handle OpSetMeshOutputsEXT.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpSetMeshOutputsEXT>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const vertexCount = transValue(spvOperands[0], func, block);
  Value *const primitiveCount = transValue(spvOperands[1], func, block);
  return getBuilder()->create<lgc::SetMeshOutputsOp>(vertexCount, primitiveCount);
}

// =====================================================================================================================
// Handle a group arithmetic operation.
//
// @param groupArithOp : The group operation.
// @param spvValue : A SPIR-V value.
Value *SPIRVToLLVM::transGroupArithOp(Builder::GroupArithOp groupArithOp, SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();

  Value *const value = transValue(spvOperands[2], func, block);

  switch (static_cast<SPIRVConstant *>(spvOperands[1])->getZExtIntValue()) {
  case GroupOperationReduce:
    return getBuilder()->CreateSubgroupClusteredReduction(groupArithOp, value, getBuilder()->CreateGetWaveSize());
  case GroupOperationInclusiveScan:
    return getBuilder()->CreateSubgroupClusteredInclusive(groupArithOp, value, getBuilder()->CreateGetWaveSize());
  case GroupOperationExclusiveScan:
    return getBuilder()->CreateSubgroupClusteredExclusive(groupArithOp, value, getBuilder()->CreateGetWaveSize());
  case GroupOperationClusteredReduce:
    return getBuilder()->CreateSubgroupClusteredReduction(groupArithOp, value, transValue(spvOperands[3], func, block));
  default:
    llvm_unreachable("Should never be called!");
    return nullptr;
  }
}

// =====================================================================================================================
// Handle OpGroupNonUniformIAdd.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformIAdd>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::IAdd, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformFAdd.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformFAdd>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::FAdd, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformIMul.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformIMul>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::IMul, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformFMul.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformFMul>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::FMul, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformSMin.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformSMin>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::SMin, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformUMin.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformUMin>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::UMin, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformFMin.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformFMin>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::FMin, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformSMax.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformSMax>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::SMax, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformUMax.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformUMax>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::UMax, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformFMax.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformFMax>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::FMax, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformBitwiseAnd.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBitwiseAnd>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::And, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformBitwiseOr.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBitwiseOr>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::Or, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformBitwiseXor.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformBitwiseXor>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::Xor, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformLogicalAnd.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformLogicalAnd>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::And, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformLogicalOr.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformLogicalOr>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::Or, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformLogicalXor.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformLogicalXor>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::Xor, spvValue);
}

// =====================================================================================================================
// Handle OpGroupNonUniformQuadBroadcast.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformQuadBroadcast>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);
  Value *const index = transValue(spvOperands[2], func, block);
  return getBuilder()->CreateSubgroupQuadBroadcast(value, index);
}

// =====================================================================================================================
// Handle OpGroupNonUniformQuadSwap.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupNonUniformQuadSwap>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  assert(static_cast<SPIRVConstant *>(spvOperands[0])->getZExtIntValue() == ScopeSubgroup);

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[1], func, block);

  switch (static_cast<SPIRVConstant *>(spvOperands[2])->getZExtIntValue()) {
  case 0:
    return getBuilder()->CreateSubgroupQuadSwapHorizontal(value);
  case 1:
    return getBuilder()->CreateSubgroupQuadSwapVertical(value);
  case 2:
    return getBuilder()->CreateSubgroupQuadSwapDiagonal(value);
  default:
    llvm_unreachable("Should never be called!");
    return nullptr;
  }
}

// =====================================================================================================================
// Handle OpSubgroupBallotKHR.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpSubgroupBallotKHR>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const predicate = transValue(spvOperands[0], func, block);
  return getBuilder()->CreateSubgroupBallot(predicate);
}

// =====================================================================================================================
// Handle OpSubgroupFirstInvocationKHR.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpSubgroupFirstInvocationKHR>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[0], func, block);
  return getBuilder()->CreateSubgroupBroadcastFirst(value);
}

// =====================================================================================================================
// Handle OpSubgroupAllKHR.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpSubgroupAllKHR>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const predicate = transValue(spvOperands[0], func, block);
  return getBuilder()->CreateSubgroupAll(predicate);
}

// =====================================================================================================================
// Handle OpSubgroupAnyKHR.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpSubgroupAnyKHR>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const predicate = transValue(spvOperands[0], func, block);
  return getBuilder()->CreateSubgroupAny(predicate);
}

// =====================================================================================================================
// Handle OpSubgroupAllEqualKHR.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpSubgroupAllEqualKHR>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[0], func, block);
  return getBuilder()->CreateSubgroupAllEqual(value);
}

// =====================================================================================================================
// Handle OpSubgroupReadInvocationKHR.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpSubgroupReadInvocationKHR>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const value = transValue(spvOperands[0], func, block);
  Value *const index = transValue(spvOperands[1], func, block);
  return getBuilder()->CreateSubgroupBroadcast(value, index);
}

// =====================================================================================================================
// Handle OpGroupIAddNonUniformAMD.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupIAddNonUniformAMD>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::IAdd, spvValue);
}

// =====================================================================================================================
// Handle OpGroupFAddNonUniformAMD.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupFAddNonUniformAMD>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::FAdd, spvValue);
}

// =====================================================================================================================
// Handle OpGroupSMinNonUniformAMD.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupSMinNonUniformAMD>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::SMin, spvValue);
}

// =====================================================================================================================
// Handle OpGroupUMinNonUniformAMD.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupUMinNonUniformAMD>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::UMin, spvValue);
}

// =====================================================================================================================
// Handle OpGroupFMinNonUniformAMD.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupFMinNonUniformAMD>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::FMin, spvValue);
}

// =====================================================================================================================
// Handle OpGroupSMaxNonUniformAMD.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupSMaxNonUniformAMD>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::SMax, spvValue);
}

// =====================================================================================================================
// Handle OpGroupUMaxNonUniformAMD.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupUMaxNonUniformAMD>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::UMax, spvValue);
}

// =====================================================================================================================
// Handle OpGroupFMaxNonUniformAMD.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpGroupFMaxNonUniformAMD>(SPIRVValue *const spvValue) {
  return transGroupArithOp(Builder::GroupArithOp::FMax, spvValue);
}

// =====================================================================================================================
// Handle OpExtInst.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpExtInst>(SPIRVValue *const spvValue) {
  SPIRVExtInst *const spvExtInst = static_cast<SPIRVExtInst *>(spvValue);

  // Just ignore this set of extended instructions
  std::vector<SPIRVValue *> spvArgValues = spvExtInst->getArgumentValues();

  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = block->getParent();

  switch (m_bm->getBuiltinSet(spvExtInst->getExtSetId())) {
  case SPIRVEIS_ShaderBallotAMD:
    switch (spvExtInst->getExtOp()) {
    case SwizzleInvocationsAMD:
      return getBuilder()->CreateSubgroupSwizzleQuad(transValue(spvArgValues[0], func, block),
                                                     transValue(spvArgValues[1], func, block));
    case SwizzleInvocationsMaskedAMD:
      return getBuilder()->CreateSubgroupSwizzleMask(transValue(spvArgValues[0], func, block),
                                                     transValue(spvArgValues[1], func, block));
    case WriteInvocationAMD:
      return getBuilder()->CreateSubgroupWriteInvocation(transValue(spvArgValues[0], func, block),
                                                         transValue(spvArgValues[1], func, block),
                                                         transValue(spvArgValues[2], func, block));
    case MbcntAMD:
      return getBuilder()->CreateSubgroupMbcnt(transValue(spvArgValues[0], func, block));
    default:
      llvm_unreachable("Should never be called!");
      return nullptr;
    }
  case SPIRVEIS_GLSL:
    return transGLSLExtInst(spvExtInst, block);

  case SPIRVEIS_ShaderExplicitVertexParameterAMD:
    return transGLSLBuiltinFromExtInst(spvExtInst, block);

  case SPIRVEIS_GcnShaderAMD:
    switch (spvExtInst->getExtOp()) {
    case CubeFaceCoordAMD:
      return getBuilder()->CreateCubeFaceCoord(transValue(spvArgValues[0], func, block));
    case CubeFaceIndexAMD:
      return getBuilder()->CreateCubeFaceIndex(transValue(spvArgValues[0], func, block));
    case TimeAMD:
      return getBuilder()->CreateReadClock(false);
    default:
      llvm_unreachable("Should never be called!");
      return nullptr;
    }

  case SPIRVEIS_ShaderTrinaryMinMaxAMD:
    return transTrinaryMinMaxExtInst(spvExtInst, block);

  case SPIRVEIS_NonSemanticDebugBreak:
    // Only one instruction for this extended instruction set
    assert(spvExtInst->getExtOp() == NonSemanticDebugBreakDebugBreak);
    return getBuilder()->CreateDebugBreak();

  case SPIRVEIS_Debug:
  case SPIRVEIS_NonSemanticShaderDebugInfo100:
    return m_dbgTran.transDebugIntrinsic(spvExtInst, block);
  case SPIRVEIS_NonSemanticDebugPrintf:
    switch (spvExtInst->getExtOp()) {
    case NonSemanticDebugPrintfDebugPrintf:
      return transDebugPrintf(spvExtInst, spvArgValues, func, block);
    default:
      return nullptr;
    }
  case SPIRVEIS_NonSemanticInfo:
    return nullptr;
  default:
    llvm_unreachable("Should never be called!");
    return nullptr;
  }
}

// =====================================================================================================================
// Handle extended instruction Nonsemantics.debugPrintf
//
// @param bi : A SPIR-V debugPrintf instruction
// @param spvValues : Instruction spirv parameters
// @param func : Which function to generate code
// @param bb : Which basicblock to generate code
Value *SPIRVToLLVM::transDebugPrintf(SPIRVInstruction *bi, const ArrayRef<SPIRVValue *> spvValues, Function *func,
                                     BasicBlock *bb) {
  auto pipelineContext = (static_cast<Llpc::Context *>(m_context))->getPipelineContext();
  auto resMapping = pipelineContext->getResourceMapping();
  unsigned nodeIndex = 0;
  if (findResourceNode(resMapping->pUserDataNodes, resMapping->userDataNodeCount, Vkgc::InternalDescriptorSetId,
                       Vkgc::PrintfBufferBindingId, &nodeIndex) == nullptr)
    return getBuilder()->getInt64(0);

  if (!m_debugOutputBuffer) {
    auto spvArrType = m_bm->addRuntimeArray(m_bm->addIntegerType(32));
    auto spvStructType = m_bm->addStructType({spvArrType});
    Type *bufType = transType(spvStructType);

    m_debugOutputBuffer =
        new GlobalVariable(*m_m, bufType, false, GlobalValue::ExternalLinkage, nullptr, "debugOutputBuffer", nullptr,
                           GlobalVariable::NotThreadLocal, SPIRAS_Uniform);

    // Setup (desc,binding) resource metadata
    auto intType = getBuilder()->getInt32Ty();
    SmallVector<Metadata *, 4> resourceMetas = {
        ConstantAsMetadata::get(ConstantInt::get(intType, Vkgc::InternalDescriptorSetId)),
        ConstantAsMetadata::get(ConstantInt::get(intType, Vkgc::PrintfBufferBindingId)),
        ConstantAsMetadata::get(ConstantInt::get(intType, 0))};

    auto resMdNode = MDNode::get(*m_context, resourceMetas);
    m_debugOutputBuffer->addMetadata(gSPIRVMD::Resource, *resMdNode);
  }

  auto spvValueItr = spvValues.begin();
  Value *formatStr = mapEntry(*spvValueItr++, nullptr);
  SmallVector<Value *> args;
  for (; spvValueItr != spvValues.end(); ++spvValueItr) {
    args.push_back(transValue(*spvValueItr, func, bb));
  }
  return getBuilder()->create<lgc::DebugPrintfOp>(m_debugOutputBuffer, formatStr, args);
}

// Translate an initializer. This has special handling for the case where the type to initialize to does not match the
// type of the initializer, which is common when dealing with interface objects.
//
// @param spvValue : The SPIR-V value that is an initializer.
// @param type : The LLVM type of the initializer.
Constant *SPIRVToLLVM::transInitializer(SPIRVValue *const spvValue, Type *const type) {
  SPIRVType *const spvType = spvValue->getType();

  if (spvValue->getOpCode() == OpConstantNull && type->isAggregateType())
    return ConstantAggregateZero::get(type);

  if (spvType->isTypeStruct()) {
    SPIRVConstantComposite *const spvConstStruct = static_cast<SPIRVConstantComposite *>(spvValue);

    std::vector<SPIRVValue *> spvMembers(spvConstStruct->getElements());
    assert(spvMembers.size() == spvType->getStructMemberCount());

    // For structs we lookup the mapping of the elements and use it to reverse map the values.
    const bool needsPad = isRemappedTypeElements(spvType);

    assert(needsPad == false || isRemappedTypeElements(spvType));

    Constant *structInitializer = PoisonValue::get(type);

    for (unsigned i = 0, memberCount = spvMembers.size(); i < memberCount; i++) {
      const unsigned memberIndex = needsPad ? lookupRemappedTypeElements(spvType, i) : i;

      Constant *const initializer = transInitializer(spvMembers[i], type->getStructElementType(memberIndex));

      structInitializer = llvm::ConstantFoldInsertValueInstruction(structInitializer, initializer, memberIndex);
      assert(structInitializer && "unexpected error creating aggregate initializer, malformed aggregate?");
    }

    return structInitializer;
  }
  if (type->isArrayTy()) {
    SPIRVConstantComposite *const spvConstArray = static_cast<SPIRVConstantComposite *>(spvValue);

    std::vector<SPIRVValue *> spvElements(spvConstArray->getElements());
    assert(spvElements.size() == type->getArrayNumElements());

    // Matrix and arrays both get here. For both we need to turn [<{element-type, pad}>] into [element-type].
    const bool needsPad = isTypeWithPad(type);

    Constant *arrayInitializer = PoisonValue::get(type);

    for (unsigned i = 0, elementCount = spvElements.size(); i < elementCount; i++) {
      if (needsPad) {
        Type *const elementType = type->getArrayElementType()->getStructElementType(0);
        Constant *const initializer = transInitializer(spvElements[i], elementType);
        arrayInitializer = llvm::ConstantFoldInsertValueInstruction(arrayInitializer, initializer, {i, 0});
        assert(arrayInitializer && "unexpected error creating aggregate initializer, malformed aggregate?");
      } else {
        Type *const elementType = type->getArrayElementType();
        Constant *const initializer = transInitializer(spvElements[i], elementType);
        arrayInitializer = llvm::ConstantFoldInsertValueInstruction(arrayInitializer, initializer, i);
        assert(arrayInitializer && "unexpected error creating aggregate initializer, malformed aggregate?");
      }
    }

    return arrayInitializer;
  }
  Constant *initializer = cast<Constant>(transValue(spvValue, nullptr, nullptr, false));
  if (initializer->getType() != type) {
    // The translated value type is different to the requested type. This can only happen in the
    // case that the SPIR-V value was (vector of) bool but the requested type was (vector of) i32 because it is a bool
    // in memory.
    assert(initializer->getType()->isIntOrIntVectorTy(1));
    assert(type->isIntOrIntVectorTy(32));
    initializer = ConstantExpr::getZExt(initializer, type);
  }
  return initializer;
}

// =====================================================================================================================
// Handle OpVariable.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpVariable>(SPIRVValue *const spvValue) {
  SPIRVVariable *const spvVar = static_cast<SPIRVVariable *>(spvValue);
  const SPIRVStorageClassKind storageClass = spvVar->getStorageClass();
  SPIRVType *const spvVarType = spvVar->getType()->getPointerElementType();

  LayoutMode layout = isStorageClassExplicitlyLaidOut(m_bm, spvVar->getType()->getPointerStorageClass())
                          ? LayoutMode::Explicit
                          : LayoutMode::Native;
  if (storageClass == StorageClassUniformConstant) {
    SPIRVType *spvElementType = spvVarType;
    while (spvElementType->getOpCode() == OpTypeArray || spvElementType->getOpCode() == OpTypeRuntimeArray)
      spvElementType = spvElementType->getArrayElementType();
    switch (spvElementType->getOpCode()) {
    case OpTypeImage:
    case OpTypeSampler:
    case OpTypeSampledImage:
      // Do nothing for image/sampler/sampledimage.
      return nullptr;
    default:
      if (!isAccelerationStructureType(spvElementType))
        layout = LayoutMode::Std430;
      break;
    }
  }

  Type *const ptrType = transType(spvVar->getType());
  unsigned addrSpace = ptrType->getPointerAddressSpace();

  Type *const varType = transType(spvVar->getType()->getPointerElementType(), 0, true, true, layout);

  SPIRVValue *const spvInitializer = spvVar->getInitializer();

  Constant *initializer = nullptr;

  // If the type has an initializer, re-create the SPIR-V initializer in LLVM.
  if (spvInitializer) {
    initializer = transInitializer(spvInitializer, varType);
  } else if (storageClass == SPIRVStorageClassKind::StorageClassWorkgroup) {
    initializer = PoisonValue::get(varType);
  } else if (m_shaderOptions->workaroundInitializeOutputsToZero &&
             storageClass == SPIRVStorageClassKind::StorageClassOutput) {
    bool isBuiltIn = false;
    if (spvVarType->hasDecorate(DecorationBuiltIn)) {
      isBuiltIn = true;
    } else if (spvVarType->isTypeStruct()) {
      for (unsigned memberId = 0; memberId < spvVarType->getStructMemberCount(); ++memberId) {
        if (spvVarType->hasMemberDecorate(memberId, DecorationBuiltIn)) {
          isBuiltIn = true;
          break;
        }
      }
    }
    if (!isBuiltIn) {
      // Initializize user-defined output variable to zero
      initializer = Constant::getNullValue(varType);
    }
  }

  bool readOnly = false;

  switch (storageClass) {
  case StorageClassPushConstant: {
    readOnly = true;
    break;
  }
  case StorageClassStorageBuffer:
  case StorageClassUniform: {
    SPIRVType *spvBlockDecoratedType = spvVarType;

    // Skip through arrays of descriptors to get to the descriptor block type.
    while (spvBlockDecoratedType->isTypeArray())
      spvBlockDecoratedType = spvBlockDecoratedType->getArrayElementType();

    assert(spvBlockDecoratedType->isTypeStruct());

    readOnly = spvBlockDecoratedType->hasDecorate(DecorationBlock) &&
               storageClass != SPIRVStorageClassKind::StorageClassStorageBuffer;
    break;
  }
  case StorageClassUniformConstant: {
    if (spvVarType->isTypeAccelerationStructureKHR()) {
      readOnly = true;
    }

    if (spvVarType->isTypeArray() && spvVarType->getArrayElementType()->isTypeAccelerationStructureKHR()) {
      readOnly = true;
    }

    break;
  }

  default: {
    break;
  }
  }

  if (spvVar->hasDecorate(DecorationNonWritable))
    readOnly = true;
  else if (spvVarType->isTypeStruct()) {
    // glslang has a bug where it'll output NonWritable on struct member types instead of the memory object
    // declarations it was meant to. Workaround this by checking that if all the struct members are non-writable,
    // make the global variable constant.
    bool allReadOnly = true;
    for (unsigned i = 0; i < spvVarType->getStructMemberCount(); i++) {
      if (!spvVarType->hasMemberDecorate(i, DecorationNonWritable)) {
        allReadOnly = false;
        break;
      }
    }

    if (allReadOnly)
      readOnly = true;
  }

  if (!readOnly && storageClass == StorageClassFunction) {
    assert(getBuilder()->GetInsertBlock());

    // Entry block allocas should appear at the start of the basic block so that if the entry block is split by
    // function inlining or other transforms later on, the allocas stay in the entry block.
    //
    // Note that as of this writing, SPIR-V doesn't have the equivalent of C's alloca() builtin, so all allocas
    // should be entry block allocas.
    auto insertPoint = getBuilder()->saveIP();
    BasicBlock *bb = getBuilder()->GetInsertBlock();
    assert(bb->isEntryBlock());
    getBuilder()->SetInsertPoint(bb, bb->getFirstInsertionPt());

    Value *const var = getBuilder()->CreateAlloca(varType, nullptr, spvVar->getName());

    getBuilder()->restoreIP(insertPoint);

    if (initializer)
      getBuilder()->CreateStore(initializer, var);

    return var;
  }

  string varName = spvVar->getName();
  if (storageClass == StorageClassCallableDataKHR || storageClass == StorageClassIncomingCallableDataKHR ||
      storageClass == StorageClassRayPayloadKHR || storageClass == StorageClassHitAttributeKHR ||
      storageClass == StorageClassIncomingRayPayloadKHR || storageClass == StorageClassShaderRecordBufferKHR) {
    unsigned loc = 0;
    spvVar->hasDecorate(DecorationLocation, 0, &loc);
    varName = SPIRVStorageClassNameMap::map(storageClass) + std::to_string(loc);
    Llpc::Context *llpcContext = static_cast<Llpc::Context *>(m_context);

    if (storageClass == StorageClassRayPayloadKHR || storageClass == StorageClassIncomingRayPayloadKHR) {
      llpcContext->getPipelineContext()->collectPayloadSize(varType, m_m->getDataLayout());
    }

    if (storageClass == StorageClassCallableDataKHR || storageClass == StorageClassIncomingCallableDataKHR) {
      llpcContext->getPipelineContext()->collectCallableDataSize(varType, m_m->getDataLayout());
    }

    if (storageClass == StorageClassHitAttributeKHR)
      llpcContext->getPipelineContext()->collectAttributeDataSize(varType, m_m->getDataLayout());
  }

  GlobalVariable *const globalVar =
      new GlobalVariable(*m_m, varType, readOnly, GlobalValue::ExternalLinkage, initializer, varName, nullptr,
                         GlobalVariable::NotThreadLocal, addrSpace);

  if (addrSpace == SPIRAS_Local) {
    globalVar->setAlignment(MaybeAlign(4));

    // NOTE: Give shared variable a name to skip "global optimize pass".
    // The pass will change constant store operations to initializerand this
    // is disallowed in backend compiler.
    if (!globalVar->hasName())
      globalVar->setName("lds");
  }

  if (storageClass == StorageClassCallableDataKHR || storageClass == StorageClassIncomingCallableDataKHR ||
      storageClass == StorageClassRayPayloadKHR || storageClass == StorageClassHitAttributeKHR ||
      storageClass == StorageClassIncomingRayPayloadKHR)
    globalVar->setAlignment(MaybeAlign(4));

  SPIRVBuiltinVariableKind builtinKind;
  if (spvVar->isBuiltin(&builtinKind))
    m_builtinGvMap[globalVar] = builtinKind;

  return globalVar;
}

// =====================================================================================================================
// Handle OpTranspose.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpTranspose>(SPIRVValue *const spvValue) {
  SPIRVInstTemplateBase *const spvTranspose = static_cast<SPIRVInstTemplateBase *>(spvValue);

  Value *const matrix = transValue(spvTranspose->getOpValue(0), getBuilder()->GetInsertBlock()->getParent(),
                                   getBuilder()->GetInsertBlock());
  return getBuilder()->CreateTransposeMatrix(matrix);
}

// =====================================================================================================================
// Handle OpMatrixTimesScalar.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpMatrixTimesScalar>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const matrix = transValue(spvOperands[0], func, block);
  Value *const scalar = transValue(spvOperands[1], func, block);
  { return getBuilder()->CreateMatrixTimesScalar(matrix, scalar); }
}

// =====================================================================================================================
// Handle OpVectorTimesMatrix.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpVectorTimesMatrix>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const vector = transValue(spvOperands[0], func, block);
  Value *const matrix = transValue(spvOperands[1], func, block);
  return getBuilder()->CreateVectorTimesMatrix(vector, matrix);
}

// =====================================================================================================================
// Handle OpMatrixTimesVector.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpMatrixTimesVector>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const matrix = transValue(spvOperands[0], func, block);
  Value *const vector = transValue(spvOperands[1], func, block);
  return getBuilder()->CreateMatrixTimesVector(matrix, vector);
}

// =====================================================================================================================
// Handle OpMatrixTimesMatrix.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpMatrixTimesMatrix>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const matrix1 = transValue(spvOperands[0], func, block);
  Value *const matrix2 = transValue(spvOperands[1], func, block);
  return getBuilder()->CreateMatrixTimesMatrix(matrix1, matrix2);
}

// =====================================================================================================================
// Handle OpOuterProduct.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpOuterProduct>(SPIRVValue *const spvValue) {
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const vector1 = transValue(spvOperands[0], func, block);
  Value *const vector2 = transValue(spvOperands[1], func, block);
  return getBuilder()->CreateOuterProduct(vector1, vector2);
}

// =====================================================================================================================
// Handle OpDot.
//
// @param spvValue : A SPIR-V value.
template <> Value *SPIRVToLLVM::transValueWithOpcode<OpDot>(SPIRVValue *const spvValue) {
  if (m_shaderOptions->noContractOpDot) {
    auto fmf = getBuilder()->getFastMathFlags();
    fmf.setAllowContract(false);
    getBuilder()->setFastMathFlags(fmf);
  }
  SPIRVInstruction *const spvInst = static_cast<SPIRVInstruction *>(spvValue);
  std::vector<SPIRVValue *> spvOperands = spvInst->getOperands();
  BasicBlock *const block = getBuilder()->GetInsertBlock();
  Function *const func = getBuilder()->GetInsertBlock()->getParent();
  Value *const vector1 = transValue(spvOperands[0], func, block);
  Value *const vector2 = transValue(spvOperands[1], func, block);
  return getBuilder()->CreateDotProduct(vector1, vector2);
}

// =====================================================================================================================
// Translate OpString
// @param spvValues : Instruction spirv parameters
Value *SPIRVToLLVM::transString(const SPIRVString *spvValue) {
  auto &debugStr = spvValue->getStr();
  auto globalTy = ArrayType::get(Type::getInt8Ty(*m_context), debugStr.size());
  auto initializer = ConstantDataArray::getString(*m_context, debugStr, false);
  auto global = new GlobalVariable(*m_m, globalTy, true, GlobalValue::InternalLinkage, initializer, "str", nullptr,
                                   GlobalValue::NotThreadLocal, SPIRAS_Constant);
  return mapEntry(spvValue, global);
}

/// For instructions, this function assumes they are created in order
/// and appended to the given basic block. An instruction may use a
/// instruction from another BB which has not been translated. Such
/// instructions should be translated to place holders at the point
/// of first use, then replaced by real instructions when they are
/// created.
///
/// When CreatePlaceHolder is true, create a load instruction of a
/// global variable as placeholder for SPIRV instruction. Otherwise,
/// create instruction and replace placeholder if there is one.
Value *SPIRVToLLVM::transValueWithoutDecoration(SPIRVValue *bv, Function *f, BasicBlock *bb, bool createPlaceHolder) {

  auto oc = bv->getOpCode();
  IntBoolOpMap::rfind(oc, &oc);

  // Translation of non-instruction values
  switch (oc) {
  case OpConstant:
  case OpSpecConstant: {
    SPIRVConstant *bConst = static_cast<SPIRVConstant *>(bv);
    SPIRVType *bt = bv->getType();
    Type *lt = transType(bt);
    switch (bt->getOpCode()) {
    case OpTypeBool:
    case OpTypeInt:
      return mapValue(bv, ConstantInt::get(lt, bConst->getZExtIntValue(), static_cast<SPIRVTypeInt *>(bt)->isSigned()));
    case OpTypeFloat: {
      const llvm::fltSemantics *fs = nullptr;
      switch (bt->getFloatBitWidth()) {
      case 16:
        fs = &APFloat::IEEEhalf();
        break;
      case 32:
        fs = &APFloat::IEEEsingle();
        break;
      case 64:
        fs = &APFloat::IEEEdouble();
        break;
      default:
        llvm_unreachable("invalid float type");
      }
      return mapValue(
          bv, ConstantFP::get(*m_context, APFloat(*fs, APInt(bt->getFloatBitWidth(), bConst->getZExtIntValue()))));
    }
    default:
      llvm_unreachable("Not implemented");
      return nullptr;
    }
  }

  case OpConstantTrue:
  case OpConstantFalse:
  case OpSpecConstantTrue:
  case OpSpecConstantFalse: {
    bool boolVal = oc == OpConstantTrue || oc == OpSpecConstantTrue
                       ? static_cast<SPIRVConstantTrue *>(bv)->getBoolValue()
                       : static_cast<SPIRVConstantFalse *>(bv)->getBoolValue();
    return boolVal ? mapValue(bv, ConstantInt::getTrue(*m_context)) : mapValue(bv, ConstantInt::getFalse(*m_context));
  }

  case OpConstantNull: {
    auto bTy = bv->getType();
    auto nullPtrTy = transType(bTy);
    Value *nullPtr = nullptr;
    // For local memory space (LDS) the NULL value is 0xFFFFFFFF, not 0x0.
    if (bTy->isTypePointer() && bTy->getPointerStorageClass() == spv::StorageClassWorkgroup) {
      auto nullPtrAsInt = getBuilder()->getInt32(0xFFFFFFFF);
      nullPtr = getBuilder()->CreateIntToPtr(nullPtrAsInt, nullPtrTy);
    } else
      nullPtr = Constant::getNullValue(nullPtrTy);
    return mapValue(bv, nullPtr);
  }

  case OpConstantComposite:
  case OpSpecConstantComposite: {
    auto bcc = static_cast<SPIRVConstantComposite *>(bv);
    std::vector<Constant *> cv;
    for (auto &i : bcc->getElements())
      cv.push_back(dyn_cast<Constant>(transValue(i, f, bb)));
    switch (bv->getType()->getOpCode()) {
    case OpTypeVector:
      return mapValue(bv, ConstantVector::get(cv));
    case OpTypeArray:
      return mapValue(bv, ConstantArray::get(dyn_cast<ArrayType>(transType(bcc->getType())), cv));
    case OpTypeStruct: {
      auto bccTy = dyn_cast<StructType>(transType(bcc->getType()));
      auto members = bccTy->getNumElements();
      auto constants = cv.size();
      // if we try to initialize constant TypeStruct, add bitcasts
      // if src and dst types are both pointers but to different types
      if (members == constants) {
        for (unsigned i = 0; i < members; ++i) {
          if (cv[i]->getType() == bccTy->getElementType(i))
            continue;
          if (!cv[i]->getType()->isPointerTy() || !bccTy->getElementType(i)->isPointerTy())
            continue;

          cv[i] = ConstantExpr::getBitCast(cv[i], bccTy->getElementType(i));
        }
      }

      return mapValue(bv, ConstantStruct::get(dyn_cast<StructType>(transType(bcc->getType())), cv));
    }
    case OpTypeMatrix: {
      return mapValue(bv, ConstantArray::get(dyn_cast<ArrayType>(transType(bcc->getType())), cv));
    }
    default:
      llvm_unreachable("not implemented");
      return nullptr;
    }
  }

  case OpSpecConstantOp: {
    auto bi = static_cast<SPIRVSpecConstantOp *>(bv)->getMappedConstant();
    return mapValue(bv, transValue(bi, nullptr, nullptr, false));
  }

  case OpUndef:
    // TODO: Stop using UndefValue since it is deprecated in favor of PoisonValue. A more faithful translation of
    // OpUndef semantics would be to insert "freeze poison" before each _use_ of it in a function, but what about uses
    // that are not in any function?
    return mapValue(bv, UndefValue::get(transType(bv->getType())));

  case OpFunctionParameter: {
    auto ba = static_cast<SPIRVFunctionParameter *>(bv);
    assert(f && "Invalid function");
    unsigned argNo = 0;
    for (Function::arg_iterator i = f->arg_begin(), e = f->arg_end(); i != e; ++i, ++argNo) {
      if (argNo == ba->getArgNo())
        return mapValue(bv, &(*i));
    }
    llvm_unreachable("Invalid argument");
    return nullptr;
  }

  case OpFunction:
    return mapValue(bv, transFunction(static_cast<SPIRVFunction *>(bv)));

  case OpLabel: {
    std::string name = bv->getName();
    if (name.empty() && SPIRVInstNamer)
      name = (Twine("spv") + Twine(bv->getId())).str();
    return mapValue(bv, BasicBlock::Create(*m_context, name, f));
  }

  case (OpVariable):
    if (bb) {
      getBuilder()->SetInsertPoint(bb);
      updateDebugLoc(bv, f);
    }
    return mapValue(bv, transValueWithOpcode<OpVariable>(bv));

  default:
    // do nothing
    break;
  }

  // During translation of OpSpecConstantOp we create an instruction
  // corresponding to the Opcode operand and then translate this instruction.
  // For such instruction BB and F should be nullptr, because it is a constant
  // expression declared out of scope of any basic block or function.
  // All other values require valid BB pointer.
  assert(((isSpecConstantOpAllowedOp(oc) && !f && !bb) || bb) && "Invalid BB");

  // Creation of place holder
  if (createPlaceHolder) {
    auto gvType = transType(bv->getType());
    auto gv = new GlobalVariable(*m_m, gvType, false, GlobalValue::PrivateLinkage, nullptr,
                                 std::string(KPlaceholderPrefix) + bv->getName(), 0, GlobalVariable::NotThreadLocal, 0);
    auto ld = new LoadInst(gvType, gv, bv->getName(), bb);
    m_placeholderMap[bv] = ld;
    return mapValue(bv, ld);
  }

  // Translation of instructions
  if (bb) {
    getBuilder()->SetInsertPoint(bb);
    updateDebugLoc(bv, f);
    setFastMathFlags(bv);
  }

  switch (static_cast<unsigned>(bv->getOpCode())) {
  case OpBranch: {
    auto br = static_cast<SPIRVBranch *>(bv);
    auto successor = cast<BasicBlock>(transValue(br->getTargetLabel(), f, bb));
    auto bi = BranchInst::Create(successor, bb);
    auto lm = static_cast<SPIRVLoopMerge *>(br->getPrevious());
    if (lm && lm->getOpCode() == OpLoopMerge)
      setLLVMLoopMetadata(lm, bi);
    else if (br->getBasicBlock()->getLoopMerge())
      setLLVMLoopMetadata(br->getBasicBlock()->getLoopMerge(), bi);

    recordBlockPredecessor(successor, bb);
    return mapValue(bv, bi);
  }

  case OpBranchConditional: {
    auto br = static_cast<SPIRVBranchConditional *>(bv);
    auto c = transValue(br->getCondition(), f, bb);

    // Workaround a bug where old shader compilers would sometimes specify
    // int/float arguments as the branch condition
    if (SPIRVWorkaroundBadSPIRV) {
      if (c->getType()->isFloatTy())
        c = new llvm::FCmpInst(*bb, llvm::CmpInst::FCMP_ONE, c, llvm::ConstantFP::get(c->getType(), 0.0));
      else if (c->getType()->isIntegerTy() && !c->getType()->isIntegerTy(1))
        c = new llvm::ICmpInst(*bb, llvm::CmpInst::ICMP_NE, c, llvm::ConstantInt::get(c->getType(), 0));
    }

    auto trueSuccessor = cast<BasicBlock>(transValue(br->getTrueLabel(), f, bb));
    auto falseSuccessor = cast<BasicBlock>(transValue(br->getFalseLabel(), f, bb));

    // OpBranchConditional can branch with OpUndef as condition. The selected jump target is undefined.
    // In LLVM IR, BranchInst with undef value is fully UB.
    // LLVM will mark the successors as unreachable, so later SimplifyCFG passes will cause
    // unwanted removal of branches. By using a freeze instruction as condition, we make sure that LLVM
    // will always operate on a fixed value.
    c = new FreezeInst(c, "cond.freeze", bb);

    auto bc = BranchInst::Create(trueSuccessor, falseSuccessor, c, bb);
    auto lm = static_cast<SPIRVLoopMerge *>(br->getPrevious());
    if (lm && lm->getOpCode() == OpLoopMerge)
      setLLVMLoopMetadata(lm, bc);
    else if (br->getBasicBlock()->getLoopMerge())
      setLLVMLoopMetadata(br->getBasicBlock()->getLoopMerge(), bc);

    recordBlockPredecessor(trueSuccessor, bb);
    recordBlockPredecessor(falseSuccessor, bb);
    return mapValue(bv, bc);
  }

  case OpPhi: {
    auto phi = static_cast<SPIRVPhi *>(bv);
    PHINode *phiNode = nullptr;
    if (bb->getFirstInsertionPt() != bb->end())
      phiNode = PHINode::Create(transType(phi->getType()), phi->getPairs().size() / 2, phi->getName(),
                                &*bb->getFirstInsertionPt());
    else
      phiNode = PHINode::Create(transType(phi->getType()), phi->getPairs().size() / 2, phi->getName(), bb);

    auto lPhi = dyn_cast<PHINode>(mapValue(bv, phiNode));

#ifndef NDEBUG
    SmallDenseSet<BasicBlock *, 4> seenPredecessors;
#endif
    phi->foreachPair([&](SPIRVValue *incomingV, SPIRVBasicBlock *incomingBb, size_t index) {
      auto translatedVal = transValue(incomingV, f, bb);
      auto translatedBb = cast<BasicBlock>(transValue(incomingBb, f, bb));

      lPhi->addIncoming(translatedVal, translatedBb);

#ifndef NDEBUG
      assert(seenPredecessors.count(translatedBb) == 0 &&
             "SPIR-V requires phi entries to be unique for duplicate predecessor blocks.");
      seenPredecessors.insert(translatedBb);
#endif
    });

    return lPhi;
  }

  case OpUnreachable:
    return mapValue(bv, new UnreachableInst(*m_context, bb));

  case OpReturn:
    return mapValue(bv, ReturnInst::Create(*m_context, bb));

  case OpReturnValue: {
    auto rv = static_cast<SPIRVReturnValue *>(bv);
    return mapValue(bv, ReturnInst::Create(*m_context, transValue(rv->getReturnValue(), f, bb), bb));
  }

  case OpSelect: {
    SPIRVSelect *bs = static_cast<SPIRVSelect *>(bv);
    return mapValue(bv, SelectInst::Create(transValue(bs->getCondition(), f, bb), transValue(bs->getTrueValue(), f, bb),
                                           transValue(bs->getFalseValue(), f, bb), bv->getName(), bb));
  }

  case OpLine:
  case OpSelectionMerge:
    return nullptr;
  case OpLoopMerge: { // Should be translated at OpBranch or OpBranchConditional cases
    SPIRVLoopMerge *lm = static_cast<SPIRVLoopMerge *>(bv);
    auto continueLabel = m_bm->get<SPIRVBasicBlock>(lm->getContinueTarget());
    auto mergeLabel = m_bm->get<SPIRVBasicBlock>(lm->getMergeBlock());
    auto continueBlock = cast<BasicBlock>(transValue(continueLabel, f, bb));
    auto mergeBlock = cast<BasicBlock>(transValue(mergeLabel, f, bb));

    continueLabel->setLoopMerge(lm);

    // Annotate the loop structure with pseudo intrinsic calls
    auto builder = getBuilder();
    IRBuilder<>::InsertPointGuard guard(*builder);

    auto loopMerge = mapValue(lm, builder->CreateNamedCall("spirv.loop.merge", builder->getInt32Ty(), {},
                                                           {Attribute::ReadNone, Attribute::Convergent}));
    builder->SetInsertPoint(continueBlock);
    builder->CreateNamedCall("spirv.loop.continue.block", builder->getVoidTy(), {loopMerge},
                             {Attribute::ReadNone, Attribute::Convergent});
    builder->SetInsertPoint(mergeBlock);
    builder->CreateNamedCall("spirv.loop.merge.block", builder->getVoidTy(), {loopMerge},
                             {Attribute::ReadNone, Attribute::Convergent});

    return loopMerge;
  }
  case OpSwitch: {
    auto bs = static_cast<SPIRVSwitch *>(bv);
    auto select = transValue(bs->getSelect(), f, bb);
    auto defaultSuccessor = dyn_cast<BasicBlock>(transValue(bs->getDefault(), f, bb));
    recordBlockPredecessor(defaultSuccessor, bb);

    // OpSwitch can branch with OpUndef as condition. The selected jump target is undefined.
    // In LLVM IR, SwitchInst with undef value is fully UB.
    // LLVM will mark the successors as unreachable, so later SimplifyCFG passes will cause
    // unwanted removal of branches. By using a freeze instruction as condition, we make sure that LLVM
    // will always operate on a fixed value.
    select = new FreezeInst(select, "cond.freeze", bb);

    auto ls = SwitchInst::Create(select, defaultSuccessor, bs->getNumPairs(), bb);
    bs->foreachPair([&](SPIRVSwitch::LiteralTy literals, SPIRVBasicBlock *label) {
      assert(!literals.empty() && "Literals should not be empty");
      assert(literals.size() <= 2 && "Number of literals should not be more then two");
      uint64_t literal = uint64_t(literals.at(0));
      if (literals.size() == 2)
        literal += uint64_t(literals.at(1)) << 32;

      auto successor = cast<BasicBlock>(transValue(label, f, bb));
      ls->addCase(ConstantInt::get(dyn_cast<IntegerType>(select->getType()), literal), successor);
      recordBlockPredecessor(successor, bb);
    });
    return mapValue(bv, ls);
  }

  case OpVectorTimesScalar: {
    auto vts = static_cast<SPIRVVectorTimesScalar *>(bv);
    auto scalar = transValue(vts->getScalar(), f, bb);
    auto vector = transValue(vts->getVector(), f, bb);
    assert(vector->getType()->isVectorTy() && "Invalid type");
    unsigned vecSize = cast<FixedVectorType>(vector->getType())->getNumElements();
    auto newVec = getBuilder()->CreateVectorSplat(vecSize, scalar, scalar->getName());
    newVec->takeName(scalar);
    auto scale = getBuilder()->CreateFMul(vector, newVec, "scale");
    return mapValue(bv, scale);
  }

  case OpCopyObject: {
    SPIRVCopyBase *copy = static_cast<SPIRVCopyBase *>(bv);
    Value *v = transValue(copy->getOperand(), f, bb);
    assert(v);
    return mapValue(bv, v);
  }

  case OpCopyLogical: {
    SPIRVCopyBase *copy = static_cast<SPIRVCopyBase *>(bv);
    AllocaInst *ai = nullptr;
    auto at = transType(copy->getOperand()->getType());
    // NOTE: Alloc instructions not in the entry block will prevent LLVM from doing function
    // inlining. Try to move those alloc instructions to the entry block.
    auto firstInst = bb->getParent()->getEntryBlock().getFirstInsertionPt();
    if (firstInst != bb->getParent()->getEntryBlock().end())
      ai = new AllocaInst(at, m_m->getDataLayout().getAllocaAddrSpace(), "", &*firstInst);
    else
      ai = new AllocaInst(at, m_m->getDataLayout().getAllocaAddrSpace(), "", bb);

    new StoreInst(transValue(copy->getOperand(), f, bb), ai, bb);
    LoadInst *li = new LoadInst(at, ai, "", bb);
    return mapValue(bv, li);
  }

  case OpCompositeConstruct: {
    auto cc = static_cast<SPIRVCompositeConstruct *>(bv);
    auto constituents = transValue(cc->getConstituents(), f, bb);
    std::vector<Constant *> cv;
    cv.reserve(constituents.size());
    for (const auto &i : constituents)
      cv.push_back(dyn_cast<Constant>(i));
    switch (bv->getType()->getOpCode()) {
    case OpTypeVector: {
      auto vecTy = transType(cc->getType());
      Value *v = PoisonValue::get(vecTy);
      for (unsigned idx = 0, i = 0, e = constituents.size(); i < e; ++i) {
        if (constituents[i]->getType()->isVectorTy()) {
          // NOTE: It is allowed to construct a vector from several "smaller"
          // scalars or vectors, such as vec4 = (vec2, vec2) or vec4 = (float,
          // vec3).
          auto compCount = cast<FixedVectorType>(constituents[i]->getType())->getNumElements();
          for (unsigned j = 0; j < compCount; ++j) {
            auto comp = ExtractElementInst::Create(constituents[i], ConstantInt::get(*m_context, APInt(32, j)), "", bb);
            v = InsertElementInst::Create(v, comp, ConstantInt::get(*m_context, APInt(32, idx)), "", bb);
            ++idx;
          }
        } else {
          v = InsertElementInst::Create(v, constituents[i], ConstantInt::get(*m_context, APInt(32, idx)), "", bb);
          ++idx;
        }
      }
      return mapValue(bv, v);
    }
    case OpTypeArray:
    case OpTypeStruct: {
      auto ccTy = transType(cc->getType());
      Value *v = PoisonValue::get(ccTy);
      for (size_t i = 0, e = constituents.size(); i < e; ++i)
        v = InsertValueInst::Create(v, constituents[i], i, "", bb);
      return mapValue(bv, v);
    }
    case OpTypeMatrix: {
      auto bvTy = bv->getType();
      auto matClmTy = transType(bvTy->getMatrixColumnType());
      auto matCount = bvTy->getMatrixColumnCount();
      auto matTy = ArrayType::get(matClmTy, matCount);

      Value *v = PoisonValue::get(matTy);
      for (unsigned i = 0, e = constituents.size(); i < e; ++i)
        v = InsertValueInst::Create(v, constituents[i], i, "", bb);
      return mapValue(bv, v);
    }
    default:
      llvm_unreachable("Unhandled type!");
    }
  }

  case OpCompositeExtract: {
    SPIRVCompositeExtract *ce = static_cast<SPIRVCompositeExtract *>(bv);
    if (ce->getComposite()->getType()->isTypeVector()) {
      assert(ce->getIndices().size() == 1 && "Invalid index");
      return mapValue(bv, ExtractElementInst::Create(transValue(ce->getComposite(), f, bb),
                                                     ConstantInt::get(*m_context, APInt(32, ce->getIndices()[0])),
                                                     bv->getName(), bb));
    }

    auto cv = transValue(ce->getComposite(), f, bb);
    auto indexedTy = ExtractValueInst::getIndexedType(cv->getType(), ce->getIndices());
    if (!indexedTy) {
      // NOTE: "OpCompositeExtract" could extract a scalar component from a
      // vector or a vector in an aggregate. But in LLVM, "extractvalue" is
      // unable to do such thing. We have to replace it with "extractelement"
      // + "extractelement" to achieve this purpose.
      assert(ce->getType()->isTypeScalar());
      std::vector<SPIRVWord> idxs = ce->getIndices();
      auto lastIdx = idxs.back();
      idxs.pop_back();

      Value *v = ExtractValueInst::Create(cv, idxs, "", bb);
      assert(v->getType()->isVectorTy());
      return mapValue(
          bv, ExtractElementInst::Create(v, ConstantInt::get(*m_context, APInt(32, lastIdx)), bv->getName(), bb));
    }
    return mapValue(bv, ExtractValueInst::Create(cv, ce->getIndices(), bv->getName(), bb));
  }

  case OpVectorExtractDynamic: {
    auto ce = static_cast<SPIRVVectorExtractDynamic *>(bv);
    return mapValue(bv, ExtractElementInst::Create(transValue(ce->getVector(), f, bb),
                                                   transValue(ce->getIndex(), f, bb), bv->getName(), bb));
  }

  case OpCompositeInsert: {
    auto ci = static_cast<SPIRVCompositeInsert *>(bv);
    if (ci->getComposite()->getType()->isTypeVector()) {
      assert(ci->getIndices().size() == 1 && "Invalid index");
      return mapValue(bv, InsertElementInst::Create(
                              transValue(ci->getComposite(), f, bb), transValue(ci->getObject(), f, bb),
                              ConstantInt::get(*m_context, APInt(32, ci->getIndices()[0])), bv->getName(), bb));
    }

    auto cv = transValue(ci->getComposite(), f, bb);
    auto indexedTy = ExtractValueInst::getIndexedType(cv->getType(), ci->getIndices());
    if (!indexedTy) {
      // NOTE: "OpCompositeInsert" could insert a scalar component to a
      // vector or a vector in an aggregate. But in LLVM, "insertvalue" is
      // unable to do such thing. We have to replace it with "extractvalue" +
      // "insertelement" + "insertvalue" to achieve this purpose.
      assert(ci->getObject()->getType()->isTypeScalar());
      std::vector<SPIRVWord> idxs = ci->getIndices();
      auto lastIdx = idxs.back();
      idxs.pop_back();

      Value *v = ExtractValueInst::Create(cv, idxs, "", bb);
      assert(v->getType()->isVectorTy());
      v = InsertElementInst::Create(v, transValue(ci->getObject(), f, bb),
                                    ConstantInt::get(*m_context, APInt(32, lastIdx)), "", bb);
      return mapValue(bv, InsertValueInst::Create(cv, v, idxs, bv->getName(), bb));
    }
    return mapValue(
        bv, InsertValueInst::Create(cv, transValue(ci->getObject(), f, bb), ci->getIndices(), bv->getName(), bb));
  }

  case OpVectorInsertDynamic: {
    auto ci = static_cast<SPIRVVectorInsertDynamic *>(bv);
    return mapValue(bv,
                    InsertElementInst::Create(transValue(ci->getVector(), f, bb), transValue(ci->getComponent(), f, bb),
                                              transValue(ci->getIndex(), f, bb), bv->getName(), bb));
  }

  case OpVectorShuffle: {
    auto vs = static_cast<SPIRVVectorShuffle *>(bv);

    auto v1 = transValue(vs->getVector1(), f, bb);
    auto v2 = transValue(vs->getVector2(), f, bb);

    auto newVecCompCount = vs->getComponents().size();
    auto vec1CompCount = vs->getVector1ComponentCount();
    auto vec2CompCount = vs->getVector2ComponentCount();

    // Converting vector of uint32_t types (vs->getComponents()) to vector of int types.
    // This is required by CreateShuffleVector which is not accepting uint32_t vectors.
    SmallVector<int, 4> mask(vs->getComponents().begin(), vs->getComponents().end());

    // If vectors are not the same size then extend smaller one
    // to be the same as bigger. This is required by shufflevector from llvm.
    if (v1->getType() != v2->getType()) {

      auto smallerVec = std::min(vec1CompCount, vec2CompCount);
      auto biggerVec = std::max(vec1CompCount, vec2CompCount);

      // Create temporary mask used to extend size of smaller vector.
      SmallVector<int> extendedVecMask = createSequentialMask(0, smallerVec, (biggerVec - smallerVec));

      // Find smaller vector and extend its size.
      if (vec1CompCount < vec2CompCount) {
        v1 = getBuilder()->CreateShuffleVector(v1, extendedVecMask);

        // Adjust mask since v1 was smaller vector and needs to be extended with undefs.
        for (size_t i = 0; i != newVecCompCount; ++i) {
          if (mask[i] >= vec1CompCount)
            mask[i] += (biggerVec - smallerVec);
        }
      } else {
        v2 = getBuilder()->CreateShuffleVector(v2, extendedVecMask);
      }
    }

    auto *newVec = getBuilder()->CreateShuffleVector(v1, v2, mask);

    return mapValue(bv, newVec);
  }

  case OpFunctionCall: {
    SPIRVFunctionCall *bc = static_cast<SPIRVFunctionCall *>(bv);
    SmallVector<Value *, 8> args;
    for (SPIRVValue *bArg : bc->getArgumentValues()) {
      Value *arg = transValue(bArg, f, bb);
      if (!arg) {
        // This arg is a variable that is (array of) image/sampler/sampledimage.
        // Materialize it.
        assert(bArg->getOpCode() == OpVariable);
        arg = transImagePointer(bArg);
      }
      args.push_back(arg);
    }
    auto call = CallInst::Create(transFunction(bc->getFunction()), args, "", bb);
    setCallingConv(call);
    setAttrByCalledFunc(call);
    return mapValue(bv, call);
  }

  case OpControlBarrier:
  case OpMemoryBarrier:
    return mapValue(bv, transBarrierFence(static_cast<SPIRVInstruction *>(bv), bb));

  case OpSNegate: {
    SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
    return mapValue(bv, BinaryOperator::CreateNSWNeg(transValue(bc->getOperand(0), f, bb), bv->getName(), bb));
  }
  case OpSMod: {
    SPIRVBinary *bc = static_cast<SPIRVBinary *>(bv);
    Value *val0 = transValue(bc->getOperand(0), f, bb);
    Value *val1 = transValue(bc->getOperand(1), f, bb);
    return mapValue(bc, getBuilder()->CreateSMod(val0, val1));
  }
  case OpFMod: {
    SPIRVFMod *bc = static_cast<SPIRVFMod *>(bv);
    Value *val0 = transValue(bc->getDividend(), f, bb);
    Value *val1 = transValue(bc->getDivisor(), f, bb);
    return mapValue(bc, getBuilder()->CreateFMod(val0, val1));
  }
  case OpFNegate: {
    SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
    Value *val0 = transValue(bc->getOperand(0), f, bb);
    auto fNeg = getBuilder()->CreateFNeg(val0);
    setFastMathFlags(fNeg);
    // Since OpFNegate is considered a floating point instruction, we must take
    // that into account (since LLVM fneg is not).
    fNeg = flushDenorm(fNeg);
    return mapValue(bv, fNeg);
  }

  case OpFConvert: {
    SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
    Value *val = transValue(bc->getOperand(0), f, bb);
    Type *destTy = transType(bc->getType());
    if (val->getType()->getScalarType()->getPrimitiveSizeInBits() <= destTy->getScalarType()->getPrimitiveSizeInBits())
      return mapValue(bv, getBuilder()->CreateFPExt(val, destTy));

    RoundingMode rm = RoundingMode::Dynamic;
    SPIRVFPRoundingModeKind rounding;
    if (bc->hasFPRoundingMode(&rounding)) {
      switch (rounding) {
      case FPRoundingModeRTE:
        rm = RoundingMode::NearestTiesToEven;
        break;
      case FPRoundingModeRTZ:
        rm = RoundingMode::TowardZero;
        break;
      case FPRoundingModeRTP:
        rm = RoundingMode::TowardPositive;
        break;
      case FPRoundingModeRTN:
        rm = RoundingMode::TowardNegative;
        break;
      default:
        llvm_unreachable("Should never be called!");
      }
      return mapValue(bv, getBuilder()->CreateFpTruncWithRounding(val, destTy, rm));
    }
    return mapValue(bv, getBuilder()->CreateFPTrunc(val, destTy));
  }

  case OpBitCount: {
    SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
    Value *val = transValue(bc->getOperand(0), f, bb);
    Value *result = getBuilder()->CreateUnaryIntrinsic(Intrinsic::ctpop, val);
    result = getBuilder()->CreateZExtOrTrunc(result, transType(bc->getType()));
    return mapValue(bv, result);
  }

  case OpBitReverse: {
    SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
    Value *val = transValue(bc->getOperand(0), f, bb);
    Value *result = getBuilder()->CreateUnaryIntrinsic(Intrinsic::bitreverse, val);
    return mapValue(bv, result);
  }

  case OpBitFieldInsert: {
    auto bc = static_cast<SPIRVInstTemplateBase *>(bv);
    Value *base = transValue(bc->getOperand(0), f, bb);
    Value *insert = transValue(bc->getOperand(1), f, bb);
    Value *offset = transValue(bc->getOperand(2), f, bb);
    Value *count = transValue(bc->getOperand(3), f, bb);
    return mapValue(bv, getBuilder()->CreateInsertBitField(base, insert, offset, count));
  }

  case OpBitFieldUExtract:
  case OpBitFieldSExtract: {
    auto bc = static_cast<SPIRVInstTemplateBase *>(bv);
    Value *base = transValue(bc->getOperand(0), f, bb);
    bool isSigned = (oc == OpBitFieldSExtract);
    Value *offset = transValue(bc->getOperand(1), f, bb);
    Value *count = transValue(bc->getOperand(2), f, bb);
    return mapValue(bv, getBuilder()->CreateExtractBitField(base, offset, count, isSigned));
  }

  case OpQuantizeToF16: {
    SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
    Value *val = transValue(bc->getOperand(0), f, bb);
    Value *result = getBuilder()->CreateQuantizeToFp16(val);
    return mapValue(bc, result);
  }

  case OpLogicalNot:
  case OpNot: {
    SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
    return mapValue(bv, BinaryOperator::CreateNot(transValue(bc->getOperand(0), f, bb), bv->getName(), bb));
  }

  case OpAll:
  case OpAny: {
    SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
    Value *val = transValue(bc->getOperand(0), f, bb);
    if (!isa<VectorType>(val->getType()))
      return val;
    Value *result = getBuilder()->CreateExtractElement(val, uint64_t(0));
    for (unsigned i = 1, e = cast<FixedVectorType>(val->getType())->getNumElements(); i != e; ++i) {
      Value *elem = getBuilder()->CreateExtractElement(val, i);
      if (oc == OpAny)
        result = getBuilder()->CreateOr(result, elem);
      else
        result = getBuilder()->CreateAnd(result, elem);
    }
    // Vector of bool is <N x i32>, but single bool result needs to be i1.
    result = getBuilder()->CreateTrunc(result, transType(bc->getType()));
    return mapValue(bc, result);
  }

  case OpIAddCarry: {
    SPIRVBinary *bc = static_cast<SPIRVBinary *>(bv);
    Value *val0 = transValue(bc->getOperand(0), f, bb);
    Value *val1 = transValue(bc->getOperand(1), f, bb);
    Value *add = getBuilder()->CreateIntrinsic(Intrinsic::uadd_with_overflow, val0->getType(), {val0, val1});
    Value *result = PoisonValue::get(transType(bc->getType()));
    result = getBuilder()->CreateInsertValue(result, getBuilder()->CreateExtractValue(add, 0), 0);
    result = getBuilder()->CreateInsertValue(
        result, getBuilder()->CreateZExt(getBuilder()->CreateExtractValue(add, 1), val0->getType()), 1);
    return mapValue(bc, result);
  }

  case OpISubBorrow: {
    SPIRVBinary *bc = static_cast<SPIRVBinary *>(bv);
    Value *val0 = transValue(bc->getOperand(0), f, bb);
    Value *val1 = transValue(bc->getOperand(1), f, bb);
    Value *sub = getBuilder()->CreateIntrinsic(Intrinsic::usub_with_overflow, val0->getType(), {val0, val1});
    Value *result = PoisonValue::get(transType(bc->getType()));
    result = getBuilder()->CreateInsertValue(result, getBuilder()->CreateExtractValue(sub, 0), 0);
    result = getBuilder()->CreateInsertValue(
        result, getBuilder()->CreateZExt(getBuilder()->CreateExtractValue(sub, 1), val0->getType()), 1);
    return mapValue(bc, result);
  }

  case OpUMulExtended:
  case OpSMulExtended: {
    SPIRVBinary *bc = static_cast<SPIRVBinary *>(bv);
    Value *val0 = transValue(bc->getOperand(0), f, bb);
    Value *val1 = transValue(bc->getOperand(1), f, bb);
    Type *inTy = val0->getType();
    const unsigned bitWidth = inTy->getScalarSizeInBits();
    Type *extendedTy = inTy->getExtendedType();
    if (oc == OpUMulExtended) {
      val0 = getBuilder()->CreateZExt(val0, extendedTy);
      val1 = getBuilder()->CreateZExt(val1, extendedTy);
    } else {
      val0 = getBuilder()->CreateSExt(val0, extendedTy);
      val1 = getBuilder()->CreateSExt(val1, extendedTy);
    }
    Value *mul = getBuilder()->CreateMul(val0, val1);
    Value *loResult = getBuilder()->CreateTrunc(mul, inTy);
    Value *hiResult =
        getBuilder()->CreateTrunc(getBuilder()->CreateLShr(mul, ConstantInt::get(mul->getType(), bitWidth)), inTy);
    Value *result = PoisonValue::get(transType(bc->getType()));
    result = getBuilder()->CreateInsertValue(result, loResult, 0);
    result = getBuilder()->CreateInsertValue(result, hiResult, 1);
    return mapValue(bc, result);
  }

  case OpIsInf: {
    SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
    Value *val0 = transValue(bc->getOperand(0), f, bb);
    Value *result = getBuilder()->CreateIsInf(val0);
    // ZExt to cope with vector of bool being represented by <N x i32>
    return mapValue(bv, getBuilder()->CreateZExt(result, transType(bc->getType())));
  }

  case OpIsNan: {
    SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
    Value *val0 = transValue(bc->getOperand(0), f, bb);
    Value *result = getBuilder()->CreateIsNaN(val0);
    // ZExt to cope with vector of bool being represented by <N x i32>
    return mapValue(bv, getBuilder()->CreateZExt(result, transType(bc->getType())));
  }

  case OpDPdx:
  case OpDPdxCoarse:
  case OpDPdxFine: {
    SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
    bool isFine = oc == OpDPdxFine;
    Value *val0 = transValue(bc->getOperand(0), f, bb);
    return mapValue(bv, getBuilder()->CreateDerivative(val0, /*isY=*/false, isFine));
  }

  case OpDPdy:
  case OpDPdyCoarse:
  case OpDPdyFine: {
    SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
    bool isFine = oc == OpDPdyFine;
    Value *val0 = transValue(bc->getOperand(0), f, bb);
    Value *dpdy = getBuilder()->CreateDerivative(val0, /*isY=*/true, isFine);

    auto llpcContext = static_cast<Llpc::Context *>(m_context);
    if (llpcContext->getPipelineType() == PipelineType::Graphics) {
      auto buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(llpcContext->getPipelineBuildInfo());
      if (buildInfo->originUpperLeft)
        dpdy = getBuilder()->CreateFNeg(dpdy);
    }

    return mapValue(bv, dpdy);
  }

  case OpFwidth:
  case OpFwidthCoarse:
  case OpFwidthFine: {
    SPIRVUnary *bc = static_cast<SPIRVUnary *>(bv);
    bool isFine = oc == OpFwidthFine;
    Value *val0 = transValue(bc->getOperand(0), f, bb);
    Value *dpdx = getBuilder()->CreateDerivative(val0, /*isY=*/false, isFine);
    Value *dpdy = getBuilder()->CreateDerivative(val0, /*isY=*/true, isFine);
    Value *absDpdx = getBuilder()->CreateUnaryIntrinsic(Intrinsic::fabs, dpdx);
    Value *absDpdy = getBuilder()->CreateUnaryIntrinsic(Intrinsic::fabs, dpdy);
    return mapValue(bv, getBuilder()->CreateFAdd(absDpdx, absDpdy));
  }

  case OpImageSampleImplicitLod:
  case OpImageSampleExplicitLod:
  case OpImageSampleDrefImplicitLod:
  case OpImageSampleDrefExplicitLod:
  case OpImageSampleProjImplicitLod:
  case OpImageSampleProjExplicitLod:
  case OpImageSampleProjDrefImplicitLod:
  case OpImageSampleProjDrefExplicitLod:
  case OpImageSparseSampleImplicitLod:
  case OpImageSparseSampleExplicitLod:
  case OpImageSparseSampleDrefImplicitLod:
  case OpImageSparseSampleDrefExplicitLod:
  case OpImageSparseSampleProjImplicitLod:
  case OpImageSparseSampleProjExplicitLod:
  case OpImageSparseSampleProjDrefImplicitLod:
  case OpImageSparseSampleProjDrefExplicitLod:
    return mapValue(bv, transSPIRVImageSampleFromInst(static_cast<SPIRVInstruction *>(bv), bb));

  case OpImageFetch:
  case OpImageSparseFetch:
  case OpImageRead:
  case OpImageSparseRead:
    return mapValue(bv, transSPIRVImageFetchReadFromInst(static_cast<SPIRVInstruction *>(bv), bb));

  case OpImageGather:
  case OpImageDrefGather:
  case OpImageSparseGather:
  case OpImageSparseDrefGather:
    return mapValue(bv, transSPIRVImageGatherFromInst(static_cast<SPIRVInstruction *>(bv), bb));

  case OpImageQuerySizeLod:
  case OpImageQuerySize:
    return mapValue(bv, transSPIRVImageQuerySizeFromInst(static_cast<SPIRVInstruction *>(bv), bb));

  case OpImageQueryLod:
    return mapValue(bv, transSPIRVImageQueryLodFromInst(static_cast<SPIRVInstruction *>(bv), bb));

  case OpImageQueryLevels:
    return mapValue(bv, transSPIRVImageQueryLevelsFromInst(static_cast<SPIRVInstruction *>(bv), bb));

  case OpImageQuerySamples:
    return mapValue(bv, transSPIRVImageQuerySamplesFromInst(static_cast<SPIRVInstruction *>(bv), bb));

  case OpImageWrite:
    return mapValue(bv, transSPIRVImageWriteFromInst(static_cast<SPIRVInstruction *>(bv), bb));

  case OpFragmentMaskFetchAMD:
    return mapValue(bv, transSPIRVFragmentMaskFetchFromInst(static_cast<SPIRVInstruction *>(bv), bb));

  case OpFragmentFetchAMD:
    return mapValue(bv, transSPIRVFragmentFetchFromInst(static_cast<SPIRVInstruction *>(bv), bb));

  case OpImageSparseTexelsResident: {
    SPIRVImageSparseTexelsResident *bi = static_cast<SPIRVImageSparseTexelsResident *>(bv);
    auto residentCode = transValue(bi->getResidentCode(), f, bb);
    return mapValue(bv, getBuilder()->CreateICmpEQ(residentCode, getBuilder()->getInt32(0)));
  }
  case OpImageTexelPointer:
    return nullptr;
  case OpPtrDiff: {
    SPIRVBinary *const bi = static_cast<SPIRVBinary *>(bv);
    Value *const op1 =
        transValue(bi->getOpValue(0), getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());
    Value *const op2 =
        transValue(bi->getOpValue(1), getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());
    Type *ty = transType(bi->getOpValue(0)->getType()->getPointerElementType());

    Value *ptrDiff = getBuilder()->CreatePtrDiff(ty, op1, op2);

    auto destType = cast<IntegerType>(transType(bv->getType()));
    auto ptrDiffType = cast<IntegerType>(ptrDiff->getType());
    assert(destType->getBitWidth() <= ptrDiffType->getBitWidth());
    if (destType->getBitWidth() < ptrDiffType->getBitWidth())
      ptrDiff = new TruncInst(ptrDiff, destType, "", bb);

    return mapValue(bv, ptrDiff);
  }

  case OpAtomicLoad:
    return mapValue(bv, transValueWithOpcode<OpAtomicLoad>(bv));
  case OpAtomicStore:
    return mapValue(bv, transValueWithOpcode<OpAtomicStore>(bv));
  case OpAtomicExchange:
    return mapValue(bv, transValueWithOpcode<OpAtomicExchange>(bv));
  case OpAtomicCompareExchange:
    return mapValue(bv, transValueWithOpcode<OpAtomicCompareExchange>(bv));
  case OpAtomicIIncrement:
    return mapValue(bv, transValueWithOpcode<OpAtomicIIncrement>(bv));
  case OpAtomicIDecrement:
    return mapValue(bv, transValueWithOpcode<OpAtomicIDecrement>(bv));
  case OpAtomicIAdd:
    return mapValue(bv, transValueWithOpcode<OpAtomicIAdd>(bv));
  case OpAtomicISub:
    return mapValue(bv, transValueWithOpcode<OpAtomicISub>(bv));
  case OpAtomicSMin:
    return mapValue(bv, transValueWithOpcode<OpAtomicSMin>(bv));
  case OpAtomicUMin:
    return mapValue(bv, transValueWithOpcode<OpAtomicUMin>(bv));
  case OpAtomicSMax:
    return mapValue(bv, transValueWithOpcode<OpAtomicSMax>(bv));
  case OpAtomicUMax:
    return mapValue(bv, transValueWithOpcode<OpAtomicUMax>(bv));
  case OpAtomicAnd:
    return mapValue(bv, transValueWithOpcode<OpAtomicAnd>(bv));
  case OpAtomicOr:
    return mapValue(bv, transValueWithOpcode<OpAtomicOr>(bv));
  case OpAtomicXor:
    return mapValue(bv, transValueWithOpcode<OpAtomicXor>(bv));
  case OpAtomicFMinEXT:
    return mapValue(bv, transValueWithOpcode<OpAtomicFMinEXT>(bv));
  case OpAtomicFMaxEXT:
    return mapValue(bv, transValueWithOpcode<OpAtomicFMaxEXT>(bv));
  case OpAtomicFAddEXT:
    return mapValue(bv, transValueWithOpcode<OpAtomicFAddEXT>(bv));
  case OpCopyMemory:
    return mapValue(bv, transValueWithOpcode<OpCopyMemory>(bv));
  case OpLoad:
    return mapValue(bv, transValueWithOpcode<OpLoad>(bv));
  case OpStore:
    return mapValue(bv, transValueWithOpcode<OpStore>(bv));
  case OpEndPrimitive:
    return mapValue(bv, transValueWithOpcode<OpEndPrimitive>(bv));
  case OpEndStreamPrimitive:
    return mapValue(bv, transValueWithOpcode<OpEndStreamPrimitive>(bv));
  case OpAccessChain:
    return mapValue(bv, transValueWithOpcode<OpAccessChain>(bv));
  case OpArrayLength:
    return mapValue(bv, transValueWithOpcode<OpArrayLength>(bv));
  case OpInBoundsAccessChain:
    return mapValue(bv, transValueWithOpcode<OpInBoundsAccessChain>(bv));
  case OpPtrAccessChain:
    return mapValue(bv, transValueWithOpcode<OpPtrAccessChain>(bv));
  case OpInBoundsPtrAccessChain:
    return mapValue(bv, transValueWithOpcode<OpInBoundsPtrAccessChain>(bv));
  case OpImage:
    return mapValue(bv, transValueWithOpcode<OpImage>(bv));
  case OpSampledImage:
    return mapValue(bv, transValueWithOpcode<OpSampledImage>(bv));
  case OpKill:
  case OpTerminateInvocation:
    return mapValue(bv, transValueWithOpcode<OpKill>(bv));
  case OpReadClockKHR:
    return mapValue(bv, transValueWithOpcode<OpReadClockKHR>(bv));
  case OpGroupAll:
    return mapValue(bv, transValueWithOpcode<OpGroupAll>(bv));
  case OpGroupAny:
    return mapValue(bv, transValueWithOpcode<OpGroupAny>(bv));
  case OpGroupBroadcast:
    return mapValue(bv, transValueWithOpcode<OpGroupBroadcast>(bv));
  case OpGroupIAdd:
    return mapValue(bv, transValueWithOpcode<OpGroupIAdd>(bv));
  case OpGroupFAdd:
    return mapValue(bv, transValueWithOpcode<OpGroupFAdd>(bv));
  case OpGroupFMin:
    return mapValue(bv, transValueWithOpcode<OpGroupFMin>(bv));
  case OpGroupUMin:
    return mapValue(bv, transValueWithOpcode<OpGroupUMin>(bv));
  case OpGroupSMin:
    return mapValue(bv, transValueWithOpcode<OpGroupSMin>(bv));
  case OpGroupFMax:
    return mapValue(bv, transValueWithOpcode<OpGroupFMax>(bv));
  case OpGroupUMax:
    return mapValue(bv, transValueWithOpcode<OpGroupUMax>(bv));
  case OpGroupSMax:
    return mapValue(bv, transValueWithOpcode<OpGroupSMax>(bv));
  case OpGroupNonUniformElect:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformElect>(bv));
  case OpGroupNonUniformAll:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformAll>(bv));
  case OpGroupNonUniformAny:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformAny>(bv));
  case OpGroupNonUniformAllEqual:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformAllEqual>(bv));
  case OpGroupNonUniformBroadcast: {
    if (SPIRVConvertNonUniformBroadcastToShuffle) {
      return mapValue(bv, transValueWithOpcode<OpGroupNonUniformShuffle>(bv));
    }
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformBroadcast>(bv));
  }
  case OpGroupNonUniformBroadcastFirst:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformBroadcastFirst>(bv));
  case OpGroupNonUniformBallot:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformBallot>(bv));
  case OpGroupNonUniformInverseBallot:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformInverseBallot>(bv));
  case OpGroupNonUniformBallotBitExtract:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformBallotBitExtract>(bv));
  case OpGroupNonUniformBallotBitCount:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformBallotBitCount>(bv));
  case OpGroupNonUniformBallotFindLSB:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformBallotFindLSB>(bv));
  case OpGroupNonUniformBallotFindMSB:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformBallotFindMSB>(bv));
  case OpGroupNonUniformShuffle:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformShuffle>(bv));
  case OpGroupNonUniformShuffleXor:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformShuffleXor>(bv));
  case OpGroupNonUniformShuffleUp:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformShuffleUp>(bv));
  case OpGroupNonUniformShuffleDown:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformShuffleDown>(bv));
  case OpGroupNonUniformIAdd:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformIAdd>(bv));
  case OpGroupNonUniformFAdd:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformFAdd>(bv));
  case OpGroupNonUniformIMul:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformIMul>(bv));
  case OpGroupNonUniformFMul:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformFMul>(bv));
  case OpGroupNonUniformSMin:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformSMin>(bv));
  case OpGroupNonUniformUMin:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformUMin>(bv));
  case OpGroupNonUniformFMin:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformFMin>(bv));
  case OpGroupNonUniformSMax:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformSMax>(bv));
  case OpGroupNonUniformUMax:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformUMax>(bv));
  case OpGroupNonUniformFMax:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformFMax>(bv));
  case OpGroupNonUniformBitwiseAnd:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformBitwiseAnd>(bv));
  case OpGroupNonUniformBitwiseOr:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformBitwiseOr>(bv));
  case OpGroupNonUniformBitwiseXor:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformBitwiseXor>(bv));
  case OpGroupNonUniformLogicalAnd:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformLogicalAnd>(bv));
  case OpGroupNonUniformLogicalOr:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformLogicalOr>(bv));
  case OpGroupNonUniformLogicalXor:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformLogicalXor>(bv));
  case OpGroupNonUniformQuadBroadcast:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformQuadBroadcast>(bv));
  case OpGroupNonUniformQuadSwap:
    return mapValue(bv, transValueWithOpcode<OpGroupNonUniformQuadSwap>(bv));
  case OpSubgroupBallotKHR:
    return mapValue(bv, transValueWithOpcode<OpSubgroupBallotKHR>(bv));
  case OpSubgroupFirstInvocationKHR:
    return mapValue(bv, transValueWithOpcode<OpSubgroupFirstInvocationKHR>(bv));
  case OpSubgroupAllKHR:
    return mapValue(bv, transValueWithOpcode<OpSubgroupAllKHR>(bv));
  case OpSubgroupAnyKHR:
    return mapValue(bv, transValueWithOpcode<OpSubgroupAnyKHR>(bv));
  case OpSubgroupAllEqualKHR:
    return mapValue(bv, transValueWithOpcode<OpSubgroupAllEqualKHR>(bv));
  case OpSubgroupReadInvocationKHR:
    return mapValue(bv, transValueWithOpcode<OpSubgroupReadInvocationKHR>(bv));
  case OpGroupIAddNonUniformAMD:
    return mapValue(bv, transValueWithOpcode<OpGroupIAddNonUniformAMD>(bv));
  case OpGroupFAddNonUniformAMD:
    return mapValue(bv, transValueWithOpcode<OpGroupFAddNonUniformAMD>(bv));
  case OpGroupFMinNonUniformAMD:
    return mapValue(bv, transValueWithOpcode<OpGroupFMinNonUniformAMD>(bv));
  case OpGroupUMinNonUniformAMD:
    return mapValue(bv, transValueWithOpcode<OpGroupUMinNonUniformAMD>(bv));
  case OpGroupSMinNonUniformAMD:
    return mapValue(bv, transValueWithOpcode<OpGroupSMinNonUniformAMD>(bv));
  case OpGroupFMaxNonUniformAMD:
    return mapValue(bv, transValueWithOpcode<OpGroupFMaxNonUniformAMD>(bv));
  case OpGroupUMaxNonUniformAMD:
    return mapValue(bv, transValueWithOpcode<OpGroupUMaxNonUniformAMD>(bv));
  case OpGroupSMaxNonUniformAMD:
    return mapValue(bv, transValueWithOpcode<OpGroupSMaxNonUniformAMD>(bv));
  case OpTranspose:
    return mapValue(bv, transValueWithOpcode<OpTranspose>(bv));
  case OpExtInst:
    return mapValue(bv, transValueWithOpcode<OpExtInst>(bv));
  case OpMatrixTimesScalar:
    return mapValue(bv, transValueWithOpcode<OpMatrixTimesScalar>(bv));
  case OpVectorTimesMatrix:
    return mapValue(bv, transValueWithOpcode<OpVectorTimesMatrix>(bv));
  case OpMatrixTimesVector:
    return mapValue(bv, transValueWithOpcode<OpMatrixTimesVector>(bv));
  case OpMatrixTimesMatrix:
    return mapValue(bv, transValueWithOpcode<OpMatrixTimesMatrix>(bv));
  case OpOuterProduct:
    return mapValue(bv, transValueWithOpcode<OpOuterProduct>(bv));
  case OpDot:
    return mapValue(bv, transValueWithOpcode<OpDot>(bv));
  case OpDemoteToHelperInvocationEXT:
    return mapValue(bv, transValueWithOpcode<OpDemoteToHelperInvocationEXT>(bv));
  case OpIsHelperInvocationEXT:
    return mapValue(bv, transValueWithOpcode<OpIsHelperInvocationEXT>(bv));
  case OpTraceRayKHR:
    return mapValue(bv, createTraceRayDialectOp(bv));
  case OpExecuteCallableKHR:
    return mapValue(bv, transValueWithOpcode<OpExecuteCallableKHR>(bv));
  case OpConvertUToAccelerationStructureKHR:
    return transValueWithOpcode<OpConvertUToAccelerationStructureKHR>(bv);
  case OpTerminateRayKHR:
    return mapValue(bv, transValueWithOpcode<OpTerminateRayKHR>(bv));
  case OpIgnoreIntersectionKHR:
    return mapValue(bv, transValueWithOpcode<OpIgnoreIntersectionKHR>(bv));
  case OpReportIntersectionKHR:
    return mapValue(bv, transValueWithOpcode<OpReportIntersectionKHR>(bv));
  case OpSDotKHR:
  case OpUDotKHR:
  case OpSUDotKHR:
  case OpSDotAccSatKHR:
  case OpUDotAccSatKHR:
  case OpSUDotAccSatKHR:
    return mapValue(bv, transSPIRVIntegerDotProductFromInst(static_cast<SPIRVInstruction *>(bv), bb));
  case OpEmitMeshTasksEXT:
    return transValueWithOpcode<OpEmitMeshTasksEXT>(bv);
  case OpSetMeshOutputsEXT:
    return transValueWithOpcode<OpSetMeshOutputsEXT>(bv);
  default: {
    auto oc = bv->getOpCode();
    if (isSPIRVCmpInstTransToLLVMInst(static_cast<SPIRVInstruction *>(bv)))
      return mapValue(bv, transCmpInst(bv, bb, f));
    if (isBinaryShiftLogicalBitwiseOpCode(oc) || isLogicalOpCode(oc))
      return mapValue(bv, transShiftLogicalBitwiseInst(bv, bb, f));
    if (isCvtOpCode(oc)) {
      Value *inst = transConvertInst(bv, f, bb);
      return mapValue(bv, inst);
    }
    return mapValue(bv, transSPIRVBuiltinFromInst(static_cast<SPIRVInstruction *>(bv), bb));
  }
  }

  llvm_unreachable("Translation of SPIRV instruction not implemented");
  return NULL;
}

void SPIRVToLLVM::truncConstantIndex(std::vector<Value *> &indices, BasicBlock *bb) {
  // Only constant int32 can be used as struct index in LLVM
  // To simplify the logic, for constant index,
  // If constant is less than UINT32_MAX , translate all constant index to int32
  // Otherwise for non constant int, try convert them to int32
  for (unsigned i = 0; i < indices.size(); ++i) {
    auto index = indices[i];
    auto int32Ty = Type::getInt32Ty(*m_context);
    if (isa<ConstantInt>(index)) {
      auto constIndex = cast<ConstantInt>(index);
      if (!constIndex->getType()->isIntegerTy(32)) {
        uint64_t constValue = constIndex->getZExtValue();
        if (constValue < UINT32_MAX) {
          auto constIndex32 = ConstantInt::get(int32Ty, constValue);
          indices[i] = constIndex32;
        }
      }
    } else {
      indices[i] = getBuilder()->CreateZExtOrTrunc(index, int32Ty);
    }
  }
}

template <class SourceTy, class FuncTy> bool SPIRVToLLVM::foreachFuncCtlMask(SourceTy source, FuncTy func) {
  SPIRVWord fcm = source->getFuncCtlMask();
  // Cancel those masks if they are both present
  if ((fcm & FunctionControlInlineMask) && (fcm & FunctionControlDontInlineMask))
    fcm &= ~(FunctionControlInlineMask | FunctionControlDontInlineMask);
  // Ignore DontInline because we rely on inlining everything.
  fcm &= ~FunctionControlDontInlineMask;
  SPIRSPIRVFuncCtlMaskMap::foreach ([&](Attribute::AttrKind attr, SPIRVFunctionControlMaskKind mask) {
    if (fcm & mask)
      func(attr);
  });
  return true;
}

Function *SPIRVToLLVM::transFunction(SPIRVFunction *bf) {
  auto loc = m_funcMap.find(bf);
  if (loc != m_funcMap.end())
    return loc->second;

  auto entryPoint = m_bm->getEntryPoint(bf->getId());
  bool isEntry = (entryPoint);
  SPIRVExecutionModelKind execModel = isEntry ? entryPoint->getExecModel() : ExecutionModelMax;
  auto linkage = isEntry ? GlobalValue::ExternalLinkage : transLinkageType(bf);
  FunctionType *ft = dyn_cast<FunctionType>(transType(bf->getFunctionType()));
  Function *f = cast<Function>(mapValue(bf, Function::Create(ft, linkage, bf->getName(), m_m)));
  mapFunction(bf, f);
  if (!f->isIntrinsic()) {
    if (isEntry) {
      // Setup metadata for execution model
      std::vector<Metadata *> execModelMDs;
      auto int32Ty = Type::getInt32Ty(*m_context);
      execModelMDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, execModel)));
      auto execModelMdNode = MDNode::get(*m_context, execModelMDs);
      f->addMetadata(gSPIRVMD::ExecutionModel, *execModelMdNode);
    }
    f->setCallingConv(CallingConv::SPIR_FUNC);

    if (isFuncNoUnwind())
      f->addFnAttr(Attribute::NoUnwind);
    foreachFuncCtlMask(bf, [&](Attribute::AttrKind attr) { f->addFnAttr(attr); });
  }

  for (Function::arg_iterator i = f->arg_begin(), e = f->arg_end(); i != e; ++i) {
    auto ba = bf->getArgument(i->getArgNo());
    mapValue(ba, &(*i));
    setName(&(*i), ba);

    SPIRVWord maxOffset = 0;
    if (ba->hasDecorate(DecorationMaxByteOffset, 0, &maxOffset)) {
      AttrBuilder builder(*m_context);
      builder.addDereferenceableAttr(maxOffset);
      i->addAttrs(builder);
    }
  }

  // Creating all basic blocks before creating instructions.
  for (size_t i = 0, e = bf->getNumBasicBlock(); i != e; ++i)
    transValue(bf->getBasicBlock(i), f, nullptr);

  // Set name for entry block
  if (f->getEntryBlock().getName().empty())
    f->getEntryBlock().setName(".entry");

  for (size_t i = 0, e = bf->getNumBasicBlock(); i != e; ++i) {
    SPIRVBasicBlock *bbb = bf->getBasicBlock(i);
    BasicBlock *bb = dyn_cast<BasicBlock>(transValue(bbb, f, nullptr));
    for (size_t bi = 0, be = bbb->getNumInst(); bi != be; ++bi) {
      SPIRVInstruction *bInst = bbb->getInst(bi);
      transValue(bInst, f, bb, false);
    }
  }
  m_dbgTran.applyDelayedDbgInfo();

  // Update phi nodes -- add missing incoming arcs.
  // This is necessary because LLVM's CFG is a multigraph, while SPIR-V's
  // CFG is not.
  for (BasicBlock &bb : *f) {

    // Add missing incoming arcs to each phi node that requires fixups.
    for (PHINode &phi : bb.phis()) {
      const unsigned initialNumIncoming = phi.getNumIncomingValues();
      for (unsigned i = 0; i != initialNumIncoming; ++i) {
        BasicBlock *predecessor = phi.getIncomingBlock(i);
        Value *incomingValue = phi.getIncomingValue(i);
        const unsigned numIncomingArcsForPred = getBlockPredecessorCounts(&bb, predecessor);

        for (unsigned j = 1; j < numIncomingArcsForPred; ++j)
          phi.addIncoming(incomingValue, predecessor);
      }
    }
  }

  m_blockPredecessorToCount.clear();

  return f;
}

// Prints LLVM-style name for type to raw_ostream
static void printTypeName(Type *ty, raw_ostream &nameStream) {
  for (;;) {
    if (auto pointerTy = dyn_cast<PointerType>(ty)) {
      nameStream << "p" << pointerTy->getAddressSpace();
      return;
    }
    if (auto arrayTy = dyn_cast<ArrayType>(ty)) {
      nameStream << "a" << arrayTy->getNumElements();
      ty = arrayTy->getElementType();
      continue;
    }
    break;
  }
  if (auto structTy = dyn_cast<StructType>(ty)) {
    nameStream << "s[";
    if (structTy->getNumElements() != 0) {
      printTypeName(structTy->getElementType(0), nameStream);
      for (unsigned i = 1; i < structTy->getNumElements(); ++i) {
        nameStream << ",";
        printTypeName(structTy->getElementType(i), nameStream);
      }
    }
    nameStream << "]";
    return;
  }
  if (auto vecTy = dyn_cast<FixedVectorType>(ty)) {
    nameStream << "v" << vecTy->getNumElements();
    ty = vecTy->getElementType();
  }
  if (ty->isFloatingPointTy()) {
    nameStream << "f" << ty->getScalarSizeInBits();
    return;
  }
  if (ty->isIntegerTy()) {
    nameStream << "i" << ty->getScalarSizeInBits();
    return;
  }
  assert(ty->isVoidTy());
  nameStream << "V";
}

// Adds LLVM-style type mangling suffix for the specified return type and args
// to the name. This is used when adding a call to an external function that
// is later lowered in a SPIRVLower* pass.
//
// @param RetTy : Return type or nullptr
// @param Args : Arg values
// @param [out] Name : String to append the type mangling to
static void appendTypeMangling(Type *retTy, ArrayRef<Value *> args, std::string &name) {
  raw_string_ostream nameStream(name);
  if (retTy && !retTy->isVoidTy()) {
    nameStream << ".";
    printTypeName(retTy, nameStream);
  }
  for (auto arg : args) {
    nameStream << ".";
    printTypeName(arg->getType(), nameStream);
  }
}

Instruction *SPIRVToLLVM::transBuiltinFromInst(const std::string &funcName, SPIRVInstruction *bi, BasicBlock *bb) {
  auto ops = bi->getOperands();
  auto retBTy = bi->hasType() ? bi->getType() : nullptr;
  // NOTE: When function returns a structure-typed value,
  // we have to mark this structure type as "literal".
  if (bi->hasType() && retBTy->getOpCode() == spv::OpTypeStruct) {
    auto structType = static_cast<SPIRVTypeStruct *>(retBTy);
    structType->setLiteral(true);
  }
  Type *retTy = bi->hasType() ? transType(retBTy) : Type::getVoidTy(*m_context);
  std::vector<Type *> argTys = transTypeVector(SPIRVInstruction::getOperandTypes(ops));
  std::vector<Value *> args = transValue(ops, bb->getParent(), bb);
  for (auto &i : argTys) {
    if (isa<FunctionType>(i))
      i = PointerType::get(i, SPIRAS_Private);
  }

  std::string mangledName(funcName);
  appendTypeMangling(retTy, args, mangledName);
  Function *func = m_m->getFunction(mangledName);
  FunctionType *ft = FunctionType::get(retTy, argTys, false);
  // ToDo: Some intermediate functions have duplicate names with
  // different function types. This is OK if the function name
  // is used internally and finally translated to unique function
  // names. However it is better to have a way to differentiate
  // between intermediate functions and final functions and make
  // sure final functions have unique names.
  if (!func || func->getFunctionType() != ft) {
    func = Function::Create(ft, GlobalValue::ExternalLinkage, mangledName, m_m);
    func->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      func->addFnAttr(Attribute::NoUnwind);
    MDNode *const funcMeta = MDNode::get(*m_context, ConstantAsMetadata::get(m_builder->getInt32(bi->getOpCode())));
    func->setMetadata(m_spirvOpMetaKindId, funcMeta);
  }
  auto call = CallInst::Create(func, args, "", bb);
  setName(call, bi);
  setAttrByCalledFunc(call);
  return call;
}

// =============================================================================
// Convert SPIR-V dimension and arrayed into Builder dimension.
static unsigned convertDimension(const SPIRVTypeImageDescriptor *desc) {
  if (desc->MS) {
    assert(desc->Dim == Dim2D || desc->Dim == DimSubpassData);
    return !desc->Arrayed ? lgc::Builder::Dim2DMsaa : lgc::Builder::Dim2DArrayMsaa;
  }
  if (!desc->Arrayed) {
    switch (static_cast<unsigned>(desc->Dim)) {
    case Dim1D:
      return lgc::Builder::Dim1D;
    case DimBuffer:
      return lgc::Builder::Dim1D;
    case Dim2D:
      return lgc::Builder::Dim2D;
    case DimRect:
      return lgc::Builder::DimRect;
    case DimCube:
      return lgc::Builder::DimCube;
    case Dim3D:
      return lgc::Builder::Dim3D;
    case DimSubpassData:
      return lgc::Builder::Dim2D;
    default:
      break;
    }
  } else {
    switch (static_cast<unsigned>(desc->Dim)) {
    case Dim1D:
      return lgc::Builder::Dim1DArray;
    case DimBuffer:
      return lgc::Builder::Dim1DArray;
    case Dim2D:
      return lgc::Builder::Dim2DArray;
    case DimCube:
      return lgc::Builder::DimCubeArray;
    default:
      break;
    }
  }
  llvm_unreachable("Unhandled image dimension");
  return 0;
}

// =============================================================================
// Get image and/or sampler descriptors, and get information from the image
// type.
void SPIRVToLLVM::getImageDesc(SPIRVValue *bImageInst, ExtractedImageInfo *info) {
  auto setEnforceReadFirstLaneFlag = [&](SPIRVValue *accessChainInst, bool isImage) {
    if (!accessChainInst || !isAccessChainOpCode(accessChainInst->getOpCode()))
      return;
    SPIRVAccessChainBase *const spvAccessChain = static_cast<SPIRVAccessChainBase *>(accessChainInst);
    std::vector<SPIRVValue *> spvIndicesVec = spvAccessChain->getIndices();
    // Check if any index is not a constant, set the flag true
    bool enforceReadFirstlane = false;
    for (auto idxIt : spvIndicesVec) {
      if (idxIt->getOpCode() != OpConstant) {
        enforceReadFirstlane = true;
        break;
      }
    }
    if (enforceReadFirstlane)
      info->flags |= isImage ? lgc::Builder::ImageFlagEnforceReadFirstLaneImage
                             : lgc::Builder::ImageFlagEnforceReadFirstLaneSampler;
  };

  bool forceNonUniform = isShaderStageInMask(convertToShaderStage(m_execModule),
                                             getPipelineOptions()->forceNonUniformResourceIndexStageMask);

  if (forceNonUniform || bImageInst->hasDecorate(DecorationNonUniformEXT)) {
    info->flags |= lgc::Builder::ImageFlagNonUniformImage;
    if (bImageInst->getType()->getOpCode() == OpTypeSampledImage)
      info->flags |= lgc::Builder::ImageFlagNonUniformSampler;
  }

  if (bImageInst->getOpCode() == OpImageTexelPointer) {
    // We are looking at the OpImageTexelPointer for an image atomic. Load the
    // image descriptor from its image pointer.
    SPIRVValue *bImagePtr = static_cast<SPIRVImageTexelPointer *>(bImageInst)->getImage();
    info->desc = &static_cast<SPIRVTypeImage *>(bImagePtr->getType()->getPointerElementType())->getDescriptor();
    info->dim = convertDimension(info->desc);
    info->imageDesc = transLoadImage(bImagePtr);
    if (isa<StructType>(info->imageDesc->getType())) {
      // Extract image descriptor from struct containing image+fmask descs.
      info->imageDesc = getBuilder()->CreateExtractValue(info->imageDesc, uint64_t(0));
    }
    if (isa<ArrayType>(info->imageDesc->getType())) {
      // Extract image descriptor from possible array of multi-plane image descriptors.
      info->imageDesc = getBuilder()->CreateExtractValue(info->imageDesc, 0);
    }
    // We also need to trace back to the OpVariable or OpFunctionParam to find
    // the coherent and volatile decorations.
    SPIRVValue *imageAccessChain = nullptr;
    while (bImagePtr->getOpCode() == OpAccessChain || bImagePtr->getOpCode() == OpInBoundsAccessChain) {
      std::vector<SPIRVValue *> operands = static_cast<SPIRVInstTemplateBase *>(bImagePtr)->getOperands();
      for (SPIRVValue *operand : operands) {
        if (forceNonUniform || operand->hasDecorate(DecorationNonUniformEXT))
          info->flags |= lgc::Builder::ImageFlagNonUniformImage;
      }
      imageAccessChain = bImagePtr;
      bImagePtr = operands[0];
    }
    assert(bImagePtr->getOpCode() == OpVariable || bImagePtr->getOpCode() == OpFunctionParameter);
    if (bImageInst->hasDecorate(DecorationCoherent))
      info->flags |= lgc::Builder::ImageFlagCoherent;
    if (bImageInst->hasDecorate(DecorationVolatile))
      info->flags |= lgc::Builder::ImageFlagVolatile;

    // Set enforce readfirstlane flag for accessing image array
    if ((info->flags & lgc::Builder::ImageFlagNonUniformImage) == 0)
      setEnforceReadFirstLaneFlag(imageAccessChain, true);

    return;
  }

  SPIRVValue *imageLoadSrc = nullptr;
  SPIRVValue *samplerLoadSrc = nullptr;
  if (bImageInst->getOpCode() == OpLoad) {
    SPIRVLoad *load = static_cast<SPIRVLoad *>(bImageInst);
    SPIRVValue *const loadSrc = load->getSrc();

    if (loadSrc->isCoherent())
      info->flags |= lgc::Builder::ImageFlagCoherent;
    if (loadSrc->isVolatile())
      info->flags |= lgc::Builder::ImageFlagVolatile;
    if (load->getType()->getOpCode() == OpTypeSampledImage) {
      imageLoadSrc = loadSrc;
      samplerLoadSrc = loadSrc;
    } else {
      imageLoadSrc = loadSrc;
    }
  }

  // We need to scan back through OpImage/OpSampledImage just to find any
  // NonUniform decoration.
  SPIRVValue *scanBackInst = bImageInst;
  while (scanBackInst->getOpCode() == OpImage || scanBackInst->getOpCode() == OpSampledImage) {
    if (scanBackInst->getOpCode() == OpSampledImage) {
      auto sampler = static_cast<SPIRVInstTemplateBase *>(scanBackInst)->getOpValue(1);
      if (forceNonUniform || sampler->hasDecorate(DecorationNonUniformEXT))
        info->flags |= lgc::Builder::ImageFlagNonUniformSampler;
      if (sampler->getOpCode() == OpLoad)
        samplerLoadSrc = static_cast<SPIRVLoad *>(sampler)->getSrc();
    }
    scanBackInst = static_cast<SPIRVInstTemplateBase *>(scanBackInst)->getOpValue(0);
    if (forceNonUniform || scanBackInst->hasDecorate(DecorationNonUniformEXT))
      info->flags |= lgc::Builder::ImageFlagNonUniformImage;
    if (scanBackInst->getOpCode() == OpLoad)
      imageLoadSrc = static_cast<SPIRVLoad *>(scanBackInst)->getSrc();
  }
  // Set enforce readfirstlane flag for accessing image or sampled image array
  if ((info->flags & lgc::Builder::ImageFlagNonUniformImage) == 0)
    setEnforceReadFirstLaneFlag(imageLoadSrc, true);
  if ((info->flags & lgc::Builder::ImageFlagNonUniformSampler) == 0)
    setEnforceReadFirstLaneFlag(samplerLoadSrc, false);

  if (imageLoadSrc && (imageLoadSrc->getOpCode() == OpVariable || imageLoadSrc->getOpCode() == OpFunctionParameter) &&
      !imageLoadSrc->hasDecorate(DecorationAliased))
    info->flags |= lgc::Builder::ImageFlagNotAliased;

  // Get the IR value for the image/sampledimage.
  Value *desc = transValue(bImageInst, getBuilder()->GetInsertBlock()->getParent(), getBuilder()->GetInsertBlock());

  SPIRVType *bImageTy = bImageInst->getType();
  if (bImageTy->getOpCode() == OpTypeSampledImage) {
    // For a sampledimage, the IR value is a struct containing the image and the
    // sampler.
    info->samplerDesc = getBuilder()->CreateExtractValue(desc, 1);
    desc = getBuilder()->CreateExtractValue(desc, uint64_t(0));
    bImageTy = static_cast<SPIRVTypeSampledImage *>(bImageTy)->getImageType();
  }
  assert(bImageTy->getOpCode() == OpTypeImage);
  info->desc = &static_cast<const SPIRVTypeImage *>(bImageTy)->getDescriptor();
  info->dim = convertDimension(info->desc);

  if (info->desc->MS) {
    // For a multisampled image, the IR value is a struct containing the image
    // descriptor and the fmask descriptor.
    info->fmaskDesc = getBuilder()->CreateExtractValue(desc, 1);
    desc = getBuilder()->CreateExtractValue(desc, uint64_t(0));
  }

  // desc might be an array of multi-plane descriptors (for YCbCrSampler conversion).
  info->imageDescArray = desc;
  if (isa<ArrayType>(desc->getType()))
    desc = getBuilder()->CreateExtractValue(desc, 0);

  info->imageDesc = desc;
}

// =============================================================================
// Get number of channels based on the image format.
// Returns 0 if number of channels is unknown.
//
// @param descriptor: The image descriptor to derive the channel count from.
unsigned SPIRVToLLVM::getImageNumChannels(const SPIRVTypeImageDescriptor *descriptor) {
  switch (descriptor->Format) {
  case ImageFormat::ImageFormatR64i:
  case ImageFormat::ImageFormatR64ui:
  case ImageFormat::ImageFormatR32f:
  case ImageFormat::ImageFormatR32i:
  case ImageFormat::ImageFormatR32ui:
  case ImageFormat::ImageFormatR16:
  case ImageFormat::ImageFormatR16Snorm:
  case ImageFormat::ImageFormatR16f:
  case ImageFormat::ImageFormatR16i:
  case ImageFormat::ImageFormatR16ui:
  case ImageFormat::ImageFormatR8:
  case ImageFormat::ImageFormatR8Snorm:
  case ImageFormat::ImageFormatR8i:
  case ImageFormat::ImageFormatR8ui:
    return 1;
  case ImageFormat::ImageFormatRg8:
  case ImageFormat::ImageFormatRg8Snorm:
  case ImageFormat::ImageFormatRg8i:
  case ImageFormat::ImageFormatRg8ui:
  case ImageFormat::ImageFormatRg32f:
  case ImageFormat::ImageFormatRg32i:
  case ImageFormat::ImageFormatRg16:
  case ImageFormat::ImageFormatRg16Snorm:
  case ImageFormat::ImageFormatRg16f:
  case ImageFormat::ImageFormatRg16i:
  case ImageFormat::ImageFormatRg16ui:
    return 2;
  case ImageFormat::ImageFormatR11fG11fB10f:
    return 3;
  case ImageFormat::ImageFormatRgba32f:
  case ImageFormat::ImageFormatRgba32i:
  case ImageFormat::ImageFormatRgba32ui:
  case ImageFormat::ImageFormatRgba16:
  case ImageFormat::ImageFormatRgba16Snorm:
  case ImageFormat::ImageFormatRgba16f:
  case ImageFormat::ImageFormatRgba16i:
  case ImageFormat::ImageFormatRgba16ui:
  case ImageFormat::ImageFormatRgba8Snorm:
  case ImageFormat::ImageFormatRgba8:
  case ImageFormat::ImageFormatRgba8i:
  case ImageFormat::ImageFormatRgba8ui:
  case ImageFormat::ImageFormatRgb10A2:
  case ImageFormat::ImageFormatRgb10a2ui:
    return 4;
  }

  return 0;
}

// =============================================================================
// Set up address operand array for image sample/gather/fetch/read/write
// builder call.
//
// @param BI : The SPIR-V instruction
// @param MaskIdx : Operand number of mask operand
// @param HasProj : Whether there is an extra projective component in coordinate
// @param [in/out] Addr : Image address array
// @param [in/out] ImageInfo : Decoded image type information; flags modified by
//        memory model image operands
// @param [out] Sample : Where to store sample number for OpImageFetch; nullptr to
//        ignore it
void SPIRVToLLVM::setupImageAddressOperands(SPIRVInstruction *bi, unsigned maskIdx, bool hasProj,
                                            MutableArrayRef<Value *> addr, ExtractedImageInfo *imageInfo,
                                            Value **sampleNum) {

  // SPIR-V allows the coordinate vector to be too wide; chop it down here.
  // Also handle the extra projective component if any.
  Value *coord = addr[lgc::Builder::ImageAddressIdxCoordinate];
  if (auto coordVecTy = dyn_cast<FixedVectorType>(coord->getType())) {
    unsigned numCoords = getBuilder()->getImageNumCoords(imageInfo->dim);
    if (hasProj) {
      addr[lgc::Builder::ImageAddressIdxProjective] = getBuilder()->CreateExtractElement(coord, numCoords);
    }
    if (numCoords < coordVecTy->getNumElements()) {
      static const int Indexes[] = {0, 1, 2, 3};
      coord = getBuilder()->CreateShuffleVector(coord, coord, ArrayRef<int>(Indexes).slice(0, numCoords));
      addr[lgc::Builder::ImageAddressIdxCoordinate] = coord;
    }
  }

  // Extra image operands. These need to be in ascending order so they take
  // their operands in the right order.
  BasicBlock *bb = getBuilder()->GetInsertBlock();
  ArrayRef<SPIRVWord> imageOpnds =
      ArrayRef<SPIRVWord>(static_cast<SPIRVInstTemplateBase *>(bi)->getOpWords()).slice(maskIdx);
  if (!imageOpnds.empty()) {
    unsigned mask = imageOpnds[0];
    imageOpnds = imageOpnds.slice(1);

    // Bias (0x1)
    if (mask & ImageOperandsBiasMask) {
      mask &= ~ImageOperandsBiasMask;
      addr[lgc::Builder::ImageAddressIdxLodBias] = transValue(m_bm->getValue(imageOpnds[0]), bb->getParent(), bb);
      imageOpnds = imageOpnds.slice(1);
    }

    // Lod (0x2)
    if (mask & ImageOperandsLodMask) {
      mask &= ~ImageOperandsLodMask;
      addr[lgc::Builder::ImageAddressIdxLod] = transValue(m_bm->getValue(imageOpnds[0]), bb->getParent(), bb);
      imageOpnds = imageOpnds.slice(1);
    }

    // Grad (0x4)
    if (mask & ImageOperandsGradMask) {
      mask &= ~ImageOperandsGradMask;
      addr[lgc::Builder::ImageAddressIdxDerivativeX] = transValue(m_bm->getValue(imageOpnds[0]), bb->getParent(), bb);
      addr[lgc::Builder::ImageAddressIdxDerivativeY] = transValue(m_bm->getValue(imageOpnds[1]), bb->getParent(), bb);
      imageOpnds = imageOpnds.slice(2);
    }

    // ConstOffset (0x8)
    if (mask & ImageOperandsConstOffsetMask) {
      mask &= ~ImageOperandsConstOffsetMask;
      addr[lgc::Builder::ImageAddressIdxOffset] = transValue(m_bm->getValue(imageOpnds[0]), bb->getParent(), bb);
      imageOpnds = imageOpnds.slice(1);
    }

    // Offset (0x10)
    if (mask & ImageOperandsOffsetMask) {
      mask &= ~ImageOperandsOffsetMask;
      assert(!addr[lgc::Builder::ImageAddressIdxOffset]);
      addr[lgc::Builder::ImageAddressIdxOffset] = transValue(m_bm->getValue(imageOpnds[0]), bb->getParent(), bb);
      imageOpnds = imageOpnds.slice(1);
    }

    // ConstOffsets (0x20)
    if (mask & ImageOperandsConstOffsetsMask) {
      mask &= ~ImageOperandsConstOffsetsMask;
      assert(!addr[lgc::Builder::ImageAddressIdxOffset]);
      addr[lgc::Builder::ImageAddressIdxOffset] = transValue(m_bm->getValue(imageOpnds[0]), bb->getParent(), bb);
      imageOpnds = imageOpnds.slice(1);
    }

    // Sample (0x40) (only on OpImageFetch)
    if (mask & ImageOperandsSampleMask) {
      mask &= ~ImageOperandsSampleMask;
      if (sampleNum) {
        *sampleNum = transValue(m_bm->getValue(imageOpnds[0]), bb->getParent(), bb);
      }
      imageOpnds = imageOpnds.slice(1);
    }

    // MinLod (0x80)
    if (mask & ImageOperandsMinLodMask) {
      mask &= ~ImageOperandsMinLodMask;
      addr[lgc::Builder::ImageAddressIdxLodClamp] = transValue(m_bm->getValue(imageOpnds[0]), bb->getParent(), bb);
      imageOpnds = imageOpnds.slice(1);
    }

    // MakeTexelAvailableKHR (0x100)
    if (mask & ImageOperandsMakeTexelAvailableKHRMask) {
      mask &= ~ImageOperandsMakeTexelAvailableKHRMask;
      imageInfo->flags |= lgc::Builder::ImageFlagCoherent;
    }

    // MakeTexelVisibleKHR (0x200)
    if (mask & ImageOperandsMakeTexelVisibleKHRMask) {
      mask &= ~ImageOperandsMakeTexelVisibleKHRMask;
      imageInfo->flags |= lgc::Builder::ImageFlagCoherent;
    }

    // NonPrivateTexelKHR (0x400)
    if (mask & ImageOperandsNonPrivateTexelKHRMask) {
      mask &= ~ImageOperandsNonPrivateTexelKHRMask;
      imageInfo->flags |= lgc::Builder::ImageFlagCoherent;
    }

    // VolatileTexelKHR (0x800)
    if (mask & ImageOperandsVolatileTexelKHRMask) {
      mask &= ~ImageOperandsVolatileTexelKHRMask;
      imageInfo->flags |= lgc::Builder::ImageFlagVolatile;
    }

    // SignExtend (0x1000)
    if (mask & ImageOperandsSignExtendMask) {
      mask &= ~ImageOperandsSignExtendMask;
      imageInfo->flags |= lgc::Builder::ImageFlagSignedResult;
    }

    // ZeroExtend (0x2000)
    if (mask & ImageOperandsZeroExtendMask)
      mask &= ~ImageOperandsZeroExtendMask;

    // Nontemporal (0x4000)
    if (mask & ImageOperandsNontemporalMask)
      mask &= ~ImageOperandsNontemporalMask;

    assert(!mask && "Unknown image operand");
  }
}

// =============================================================================
// Handle fetch/read/write/atomic aspects of coordinate.
// This handles:
// 1. adding any offset onto the coordinate;
// 2. modifying coordinate for subpass data;
// 3. for a cube array, separating the layer and face, as expected by the
//    Builder interface
void SPIRVToLLVM::handleImageFetchReadWriteCoord(SPIRVInstruction *bi, ExtractedImageInfo *imageInfo,
                                                 MutableArrayRef<Value *> addr, bool enableMultiView) {

  // Add the offset (if any) onto the coordinate. The offset might be narrower
  // than the coordinate.
  Value *coord = addr[lgc::Builder::ImageAddressIdxCoordinate];
  if (auto offset = addr[lgc::Builder::ImageAddressIdxOffset]) {
    if (isa<VectorType>(coord->getType())) {
      if (!isa<VectorType>(offset->getType())) {
        offset = getBuilder()->CreateInsertElement(Constant::getNullValue(coord->getType()), offset, uint64_t(0));
      } else if (cast<FixedVectorType>(coord->getType())->getNumElements() !=
                 cast<FixedVectorType>(offset->getType())->getNumElements()) {
        offset = getBuilder()->CreateShuffleVector(
            offset, Constant::getNullValue(offset->getType()),
            ArrayRef<int>({0, 1, 2, 3}).slice(0, cast<FixedVectorType>(coord->getType())->getNumElements()));
      }
    }
    coord = getBuilder()->CreateAdd(coord, offset);
  }

  if (imageInfo->desc->Dim == DimSubpassData) {
    // Modify coordinate for subpass data.
    if (!enableMultiView) {
      // Subpass data without multiview: Add the x,y dimensions (converted to
      // signed int) of the fragment coordinate on to the texel coordinate.
      imageInfo->flags |= lgc::Builder::ImageFlagAddFragCoord;
    } else {
      // Subpass data with multiview: Use the fragment coordinate as x,y, and
      // use ViewIndex as z. We need to pass in a (0,0,0) coordinate.
      imageInfo->flags |= lgc::Builder::ImageFlagAddFragCoord | lgc::Builder::ImageFlagCheckMultiView;
    }
  }

  // For a cube array, separate the layer and face.
  if (imageInfo->dim == lgc::Builder::DimCubeArray) {
    SmallVector<Value *, 4> components;
    for (unsigned i = 0; i != 3; ++i)
      components.push_back(getBuilder()->CreateExtractElement(coord, i));
    components.push_back(getBuilder()->CreateUDiv(components[2], getBuilder()->getInt32(6)));
    components[2] = getBuilder()->CreateURem(components[2], getBuilder()->getInt32(6));
    coord = PoisonValue::get(FixedVectorType::get(getBuilder()->getInt32Ty(), 4));
    for (unsigned i = 0; i != 4; ++i)
      coord = getBuilder()->CreateInsertElement(coord, components[i], i);
  }

  addr[lgc::Builder::ImageAddressIdxCoordinate] = coord;
  return;
}

// =============================================================================
// Translate OpFragmentFetchAMD to LLVM IR
Value *SPIRVToLLVM::transSPIRVFragmentFetchFromInst(SPIRVInstruction *bi, BasicBlock *bb) {

  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo imageInfo = {bb};
  auto bii = static_cast<SPIRVInstTemplateBase *>(bi);
  getImageDesc(bii->getOpValue(0), &imageInfo);

  assert(imageInfo.desc->Dim == Dim2D || imageInfo.desc->Dim == DimSubpassData);
  imageInfo.dim = !imageInfo.desc->Arrayed ? lgc::Builder::Dim2DMsaa : lgc::Builder::Dim2DArrayMsaa;

  // Set up address arguments.
  Value *coord = transValue(bii->getOpValue(1), bb->getParent(), bb);

  // Handle fetch/read/write/atomic aspects of coordinate. (This converts to
  // signed i32 and adds on the FragCoord if DimSubpassData.)
  Value *addr[lgc::Builder::ImageAddressCount] = {};
  addr[lgc::Builder::ImageAddressIdxCoordinate] = coord;
  handleImageFetchReadWriteCoord(bi, &imageInfo, addr,
                                 /*EnableMultiView=*/false);
  coord = addr[lgc::Builder::ImageAddressIdxCoordinate];

  // For a fragment fetch, there is an extra operand for the fragment id, which
  // we must supply as an extra coordinate.
  Value *fragId = transValue(bii->getOpValue(2), bb->getParent(), bb);
  Value *newCoord = PoisonValue::get(FixedVectorType::get(getBuilder()->getInt32Ty(), 3 + imageInfo.desc->Arrayed));
  for (unsigned i = 0; i != 2 + imageInfo.desc->Arrayed; ++i) {
    newCoord = getBuilder()->CreateInsertElement(newCoord, getBuilder()->CreateExtractElement(coord, i), i);
  }
  coord = getBuilder()->CreateInsertElement(newCoord, fragId, 2 + imageInfo.desc->Arrayed);

  // Get the return type for the Builder method.
  Type *resultTy = transType(bii->getType());

  // Create the image load.
  return getBuilder()->CreateImageLoad(resultTy, imageInfo.dim, imageInfo.flags, imageInfo.imageDesc, coord, nullptr);
}

// =============================================================================
// Translate OpFragmentMaskFetchAMD to LLVM IR
Value *SPIRVToLLVM::transSPIRVFragmentMaskFetchFromInst(SPIRVInstruction *bi, BasicBlock *bb) {

  if (getPipelineOptions()->shadowDescriptorTableUsage != Vkgc::ShadowDescriptorTableUsage::Disable) {
    // Get image type descriptor and fmask descriptor.
    ExtractedImageInfo imageInfo = {bb};
    auto bii = static_cast<SPIRVInstTemplateBase *>(bi);
    getImageDesc(bii->getOpValue(0), &imageInfo);

    assert(imageInfo.desc->Dim == Dim2D || imageInfo.desc->Dim == DimSubpassData);
    imageInfo.dim = !imageInfo.desc->Arrayed ? lgc::Builder::Dim2D : lgc::Builder::Dim3D;

    // Set up address arguments.
    Value *coord = transValue(bii->getOpValue(1), bb->getParent(), bb);

    // Handle fetch/read/write/atomic aspects of coordinate. (This converts to
    // signed i32 and adds on the FragCoord if DimSubpassData.)
    Value *addr[lgc::Builder::ImageAddressCount] = {};
    addr[lgc::Builder::ImageAddressIdxCoordinate] = coord;
    handleImageFetchReadWriteCoord(bi, &imageInfo, addr,
                                   /*EnableMultiView=*/false);
    coord = addr[lgc::Builder::ImageAddressIdxCoordinate];

    // Get the return type for the Builder method. It returns v4i32, then we
    // extract just the R channel.
    Type *resultTy = FixedVectorType::get(transType(bi->getType()), 4);

    // Create the image load.
    Value *result =
        getBuilder()->CreateImageLoad(resultTy, imageInfo.dim, imageInfo.flags, imageInfo.fmaskDesc, coord, nullptr);
    return getBuilder()->CreateExtractElement(result, uint64_t(0));
  }

  // Fmask is unavailable. This is a no-op for cases where the shadow descriptor table cannot be used
  // and fmask descriptors aren't present.

  // Output independent fragment pointers for each sample. Each 4 bits correspond to each sample index.
  // Always use 8 fragment pointers, which is the max fragment count where fmask is actually supported.
  static constexpr unsigned UncompressedFmaskValue = 0x76543210;
  return getBuilder()->getInt32(UncompressedFmaskValue);
}

// =============================================================================
// Translate SPIR-V image atomic operations to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageAtomicOpFromInst(SPIRVInstruction *bi, BasicBlock *bb) {
  // Parse the operands.
  unsigned opndIdx = 0;
  auto bit = static_cast<SPIRVInstTemplateBase *>(bi);
  auto pointerBi = static_cast<SPIRVImageTexelPointer *>(bit->getOpValue(opndIdx++));
  assert(pointerBi->getOpCode() == OpImageTexelPointer);
  unsigned scope = static_cast<SPIRVConstant *>(bit->getOpValue(opndIdx++))->getZExtIntValue();
  unsigned semantics = static_cast<SPIRVConstant *>(bit->getOpValue(opndIdx++))->getZExtIntValue();
  if (bit->getOpCode() == OpAtomicCompareExchange) {
    // Ignore unequal memory semantics
    ++opndIdx;
  }
  Value *inputData = nullptr;
  if (bit->getOpCode() != OpAtomicLoad && bit->getOpCode() != OpAtomicIIncrement &&
      bit->getOpCode() != OpAtomicIDecrement)
    inputData = transValue(bit->getOpValue(opndIdx++), bb->getParent(), bb);
  Value *comparator = nullptr;
  if (bit->getOpCode() == OpAtomicCompareExchange)
    comparator = transValue(bit->getOpValue(opndIdx++), bb->getParent(), bb);

  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo imageInfo = {bb};
  getImageDesc(pointerBi, &imageInfo);

  // Set up address arguments.
  Value *coord = transValue(pointerBi->getCoordinate(), bb->getParent(), bb);
  Value *sampleNum = transValue(pointerBi->getSample(), bb->getParent(), bb);

  // For a multi-sampled image, put the sample ID on the end.
  if (imageInfo.desc->MS) {
    sampleNum = getBuilder()->CreateInsertElement(PoisonValue::get(coord->getType()), sampleNum, uint64_t(0));
    SmallVector<int, 4> idxs;
    idxs.push_back(0);
    idxs.push_back(1);
    if (imageInfo.desc->Arrayed)
      idxs.push_back(2);
    idxs.push_back(cast<FixedVectorType>(coord->getType())->getNumElements());
    coord = getBuilder()->CreateShuffleVector(coord, sampleNum, idxs);
  }

  // Handle fetch/read/write/atomic aspects of coordinate. (This separates the
  // cube face and ID.)
  Value *addr[lgc::Builder::ImageAddressCount] = {};
  addr[lgc::Builder::ImageAddressIdxCoordinate] = coord;
  handleImageFetchReadWriteCoord(bi, &imageInfo, addr);
  coord = addr[lgc::Builder::ImageAddressIdxCoordinate];

  // Determine the atomic ordering.
  AtomicOrdering ordering = AtomicOrdering::NotAtomic;
  if (scope != ScopeInvocation) {
    if (semantics & MemorySemanticsSequentiallyConsistentMask)
      ordering = AtomicOrdering::SequentiallyConsistent;
    else if (semantics & MemorySemanticsAcquireReleaseMask)
      ordering = AtomicOrdering::AcquireRelease;
    else if (semantics & MemorySemanticsAcquireMask)
      ordering = AtomicOrdering::Acquire;
    else if (semantics & MemorySemanticsReleaseMask)
      ordering = AtomicOrdering::Release;

    if (ordering != AtomicOrdering::NotAtomic) {
      // Upgrade the ordering if we need to make it available or visible
      if (semantics & (MemorySemanticsMakeAvailableKHRMask | MemorySemanticsMakeVisibleKHRMask))
        ordering = AtomicOrdering::SequentiallyConsistent;
    }
  }

  // Create the image atomic op.
  unsigned atomicOp = 0;
  Value *result = nullptr;
  switch (bi->getOpCode()) {
  case OpAtomicCompareExchange:
    result = getBuilder()->CreateImageAtomicCompareSwap(imageInfo.dim, imageInfo.flags, ordering, imageInfo.imageDesc,
                                                        coord, inputData, comparator);
    break;

  case OpAtomicStore:
  case OpAtomicExchange:
    atomicOp = lgc::Builder::ImageAtomicSwap;
    break;
  case OpAtomicLoad:
    atomicOp = lgc::Builder::ImageAtomicAdd;
    inputData = getBuilder()->getIntN(bit->getType()->getBitWidth(), 0);
    break;
  case OpAtomicIIncrement:
    atomicOp = lgc::Builder::ImageAtomicAdd;
    inputData = getBuilder()->getIntN(bit->getType()->getBitWidth(), 1);
    break;
  case OpAtomicIDecrement:
    atomicOp = lgc::Builder::ImageAtomicSub;
    inputData = getBuilder()->getIntN(bit->getType()->getBitWidth(), 1);
    break;
  case OpAtomicIAdd:
    atomicOp = lgc::Builder::ImageAtomicAdd;
    break;
  case OpAtomicISub:
    atomicOp = lgc::Builder::ImageAtomicSub;
    break;
  case OpAtomicSMin:
    atomicOp = lgc::Builder::ImageAtomicSMin;
    break;
  case OpAtomicUMin:
    atomicOp = lgc::Builder::ImageAtomicUMin;
    break;
  case OpAtomicSMax:
    atomicOp = lgc::Builder::ImageAtomicSMax;
    break;
  case OpAtomicUMax:
    atomicOp = lgc::Builder::ImageAtomicUMax;
    break;
  case OpAtomicAnd:
    atomicOp = lgc::Builder::ImageAtomicAnd;
    break;
  case OpAtomicOr:
    atomicOp = lgc::Builder::ImageAtomicOr;
    break;
  case OpAtomicXor:
    atomicOp = lgc::Builder::ImageAtomicXor;
    break;
  case OpAtomicFMinEXT:
    atomicOp = lgc::Builder::ImageAtomicFMin;
    break;
  case OpAtomicFMaxEXT:
    atomicOp = lgc::Builder::ImageAtomicFMax;
    break;
  case OpAtomicFAddEXT:
    atomicOp = lgc::Builder::ImageAtomicFAdd;
    break;
  default:
    llvm_unreachable("Unknown image atomic op");
    break;
  }

  if (!result) {
    result = getBuilder()->CreateImageAtomic(atomicOp, imageInfo.dim, imageInfo.flags, ordering, imageInfo.imageDesc,
                                             coord, inputData);
  }
  if (bi->getOpCode() == OpAtomicLoad && bi->getType()->isTypeFloat())
    result = getBuilder()->CreateBitCast(result, transFPType(bi->getType()));

  return result;
}

// =============================================================================
// Helper function for handling converting sampler select ladder
//
// @param result : [in] The result from non-converting sampler path.
// @param convertingSamplerIdx : [in] The index of converting sampler that will be used in createImageOp.
// @param createImageOp : [in] A function that accepts converting sampler and returns the same type as result.
Value *SPIRVToLLVM::ConvertingSamplerSelectLadderHelper(Value *result, Value *convertingSamplerIdx,
                                                        const std::function<Value *(Value *)> &createImageOp) {
  // We have converting samplers. We need to create a converting image sample for each possible one, and
  // select the one we want with a select ladder. In any sensible case, the converting sampler index is
  // statically determinable by later optimizations, and all but the correct image sample get optimized away.
  // The converting sampler index is a 1-based index into all the converting sampler values we have. For example,
  // if m_convertingSamplers has two entries, the first with an array of 3 samplers (24 ints) and the second with an
  // array of 5 samplers (40 ints), then the first entry's 3 samplers are referred to as 1,2,3, and the second
  // entry's 5 samplers are referred to as 4,5,6,7,8.
  int thisConvertingSamplerIdx = 1;
  for (const ConvertingSampler &convertingSampler : m_convertingSamplers) {
    unsigned arraySize = convertingSampler.values.size() / ConvertingSamplerDwordCount;
    for (unsigned idx = 0; idx != arraySize; ++idx) {
      // We want to do a converting image sample for this sampler value. First get the sampler value.
      SmallVector<Constant *, ConvertingSamplerDwordCount> samplerInts;
      for (unsigned component = 0; component != ConvertingSamplerDwordCount; ++component) {
        samplerInts.push_back(
            getBuilder()->getInt32(convertingSampler.values[idx * ConvertingSamplerDwordCount + component]));
      }
      Value *thisResult = createImageOp(ConstantVector::get(samplerInts));
      // Add to select ladder.
      Value *selector =
          getBuilder()->CreateICmpEQ(convertingSamplerIdx, getBuilder()->getInt32(thisConvertingSamplerIdx));
      result = getBuilder()->CreateSelect(selector, thisResult, result);
      ++thisConvertingSamplerIdx;
    }
  }
  return result;
}

// =============================================================================
// Translate image sample to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageSampleFromInst(SPIRVInstruction *bi, BasicBlock *bb) {
  // Get image type descriptor and load resource and sampler descriptors.
  ExtractedImageInfo imageInfo = {bb};
  auto bii = static_cast<SPIRVImageInstBase *>(bi);
  getImageDesc(bii->getOpValue(0), &imageInfo);

  // Determine the return type we want from the builder call. For a sparse
  // sample/gather, the struct is {texel,TFE} in the builder call (to reflect
  // the hardware), but {TFE,texel} in SPIR-V.
  Type *origResultTy = transType(bii->getType());
  Type *resultTy = origResultTy;
  if (auto structResultTy = dyn_cast<StructType>(resultTy)) {
    resultTy = StructType::get(getBuilder()->getContext(),
                               {structResultTy->getElementType(1), structResultTy->getElementType(0)});
  }

  // Set up address arguments.
  unsigned opndIdx = 1;
  Value *addr[lgc::Builder::ImageAddressCount] = {};
  addr[lgc::Builder::ImageAddressIdxCoordinate] = transValue(bii->getOpValue(opndIdx++), bb->getParent(), bb);

  switch (unsigned(bii->getOpCode())) {
  case OpImageSampleDrefImplicitLod:
  case OpImageSampleDrefExplicitLod:
  case OpImageSampleProjDrefImplicitLod:
  case OpImageSampleProjDrefExplicitLod:
  case OpImageSparseSampleDrefImplicitLod:
  case OpImageSparseSampleDrefExplicitLod:
  case OpImageSparseSampleProjDrefImplicitLod:
  case OpImageSparseSampleProjDrefExplicitLod:
    // This instruction has a dref operand.
    addr[lgc::Builder::ImageAddressIdxZCompare] = transValue(bii->getOpValue(opndIdx++), bb->getParent(), bb);
    break;
  default:
    break;
  }

  bool hasProj = false;
  switch (bii->getOpCode()) {
  case OpImageSampleProjImplicitLod:
  case OpImageSampleProjExplicitLod:
  case OpImageSampleProjDrefImplicitLod:
  case OpImageSampleProjDrefExplicitLod:
  case OpImageSparseSampleProjImplicitLod:
  case OpImageSparseSampleProjExplicitLod:
  case OpImageSparseSampleProjDrefImplicitLod:
  case OpImageSparseSampleProjDrefExplicitLod:
    // This instruction has an extra projective coordinate component.
    hasProj = true;
    break;
  default:
    break;
  }

  setupImageAddressOperands(bii, opndIdx, hasProj, addr, &imageInfo, nullptr);

  // First do a normal image sample, extracting the sampler from the {sampler,convertingSamplerIdx} struct.
  Value *samplerDesc = getBuilder()->CreateExtractValue(imageInfo.samplerDesc, 0);
  Value *result =
      getBuilder()->CreateImageSample(resultTy, imageInfo.dim, imageInfo.flags, imageInfo.imageDesc, samplerDesc, addr);

  if (!m_convertingSamplers.empty()) {
    auto createImageSampleConvert = [&](Value *samplerDescIn) -> Value * {
      return getBuilder()->CreateImageSampleConvert(resultTy, imageInfo.dim, imageInfo.flags, imageInfo.imageDescArray,
                                                    samplerDescIn, addr);
    };
    Value *convertingSamplerIdx = getBuilder()->CreateExtractValue(imageInfo.samplerDesc, 1);
    result = ConvertingSamplerSelectLadderHelper(result, convertingSamplerIdx, createImageSampleConvert);
  }

  // For a sparse sample, swap the struct elements back again.
  if (resultTy != origResultTy) {
    Value *swappedResult = getBuilder()->CreateInsertValue(PoisonValue::get(origResultTy),
                                                           getBuilder()->CreateExtractValue(result, 1), unsigned(0));
    swappedResult =
        getBuilder()->CreateInsertValue(swappedResult, getBuilder()->CreateExtractValue(result, unsigned(0)), 1);
    result = swappedResult;
  }
  return result;
}

// =============================================================================
// Translate image gather to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageGatherFromInst(SPIRVInstruction *bi, BasicBlock *bb) {
  // Get image type descriptor and load resource and sampler descriptors.
  ExtractedImageInfo imageInfo = {bb};
  auto bii = static_cast<SPIRVImageInstBase *>(bi);
  getImageDesc(bii->getOpValue(0), &imageInfo);

  // Determine whether the result type of the gather is signed int.
  auto biiTy = bii->getType();
  if (biiTy->isTypeStruct())
    biiTy = static_cast<SPIRVTypeStruct *>(biiTy)->getMemberType(1);
  if (biiTy->isTypeVector())
    biiTy = static_cast<SPIRVTypeVector *>(biiTy)->getComponentType();
  if (biiTy->isTypeInt() && static_cast<SPIRVTypeInt *>(biiTy)->isSigned())
    imageInfo.flags |= lgc::Builder::ImageFlagSignedResult;

  // Determine the return type we want from the builder call. For a sparse
  // sample/gather, the struct is {texel,TFE} in the builder call (to reflect
  // the hardware), but {TFE,texel} in SPIR-V.
  Type *origResultTy = transType(bii->getType());
  Type *resultTy = origResultTy;
  if (auto structResultTy = dyn_cast<StructType>(resultTy)) {
    resultTy = StructType::get(getBuilder()->getContext(),
                               {structResultTy->getElementType(1), structResultTy->getElementType(0)});
  }

  // Set up address arguments.
  unsigned opndIdx = 1;
  Value *addr[lgc::Builder::ImageAddressCount] = {};
  addr[lgc::Builder::ImageAddressIdxCoordinate] = transValue(bii->getOpValue(opndIdx++), bb->getParent(), bb);

  switch (unsigned(bii->getOpCode())) {
  case OpImageGather:
  case OpImageSparseGather:
    // Component for OpImageGather
    addr[lgc::Builder::ImageAddressIdxComponent] = transValue(bii->getOpValue(opndIdx++), bb->getParent(), bb);
    break;
  case OpImageDrefGather:
  case OpImageSparseDrefGather:
    // This instruction has a dref operand.
    addr[lgc::Builder::ImageAddressIdxZCompare] = transValue(bii->getOpValue(opndIdx++), bb->getParent(), bb);
    break;
  default:
    break;
  }

  Value *constOffsets = nullptr;
  setupImageAddressOperands(bii, opndIdx, /*HasProj=*/false, addr, &imageInfo, nullptr);

  if (!addr[lgc::Builder::ImageAddressIdxLod] && !addr[lgc::Builder::ImageAddressIdxLodBias] &&
      !addr[lgc::Builder::ImageAddressIdxDerivativeX]) {
    // A gather with no lod, bias or derivatives is done with lod 0, not
    // implicit lod. Except that does not happen if there is no lod clamp, and
    // this is a fragment shader, and CapabilityImageGatherBiasLodAMD was
    // declared.
    if (addr[lgc::Builder::ImageAddressIdxLodClamp] || !m_enableGatherLodNz) {
      addr[lgc::Builder::ImageAddressIdxLod] = Constant::getNullValue(getBuilder()->getFloatTy());
    }
  }

  // A sampler descriptor is encoded as {desc,convertingSamplerIdx}. Extract the actual sampler.
  Value *samplerDesc = getBuilder()->CreateExtractValue(imageInfo.samplerDesc, 0);

  Value *result = nullptr;
  if (constOffsets) {
    // A gather with non-standard offsets is done as four separate gathers. If
    // it is a sparse gather, we just use the residency code from the last one.
    result = PoisonValue::get(resultTy);
    Value *residency = nullptr;
    if (resultTy != origResultTy)
      result = PoisonValue::get(cast<StructType>(resultTy)->getElementType(0));
    for (int idx = 3; idx >= 0; --idx) {
      addr[lgc::Builder::ImageAddressIdxOffset] = getBuilder()->CreateExtractValue(constOffsets, idx);
      Value *singleResult = getBuilder()->CreateImageGather(resultTy, imageInfo.dim, imageInfo.flags,
                                                            imageInfo.imageDesc, samplerDesc, addr);
      if (resultTy != origResultTy) {
        // Handle sparse.
        residency = getBuilder()->CreateExtractValue(singleResult, 1);
        singleResult = getBuilder()->CreateExtractValue(singleResult, 0);
      }
      result = getBuilder()->CreateInsertElement(result, getBuilder()->CreateExtractElement(singleResult, 3), idx);
    }
    if (resultTy != origResultTy) {
      // Handle sparse.
      result = getBuilder()->CreateInsertValue(PoisonValue::get(origResultTy), result, 1);
      result = getBuilder()->CreateInsertValue(result, residency, 0);
    }
    return result;
  }

  // Create the image gather call.
  result =
      getBuilder()->CreateImageGather(resultTy, imageInfo.dim, imageInfo.flags, imageInfo.imageDesc, samplerDesc, addr);

  // For a sparse gather, swap the struct elements back again.
  if (resultTy != origResultTy) {
    Value *swappedResult = getBuilder()->CreateInsertValue(PoisonValue::get(origResultTy),
                                                           getBuilder()->CreateExtractValue(result, 1), unsigned(0));
    swappedResult =
        getBuilder()->CreateInsertValue(swappedResult, getBuilder()->CreateExtractValue(result, unsigned(0)), 1);
    result = swappedResult;
  }
  return result;
}

// =============================================================================
// Translate image fetch/read to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageFetchReadFromInst(SPIRVInstruction *bi, BasicBlock *bb) {
  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo imageInfo = {bb};
  auto bii = static_cast<SPIRVImageInstBase *>(bi);
  getImageDesc(bii->getOpValue(0), &imageInfo);

  // OpImageFetch fetches from an image whose Sampled operand is 1. Ops that modify an image (like OpImageWrite) work
  // on an image whose Sampled operand is 0 or 2. So if the image is known not to alias any other memory object then
  // nothing can modify it, so we can mark the load as invariant.
  if (bi->getOpCode() == OpImageFetch && (imageInfo.flags & lgc::Builder::ImageFlagNotAliased))
    imageInfo.flags |= lgc::Builder::ImageFlagInvariant;

  // Determine the return type we want from the builder call. For a sparse
  // fetch, the struct is {texel,TFE} in the builder call (to reflect
  // the hardware), but {TFE,texel} in SPIR-V.
  Type *origResultTy = transType(bi->getType());
  Type *resultTy = origResultTy;
  if (auto structResultTy = dyn_cast<StructType>(resultTy)) {
    resultTy = StructType::get(getBuilder()->getContext(),
                               {structResultTy->getElementType(1), structResultTy->getElementType(0)});
  }

  // Set up address arguments.
  Value *addr[lgc::Builder::ImageAddressCount] = {};
  unsigned opndIdx = 1;
  addr[lgc::Builder::ImageAddressIdxCoordinate] = transValue(bii->getOpValue(opndIdx++), bb->getParent(), bb);

  Value *sampleNum = nullptr;
  setupImageAddressOperands(bii, opndIdx, /*HasProj=*/false, addr, &imageInfo, &sampleNum);

  // Handle fetch/read/write aspects of coordinate.
  handleImageFetchReadWriteCoord(bi, &imageInfo, addr);

  Value *result = nullptr;
  Value *coord = addr[lgc::Builder::ImageAddressIdxCoordinate];
  if (sampleNum) {
    if (bi->getOpCode() == OpImageFetch || bi->getOpCode() == OpImageSparseFetch ||
        imageInfo.desc->Dim == DimSubpassData) {
      // This is an OpImageFetch with sample, or an OpImageRead with sample and
      // subpass data dimension. We need to use the fmask variant of the builder
      // method. First we need to get the fmask descriptor.
      result = getBuilder()->CreateImageLoadWithFmask(resultTy, imageInfo.dim, imageInfo.flags, imageInfo.imageDesc,
                                                      imageInfo.fmaskDesc, coord, sampleNum);
    } else {
      // This is an OpImageRead with sample but not subpass data dimension.
      // Append the sample onto the coordinate.
      assert(imageInfo.dim == lgc::Builder::Dim2DMsaa || imageInfo.dim == lgc::Builder::Dim2DArrayMsaa);
      sampleNum = getBuilder()->CreateInsertElement(PoisonValue::get(coord->getType()), sampleNum, uint64_t(0));
      coord = getBuilder()->CreateShuffleVector(
          coord, sampleNum,
          ArrayRef<int>({0, 1, 2, 3}).slice(0, cast<FixedVectorType>(coord->getType())->getNumElements() + 1));
    }
  }

  if (!result) {
    // We did not do the "load with fmask" above. Do the normal image load now.
    Value *lod = addr[lgc::Builder::ImageAddressIdxLod];
    result = getBuilder()->CreateImageLoad(resultTy, imageInfo.dim, imageInfo.flags, imageInfo.imageDesc, coord, lod);
  }

  // For a sparse read/fetch, swap the struct elements back again.
  if (resultTy != origResultTy) {
    Value *swappedResult = getBuilder()->CreateInsertValue(PoisonValue::get(origResultTy),
                                                           getBuilder()->CreateExtractValue(result, 1), unsigned(0));
    swappedResult =
        getBuilder()->CreateInsertValue(swappedResult, getBuilder()->CreateExtractValue(result, unsigned(0)), 1);
    result = swappedResult;
  }
  return result;
}

// =============================================================================
// Translate image write to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageWriteFromInst(SPIRVInstruction *bi, BasicBlock *bb) {
  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo imageInfo = {bb};
  auto bii = static_cast<SPIRVImageInstBase *>(bi);
  getImageDesc(bii->getOpValue(0), &imageInfo);

  // Set up address arguments and get the texel.
  Value *addr[lgc::Builder::ImageAddressCount] = {};
  unsigned opndIdx = 1;
  addr[lgc::Builder::ImageAddressIdxCoordinate] = transValue(bii->getOpValue(opndIdx++), bb->getParent(), bb);
  Value *texel = transValue(bii->getOpValue(opndIdx++), bb->getParent(), bb);

  // Create smaller texel vector, if num texel components > num channels. This
  // ensures that no unused channels are being stored.
  if (auto texelVecType = dyn_cast<FixedVectorType>(texel->getType())) {
    unsigned numChannels = 0;
    if (!m_workaroundStorageImageFormats)
      numChannels = getImageNumChannels(imageInfo.desc);
    if (numChannels > 0 && numChannels < texelVecType->getNumElements()) {
      if (numChannels == 1) {
        texel = getBuilder()->CreateExtractElement(texel, getBuilder()->getInt32(0));
      } else {
        SmallVector<int, 3> channels;
        for (int channel = 0; channel < numChannels; ++channel) {
          channels.push_back(channel);
        }

        texel = getBuilder()->CreateShuffleVector(texel, ArrayRef<int>(channels));
      }
    }
  }

  Value *sampleNum = nullptr;
  setupImageAddressOperands(bii, opndIdx, /*HasProj=*/false, addr, &imageInfo, &sampleNum);

  // Handle fetch/read/write aspects of coordinate.
  handleImageFetchReadWriteCoord(bii, &imageInfo, addr);

  Value *coord = addr[lgc::Builder::ImageAddressIdxCoordinate];
  if (sampleNum) {
    // Append the sample onto the coordinate.
    assert(imageInfo.dim == lgc::Builder::Dim2DMsaa || imageInfo.dim == lgc::Builder::Dim2DArrayMsaa);
    sampleNum = getBuilder()->CreateInsertElement(PoisonValue::get(coord->getType()), sampleNum, uint64_t(0));
    coord = getBuilder()->CreateShuffleVector(
        coord, sampleNum,
        ArrayRef<int>({0, 1, 2, 3}).slice(0, cast<FixedVectorType>(coord->getType())->getNumElements() + 1));
  }

  // Do the image store.
  Value *lod = addr[lgc::Builder::ImageAddressIdxLod];
  return getBuilder()->CreateImageStore(texel, imageInfo.dim, imageInfo.flags, imageInfo.imageDesc, coord, lod);
}

// =============================================================================
// Translate OpImageQueryLevels to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageQueryLevelsFromInst(SPIRVInstruction *bi, BasicBlock *bb) {
  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo imageInfo = {bb};
  auto bii = static_cast<SPIRVImageInstBase *>(bi);
  getImageDesc(bii->getOpValue(0), &imageInfo);

  // Generate the operation.
  return getBuilder()->CreateImageQueryLevels(imageInfo.dim, imageInfo.flags, imageInfo.imageDesc);
}

// =============================================================================
// Translate OpImageQuerySamples to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageQuerySamplesFromInst(SPIRVInstruction *bi, BasicBlock *bb) {
  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo imageInfo = {bb};
  auto bii = static_cast<SPIRVImageInstBase *>(bi);
  getImageDesc(bii->getOpValue(0), &imageInfo);

  // Generate the operation.
  return getBuilder()->CreateImageQuerySamples(imageInfo.dim, imageInfo.flags, imageInfo.imageDesc);
}

// =============================================================================
// Translate OpImageQuerySize/OpImageQuerySizeLod to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageQuerySizeFromInst(SPIRVInstruction *bi, BasicBlock *bb) {
  // Get image type descriptor and load resource descriptor.
  ExtractedImageInfo imageInfo = {bb};
  auto bii = static_cast<SPIRVImageInstBase *>(bi);
  getImageDesc(bii->getOpValue(0), &imageInfo);

  // Generate the operation.
  Value *lod = getBuilder()->getInt32(0);
  if (bii->getOpCode() == OpImageQuerySizeLod)
    lod = transValue(bii->getOpValue(1), bb->getParent(), bb);
  return getBuilder()->CreateImageQuerySize(imageInfo.dim, imageInfo.flags, imageInfo.imageDesc, lod);
}

// =============================================================================
// Translate OpImageQueryLod to LLVM IR
Value *SPIRVToLLVM::transSPIRVImageQueryLodFromInst(SPIRVInstruction *bi, BasicBlock *bb) {
  // Get image type descriptor and load resource and sampler descriptors.
  ExtractedImageInfo imageInfo = {bb};
  auto bii = static_cast<SPIRVImageInstBase *>(bi);
  getImageDesc(bii->getOpValue(0), &imageInfo);

  // A sampler descriptor is encoded as {desc,convertingSamplerIdx}. Extract the actual sampler.
  Value *samplerDesc = getBuilder()->CreateExtractValue(imageInfo.samplerDesc, 0);

  // Generate the operation for normal image get lod.
  Value *coord = transValue(bii->getOpValue(1), bb->getParent(), bb);
  Value *result =
      getBuilder()->CreateImageGetLod(imageInfo.dim, imageInfo.flags, imageInfo.imageDesc, samplerDesc, coord);

  if (!m_convertingSamplers.empty()) {
    auto createImageGetLod = [&](Value *samplerDescIn) -> Value * {
      return getBuilder()->CreateImageGetLod(imageInfo.dim, imageInfo.flags, imageInfo.imageDesc, samplerDescIn, coord);
    };
    Value *convertingSamplerIdx = getBuilder()->CreateExtractValue(imageInfo.samplerDesc, 1);
    result = ConvertingSamplerSelectLadderHelper(result, convertingSamplerIdx, createImageGetLod);
  }

  // NOTE: This is a workaround. When UV width equals 0, the result return 0, but we expect the value is
  // -22.0 to -infinity.
  // Derivative operations are performed on the XY of UV coordinate respectively, if both of result
  // are zero, we will return -FLT_MAX for LOD.
  Value *absDpdx =
      getBuilder()->CreateUnaryIntrinsic(Intrinsic::fabs, getBuilder()->CreateDerivative(coord, false, true));
  Value *absDpdy =
      getBuilder()->CreateUnaryIntrinsic(Intrinsic::fabs, getBuilder()->CreateDerivative(coord, true, true));
  Value *absDdpxPlusDpdy = getBuilder()->CreateFAdd(absDpdx, absDpdy);
  Value *maybeZero = getBuilder()->CreateFCmpOEQ(absDdpxPlusDpdy, ConstantFP::get(coord->getType(), 0.0));

  Value *isZero = getBuilder()->getTrue();
  if (maybeZero->getType()->isVectorTy()) {
    for (unsigned elemIdx = 0; elemIdx < cast<FixedVectorType>(maybeZero->getType())->getNumElements(); elemIdx++) {
      Value *elem = getBuilder()->CreateExtractElement(maybeZero, elemIdx);
      isZero = getBuilder()->CreateAnd(elem, isZero);
    }
  } else {
    isZero = maybeZero;
  }

  auto fixValue = getBuilder()->CreateInsertElement(
      result, ConstantFP::get(m_builder->getFloatTy(), -std::numeric_limits<float>::max()), 1u);
  result = getBuilder()->CreateSelect(isZero, fixValue, result);

  return result;
}

// =============================================================================
// Translate integer dot product to LLVM IR
Value *SPIRVToLLVM::transSPIRVIntegerDotProductFromInst(SPIRVInstruction *bi, BasicBlock *bb) {
  auto bc = static_cast<SPIRVIntegerDotProductInstBase *>(bi);
  Value *vector1 = transValue(bc->getOperand(0), bb->getParent(), bb);
  Value *vector2 = transValue(bc->getOperand(1), bb->getParent(), bb);

  auto oc = bc->getOpCode();
  unsigned flags = 0;
  if (oc == OpSDotKHR || oc == OpSDotAccSatKHR)
    flags = lgc::Builder::FirstVectorSigned | lgc::Builder::SecondVectorSigned;
  else if (oc == OpSUDotKHR || oc == OpSUDotAccSatKHR)
    flags = lgc::Builder::FirstVectorSigned;

  Value *accumulator = nullptr;
  Type *returnTy = transType(bc->getType());
  const bool hasAccumulator = (oc == OpSDotAccSatKHR || oc == OpUDotAccSatKHR || oc == OpSUDotAccSatKHR);
  accumulator = hasAccumulator ? transValue(bc->getOperand(2), bb->getParent(), bb)
                               : getBuilder()->getIntN(returnTy->getScalarSizeInBits(), 0);

  if (vector1->getType()->isIntegerTy(32)) {
    const unsigned operandId = hasAccumulator ? 3 : 2;
    SPIRVConstant *constOp = static_cast<SPIRVConstant *>(bc->getOperand(operandId));
    spv::PackedVectorFormat packedVecFormat = static_cast<spv::PackedVectorFormat>(constOp->getZExtIntValue());
    Type *vecTy = nullptr;
    switch (packedVecFormat) {
    case spv::PackedVectorFormatPackedVectorFormat4x8Bit:
      vecTy = FixedVectorType::get(getBuilder()->getInt8Ty(), 4);
      break;
    default:
      llvm_unreachable("invalid packed vector format");
      break;
    }
    vector1 = getBuilder()->CreateBitCast(vector1, vecTy);
    vector2 = getBuilder()->CreateBitCast(vector2, vecTy);
  }
  Value *scalar = getBuilder()->CreateIntegerDotProduct(vector1, vector2, accumulator, flags);
  return scalar;
}

// =============================================================================

Instruction *SPIRVToLLVM::transSPIRVBuiltinFromInst(SPIRVInstruction *bi, BasicBlock *bb) {
  assert(bb && "Invalid BB");
  return transBuiltinFromInst(getName(bi->getOpCode()), bi, bb);
}

bool SPIRVToLLVM::translate(ExecutionModel entryExecModel, const char *entryName) {
  if (!transAddressingModel())
    return false;

  // Find the targeted entry-point in this translation
  auto entryPoint = m_bm->getEntryPoint(entryExecModel, entryName);

  if (!entryPoint && !m_bm->hasCapability(CapabilityLinkage))
    return false;

  m_entryTarget = (entryPoint != nullptr) ? m_bm->get<SPIRVFunction>(entryPoint->getTargetId()) : nullptr;

  m_execModule = entryExecModel;
  m_fpControlFlags.U32All = 0;
  static_assert(SPIRVTW_8Bit == (8 >> 3), "Unexpected value!");
  static_assert(SPIRVTW_16Bit == (16 >> 3), "Unexpected value!");
  static_assert(SPIRVTW_32Bit == (32 >> 3), "Unexpected value!");
  static_assert(SPIRVTW_64Bit == (64 >> 3), "Unexpected value!");

  bool hasXfbOuts = false;
  if (m_entryTarget) {
    if (auto em = m_entryTarget->getExecutionMode(ExecutionModeDenormPreserve))
      m_fpControlFlags.DenormPreserve = em->getLiterals()[0] >> 3;

    if (auto em = m_entryTarget->getExecutionMode(ExecutionModeDenormFlushToZero))
      m_fpControlFlags.DenormFlushToZero = em->getLiterals()[0] >> 3;

    if (auto em = m_entryTarget->getExecutionMode(ExecutionModeSignedZeroInfNanPreserve))
      m_fpControlFlags.SignedZeroInfNanPreserve = em->getLiterals()[0] >> 3;

    if (auto em = m_entryTarget->getExecutionMode(ExecutionModeRoundingModeRTE))
      m_fpControlFlags.RoundingModeRTE = em->getLiterals()[0] >> 3;

    if (auto em = m_entryTarget->getExecutionMode(ExecutionModeRoundingModeRTZ))
      m_fpControlFlags.RoundingModeRTZ = em->getLiterals()[0] >> 3;

    if (m_execModule >= ExecutionModelVertex && m_execModule <= ExecutionModelGeometry)
      hasXfbOuts = m_entryTarget->getExecutionMode(ExecutionModeXfb) != nullptr;

  } else {
    createLibraryEntryFunc();
  }

  // Determine any denormal overrides to be applied.
  Vkgc::DenormalMode fp32DenormalMode =
      Fp32DenormalModeOpt != Vkgc::DenormalMode::Auto ? Fp32DenormalModeOpt : m_shaderOptions->fp32DenormalMode;

  // Set common shader mode (FP mode and useSubgroupSize) for middle-end.
  CommonShaderMode shaderMode = {};
  if (m_fpControlFlags.RoundingModeRTE & SPIRVTW_16Bit)
    shaderMode.fp16RoundMode = FpRoundMode::Even;
  else if (m_fpControlFlags.RoundingModeRTZ & SPIRVTW_16Bit)
    shaderMode.fp16RoundMode = FpRoundMode::Zero;
  if (m_fpControlFlags.RoundingModeRTE & SPIRVTW_32Bit)
    shaderMode.fp32RoundMode = FpRoundMode::Even;
  else if (m_fpControlFlags.RoundingModeRTZ & SPIRVTW_32Bit)
    shaderMode.fp32RoundMode = FpRoundMode::Zero;
  if (m_fpControlFlags.RoundingModeRTE & SPIRVTW_64Bit)
    shaderMode.fp64RoundMode = FpRoundMode::Even;
  else if (m_fpControlFlags.RoundingModeRTZ & SPIRVTW_64Bit)
    shaderMode.fp64RoundMode = FpRoundMode::Zero;
  if (m_fpControlFlags.DenormPreserve & SPIRVTW_16Bit)
    shaderMode.fp16DenormMode = FpDenormMode::FlushNone;
  else if (m_fpControlFlags.DenormFlushToZero & SPIRVTW_16Bit)
    shaderMode.fp16DenormMode = FpDenormMode::FlushInOut;
  if (m_fpControlFlags.DenormPreserve & SPIRVTW_32Bit || fp32DenormalMode == Vkgc::DenormalMode::Preserve)
    shaderMode.fp32DenormMode = FpDenormMode::FlushNone;
  else if (m_fpControlFlags.DenormFlushToZero & SPIRVTW_32Bit || fp32DenormalMode == Vkgc::DenormalMode::FlushToZero)
    shaderMode.fp32DenormMode = FpDenormMode::FlushInOut;
  if (m_fpControlFlags.DenormPreserve & SPIRVTW_64Bit)
    shaderMode.fp64DenormMode = FpDenormMode::FlushNone;
  else if (m_fpControlFlags.DenormFlushToZero & SPIRVTW_64Bit)
    shaderMode.fp64DenormMode = FpDenormMode::FlushInOut;

  shaderMode.useSubgroupSize = m_moduleUsage->useSubgroupSize;

  // Shader modes contain also data for other modules (subgroup size usage), so query it in the pipeline context.
  auto pipelineContext = (static_cast<Llpc::Context *>(m_context))->getPipelineContext();
  unsigned subgroupSizeUsage = pipelineContext->getSubgroupSizeUsage();
  // NOTE: setCommonShaderMode() supports the graphics and compute stage, does not support raytracing stage
  shaderMode.useSubgroupSize =
      pipelineContext->getPipelineType() == PipelineType::RayTracing ? subgroupSizeUsage : shaderMode.useSubgroupSize;

  if (pipelineContext->getPipelineType() == PipelineType::Graphics && subgroupSizeUsage) {
    for (lgc::ShaderStage stage : lgc::enumRange<lgc::ShaderStage>()) {
      if (subgroupSizeUsage & (1 << stage)) {
        Pipeline::setSubgroupSizeUsage(*m_m, stage, true);
      }
    }
  }

  // Figure out the LGC shader stage so we can use it in the setCommonShaderMode call.
  lgc::ShaderStage shaderStage = lgc::ShaderStageCompute;
  switch (entryExecModel) {
  case ExecutionModelTaskEXT:
    shaderStage = lgc::ShaderStageTask;
    break;
  case ExecutionModelVertex:
    shaderStage = lgc::ShaderStageVertex;
    break;
  case ExecutionModelTessellationControl:
    shaderStage = lgc::ShaderStageTessControl;
    break;
  case ExecutionModelTessellationEvaluation:
    shaderStage = lgc::ShaderStageTessEval;
    break;
  case ExecutionModelGeometry:
    shaderStage = lgc::ShaderStageGeometry;
    break;
  case ExecutionModelMeshEXT:
    shaderStage = lgc::ShaderStageMesh;
    break;
  case ExecutionModelFragment:
    shaderStage = lgc::ShaderStageFragment;
    break;
  default:
    break;
  }

  Pipeline::setCommonShaderMode(*m_m, shaderStage, shaderMode);

  m_enableGatherLodNz =
      m_bm->hasCapability(CapabilityImageGatherBiasLodAMD) && entryExecModel == ExecutionModelFragment;

  // Find the compile unit first since it might be needed during translation of
  // debug intrinsics.
  MDNode *compilationUnit = nullptr;
  for (SPIRVExtInst *EI : m_bm->getDebugInstVec()) {
    // Translate Compile Unit first.
    // It shouldn't be far from the beginning of the vector
    if (EI->getExtOp() == SPIRVDebug::CompilationUnit) {
      compilationUnit = m_dbgTran.transDebugInst(EI);
      // Fixme: there might be more than one Compile Unit.
      break;
    }
  }
  if (!compilationUnit) {
    m_dbgTran.createCompilationUnit();
  }

  for (unsigned i = 0, e = m_bm->getNumConstants(); i != e; ++i) {
    auto bv = m_bm->getConstant(i);
    auto oc = bv->getOpCode();
    if (oc == OpSpecConstant || oc == OpSpecConstantTrue || oc == OpSpecConstantFalse) {
      unsigned specId = SPIRVID_INVALID;
      bv->hasDecorate(DecorationSpecId, 0, &specId);
      // assert(SpecId != SPIRVID_INVALID);
      if (m_specConstMap.find(specId) != m_specConstMap.end()) {
        auto specConstEntry = m_specConstMap.at(specId);
        assert(specConstEntry.DataSize <= sizeof(uint64_t));
        uint64_t data = 0;
        memcpy(&data, specConstEntry.Data, specConstEntry.DataSize);

        if (oc == OpSpecConstant)
          static_cast<SPIRVConstant *>(bv)->setZExtIntValue(data);
        else if (oc == OpSpecConstantTrue)
          static_cast<SPIRVSpecConstantTrue *>(bv)->setBoolValue(data != 0);
        else if (oc == OpSpecConstantFalse)
          static_cast<SPIRVSpecConstantFalse *>(bv)->setBoolValue(data != 0);
        else
          llvm_unreachable("Invalid op code");
      }
    } else if (oc == OpSpecConstantOp) {
      // NOTE: Constant folding is applied to OpSpecConstantOp because at this
      // time, specialization info is obtained and all specialization constants
      // get their own finalized specialization values.

      auto bi = static_cast<SPIRVSpecConstantOp *>(bv);
      bv = createValueFromSpecConstantOp(bi, m_fpControlFlags.RoundingModeRTE);
      bi->mapToConstant(bv);
    }
  }

  for (auto spirvString : m_bm->getStringVec()) {
    transString(spirvString);
  }

  for (unsigned i = 0, e = m_bm->getNumVariables(); i != e; ++i) {
    auto bv = m_bm->getVariable(i);
    if (bv->getStorageClass() != StorageClassFunction)
      transValue(bv, nullptr, nullptr);
  }

  for (unsigned i = 0, e = m_bm->getNumFunctions(); i != e; ++i) {
    auto bf = m_bm->getFunction(i);
    // Non entry-points and targeted entry-point should be translated.
    // Set DLLExport on targeted entry-point so we can find it later.
    if (!m_bm->getEntryPoint(bf->getId()) || bf == m_entryTarget) {
      auto f = transFunction(bf);
      if (bf == m_entryTarget)
        f->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
    }
  }

  if (m_scratchBoundsChecksEnabled) {
    // Insert the scratch out of bounds checks for any feasible memory instruction. The SPIRV to LLVM memop mapping
    // gets filled while translating OpLoads and OpStores.
    for (Function &func : m_m->getFunctionList()) {
      Function *ff = &func;
      for (auto &memOpPair : m_spirvMemopToLlvmMemopMapping[ff]) {
        insertScratchBoundsChecks(memOpPair.first, memOpPair.second, ff);
      }

      m_spirvMemopToLlvmMemopMapping[ff].clear();
    }
  }

  if (!transMetadata())
    return false;

  if (pipelineContext->getPipelineType() == PipelineType::Graphics)
    createXfbMetadata(hasXfbOuts);

  postProcessRowMajorMatrix();
  if (!m_moduleUsage->keepUnusedFunctions)
    eraseUselessFunctions(m_m);
  m_dbgTran.finalize();
  return true;
}

bool SPIRVToLLVM::transAddressingModel() {
  switch (m_bm->getAddressingModel()) {
  case AddressingModelPhysical64:
    m_m->setTargetTriple(SPIR_TARGETTRIPLE64);
    m_m->setDataLayout(SPIR_DATALAYOUT64);
    break;
  case AddressingModelPhysical32:
    m_m->setTargetTriple(SPIR_TARGETTRIPLE32);
    m_m->setDataLayout(SPIR_DATALAYOUT32);
    break;
  case AddressingModelLogical:
  case AddressingModelPhysicalStorageBuffer64EXT:
    break;
  default:
    SPIRVCKRT(0, InvalidAddressingModel, "Actual addressing mode is " + std::to_string(m_bm->getAddressingModel()));
  }
  return true;
}

bool SPIRVToLLVM::transDecoration(SPIRVValue *bv, Value *v) {
  if (!transShaderDecoration(bv, v))
    return false;
  return true;
}

bool SPIRVToLLVM::transNonTemporalMetadata(Instruction *i) {
  Constant *one = ConstantInt::get(Type::getInt32Ty(*m_context), 1);
  MDNode *node = MDNode::get(*m_context, ConstantAsMetadata::get(one));
  i->setMetadata(m_m->getMDKindID("nontemporal"), node);
  return true;
}

bool SPIRVToLLVM::transMetadata() {
  for (unsigned i = 0, e = m_bm->getNumFunctions(); i != e; ++i) {
    SPIRVFunction *bf = m_bm->getFunction(i);
    auto entryPoint = m_bm->getEntryPoint(bf->getId());
    if (entryPoint && bf != m_entryTarget)
      continue; // Ignore those untargeted entry-points

    if (!entryPoint)
      continue;

    SPIRVExecutionModelKind execModel = entryPoint->getExecModel();
    if ((execModel >= ExecutionModelVertex && execModel <= ExecutionModelGLCompute) ||
        execModel == ExecutionModelTaskEXT || execModel == ExecutionModelMeshEXT) {
      // Give the shader modes to middle-end
      if (execModel == ExecutionModelVertex) {
      } else if (execModel == ExecutionModelTessellationControl || execModel == ExecutionModelTessellationEvaluation) {
        TessellationMode tessellationMode = {};

        tessellationMode.vertexSpacing = VertexSpacing::Unknown;
        if (bf->getExecutionMode(ExecutionModeSpacingEqual))
          tessellationMode.vertexSpacing = VertexSpacing::Equal;
        else if (bf->getExecutionMode(ExecutionModeSpacingFractionalEven))
          tessellationMode.vertexSpacing = VertexSpacing::FractionalEven;
        else if (bf->getExecutionMode(ExecutionModeSpacingFractionalOdd))
          tessellationMode.vertexSpacing = VertexSpacing::FractionalOdd;

        tessellationMode.vertexOrder = VertexOrder::Unknown;
        if (bf->getExecutionMode(ExecutionModeVertexOrderCw))
          tessellationMode.vertexOrder = VertexOrder::Cw;
        else if (bf->getExecutionMode(ExecutionModeVertexOrderCcw))
          tessellationMode.vertexOrder = VertexOrder::Ccw;

        tessellationMode.primitiveMode = PrimitiveMode::Unknown;
        if (bf->getExecutionMode(ExecutionModeTriangles))
          tessellationMode.primitiveMode = PrimitiveMode::Triangles;
        else if (bf->getExecutionMode(ExecutionModeQuads))
          tessellationMode.primitiveMode = PrimitiveMode::Quads;
        else if (bf->getExecutionMode(ExecutionModeIsolines))
          tessellationMode.primitiveMode = PrimitiveMode::Isolines;

        if (bf->getExecutionMode(ExecutionModePointMode))
          tessellationMode.pointMode = true;

        if (auto em = bf->getExecutionMode(ExecutionModeOutputVertices))
          tessellationMode.outputVertices = em->getLiterals()[0];

        lgc::ShaderStage shaderStage =
            execModel == ExecutionModelTessellationControl ? lgc::ShaderStageTessControl : lgc::ShaderStageTessEval;
        Pipeline::setTessellationMode(*m_m, shaderStage, tessellationMode);

      } else if (execModel == ExecutionModelGeometry) {
        GeometryShaderMode geometryMode = {};

        if (bf->getExecutionMode(ExecutionModeInputPoints))
          geometryMode.inputPrimitive = InputPrimitives::Points;
        else if (bf->getExecutionMode(ExecutionModeInputLines))
          geometryMode.inputPrimitive = InputPrimitives::Lines;
        else if (bf->getExecutionMode(ExecutionModeInputLinesAdjacency))
          geometryMode.inputPrimitive = InputPrimitives::LinesAdjacency;
        else if (bf->getExecutionMode(ExecutionModeTriangles))
          geometryMode.inputPrimitive = InputPrimitives::Triangles;
        else if (bf->getExecutionMode(ExecutionModeInputTrianglesAdjacency))
          geometryMode.inputPrimitive = InputPrimitives::TrianglesAdjacency;

        if (bf->getExecutionMode(ExecutionModeOutputPoints))
          geometryMode.outputPrimitive = OutputPrimitives::Points;
        else if (bf->getExecutionMode(ExecutionModeOutputLineStrip))
          geometryMode.outputPrimitive = OutputPrimitives::LineStrip;
        else if (bf->getExecutionMode(ExecutionModeOutputTriangleStrip))
          geometryMode.outputPrimitive = OutputPrimitives::TriangleStrip;

        if (auto em = bf->getExecutionMode(ExecutionModeInvocations))
          geometryMode.invocations = em->getLiterals()[0];
        geometryMode.invocations = std::max(1U, geometryMode.invocations);

        if (auto em = bf->getExecutionMode(ExecutionModeOutputVertices))
          geometryMode.outputVertices = em->getLiterals()[0];

        Pipeline::setGeometryShaderMode(*m_m, geometryMode);

      } else if (execModel == ExecutionModelMeshEXT) {
        MeshShaderMode meshMode = {};
        if (bf->getExecutionMode(ExecutionModeOutputPoints))
          meshMode.outputPrimitive = OutputPrimitives::Points;
        else if (bf->getExecutionMode(ExecutionModeOutputLinesEXT))
          meshMode.outputPrimitive = OutputPrimitives::Lines;
        else if (bf->getExecutionMode(ExecutionModeOutputTrianglesEXT))
          meshMode.outputPrimitive = OutputPrimitives::Triangles;

        if (auto em = bf->getExecutionMode(ExecutionModeOutputVertices))
          meshMode.outputVertices = em->getLiterals()[0];

        if (auto em = bf->getExecutionMode(ExecutionModeOutputPrimitivesEXT))
          meshMode.outputPrimitives = em->getLiterals()[0];

        if (auto em = bf->getExecutionMode(ExecutionModeLocalSize)) {
          meshMode.workgroupSizeX = em->getLiterals()[0];
          meshMode.workgroupSizeY = em->getLiterals()[1];
          meshMode.workgroupSizeZ = em->getLiterals()[2];
        } else if ((em = bf->getExecutionMode(ExecutionModeLocalSizeId))) {
          auto workGroupSizeX = static_cast<SPIRVConstant *>(m_bm->getValue(em->getLiterals()[0]));
          meshMode.workgroupSizeX = workGroupSizeX->getZExtIntValue();

          auto workGroupSizeY = static_cast<SPIRVConstant *>(m_bm->getValue(em->getLiterals()[1]));
          meshMode.workgroupSizeY = workGroupSizeY->getZExtIntValue();

          auto workGroupSizeZ = static_cast<SPIRVConstant *>(m_bm->getValue(em->getLiterals()[2]));
          meshMode.workgroupSizeZ = workGroupSizeZ->getZExtIntValue();
        }

        // Traverse the constant list to find gl_WorkGroupSize and use the
        // values to overwrite local sizes
        for (unsigned i = 0, e = m_bm->getNumConstants(); i != e; ++i) {
          auto bv = m_bm->getConstant(i);
          SPIRVWord builtIn = SPIRVID_INVALID;
          if ((bv->getOpCode() == OpSpecConstant || bv->getOpCode() == OpSpecConstantComposite ||
               bv->getOpCode() == OpConstant || bv->getOpCode() == OpConstantComposite) &&
              bv->hasDecorate(DecorationBuiltIn, 0, &builtIn)) {
            if (builtIn == spv::BuiltInWorkgroupSize) {
              // NOTE: Overwrite values of local sizes specified in execution
              // mode if the constant corresponding to gl_WorkGroupSize
              // exists. Take its value since gl_WorkGroupSize could be a
              // specialization constant.
              auto workGroupSize = static_cast<SPIRVSpecConstantComposite *>(bv);

              // Declared: const uvec3 gl_WorkGroupSize
              assert(workGroupSize->getElements().size() == 3);
              auto workGroupSizeX = static_cast<SPIRVConstant *>(workGroupSize->getElements()[0]);
              auto workGroupSizeY = static_cast<SPIRVConstant *>(workGroupSize->getElements()[1]);
              auto workGroupSizeZ = static_cast<SPIRVConstant *>(workGroupSize->getElements()[2]);

              meshMode.workgroupSizeX = workGroupSizeX->getZExtIntValue();
              meshMode.workgroupSizeY = workGroupSizeY->getZExtIntValue();
              meshMode.workgroupSizeZ = workGroupSizeZ->getZExtIntValue();

              break;
            }
          }
        }

        unsigned overrideShaderGroupSizeX = m_shaderOptions->overrideShaderThreadGroupSizeX;
        unsigned overrideShaderGroupSizeY = m_shaderOptions->overrideShaderThreadGroupSizeY;
        unsigned overrideShaderGroupSizeZ = m_shaderOptions->overrideShaderThreadGroupSizeZ;

        if (overrideShaderGroupSizeX != 0 || overrideShaderGroupSizeY != 0 || overrideShaderGroupSizeZ != 0) {
          meshMode.workgroupSizeX = overrideShaderGroupSizeX;
          meshMode.workgroupSizeY = overrideShaderGroupSizeY;
          meshMode.workgroupSizeZ = overrideShaderGroupSizeZ;
        }

        Pipeline::setMeshShaderMode(*m_m, meshMode);
      } else if (execModel == ExecutionModelFragment) {
        FragmentShaderMode fragmentMode = {};

        if (bf->getExecutionMode(ExecutionModePixelCenterInteger))
          fragmentMode.pixelCenterInteger = true;

        if (bf->getExecutionMode(ExecutionModeEarlyFragmentTests))
          fragmentMode.earlyFragmentTests = true;

        if (bf->getExecutionMode(ExecutionModePostDepthCoverage))
          fragmentMode.postDepthCoverage = true;

        if (bf->getExecutionMode(ExecutionModeDepthLess))
          fragmentMode.conservativeDepth = ConservativeDepth::LessEqual;
        else if (bf->getExecutionMode(ExecutionModeDepthGreater))
          fragmentMode.conservativeDepth = ConservativeDepth::GreaterEqual;

        if (bf->getExecutionMode(ExecutionModeEarlyAndLateFragmentTestsAMD)) {
          fragmentMode.earlyAndLatFragmentTests = true;
          if (bf->getExecutionMode(ExecutionModeStencilRefReplacingEXT)) {
            if (bf->getExecutionMode(ExecutionModeStencilRefGreaterFrontAMD))
              fragmentMode.conservativeStencilFront = ConservativeDepth::GreaterEqual;
            else if (bf->getExecutionMode(ExecutionModeStencilRefLessFrontAMD))
              fragmentMode.conservativeStencilFront = ConservativeDepth::LessEqual;

            if (bf->getExecutionMode(ExecutionModeStencilRefGreaterBackAMD))
              fragmentMode.conservativeStencilBack = ConservativeDepth::GreaterEqual;
            else if (bf->getExecutionMode(ExecutionModeStencilRefLessBackAMD))
              fragmentMode.conservativeStencilBack = ConservativeDepth::LessEqual;
          } else if (bf->getExecutionMode(ExecutionModeDepthReplacing)) {
            assert(bf->getExecutionMode(ExecutionModeDepthUnchanged) ||
                   bf->getExecutionMode(ExecutionModeDepthGreater) || bf->getExecutionMode(ExecutionModeDepthLess));
          } else
            fragmentMode.earlyFragmentTests = true;
        }

        Pipeline::setFragmentShaderMode(*m_m, fragmentMode);

      } else if (execModel == ExecutionModelGLCompute || execModel == ExecutionModelTaskEXT) {
        unsigned workgroupSizeX = 0;
        unsigned workgroupSizeY = 0;
        unsigned workgroupSizeZ = 0;

        if (auto em = bf->getExecutionMode(ExecutionModeLocalSize)) {
          workgroupSizeX = em->getLiterals()[0];
          workgroupSizeY = em->getLiterals()[1];
          workgroupSizeZ = em->getLiterals()[2];
        } else if ((em = bf->getExecutionMode(ExecutionModeLocalSizeId))) {
          workgroupSizeX = static_cast<SPIRVConstant *>(m_bm->getValue(em->getLiterals()[0]))->getZExtIntValue();
          workgroupSizeY = static_cast<SPIRVConstant *>(m_bm->getValue(em->getLiterals()[1]))->getZExtIntValue();
          workgroupSizeZ = static_cast<SPIRVConstant *>(m_bm->getValue(em->getLiterals()[2]))->getZExtIntValue();
        }
        // Traverse the constant list to find gl_WorkGroupSize and use the
        // values to overwrite local sizes
        for (unsigned i = 0, e = m_bm->getNumConstants(); i != e; ++i) {
          auto bv = m_bm->getConstant(i);
          SPIRVWord builtIn = SPIRVID_INVALID;
          if ((bv->getOpCode() == OpSpecConstant || bv->getOpCode() == OpSpecConstantComposite ||
               bv->getOpCode() == OpConstant || bv->getOpCode() == OpConstantComposite) &&
              bv->hasDecorate(DecorationBuiltIn, 0, &builtIn)) {
            if (builtIn == spv::BuiltInWorkgroupSize) {
              // NOTE: Overwrite values of local sizes specified in execution
              // mode if the constant corresponding to gl_WorkGroupSize
              // exists. Take its value since gl_WorkGroupSize could be a
              // specialization constant.
              auto workGroupSize = static_cast<SPIRVSpecConstantComposite *>(bv);

              // Declared: const uvec3 gl_WorkGroupSize
              assert(workGroupSize->getElements().size() == 3);
              workgroupSizeX = static_cast<SPIRVConstant *>(workGroupSize->getElements()[0])->getZExtIntValue();
              workgroupSizeY = static_cast<SPIRVConstant *>(workGroupSize->getElements()[1])->getZExtIntValue();
              workgroupSizeZ = static_cast<SPIRVConstant *>(workGroupSize->getElements()[2])->getZExtIntValue();

              break;
            }
          }
        }

        ComputeShaderMode computeMode = {};

        unsigned overrideShaderGroupSizeX = m_shaderOptions->overrideShaderThreadGroupSizeX;
        unsigned overrideShaderGroupSizeY = m_shaderOptions->overrideShaderThreadGroupSizeY;
        unsigned overrideShaderGroupSizeZ = m_shaderOptions->overrideShaderThreadGroupSizeZ;

        unsigned overrideThreadGroupSizeX = getPipelineOptions()->overrideThreadGroupSizeX;
        unsigned overrideThreadGroupSizeY = getPipelineOptions()->overrideThreadGroupSizeY;
        unsigned overrideThreadGroupSizeZ = getPipelineOptions()->overrideThreadGroupSizeZ;
        if (overrideShaderGroupSizeX != 0 || overrideShaderGroupSizeY != 0 || overrideShaderGroupSizeZ != 0) {
          computeMode.workgroupSizeX = overrideShaderGroupSizeX;
          computeMode.workgroupSizeY = overrideShaderGroupSizeY;
          computeMode.workgroupSizeZ = overrideShaderGroupSizeZ;
        } else if (overrideThreadGroupSizeX != 0 || overrideThreadGroupSizeY != 0 || overrideThreadGroupSizeZ != 0) {
          computeMode.workgroupSizeX = overrideThreadGroupSizeX;
          computeMode.workgroupSizeY = overrideThreadGroupSizeY;
          computeMode.workgroupSizeZ = overrideThreadGroupSizeZ;
        } else if (m_moduleUsage->rayQueryLibrary) {
          // Ray Query library Shader can not overwrite the compute mode settings
        } else {
          computeMode.workgroupSizeX = workgroupSizeX;
          computeMode.workgroupSizeY = workgroupSizeY;
          computeMode.workgroupSizeZ = workgroupSizeZ;
        }
        Pipeline::setComputeShaderMode(*m_m, computeMode);

      } else
        llvm_unreachable("Invalid execution model");

      // Skip the following processing for GLSL
      continue;
    }
  }
  return true;
}

bool SPIRVToLLVM::checkContains64BitType(SPIRVType *bt) {
  if (bt->isTypeScalar())
    return bt->getBitWidth() == 64;
  if (bt->isTypeVector())
    return checkContains64BitType(bt->getVectorComponentType());
  if (bt->isTypeMatrix())
    return checkContains64BitType(bt->getMatrixColumnType());
  if (bt->isTypeArray())
    return checkContains64BitType(bt->getArrayElementType());
  if (bt->isTypeStruct()) {
    bool contains64BitType = false;
    auto memberCount = bt->getStructMemberCount();
    for (auto memberIdx = 0; memberIdx < memberCount; ++memberIdx) {
      auto memberTy = bt->getStructMemberType(memberIdx);
      contains64BitType = contains64BitType || checkContains64BitType(memberTy);
    }
    return contains64BitType;
  }
  llvm_unreachable("Invalid type");
  return false;
}

bool SPIRVToLLVM::transShaderDecoration(SPIRVValue *bv, Value *v) {
  auto gv = dyn_cast<GlobalVariable>(v);
  // Some SPIR-V instructions (e.g. OpAccessChain) can become no-ops in LLVM IR,
  // so we must explicitly check both the SPIR-V opcode and the LLVM type.
  if (gv && bv->getOpCode() == OpVariable) {
    auto as = gv->getType()->getAddressSpace();

    auto spvTy = bv->getType()->getPointerElementType();
    while (spvTy->getOpCode() == OpTypeArray || spvTy->getOpCode() == OpTypeRuntimeArray)
      spvTy = spvTy->getArrayElementType();
    if (isAccelerationStructureType(spvTy)) {
      // NOTE: For acceleration structure, we use SPIRAS_Constant address space, but to apply decoration correctly,
      // we assume it is SPIRAS_Uniform here.
      assert(as == SPIRAS_Constant);
      as = SPIRAS_Uniform;
    }

    if (as == SPIRAS_Input || as == SPIRAS_Output) {
      // Translate decorations of inputs and outputs

      // Build input/output metadata
      ShaderInOutDecorate inOutDec = {};
      inOutDec.Value.U32All = 0;
      inOutDec.IsBuiltIn = false;
      inOutDec.Interp.Mode = InterpModeSmooth;
      inOutDec.Interp.Loc = InterpLocCenter;
      inOutDec.PerPatch = false;
      inOutDec.PerPrimitive = false;
      inOutDec.StreamId = 0;
      inOutDec.Index = 0;
      inOutDec.IsXfb = false;
      inOutDec.XfbBuffer = 0;
      inOutDec.XfbStride = 0;
      inOutDec.XfbOffset = 0;
      inOutDec.contains64BitType = false;

      SPIRVWord loc = SPIRVID_INVALID;
      if (bv->hasDecorate(DecorationLocation, 0, &loc)) {
        inOutDec.IsBuiltIn = false;
        inOutDec.Value.Loc = loc;
      }

      SPIRVWord index = SPIRVID_INVALID;
      if (bv->hasDecorate(DecorationIndex, 0, &index))
        inOutDec.Index = index;

      SPIRVWord builtIn = SPIRVID_INVALID;
      if (bv->hasDecorate(DecorationBuiltIn, 0, &builtIn)) {
        inOutDec.IsBuiltIn = true;
        inOutDec.Value.BuiltIn = builtIn;

        // NOTE: According to the vulkan spec, sample shading is enabled if the fragment shader's entry point
        // interface includes input variables decorated with a BuiltIn of SampleId or SamplePosition built-ins.
        // If gl_sampleId or gl_samplePosition is declared but not used in a fragment shader, it will be removed
        // in lower stage, in those cases, we need to add metadata and use it later.
        if ((m_execModule == ExecutionModelFragment) &&
            (builtIn == spv::BuiltInSampleId || builtIn == spv::BuiltInSamplePosition)) {
          auto nameMeta = m_m->getNamedMetadata(lgc::SampleShadingMetaName);
          if (!nameMeta) {
            nameMeta = m_m->getOrInsertNamedMetadata(lgc::SampleShadingMetaName);
            nameMeta->addOperand(MDNode::get(*m_context, MDString::get(*m_context, lgc::SampleShadingMetaName)));
          }
        }

        Llpc::Context *llpcContext = static_cast<Llpc::Context *>(m_context);
        llpcContext->getPipelineContext()->collectBuiltIn(builtIn);
      } else if (bv->getName() == "gl_in" || bv->getName() == "gl_out" || bv->getName() == "gl_MeshVerticesEXT") {
        inOutDec.IsBuiltIn = true;
        inOutDec.Value.BuiltIn = BuiltInPerVertex;
      } else if (bv->getName() == "gl_MeshPrimitivesEXT") {
        inOutDec.IsBuiltIn = true;
        inOutDec.Value.BuiltIn = BuiltInPerPrimitive;
      }

      SPIRVWord component = SPIRVID_INVALID;
      if (bv->hasDecorate(DecorationComponent, 0, &component))
        inOutDec.Component = component;

      if (bv->hasDecorate(DecorationPerVertexKHR)) {
        inOutDec.Interp.Mode = InterpModeCustom;
        inOutDec.Interp.Loc = InterpLocCustom;
        inOutDec.PerVertexDimension = true;
      }

      if (bv->hasDecorate(DecorationFlat))
        inOutDec.Interp.Mode = InterpModeFlat;

      if (bv->hasDecorate(DecorationNoPerspective))
        inOutDec.Interp.Mode = InterpModeNoPersp;

      if (bv->hasDecorate(DecorationCentroid))
        inOutDec.Interp.Loc = InterpLocCentroid;

      if (bv->hasDecorate(DecorationSample))
        inOutDec.Interp.Loc = InterpLocSample;

      if (bv->hasDecorate(DecorationExplicitInterpAMD)) {
        inOutDec.Interp.Mode = InterpModeCustom;
        inOutDec.Interp.Loc = InterpLocCustom;
      }

      if (bv->hasDecorate(DecorationPatch))
        inOutDec.PerPatch = true;

      if (bv->hasDecorate(DecorationPerPrimitiveEXT)) {
        inOutDec.PerPrimitive = true;
        // Per-primitive inputs always use flat shading
        inOutDec.Interp.Mode = InterpModeFlat;
      }

      SPIRVWord streamId = SPIRVID_INVALID;
      if (bv->hasDecorate(DecorationStream, 0, &streamId))
        inOutDec.StreamId = streamId;

      SPIRVWord xfbBuffer = SPIRVID_INVALID;
      if (bv->hasDecorate(DecorationXfbBuffer, 0, &xfbBuffer))
        inOutDec.XfbBuffer = xfbBuffer;

      SPIRVWord xfbStride = SPIRVID_INVALID;
      if (bv->hasDecorate(DecorationXfbStride, 0, &xfbStride))
        inOutDec.XfbStride = xfbStride;

      SPIRVWord xfbOffset = SPIRVID_INVALID;
      if (bv->hasDecorate(DecorationOffset, 0, &xfbOffset)) {
        // NOTE: Transform feedback is triggered only if "xfb_offset" is specified. And in such case, "xfb_stride"
        // must be specified as well.
        if (inOutDec.XfbStride > 0)
          inOutDec.IsXfb = true;
        inOutDec.XfbOffset = xfbOffset;
      }

      // If dual source blend is dynamically set, need to confirm whether the fragment shader actually uses
      // dual-source blending by checking if there is an output at Location 0. If there isn't any output in
      // index, the poison value will be exported.
      Llpc::Context *llpcContext = static_cast<Llpc::Context *>(m_context);
      if ((llpcContext->getPipelineType() == PipelineType::Graphics) && (m_execModule == spv::ExecutionModelFragment)) {
        auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(llpcContext->getPipelineBuildInfo());
        if (buildInfo->cbState.dualSourceBlendDynamic && (as == SPIRAS_Output) && (inOutDec.Value.Loc == 0) &&
            (inOutDec.Index == 1)) {
          llpcContext->getPipelineContext()->setUseDualSourceBlend(true);
        }
      }

      Type *mdTy = nullptr;
      SPIRVType *bt = bv->getType()->getPointerElementType();
      bool vs64BitsAttribInputSingleLoc = (as == SPIRAS_Input && m_execModule == ExecutionModelVertex &&
                                           getPipelineOptions()->vertex64BitsAttribSingleLoc);
      auto md = buildShaderInOutMetadata(bt, inOutDec, mdTy, vs64BitsAttribInputSingleLoc);

      // Setup input/output metadata
      std::vector<Metadata *> mDs;
      mDs.push_back(ConstantAsMetadata::get(md));
      auto mdNode = MDNode::get(*m_context, mDs);
      gv->addMetadata(gSPIRVMD::InOut, *mdNode);

    } else if (as == SPIRAS_Uniform) {
      // Translate decorations of blocks
      SPIRVType *blockTy = bv->getType()->getPointerElementType();
      // If not task payload, try to remove block array dimensions. Note that task
      // payload doesn't have such dimensions.
      if (bv->getType()->getPointerStorageClass() != StorageClassTaskPayloadWorkgroupEXT) {
        // Remove array dimensions, it is useless for block metadata building
        while (blockTy->isTypeArray())
          blockTy = blockTy->getArrayElementType();
      }

      // Get values of descriptor binding and set based on corresponding
      // decorations
      SPIRVWord binding = SPIRVID_INVALID;
      unsigned descSet = SPIRVID_INVALID;
      bool hasBinding = bv->hasDecorate(DecorationBinding, 0, &binding);
      bool hasDescSet = false;
      hasDescSet = bv->hasDecorate(DecorationDescriptorSet, 0, &descSet);

      // TODO: Currently, set default binding and descriptor to 0. Will be
      // changed later.
      if (!hasBinding)
        binding = 0;
      if (!hasDescSet)
        descSet = 0;

      if ((bv->getType()->getPointerStorageClass() != StorageClassUniformConstant &&
           bv->getType()->getPointerStorageClass() != StorageClassTaskPayloadWorkgroupEXT) ||
          isAccelerationStructureType(blockTy)) {
        assert(blockTy->isTypeStruct() || blockTy->isTypeAccelerationStructureKHR() ||
               bv->getType()->getPointerStorageClass() == StorageClassAtomicCounter);

        if (getPipelineOptions()->replaceSetWithResourceType) {
          bool hasBlock = blockTy->hasDecorate(DecorationBlock);
          bool hasBufferBlock = blockTy->hasDecorate(DecorationBufferBlock);

          if (bv->getType()->getPointerStorageClass() == StorageClassStorageBuffer) {
            descSet = PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorBuffer);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 63
          } else if (bv->getType()->getPointerStorageClass() == StorageClassAtomicCounter) {
            descSet = PipelineContext::getGlResourceNodeSetFromType(ResourceMappingNodeType::DescriptorAtomicCounter);
#endif
          } else {
            if (hasBlock && !hasBufferBlock) {
              descSet =
                  PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorConstBuffer);
            } else if (!hasBlock && hasBufferBlock) {
              descSet = PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorBuffer);
            }
          }
        }

        // Setup resource metadata
        auto int32Ty = Type::getInt32Ty(*m_context);
        std::vector<Metadata *> resMDs;
        resMDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, descSet)));
        resMDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, binding)));
        auto resMdNode = MDNode::get(*m_context, resMDs);
        gv->addMetadata(gSPIRVMD::Resource, *resMdNode);

        // Build block metadata
        const bool isUniformBlock = bv->getType()->getPointerStorageClass() != StorageClassStorageBuffer &&
                                    blockTy->hasDecorate(DecorationBlock);

        ShaderBlockDecorate blockDec = {};
        SPIRVWord atomicCounterOffset = SPIRVID_INVALID;
        if (bv->hasDecorate(DecorationOffset, 0, &atomicCounterOffset)) {
          blockDec.Offset = atomicCounterOffset;
        }

        blockDec.NonWritable = isUniformBlock;
        Type *blockMdTy = nullptr;
        auto blockMd = buildShaderBlockMetadata(blockTy, blockDec, blockMdTy, bv->getType()->getPointerStorageClass());

        std::vector<Metadata *> blockMDs;
        blockMDs.push_back(ConstantAsMetadata::get(blockMd));
        auto blockMdNode = MDNode::get(*m_context, blockMDs);
        if (bv->getType()->getPointerStorageClass() == StorageClassAtomicCounter) {
          gv->addMetadata(gSPIRVMD::AtomicCounter, *blockMdNode);
        } else {
          gv->addMetadata(gSPIRVMD::Block, *blockMdNode);
        }
      } else if (bv->getType()->getPointerStorageClass() == StorageClassTaskPayloadWorkgroupEXT) {
        // Setup metadata for task payload
        gv->addMetadata(gSPIRVMD::TaskPayload, *MDNode::get(*m_context, {}));
      } else {
        // Setup metadata for uniform constant.
        auto int32Ty = Type::getInt32Ty(*m_context);
        std::vector<Metadata *> mDs;

        SPIRVWord loc = SPIRVID_INVALID;
        bv->hasDecorate(DecorationLocation, 0, &loc);
        assert(loc != SPIRVID_INVALID && "Default uniform constant should get location\n");

        // Get the offset in default uniform constant buffer from location
        auto locationFound = getUniformConstantEntryByLocation(static_cast<Llpc::Context *>(m_context),
                                                               convertToShaderStage(m_execModule), loc);
        unsigned offset = locationFound == nullptr ? 0 : locationFound->offset;

        mDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, Vkgc::InternalDescriptorSetId)));
        mDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, Vkgc::ConstantBuffer0Binding)));
        mDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, offset)));
        mDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, loc)));
        auto mdNode = MDNode::get(*m_context, mDs);
        gv->addMetadata(gSPIRVMD::UniformConstant, *mdNode);
      }
    } else if (bv->getType()->isTypePointer() && bv->getType()->getPointerStorageClass() == StorageClassPushConstant) {
      // Translate decorations of push constants

      SPIRVType *pushConstTy = bv->getType()->getPointerElementType();
      assert(pushConstTy->isTypeStruct());

      // Build push constant specific metadata
      unsigned pushConstSize = 0;
      unsigned matrixStride = SPIRVID_INVALID;
      bool isRowMajor = false;
      pushConstSize = calcShaderBlockSize(pushConstTy, pushConstSize, matrixStride, isRowMajor);

      auto int32Ty = Type::getInt32Ty(*m_context);
      std::vector<Metadata *> pushConstMDs;
      pushConstMDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, pushConstSize)));
      auto pushConstMdNode = MDNode::get(*m_context, pushConstMDs);
      gv->addMetadata(gSPIRVMD::PushConst, *pushConstMdNode);

      // Build general block metadata
      ShaderBlockDecorate blockDec = {};
      Type *blockMdTy = nullptr;
      auto blockMd =
          buildShaderBlockMetadata(pushConstTy, blockDec, blockMdTy, bv->getType()->getPointerStorageClass());

      std::vector<Metadata *> blockMDs;
      blockMDs.push_back(ConstantAsMetadata::get(blockMd));
      auto blockMdNode = MDNode::get(*m_context, blockMDs);
      gv->addMetadata(gSPIRVMD::Block, *blockMdNode);
    } else if (as == SPIRAS_Local) {
      if (m_bm->hasCapability(CapabilityWorkgroupMemoryExplicitLayoutKHR)) {
        std::vector<Metadata *> mDs;
        mDs.push_back(ConstantAsMetadata::get(
            ConstantInt::get(Type::getInt32Ty(*m_context), bv->hasDecorate(DecorationAliased))));
        auto mdNode = MDNode::get(*m_context, mDs);
        gv->addMetadata(gSPIRVMD::Lds, *mdNode);
      }
    } else {
      // image/sampler/sampledimage are lowered without GlobalVariable, so we should not arrive here anymore.
      assert(as != SPIRAS_Constant && "We don't have decoration on constant globalvariable now.\n");
    }
  } else {
    bool isNonUniform = bv->hasDecorate(DecorationNonUniformEXT);
    if (isNonUniform && isa<Instruction>(v)) {
      std::vector<Value *> args;
      args.push_back(v);
      auto types = getTypes(args);
      auto voidTy = Type::getVoidTy(*m_context);
      auto bb = cast<Instruction>(v)->getParent();

      // Per-instruction metadata is not safe, LLVM optimizer may remove them,
      // so we choose to add a dummy instruction and remove them when it isn't
      // needed.
      std::string mangledFuncName(gSPIRVMD::NonUniform);
      appendTypeMangling(voidTy, args, mangledFuncName);
      auto f = getOrCreateFunction(m_m, voidTy, types, mangledFuncName);
      if (bb->getTerminator()) {
        // NOTE: For OpCopyObject, we use the operand value directly, which may be in another block that already has a
        // terminator. In this case, insert the call before the terminator.
        assert(bv->getOpCode() == OpCopyObject);
        CallInst::Create(f, args, "", bb->getTerminator());
      } else {
        CallInst::Create(f, args, "", bb);
      }
    }
  }

  return true;
}

// Calculates shader block size
unsigned SPIRVToLLVM::calcShaderBlockSize(SPIRVType *bt, unsigned blockSize, unsigned matrixStride, bool isRowMajor) {
  if (bt->isTypeStruct()) {
    if (bt->getStructMemberCount() == 0)
      blockSize = 0;
    else {
      // Find member with max offset
      unsigned memberIdxWithMaxOffset = 0;
      unsigned maxOffset = 0;
      for (unsigned memberIdx = 0; memberIdx < bt->getStructMemberCount(); ++memberIdx) {
        unsigned offset = 0;
        if (bt->hasMemberDecorate(memberIdx, DecorationOffset, 0, &offset)) {
          if (offset > maxOffset) {
            maxOffset = offset;
            memberIdxWithMaxOffset = memberIdx;
          }
        } else
          llvm_unreachable("Missing offset decoration");
      }

      unsigned memberMatrixStride = matrixStride;
      bt->hasMemberDecorate(memberIdxWithMaxOffset, DecorationMatrixStride, 0, &memberMatrixStride);

      bool isMemberRowMajor = isRowMajor;
      if (bt->hasMemberDecorate(memberIdxWithMaxOffset, DecorationRowMajor))
        isMemberRowMajor = true;
      else if (bt->hasMemberDecorate(memberIdxWithMaxOffset, DecorationColMajor))
        isMemberRowMajor = false;

      SPIRVType *memberTy = bt->getStructMemberType(memberIdxWithMaxOffset);
      blockSize += calcShaderBlockSize(memberTy, maxOffset, memberMatrixStride, isMemberRowMajor);
    }
  } else if (bt->isTypeArray() || bt->isTypeMatrix()) {
    if (bt->isTypeArray()) {
      unsigned arrayStride = 0;
      if (!bt->hasDecorate(DecorationArrayStride, 0, &arrayStride))
        llvm_unreachable("Missing array stride decoration");
      unsigned numElems = bt->getArrayLength();
      blockSize += numElems * arrayStride;
    } else {
      assert(matrixStride != SPIRVID_INVALID);
      unsigned numVectors =
          isRowMajor ? bt->getMatrixColumnType()->getVectorComponentCount() : bt->getMatrixColumnCount();
      blockSize += numVectors * matrixStride;
    }
  } else if (bt->isTypeVector()) {
    unsigned sizeInBytes = bt->getVectorComponentType()->getBitWidth() / 8;
    unsigned numComps = bt->getVectorComponentCount();
    blockSize += sizeInBytes * numComps;
  } else if (bt->isTypeScalar()) {
    unsigned sizeInBytes = bt->getBitWidth() / 8;
    blockSize += sizeInBytes;
  } else if (bt->isTypeForwardPointer()) {
    // Forward pointers in shader blocks are always 64-bit.
    blockSize += 8;
  } else
    llvm_unreachable("Invalid shader block type");

  return blockSize;
}

// Builds shader input/output metadata.
Constant *SPIRVToLLVM::buildShaderInOutMetadata(SPIRVType *bt, ShaderInOutDecorate &inOutDec, Type *&mdTy,
                                                bool vs64BitsAttribInputSingleLoc) {
  SPIRVWord loc = SPIRVID_INVALID;
  if (bt->hasDecorate(DecorationLocation, 0, &loc)) {
    inOutDec.Value.Loc = loc;
    inOutDec.IsBuiltIn = false;
  }

  SPIRVWord index = SPIRVID_INVALID;
  if (bt->hasDecorate(DecorationIndex, 0, &index))
    inOutDec.Index = index;

  SPIRVWord builtIn = SPIRVID_INVALID;
  if (bt->hasDecorate(DecorationBuiltIn, 0, &builtIn)) {
    inOutDec.Value.BuiltIn = builtIn;
    inOutDec.IsBuiltIn = true;
  }

  SPIRVWord component = SPIRVID_INVALID;
  if (bt->hasDecorate(DecorationComponent, 0, &component))
    inOutDec.Component = component;

  if (bt->hasDecorate(DecorationFlat))
    inOutDec.Interp.Mode = InterpModeFlat;

  if (bt->hasDecorate(DecorationNoPerspective))
    inOutDec.Interp.Mode = InterpModeNoPersp;

  if (bt->hasDecorate(DecorationCentroid))
    inOutDec.Interp.Loc = InterpLocCentroid;

  if (bt->hasDecorate(DecorationSample))
    inOutDec.Interp.Loc = InterpLocSample;

  if (bt->hasDecorate(DecorationExplicitInterpAMD)) {
    inOutDec.Interp.Mode = InterpModeCustom;
    inOutDec.Interp.Loc = InterpLocCustom;
  }

  if (bt->hasDecorate(DecorationPatch))
    inOutDec.PerPatch = true;

  if (bt->hasDecorate(DecorationPerPrimitiveEXT)) {
    inOutDec.PerPrimitive = true;
    // Per-primitive inputs always use flat shading
    inOutDec.Interp.Mode = InterpModeFlat;
  }

  SPIRVWord streamId = SPIRVID_INVALID;
  if (bt->hasDecorate(DecorationStream, 0, &streamId))
    inOutDec.StreamId = streamId;

  SPIRVWord xfbBuffer = SPIRVID_INVALID;
  if (bt->hasDecorate(DecorationXfbBuffer, 0, &xfbBuffer))
    inOutDec.XfbBuffer = xfbBuffer;

  SPIRVWord xfbStride = SPIRVID_INVALID;
  if (bt->hasDecorate(DecorationXfbStride, 0, &xfbStride))
    inOutDec.XfbStride = xfbStride;

  if (bt->isTypeScalar() || bt->isTypeVector() || bt->isTypePointer()) {
    // Handle scalar or vector type or pointer type
    assert(inOutDec.Value.U32All != SPIRVID_INVALID);

    // Build metadata for the scalar/vector
    ShaderInOutMetadata inOutMd = {};
    if (inOutDec.IsXfb)
      inOutMd.IsXfb = true;

    if (inOutDec.IsBuiltIn) {
      inOutMd.IsBuiltIn = true;
      inOutMd.IsLoc = false;
      inOutMd.Value = inOutDec.Value.BuiltIn;
    } else {
      inOutMd.IsLoc = true;
      inOutMd.IsBuiltIn = false;
      inOutMd.Value = inOutDec.Value.Loc;
      inOutMd.Index = inOutDec.Index;
    }

    inOutMd.Component = inOutDec.Component;
    inOutMd.InterpMode = inOutDec.Interp.Mode;
    inOutMd.InterpLoc = inOutDec.Interp.Loc;
    inOutMd.PerPatch = inOutDec.PerPatch;
    inOutMd.PerPrimitive = inOutDec.PerPrimitive;
    inOutMd.StreamId = inOutDec.StreamId;
    inOutMd.XfbBuffer = inOutDec.XfbBuffer;
    inOutMd.XfbStride = inOutDec.XfbStride;
    inOutMd.XfbOffset = inOutDec.XfbOffset;
    inOutMd.XfbExtraOffset = inOutDec.XfbExtraOffset;

    // Check signedness for generic input/output
    if (!inOutDec.IsBuiltIn) {
      SPIRVType *scalarTy = bt->isTypeVector() ? bt->getVectorComponentType() : bt;
      if (scalarTy->isTypeInt())
        inOutMd.Signedness = static_cast<SPIRVTypeInt *>(scalarTy)->isSigned();
    }

    // Update next location value
    if (!inOutDec.IsBuiltIn) {
      unsigned width = bt->isTypePointer() ? 64 : bt->getBitWidth();
      if (bt->isTypeVector())
        width *= bt->getVectorComponentCount();
      assert(width <= 64 * 4);

      if (vs64BitsAttribInputSingleLoc)
        inOutDec.Value.Loc += 1;
      else
        inOutDec.Value.Loc += width <= 32 * 4 ? 1 : 2;
      unsigned alignment = 32;
      unsigned baseStride = 4; // Strides in (bytes)
      inOutDec.XfbExtraOffset += (((width + alignment - 1) / alignment) * baseStride);
    }

    auto int64Ty = Type::getInt64Ty(*m_context);
    std::vector<Type *> mdTys;
    mdTys.push_back(int64Ty); // Content of "ShaderInOutMetadata.U64All[0]"
    mdTys.push_back(int64Ty); // Content of "ShaderInOutMetadata.U64All[1]"
    mdTy = StructType::get(*m_context, mdTys);

    std::vector<Constant *> mdValues;
    mdValues.push_back(ConstantInt::get(int64Ty, inOutMd.U64All[0]));
    mdValues.push_back(ConstantInt::get(int64Ty, inOutMd.U64All[1]));

    return ConstantStruct::get(static_cast<StructType *>(mdTy), mdValues);
  }
  if (bt->isTypeArray() || bt->isTypeMatrix()) {
    // Handle array or matrix type
    auto int32Ty = Type::getInt32Ty(*m_context);
    auto int64Ty = Type::getInt64Ty(*m_context);

    // Build element metadata
    auto elemTy = bt->isTypeArray() ? bt->getArrayElementType() : bt->getMatrixColumnType();
    unsigned numElems = bt->isTypeArray() ? bt->getArrayLength() : bt->getMatrixColumnCount();

    unsigned startLoc = inOutDec.Value.Loc;

    bool alignTo64Bit = checkContains64BitType(elemTy);

    unsigned startXfbExtraOffset = inOutDec.XfbExtraOffset;
    // Align StartXfbExtraOffset to 64-bit (8 bytes)
    if (alignTo64Bit)
      startXfbExtraOffset = roundUpToMultiple(inOutDec.XfbOffset + inOutDec.XfbExtraOffset, 8u) - inOutDec.XfbOffset;

    // PerVertex is not inherted.
    bool isPerVertexDimension = inOutDec.PerVertexDimension;
    inOutDec.PerVertexDimension = false;

    Type *elemMdTy = nullptr;
    auto elemDec = inOutDec; // Inherit from parent
    elemDec.XfbExtraOffset = startXfbExtraOffset;
    auto elemMd = buildShaderInOutMetadata(elemTy, elemDec, elemMdTy, vs64BitsAttribInputSingleLoc);

    inOutDec.PerVertexDimension = isPerVertexDimension;

    if (elemDec.PerPatch)
      inOutDec.PerPatch = true; // Set "per-patch" flag

    if (elemDec.PerPrimitive)
      inOutDec.PerPrimitive = true; // Set "per-primitive" flag

    inOutDec.IsBlockArray = elemTy->hasDecorate(DecorationBlock) || elemDec.IsBlockArray; // Multi-dimension array

    unsigned stride = elemDec.Value.Loc - startLoc;

    unsigned xfbArrayStride = 0;
    if (inOutDec.IsBlockArray) {
      // NOTE: For block array, each block array element is handled within its
      // own capture buffer. The transform feedback array stride is the flatten
      // dimension of an array element.
      assert(elemTy->isTypeArray() || elemTy->isTypeStruct());
      xfbArrayStride = elemTy->isTypeArray() ? elemDec.XfbArrayStride * elemTy->getArrayLength() : 1;
    } else {
      // NOTE: For other non-block arrays, the transform feedback array stride
      // is the occupied byte count of an array element.
      xfbArrayStride = elemDec.XfbExtraOffset - startXfbExtraOffset;

      // Align XfbArrayStride to 64-bit (8 bytes)
      if (alignTo64Bit)
        xfbArrayStride = roundUpToMultiple(xfbArrayStride, 8u);

      // Update next location value
      if (!inOutDec.IsBuiltIn) {
        inOutDec.Value.Loc = startLoc + stride * numElems;
        inOutDec.XfbExtraOffset = startXfbExtraOffset + xfbArrayStride * numElems;
      }
    }

    // Built metadata for the array/matrix
    std::vector<Type *> mdTys;
    mdTys.push_back(int32Ty);  // Stride
    mdTys.push_back(elemMdTy); // Element MD type
    mdTys.push_back(int64Ty);  // Content of "ShaderInOutMetadata.U64All[0]"
    mdTys.push_back(int64Ty);  // Content of "ShaderInOutMetadata.U64All[1]"
    mdTy = StructType::get(*m_context, mdTys);

    ShaderInOutMetadata inOutMd = {};
    if (inOutDec.IsXfb)
      inOutMd.IsXfb = true;
    if (inOutDec.IsBuiltIn) {
      inOutMd.IsBuiltIn = true;
      inOutMd.IsLoc = false;
      inOutMd.Value = inOutDec.Value.BuiltIn;
    } else {
      inOutMd.IsLoc = true;
      inOutMd.IsBuiltIn = false;
      inOutMd.Value = startLoc;
    }

    inOutMd.Component = inOutDec.Component;
    inOutMd.InterpMode = inOutDec.Interp.Mode;
    inOutMd.InterpLoc = inOutDec.Interp.Loc;
    inOutMd.PerPatch = inOutDec.PerPatch;
    inOutMd.PerPrimitive = inOutDec.PerPrimitive;
    inOutMd.StreamId = inOutDec.StreamId;
    inOutMd.IsBlockArray = inOutDec.IsBlockArray;
    inOutMd.XfbBuffer = inOutDec.XfbBuffer;
    inOutMd.XfbStride = inOutDec.XfbStride;
    inOutMd.XfbOffset = inOutDec.XfbOffset;
    inOutMd.XfbArrayStride = xfbArrayStride;
    inOutMd.XfbExtraOffset = startXfbExtraOffset;
    inOutMd.PerVertexDimension = inOutDec.PerVertexDimension;

    std::vector<Constant *> mdValues;
    mdValues.push_back(ConstantInt::get(int32Ty, stride));
    mdValues.push_back(elemMd);
    mdValues.push_back(ConstantInt::get(int64Ty, inOutMd.U64All[0]));
    mdValues.push_back(ConstantInt::get(int64Ty, inOutMd.U64All[1]));

    return ConstantStruct::get(static_cast<StructType *>(mdTy), mdValues);
  }
  if (bt->isTypeStruct()) {
    // Handle structure type
    std::vector<Type *> memberMdTys;
    std::vector<Constant *> memberMdValues;

    // Build metadata for each structure member
    unsigned xfbExtraOffset = inOutDec.XfbExtraOffset;
    unsigned structXfbExtraOffset = 0;
    auto numMembers = bt->getStructMemberCount();

    // Get Block starting transform feedback offset,
    SPIRVWord blockXfbOffset = SPIRVID_INVALID;
    SPIRVWord xfbOffset = SPIRVID_INVALID;

    // Do iteration to deal with transform feedback buffer info
    // Check if the structure member specifies transform feedback buffer ID or stride
    // Enable transform feedback buffer if transform feedback offset is declared, and then
    // find the minimum member transform feedback offset as starting block transform feedback offset
    for (auto memberIdx = 0; memberIdx < numMembers; ++memberIdx) {
      if (bt->hasMemberDecorate(memberIdx, DecorationOffset, 0, &xfbOffset)) {
        if (xfbOffset < blockXfbOffset)
          blockXfbOffset = xfbOffset;
      }
    }
    if (blockXfbOffset == SPIRVID_INVALID)
      blockXfbOffset = 0;

    for (auto memberIdx = 0; memberIdx < numMembers; ++memberIdx) {
      auto memberDec = inOutDec;

      SPIRVWord memberLoc = SPIRVID_INVALID;
      if (bt->hasMemberDecorate(memberIdx, DecorationLocation, 0, &memberLoc)) {
        memberDec.IsBuiltIn = false;
        memberDec.Value.Loc = memberLoc;
      }

      SPIRVWord memberBuiltIn = SPIRVID_INVALID;
      if (bt->hasMemberDecorate(memberIdx, DecorationBuiltIn, 0, &memberBuiltIn)) {
        memberDec.IsBuiltIn = true;
        memberDec.Value.BuiltIn = memberBuiltIn;
      }

      SPIRVWord memberComponent = SPIRVID_INVALID;
      if (bt->hasMemberDecorate(memberIdx, DecorationComponent, 0, &memberComponent))
        memberDec.Component = memberComponent;

      if (bt->hasMemberDecorate(memberIdx, DecorationFlat))
        memberDec.Interp.Mode = InterpModeFlat;

      if (bt->hasMemberDecorate(memberIdx, DecorationNoPerspective))
        memberDec.Interp.Mode = InterpModeNoPersp;

      if (bt->hasMemberDecorate(memberIdx, DecorationCentroid))
        memberDec.Interp.Loc = InterpLocCentroid;

      if (bt->hasMemberDecorate(memberIdx, DecorationSample))
        memberDec.Interp.Loc = InterpLocSample;

      if (bt->hasMemberDecorate(memberIdx, DecorationExplicitInterpAMD)) {
        memberDec.Interp.Mode = InterpModeCustom;
        memberDec.Interp.Loc = InterpLocCustom;
      }

      if (bt->hasMemberDecorate(memberIdx, DecorationPatch))
        memberDec.PerPatch = true;

      if (bt->hasMemberDecorate(memberIdx, DecorationPerPrimitiveEXT)) {
        memberDec.PerPrimitive = true;
        // Per-primitive inputs always use flat shading
        memberDec.Interp.Mode = InterpModeFlat;
      }

      if (bt->hasMemberDecorate(memberIdx, DecorationXfbBuffer, 0, &xfbBuffer))
        memberDec.XfbBuffer = xfbBuffer;

      if (bt->hasMemberDecorate(memberIdx, DecorationXfbStride, 0, &xfbStride))
        memberDec.XfbStride = xfbStride;

      auto memberTy = bt->getStructMemberType(memberIdx);
      bool alignTo64Bit = checkContains64BitType(memberTy);
      if (bt->hasMemberDecorate(memberIdx, DecorationOffset, 0, &xfbOffset)) {
        // For the structure member, if it has DecorationOffset,
        // Then use DecorationOffset as starting XfbExtraOffset
        memberDec.XfbExtraOffset = xfbOffset - blockXfbOffset;
        memberDec.XfbOffset = blockXfbOffset;
        // NOTE: Transform feedback is triggered only if "xfb_offset" is specified. And in such case, "xfb_stride"
        // must be specified as well.
        if (memberDec.XfbStride > 0)
          memberDec.IsXfb = true;
      } else {
        if (alignTo64Bit)
          // Align next XfbExtraOffset to 64-bit (8 bytes)
          memberDec.XfbExtraOffset = roundUpToMultiple(xfbExtraOffset, 8u);
        else
          memberDec.XfbExtraOffset = xfbExtraOffset;
      }
      SPIRVWord memberStreamId = SPIRVID_INVALID;
      if (bt->hasMemberDecorate(memberIdx, DecorationStream, 0, &memberStreamId))
        memberDec.StreamId = memberStreamId;
      Type *memberMdTy = nullptr;
      auto memberMd = buildShaderInOutMetadata(memberTy, memberDec, memberMdTy, vs64BitsAttribInputSingleLoc);

      // Align next XfbExtraOffset to 64-bit (8 bytes)
      xfbExtraOffset = memberDec.XfbExtraOffset;

      if (alignTo64Bit)
        xfbExtraOffset = roundUpToMultiple(xfbExtraOffset, 8u);

      structXfbExtraOffset = std::max(structXfbExtraOffset, xfbExtraOffset);

      if (memberDec.IsBuiltIn)
        inOutDec.IsBuiltIn = true; // Set "builtin" flag
      else
        inOutDec.Value.Loc = memberDec.Value.Loc; // Update next location value

      if (memberDec.PerPatch)
        inOutDec.PerPatch = true; // Set "per-patch" flag

      if (memberDec.PerPrimitive)
        inOutDec.PerPrimitive = true; // Set "per-primitive" flag

      memberMdTys.push_back(memberMdTy);
      memberMdValues.push_back(memberMd);
    }

    inOutDec.XfbExtraOffset = structXfbExtraOffset;
    // Build  metadata for the structure
    mdTy = StructType::get(*m_context, memberMdTys);
    return ConstantStruct::get(static_cast<StructType *>(mdTy), memberMdValues);
  }

  llvm_unreachable("Invalid type");
  return nullptr;
}

// Builds shader block metadata.
Constant *SPIRVToLLVM::buildShaderBlockMetadata(SPIRVType *bt, ShaderBlockDecorate &blockDec, Type *&mdTy,
                                                SPIRVStorageClassKind storageClass) {
  if (bt->isTypeVector() || bt->isTypeScalar() || storageClass == StorageClassAtomicCounter) {
    // Handle scalar or vector type
    ShaderBlockMetadata blockMd = {};
    blockMd.offset = blockDec.Offset;
    blockMd.IsMatrix = false; // Scalar or vector, clear matrix flag
    blockMd.IsRowMajor = false;
    blockMd.MatrixStride = blockDec.MatrixStride;
    blockMd.Restrict = blockDec.Restrict;
    blockMd.Coherent = blockDec.Coherent;
    blockMd.Volatile = blockDec.Volatile;
    blockMd.NonWritable = blockDec.NonWritable;
    blockMd.NonReadable = blockDec.NonReadable;

    mdTy = Type::getInt64Ty(*m_context);
    return ConstantInt::get(mdTy, blockMd.U64All);
  }
  if (bt->isTypeArray() || bt->isTypeMatrix() || bt->isTypePointer()) {
    // Handle array or matrix type
    auto int32Ty = Type::getInt32Ty(*m_context);
    auto int64Ty = Type::getInt64Ty(*m_context);

    unsigned stride = 0;
    SPIRVType *elemTy = nullptr;
    ShaderBlockMetadata blockMd = {};
    if (bt->isTypeArray()) {
      // NOTE: Here, we should keep matrix stride and the flag of row-major
      // matrix. For SPIR-V, such decorations are specified on structure
      // members.
      blockDec.IsMatrix = false;
      SPIRVWord arrayStride = 0;
      bt->hasDecorate(DecorationArrayStride, 0, &arrayStride);
      stride = arrayStride;
      elemTy = bt->getArrayElementType();

    } else if (bt->isTypePointer()) {
      blockDec.IsMatrix = false;
      SPIRVWord arrayStride = 0;
      bt->hasDecorate(DecorationArrayStride, 0, &arrayStride);
      stride = arrayStride;
      elemTy = bt->getPointerElementType();
      blockMd.IsPointer = true;
    } else {
      blockDec.IsMatrix = true;
      stride = blockDec.MatrixStride;
      elemTy = bt->getMatrixColumnType();
    }

    // Build element metadata
    Type *elemMdTy = nullptr;
    auto elemDec = blockDec; // Inherit from parent
    auto elemMd = buildShaderBlockMetadata(elemTy, elemDec, elemMdTy, storageClass);

    // Build metadata for the array/matrix
    std::vector<Type *> mdTys;
    mdTys.push_back(int32Ty);  // Stride
    mdTys.push_back(int64Ty);  // Content of ShaderBlockMetadata
    mdTys.push_back(elemMdTy); // Element MD type
    mdTy = StructType::get(*m_context, mdTys);

    blockMd.offset = blockDec.Offset;
    blockMd.IsMatrix = blockDec.IsMatrix;
    blockMd.IsRowMajor = false;
    blockMd.MatrixStride = blockDec.MatrixStride;
    blockMd.Restrict = blockDec.Restrict;
    blockMd.Coherent = blockDec.Coherent;
    blockMd.Volatile = blockDec.Volatile;
    blockMd.NonWritable = blockDec.NonWritable;
    blockMd.NonReadable = blockDec.NonReadable;

    std::vector<Constant *> mdValues;
    mdValues.push_back(ConstantInt::get(int32Ty, stride));
    mdValues.push_back(ConstantInt::get(int64Ty, blockMd.U64All));
    mdValues.push_back(elemMd);
    return ConstantStruct::get(static_cast<StructType *>(mdTy), mdValues);
  }
  if (bt->isTypeStruct()) {
    // Handle structure type
    blockDec.IsMatrix = false;

    std::vector<Type *> memberMdTys;
    std::vector<Constant *> memberMdValues;

    // Build metadata for each structure member
    unsigned numMembers = bt->getStructMemberCount();
    for (unsigned memberIdx = 0; memberIdx < numMembers; ++memberIdx) {
      SPIRVWord memberMatrixStride = 0;

      // Check member decorations
      auto memberDec = blockDec; // Inherit from parent

      const unsigned remappedIdx = isRemappedTypeElements(bt) ? lookupRemappedTypeElements(bt, memberIdx) : memberIdx;
      const DataLayout &dl = m_m->getDataLayout();
      Type *const ty = transType(bt, 0, false, true, LayoutMode::Explicit);
      assert(ty->isStructTy());
      const StructLayout *const sl = dl.getStructLayout(static_cast<StructType *>(ty));

      // Workaround SPIR-V 1.0 bug where sometimes structs had illegal overlap
      // in their struct offsets.
      if (m_bm->getSPIRVVersion() == SpvVersion10 && remappedIdx == UINT32_MAX)
        memberDec.Offset = UINT32_MAX;
      else
        memberDec.Offset = sl->getElementOffset(remappedIdx);

      if (bt->hasMemberDecorate(memberIdx, DecorationMatrixStride, 0, &memberMatrixStride))
        memberDec.MatrixStride = memberMatrixStride;

      if (bt->hasMemberDecorate(memberIdx, DecorationRestrict))
        memberDec.Restrict = true;
      if (bt->hasMemberDecorate(memberIdx, DecorationCoherent))
        memberDec.Coherent = true;
      if (bt->hasMemberDecorate(memberIdx, DecorationVolatile))
        memberDec.Volatile = true;
      if (bt->hasMemberDecorate(memberIdx, DecorationNonWritable))
        memberDec.NonWritable = true;
      if (bt->hasMemberDecorate(memberIdx, DecorationNonReadable))
        memberDec.NonReadable = true;

      // Build metadata for structure member
      auto memberTy = bt->getStructMemberType(memberIdx);
      Type *memberMdTy = nullptr;
      auto memberMeta = buildShaderBlockMetadata(memberTy, memberDec, memberMdTy, storageClass);

      if (remappedIdx > memberIdx) {
        memberMdTys.push_back(Type::getInt32Ty(*m_context));
        memberMdValues.push_back(PoisonValue::get(Type::getInt32Ty(*m_context)));
      }

      memberMdTys.push_back(memberMdTy);
      memberMdValues.push_back(memberMeta);
    }

    // Build metadata for the structure
    // Member structure type and value
    auto structMdTy = StructType::get(*m_context, memberMdTys);
    auto structMd = ConstantStruct::get(static_cast<StructType *>(structMdTy), memberMdValues);
    auto int64Ty = Type::getInt64Ty(*m_context);
    ShaderBlockMetadata blockMd = {};
    blockMd.offset = blockDec.Offset;
    blockMd.IsStruct = true;

    // Construct structure metadata
    std::vector<Type *> mdTys;
    mdTys.push_back(int64Ty);    // Content of ShaderBlockMetadata
    mdTys.push_back(structMdTy); // Structure MD type

    // Structure MD type
    mdTy = StructType::get(*m_context, mdTys);
    std::vector<Constant *> mdValues;
    mdValues.push_back(ConstantInt::get(int64Ty, blockMd.U64All));
    mdValues.push_back(structMd);

    return ConstantStruct::get(static_cast<StructType *>(mdTy), mdValues);
  }
  if (bt->isTypeForwardPointer()) {
    ShaderBlockMetadata blockMd = {};
    blockMd.offset = blockDec.Offset;
    blockMd.IsMatrix = false; // Scalar or vector, clear matrix flag
    blockMd.IsRowMajor = false;
    blockMd.MatrixStride = 0;
    blockMd.Restrict = blockDec.Restrict;
    blockMd.Coherent = blockDec.Coherent;
    blockMd.Volatile = blockDec.Volatile;
    blockMd.NonWritable = blockDec.NonWritable;
    blockMd.NonReadable = blockDec.NonReadable;

    mdTy = Type::getInt64Ty(*m_context);
    return ConstantInt::get(mdTy, blockMd.U64All);
  } else if (bt->isTypeAccelerationStructureKHR()) {
    ShaderBlockMetadata blockMd = {};
    blockMd.IsAccelerationStructure = true;
    mdTy = Type::getInt64Ty(*m_context);
    return ConstantInt::get(mdTy, blockMd.U64All);
  }

  llvm_unreachable("Invalid type");
  return nullptr;
}

// =============================================================================
// Translate GLSL.std.450 extended instruction
Value *SPIRVToLLVM::transGLSLExtInst(SPIRVExtInst *extInst, BasicBlock *bb) {
  auto bArgs = extInst->getArguments();
  auto args = transValue(extInst->getValues(bArgs), bb->getParent(), bb);
  switch (static_cast<GLSLExtOpKind>(extInst->getExtOp())) {

  case GLSLstd450Round:
  case GLSLstd450RoundEven:
    // Round to whole number
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::rint, args[0]);

  case GLSLstd450Trunc:
    // Trunc to whole number
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::trunc, args[0]);

  case GLSLstd450FAbs:
    // FP absolute value
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::fabs, args[0]);

  case GLSLstd450SAbs:
    // Signed integer absolute value
    return getBuilder()->CreateSAbs(args[0]);

  case GLSLstd450FSign:
    // Get sign of FP value
    return getBuilder()->CreateFSign(args[0]);

  case GLSLstd450SSign:
    // Get sign of signed integer value
    return getBuilder()->CreateSSign(args[0]);

  case GLSLstd450Floor:
    // Round down to whole number
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::floor, args[0]);

  case GLSLstd450Ceil:
    // Round up to whole number
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::ceil, args[0]);

  case GLSLstd450Fract:
    // Get fractional part
    return getBuilder()->CreateFract(args[0]);

  case GLSLstd450Radians:
    // Convert from degrees to radians
    return getBuilder()->CreateFMul(args[0], getBuilder()->getPiOver180(args[0]->getType()));

  case GLSLstd450Degrees:
    // Convert from radians to degrees
    return getBuilder()->CreateFMul(args[0], getBuilder()->get180OverPi(args[0]->getType()));

  case GLSLstd450Sin:
    // sin operation
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::sin, args[0]);

  case GLSLstd450Cos:
    // cos operation
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::cos, args[0]);

  case GLSLstd450Tan:
    // tan operation
    return getBuilder()->CreateTan(args[0]);

  case GLSLstd450Asin:
    // arcsin operation
    return getBuilder()->CreateASin(args[0]);

  case GLSLstd450Acos:
    // arccos operation
    return getBuilder()->CreateACos(args[0]);

  case GLSLstd450Atan:
    // arctan operation
    return getBuilder()->CreateATan(args[0]);

  case GLSLstd450Sinh:
    // hyperbolic sin operation
    return getBuilder()->CreateSinh(args[0]);

  case GLSLstd450Cosh:
    // hyperbolic cos operation
    return getBuilder()->CreateCosh(args[0]);

  case GLSLstd450Tanh:
    // hyperbolic tan operation
    return getBuilder()->CreateTanh(args[0]);

  case GLSLstd450Asinh:
    // hyperbolic arcsin operation
    return getBuilder()->CreateASinh(args[0]);

  case GLSLstd450Acosh:
    // hyperbolic arccos operation
    return getBuilder()->CreateACosh(args[0]);

  case GLSLstd450Atanh:
    // hyperbolic arctan operation
    return getBuilder()->CreateATanh(args[0]);

  case GLSLstd450Atan2:
    // arctan operation with Y/X input
    return getBuilder()->CreateATan2(args[0], args[1]);

  case GLSLstd450Pow:
    // Power: x^y
    return getBuilder()->CreatePower(args[0], args[1]);

  case GLSLstd450Exp:
    // Exponent: e^x
    return getBuilder()->CreateExp(args[0]);

  case GLSLstd450Log:
    // Natural logarithm: log(x)
    return getBuilder()->CreateLog(args[0]);

  case GLSLstd450Exp2:
    // Base 2 exponent: 2^x
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::exp2, args[0]);

  case GLSLstd450Log2:
    // Base 2 logarithm: log2(x)
    return getBuilder()->CreateUnaryIntrinsic(Intrinsic::log2, args[0]);

  case GLSLstd450Sqrt:
    // Square root
    return getBuilder()->CreateSqrt(args[0]);

  case GLSLstd450InverseSqrt: {
    // Inverse square root
    return getBuilder()->CreateInverseSqrt(args[0]);
  }

  case GLSLstd450Determinant:
    // Determinant of square matrix
    return getBuilder()->CreateDeterminant(args[0]);

  case GLSLstd450MatrixInverse:
    // Inverse of square matrix
    return getBuilder()->CreateMatrixInverse(args[0]);

  case GLSLstd450Modf: {
    // Split input into fractional and whole number parts.
    Value *wholeNum = getBuilder()->CreateUnaryIntrinsic(Intrinsic::trunc, args[0]);
    Value *fract = getBuilder()->CreateFSub(args[0], wholeNum);
    // Vectors are represented as arrays in memory, so we need to cast the pointer of array to pointer of vector before
    // storing.
    if (wholeNum->getType()->isVectorTy()) {
      assert(args[1]->getType()->isPointerTy());
      Type *const castType = wholeNum->getType()->getPointerTo(args[1]->getType()->getPointerAddressSpace());
      args[1] = getBuilder()->CreateBitCast(args[1], castType);
    }
    getBuilder()->CreateStore(wholeNum, args[1]);
    return fract;
  }

  case GLSLstd450ModfStruct: {
    // Split input into fractional and whole number parts.
    Value *wholeNum = getBuilder()->CreateUnaryIntrinsic(Intrinsic::trunc, args[0]);
    Value *fract = getBuilder()->CreateFSub(args[0], wholeNum);
    Value *result = PoisonValue::get(transType(extInst->getType()));
    result = getBuilder()->CreateInsertValue(result, fract, 0);
    result = getBuilder()->CreateInsertValue(result, wholeNum, 1);
    return result;
  }

  case GLSLstd450FMin:
  case GLSLstd450NMin: {
    // FMin: FP minimum (undefined result for NaN)
    // NMin: FP minimum (preserve NaN)
    FastMathFlags fmf = getBuilder()->getFastMathFlags();
    fmf.setNoNaNs(extInst->getExtOp() == GLSLstd450FMin);
    getBuilder()->setFastMathFlags(fmf);
    return getBuilder()->CreateFMin(args[0], args[1]);
  }

  case GLSLstd450UMin: {
    // Unsigned integer minimum
    return getBuilder()->CreateBinaryIntrinsic(Intrinsic::umin, args[0], args[1]);
  }

  case GLSLstd450SMin: {
    // Signed integer minimum
    return getBuilder()->CreateBinaryIntrinsic(Intrinsic::smin, args[0], args[1]);
  }

  case GLSLstd450FMax:
  case GLSLstd450NMax: {
    // FMax: FP maximum (undefined result for NaN)
    // NMax: FP maximum (preserve NaN)
    FastMathFlags fmf = getBuilder()->getFastMathFlags();
    fmf.setNoNaNs(extInst->getExtOp() == GLSLstd450FMax);
    getBuilder()->setFastMathFlags(fmf);
    return getBuilder()->CreateFMax(args[0], args[1]);
  }

  case GLSLstd450UMax: {
    // Unsigned integer maximum
    return getBuilder()->CreateBinaryIntrinsic(Intrinsic::umax, args[0], args[1]);
  }

  case GLSLstd450SMax: {
    // Signed integer maximum
    return getBuilder()->CreateBinaryIntrinsic(Intrinsic::smax, args[0], args[1]);
  }

  case GLSLstd450FClamp:
  case GLSLstd450NClamp: {
    // FClamp: FP clamp with undefined result if any input is NaN
    // NClamp: FP clamp with "avoid NaN" semantics
    FastMathFlags preservedFmf = getBuilder()->getFastMathFlags();
    FastMathFlags modifiedFmf = preservedFmf;
    modifiedFmf.setNoNaNs(extInst->getExtOp() == GLSLstd450FClamp);
    getBuilder()->setFastMathFlags(modifiedFmf);
    Value *result = getBuilder()->CreateFClamp(args[0], args[1], args[2]);
    getBuilder()->setFastMathFlags(preservedFmf);
    return result;
  }

  case GLSLstd450UClamp: {
    // Unsigned integer clamp
    Value *max1 = getBuilder()->CreateBinaryIntrinsic(Intrinsic::umax, args[0], args[1]);
    return getBuilder()->CreateBinaryIntrinsic(Intrinsic::umin, args[2], max1);
  }

  case GLSLstd450SClamp: {
    // Signed integer clamp
    Value *max1 = getBuilder()->CreateBinaryIntrinsic(Intrinsic::smax, args[0], args[1]);
    return getBuilder()->CreateBinaryIntrinsic(Intrinsic::smin, args[2], max1);
  }

  case GLSLstd450FMix: {
    // Linear blend
    return getBuilder()->createFMix(args[0], args[1], args[2]);
  }

  case GLSLstd450Step: {
    // x < edge ? 0.0 : 1.0
    Value *edge = args[0];
    Value *x = args[1];
    Value *cmp = getBuilder()->CreateFCmpOLT(x, edge);
    return getBuilder()->CreateSelect(cmp, Constant::getNullValue(x->getType()), ConstantFP::get(x->getType(), 1.0));
  }

  case GLSLstd450SmoothStep:
    // Smooth step operation
    return getBuilder()->CreateSmoothStep(args[0], args[1], args[2]);

  case GLSLstd450Fma:
    // Fused multiply and add
    if (m_shaderOptions->disableFMA || DisableFMA) {
      IRBuilderBase::FastMathFlagGuard FMFGuard(*getBuilder());
      getBuilder()->getFastMathFlags().setAllowContract(false);
      return getBuilder()->CreateFAdd(getBuilder()->CreateFMul(args[0], args[1]), args[2]);
    }
    return getBuilder()->CreateFma(args[0], args[1], args[2]);

  case GLSLstd450Frexp:
  case GLSLstd450FrexpStruct: {
    // Split input into significand (mantissa) and exponent.
    Value *mant = getBuilder()->CreateExtractSignificand(args[0]);
    Value *exp = getBuilder()->CreateExtractExponent(args[0]);
    if (extInst->getExtOp() == GLSLstd450FrexpStruct) {
      // FrexpStruct: Return the two values as a struct.
      Value *result = PoisonValue::get(transType(extInst->getType()));
      result = getBuilder()->CreateInsertValue(result, mant, 0);
      exp = getBuilder()->CreateSExtOrTrunc(exp, result->getType()->getStructElementType(1));
      result = getBuilder()->CreateInsertValue(result, exp, 1);
      return result;
    }
    // Frexp: Store the exponent and return the mantissa.
    Type *pointeeType = getPointeeType(extInst->getValues(bArgs)[1]);
    exp = getBuilder()->CreateSExtOrTrunc(exp, pointeeType);
    // Vectors are represented as arrays in memory, so we need to cast the pointer of array to pointer of vector before
    // storing.
    if (exp->getType()->isVectorTy()) {
      assert(args[1]->getType()->isPointerTy());
      Type *const castType = exp->getType()->getPointerTo(args[1]->getType()->getPointerAddressSpace());
      args[1] = getBuilder()->CreateBitCast(args[1], castType);
    }
    getBuilder()->CreateStore(exp, args[1]);
    return mant;
  }

  case GLSLstd450Ldexp:
    // Construct FP value from mantissa and exponent
    return getBuilder()->CreateLdexp(args[0], args[1]);

  case GLSLstd450PackSnorm4x8: {
    // Convert <4 x float> into signed normalized <4 x i8> then pack into i32.
    // round(clamp(c, -1, +1) * 127.0)
    Value *val = getBuilder()->CreateFClamp(args[0], ConstantFP::get(args[0]->getType(), -1.0),
                                            ConstantFP::get(args[0]->getType(), 1.0));
    val = getBuilder()->CreateFMul(val, ConstantFP::get(args[0]->getType(), 127.0));
    val = getBuilder()->CreateUnaryIntrinsic(Intrinsic::rint, val);
    val = getBuilder()->CreateFPToSI(val, FixedVectorType::get(getBuilder()->getInt8Ty(), 4));
    return getBuilder()->CreateBitCast(val, getBuilder()->getInt32Ty());
  }

  case GLSLstd450PackUnorm4x8: {
    // Convert <4 x float> into unsigned normalized <4 x i8> then pack into i32.
    // round(clamp(c, 0, +1) * 255.0)
    Value *val = getBuilder()->CreateFClamp(args[0], Constant::getNullValue(args[0]->getType()),
                                            ConstantFP::get(args[0]->getType(), 1.0));
    val = getBuilder()->CreateFMul(val, ConstantFP::get(args[0]->getType(), 255.0));
    val = getBuilder()->CreateUnaryIntrinsic(Intrinsic::rint, val);
    val = getBuilder()->CreateFPToUI(val, FixedVectorType::get(getBuilder()->getInt8Ty(), 4));
    return getBuilder()->CreateBitCast(val, getBuilder()->getInt32Ty());
  }

  case GLSLstd450PackSnorm2x16: {
    // Convert <2 x float> into signed normalized <2 x i16> then pack into i32.
    // round(clamp(c, -1, +1) * 32767.0)
    Value *val = getBuilder()->CreateFClamp(args[0], ConstantFP::get(args[0]->getType(), -1.0),
                                            ConstantFP::get(args[0]->getType(), 1.0));
    val = getBuilder()->CreateFMul(val, ConstantFP::get(args[0]->getType(), 32767.0));
    val = getBuilder()->CreateUnaryIntrinsic(Intrinsic::rint, val);
    val = getBuilder()->CreateFPToSI(val, FixedVectorType::get(getBuilder()->getInt16Ty(), 2));
    return getBuilder()->CreateBitCast(val, getBuilder()->getInt32Ty());
  }

  case GLSLstd450PackUnorm2x16: {
    // Convert <2 x float> into unsigned normalized <2 x i16> then pack into i32.
    // round(clamp(c, 0, +1) * 65535.0)
    Value *val = getBuilder()->CreateFClamp(args[0], Constant::getNullValue(args[0]->getType()),
                                            ConstantFP::get(args[0]->getType(), 1.0));
    val = getBuilder()->CreateFMul(val, ConstantFP::get(args[0]->getType(), 65535.0));
    val = getBuilder()->CreateUnaryIntrinsic(Intrinsic::rint, val);
    val = getBuilder()->CreateFPToUI(val, FixedVectorType::get(getBuilder()->getInt16Ty(), 2));
    return getBuilder()->CreateBitCast(val, getBuilder()->getInt32Ty());
  }

  case GLSLstd450PackHalf2x16: {
    // Convert <2 x float> into <2 x half> then pack into i32.
    Value *val = getBuilder()->CreateFPTrunc(args[0], FixedVectorType::get(getBuilder()->getHalfTy(), 2));
    return getBuilder()->CreateBitCast(val, getBuilder()->getInt32Ty());
  }

  case GLSLstd450PackDouble2x32:
    // Cast <2 x i32> to double.
    return getBuilder()->CreateBitCast(args[0], getBuilder()->getDoubleTy());

  case GLSLstd450UnpackSnorm2x16: {
    // Unpack i32 into <2 x i16> then treat as signed normalized and convert to
    // <2 x float>.
    Value *val = getBuilder()->CreateBitCast(args[0], FixedVectorType::get(getBuilder()->getInt16Ty(), 2));
    val = getBuilder()->CreateSIToFP(val, FixedVectorType::get(getBuilder()->getFloatTy(), 2));
    Value *multiplier = getBuilder()->getOneOverPower2MinusOne(val->getType(), 15); // 1/32767
    val = getBuilder()->CreateFMul(val, multiplier);
    return getBuilder()->CreateFClamp(val, ConstantFP::get(val->getType(), -1.0), ConstantFP::get(val->getType(), 1.0));
  }

  case GLSLstd450UnpackUnorm2x16: {
    // Unpack i32 into <2 x i16> then treat as unsigned normalized and convert
    // to <2 x float>.
    Value *val = getBuilder()->CreateBitCast(args[0], FixedVectorType::get(getBuilder()->getInt16Ty(), 2));
    val = getBuilder()->CreateUIToFP(val, FixedVectorType::get(getBuilder()->getFloatTy(), 2));
    Value *multiplier = getBuilder()->getOneOverPower2MinusOne(val->getType(), 16); // 1/65535
    return getBuilder()->CreateFMul(val, multiplier);
  }

  case GLSLstd450UnpackHalf2x16: {
    // Unpack <2 x half> from i32 and convert to <2 x float>.
    // This is required to flush denorm to zero if that mode is enabled.
    Value *val = getBuilder()->CreateBitCast(args[0], FixedVectorType::get(getBuilder()->getHalfTy(), 2));
    val = flushDenorm(val);
    return getBuilder()->CreateFPExt(val, FixedVectorType::get(getBuilder()->getFloatTy(), 2));
  }

  case GLSLstd450UnpackSnorm4x8: {
    // Unpack i32 into <4 x i8> then treat as signed normalized and convert to
    // <4 x float>.
    Value *val = getBuilder()->CreateBitCast(args[0], FixedVectorType::get(getBuilder()->getInt8Ty(), 4));
    val = getBuilder()->CreateSIToFP(val, FixedVectorType::get(getBuilder()->getFloatTy(), 4));
    Value *multiplier = getBuilder()->getOneOverPower2MinusOne(val->getType(), 7); // 1/127
    val = getBuilder()->CreateFMul(val, multiplier);
    return getBuilder()->CreateFClamp(val, ConstantFP::get(val->getType(), -1.0), ConstantFP::get(val->getType(), 1.0));
  }

  case GLSLstd450UnpackUnorm4x8: {
    // Unpack i32 into <4 x i8> then treat as unsigned normalized and convert to
    // <4 x float>.
    Value *val = getBuilder()->CreateBitCast(args[0], FixedVectorType::get(getBuilder()->getInt8Ty(), 4));
    val = getBuilder()->CreateUIToFP(val, FixedVectorType::get(getBuilder()->getFloatTy(), 4));
    Value *multiplier = getBuilder()->getOneOverPower2MinusOne(val->getType(), 8); // 1/255
    return getBuilder()->CreateFMul(val, multiplier);
  }

  case GLSLstd450UnpackDouble2x32:
    // Cast double to <2 x i32>.
    return getBuilder()->CreateBitCast(args[0], FixedVectorType::get(getBuilder()->getInt32Ty(), 2));

  case GLSLstd450Length: {
    // Get length of vector.
    if (!isa<VectorType>(args[0]->getType()))
      return getBuilder()->CreateUnaryIntrinsic(Intrinsic::fabs, args[0]);
    Value *dot = getBuilder()->CreateDotProduct(args[0], args[0]);
    return getBuilder()->CreateSqrt(dot);
  }

  case GLSLstd450Distance: {
    // Get distance between two points.
    Value *diff = getBuilder()->CreateFSub(args[0], args[1]);
    if (!isa<VectorType>(diff->getType()))
      return getBuilder()->CreateUnaryIntrinsic(Intrinsic::fabs, diff);
    Value *dot = getBuilder()->CreateDotProduct(diff, diff);
    return getBuilder()->CreateSqrt(dot);
  }

  case GLSLstd450Cross:
    // Vector cross product.
    return getBuilder()->CreateCrossProduct(args[0], args[1]);

  case GLSLstd450Normalize:
    // Normalize vector to magnitude 1.
    return getBuilder()->CreateNormalizeVector(args[0]);

  case GLSLstd450FaceForward:
    // Face forward operation.
    return getBuilder()->CreateFaceForward(args[0], args[1], args[2]);

  case GLSLstd450Reflect:
    // Reflect operation.
    return getBuilder()->CreateReflect(args[0], args[1]);

  case GLSLstd450Refract:
    // Refract operation.
    return getBuilder()->CreateRefract(args[0], args[1], args[2]);

  case GLSLstd450FindILsb: {
    // Find integer least-significant 1-bit. 0 input gives -1 result.
    // The spec claims that the result must be the same type as the input, but I
    // have seen SPIR-V that does not do that.
    Value *isZero = getBuilder()->CreateICmpEQ(args[0], Constant::getNullValue(args[0]->getType()));
    Value *result = getBuilder()->CreateBinaryIntrinsic(Intrinsic::cttz, args[0], getBuilder()->getTrue());
    result = getBuilder()->CreateSelect(isZero, Constant::getAllOnesValue(result->getType()), result);
    return getBuilder()->CreateSExtOrTrunc(result, transType(extInst->getType()));
  }

  case GLSLstd450FindSMsb: {
    // Find signed integer most-significant bit. 0 or -1 input gives -1 result.
    Value *result = getBuilder()->CreateFindSMsb(args[0]);
    // TODO: According to the SPIR-V spec, FindSMsb expects the input value and result to have both the
    // same number of components and the same component width. But glslang violates this rule. Thus,
    // we have a workaround here for this mismatch.
    return getBuilder()->CreateSExtOrTrunc(result, transType(extInst->getType()));
  }

  case GLSLstd450FindUMsb: {
    // Find unsigned integer most-significant 1-bit. 0 input gives -1 result.
    // The spec claims that the result must be the same type as the input, but I
    // have seen SPIR-V that does not do that.
    Value *result = getBuilder()->CreateBinaryIntrinsic(Intrinsic::ctlz, args[0], getBuilder()->getFalse());
    result = getBuilder()->CreateSub(
        ConstantInt::get(result->getType(), result->getType()->getScalarType()->getPrimitiveSizeInBits() - 1), result);
    return getBuilder()->CreateSExtOrTrunc(result, transType(extInst->getType()));
  }

  case GLSLstd450InterpolateAtCentroid:
  case GLSLstd450InterpolateAtSample:
  case GLSLstd450InterpolateAtOffset:
    // These InterpolateAt* instructions are handled the old way, by generating
    // a call.
    return transGLSLBuiltinFromExtInst(extInst, bb);

  default:
    llvm_unreachable("Unrecognized GLSLstd450 extended instruction");
  }
}

// =============================================================================
// Flush denorm to zero if DenormFlushToZero is set in the shader
Value *SPIRVToLLVM::flushDenorm(Value *val) {
  if ((m_fpControlFlags.DenormFlushToZero * 8) & val->getType()->getScalarType()->getPrimitiveSizeInBits())
    val = getBuilder()->CreateUnaryIntrinsic(Intrinsic::canonicalize, val);
  return val;
}

// =============================================================================
// Translate ShaderTrinaryMinMax extended instructions
Value *SPIRVToLLVM::transTrinaryMinMaxExtInst(SPIRVExtInst *extInst, BasicBlock *bb) {
  const auto &bArgs = extInst->getArguments();
  auto args = transValue(extInst->getValues(bArgs), bb->getParent(), bb);
  switch (extInst->getExtOp()) {

  case FMin3AMD: {
    // Minimum of three FP values. Undefined result if any NaNs.
    FastMathFlags fmf = getBuilder()->getFastMathFlags();
    fmf.setNoNaNs();
    getBuilder()->setFastMathFlags(fmf);
    return getBuilder()->CreateFMin3(args[0], args[1], args[2]);
  }

  case FMax3AMD: {
    // Maximum of three FP values. Undefined result if any NaNs.
    FastMathFlags fmf = getBuilder()->getFastMathFlags();
    fmf.setNoNaNs();
    getBuilder()->setFastMathFlags(fmf);
    return getBuilder()->CreateFMax3(args[0], args[1], args[2]);
  }

  case FMid3AMD: {
    // Middle of three FP values. Undefined result if any NaNs.
    FastMathFlags fmf = getBuilder()->getFastMathFlags();
    fmf.setNoNaNs();
    getBuilder()->setFastMathFlags(fmf);
    return getBuilder()->CreateFMid3(args[0], args[1], args[2]);
  }

  case UMin3AMD: {
    // Minimum of three unsigned integer values.
    Value *min1 = getBuilder()->CreateBinaryIntrinsic(Intrinsic::umin, args[0], args[1]);
    return getBuilder()->CreateBinaryIntrinsic(Intrinsic::umin, min1, args[2]);
  }

  case UMax3AMD: {
    // Maximum of three unsigned integer values.
    Value *max1 = getBuilder()->CreateBinaryIntrinsic(Intrinsic::umax, args[0], args[1]);
    return getBuilder()->CreateBinaryIntrinsic(Intrinsic::umax, max1, args[2]);
  }

  case UMid3AMD: {
    // Middle of three unsigned integer values.
    Value *min1 = getBuilder()->CreateBinaryIntrinsic(Intrinsic::umin, args[0], args[1]);
    Value *max1 = getBuilder()->CreateBinaryIntrinsic(Intrinsic::umax, args[0], args[1]);
    Value *min2 = getBuilder()->CreateBinaryIntrinsic(Intrinsic::umin, max1, args[2]);
    return getBuilder()->CreateBinaryIntrinsic(Intrinsic::umax, min1, min2);
  }

  case SMin3AMD: {
    // Minimum of three signed integer values.
    Value *min1 = getBuilder()->CreateBinaryIntrinsic(Intrinsic::smin, args[0], args[1]);
    return getBuilder()->CreateBinaryIntrinsic(Intrinsic::smin, min1, args[2]);
  }

  case SMax3AMD: {
    // Maximum of three signed integer values.
    Value *max1 = getBuilder()->CreateBinaryIntrinsic(Intrinsic::smax, args[0], args[1]);
    return getBuilder()->CreateBinaryIntrinsic(Intrinsic::smax, max1, args[2]);
  }

  case SMid3AMD: {
    // Middle of three signed integer values.
    Value *min1 = getBuilder()->CreateBinaryIntrinsic(Intrinsic::smin, args[0], args[1]);
    Value *max1 = getBuilder()->CreateBinaryIntrinsic(Intrinsic::smax, args[0], args[1]);
    Value *min2 = getBuilder()->CreateBinaryIntrinsic(Intrinsic::smin, max1, args[2]);
    return getBuilder()->CreateBinaryIntrinsic(Intrinsic::smax, min1, min2);
  }

  default:
    llvm_unreachable("Unrecognized ShaderTrinaryMinMax instruction");
  }
}

// =============================================================================
Value *SPIRVToLLVM::transGLSLBuiltinFromExtInst(SPIRVExtInst *bc, BasicBlock *bb) {
  assert(bb && "Invalid BB");

  SPIRVExtInstSetKind set = m_bm->getBuiltinSet(bc->getExtSetId());
  assert((set == SPIRVEIS_GLSL || set == SPIRVEIS_ShaderExplicitVertexParameterAMD) &&
         "Not valid extended instruction");

  SPIRVWord entryPoint = bc->getExtOp();
  auto bArgs = bc->getArguments();
  std::vector<Type *> argTys = transTypeVector(bc->getValueTypes(bArgs));
  string unmangledName = "";
  if (set == SPIRVEIS_GLSL)
    unmangledName = GLSLExtOpMap::map(static_cast<GLSLExtOpKind>(entryPoint));
  else if (set == SPIRVEIS_ShaderExplicitVertexParameterAMD)
    unmangledName = ShaderExplicitVertexParameterAMDExtOpMap::map(
        static_cast<ShaderExplicitVertexParameterAMDExtOpKind>(entryPoint));

  string mangledName(unmangledName);
  std::vector<Value *> args = transValue(bc->getArgumentValues(), bb->getParent(), bb);
  Type *retType = transType(bc->getType());
  appendTypeMangling(retType, args, mangledName);
  FunctionType *funcTy = FunctionType::get(retType, argTys, false);
  Function *func = m_m->getFunction(mangledName);
  if (!func) {
    func = Function::Create(funcTy, GlobalValue::ExternalLinkage, mangledName, m_m);
    func->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      func->addFnAttr(Attribute::NoUnwind);
  }
  CallInst *call = CallInst::Create(func, args, bc->getName(), bb);
  setCallingConv(call);
  call->addFnAttr(Attribute::NoUnwind);
  return call;
}

Instruction *SPIRVToLLVM::transBarrier(BasicBlock *bb, SPIRVWord execScope, SPIRVWord memSema, SPIRVWord memScope) {
  transMemFence(bb, memSema, memScope);
  return getBuilder()->CreateBarrier();
}

Instruction *SPIRVToLLVM::transMemFence(BasicBlock *bb, SPIRVWord memSema, SPIRVWord memScope) {
  AtomicOrdering ordering = AtomicOrdering::NotAtomic;

  if (memSema & MemorySemanticsSequentiallyConsistentMask)
    ordering = AtomicOrdering::SequentiallyConsistent;
  else if (memSema & MemorySemanticsAcquireReleaseMask)
    ordering = AtomicOrdering::AcquireRelease;
  else if (memSema & MemorySemanticsAcquireMask)
    ordering = AtomicOrdering::Acquire;
  else if (memSema & MemorySemanticsReleaseMask)
    ordering = AtomicOrdering::Release;
  else if (memSema != MemorySemanticsMaskNone && m_bm->getMemoryModel() != MemoryModelVulkan) {
    // Some shaders written for pre-Vulkan memory models use e.g.:
    // OpMemoryBarrier 1, 512 // 512 = CrossWorkgroupMemory
    // and expect some ordering, even though none of the low 4 (ordering) bits
    // of the semantics are set, so we set a reasonable default here.
    ordering = AtomicOrdering::AcquireRelease;
  }

  if (ordering == AtomicOrdering::NotAtomic)
    return nullptr;

  // Upgrade the ordering if we need to make it available or visible
  if (memSema & (MemorySemanticsMakeAvailableKHRMask | MemorySemanticsMakeVisibleKHRMask))
    ordering = AtomicOrdering::SequentiallyConsistent;

  SyncScope::ID scope = SyncScope::System;

  switch (memScope) {
  case ScopeCrossDevice:
  case ScopeDevice:
  case ScopeQueueFamilyKHR:
  case ScopeShaderCallKHR:
    scope = SyncScope::System;
    break;
  case ScopeInvocation:
    scope = SyncScope::SingleThread;
    break;
  case ScopeWorkgroup:
    scope = m_context->getOrInsertSyncScopeID("workgroup");
    break;
  case ScopeSubgroup:
    scope = m_context->getOrInsertSyncScopeID("wavefront");
    break;
  default:
    llvm_unreachable("Invalid scope");
  }

  return new FenceInst(*m_context, ordering, scope, bb);
}

Instruction *SPIRVToLLVM::transBarrierFence(SPIRVInstruction *mb, BasicBlock *bb) {
  assert(bb && "Invalid BB");
  std::string funcName;
  auto getIntVal = [](SPIRVValue *value) { return static_cast<SPIRVConstant *>(value)->getZExtIntValue(); };

  Instruction *barrier = nullptr;

  if (mb->getOpCode() == OpMemoryBarrier) {
    auto memB = static_cast<SPIRVMemoryBarrier *>(mb);

    SPIRVWord memScope = getIntVal(memB->getOpValue(0));
    SPIRVWord memSema = getIntVal(memB->getOpValue(1));

    barrier = transMemFence(bb, memSema, memScope);
  } else if (mb->getOpCode() == OpControlBarrier) {
    auto ctlB = static_cast<SPIRVControlBarrier *>(mb);

    SPIRVWord execScope = getIntVal(ctlB->getExecScope());
    SPIRVWord memSema = getIntVal(ctlB->getMemSemantic());
    SPIRVWord memScope = getIntVal(ctlB->getMemScope());

    barrier = transBarrier(bb, execScope, memSema, memScope);
  } else
    llvm_unreachable("Invalid instruction");

  if (barrier) {
    setName(barrier, mb);

    if (CallInst *call = dyn_cast<CallInst>(barrier))
      setAttrByCalledFunc(call);
  }

  return barrier;
}

llvm::GlobalValue::LinkageTypes SPIRVToLLVM::transLinkageType(const SPIRVValue *v) {
  if (v->getLinkageType() == LinkageTypeInternal) {
    if (v->getOpCode() == OpVariable) {
      // Variable declaration
      SPIRVStorageClassKind storageClass = static_cast<const SPIRVVariable *>(v)->getStorageClass();
      if (storageClass == StorageClassUniformConstant || storageClass == StorageClassInput ||
          storageClass == StorageClassUniform || storageClass == StorageClassPushConstant ||
          storageClass == StorageClassStorageBuffer || storageClass == StorageClassAtomicCounter)
        return GlobalValue::ExternalLinkage;
      if (storageClass == StorageClassPrivate || storageClass == StorageClassOutput)
        return GlobalValue::PrivateLinkage;
    }
    return GlobalValue::InternalLinkage;
  }
  if (v->getLinkageType() == LinkageTypeImport) {
    // Function declaration
    if (v->getOpCode() == OpFunction) {
      if (static_cast<const SPIRVFunction *>(v)->getNumBasicBlock() == 0)
        return GlobalValue::ExternalLinkage;
    }
    // Variable declaration
    if (v->getOpCode() == OpVariable) {
      if (static_cast<const SPIRVVariable *>(v)->getInitializer() == 0)
        return GlobalValue::ExternalLinkage;
    }
    // Definition
    return GlobalValue::AvailableExternallyLinkage;
  }
  // LinkageTypeExport
  if (v->getOpCode() == OpVariable) {
    if (static_cast<const SPIRVVariable *>(v)->getInitializer() == 0)
      // Tentative definition
      return GlobalValue::CommonLinkage;
  }
  return GlobalValue::ExternalLinkage;
}

llvm::Function *SPIRVToLLVM::createLibraryEntryFunc() {
  auto builder = getBuilder();
  FunctionType *funcTy = FunctionType::get(builder->getVoidTy(), {}, false);
  auto func = Function::Create(funcTy, GlobalValue::ExternalLinkage, "libraryEntry", m_m);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  builder->SetInsertPoint(entryBlock);
  builder->CreateRetVoid();
  std::vector<Metadata *> execModelMDs;
  execModelMDs.push_back(ConstantAsMetadata::get(ConstantInt::get(builder->getInt32Ty(), ExecutionModelGLCompute)));
  auto execModelMdNode = MDNode::get(*m_context, execModelMDs);
  func->addMetadata(gSPIRVMD::ExecutionModel, *execModelMdNode);
  func->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  return func;
}

const Vkgc::PipelineOptions *SPIRVToLLVM::getPipelineOptions() const {
  assert(m_context && "Invalid context!");

  return static_cast<Llpc::Context *>(m_context)->getPipelineContext()->getPipelineOptions();
}

bool SPIRVToLLVM::scratchBoundsChecksEnabled() const {
  assert(m_context && "Invalid context!");
  const Vkgc::GfxIpVersion gfxIp = static_cast<Llpc::Context *>(m_context)->getPipelineContext()->getGfxIpVersion();
  return gfxIp.major >= 9 || getPipelineOptions()->enableScratchAccessBoundsChecks;
}

SPIRVAccessChainBase *SPIRVToLLVM::deriveAccessChain(SPIRVValue *memOp,
                                                     SPIRVToLLVM::LlvmMemOpType instructionType) const {
  if (instructionType == LlvmMemOpType::IS_LOAD)
    return static_cast<SPIRVAccessChainBase *>(static_cast<SPIRVLoad *>(memOp)->getSrc());

  return static_cast<SPIRVAccessChainBase *>(static_cast<SPIRVStore *>(memOp)->getDst());
}

bool SPIRVToLLVM::shouldInsertScratchBoundsCheck(SPIRVValue *memOp, SPIRVToLLVM::LlvmMemOpType instructionType) const {
  assert(m_scratchBoundsChecksEnabled);

  const auto accessChain = deriveAccessChain(memOp, instructionType);
  if (!accessChain || accessChain->getOpCode() != OpAccessChain)
    return false;

  SPIRVStorageClassKind pointerStorageClass = accessChain->getType()->getPointerStorageClass();

  // We don't want bounds checks if we are outside private or function storage class.
  return pointerStorageClass == StorageClassPrivate || pointerStorageClass == StorageClassFunction;
}

Instruction *SPIRVToLLVM::getLastInsertedValue() {
  BasicBlock *insertBlock = getBuilder()->GetInsertBlock();

  if (!insertBlock->empty())
    return &insertBlock->back();

  return nullptr;
}

// Records all instructions from baseNode to the latest inserted instructions and stores the instruction classification
// here. If we have used addLoadInstRecursively, these will be loads, if we have used addStoreInstRecursively, these
// will be stores. In the load case, the last instruction will be connected with the subsequent instructions by
// inserting a phi node.
void SPIRVToLLVM::gatherScratchBoundsChecksMemOps(SPIRVValue *spirvMemOp, Instruction *baseNode,
                                                  BasicBlock *baseNodeParent,
                                                  SPIRVToLLVM::LlvmMemOpType instructionType, Function *parent) {
  assert(m_scratchBoundsChecksEnabled);
  if (!shouldInsertScratchBoundsCheck(spirvMemOp, instructionType))
    return;

  Instruction *instr = nullptr;
  if (baseNode) {
    instr = baseNode;
  } else {
    // Start iterating from the beginning of the basic block (if at the time of calling gatherScratchBoundsChecksMemOps
    // the BB was empty).
    instr = &baseNodeParent->front();
  }

  if (!instr) {
    // If there is no base instruction to work with, there won't be any instruction range for insertion.
    return;
  }

  // We are using SPIRVValue * as key here to prevent issues with loads and stores using the same access chain,
  // therefore getting grouped together by accident.
  auto &mapping = m_spirvMemopToLlvmMemopMapping[parent][spirvMemOp];
  mapping.memOpType = instructionType;

  // For loads, we want to save the original SPIRVValue *, as we might have subsequent loads (e. g., loads as inputs of
  // access chains) where the control flow controls in a new phi node and we need to update the m_valueMap for this
  // SPIRVValue * with the new phi node, so the out of bounds checks recognizes that it needs to continue working with
  // the phi.
  if (instructionType == LlvmMemOpType::IS_LOAD)
    mapping.instructionOrigin = spirvMemOp;

  auto &llvmMemOps = mapping.llvmInstructions;

  while (instr && instr->getIterator() != getBuilder()->GetInsertPoint()) {
    instr = instr->getNextNode();
    if (instr)
      llvmMemOps.push_back(instr);
  }
}

// This method inserts bounds checks for loads and stores. We are handling private and function memory classes here. The
// idea behind this method is that while translating a OpLoad or OpStore which loads from local scratch memory using a
// unknown (run-time) index into an array, we save pointers to the SPIR-V value and the corresponding LLVM
// memops. Here, we parse the operands of the corresponding access chain, add a chain of ANDed compares to check if the
// bounds requirements are met and then split the basic block around the memop twice, inserting a conditional jump to
// either the intermediate block which handles the memory operation or the final block (which will skip the memory op,
// if bounds checks failed). Currently, OpLoads will yield zero and OpStores will just omit the stores, if bounds checks
// failed. As multiple memory operations can be created while translating a SPIR-V memory instruction, we are collecting
// these to insert only one OOB guard. If multiple subsequent loads (for example, arr[arr[dynIndex]]) are wrapped with
// OOB checks, the algorithm makes sure that the phi node gets reused instead of just handling the loads independently.
//
// This implementation handles nested structs using arrays, simple vector / matrix accesses and 1...n-dimensional
// arrays.
//
// An example: We have the following sequence in LLVM IR:
//
// entry:
// %load = load <4 x float>, <4 x float> ... localArray[runtimeIndex][runtimeIndex * 2]
//
// which can cause a out of bounds access.
//
// However, we are converting it to a sequence like this:
//
// entry:
//
// %shouldLoad1 = icmp ult i32 runtimeIndex, localArray.dim1.upperBound
// %shouldLoad2 = icmp ult i32 runtimeIndex * 2, localArray.dim2.upperBound
// %shouldLoad = and i1 %shouldLoad1, %shouldLoad2
// br i1 %shouldLoad, label %loadBlock, label %finalBlock
//
// loadBlock:
// %load = load <4 x float>, <4 x float> ... localArray[runtimeIndex][runtimeIndex * 2]
// br label %finalBlock
//
// finalBlock:
// %finalResult = phi ... <4 x float> [ zeroinitializer, %entry], [ %load, loadBlock ]
// ... use %finalResult subsequently
//
void SPIRVToLLVM::insertScratchBoundsChecks(SPIRVValue *memOp, const ScratchBoundsCheckData &scratchBoundsCheckData,
                                            llvm::Function *parent) {
  // Hoisting the map access outside the loop in ::gatherScratchBoundsChecksMemOps creates a map entry, so we need to
  // check here.
  if (scratchBoundsCheckData.llvmInstructions.empty())
    return;

  // Helper used for checking if we need to insert a guard by determining a possible out of bounds violation at compile
  // time.
  static auto isConstantInBoundsAccess = [](SPIRVValue *accessChainIndex, uint64_t upperBound) {
    return isConstantOpCode(accessChainIndex->getOpCode()) &&
           (static_cast<SPIRVConstant *>(accessChainIndex)->getZExtIntValue() < upperBound);
  };

  SPIRVAccessChainBase *accessChain = deriveAccessChain(memOp, scratchBoundsCheckData.memOpType);
  const auto accessChainIndices = accessChain->getIndices();
  if (accessChainIndices.empty())
    return;

  SPIRVType *baseType = accessChain->getBase()->getType()->getPointerElementType();
  if (!baseType)
    return;

  Instruction *lastMemOp = scratchBoundsCheckData.llvmInstructions.back();
  getBuilder()->SetInsertPoint(lastMemOp);
  BasicBlock *checkBlock = lastMemOp->getParent();

  Value *finalCmpResult = nullptr;
  bool hasGuard = false;

  auto insertOutOfBoundsGuard = [&](SPIRVValue *accessChainIndex, uint64_t upperBound) {
    Value *comparisonValue = transValue(accessChainIndex, checkBlock->getParent(), checkBlock);
    ConstantInt *arrayUpperBound = getBuilder()->getInt32(upperBound);
    auto cmpResult = getBuilder()->CreateICmpULT(comparisonValue, arrayUpperBound);

    if (finalCmpResult)
      finalCmpResult = getBuilder()->CreateAnd(finalCmpResult, cmpResult);
    else
      finalCmpResult = cmpResult;

    hasGuard = true;
  };

  // Build a chain of OOB checks by consuming all feasible access chain indices front to back.
  for (SPIRVValue *currentIndex : accessChainIndices) {
    Op opcode = baseType->getOpCode();

    if (opcode == OpTypeArray) {
      const auto baseArray = static_cast<const SPIRVTypeArray *>(baseType);
      const uint64_t arrayLength = baseArray->getArrayLength();

      if (!isConstantInBoundsAccess(currentIndex, arrayLength))
        insertOutOfBoundsGuard(currentIndex, arrayLength);

      baseType = baseType->getArrayElementType();
    } else if (opcode == OpTypeStruct) {
      // Process struct members. The current access chain processes a struct, the indices are pointing to struct members
      // whereas the actual out of bounds guards will be inserted in further iterations.
      assert(isConstantOpCode(currentIndex->getOpCode()));

      const uint64_t structMemberIndex = static_cast<SPIRVConstant *>(currentIndex)->getZExtIntValue();
      baseType = baseType->getStructMemberType(static_cast<size_t>(structMemberIndex));
    } else if (opcode == OpTypeMatrix) {
      const auto matrix = static_cast<SPIRVTypeMatrix *>(baseType);
      auto columnCount = matrix->getColumnCount();

      // Don't insert checks for constant column indices.
      if (!isConstantInBoundsAccess(currentIndex, columnCount))
        insertOutOfBoundsGuard(currentIndex, columnCount);

      baseType = baseType->getMatrixColumnType();
    } else if (opcode == OpTypeVector) {
      const auto vector = static_cast<SPIRVTypeVector *>(baseType);
      auto componentCount = vector->getComponentCount();

      // Don't insert checks for constant component indices.
      if (!isConstantInBoundsAccess(currentIndex, componentCount))
        insertOutOfBoundsGuard(currentIndex, componentCount);

      baseType = baseType->getVectorComponentType();
    } else {
      break;
    }

    assert(baseType);
  }

  // No guard inserted, so no work to do.
  if (!hasGuard)
    return;

  Instruction *thenTerminator = SplitBlockAndInsertIfThen(finalCmpResult, lastMemOp, false);
  getBuilder()->SetInsertPoint(thenTerminator);

  // For loads, we need to insert a phi node and adjust the m_valueMap so the phi node gets used for subsequent loads.
  if (scratchBoundsCheckData.memOpType == LlvmMemOpType::IS_LOAD) {
    BasicBlock *finalBlock = thenTerminator->getParent()->getUniqueSuccessor();
    getBuilder()->SetInsertPoint(&finalBlock->front());
    PHINode *convergePhi = getBuilder()->CreatePHI(lastMemOp->getType(), 2);
    lastMemOp->replaceAllUsesWith(convergePhi);

    convergePhi->addIncoming(Constant::getNullValue(lastMemOp->getType()), checkBlock);
    convergePhi->addIncoming(lastMemOp, thenTerminator->getParent());

    if (scratchBoundsCheckData.instructionOrigin) {
      auto it = m_valueMap.find(scratchBoundsCheckData.instructionOrigin);
      if (it != m_valueMap.end())
        m_valueMap[scratchBoundsCheckData.instructionOrigin] = convergePhi;
    }

    for (auto &it : m_valueMap) {
      if (it.second == lastMemOp)
        it.second = convergePhi;
    }
  }

  // Re-create all instructions before the terminator in the then block.
  getBuilder()->SetInsertPoint(&thenTerminator->getParent()->front());
  for (Instruction *llvmInstr : scratchBoundsCheckData.llvmInstructions) {
    Instruction *clone = llvmInstr->clone();
    getBuilder()->Insert(clone);
    llvmInstr->replaceAllUsesWith(clone);
    llvmInstr->eraseFromParent();
  }
}

void SPIRVToLLVM::createXfbMetadata(bool hasXfbOuts) {
  auto llpcContext = static_cast<Llpc::Context *>(m_context);
  auto pipelineBuildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(llpcContext->getPipelineBuildInfo());
  bool needXfbMetadata = hasXfbOuts && !pipelineBuildInfo->apiXfbOutData.forceDisableStreamOut;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 69
  needXfbMetadata |= pipelineBuildInfo->apiXfbOutData.forceEnablePrimStats;
#endif

  if (!needXfbMetadata)
    return;

  // Preserve XFB relevant information in named metadata !lgc.xfb.stage, defined as follows:
  // lgc.xfb.state metadata = {
  // i32 buffer0Stream, i32 buffer0Stride, i32 buffer1Stream, i32 buffer1Stride,
  // i32 buffer2Stream, i32 buffer2Stride, i32 buffer3Stream, i32 buffer3Stride }
  // ; buffer${i}Stream is the vertex StreamId that writes to buffer i, or -1 if that particular buffer is unused (e.g.
  // StreamId in EmitVertex)
  static const unsigned MaxXfbBuffers = 4;
  std::array<unsigned, 2 * MaxXfbBuffers> xfbState{InvalidValue, 0,  // xfbBuffer[0] -> <streamId, xfbStride>
                                                   InvalidValue, 0,  // xfbBuffer[1] -> <streamId, xfbStride>
                                                   InvalidValue, 0,  // xfbBuffer[2] -> <streamId, xfbStride>
                                                   InvalidValue, 0}; // xfbBuffer[3] -> <streamId, xfbStride>
  if (hasXfbOuts && !pipelineBuildInfo->apiXfbOutData.forceDisableStreamOut) {
    const bool useXfbDecorations = pipelineBuildInfo->apiXfbOutData.numXfbOutInfo == 0;
    if (useXfbDecorations) {
      for (unsigned i = 0, e = m_bm->getNumVariables(); i != e; ++i) {
        auto bv = m_bm->getVariable(i);
        if (bv->getStorageClass() == StorageClassOutput) {
          SPIRVWord xfbBuffer = InvalidValue;
          // NOTE: XFbBuffer may be decorated on the pointer element
          SPIRVType *pointerElemTy = bv->getType()->getPointerElementType();
          SPIRVEntry *entries[2] = {bv, pointerElemTy};
          unsigned memberCount = 0;
          if (pointerElemTy->isTypeStruct())
            memberCount = pointerElemTy->getStructMemberCount();
          for (unsigned id = 0; id < 2; ++id) {
            auto entry = entries[id];
            if (entry->hasDecorate(DecorationXfbBuffer, 0, &xfbBuffer)) {
              const unsigned indexOfBuffer = 2 * xfbBuffer;
              xfbState[indexOfBuffer] = 0;
              SPIRVWord streamId = InvalidValue;
              if (entry->hasDecorate(DecorationStream, 0, &streamId))
                xfbState[indexOfBuffer] = streamId;

              SPIRVWord xfbStride = InvalidValue;
              if (entry->hasDecorate(DecorationXfbStride, 0, &xfbStride)) {
                const unsigned indexOfStride = indexOfBuffer + 1;
                xfbState[indexOfStride] = xfbStride;
              }
            } else if (id == 1) {
              for (unsigned i = 0; i < memberCount; ++i) {
                if (entry->hasMemberDecorate(i, DecorationXfbBuffer, 0, &xfbBuffer)) {
                  const unsigned indexOfBuffer = 2 * xfbBuffer;
                  xfbState[indexOfBuffer] = 0;
                  SPIRVWord streamId = InvalidValue;
                  if (entry->hasMemberDecorate(i, DecorationStream, 0, &streamId))
                    xfbState[indexOfBuffer] = streamId;

                  SPIRVWord xfbStride = InvalidValue;
                  if (entry->hasMemberDecorate(i, DecorationXfbStride, 0, &xfbStride)) {
                    const unsigned indexOfStride = indexOfBuffer + 1;
                    xfbState[indexOfStride] = xfbStride;
                  }
                }
              }
            }
          }
          if (xfbBuffer == InvalidValue)
            continue;

          // Update indexOfBuffer for block array, the N array-elements are captured by N consecutive buffers.
          SPIRVType *bt = bv->getType()->getPointerElementType();
          if (bt->isTypeArray()) {
            assert(m_valueMap.find(bv) != m_valueMap.end());
            auto output = cast<GlobalVariable>(m_valueMap[bv]);
            MDNode *metaNode = output->getMetadata(gSPIRVMD::InOut);
            assert(metaNode);
            auto elemMeta = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));
            // Find the innermost array-element
            auto elemTy = bt;
            uint64_t elemCount = 0;
            ShaderInOutMetadata outMetadata = {};
            do {
              assert(elemMeta->getNumOperands() == 4);
              outMetadata.U64All[0] = cast<ConstantInt>(elemMeta->getOperand(2))->getZExtValue();
              outMetadata.U64All[1] = cast<ConstantInt>(elemMeta->getOperand(3))->getZExtValue();
              elemCount = elemTy->getArrayLength();

              elemTy = elemTy->getArrayElementType();
              elemMeta = cast<Constant>(elemMeta->getOperand(1));
            } while (elemTy->isTypeArray());

            if (outMetadata.IsBlockArray) {
              // The even index (0,2,4,6) of !lgc.xfb.state metadata corresponds the buffer index (0~3).
              const unsigned bufferIdx = outMetadata.XfbBuffer;
              const int streamId = xfbState[2 * bufferIdx];
              const unsigned stride = xfbState[2 * bufferIdx + 1];
              for (unsigned idx = 0; idx < elemCount; ++idx) {
                const unsigned indexOfBuffer = 2 * (bufferIdx + idx);
                xfbState[indexOfBuffer] = streamId;
                xfbState[indexOfBuffer + 1] = stride;
              }
            }
          }
        }
      }
    } else {
      for (unsigned idx = 0; idx < pipelineBuildInfo->apiXfbOutData.numXfbOutInfo; ++idx) {
        const auto &xfbInfo = pipelineBuildInfo->apiXfbOutData.pXfbOutInfos[idx];
        const unsigned indexOfBuffer = 2 * xfbInfo.xfbBuffer;
        xfbState[indexOfBuffer] = xfbInfo.streamId;
        xfbState[indexOfBuffer + 1] = xfbInfo.xfbStride;
      }
    }
  }

  unsigned mdKindId = m_context->getMDKindID(lgc::XfbStateMetadataName);
  std::array<Metadata *, 2 * MaxXfbBuffers> metadatas{};
  for (unsigned i = 0; i < 2 * MaxXfbBuffers; ++i)
    metadatas[i] = ConstantAsMetadata::get(m_builder->getInt32(xfbState[i]));
  auto entryFunc = m_funcMap[m_entryTarget];
  assert(entryFunc);
  entryFunc->setMetadata(mdKindId, MDNode::get(*m_context, metadatas));
}

} // namespace SPIRV

bool llvm::readSpirv(Builder *builder, const ShaderModuleUsage *shaderInfo, const PipelineShaderOptions *shaderOptions,
                     std::istream &is, spv::ExecutionModel entryExecModel, const char *entryName,
                     const SPIRVSpecConstMap &specConstMap, ArrayRef<ConvertingSampler> convertingSamplers, Module *m,
                     std::string &errMsg) {
  assert(entryExecModel != ExecutionModelKernel && "Not support ExecutionModelKernel");

  std::unique_ptr<SPIRVModule> bm(SPIRVModule::createSPIRVModule());

  is >> *bm;

  SPIRVToLLVM btl(m, bm.get(), specConstMap, convertingSamplers, builder, shaderInfo, shaderOptions);
  bool succeed = true;
  if (!btl.translate(entryExecModel, entryName)) {
    bm->getError(errMsg);
    succeed = false;
  }

  if (DbgSaveTmpLLVM)
    dumpLLVM(m, DbgTmpLLVMFileName);

  return succeed;
}
