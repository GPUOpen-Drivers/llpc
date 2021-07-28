/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Builder.h
 * @brief LLPC header file: declaration of lgc::Builder interface
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/BuilderBase.h"
#include "lgc/BuiltIns.h"
#include "lgc/CommonDefs.h"
#include "llvm/Support/AtomicOrdering.h"

namespace lgc {

class LgcContext;
struct CommonShaderMode;
struct ComputeShaderMode;
struct FragmentShaderMode;
struct GeometryShaderMode;
class Pipeline;
class ShaderModes;
struct TessellationMode;

// =====================================================================================================================
// Class that represents extra information on an input or output.
// For an FS input, if HasInterpAux(), then CreateReadInput's pVertexIndex is actually an auxiliary value
// for interpolation:
//  - InterpLocCenter: auxiliary value is v2f32 offset from center of pixel
//  - InterpLocSample: auxiliary value is i32 sample ID
//  - InterpLocExplicit: auxiliary value is i32 vertex number
class InOutInfo {
public:
  // Interpolation mode
  enum {
    InterpModeSmooth = 0,  // Smooth (perspective)
    InterpModeFlat = 1,    // Flat
    InterpModeNoPersp = 2, // Linear (no perspective)
    InterpModeCustom = 3,  // Custom
  };
  enum {
    InterpLocUnknown = 0,  // Unknown
    InterpLocCenter = 1,   // Center
    InterpLocCentroid = 2, // Centroid
    InterpLocSample = 3,   // Sample
    InterpLocExplicit = 4, // Mode must be InterpModeCustom
  };

  InOutInfo() { m_data.u32All = 0; }
  InOutInfo(unsigned data) { this->m_data.u32All = data; }
  InOutInfo(const InOutInfo &inOutInfo) { m_data.u32All = inOutInfo.getData(); }

  unsigned getData() const { return m_data.u32All; }

  unsigned getInterpMode() const { return m_data.bits.interpMode; }
  void setInterpMode(unsigned mode) { m_data.bits.interpMode = mode; }

  unsigned getInterpLoc() const { return m_data.bits.interpLoc; }
  void setInterpLoc(unsigned loc) { m_data.bits.interpLoc = loc; }

  bool hasInterpAux() const { return m_data.bits.hasInterpAux; }
  void setHasInterpAux(bool hasInterpAux = true) { m_data.bits.hasInterpAux = hasInterpAux; }

  bool hasStreamId() const { return m_data.bits.hasStreamId; }
  unsigned getStreamId() const { return m_data.bits.streamId; }
  void setStreamId(unsigned streamId) {
    m_data.bits.hasStreamId = true;
    m_data.bits.streamId = streamId;
  }

  bool isSigned() const { return m_data.bits.isSigned; }
  void setIsSigned(bool isSigned = true) { m_data.bits.isSigned = isSigned; }

  unsigned getArraySize() const { return m_data.bits.arraySize; }
  void setArraySize(unsigned arraySize) { m_data.bits.arraySize = arraySize; }

private:
  union {
    struct {
      unsigned interpMode : 4;   // FS input: interpolation mode
      unsigned interpLoc : 3;    // FS input: interpolation location
      unsigned hasInterpAux : 1; // FS input: there is an interpolation auxiliary value
      unsigned streamId : 2;     // GS output: vertex stream ID (0 if none)
      unsigned hasStreamId : 1;  // GS output: true if it has a stream ID
      unsigned isSigned : 1;     // FS output: is signed integer. Determines whether i16-component output
                                 //    is zero- or sign-extended
      unsigned arraySize : 4;    // Built-in array input: shader-defined array size. Must be set for
                                 //    a read or write of ClipDistance or CullDistance that is of the
                                 //    whole array or of an element with a variable index.
    } bits;
    unsigned u32All;
  } m_data;
};

// =====================================================================================================================
// Builder is the part of the LLPC middle-end interface used by the front-end to build IR. It is a subclass
// of IRBuilder<>, so the front-end can use its methods to create IR instructions at the set insertion
// point. In addition it has its own Create* methods to create graphics-specific IR constructs.
//
class Builder : public BuilderBase {
public:
  // The group arithmetic operations the builder can consume.
  // NOTE: We rely on casting this implicitly to an integer, so we cannot use an enum class.
  enum GroupArithOp { IAdd = 0, FAdd, IMul, FMul, SMin, UMin, FMin, SMax, UMax, FMax, And, Or, Xor };

  virtual ~Builder() {}

  // Static methods to create a BuilderImpl or BuilderRecorder. These are not used as part of the LGC
  // interface; use LgcContext::createBuilder instead.
  static Builder *createBuilderImpl(LgcContext *context, Pipeline *pipeline);
  static Builder *createBuilderRecorder(LgcContext *context, Pipeline *pipeline, bool omitOpcodes);

  // Get the type pElementTy, turned into a vector of the same vector width as pMaybeVecTy if the latter
  // is a vector type.
  static llvm::Type *getConditionallyVectorizedTy(llvm::Type *elementTy, llvm::Type *maybeVecTy);

  // Get the LgcContext
  LgcContext *getLgcContext() const { return m_builderContext; }

  // Set the current shader stage, clamp shader stage to the ShaderStageCompute
  void setShaderStage(ShaderStage stage) { m_shaderStage = stage > ShaderStageCompute ? ShaderStageCompute : stage; }

  // -----------------------------------------------------------------------------------------------------------------
  // Methods to set shader modes (FP modes, tessellation modes, fragment modes, workgroup size) for the current
  // shader that come from the input language. The structs passed to the methods are declared in Pipeline.h.
  // For a particular shader stage, these methods must be called before any Builder::Create* calls that
  // generate IR.

  // Set the common shader mode for the current shader, containing hardware FP round and denorm modes.
  // The client should always zero-initialize the struct before setting it up, in case future versions
  // add more fields. A local struct variable can be zero-initialized with " = {}".
  void setCommonShaderMode(const CommonShaderMode &commonShaderMode);

  // Get the common shader mode for the current shader.
  const CommonShaderMode &getCommonShaderMode();

  // Set the tessellation mode. This can be called in multiple shaders, and the values are merged
  // together -- a zero value in one call is overridden by a non-zero value in another call. LLPC needs
  // that because SPIR-V allows some of these execution mode items to appear in either the TCS or TES.
  // The client should always zero-initialize the struct before setting it up, in case future versions
  // add more fields. A local struct variable can be zero-initialized with " = {}".
  void setTessellationMode(const TessellationMode &tessellationMode);

  // Set the geometry shader state.
  // The client should always zero-initialize the struct before setting it up, in case future versions
  // add more fields. A local struct variable can be zero-initialized with " = {}".
  void setGeometryShaderMode(const GeometryShaderMode &geometryShaderMode);

  // Set the fragment shader mode.
  // The client should always zero-initialize the struct before setting it up, in case future versions
  // add more fields. A local struct variable can be zero-initialized with " = {}".
  void setFragmentShaderMode(const FragmentShaderMode &fragmentShaderMode);

  // Set the compute shader modes.
  // The client should always zero-initialize the struct before setting it up, in case future versions
  // add more fields. A local struct variable can be zero-initialized with " = {}".
  void setComputeShaderMode(const ComputeShaderMode &computeShaderMode);

  // Get the compute shader mode (workgroup size)
  const ComputeShaderMode &getComputeShaderMode();

  // Record shader modes into IR metadata if this is a shader compile (no PipelineState).
  virtual void recordShaderModes(llvm::Module *module) {}

  // -----------------------------------------------------------------------------------------------------------------
  // Base class operations

  // Create scalar from dot product of scalar or vector FP type. (The dot product of two scalars is their product.)
  // The two vectors must be the same floating point scalar/vector type.
  // Returns a value whose type is the element type of the vectors.
  //
  // @param vector1 : The float vector 1
  // @param vector2 : The float vector 2
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateDotProduct(llvm::Value *const vector1, llvm::Value *const vector2,
                                        const llvm::Twine &instName = "") = 0;

  // Bit settings in flags argument for integer dot product
  enum { FirstVectorSigned = 1, SecondVectorSigned = 2 };

