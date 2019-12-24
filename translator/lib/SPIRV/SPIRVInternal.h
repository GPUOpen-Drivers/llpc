//===- LLVMSPIRVInternal.h - SPIR-V internal header file --------*- C++ -*-===//
//
//                     The LLVM/SPIRV Translator
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
/// This file declares classes and functions shared by SPIR-V reader/writer.
///
//===----------------------------------------------------------------------===//
#ifndef SPIRV_SPIRVINTERNAL_H
#define SPIRV_SPIRVINTERNAL_H

#include "NameMangleAPI.h"
#include "libSPIRV/SPIRVEnum.h"
#include "libSPIRV/SPIRVError.h"
#include "libSPIRV/SPIRVNameMapEnum.h"
#include "libSPIRV/SPIRVType.h"
#include "libSPIRV/SPIRVUtil.h"
#include "LLVMSPIRVLib.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include <functional>
#include <utility>

using namespace SPIRV;
using namespace llvm;

namespace SPIRV {

/// The LLVM/SPIR-V translator version used to fill the lower 16 bits of the
/// generator's magic number in the generated SPIR-V module.
/// This number should be bumped up whenever the generated SPIR-V changes.
const static unsigned short KTranslatorVer = 14;

#define SPCV_TARGET_LLVM_IMAGE_TYPE_ENCODE_ACCESS_QUAL 0

#ifndef LLVM_DEBUG
#define LLVM_DEBUG DEBUG
#endif

class SPIRVOpaqueType;
typedef SPIRVMap<std::string, Op, SPIRVOpaqueType> SPIRVOpaqueTypeOpCodeMap;

// Ad hoc function used by LLVM/SPIRV converter for type casting
#define SPCV_CAST "spcv.cast"
#define LLVM_MEMCPY "llvm.memcpy"

template <> inline void SPIRVMap<unsigned, Op>::init() {
#define _SPIRV_OP(x, y) add(Instruction::x, Op##y);
  /* Casts */
  _SPIRV_OP(ZExt, UConvert)
  _SPIRV_OP(SExt, SConvert)
  _SPIRV_OP(Trunc, UConvert)
  _SPIRV_OP(FPToUI, ConvertFToU)
  _SPIRV_OP(FPToSI, ConvertFToS)
  _SPIRV_OP(UIToFP, ConvertUToF)
  _SPIRV_OP(SIToFP, ConvertSToF)
  _SPIRV_OP(FPTrunc, FConvert)
  _SPIRV_OP(FPExt, FConvert)
  _SPIRV_OP(PtrToInt, ConvertPtrToU)
  _SPIRV_OP(IntToPtr, ConvertUToPtr)
  _SPIRV_OP(BitCast, Bitcast)
  _SPIRV_OP(GetElementPtr, AccessChain)
  /*Binary*/
  _SPIRV_OP(And, BitwiseAnd)
  _SPIRV_OP(Or, BitwiseOr)
  _SPIRV_OP(Xor, BitwiseXor)
  _SPIRV_OP(Add, IAdd)
  _SPIRV_OP(FAdd, FAdd)
  _SPIRV_OP(Sub, ISub)
  _SPIRV_OP(FSub, FSub)
  _SPIRV_OP(Mul, IMul)
  _SPIRV_OP(FMul, FMul)
  _SPIRV_OP(UDiv, UDiv)
  _SPIRV_OP(SDiv, SDiv)
  _SPIRV_OP(FDiv, FDiv)
  _SPIRV_OP(SRem, SRem)
  _SPIRV_OP(FRem, FRem)
  _SPIRV_OP(URem, UMod)
  _SPIRV_OP(Shl, ShiftLeftLogical)
  _SPIRV_OP(LShr, ShiftRightLogical)
  _SPIRV_OP(AShr, ShiftRightArithmetic)
#undef _SPIRV_OP
}
typedef SPIRVMap<unsigned, Op> OpCodeMap;

template <> inline void SPIRVMap<CmpInst::Predicate, Op>::init() {
#define _SPIRV_OP(x, y) add(CmpInst::x, Op##y);
  _SPIRV_OP(FCMP_OEQ, FOrdEqual)
  _SPIRV_OP(FCMP_OGT, FOrdGreaterThan)
  _SPIRV_OP(FCMP_OGE, FOrdGreaterThanEqual)
  _SPIRV_OP(FCMP_OLT, FOrdLessThan)
  _SPIRV_OP(FCMP_OLE, FOrdLessThanEqual)
  _SPIRV_OP(FCMP_ONE, FOrdNotEqual)
  _SPIRV_OP(FCMP_UEQ, FUnordEqual)
  _SPIRV_OP(FCMP_UGT, FUnordGreaterThan)
  _SPIRV_OP(FCMP_UGE, FUnordGreaterThanEqual)
  _SPIRV_OP(FCMP_ULT, FUnordLessThan)
  _SPIRV_OP(FCMP_ULE, FUnordLessThanEqual)
  _SPIRV_OP(FCMP_UNE, FUnordNotEqual)
  _SPIRV_OP(ICMP_EQ, IEqual)
  _SPIRV_OP(ICMP_NE, INotEqual)
  _SPIRV_OP(ICMP_UGT, UGreaterThan)
  _SPIRV_OP(ICMP_UGE, UGreaterThanEqual)
  _SPIRV_OP(ICMP_ULT, ULessThan)
  _SPIRV_OP(ICMP_ULE, ULessThanEqual)
  _SPIRV_OP(ICMP_SGT, SGreaterThan)
  _SPIRV_OP(ICMP_SGE, SGreaterThanEqual)
  _SPIRV_OP(ICMP_SLT, SLessThan)
  _SPIRV_OP(ICMP_SLE, SLessThanEqual)
#if SPV_VERSION >= 0x10400
  _SPIRV_OP(ICMP_EQ, PtrEqual)
  _SPIRV_OP(ICMP_NE, PtrNotEqual)
#endif
#undef _SPIRV_OP
}
typedef SPIRVMap<CmpInst::Predicate, Op> CmpMap;

class IntBoolOpMapId;
template <> inline void SPIRVMap<Op, Op, IntBoolOpMapId>::init() {
  add(OpNot, OpLogicalNot);
  add(OpBitwiseAnd, OpLogicalAnd);
  add(OpBitwiseOr, OpLogicalOr);
  add(OpBitwiseXor, OpLogicalNotEqual);
  add(OpIEqual, OpLogicalEqual);
  add(OpINotEqual, OpLogicalNotEqual);
}
typedef SPIRVMap<Op, Op, IntBoolOpMapId> IntBoolOpMap;

#define SPIR_TARGETTRIPLE32 "spir-unknown-unknown"
#define SPIR_TARGETTRIPLE64 "spir64-unknown-unknown"
#define SPIR_DATALAYOUT32                                                      \
  "e-p:32:32:32-i1:8:8-i8:8:8-i16:16:16-i32:32:32"                             \
  "-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32"                         \
  "-v32:32:32-v48:64:64-v64:64:64-v96:128:128"                                 \
  "-v128:128:128-v192:256:256-v256:256:256"                                    \
  "-v512:512:512-v1024:1024:1024"
#define SPIR_DATALAYOUT64                                                      \
  "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32"                             \
  "-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32"                         \
  "-v32:32:32-v48:64:64-v64:64:64-v96:128:128"                                 \
  "-v128:128:128-v192:256:256-v256:256:256"                                    \
  "-v512:512:512-v1024:1024:1024"

enum SPIRAddressSpace {
  SPIRAS_Generic   = 0,  // AMDGPUAS::FLAT_ADDRESS
  SPIRAS_Global    = 1,  // AMDGPUAS::GLOBAL_ADDRESS
  SPIRAS_Local     = 3,  // AMDGPUAS::LOCAL_ADDRESS
  SPIRAS_Constant  = 4,  // AMDGPUAS::CONSTANT_ADDRESS
  SPIRAS_Private   = 5,  // AMDGPUAS::PRIVATE_ADDRESS
  SPIRAS_Uniform   = 7,  // Memory buffer descriptor
  SPIRAS_Input     = 64,
  SPIRAS_Output    = 65,
  SPIRAS_Count,
};

template <> inline void SPIRVMap<SPIRAddressSpace, std::string>::init() {
  add(SPIRAS_Private, "Private");
  add(SPIRAS_Global, "Global");
  add(SPIRAS_Constant, "Constant");
  add(SPIRAS_Local, "Local");
  add(SPIRAS_Generic, "Generic");
  add(SPIRAS_Input, "Input");
  add(SPIRAS_Output, "Output");
  add(SPIRAS_Uniform, "Uniform");
}

template <>
inline void SPIRVMap<SPIRAddressSpace, SPIRVStorageClassKind>::init() {
  add(SPIRAS_Private, StorageClassFunction);
  add(SPIRAS_Global, StorageClassCrossWorkgroup);
  add(SPIRAS_Constant, StorageClassUniformConstant);
  add(SPIRAS_Local, StorageClassWorkgroup);
  add(SPIRAS_Generic, StorageClassGeneric);
  add(SPIRAS_Input, StorageClassInput);
  add(SPIRAS_Output, StorageClassOutput);
  add(SPIRAS_Uniform, StorageClassUniform);
  add(SPIRAS_Private, StorageClassPrivate);
  add(SPIRAS_Constant, StorageClassPushConstant);
  add(SPIRAS_Uniform, StorageClassStorageBuffer);
  add(SPIRAS_Global, StorageClassPhysicalStorageBufferEXT);
}
typedef SPIRVMap<SPIRAddressSpace, SPIRVStorageClassKind> SPIRSPIRVAddrSpaceMap;

template <>
inline void
SPIRVMap<Attribute::AttrKind, SPIRVFunctionControlMaskKind>::init() {
  add(Attribute::ReadNone, FunctionControlPureMask);
  add(Attribute::ReadOnly, FunctionControlConstMask);
  add(Attribute::AlwaysInline, FunctionControlInlineMask);
  add(Attribute::NoInline, FunctionControlDontInlineMask);
}
typedef SPIRVMap<Attribute::AttrKind, SPIRVFunctionControlMaskKind>
    SPIRSPIRVFuncCtlMaskMap;

namespace kLLVMTypeName {
const static char StructPrefix[] = "struct.";
} // namespace kLLVMTypeName

namespace kSPIRVTypeName {
const static char Delimiter = '.';
const static char Image[] = "Image";
const static char PostfixDelim = '_';
const static char Prefix[] = "spirv";
const static char PrefixAndDelim[] = "spirv.";
const static char SampledImg[] = "SampledImage";
const static char Sampler[] = "Sampler";
const static char VariablePtr[] = "VarPtr";
} // namespace kSPIRVTypeName

namespace kSPIRVName {
const static char GroupPrefix[] = "group_";
const static char Prefix[] = "__spirv_";
const static char Postfix[] = "__";
const static char ImageQuerySize[] = "ImageQuerySize";
const static char ImageQuerySizeLod[] = "ImageQuerySizeLod";
const static char ImageSampleExplicitLod[] = "ImageSampleExplicitLod";
const static char ReservedPrefix[] = "reserved_";
const static char SampledImage[] = "SampledImage";
const static char TempSampledImage[] = "TempSampledImage";
} // namespace kSPIRVName

namespace gSPIRVMD {
  const static char Prefix[]            = "spirv.";
  const static char InOut[]             = "spirv.InOut";
  const static char Block[]             = "spirv.Block";
  const static char PushConst[]         = "spirv.PushConst";
  const static char Resource[]          = "spirv.Resource";
  const static char ExecutionModel[]    = "spirv.ExecutionModel";
  const static char ImageCall[]         = "spirv.ImageCall";
  const static char ImageMemory[]       = "spirv.ImageMemory";
  const static char BufferLoad[]        = "spirv.BufferLoad";
  const static char BufferStore[]       = "spirv.BufferStore";
  const static char AccessChain[]       = "spirv.AccessChain";
  const static char StorageBufferCall[] = "spirv.StorageBufferCall";
  const static char NonUniform[]        = "spirv.NonUniform";
}

enum SPIRVBlockTypeKind {
  BlockTypeUnknown,
  BlockTypeUniform,
  BlockTypeShaderStorage,
};

enum SPIRVInterpModeKind {
  InterpModeSmooth,
  InterpModeFlat,
  InterpModeNoPersp,
  InterpModeCustom,
};

enum SPIRVInterpLocKind {
  InterpLocUnknown,
  InterpLocCenter,
  InterpLocCentroid,
  InterpLocSample,
  InterpLocCustom,
};

enum SPIRVImageOpKind {
  ImageOpSample,
  ImageOpFetch,
  ImageOpGather,
  ImageOpQueryNonLod,
  ImageOpQueryLod,
  ImageOpRead,
  ImageOpWrite,
  ImageOpAtomicLoad,
  ImageOpAtomicStore,
  ImageOpAtomicExchange,
  ImageOpAtomicCompareExchange,
  ImageOpAtomicIIncrement,
  ImageOpAtomicIDecrement,
  ImageOpAtomicIAdd,
  ImageOpAtomicISub,
  ImageOpAtomicSMin,
  ImageOpAtomicUMin,
  ImageOpAtomicSMax,
  ImageOpAtomicUMax,
  ImageOpAtomicAnd,
  ImageOpAtomicOr,
  ImageOpAtomicXor
};

template<> inline void
SPIRVMap<SPIRVImageOpKind, std::string>::init() {
  add(ImageOpSample,                "sample");
  add(ImageOpFetch,                 "fetch");
  add(ImageOpGather,                "gather");
  add(ImageOpQueryNonLod,           "querynonlod");
  add(ImageOpQueryLod,              "querylod");
  add(ImageOpRead,                  "read");
  add(ImageOpWrite,                 "write");
  add(ImageOpAtomicLoad,            "atomicload");
  add(ImageOpAtomicStore,           "atomicstore");
  add(ImageOpAtomicExchange,        "atomicexchange");
  add(ImageOpAtomicCompareExchange, "atomiccompexchange");
  add(ImageOpAtomicIIncrement,      "atomiciincrement");
  add(ImageOpAtomicIDecrement,      "atomicidecrement");
  add(ImageOpAtomicIAdd,            "atomiciadd");
  add(ImageOpAtomicISub,            "atomicisub");
  add(ImageOpAtomicSMin,            "atomicsmin");
  add(ImageOpAtomicUMin,            "atomicumin");
  add(ImageOpAtomicSMax,            "atomicsmax");
  add(ImageOpAtomicUMax,            "atomicumax");
  add(ImageOpAtomicAnd,             "atomicand");
  add(ImageOpAtomicOr,              "atomicor");
  add(ImageOpAtomicXor,             "atomicxor");
}
typedef SPIRVMap<SPIRVImageOpKind, std::string> SPIRVImageOpKindNameMap;

class ImageQueryOpKindNameMapId;
template<> inline void
SPIRVMap<Op, std::string, ImageQueryOpKindNameMapId>::init() {
  add(OpImageQuerySizeLod,  ".sizelod");
  add(OpImageQuerySize,     ".sizelod");    // Note: OpImageQuerySize is
                                            // implemented as OpImageQuerySizeLod
                                            // with lod = 0
  add(OpImageQueryLod,      ".lod");
  add(OpImageQueryLevels,   ".levels");
  add(OpImageQuerySamples,  ".samples");
}
typedef SPIRVMap<Op, std::string, ImageQueryOpKindNameMapId> SPIRVImageQueryOpKindNameMap;

union SPIRVImageOpInfo {
  struct {
    SPIRVImageOpKind OpKind               : 6;  // Kind of image operation
    uint32_t         OperMask             : 3;  // Index of image operand mask
    uint32_t         OperDref             : 3;  // Index of Dref operand
    uint32_t         HasProj              : 1;  // Whether project is present
    uint32_t         IsSparse             : 1;  // Is sparse image operation
    uint32_t         OperAtomicData       : 3;  // Index of atomic value
                                                // operand
    uint32_t         OperAtomicComparator : 3;  // Index of atomic comparator
                                                // operand (valid for
                                                // CompareExchange)
    uint32_t         OperScope            : 3;  // Index of the scope (valid for atomics)
    uint32_t         Unused               : 9;
  };
  uint32_t           U32All;
};

static const uint32_t InvalidOperIdx = 0x7;

template<> inline void
SPIRVMap<Op, SPIRVImageOpInfo>::init() {
  //       Image OpCode                           OpCode Kind                 Mask              ref             Proj    Sparse  AtomicData      AtomicComparator    Scope
  //---------------------------------------------------------------------------------------------------------------------------------------------------------------------
  add(OpImageSampleImplicitLod,               { ImageOpSample,                  2,              InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSampleExplicitLod,               { ImageOpSample,                  2,              InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSampleDrefImplicitLod,           { ImageOpSample,                  3,              3,              false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSampleDrefExplicitLod,           { ImageOpSample,                  3,              3,              false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSampleProjImplicitLod,           { ImageOpSample,                  2,              InvalidOperIdx, true,   false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSampleProjExplicitLod,           { ImageOpSample,                  2,              InvalidOperIdx, true,   false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSampleProjDrefImplicitLod,       { ImageOpSample,                  3,              3,              true,   false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSampleProjDrefExplicitLod,       { ImageOpSample,                  3,              3,              true,   false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageFetch,                           { ImageOpFetch,                   2,              InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageGather,                          { ImageOpGather,                  3,              InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageDrefGather,                      { ImageOpGather,                  3,              3,              false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageRead,                            { ImageOpRead,                    2,              InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageWrite,                           { ImageOpWrite,                   3,              InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });

  add(OpImageSparseSampleImplicitLod,         { ImageOpSample,                  2,              InvalidOperIdx, false,  true,   InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSparseSampleExplicitLod,         { ImageOpSample,                  2,              InvalidOperIdx, false,  true,   InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSparseSampleDrefImplicitLod,     { ImageOpSample,                  3,              3,              false,  true,   InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSparseSampleDrefExplicitLod,     { ImageOpSample,                  3,              3,              false,  true,   InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSparseSampleProjImplicitLod,     { ImageOpSample,                  2,              InvalidOperIdx, true,   true,   InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSparseSampleProjExplicitLod,     { ImageOpSample,                  2,              InvalidOperIdx, true,   true,   InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSparseSampleProjDrefImplicitLod, { ImageOpSample,                  3,              3,              true,   true,   InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSparseSampleProjDrefExplicitLod, { ImageOpSample,                  3,              3,              true,   true,   InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSparseFetch,                     { ImageOpFetch,                   2,              InvalidOperIdx, false,  true,   InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSparseGather,                    { ImageOpGather,                  3,              InvalidOperIdx, false,  true,   InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSparseDrefGather,                { ImageOpGather,                  3,              3,              false,  true,   InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageSparseRead,                      { ImageOpRead,                    2,              InvalidOperIdx, false,  true,   InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });

  add(OpImageQuerySizeLod,                    { ImageOpQueryNonLod,             InvalidOperIdx, InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageQuerySize,                       { ImageOpQueryNonLod,             InvalidOperIdx, InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageQueryLod,                        { ImageOpQueryLod,                InvalidOperIdx, InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageQueryLevels,                     { ImageOpQueryNonLod,             InvalidOperIdx, InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });
  add(OpImageQuerySamples,                    { ImageOpQueryNonLod,             InvalidOperIdx, InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, InvalidOperIdx });

  add(OpAtomicLoad,                           { ImageOpAtomicLoad,              InvalidOperIdx, InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, 1 });
  add(OpAtomicStore,                          { ImageOpAtomicStore,             InvalidOperIdx, InvalidOperIdx, false,  false,  3,              InvalidOperIdx, 1 });
  add(OpAtomicExchange,                       { ImageOpAtomicExchange,          InvalidOperIdx, InvalidOperIdx, false,  false,  3,              InvalidOperIdx, 1 });
  add(OpAtomicCompareExchange,                { ImageOpAtomicCompareExchange,   InvalidOperIdx, InvalidOperIdx, false,  false,  4,              5,              1 });
  add(OpAtomicIIncrement,                     { ImageOpAtomicIIncrement,        InvalidOperIdx, InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, 1 });
  add(OpAtomicIDecrement,                     { ImageOpAtomicIDecrement,        InvalidOperIdx, InvalidOperIdx, false,  false,  InvalidOperIdx, InvalidOperIdx, 1 });
  add(OpAtomicIAdd,                           { ImageOpAtomicIAdd,              InvalidOperIdx, InvalidOperIdx, false,  false,  3,              InvalidOperIdx, 1 });
  add(OpAtomicISub,                           { ImageOpAtomicISub,              InvalidOperIdx, InvalidOperIdx, false,  false,  3,              InvalidOperIdx, 1 });
  add(OpAtomicSMin,                           { ImageOpAtomicSMin,              InvalidOperIdx, InvalidOperIdx, false,  false,  3,              InvalidOperIdx, 1 });
  add(OpAtomicUMin,                           { ImageOpAtomicUMin,              InvalidOperIdx, InvalidOperIdx, false,  false,  3,              InvalidOperIdx, 1 });
  add(OpAtomicSMax,                           { ImageOpAtomicSMax,              InvalidOperIdx, InvalidOperIdx, false,  false,  3,              InvalidOperIdx, 1 });
  add(OpAtomicUMax,                           { ImageOpAtomicUMax,              InvalidOperIdx, InvalidOperIdx, false,  false,  3,              InvalidOperIdx, 1 });
  add(OpAtomicAnd,                            { ImageOpAtomicAnd,               InvalidOperIdx, InvalidOperIdx, false,  false,  3,              InvalidOperIdx, 1 });
  add(OpAtomicOr,                             { ImageOpAtomicOr,                InvalidOperIdx, InvalidOperIdx, false,  false,  3,              InvalidOperIdx, 1 });
  add(OpAtomicXor,                            { ImageOpAtomicXor,               InvalidOperIdx, InvalidOperIdx, false,  false,  3,              InvalidOperIdx, 1 });
}
typedef SPIRVMap<Op, SPIRVImageOpInfo> SPIRVImageOpInfoMap;

// "<" operator overloading, just to pass compilation for "SPIRVMap::rmap",
// not actually used
inline bool operator<(const SPIRVImageOpInfo &L, const SPIRVImageOpInfo &R) {
  return L.U32All < R.U32All;
}

/// Additional information for mangling a function argument type.
struct BuiltinArgTypeMangleInfo {
  bool IsSigned;
  bool IsVoidPtr;
  bool IsEnum;
  bool IsSampler;
  bool IsAtomic;
  bool IsLocalArgBlock;
  SPIR::TypePrimitiveEnum Enum;
  unsigned Attr;
  BuiltinArgTypeMangleInfo()
      : IsSigned(true), IsVoidPtr(false), IsEnum(false), IsSampler(false),
        IsAtomic(false), IsLocalArgBlock(false), Enum(SPIR::PRIMITIVE_NONE),
        Attr(0) {}
};

/// Information for mangling builtin function.
class BuiltinFuncMangleInfo {
public:
  /// Translate builtin function name and set
  /// argument attributes and unsigned args.
  BuiltinFuncMangleInfo(const std::string &UniqName = "")
      : LocalArgBlockIdx(-1), VarArgIdx(-1) {
    if (!UniqName.empty())
      init(UniqName);
  }
  virtual ~BuiltinFuncMangleInfo() {}
  const std::string &getUnmangledName() const { return UnmangledName; }
  void addUnsignedArg(int Ndx) { UnsignedArgs.insert(Ndx); }
  void addVoidPtrArg(int Ndx) { VoidPtrArgs.insert(Ndx); }
  void addSamplerArg(int Ndx) { SamplerArgs.insert(Ndx); }
  void addAtomicArg(int Ndx) { AtomicArgs.insert(Ndx); }
  void setLocalArgBlock(int Ndx) {
    assert(0 <= Ndx && "it is not allowed to set less than zero index");
    LocalArgBlockIdx = Ndx;
  }
  void setEnumArg(int Ndx, SPIR::TypePrimitiveEnum Enum) {
    EnumArgs[Ndx] = Enum;
  }
  void setArgAttr(int Ndx, unsigned Attr) { Attrs[Ndx] = Attr; }
  void setVarArg(int Ndx) {
    assert(0 <= Ndx && "it is not allowed to set less than zero index");
    VarArgIdx = Ndx;
  }
  bool isArgUnsigned(int Ndx) {
    return UnsignedArgs.count(-1) || UnsignedArgs.count(Ndx);
  }
  bool isArgVoidPtr(int Ndx) {
    return VoidPtrArgs.count(-1) || VoidPtrArgs.count(Ndx);
  }
  bool isArgSampler(int Ndx) { return SamplerArgs.count(Ndx); }
  bool isArgAtomic(int Ndx) { return AtomicArgs.count(Ndx); }
  bool isLocalArgBlock(int Ndx) { return LocalArgBlockIdx == Ndx; }
  bool isArgEnum(int Ndx, SPIR::TypePrimitiveEnum *Enum = nullptr) {
    auto Loc = EnumArgs.find(Ndx);
    if (Loc == EnumArgs.end())
      Loc = EnumArgs.find(-1);
    if (Loc == EnumArgs.end())
      return false;
    if (Enum)
      *Enum = Loc->second;
    return true;
  }
  unsigned getArgAttr(int Ndx) {
    auto Loc = Attrs.find(Ndx);
    if (Loc == Attrs.end())
      Loc = Attrs.find(-1);
    if (Loc == Attrs.end())
      return 0;
    return Loc->second;
  }
  // get ellipsis index, single ellipsis at the end of the function is possible
  // only return value < 0 if none
  int getVarArg() const { return VarArgIdx; }
  BuiltinArgTypeMangleInfo getTypeMangleInfo(int Ndx) {
    BuiltinArgTypeMangleInfo Info;
    Info.IsSigned = !isArgUnsigned(Ndx);
    Info.IsVoidPtr = isArgVoidPtr(Ndx);
    Info.IsEnum = isArgEnum(Ndx, &Info.Enum);
    Info.IsSampler = isArgSampler(Ndx);
    Info.IsAtomic = isArgAtomic(Ndx);
    Info.IsLocalArgBlock = isLocalArgBlock(Ndx);
    Info.Attr = getArgAttr(Ndx);
    return Info;
  }
  virtual void init(const std::string &UniqUnmangledName) {
    UnmangledName = UniqUnmangledName;
  }

protected:
  std::string UnmangledName;
  std::set<int> UnsignedArgs; // unsigned arguments, or -1 if all are unsigned
  std::set<int> VoidPtrArgs;  // void pointer arguments, or -1 if all are void
                              // pointer
  std::set<int> SamplerArgs;  // sampler arguments
  std::set<int> AtomicArgs;   // atomic arguments
  std::map<int, SPIR::TypePrimitiveEnum> EnumArgs; // enum arguments
  std::map<int, unsigned> Attrs;                   // argument attributes
  int LocalArgBlockIdx; // index of a block with local arguments, idx < 0 if
                        // none
  int VarArgIdx;        // index of ellipsis argument, idx < 0 if none
};

/// \returns a vector of types for a collection of values.
template <class T> std::vector<Type *> getTypes(T V) {
  std::vector<Type *> Tys;
  for (auto &I : V)
    Tys.push_back(I->getType());
  return Tys;
}

/// Move elements of std::vector from [begin, end) to target.
template <typename T>
void move(std::vector<T> &V, size_t Begin, size_t End, size_t Target) {
  assert(Begin < End && End <= V.size() && Target <= V.size() &&
         !(Begin < Target && Target < End));
  if (Begin <= Target && Target <= End)
    return;
  auto B = V.begin() + Begin, E = V.begin() + End;
  if (Target > V.size())
    Target = V.size();
  if (Target > End)
    Target -= (End - Begin);
  std::vector<T> Segment(B, E);
  V.erase(B, E);
  V.insert(V.begin() + Target, Segment.begin(), Segment.end());
}

void removeFnAttr(LLVMContext *Context, CallInst *Call,
                  Attribute::AttrKind Attr);
void addFnAttr(LLVMContext *Context, CallInst *Call, Attribute::AttrKind Attr);

Function *getOrCreateFunction(Module *M, Type *RetTy, ArrayRef<Type *> ArgTypes,
                              StringRef Name,
                              BuiltinFuncMangleInfo *Mangle = nullptr,
                              AttributeList *Attrs = nullptr,
                              bool TakeName = true);

/// Check if a function type is void(void).
bool isVoidFuncTy(FunctionType *FT);

void dumpUsers(Value *V, StringRef Prompt = "");

bool eraseUselessFunctions(Module *M);

/// Erase a function if it is declaration, has internal linkage and has no use.
bool eraseIfNoUse(Function *F);

void eraseIfNoUse(Value *V);

// 4 DWORD size of buffer descriptor
static const uint32_t DescriptorSizeBuffer = 4;

/// Mangle builtin function name.
/// \return \param UniqName if \param BtnInfo is null pointer, otherwise
///    return IA64 mangled name.
std::string mangleBuiltin(const std::string &UniqName,
                          ArrayRef<Type *> ArgTypes,
                          BuiltinFuncMangleInfo *BtnInfo);

template <> inline void SPIRVMap<std::string, Op, SPIRVOpaqueType>::init() {
  add(kSPIRVTypeName::Image, OpTypeImage);
  add(kSPIRVTypeName::Sampler, OpTypeSampler);
  add(kSPIRVTypeName::SampledImg, OpTypeSampledImage);
}

/// Metadata for shader inputs and outputs, valid for scalar or vector type.
union ShaderInOutMetadata {
  struct
  {
    // BYTE 0~1
    uint64_t Value              : 16; // Generic location or SPIR-V built-in ID
    // BYTE 2
    uint64_t Index              : 1;  // Output index for dual source blending
    uint64_t IsLoc              : 1;  // Whether value is a location
    uint64_t IsBuiltIn          : 1;  // Whether value is a SPIR-V built-in ID
    uint64_t Component          : 2;  // Component offset of inputs and outputs
    uint64_t Signedness         : 1;  // Signedness of the input/output, valid
                                      // for integer (0 - unsigned, 1 - signed)
    uint64_t InterpMode         : 2;  // Interpolation mode (fragment shader)
    // BYTE 3
    uint64_t InterpLoc          : 3;  // Interpolation location (fragment
                                      // shader)
    uint64_t PerPatch           : 1;  // Whether this is a per-patch input/
                                      // output (tessellation shader)
    uint64_t StreamId           : 2;  // ID of output stream (geometry shader)
    uint64_t XfbBuffer          : 2;  // Transform feedback buffer ID
    // BYTE 4~5
    uint64_t IsXfb              : 1;  // Whether this is for transform feedback
    uint64_t XfbOffset          : 15; // Transform feedback offset
    // BYTE 6~7
    uint64_t XfbStride          : 16; // Transform feedback stride
    // BYTE 8~9
    uint64_t IsBlockArray       : 1;  // Whether we are handling block array
    uint64_t XfbArrayStride     : 16; // Transform feedback array stride (for
                                      //   block array, it's flatten dimension
                                      //   of an element (1 if element is not
                                      //   sub-array; for non block array, it's
                                      //   occupied byte count of an element)
    // BYTE 10~11
    uint64_t XfbExtraOffset     : 16; // Transform feedback extra offset
  };
  uint64_t U64All[2];
};

/// Info structure for all decorations applied to shader inputs and outputs.
struct ShaderInOutDecorate {
  union
  {
    uint32_t     BuiltIn;           // SPIR-V built-in ID
    uint32_t     Loc;               // Location of generic inputs and outputs

    uint32_t     U32All;
  } Value;

  uint32_t       Index;             // Output index for dual source blending

  bool           IsBuiltIn;         // Whether this is a SPIR-V built-in

  bool           IsXfb;             // Whether this is a for transform feedback

  bool           IsBlockArray;      // Whether we are handling a block array

  uint32_t       Component;         // Component offset of inputs and outputs

  bool           PerPatch;          // Whether this is a per-patch input/output
                                    // (tessellation shader)
  struct
  {
      SPIRVInterpModeKind   Mode;   // Interpolation mode
      SPIRVInterpLocKind    Loc;    // Interpolation location
  } Interp;

  uint32_t       StreamId;           // ID of output stream (geometry shader)
  uint32_t       XfbBuffer;          // Transform feedback buffer ID
  uint32_t       XfbOffset;          // Transform feedback offset
  uint32_t       XfbStride;          // Transform feedback stride
  uint32_t       XfbExtraOffset;     // Transform feedback extra offset
  uint32_t       XfbArrayStride;     // Transform feedback array stride (for
                                     //   block array, it's flatten dimension
                                     //   of an element (1 if element is not
                                     //   sub-array; for non block array, it's
                                     //   occupied byte count of an element)
  bool           contains64BitType;  // Whether contains 64-bit type
};

/// Metadata for shader block.
union ShaderBlockMetadata {
  struct
  {
    uint32_t offset       : 32; // Offset (bytes) in block
    uint32_t IsMatrix     : 1;  // Whether it it is a matrix
    uint32_t IsRowMajor   : 1;  // Whether it is a "row_major" qualified matrix
    uint32_t MatrixStride : 6;  // Matrix stride, valid for matrix
    uint32_t Restrict     : 1;  // Whether "restrict" qualifier is present
    uint32_t Coherent     : 1;  // Whether "coherent" qualifier is present
    uint32_t Volatile     : 1;  // Whether "volatile" qualifier is present
    uint32_t NonWritable  : 1;  // Whether "readonly" qualifier is present
    uint32_t NonReadable  : 1;  // Whether "writeonly" qualifier is present
    uint32_t IsPointer    : 1;  // Whether it is a pointer
    uint32_t IsStruct     : 1;  // Whether it is a structure
    uint32_t Unused       : 17;
  };
  uint64_t U64All;
};

/// Info structure for all decorations applied to shader block.
struct ShaderBlockDecorate {
  uint32_t    Offset;       // Offset (bytes) in block
  bool        IsMatrix;     // Whether it is a matrix
  bool        IsRowMajor;   // Whether it is a "row_major" qualified matrix
  uint32_t    MatrixStride; // Matrix stride, valid for matrix
  bool        Restrict;     // Whether "restrict" qualifier is present
  bool        Coherent;     // Whether "coherent" qualifier is present
  bool        Volatile;     // Whether "volatile" qualifier is present
  bool        NonWritable;  // Whether "readonly" qualifier is present
  bool        NonReadable;  // Whether "writeonly" qualifier is present
};

/// Metadata for image emulation call.
union ShaderImageCallMetadata {
  struct {
    SPIRVImageOpKind  OpKind             : 6; // Kind of image operation
    uint32_t          Dim                : 3; // Image dimension
    uint32_t          Arrayed            : 1; // Whether image is arrayed
    uint32_t          Multisampled       : 1; // Whether image is multisampled
    uint32_t          NonUniformSampler  : 1; // Whether sampler is non-uniform
    uint32_t          NonUniformResource : 1; // Whether resource is non-uniform
    uint32_t          WriteOnly          : 1; // Whetehr it is a write-only operation
    uint32_t          Unused             : 18;
  };
  uint32_t U32All;
};

/// Metadata for image memory (memory qualifiers)
union ShaderImageMemoryMetadata {
  struct {
    uint32_t Restrict     : 1;    // Whether "restrict" qualifier is present
    uint32_t Coherent     : 1;    // Whether "coherent" qualifier is present
    uint32_t Volatile     : 1;    // Whether "volatile" qualifier is present
    uint32_t NonWritable  : 1;    // Whether "readonly" qualifier is present
    uint32_t NonReadable  : 1;    // Whether "writeonly" qualifier is present

    uint32_t Unused       : 27;
  };
  uint32_t U32All;
};

/// Flags used for floating-point control
union ShaderFloatControlFlags {
  struct {
  uint32_t DenormPerserve           : 4;    // Preserve denormals
  uint32_t DenormFlushToZero        : 4;    // Flush denormals to zeros
  uint32_t SignedZeroInfNanPreserve : 4;    // Preserve signed zero, INF, NaN
  uint32_t RoundingModeRTE          : 4;    // Rounding to even
  uint32_t RoundingModeRTZ          : 4;    // Rounding to zero

  uint32_t Unused                   : 12;
  };
  uint32_t U32All;
};

/// Metadata for execution modes of each shader entry-point
union ShaderExecModeMetadata {
  struct {
    struct
    {
      ShaderFloatControlFlags FpControlFlags; // Floating-point control flags
    } common;

    union {
      struct {
        uint32_t Xfb                      : 1;  // Transform feedback mode
        uint32_t Unused                   : 31;
      } vs;

      struct {
        uint32_t SpacingEqual             : 1;  // Layout "equal_spacing"
        uint32_t SpacingFractionalEven    : 1;  // Layout "fractional_even_spacing"
        uint32_t SpacingFractionalOdd     : 1;  // Layout "fractional_odd_spacing"
        uint32_t VertexOrderCw            : 1;  // Layout "cw"
        uint32_t VertexOrderCcw           : 1;  // Layout "ccw"
        uint32_t PointMode                : 1;  // Layout "point_mode"
        uint32_t Triangles                : 1;  // Layout "triangles"
        uint32_t Quads                    : 1;  // Layout "quads"
        uint32_t Isolines                 : 1;  // Layout "isolines"
        uint32_t Xfb                      : 1;  // Transform feedback mode
        uint32_t Unused                   : 22;

        uint32_t OutputVertices;                // Layout "vertices ="
      } ts;

      struct {
        uint32_t InputPoints              : 1;  // Layout "points"
        uint32_t InputLines               : 1;  // Layout "lines"
        uint32_t InputLinesAdjacency      : 1;  // Layout "lines_adjacency"
        uint32_t Triangles                : 1;  // Layout "triangles"
        uint32_t InputTrianglesAdjacency  : 1;  // Layout "triangles_adjacency"
        uint32_t OutputPoints             : 1;  // Layout "points"
        uint32_t OutputLineStrip          : 1;  // Layout "line_strip"
        uint32_t OutputTriangleStrip      : 1;  // Layout "triangle_strip"
        uint32_t Xfb                      : 1;  // Transform feedback mode
        uint32_t Unused                   : 23;

        uint32_t Invocations;                   // Layout "invocations ="
        uint32_t OutputVertices;                // Layout "max_vertices ="
      } gs;

      struct {
        uint32_t OriginUpperLeft          : 1;  // Layout "origin_upper_left"
        uint32_t PixelCenterInteger       : 1;  // Layout "pixel_center_integer"
        uint32_t EarlyFragmentTests       : 1;  // Layout "early_fragment_tests"
        uint32_t DepthUnchanged           : 1;  // Layout "depth_unchanged"
        uint32_t DepthGreater             : 1;  // Layout "depth_greater"
        uint32_t DepthLess                : 1;  // Layout "depth_less"
        uint32_t DepthReplacing           : 1;  // Layout "depth_any"
        uint32_t PostDepthCoverage        : 1;  // Layout "post_depth_coverage"
        uint32_t Unused                   : 24;
      } fs;

      struct {
        uint32_t LocalSizeX;                    // Layout "local_size_x ="
        uint32_t LocalSizeY;                    // Layout "local_size_y ="
        uint32_t LocalSizeZ;                    // Layout "local_size_z ="
      } cs;
    };
  };
  uint32_t U32All[4];
};

} // namespace SPIRV

#endif // SPIRV_SPIRVINTERNAL_H
