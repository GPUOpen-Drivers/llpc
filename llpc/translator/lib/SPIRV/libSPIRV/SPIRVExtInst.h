//===- SPIRVBuiltin.h - SPIR-V extended instruction --------------*- C++ -*-===//
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
/// This file defines SPIR-V extended instructions.
///
//===----------------------------------------------------------------------===//

#ifndef SPIRV_LIBSPIRV_SPIRVEXTINST_H
#define SPIRV_LIBSPIRV_SPIRVEXTINST_H

#include "SPIRV.debug.h"
#include "SPIRVEnum.h"
#include "SPIRVUtil.h"
#include <string>
#include <vector>

namespace SPIRV {

inline bool isGLSLBuiltinSet(SPIRVExtInstSetKind Set) {
  return Set == SPIRVEIS_GLSL;
}

typedef GLSLstd450 GLSLExtOpKind;

template <> inline void SPIRVMap<GLSLExtOpKind, std::string>::init() {
  add(GLSLstd450Round, "round");
  add(GLSLstd450RoundEven, "roundEven");
  add(GLSLstd450Trunc, "trunc");
  add(GLSLstd450FAbs, "fabs");
  add(GLSLstd450SAbs, "sabs");
  add(GLSLstd450FSign, "fsign");
  add(GLSLstd450SSign, "ssign");
  add(GLSLstd450Floor, "floor");
  add(GLSLstd450Ceil, "ceil");
  add(GLSLstd450Fract, "fract");

  add(GLSLstd450Radians, "radians");
  add(GLSLstd450Degrees, "degrees");
  add(GLSLstd450Sin, "sin");
  add(GLSLstd450Cos, "cos");
  add(GLSLstd450Tan, "tan");
  add(GLSLstd450Asin, "asin");
  add(GLSLstd450Acos, "acos");
  add(GLSLstd450Atan, "atan");
  add(GLSLstd450Sinh, "sinh");
  add(GLSLstd450Cosh, "cosh");
  add(GLSLstd450Tanh, "tanh");
  add(GLSLstd450Asinh, "asinh");
  add(GLSLstd450Acosh, "acosh");
  add(GLSLstd450Atanh, "atanh");
  add(GLSLstd450Atan2, "atan2");

  add(GLSLstd450Pow, "pow");
  add(GLSLstd450Exp, "exp");
  add(GLSLstd450Log, "log");
  add(GLSLstd450Exp2, "exp2");
  add(GLSLstd450Log2, "log2");
  add(GLSLstd450Sqrt, "sqrt");
  add(GLSLstd450InverseSqrt, "inverseSqrt");

  add(GLSLstd450Determinant, "determinant");
  add(GLSLstd450MatrixInverse, "matrixInverse");

  add(GLSLstd450Modf, "modf");
  add(GLSLstd450ModfStruct, "modfStruct");
  add(GLSLstd450FMin, "fmin");
  add(GLSLstd450UMin, "umin");
  add(GLSLstd450SMin, "smin");
  add(GLSLstd450FMax, "fmax");
  add(GLSLstd450UMax, "umax");
  add(GLSLstd450SMax, "smax");
  add(GLSLstd450FClamp, "fclamp");
  add(GLSLstd450UClamp, "uclamp");
  add(GLSLstd450SClamp, "sclamp");
  add(GLSLstd450FMix, "fmix");
  add(GLSLstd450IMix, "imix");
  add(GLSLstd450Step, "step");
  add(GLSLstd450SmoothStep, "smoothStep");

  add(GLSLstd450Fma, "fma");
  add(GLSLstd450Frexp, "frexp");
  add(GLSLstd450FrexpStruct, "frexpStruct");
  add(GLSLstd450Ldexp, "ldexp");

  add(GLSLstd450PackSnorm4x8, "packSnorm4x8");
  add(GLSLstd450PackUnorm4x8, "packUnorm4x8");
  add(GLSLstd450PackSnorm2x16, "packSnorm2x16");
  add(GLSLstd450PackUnorm2x16, "packUnorm2x16");
  add(GLSLstd450PackHalf2x16, "packHalf2x16");
  add(GLSLstd450PackDouble2x32, "packDouble2x32");
  add(GLSLstd450UnpackSnorm2x16, "unpackSnorm2x16");
  add(GLSLstd450UnpackUnorm2x16, "unpackUnorm2x16");
  add(GLSLstd450UnpackHalf2x16, "unpackHalf2x16");
  add(GLSLstd450UnpackSnorm4x8, "unpackSnorm4x8");
  add(GLSLstd450UnpackUnorm4x8, "unpackUnorm4x8");
  add(GLSLstd450UnpackDouble2x32, "unpackDouble2x32");

  add(GLSLstd450Length, "length");
  add(GLSLstd450Distance, "distance");
  add(GLSLstd450Cross, "cross");
  add(GLSLstd450Normalize, "normalize");
  add(GLSLstd450FaceForward, "faceForward");
  add(GLSLstd450Reflect, "reflect");
  add(GLSLstd450Refract, "refract");

  add(GLSLstd450FindILsb, "findILsb");
  add(GLSLstd450FindSMsb, "findSMsb");
  add(GLSLstd450FindUMsb, "findUMsb");

  add(GLSLstd450InterpolateAtCentroid, "interpolateAtCentroid");
  add(GLSLstd450InterpolateAtSample, "interpolateAtSample");
  add(GLSLstd450InterpolateAtOffset, "interpolateAtOffset");

  add(GLSLstd450NMin, "nmin");
  add(GLSLstd450NMax, "nmax");
  add(GLSLstd450NClamp, "nclamp");
}
SPIRV_DEF_NAMEMAP(GLSLExtOpKind, GLSLExtOpMap)

typedef ShaderBallotAMD ShaderBallotAMDExtOpKind;

template <> inline void SPIRVMap<ShaderBallotAMDExtOpKind, std::string>::init() {
  add(SwizzleInvocationsAMD, "SwizzleInvocationsAMD");
  add(SwizzleInvocationsMaskedAMD, "SwizzleInvocationsMaskedAMD");
  add(WriteInvocationAMD, "WriteInvocationAMD");
  add(MbcntAMD, "MbcntAMD");
}
SPIRV_DEF_NAMEMAP(ShaderBallotAMDExtOpKind, ShaderBallotAMDExtOpMap)

typedef ShaderExplicitVertexParameterAMD ShaderExplicitVertexParameterAMDExtOpKind;

template <> inline void SPIRVMap<ShaderExplicitVertexParameterAMDExtOpKind, std::string>::init() {
  add(InterpolateAtVertexAMD, "InterpolateAtVertexAMD");
}
SPIRV_DEF_NAMEMAP(ShaderExplicitVertexParameterAMDExtOpKind, ShaderExplicitVertexParameterAMDExtOpMap)

typedef GcnShaderAMD GcnShaderAMDExtOpKind;

template <> inline void SPIRVMap<GcnShaderAMDExtOpKind, std::string>::init() {
  add(CubeFaceIndexAMD, "CubeFaceIndexAMD");
  add(CubeFaceCoordAMD, "CubeFaceCoordAMD");
  add(TimeAMD, "TimeAMD");
}

SPIRV_DEF_NAMEMAP(GcnShaderAMDExtOpKind, GcnShaderAMDExtOpMap)

typedef ShaderTrinaryMinMaxAMD ShaderTrinaryMinMaxAMDExtOpKind;

template <> inline void SPIRVMap<ShaderTrinaryMinMaxAMDExtOpKind, std::string>::init() {
  add(FMin3AMD, "FMin3AMD");
  add(UMin3AMD, "UMin3AMD");
  add(SMin3AMD, "SMin3AMD");
  add(FMax3AMD, "FMax3AMD");
  add(UMax3AMD, "UMax3AMD");
  add(SMax3AMD, "SMax3AMD");
  add(FMid3AMD, "FMid3AMD");
  add(UMid3AMD, "UMid3AMD");
  add(SMid3AMD, "SMid3AMD");
}

SPIRV_DEF_NAMEMAP(ShaderTrinaryMinMaxAMDExtOpKind, ShaderTrinaryMinMaxAMDExtOpMap)

typedef uint32_t NonSemanticInfoExtOpKind;

typedef SPIRVDebug::Instruction SPIRVDebugExtOpKind;
template <> inline void SPIRVMap<SPIRVDebugExtOpKind, std::string>::init() {
  add(SPIRVDebug::DebugInfoNone, "DebugInfoNone");
  add(SPIRVDebug::CompilationUnit, "DebugCompileUnit");
  add(SPIRVDebug::Source, "DebugSource");
  add(SPIRVDebug::TypeBasic, "DebugTypeBasic");
  add(SPIRVDebug::TypePointer, "DebugTypePointer");
  add(SPIRVDebug::TypeArray, "DebugTypeArray");
  add(SPIRVDebug::TypeVector, "DebugTypeVector");
  add(SPIRVDebug::TypeQualifier, "DebugTypeQualifier");
  add(SPIRVDebug::TypeFunction, "DebugTypeFunction");
  add(SPIRVDebug::TypeComposite, "DebugTypeComposite");
  add(SPIRVDebug::TypeMember, "DebugTypeMember");
  add(SPIRVDebug::TypeEnum, "DebugTypeEnum");
  add(SPIRVDebug::Typedef, "DebugTypedef");
  add(SPIRVDebug::TypeTemplateParameter, "DebugTemplateParameter");
  add(SPIRVDebug::TypeTemplateParameterPack, "DebugTemplateParameterPack");
  add(SPIRVDebug::TypeTemplateTemplateParameter, "DebugTemplateTemplateParameter");
  add(SPIRVDebug::TypeTemplate, "DebugTemplate");
  add(SPIRVDebug::TypePtrToMember, "DebugTypePtrToMember,");
  add(SPIRVDebug::Inheritance, "DebugInheritance");
  add(SPIRVDebug::Function, "DebugFunction");
  add(SPIRVDebug::FunctionDecl, "DebugFunctionDecl");
  add(SPIRVDebug::LexicalBlock, "DebugLexicalBlock");
  add(SPIRVDebug::LexicalBlockDiscriminator, "LexicalBlockDiscriminator");
  add(SPIRVDebug::LocalVariable, "DebugLocalVariable");
  add(SPIRVDebug::InlinedVariable, "DebugInlinedVariable");
  add(SPIRVDebug::GlobalVariable, "DebugGlobalVariable");
  add(SPIRVDebug::Declare, "DebugDeclare");
  add(SPIRVDebug::Value, "DebugValue");
  add(SPIRVDebug::Scope, "DebugScope");
  add(SPIRVDebug::NoScope, "DebugNoScope");
  add(SPIRVDebug::InlinedAt, "DebugInlinedAt");
  add(SPIRVDebug::ImportedEntity, "DebugImportedEntity");
  add(SPIRVDebug::Expression, "DebugExpression");
  add(SPIRVDebug::Operation, "DebugOperation");
}
SPIRV_DEF_NAMEMAP(SPIRVDebugExtOpKind, SPIRVDebugExtOpMap)
} // namespace SPIRV

#endif