  // Create code to calculate the dot product of two integer vectors, with optional accumulator, using hardware support
  // where available.
  // Use a value of 0 for no accumulation and the value type is consistent with the result type. The result is saturated
  // if there is an accumulator. The component type of input vectors can have 8-bit/16-bit/32-bit and i32/i16/i8 result.
  //
  // @param vector1 : The integer vector 1
  // @param vector2 : The integer vector 2
  // @param accumulator : The accumulator to the scalar of dot product
  // @param flags : Bit 0 is "first vector is signed" and bit 1 is "second vector is signed"
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateIntegerDotProduct(llvm::Value *vector1, llvm::Value *vector2, llvm::Value *accumulator,
                                               unsigned flags, const llvm::Twine &instName = "") = 0;

  // Create a call to the specified intrinsic with one operand, mangled on its type.
  // This is an override of the same method in IRBuilder<>; the difference is that this one sets fast math
  // flags from the Builder if none are specified by pFmfSource.
  //
  // @param id : Intrinsic ID
  // @param value : Input value
  // @param fmfSource : Instruction to copy fast math flags from; nullptr to get from Builder
  // @param instName : Name to give instruction
  llvm::CallInst *CreateUnaryIntrinsic(llvm::Intrinsic::ID id, llvm::Value *value,
                                       llvm::Instruction *fmfSource = nullptr, const llvm::Twine &instName = "");

  // Create a call to the specified intrinsic with two operands of the same type, mangled on that type.
  // This is an override of the same method in IRBuilder<>; the difference is that this one sets fast math
  // flags from the Builder if none are specified by pFmfSource.
  //
  // @param id : Intrinsic ID
  // @param value1 : Input value 1
  // @param value2 : Input value 2
  // @param fmfSource : Instruction to copy fast math flags from; nullptr to get from Builder
  // @param name : Name to give instruction
  llvm::CallInst *CreateBinaryIntrinsic(llvm::Intrinsic::ID id, llvm::Value *value1, llvm::Value *value2,
                                        llvm::Instruction *fmfSource = nullptr, const llvm::Twine &name = "");

  //
  // @param id : Intrinsic ID
  // @param types : Types
  // @param args : Input values
  // @param fmfSource : Instruction to copy fast math flags from; nullptr to get from Builder
  // @param name : Name to give instruction
  llvm::CallInst *CreateIntrinsic(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type *> types,
                                  llvm::ArrayRef<llvm::Value *> args, llvm::Instruction *fmfSource = nullptr,
                                  const llvm::Twine &name = "");

  // -----------------------------------------------------------------------------------------------------------------
  // Arithmetic operations

  // Methods to get useful FP constants. Using these (rather than just using for example
  // ConstantFP::get(.., 180 / M_PI)) ensures that we always get the same value, independent of the
  // host platform and its compiler.

  // Get a constant of FP or vector of FP type for the value PI/180, for converting radians to degrees.
  llvm::Constant *getPiOver180(llvm::Type *ty);

  // Get a constant of FP or vector of FP type for the value 180/PI, for converting degrees to radians.
  llvm::Constant *get180OverPi(llvm::Type *ty);

  // Get a constant of FP or vector of FP type for the value 1/(2^n - 1)
  llvm::Constant *getOneOverPower2MinusOne(llvm::Type *ty, unsigned n);

  // Create calculation of 2D texture coordinates that would be used for accessing the selected cube map face for
  // the given cube map texture coordinates. Returns <2 x float>.
  //
  // @param coord : Input coordinate <3 x float>
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateCubeFaceCoord(llvm::Value *coord, const llvm::Twine &instName = "") = 0;

  // Create calculation of the index of the cube map face that would be accessed by a texture lookup function for
  // the given cube map texture coordinates. Returns a single float with value:
  //  0.0 = the cube map face facing the positive X direction
  //  1.0 = the cube map face facing the negative X direction
  //  2.0 = the cube map face facing the positive Y direction
  //  3.0 = the cube map face facing the negative Y direction
  //  4.0 = the cube map face facing the positive Z direction
  //  5.0 = the cube map face facing the negative Z direction
  //
  // @param coord : Input coordinate <3 x float>
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateCubeFaceIndex(llvm::Value *coord, const llvm::Twine &instName = "") = 0;

  // Create scalar or vector FP truncate operation with the given rounding mode.
  // Currently the rounding mode is only implemented for float/double -> half conversion.
  //
  // @param value : Input value
  // @param destTy : Type to convert to
  // @param roundingMode : Rounding mode
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateFpTruncWithRounding(llvm::Value *value, llvm::Type *destTy, unsigned roundingMode,
                                                 const llvm::Twine &instName = "") = 0;

  // Create quantize operation: truncates float (or vector) value to a value that is representable by a half.
  //
  // @param value : Input value (float or float vector)
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateQuantizeToFp16(llvm::Value *value, const llvm::Twine &instName = "") = 0;

  // Create signed integer modulo operation, where the sign of the result (if not zero) is the same as
  // the sign of the divisor. The result is undefined if pDivisor is zero.
  //
  // @param dividend : Dividend value
  // @param divisor : Divisor value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSMod(llvm::Value *dividend, llvm::Value *divisor, const llvm::Twine &instName = "") = 0;

  // Create FP modulo operation, where the sign of the result (if not zero) is the same as
  // the sign of the divisor. The result is undefined if pDivisor is zero.
  //
  // @param dividend : Dividend value
  // @param divisor : Divisor value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateFMod(llvm::Value *dividend, llvm::Value *divisor, const llvm::Twine &instName = "") = 0;

  // Create scalar/vector float/half fused multiply-and-add, to compute a * b + c
  //
  // @param a : One value to multiply
  // @param b : The other value to multiply
  // @param c : The value to add to the product of A and B
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateFma(llvm::Value *a, llvm::Value *b, llvm::Value *c, const llvm::Twine &instName = "") = 0;

  // Create a "tan" operation for a scalar or vector float or half.
  //
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateTan(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create an "asin" operation for a scalar or vector float or half.
  //
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateASin(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create an "acos" operation for a scalar or vector float or half.
  //
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateACos(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create an "atan" operation for a scalar or vector float or half.
  //
  // @param yOverX : Input value Y/X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateATan(llvm::Value *yOverX, const llvm::Twine &instName = "") = 0;

  // Create an "atan2" operation for a scalar or vector float or half.
  // Returns atan(Y/X) but in the correct quadrant for the input value signs.
  //
  // @param y : Input value Y
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateATan2(llvm::Value *y, llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create a "sinh" operation for a scalar or vector float or half.
  //
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSinh(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create a "cosh" operation for a scalar or vector float or half.
  //
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateCosh(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create a "tanh" operation for a scalar or vector float or half.
  //
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateTanh(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create an "asinh" operation for a scalar or vector float or half.
  //
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateASinh(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create an "acosh" operation for a scalar or vector float or half.
  //
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateACosh(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create an "atanh" operation for a scalar or vector float or half.
  //
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateATanh(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create a "power" operation for a scalar or vector float or half, calculating X ^ Y
  //
  // @param x : Input value X
  // @param y : Input value Y
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreatePower(llvm::Value *x, llvm::Value *y, const llvm::Twine &instName = "") = 0;

  // Create an "exp" operation for a scalar or vector float or half.
  //
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateExp(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create a "log" operation for a scalar or vector float or half.
  //
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateLog(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create a square root operation for a scalar or vector FP type
  //
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSqrt(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create "signed integer abs" operation for a scalar or vector integer value.
  //
  // @param x : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSAbs(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create "fsign" operation for a scalar or vector floating-point type, returning -1.0, 0.0 or +1.0 if the input
  // value is negative, zero or positive.
  //
  // @param inValue : Input value X
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateFSign(llvm::Value *inValue, const llvm::Twine &instName = "") = 0;

  // Create "ssign" operation for a scalar or vector integer type, returning -1, 0 or +1 if the input
  // value is negative, zero or positive.
  //
  // @param x : Input value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSSign(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create "fract" operation for a scalar or vector floating-point type, returning x - floor(x).
  //
  // @param x : Input value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateFract(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create "smoothStep" operation. Result is 0.0 if x <= edge0 and 1.0 if x >= edge1 and performs smooth Hermite
  // interpolation between 0 and 1 when edge0 < x < edge1. This is equivalent to:
  // t * t * (3 - 2 * t), where t = clamp ((x - edge0) / (edge1 - edge0), 0, 1)
  // Result is undefined if edge0 >= edge1.
  //
  // @param edge0 : Edge0 value
  // @param edge1 : Edge1 value
  // @param x : X (input) value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSmoothStep(llvm::Value *edge0, llvm::Value *edge1, llvm::Value *x,
                                        const llvm::Twine &instName = "") = 0;

  // Create "ldexp" operation: given an FP mantissa and int exponent, build an FP value
  //
  // @param x : Mantissa
  // @param exp : Exponent
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateLdexp(llvm::Value *x, llvm::Value *exp, const llvm::Twine &instName = "") = 0;

  // Create "extract significand" operation: given an FP scalar or vector value, return the significand in the range
  // [0.5,1.0), of the same type as the input. If the input is 0, the result is 0. If the input is infinite or NaN,
  // the result is undefined.
  //
  // @param value : Input value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateExtractSignificand(llvm::Value *value, const llvm::Twine &instName = "") = 0;

  // Create "extract exponent" operation: given an FP scalar or vector value, return the exponent as a signed integer.
  // If the input is (vector of) half, the result type is (vector of) i16, otherwise it is (vector of) i32.
  // If the input is 0, the result is 0. If the input is infinite or NaN, the result is undefined.
  //
  // @param value : Input value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateExtractExponent(llvm::Value *value, const llvm::Twine &instName = "") = 0;

  // Create vector cross product operation. Inputs must be <3 x FP>
  //
  // @param x : Input value X
  // @param y : Input value Y
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateCrossProduct(llvm::Value *x, llvm::Value *y, const llvm::Twine &instName = "") = 0;

  // Create FP scalar/vector normalize operation: returns a scalar/vector with the same direction and magnitude 1.
  //
  // @param x : Input value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateNormalizeVector(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create "face forward" operation: given three FP scalars/vectors {N, I, Nref}, if the dot product of
  // Nref and I is negative, the result is N, otherwise it is -N
  //
  // @param n : Input value "N"
  // @param i : Input value "I"
  // @param nref : Input value "Nref"
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateFaceForward(llvm::Value *n, llvm::Value *i, llvm::Value *nref,
                                         const llvm::Twine &instName = "") = 0;

  // Create "reflect" operation. For the incident vector I and normalized surface orientation N, the result is
  // the reflection direction:
  // I - 2 * dot(N, I) * N
  //
  // @param i : Input value "I"
  // @param n : Input value "N"
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateReflect(llvm::Value *i, llvm::Value *n, const llvm::Twine &instName = "") = 0;

  // Create "refract" operation. For the normalized incident vector I, normalized surface orientation N and ratio
  // of indices of refraction eta, the result is the refraction vector:
  // k = 1.0 - eta * eta * (1.0 - dot(N,I) * dot(N,I))
  // If k < 0.0 the result is 0.0.
  // Otherwise, the result is eta * I - (eta * dot(N,I) + sqrt(k)) * N
  //
  // @param i : Input value "I"
  // @param n : Input value "N"
  // @param eta : Input value "eta"
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateRefract(llvm::Value *i, llvm::Value *n, llvm::Value *eta,
                                     const llvm::Twine &instName = "") = 0;

  // Create "fclamp" operation, returning min(max(x, minVal), maxVal). Result is undefined if minVal > maxVal.
  // This honors the fast math flags; clear "nnan" in fast math flags in order to obtain the "NaN avoiding
  // semantics" for the min and max where, if one input is NaN, it returns the other one.
  // It also honors the shader's FP mode being "flush denorm".
  //
  // @param x : Value to clamp
  // @param minVal : Minimum of clamp range
  // @param maxVal : Maximum of clamp range
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateFClamp(llvm::Value *x, llvm::Value *minVal, llvm::Value *maxVal,
                                    const llvm::Twine &instName = "") = 0;

  // Create "fmin" operation, returning the minimum of two scalar or vector FP values.
  // This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
  // It also honors the shader's FP mode being "flush denorm".
  //
  // @param value1 : First value
  // @param value2 : Second value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateFMin(llvm::Value *value1, llvm::Value *value2, const llvm::Twine &instName = "") = 0;

  // Create "fmax" operation, returning the maximum of two scalar or vector float or half values.
  // This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
  // It also honors the shader's FP mode being "flush denorm".
  //
  // @param value1 : First value
  // @param value2 : Second value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateFMax(llvm::Value *value1, llvm::Value *value2, const llvm::Twine &instName = "") = 0;

  // Create "fmin3" operation, returning the minimum of three scalar or vector float or half values.
  // This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
  // It also honors the shader's FP mode being "flush denorm".
  //
  // @param value1 : First value
  // @param value2 : Second value
  // @param value3 : Third value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateFMin3(llvm::Value *value1, llvm::Value *value2, llvm::Value *value3,
                                   const llvm::Twine &instName = "") = 0;

  // Create "fmax3" operation, returning the maximum of three scalar or vector float or half values.
  // This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
  // It also honors the shader's FP mode being "flush denorm".
  //
  // @param value1 : First value
  // @param value2 : Second value
  // @param value3 : Third value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateFMax3(llvm::Value *value1, llvm::Value *value2, llvm::Value *value3,
                                   const llvm::Twine &instName = "") = 0;

  // Create "fmid3" operation, returning the middle one of three scalar or vector float or half values.
  // This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
  // It also honors the shader's FP mode being "flush denorm".
  //
  // @param value1 : First value
  // @param value2 : Second value
  // @param value3 : Third value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateFMid3(llvm::Value *value1, llvm::Value *value2, llvm::Value *value3,
                                   const llvm::Twine &instName = "") = 0;

  // Create "isInf" operation: return true if the supplied FP (or vector) value is infinity
  //
  // @param x : Input value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateIsInf(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create "isNaN" operation: return true if the supplied FP (or vector) value is NaN
  //
  // @param x : Input value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateIsNaN(llvm::Value *x, const llvm::Twine &instName = "") = 0;

  // Create an "insert bitfield" operation for a (vector of) integer type.
  // Returns a value where the "pCount" bits starting at bit "pOffset" come from the least significant "pCount"
  // bits in "pInsert", and remaining bits come from "pBase". The result is undefined if "pCount"+"pOffset" is
  // more than the number of bits (per vector element) in "pBase" and "pInsert".
  // If "pBase" and "pInsert" are vectors, "pOffset" and "pCount" can be either scalar or vector of the same
  // width. The scalar type of "pOffset" and "pCount" must be integer, but can be different to that of "pBase"
  // and "pInsert" (and different to each other too).
  //
  // @param base : Base value
  // @param insert : Value to insert (same type as base)
  // @param offset : Bit number of least-significant end of bitfield
  // @param count : Count of bits in bitfield
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateInsertBitField(llvm::Value *base, llvm::Value *insert, llvm::Value *offset,
                                            llvm::Value *count, const llvm::Twine &instName = "") = 0;

  // Create an "extract bitfield " operation for a (vector of) i32.
  // Returns a value where the least significant "pCount" bits come from the "pCount" bits starting at bit
  // "pOffset" in "pBase", and that is zero- or sign-extended (depending on "isSigned") to the rest of the value.
  // If "pBase" and "pInsert" are vectors, "pOffset" and "pCount" can be either scalar or vector of the same
  // width. The scalar type of "pOffset" and "pCount" must be integer, but can be different to that of "pBase"
  // (and different to each other too).
  //
  // @param base : Base value
  // @param offset : Bit number of least-significant end of bitfield
  // @param count : Count of bits in bitfield
  // @param isSigned : True for a signed int bitfield extract, false for unsigned
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateExtractBitField(llvm::Value *base, llvm::Value *offset, llvm::Value *count, bool isSigned,
                                             const llvm::Twine &instName = "") = 0;

  // Create "find MSB" operation for a (vector of) signed i32. For a postive number, the result is the bit number of
  // the most significant 1-bit. For a negative number, the result is the bit number of the most significant 0-bit.
  // For a value of 0 or -1, the result is -1.
  //
  // Note that unsigned "find MSB" is not provided as a Builder method, because it is easily synthesized from
  // the standard LLVM intrinsic llvm.ctlz. Similarly "find LSB" is not provided because it is easily synthesized
  // from the standard LLVM intrinsic llvm.cttz.
  //
  // @param value : Input value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateFindSMsb(llvm::Value *value, const llvm::Twine &instName = "") = 0;

  // Create "fmix" operation, returning ( 1 - A ) * X + A * Y. Result would be FP scalar or vector value.
  // Returns scalar, if and only if "pX", "pY" and "pA" are all scalars.
  // Returns vector, if "pX" and "pY" are vector but "pA" is a scalar, under such condition, "pA" will be splatted.
  // Returns vector, if "pX", "pY" and "pA" are all vectors.
  //
  // Note that when doing vector calculation, it means add/sub are element-wise between vectors, and the product will
  // be Hadamard product.
  //
  // @param x : Left Value
  // @param y : Right Value
  // @param a : Wight Value
  virtual llvm::Value *createFMix(llvm::Value *x, llvm::Value *y, llvm::Value *a, const llvm::Twine &instName = "") = 0;

  // -----------------------------------------------------------------------------------------------------------------
  // Descriptor operations
  //
  // The API here has two classes of descriptor, with different ways of handling the two classes:
  //
  // 1. A buffer descriptor is loaded in one step given its descriptor set, binding and index.
  //    It is done this way because the implementation needs to be able to handle normal buffer
  //    descriptors, compact buffer descriptors and inline buffers, without the input language (SPIR-V)
  //    telling us which one it is.
  //
  // 2. An image/sampler/texelbuffer/F-mask descriptor has a three-step API:
  //    a. Get a pointer to the descriptor or array of descriptors given the descriptor set and binding.
  //    b. Zero or more calls to add on an array index.
  //    c. Load the descriptor from its pointer.
  //    SPIR-V allows a pointer to an image/sampler to be passed as a function arg (and maybe in other
  //    ways). This API is formulated to allow the front-end to implement that. Step (c) can be
  //    performed without needing to see the resource node used in (a).

  // Bit settings for flags argument in CreateLoadBufferDesc.
  enum {
    BufferFlagNonUniform = 1, // Descriptor index is non-uniform
    BufferFlagWritten = 2,    // Buffer is (or might be) written to
    BufferFlagReserved4 = 4,  // Reserved for future functionality
    BufferFlagReserved8 = 8,  // Reserved for future functionality
  };

  // Get the type of pointer returned by CreateLoadBufferDesc.
  llvm::PointerType *getBufferDescTy(llvm::Type *pointeeTy);

  // Create a load of a buffer descriptor.
  //
  // @param descSet : Descriptor set
  // @param binding : Descriptor binding
  // @param descIndex : Descriptor index
  // @param flags : BufferFlag* bit settings
  // @param pointeeTy : Type that the returned pointer should point to.
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateLoadBufferDesc(unsigned descSet, unsigned binding, llvm::Value *descIndex, unsigned flags,
                                            llvm::Type *pointeeTy, const llvm::Twine &instName = "") = 0;

  // Get the type of a descriptor
  //
  // @param descType : Descriptor type, one of the ResourceNodeType values
  llvm::VectorType *getDescTy(ResourceNodeType descType);

  // Get the type of pointer to descriptor.
  //
  // @param descType : Descriptor type, one of the ResourceNodeType values
  llvm::Type *getDescPtrTy(ResourceNodeType descType);

  // Create a get of the stride (in bytes) of a descriptor. Returns an i32 value.
  //
  // @param descType : Descriptor type, one of ResourceNodeType::DescriptorSampler, DescriptorResource,
  //                   DescriptorTexelBuffer, DescriptorFmask.
  // @param descSet : Descriptor set
  // @param binding : Descriptor binding
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateGetDescStride(ResourceNodeType descType, unsigned descSet, unsigned binding,
                                           const llvm::Twine &instName = "") = 0;

  // Create a pointer to a descriptor. Returns a value of the type returned by GetSamplerDescPtrTy, GetImageDescPtrTy,
  // GetTexelBufferDescPtrTy or GetFmaskDescPtrTy, depending on descType.
  //
  // @param descType : Descriptor type, one of ResourceNodeType::DescriptorSampler, DescriptorResource,
  //                   DescriptorTexelBuffer, DescriptorFmask.
  // @param descSet : Descriptor set
  // @param binding : Descriptor binding
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateGetDescPtr(ResourceNodeType descType, unsigned descSet, unsigned binding,
                                        const llvm::Twine &instName = "") = 0;

  // Create a load of the push constants pointer.
  // This returns a pointer to the ResourceNodeType::PushConst resource in the top-level user data table.
  //
  // @param pushConstantsTy : Type that the returned pointer will point to
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateLoadPushConstantsPtr(llvm::Type *pushConstantsTy, const llvm::Twine &instName = "") = 0;

  // Create a buffer length query based on the specified descriptor, subtracting an offset from the length. The result
  // is 0 for a null descriptor when allowNullDescriptor is enabled.
  //
  // @param bufferDesc : The buffer descriptor to query.
  // @param offset: The offset to subtract from the buffer length.
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateGetBufferDescLength(llvm::Value *const bufferDesc, llvm::Value *offset,
                                                 const llvm::Twine &instName = "") = 0;

  // Return the i64 difference between two pointers, dividing out the size of the pointed-to objects.
  // For buffer fat pointers, delays the translation to patch phase.
  //
  // @param lhs : Left hand side of the subtraction.
  // @param rhs : Reft hand side of the subtraction.
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreatePtrDiff(llvm::Value *lhs, llvm::Value *rhs, const llvm::Twine &instName = "") = 0;

  // -----------------------------------------------------------------------------------------------------------------
  // Image operations

  // Possible values for dimension argument for image methods.
  enum {
    Dim1D = 0,          // Coordinate: x
    Dim2D = 1,          // Coordinate: x, y
    Dim3D = 2,          // Coordinate: x, y, z
    DimCube = 3,        // Coordinate: x, y, face
    Dim1DArray = 4,     // Coordinate: x, slice
    Dim2DArray = 5,     // Coordinate: x, y, slice
    Dim2DMsaa = 6,      // Coordinate: x, y, fragid
    Dim2DArrayMsaa = 7, // Coordinate: x, y, slice, fragid
    DimCubeArray = 8,   // Coordinate: x, y, face, slice (despite both SPIR-V and ISA
                        //    combining face and slice into one component)
  };

  // Get the number of coordinates for the specified dimension argument.
  //
  // @param dim : Image dimension
  static unsigned getImageNumCoords(unsigned dim) {
    switch (dim) {
    case Dim1D:
      return 1;
    case Dim2D:
      return 2;
    case Dim3D:
      return 3;
    case DimCube:
      return 3;
    case Dim1DArray:
      return 2;
    case Dim2DArray:
      return 3;
    case Dim2DMsaa:
      return 3;
    case Dim2DArrayMsaa:
      return 4;
    case DimCubeArray:
      return 4;
    }
    llvm_unreachable("Should never be called!");
    return 0;
  }

  // Get the number of components of a size query for the specified dimension argument.
  //
  // @param dim : Image dimension
  static unsigned getImageQuerySizeComponentCount(unsigned dim) {
    switch (dim) {
    case Dim1D:
      return 1;
    case Dim2D:
      return 2;
    case Dim3D:
      return 3;
    case DimCube:
      return 2;
    case Dim1DArray:
      return 2;
    case Dim2DArray:
      return 3;
    case Dim2DMsaa:
      return 2;
    case Dim2DArrayMsaa:
      return 3;
    case DimCubeArray:
      return 3;
    }
    llvm_unreachable("Should never be called!");
    return 0;
  }

  // Bit settings in flags argument for image methods.
  enum {
    ImageFlagCoherent = 1,                       // Coherent memory access
    ImageFlagVolatile = 2,                       // Volatile memory access
    ImageFlagSignedResult = 4,                   // For a gather with integer result, whether it is signed
    ImageFlagNonUniformImage = 8,                // Whether the image descriptor is non-uniform
    ImageFlagNonUniformSampler = 0x10,           // Whether the sampler descriptor is non-uniform
    ImageFlagAddFragCoord = 0x20,                // Add FragCoord (converted to signed int) on to coordinate x,y.
                                                 // Image load, store and atomic only.
    ImageFlagCheckMultiView = 0x40,              // If pipeline state enables multiview, use ViewIndex as coordinate z.
                                                 // Otherwise, acts the same as ImageFlagAddFragCoord
    ImageFlagEnforceReadFirstLaneImage = 0x80,   // Whether enabling readfirstlane on the image descriptor
    ImageFlagEnforceReadFirstLaneSampler = 0x100 // Whether enabling readfirstlane on the sampler desriptor
  };

  // Address array indices for image sample and gather methods. Where an optional entry is missing (either
  // nullptr, or the array is not long enough for it), then it assumes a default value.
  enum {
    ImageAddressIdxCoordinate = 0,  // Coordinate - a scalar or vector of float or half exactly as wide as
                                    //    returned by GetImageNumCoords(dim)
    ImageAddressIdxProjective = 1,  // Projective coordinate - divided into each coordinate (image sample only)
                                    //  (optional; default no projective divide)
    ImageAddressIdxComponent = 2,   // Component - constant i32 component for gather
    ImageAddressIdxDerivativeX = 3, // X derivative - vector of float or half with number of coordinates
                                    //  excluding array slice (optional; default is to use
                                    //  implicit derivatives).
    ImageAddressIdxDerivativeY = 4, // Y derivative - vector of float or half with number of coordinates
                                    //  excluding array slice (optional; default is to use
                                    //  implicit derivatives).
    ImageAddressIdxLod = 5,         // float level of detail (optional; default is to use
                                    //  implicit computed LOD)
    ImageAddressIdxLodBias = 6,     // float bias to add to the computed LOD (optional;
                                    //  default 0.0)
    ImageAddressIdxLodClamp = 7,    // float value to clamp LOD to (optional; default
                                    //  no clamping)
    ImageAddressIdxOffset = 8,      // Offset to add to coordinates - scalar or vector of i32, padded with 0s
                                    //  if not wide enough (optional; default all 0s). Alternatively, for
                                    //  independent offsets in a gather, a 4-array of the same, which is
                                    //  implemented as four separate gather instructions
    ImageAddressIdxZCompare = 9,    // float Z-compare value (optional; default no Z-compare)
    ImageAddressCount = 10          // All image address indices are less than this
  };

  // Atomic operation, for use in CreateImageAtomic.
  enum {
    ImageAtomicSwap = 0, // Atomic operation: swap
    ImageAtomicAdd = 2,  // Atomic operation: add
    ImageAtomicSub = 3,  // Atomic operation: subtract
    ImageAtomicSMin = 4, // Atomic operation: signed minimum
    ImageAtomicUMin = 5, // Atomic operation: unsigned minimum
    ImageAtomicSMax = 6, // Atomic operation: signed maximum
    ImageAtomicUMax = 7, // Atomic operation: unsigned maximum
    ImageAtomicAnd = 8,  // Atomic operation: and
    ImageAtomicOr = 9,   // Atomic operation: or
    ImageAtomicXor = 10  // Atomic operation: xor
  };

  // Create an image load.
  //
  // @param resultTy : Result type
  // @param dim : Image dimension
  // @param flags : ImageFlag* flags
  // @param imageDesc : Image descriptor or texel buffer descriptor.
  // @param coord : Coordinates: scalar or vector i32, exactly right width
  // @param mipLevel : Mipmap level if doing load_mip, otherwise nullptr
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateImageLoad(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                       llvm::Value *coord, llvm::Value *mipLevel, const llvm::Twine &instName = "") = 0;

  // Create an image load with fmask. Dim must be 2DMsaa or 2DArrayMsaa. If the F-mask descriptor has a valid
  // format field, then it reads "fmask_texel_R", the R component of the texel read from the given coordinates
  // in the F-mask image, and calculates the sample number to use as the sample'th nibble (where sample=0 means
  // the least significant nibble) of fmask_texel_R. If the F-mask descriptor has an invalid format, then it
  // just uses the supplied sample number. The calculated sample is then appended to the supplied coordinates
  // for a normal image load.
  //
  // @param resultTy : Result type
  // @param dim : Image dimension, 2DMsaa or 2DArrayMsaa
  // @param flags : ImageFlag* flags
  // @param imageDesc : Image descriptor
  // @param fmaskDesc : Fmask descriptor
  // @param coord : Coordinates: scalar or vector i32, exactly right width for given dimension excluding sample
  // @param sampleNum : Sample number, i32
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateImageLoadWithFmask(llvm::Type *resultTy, unsigned dim, unsigned flags,
                                                llvm::Value *imageDesc, llvm::Value *fmaskDesc, llvm::Value *coord,
                                                llvm::Value *sampleNum, const llvm::Twine &instName = "") = 0;

  // Create an image store.
  //
  // @param texel : Texel value to store; v4i16, v4i32, v4f16 or v4f32
  // @param dim : Image dimension
  // @param flags : ImageFlag* flags
  // @param imageDesc : Image descriptor or texel buffer descriptor
  // @param coord : Coordinates: scalar or vector i32, exactly right width
  // @param mipLevel : Mipmap level if doing store_mip, otherwise nullptr
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateImageStore(llvm::Value *texel, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                        llvm::Value *coord, llvm::Value *mipLevel,
                                        const llvm::Twine &instName = "") = 0;

  // Create an image sample.
  // The return type is specified by pResultTy as follows:
  // * If it is a struct, then the method generates a TFE (texel fail enable) operation. The first field is the
  //   texel type, and the second field is i32, where bit 0 is the TFE bit. Otherwise, the return type is the texel
  //   type.
  // * If the ZCompare address component is supplied, then the texel type is the scalar texel component
  //   type. Otherwise the texel type is a 4-vector of the texel component type.
  // * The texel component type is i32, f16 or f32.
  //
  // @param resultTy : Result type
  // @param dim : Image dimension
  // @param flags : ImageFlag* flags
  // @param imageDesc : Image descriptor
  // @param samplerDesc : Sampler descriptor
  // @param address : Address and other arguments
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateImageSample(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                         llvm::Value *samplerDesc, llvm::ArrayRef<llvm::Value *> address,
                                         const llvm::Twine &instName = "") = 0;

  // Create an image sample with a converting sampler.
  // The caller supplies all arguments to the image sample op in "address", in the order specified
  // by the indices defined as ImageIndex* below.
  //
  // @param resultTy : Result type
  // @param dim : Image dimension
  // @param flags : ImageFlag* flags
  // @param imageDescArray : Image descriptor, or array of up to three descriptors for multi-plane
  // @param convertingSamplerDesc : Converting sampler descriptor (constant v8i32)
  // @param address : Address and other arguments
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateImageSampleConvert(llvm::Type *resultTy, unsigned dim, unsigned flags,
                                                llvm::Value *imageDescArray, llvm::Value *convertingSamplerDesc,
                                                llvm::ArrayRef<llvm::Value *> address,
                                                const llvm::Twine &instName = "") = 0;

  // Create an image gather.
  // The return type is specified by pResultTy as follows:
  // * If it is a struct, then the method generates a TFE (texel fail enable) operation. The first field is the
  //   texel type, and the second field is i32, where bit 0 is the TFE bit. Otherwise, the return type is the texel
  //   type.
  // * The texel type is a 4-vector of the texel component type, which is i32, f16 or f32.
  //
  // @param resultTy : Result type
  // @param dim : Image dimension
  // @param flags : ImageFlag* flags
  // @param imageDesc : Image descriptor
  // @param samplerDesc : Sampler descriptor
  // @param address : Address and other arguments
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateImageGather(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                         llvm::Value *samplerDesc, llvm::ArrayRef<llvm::Value *> address,
                                         const llvm::Twine &instName = "") = 0;

  // Create an image atomic operation other than compare-and-swap. An add of +1 or -1, or a sub
  // of -1 or +1, is generated as inc or dec. Result type is the same as the input value type.
  // Normally pImageDesc is an image descriptor, as returned by CreateLoadImageDesc, and this method
  // creates an image atomic instruction. But pImageDesc can instead be a texel buffer descriptor, as
  // returned by CreateLoadTexelBufferDesc, in which case the method creates a buffer atomic instruction.
  //
  // @param atomicOp : Atomic op to create
  // @param dim : Image dimension
  // @param flags : ImageFlag* flags
  // @param ordering : Atomic ordering
  // @param imageDesc : Image descriptor or texel buffer descriptor
  // @param coord : Coordinates: scalar or vector i32, exactly right width
  // @param inputValue : Input value: i32
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateImageAtomic(unsigned atomicOp, unsigned dim, unsigned flags, llvm::AtomicOrdering ordering,
                                         llvm::Value *imageDesc, llvm::Value *coord, llvm::Value *inputValue,
                                         const llvm::Twine &instName = "") = 0;

  // Create an image atomic compare-and-swap.
  // Normally pImageDesc is an image descriptor, as returned by CreateLoadImageDesc, and this method
  // creates an image atomic instruction. But pImageDesc can instead be a texel buffer descriptor, as
  // returned by CreateLoadTexelBufferDesc, in which case the method creates a buffer atomic instruction.
  //
  // @param dim : Image dimension
  // @param flags : ImageFlag* flags
  // @param ordering : Atomic ordering
  // @param imageDesc : Image descriptor or texel buffer descriptor
  // @param coord : Coordinates: scalar or vector i32, exactly right width
  // @param inputValue : Input value: i32
  // @param comparatorValue : Value to compare against: i32
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateImageAtomicCompareSwap(unsigned dim, unsigned flags, llvm::AtomicOrdering ordering,
                                                    llvm::Value *imageDesc, llvm::Value *coord, llvm::Value *inputValue,
                                                    llvm::Value *comparatorValue, const llvm::Twine &instName = "") = 0;

  // Create a query of the number of mipmap levels in an image. Returns an i32 value.
  //
  // @param dim : Image dimension
  // @param flags : ImageFlag* flags
  // @param imageDesc : Image descriptor or texel buffer descriptor
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateImageQueryLevels(unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                              const llvm::Twine &instName = "") = 0;

  // Create a query of the number of samples in an image. Returns an i32 value.
  //
  // @param dim : Image dimension
  // @param flags : ImageFlag* flags
  // @param imageDesc : Image descriptor or texel buffer descriptor
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateImageQuerySamples(unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                               const llvm::Twine &instName = "") = 0;

  // Create a query of size of an image at the specified LOD.
  // Returns an i32 scalar or vector of the width given by GetImageQuerySizeComponentCount.
  //
  // @param dim : Image dimension
  // @param flags : ImageFlag* flags
  // @param imageDesc : Image descriptor or texel buffer descriptor
  // @param lod : LOD
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateImageQuerySize(unsigned dim, unsigned flags, llvm::Value *imageDesc, llvm::Value *lod,
                                            const llvm::Twine &instName = "") = 0;

  // Create a get of the LOD that would be used for an image sample with the given coordinates
  // and implicit LOD. Returns a v2f32 containing the layer number and the implicit level of
  // detail relative to the base level.
  //
  // @param dim : Image dimension
  // @param flags : ImageFlag* flags
  // @param imageDesc : Image descriptor
  // @param samplerDesc : Sampler descriptor
  // @param coord : Coordinates: scalar or vector f32, exactly right width without array layer
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateImageGetLod(unsigned dim, unsigned flags, llvm::Value *imageDesc, llvm::Value *samplerDesc,
                                         llvm::Value *coord, const llvm::Twine &instName = "") = 0;

  // -----------------------------------------------------------------------------------------------------------------
  // Shader input/output methods

  // Create a read of (part of) a generic (user) input value, passed from the previous shader stage.
  // The result type is as specified by pResultTy, a scalar or vector type with no more than four elements.
  // A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
  // 64-bit components. Two consecutive locations together can contain up to a 4-vector of 64-bit components.
  // A non-constant pLocationOffset is currently only supported for TCS and TES, and for an FS custom-interpolated
  // input.
  //
  // @param resultTy : Type of value to read
  // @param location : Base location (row) of input
  // @param locationOffset : Location offset; must be within locationCount if variable
  // @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
  // 64-bit elements.)
  // @param locationCount : Count of locations taken by the input. Ignored if pLocationOffset is const
  // @param inputInfo : Extra input info (FS interp info)
  // @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index; for FS custom interpolated input: auxiliary
  // interpolation value; else nullptr
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateReadGenericInput(llvm::Type *resultTy, unsigned location, llvm::Value *locationOffset,
                                              llvm::Value *elemIdx, unsigned locationCount, InOutInfo inputInfo,
                                              llvm::Value *vertexIndex, const llvm::Twine &instName = "") = 0;

  // Create a read of (part of) a generic (user) output value, returning the value last written in this shader stage.
  // The result type is as specified by pResultTy, a scalar or vector type with no more than four elements.
  // A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
  // 64-bit components. Two consecutive locations together can contain up to a 4-vector of 64-bit components.
  // This operation is only supported for TCS; other shader stages do not have per-vertex outputs, and
  // the frontend is expected to do its own cacheing of a written output if the shader wants to read it back again.
  //
  // @param resultTy : Type of value to read
  // @param location : Base location (row) of output
  // @param locationOffset : Location offset; must be within locationCount if variable
  // @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
  // 64-bit elements.)
  // @param locationCount : Count of locations taken by the output. Ignored if pLocationOffset is const
  // @param outputInfo : Extra output info (GS stream ID)
  // @param vertexIndex : For TCS per-vertex output: vertex index; else nullptr
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateReadGenericOutput(llvm::Type *resultTy, unsigned location, llvm::Value *locationOffset,
                                               llvm::Value *elemIdx, unsigned locationCount, InOutInfo outputInfo,
                                               llvm::Value *vertexIndex, const llvm::Twine &instName = "") = 0;

  // Create a write of (part of) a generic (user) output value, setting the value to pass to the next shader stage.
  // The value to write must be a scalar or vector type with no more than four elements.
  // A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
  // 64-bit components. Two consecutive locations together can contain up to a 4-vector of 64-bit components.
  // A non-constant pLocationOffset is currently only supported for TCS.
  //
  // @param valueToWrite : Value to write
  // @param location : Base location (row) of output
  // @param locationOffset : Location offset; must be within locationCount if variable
  // @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
  // 64-bit elements.)
  // @param locationCount : Count of locations taken by the output. Ignored if pLocationOffset is const
  // @param outputInfo : Extra output info (GS stream ID, FS integer signedness)
  // @param vertexIndex : For TCS per-vertex output: vertex index; else nullptr
  virtual llvm::Instruction *CreateWriteGenericOutput(llvm::Value *valueToWrite, unsigned location,
                                                      llvm::Value *locationOffset, llvm::Value *elemIdx,
                                                      unsigned locationCount, InOutInfo outputInfo,
                                                      llvm::Value *vertexIndex) = 0;

  // Create a write to an XFB (transform feedback / streamout) buffer.
  // The value to write must be a scalar or vector type with no more than four elements.
  // A non-constant pXfbOffset is not currently supported.
  // The value is written to the XFB only if this is in the last-vertex-stage shader, i.e. VS (if no TCS/TES/GS),
  // TES (if no GS) or GS.
  //
  // For GS, there is assumed to be an _output correspondence_, that is, for a particular stream ID, the
  // value written to the XFB offset is the same value that is written to a particular
  // built-in or user output location. CreateWriteOutput or CreateWriteBuiltIn (as applicable) must be used to
  // actually write the same value to that location/built-in, then the value written to XFB for each affected
  // vertex is undefined.
  // If calls to CreateWriteXfbOutput for multiple vertices in a primitive, or in
  // different primitives in the same stream, have different output correspondence, then it is undefined which
  // of those correspondences is actually used when writing to XFB for each affected vertex.
  //
  // @param valueToWrite : Value to write
  // @param isBuiltIn : True for built-in, false for user output (ignored if not GS)
  // @param location : Location (row) or built-in kind of output (ignored if not GS)
  // @param xfbBuffer : XFB buffer ID
  // @param xfbStride : XFB stride
  // @param xfbOffset : XFB byte offset
  // @param outputInfo : Extra output info (GS stream ID)
  virtual llvm::Instruction *CreateWriteXfbOutput(llvm::Value *valueToWrite, bool isBuiltIn, unsigned location,
                                                  unsigned xfbBuffer, unsigned xfbStride, llvm::Value *xfbOffset,
                                                  InOutInfo outputInfo) = 0;

  // Get the type of a built-in. Where the built-in has a shader-defined array length (ClipDistance,
  // CullDistance, SampleMask), inOutInfo.GetArraySize() is used as the array size.
  //
  // @param builtIn : Built-in kind, one of the BuiltIn* constants
  // @param inOutInfo : Extra input/output info (shader-defined array length)
  llvm::Type *getBuiltInTy(BuiltInKind builtIn, InOutInfo inOutInfo);

  // Create a read of (part of) a built-in input value.
  // The type of the returned value is the fixed type of the specified built-in (see BuiltInDefs.h),
  // or the element type if pIndex is not nullptr. For ClipDistance or CullDistance when pIndex is nullptr,
  // the array size is determined by inputInfo.GetArraySize().
  //
  // @param builtIn : Built-in kind, one of the BuiltIn* constants
  // @param inputInfo : Extra input info (shader-defined array length)
  // @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index, else nullptr
  // @param index : Array or vector index to access part of an input, else nullptr
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateReadBuiltInInput(BuiltInKind builtIn, InOutInfo inputInfo, llvm::Value *vertexIndex,
                                              llvm::Value *index, const llvm::Twine &instName = "") = 0;

  // Create a read of (part of) a built-in output value.
  // The type of the returned value is the fixed type of the specified built-in (see BuiltInDefs.h),
  // or the element type if pIndex is not nullptr.
  // This operation is only supported for TCS; other shader stages do not have per-vertex outputs, and
  // the frontend is expected to do its own cacheing of a written output if the shader wants to read it back again.
  //
  // @param builtIn : Built-in kind, one of the BuiltIn* constants
  // @param outputInfo : Extra output info (shader-defined array length)
  // @param vertexIndex : For TCS per-vertex output: vertex index, else nullptr
  // @param index : Array or vector index to access part of an input, else nullptr
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateReadBuiltInOutput(BuiltInKind builtIn, InOutInfo outputInfo, llvm::Value *vertexIndex,
                                               llvm::Value *index, const llvm::Twine &instName = "") = 0;

  // Create a write of (part of) a built-in output value.
  // The type of the value to write must be the fixed type of the specified built-in (see BuiltInDefs.h),
  // or the element type if pIndex is not nullptr.
  //
  // @param valueToWrite : Value to write
  // @param builtIn : Built-in kind, one of the BuiltIn* constants
  // @param outputInfo : Extra output info (shader-defined array length; GS stream id)
  // @param vertexIndex : For TCS per-vertex output: vertex index, else nullptr
  // @param index : For TCS: array or vector index to access part of an output, else nullptr
  virtual llvm::Instruction *CreateWriteBuiltInOutput(llvm::Value *valueToWrite, BuiltInKind builtIn,
                                                      InOutInfo outputInfo, llvm::Value *vertexIndex,
                                                      llvm::Value *index) = 0;

  // -----------------------------------------------------------------------------------------------------------------
  // Matrix operations

  // Create a matrix transpose.
  //
  // @param matrix : The matrix to transpose
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateTransposeMatrix(llvm::Value *const matrix, const llvm::Twine &instName = "") = 0;

  // Create matrix multiplication: matrix times scalar, resulting in matrix
  //
  // @param matrix : The column major matrix, [n x <n x float>]
  // @param scalar : The float scalar
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateMatrixTimesScalar(llvm::Value *const matrix, llvm::Value *const scalar,
                                               const llvm::Twine &instName = "") = 0;

  // Create matrix multiplication: vector times matrix, resulting in vector
  //
  // @param vector : The float vector
  // @param matrix : The column major matrix, n x <n x float>
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateVectorTimesMatrix(llvm::Value *const vector, llvm::Value *const matrix,
                                               const llvm::Twine &instName = "") = 0;

  // Create matrix multiplication: matrix times vector, resulting in vector
  //
  // @param matrix : The column major matrix, n x <n x float>
  // @param vector : The float vector
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateMatrixTimesVector(llvm::Value *const matrix, llvm::Value *const vector,
                                               const llvm::Twine &instName = "") = 0;

  // Create matrix multiplication:  matrix times matrix, resulting in matrix
  //
  // @param matrix1 : The float matrix 1
  // @param matrix2 : The float matrix 2
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateMatrixTimesMatrix(llvm::Value *const matrix1, llvm::Value *const matrix2,
                                               const llvm::Twine &instName = "") = 0;

  // Create vector outer product operation, resulting in matrix
  //
  // @param vector1 : The float vector 1
  // @param vector2 : The float vector 2
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateOuterProduct(llvm::Value *const vector1, llvm::Value *const vector2,
                                          const llvm::Twine &instName = "") = 0;

  // Create matrix determinant operation. Matrix must be square
  //
  // @param matrix : Matrix
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateDeterminant(llvm::Value *const matrix, const llvm::Twine &instName = "") = 0;

  // Create matrix inverse operation. Matrix must be square. Result is undefined if the matrix
  // is singular or poorly conditioned (nearly singular).
  //
  // @param matrix : Matrix
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateMatrixInverse(llvm::Value *const matrix, const llvm::Twine &instName = "") = 0;

  // -----------------------------------------------------------------------------------------------------------------
  // Miscellaneous operations

  // In the GS, emit the current values of outputs (as written by CreateWriteBuiltIn and CreateWriteOutput) to
  // the current output primitive in the specified output-primitive stream.
  //
  // @param streamId : Stream number, 0 if only one stream is present
  virtual llvm::Instruction *CreateEmitVertex(unsigned streamId) = 0;

  // In the GS, finish the current primitive and start a new one in the specified output-primitive stream.
  //
  // @param streamId : Stream number, 0 if only one stream is present
  virtual llvm::Instruction *CreateEndPrimitive(unsigned streamId) = 0;

  // Create a workgroup control barrier.
  virtual llvm::Instruction *CreateBarrier() = 0;

  // Create a "kill". Only allowed in a fragment shader.
  //
  // @param instName : Name to give instruction(s)
  virtual llvm::Instruction *CreateKill(const llvm::Twine &instName = "") = 0;

  // Create a "readclock".
  //
  // @param realtime : Whether to read real-time clock counter
  // @param instName : Name to give instruction(s)
  virtual llvm::Instruction *CreateReadClock(bool realtime, const llvm::Twine &instName = "") = 0;

  // Create derivative calculation on float or vector of float or half
  //
  // @param value : Input value
  // @param isDirectionY : False for derivative in X direction, true for Y direction
  // @param isFine : True for "fine" calculation, where the value in the current fragment is used. False for "coarse"
  // calculation, where it might use fewer locations to calculate.
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateDerivative(llvm::Value *value, bool isDirectionY, bool isFine,
                                        const llvm::Twine &instName = "") = 0;

  // Create a demote to helper invocation operation. Only allowed in a fragment shader.
  //
  // @param instName : Name to give instruction(s)
  virtual llvm::Instruction *CreateDemoteToHelperInvocation(const llvm::Twine &instName = "") = 0;

  // Create a helper invocation query. Only allowed in a fragment shader.
  //
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateIsHelperInvocation(const llvm::Twine &instName = "") = 0;

  // -----------------------------------------------------------------------------------------------------------------
  // Subgroup operations

  // Create a get subgroup size query.
  //
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateGetSubgroupSize(const llvm::Twine &instName = "") = 0;

  // Create a subgroup elect.
  //
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupElect(const llvm::Twine &instName = "") = 0;

  // Create a subgroup all.
  //
  // @param value : The value to compare
  // @param wqm : Executed in WQM (whole quad mode)
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupAll(llvm::Value *const value, bool wqm = false,
                                         const llvm::Twine &instName = "") = 0;

  // Create a subgroup any
  //
  // @param value : The value to compare
  // @param wqm : Executed in WQM (whole quad mode)
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupAny(llvm::Value *const value, bool wqm = false,
                                         const llvm::Twine &instName = "") = 0;

  // Create a subgroup all equal.
  //
  // @param value : The value to compare
  // @param wqm : Executed in WQM (whole quad mode)
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupAllEqual(llvm::Value *const value, bool wqm = false,
                                              const llvm::Twine &instName = "") = 0;

  // Create a subgroup broadcast.
  //
  // @param value : The value to broadcast
  // @param index : The index to broadcast from
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupBroadcast(llvm::Value *const value, llvm::Value *const index,
                                               const llvm::Twine &instName = "") = 0;

  // Create a subgroup broadcast first.
  //
  // @param value : The value to broadcast
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupBroadcastFirst(llvm::Value *const value, const llvm::Twine &instName = "") = 0;

  // Create a subgroup ballot.
  //
  // @param value : The value to contribute
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupBallot(llvm::Value *const value, const llvm::Twine &instName = "") = 0;

  // Create a subgroup inverse ballot.
  //
  // @param value : The ballot value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupInverseBallot(llvm::Value *const value, const llvm::Twine &instName = "") = 0;

  // Create a subgroup ballot bit extract.
  //
  // @param value : The ballot value
  // @param index : The index to extract from the ballot
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupBallotBitExtract(llvm::Value *const value, llvm::Value *const index,
                                                      const llvm::Twine &instName = "") = 0;

  // Create a subgroup ballot bit count.
  //
  // @param value : The ballot value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupBallotBitCount(llvm::Value *const value, const llvm::Twine &instName = "") = 0;

  // Create a subgroup ballot inclusive bit count.
  //
  // @param value : The ballot value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupBallotInclusiveBitCount(llvm::Value *const value,
                                                             const llvm::Twine &instName = "") = 0;

  // Create a subgroup ballot exclusive bit count.
  //
  // @param value : The ballot value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupBallotExclusiveBitCount(llvm::Value *const value,
                                                             const llvm::Twine &instName = "") = 0;

  // Create a subgroup ballot find least significant bit.
  //
  // @param value : The ballot value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupBallotFindLsb(llvm::Value *const value, const llvm::Twine &instName = "") = 0;

  // Create a subgroup ballot find most significant bit.
  //
  // @param value : The ballot value
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupBallotFindMsb(llvm::Value *const value, const llvm::Twine &instName = "") = 0;

  // Create a subgroup shuffle.
  //
  // @param value : The value to shuffle
  // @param index : The index to shuffle from
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupShuffle(llvm::Value *const value, llvm::Value *const index,
                                             const llvm::Twine &instName = "") = 0;

  // Create a subgroup shuffle xor.
  //
  // @param value : The value to shuffle
  // @param mask : The mask to shuffle with
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupShuffleXor(llvm::Value *const value, llvm::Value *const mask,
                                                const llvm::Twine &instName = "") = 0;

  // Create a subgroup shuffle up.
  //
  // @param value : The value to shuffle
  // @param delta : The delta to shuffle up to
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupShuffleUp(llvm::Value *const value, llvm::Value *const delta,
                                               const llvm::Twine &instName = "") = 0;

  // Create a subgroup shuffle down.
  //
  // @param value : The value to shuffle
  // @param delta : The delta to shuffle down to
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupShuffleDown(llvm::Value *const value, llvm::Value *const delta,
                                                 const llvm::Twine &instName = "") = 0;

  // Create a subgroup clustered reduction.
  //
  // @param groupArithOp : The group arithmetic operation to perform
  // @param value : The value to perform on
  // @param clusterSize : The cluster size
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupClusteredReduction(GroupArithOp groupArithOp, llvm::Value *const value,
                                                        llvm::Value *const clusterSize,
                                                        const llvm::Twine &instName = "") = 0;

  // Create a subgroup clustered inclusive scan.
  //
  // @param groupArithOp : The group arithmetic operation to perform
  // @param value : The value to perform on
  // @param clusterSize : The cluster size
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupClusteredInclusive(GroupArithOp groupArithOp, llvm::Value *const value,
                                                        llvm::Value *const clusterSize,
                                                        const llvm::Twine &instName = "") = 0;

  // Create a subgroup clustered exclusive scan.
  //
  // @param groupArithOp : The group arithmetic operation to perform
  // @param value : The value to perform on
  // @param clusterSize : The cluster size
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupClusteredExclusive(GroupArithOp groupArithOp, llvm::Value *const value,
                                                        llvm::Value *const clusterSize,
                                                        const llvm::Twine &instName = "") = 0;

  // Create a subgroup quad broadcast.
  //
  // @param value : The value to broadcast
  // @param index : The index within the quad to broadcast from
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupQuadBroadcast(llvm::Value *const value, llvm::Value *const index,
                                                   const llvm::Twine &instName = "") = 0;

  // Create a subgroup quad swap horizontal.
  //
  // @param value : The value to swap
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupQuadSwapHorizontal(llvm::Value *const value, const llvm::Twine &instName = "") = 0;

  // Create a subgroup quad swap vertical.
  //
  // @param value : The value to swap
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupQuadSwapVertical(llvm::Value *const value, const llvm::Twine &instName = "") = 0;

  // Create a subgroup quad swap diagonal.
  //
  // @param value : The value to swap
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupQuadSwapDiagonal(llvm::Value *const value, const llvm::Twine &instName = "") = 0;

  // Create a subgroup swizzle quad.
  //
  // @param value : The value to swizzle.
  // @param offset : The value to specify the swizzle offsets.
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupSwizzleQuad(llvm::Value *const value, llvm::Value *const offset,
                                                 const llvm::Twine &instName = "") = 0;

  // Create a subgroup swizzle masked.
  //
  // @param value : The value to swizzle.
  // @param mask : The value to specify the swizzle masks.
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupSwizzleMask(llvm::Value *const value, llvm::Value *const mask,
                                                 const llvm::Twine &instName = "") = 0;

  // Create a subgroup write invocation.
  //
  // @param inputValue : The value to return for all but one invocations.
  // @param writeValue : The value to return for one invocation.
  // @param index : The index of the invocation that gets the write value.
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupWriteInvocation(llvm::Value *const inputValue, llvm::Value *const writeValue,
                                                     llvm::Value *const index, const llvm::Twine &instName = "") = 0;

  // Create a subgroup mbcnt.
  //
  // @param mask : The mask to mbcnt with.
  // @param instName : Name to give instruction(s)
  virtual llvm::Value *CreateSubgroupMbcnt(llvm::Value *const mask, const llvm::Twine &instName = "") = 0;

protected:
  Builder(LgcContext *builderContext);

  // Get the ShaderModes object. For a pipeline compilation, it comes from the PipelineState. For a shader
  // compilation, there is no PipelineState, so BuilderRecorder creates its own ShaderModes.
  virtual ShaderModes *getShaderModes() = 0;

  // Get a constant of FP or vector of FP type from the given APFloat, converting APFloat semantics where necessary
  llvm::Constant *getFpConstant(llvm::Type *ty, llvm::APFloat value);

  bool m_isBuilderRecorder = false;               // Whether this is a BuilderRecorder
  ShaderStage m_shaderStage = ShaderStageInvalid; // Current shader stage being built.

  //
  // @param matrixType : The matrix type to tranpose
  llvm::Type *getTransposedMatrixTy(llvm::Type *const matrixType) const;

  typedef std::function<llvm::Value *(Builder &builder, llvm::ArrayRef<llvm::Value *> mappedArgs,
                                      llvm::ArrayRef<llvm::Value *> passthroughArgs)>
      PFN_MapToInt32Func;

  // Create a call that'll map the massage arguments to an i32 type (for functions that only take i32).
  //
  // @param mapFunc : Pointer to the function to call on each i32.
  // @param mappedArgs : The arguments to massage into an i32 type.
  // @param passthroughArgs : The arguments to pass-through without massaging.
  llvm::Value *CreateMapToInt32(PFN_MapToInt32Func mapFunc, llvm::ArrayRef<llvm::Value *> mappedArgs,
                                llvm::ArrayRef<llvm::Value *> passthroughArgs);

private:
  Builder() = delete;
  Builder(const Builder &) = delete;
  Builder &operator=(const Builder &) = delete;

  LgcContext *m_builderContext; // Builder context
};

} // namespace lgc
