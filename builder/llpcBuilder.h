/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilder.h
 * @brief LLPC header file: declaration of Llpc::Builder interface
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llpcDebug.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/AtomicOrdering.h"

namespace llvm
{

class ModulePass;
class PassRegistry;
class Timer;

namespace legacy
{

class PassManager;

}

void initializeBuilderReplayerPass(PassRegistry&);

} // llvm

namespace Llpc
{

using namespace llvm;

class BuilderContext;
class Context;
class PipelineState;

// =====================================================================================================================
// Initialize the pass that gets created by a Builder
inline static void InitializeBuilderPasses(
    PassRegistry& passRegistry)   // Pass registry
{
    initializeBuilderReplayerPass(passRegistry);
}

// =====================================================================================================================
// The LLPC Builder interface
//
// Builder is the major part of the interface into the LLPC middle-end. Builder itself is used by the
// front-end to set up pipeline state, generate IR for LLPC-specific operations, and then run middle-end
// and back-end passes to generate to ISA.
//
// Builder is a subclass of llvm::IRBuilder, so it uses its concept of an insertion point with debug
// location, and it exposes all the IRBuilder methods for building IR. However, unlike IRBuilder, LLPC's
// Builder is designed to have a single instance that contains some other state used during the IR
// building process.
//
// The typical front-end flow to use the middle-end interface is as follows:
//
// 1. Create a BuilderContext using "new BuilderContext". A BuilderContext can, and should, be shared
//    between multiple compiles, although not concurrent compiles. BuilderContext contains state that
//    is shared between multiple compiles. Creating the BuilderContext is the point at which the
//    front-end decides whether to use BuilderImpl (generate IR directly) or BuilderRecorder (record
//    Builder calls and replay them at the start of middle-end passes).
//
// 2. Use BuilderContext::SetTargetMachine to specify which GPU we are compiling for.
//
// 3. For a single compile, use BuilderContext::CreateBuilder to create the Builder object.
//
// 4. Use Builder calls to specify the pipeline state:
//      Builder::SetUserDataNodes
//    Setting pipeline state can be deferred to just before pipeline linking if using BuilderRecorder.
//    If using BuilderImpl, it must be done here before any Builder calls that generate IR.
//
// 5. For each shader stage, create or process an IR module, using Builder calls to generate new IR.
//
// 6. Call Builder::Link to link the shader IR modules into a pipeline IR module. (This needs to be
//    done even if the pipeline only has a single shader, such as a compute pipeline.)
//    if using BuilderRecorder, this also records the pipeline state into IR metadata.
//
// 7. Call Builder::Generate to run middle-end and back-end passes and generate the ELF.
//    (Global options such as -filetype and -emit-llvm cause the output to be something other than ELF.)
//    The front-end can pass a call-back function into Builder::Generate to check a shader cache
//    after input and output mapping, and elect to remove already-cached shaders from the pipeline.
//
class Builder : public IRBuilder<>
{
public:
    // The group arithmetic operations the builder can consume.
    // NOTE: We rely on casting this implicitly to an integer, so we cannot use an enum class.
    enum GroupArithOp
    {
        IAdd = 0,
        FAdd,
        IMul,
        FMul,
        SMin,
        UMin,
        FMin,
        SMax,
        UMax,
        FMax,
        And,
        Or,
        Xor
    };

    virtual ~Builder();

    // Create the BuilderImpl or BuilderRecorder, depending on -use-builder-recorder option
    static Builder* Create(LLVMContext& context);

    // If this is a BuilderRecorder, create the BuilderReplayer pass, otherwise return nullptr.
    virtual ModulePass* CreateBuilderReplayer() { return nullptr; }

    // Get the type pElementTy, turned into a vector of the same vector width as pMaybeVecTy if the latter
    // is a vector type.
    static Type* GetConditionallyVectorizedTy(Type* pElementTy, Type* pMaybeVecTy);

    // Get the LLPC context. This overrides the IRBuilder method that gets the LLVM context.
    Llpc::Context& getContext() const;

    // Get the BuilderContext
    BuilderContext* GetBuilderContext() const { return m_pBuilderContext; }

    // Prepare a pass manager. This manually adds a target-aware TLI pass, so middle-end optimizations do not
    // think that we have library functions.
    void PreparePassManager(
        legacy::PassManager*  pPassMgr);  // [in/out] Pass manager

    // Set the current shader stage.
    void SetShaderStage(ShaderStage stage) { m_shaderStage = stage; }

    // -----------------------------------------------------------------------------------------------------------------
    // Methods to set pipeline state

    // Set the mask of shader stages that are present in the pipeline.
    void SetShaderStageMask(uint32_t mask);

    // Set the resource mapping nodes for the pipeline. "nodes" describes the user data
    // supplied to the shader as a hierarchical table (max two levels) of descriptors.
    // "immutableDescs" contains descriptors (currently limited to samplers), whose values are hard
    // coded by the application. Each one is a duplicate of one in "nodes". A use of one of these immutable
    // descriptors in the applicable Create* method is converted directly to the constant value.
    //
    // If using a BuilderImpl, this method must be called before any Create* methods.
    // If using a BuilderRecorder, it can be delayed until after linking.
    void SetUserDataNodes(
        ArrayRef<ResourceMappingNode>   nodes,            // The resource mapping nodes
        ArrayRef<DescriptorRangeValue>  rangeValues);     // The descriptor range values

    // -----------------------------------------------------------------------------------------------------------------
    // Methods to link and generate pipeline

    // Link the individual shader modules into a single pipeline module. The frontend must have
    // finished calling Builder::Create* methods and finished building the IR. In the case that
    // there are multiple shader modules, they are all freed by this call, and the linked pipeline
    // module is returned. If there is a single shader module, this might instead just return that.
    // Before calling this, each shader module needs to have one global function for the shader
    // entrypoint, then all other functions with internal linkage.
    // Returns the pipeline module, or nullptr on link failure.
    virtual Module* Link(
        ArrayRef<Module*> modules,   // Array of modules indexed by shader stage, with nullptr entry
                                     //  for any stage not present in the pipeline
        bool linkNativeStages);      // Whether to link native shader stage modules

    // Typedef of function passed in to Generate to check the shader cache.
    // Returns the updated shader stage mask, allowing the client to decide not to compile shader stages
    // that got a hit in the cache.
    typedef std::function<uint32_t(
        const Module*               pModule,      // [in] Module
        uint32_t                    stageMask,    // Shader stage mask
        ArrayRef<ArrayRef<uint8_t>> stageHashes   // Per-stage hash of in/out usage
    )> CheckShaderCacheFunc;

    // Generate pipeline module by running patch, middle-end optimization and backend codegen passes.
    // The output is normally ELF, but IR disassembly if an option is used to stop compilation early.
    // Output is written to outStream.
    // Like other Builder methods, on error, this calls report_fatal_error, which you can catch by setting
    // a diagnostic handler with LLVMContext::setDiagnosticHandler.
    virtual void Generate(
        std::unique_ptr<Module>   pipelineModule,       // IR pipeline module
        raw_pwrite_stream&        outStream,            // [in/out] Stream to write ELF or IR disassembly output
        CheckShaderCacheFunc      checkShaderCacheFunc, // Function to check shader cache in graphics pipeline
        ArrayRef<Timer*>          timers);              // Timers for: patch passes, llvm optimizations, codegen

    // -----------------------------------------------------------------------------------------------------------------
    // Base class operations

    // Create scalar from dot product of scalar or vector FP type. (The dot product of two scalars is their product.)
    // The two vectors must be the same floating point scalar/vector type.
    // Returns a value whose type is the element type of the vectors.
    virtual Value* CreateDotProduct(
        Value* const pVector1,            // [in] The float vector 1
        Value* const pVector2,            // [in] The float vector 2
        const Twine& instName = "") = 0;  // [in] Name to give instruction(s)

    // Create a call to the specified intrinsic with one operand, mangled on its type.
    // This is an override of the same method in IRBuilder<>; the difference is that this one sets fast math
    // flags from the Builder if none are specified by pFmfSource.
    CallInst* CreateUnaryIntrinsic(
        Intrinsic::ID id,                   // Intrinsic ID
        Value*        pValue,               // [in] Input value
        Instruction*  pFmfSource = nullptr, // [in] Instruction to copy fast math flags from; nullptr to get
                                            //    from Builder
        const Twine&  instName = "");       // [in] Name to give instruction

    // Create a call to the specified intrinsic with two operands of the same type, mangled on that type.
    // This is an override of the same method in IRBuilder<>; the difference is that this one sets fast math
    // flags from the Builder if none are specified by pFmfSource.
    CallInst* CreateBinaryIntrinsic(
        Intrinsic::ID id,                   // Intrinsic ID
        Value*        pValue1,              // [in] Input value 1
        Value*        pValue2,              // [in] Input value 2
        Instruction*  pFmfSource = nullptr, // [in] Instruction to copy fast math flags from; nullptr to get
                                            //    from Builder
        const Twine&  name = "");           // [in] Name to give instruction

    CallInst* CreateIntrinsic(
        Intrinsic::ID    id,                   // Intrinsic ID
        ArrayRef<Type*>  types,                // [in] Types
        ArrayRef<Value*> args,                 // [in] Input values
        Instruction*     pFmfSource = nullptr, // [in] Instruction to copy fast math flags from; nullptr to get
                                               //    from Builder
        const Twine&     name = "");           // [in] Name to give instruction

    // -----------------------------------------------------------------------------------------------------------------
    // Arithmetic operations

    // Methods to get useful FP constants. Using these (rather than just using for example
    // ConstantFP::get(.., 180 / M_PI)) ensures that we always get the same value, independent of the
    // host platform and its compiler.

    // Get a constant of FP or vector of FP type for the value PI/180, for converting radians to degrees.
    Constant* GetPiOver180(Type* pTy);

    // Get a constant of FP or vector of FP type for the value 180/PI, for converting degrees to radians.
    Constant* Get180OverPi(Type* pTy);

    // Get a constant of FP or vector of FP type for the value 1/(2^n - 1)
    Constant* GetOneOverPower2MinusOne(Type* pTy, uint32_t n);

    // Create calculation of 2D texture coordinates that would be used for accessing the selected cube map face for
    // the given cube map texture coordinates. Returns <2 x float>.
    virtual Value* CreateCubeFaceCoord(
        Value*        pCoord,             // [in] Input coordinate <3 x float>
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Create calculation of the index of the cube map face that would be accessed by a texture lookup function for
    // the given cube map texture coordinates. Returns a single float with value:
    //  0.0 = the cube map face facing the positive X direction
    //  1.0 = the cube map face facing the negative X direction
    //  2.0 = the cube map face facing the positive Y direction
    //  3.0 = the cube map face facing the negative Y direction
    //  4.0 = the cube map face facing the positive Z direction
    //  5.0 = the cube map face facing the negative Z direction
    virtual Value* CreateCubeFaceIndex(
        Value*        pCoord,             // [in] Input coordinate <3 x float>
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Create scalar or vector FP truncate operation with the given rounding mode.
    // Currently the rounding mode is only implemented for float/double -> half conversion.
    virtual Value* CreateFpTruncWithRounding(
        Value*                                pValue,             // [in] Input value
        Type*                                 pDestTy,            // [in] Type to convert to
        ConstrainedFPIntrinsic::RoundingMode  roundingMode,       // Rounding mode
        const Twine&                          instName = "") = 0; // [in] Name to give instruction(s)

    // Create quantize operation: truncates float (or vector) value to a value that is representable by a half.
    virtual Value* CreateQuantizeToFp16(
        Value*        pValue,               // [in] Input value (float or float vector)
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create signed integer modulo operation, where the sign of the result (if not zero) is the same as
    // the sign of the divisor. The result is undefined if pDivisor is zero.
    virtual Value* CreateSMod(
        Value*        pDividend,            // [in] Dividend value
        Value*        pDivisor,             // [in] Divisor value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create FP modulo operation, where the sign of the result (if not zero) is the same as
    // the sign of the divisor. The result is undefined if pDivisor is zero.
    virtual Value* CreateFMod(
        Value*        pDividend,            // [in] Dividend value
        Value*        pDivisor,             // [in] Divisor value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create scalar/vector float/half fused multiply-and-add, to compute a * b + c
    virtual Value* CreateFma(
        Value*        pA,                   // [in] One value to multiply
        Value*        pB,                   // [in] The other value to multiply
        Value*        pC,                   // [in] The value to add to the product of A and B
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create a "tan" operation for a scalar or vector float or half.
    virtual Value* CreateTan(
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create an "asin" operation for a scalar or vector float or half.
    virtual Value* CreateASin(
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create an "acos" operation for a scalar or vector float or half.
    virtual Value* CreateACos(
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create an "atan" operation for a scalar or vector float or half.
    virtual Value* CreateATan(
        Value*        pYOverX,              // [in] Input value Y/X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create an "atan2" operation for a scalar or vector float or half.
    // Returns atan(Y/X) but in the correct quadrant for the input value signs.
    virtual Value* CreateATan2(
        Value*        pY,                   // [in] Input value Y
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create a "sinh" operation for a scalar or vector float or half.
    virtual Value* CreateSinh(
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create a "cosh" operation for a scalar or vector float or half.
    virtual Value* CreateCosh(
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create a "tanh" operation for a scalar or vector float or half.
    virtual Value* CreateTanh(
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create an "asinh" operation for a scalar or vector float or half.
    virtual Value* CreateASinh(
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create an "acosh" operation for a scalar or vector float or half.
    virtual Value* CreateACosh(
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create an "atanh" operation for a scalar or vector float or half.
    virtual Value* CreateATanh(
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create a "power" operation for a scalar or vector float or half, calculating X ^ Y
    virtual Value* CreatePower(
        Value*        pX,                   // [in] Input value X
        Value*        pY,                   // [in] Input value Y
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create an "exp" operation for a scalar or vector float or half.
    virtual Value* CreateExp(
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create a "log" operation for a scalar or vector float or half.
    virtual Value* CreateLog(
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create an inverse square root operation for a scalar or vector FP type
    virtual Value* CreateInverseSqrt(
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "signed integer abs" operation for a scalar or vector integer value.
    virtual Value* CreateSAbs(
        Value*        pX,                   // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "fsign" operation for a scalar or vector floating-point type, returning -1.0, 0.0 or +1.0 if the input
    // value is negative, zero or positive.
    virtual Value* CreateFSign(
        Value*        pInValue,             // [in] Input value X
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "ssign" operation for a scalar or vector integer type, returning -1, 0 or +1 if the input
    // value is negative, zero or positive.
    virtual Value* CreateSSign(
        Value*        pX,                   // [in] Input value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "fract" operation for a scalar or vector floating-point type, returning x - floor(x).
    virtual Value* CreateFract(
        Value*        pX,                   // [in] Input value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "smoothStep" operation. Result is 0.0 if x <= edge0 and 1.0 if x >= edge1 and performs smooth Hermite
    // interpolation between 0 and 1 when edge0 < x < edge1. This is equivalent to:
    // t * t * (3 - 2 * t), where t = clamp ((x - edge0) / (edge1 - edge0), 0, 1)
    // Result is undefined if edge0 >= edge1.
    virtual Value* CreateSmoothStep(
        Value*        pEdge0,               // [in] Edge0 value
        Value*        pEdge1,               // [in] Edge1 value
        Value*        pX,                   // [in] X (input) value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "ldexp" operation: given an FP mantissa and int exponent, build an FP value
    virtual Value* CreateLdexp(
        Value*        pX,                   // [in] Mantissa
        Value*        pExp,                 // [in] Exponent
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "extract significand" operation: given an FP scalar or vector value, return the significand in the range
    // [0.5,1.0), of the same type as the input. If the input is 0, the result is 0. If the input is infinite or NaN,
    // the result is undefined.
    virtual Value* CreateExtractSignificand(
        Value*        pValue,               // [in] Input value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "extract exponent" operation: given an FP scalar or vector value, return the exponent as a signed integer.
    // If the input is (vector of) half, the result type is (vector of) i16, otherwise it is (vector of) i32.
    // If the input is 0, the result is 0. If the input is infinite or NaN, the result is undefined.
    virtual Value* CreateExtractExponent(
        Value*        pValue,               // [in] Input value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create vector cross product operation. Inputs must be <3 x FP>
    virtual Value* CreateCrossProduct(
        Value*        pX,                   // [in] Input value X
        Value*        pY,                   // [in] Input value Y
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create FP scalar/vector normalize operation: returns a scalar/vector with the same direction and magnitude 1.
    virtual Value* CreateNormalizeVector(
        Value*        pX,                   // [in] Input value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "face forward" operation: given three FP scalars/vectors {N, I, Nref}, if the dot product of
    // Nref and I is negative, the result is N, otherwise it is -N
    virtual Value* CreateFaceForward(
        Value*        pN,                   // [in] Input value "N"
        Value*        pI,                   // [in] Input value "I"
        Value*        pNref,                // [in] Input value "Nref"
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "reflect" operation. For the incident vector I and normalized surface orientation N, the result is
    // the reflection direction:
    // I - 2 * dot(N, I) * N
    virtual Value* CreateReflect(
        Value*        pI,                   // [in] Input value "I"
        Value*        pN,                   // [in] Input value "N"
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "refract" operation. For the normalized incident vector I, normalized surface orientation N and ratio
    // of indices of refraction eta, the result is the refraction vector:
    // k = 1.0 - eta * eta * (1.0 - dot(N,I) * dot(N,I))
    // If k < 0.0 the result is 0.0.
    // Otherwise, the result is eta * I - (eta * dot(N,I) + sqrt(k)) * N
    virtual Value* CreateRefract(
        Value*        pI,                   // [in] Input value "I"
        Value*        pN,                   // [in] Input value "N"
        Value*        pEta,                 // [in] Input value "eta"
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "fclamp" operation, returning min(max(x, minVal), maxVal). Result is undefined if minVal > maxVal.
    // This honors the fast math flags; clear "nnan" in fast math flags in order to obtain the "NaN avoiding
    // semantics" for the min and max where, if one input is NaN, it returns the other one.
    // It also honors the shader's FP mode being "flush denorm".
    virtual Value* CreateFClamp(
        Value*        pX,                   // [in] Value to clamp
        Value*        pMinVal,              // [in] Minimum of clamp range
        Value*        pMaxVal,              // [in] Maximum of clamp range
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "fmin" operation, returning the minimum of two scalar or vector FP values.
    // This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
    // It also honors the shader's FP mode being "flush denorm".
    virtual Value* CreateFMin(
        Value*        pValue1,              // [in] First value
        Value*        pValue2,              // [in] Second value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "fmax" operation, returning the maximum of two scalar or vector float or half values.
    // This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
    // It also honors the shader's FP mode being "flush denorm".
    virtual Value* CreateFMax(
        Value*        pValue1,              // [in] First value
        Value*        pValue2,              // [in] Second value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "fmin3" operation, returning the minimum of three scalar or vector float or half values.
    // This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
    // It also honors the shader's FP mode being "flush denorm".
    virtual Value* CreateFMin3(
        Value*        pValue1,              // [in] First value
        Value*        pValue2,              // [in] Second value
        Value*        pValue3,              // [in] Third value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "fmax3" operation, returning the maximum of three scalar or vector float or half values.
    // This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
    // It also honors the shader's FP mode being "flush denorm".
    virtual Value* CreateFMax3(
        Value*        pValue1,              // [in] First value
        Value*        pValue2,              // [in] Second value
        Value*        pValue3,              // [in] Third value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "fmid3" operation, returning the middle one of three scalar or vector float or half values.
    // This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
    // It also honors the shader's FP mode being "flush denorm".
    virtual Value* CreateFMid3(
        Value*        pValue1,              // [in] First value
        Value*        pValue2,              // [in] Second value
        Value*        pValue3,              // [in] Third value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "isInf" operation: return true if the supplied FP (or vector) value is infinity
    virtual Value* CreateIsInf(
        Value*        pX,                   // [in] Input value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "isNaN" operation: return true if the supplied FP (or vector) value is NaN
    virtual Value* CreateIsNaN(
        Value*        pX,                   // [in] Input value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create an "insert bitfield" operation for a (vector of) integer type.
    // Returns a value where the "pCount" bits starting at bit "pOffset" come from the least significant "pCount"
    // bits in "pInsert", and remaining bits come from "pBase". The result is undefined if "pCount"+"pOffset" is
    // more than the number of bits (per vector element) in "pBase" and "pInsert".
    // If "pBase" and "pInsert" are vectors, "pOffset" and "pCount" can be either scalar or vector of the same
    // width. The scalar type of "pOffset" and "pCount" must be integer, but can be different to that of "pBase"
    // and "pInsert" (and different to each other too).
    virtual Value* CreateInsertBitField(
        Value*        pBase,                // [in] Base value
        Value*        pInsert,              // [in] Value to insert (same type as base)
        Value*        pOffset,              // Bit number of least-significant end of bitfield
        Value*        pCount,               // Count of bits in bitfield
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create an "extract bitfield " operation for a (vector of) i32.
    // Returns a value where the least significant "pCount" bits come from the "pCount" bits starting at bit
    // "pOffset" in "pBase", and that is zero- or sign-extended (depending on "isSigned") to the rest of the value.
    // If "pBase" and "pInsert" are vectors, "pOffset" and "pCount" can be either scalar or vector of the same
    // width. The scalar type of "pOffset" and "pCount" must be integer, but can be different to that of "pBase"
    // (and different to each other too).
    virtual Value* CreateExtractBitField(
        Value*        pBase,                // [in] Base value
        Value*        pOffset,              // Bit number of least-significant end of bitfield
        Value*        pCount,               // Count of bits in bitfield
        bool          isSigned,             // True for a signed int bitfield extract, false for unsigned
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create "find MSB" operation for a (vector of) signed i32. For a postive number, the result is the bit number of
    // the most significant 1-bit. For a negative number, the result is the bit number of the most significant 0-bit.
    // For a value of 0 or -1, the result is -1.
    //
    // Note that unsigned "find MSB" is not provided as a Builder method, because it is easily synthesized from
    // the standard LLVM intrinsic llvm.ctlz. Similarly "find LSB" is not provided because it is easily synthesized
    // from the standard LLVM intrinsic llvm.cttz.
    virtual Value* CreateFindSMsb(
        Value*        pValue,               // [in] Input value
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // -----------------------------------------------------------------------------------------------------------------
    // Descriptor operations

    // Get the type of pointer returned by CreateLoadBufferDesc.
    PointerType* GetBufferDescTy(Type* pPointeeTy);

    // Create a load of a buffer descriptor.
    virtual Value* CreateLoadBufferDesc(
        uint32_t      descSet,            // Descriptor set
        uint32_t      binding,            // Descriptor binding
        Value*        pDescIndex,         // [in] Descriptor index
        bool          isNonUniform,       // Whether the descriptor index is non-uniform
        Type*         pPointeeTy,         // [in] Type that the returned pointer should point to.
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Add index onto pointer to image/sampler/texelbuffer/F-mask array of descriptors.
    virtual Value* CreateIndexDescPtr(
        Value*        pDescPtr,           // [in] Descriptor pointer, as returned by this function or one of
                                          //    the CreateGet*DescPtr methods
        Value*        pIndex,             // [in] Index value
        bool          isNonUniform,       // Whether the descriptor index is non-uniform
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Load image/sampler/texelbuffer/F-mask descriptor from pointer.
    // Returns <8 x i32> descriptor for image or F-mask, or <4 x i32> descriptor for sampler or texel buffer.
    virtual Value* CreateLoadDescFromPtr(
        Value*        pDescPtr,           // [in] Descriptor pointer, as returned by CreateIndexDesc or one of
                                          //    the CreateGet*DescPtr methods
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Get the type of an image descriptor.
    VectorType* GetImageDescTy();

    // Get the type of an F-mask descriptor.
    VectorType* GetFmaskDescTy();

    // Get the type of a sampler descriptor.
    VectorType* GetSamplerDescTy();

    // Get the type of a texel buffer descriptor.
    VectorType* GetTexelBufferDescTy();

    // Get the type of pointer to image or F-mask descriptor, as returned by CreateGetImageDescPtr.
    // The type is in fact a struct containing the actual pointer plus a stride in dwords.
    // Currently the stride is not set up or used by anything; in the future, CreateGet*DescPtr calls will
    // set up the stride, and CreateIndexDescPtr will use it.
    Type* GetImageDescPtrTy();

    // Get the type of pointer to F-mask descriptor, as returned by CreateGetFmaskDescPtr.
    // The type is in fact a struct containing the actual pointer plus a stride in dwords.
    // Currently the stride is not set up or used by anything; in the future, CreateGet*DescPtr calls will
    // set up the stride, and CreateIndexDescPtr will use it.
    Type* GetFmaskDescPtrTy();

    // Get the type of pointer to texel buffer descriptor, as returned by CreateGetTexelBufferDescPtr.
    // The type is in fact a struct containing the actual pointer plus a stride in dwords.
    // Currently the stride is not set up or used by anything; in the future, CreateGet*DescPtr calls will
    // set up the stride, and CreateIndexDescPtr will use it.
    Type* GetTexelBufferDescPtrTy();

    // Get the type of pointer to sampler descriptor, as returned by CreateGetSamplerDescPtr.
    // The type is in fact a struct containing the actual pointer plus a stride in dwords.
    // Currently the stride is not set up or used by anything; in the future, CreateGet*DescPtr calls will
    // set up the stride, and CreateIndexDescPtr will use it.
    Type* GetSamplerDescPtrTy();

    // Create a pointer to sampler descriptor. Returns a value of the type returned by GetSamplerDescPtrTy.
    virtual Value* CreateGetSamplerDescPtr(
        uint32_t      descSet,          // Descriptor set
        uint32_t      binding,          // Descriptor binding
        const Twine&  instName = ""     // [in] Name to give instruction(s)
    ) = 0;

    // Create a pointer to image descriptor. Returns a value of the type returned by GetImageDescPtrTy.
    virtual Value* CreateGetImageDescPtr(
        uint32_t      descSet,          // Descriptor set
        uint32_t      binding,          // Descriptor binding
        const Twine&  instName = ""     // [in] Name to give instruction(s)
    ) = 0;

    // Create a pointer to texel buffer descriptor. Returns a value of the type returned by GetTexelBufferDescPtrTy.
    virtual Value* CreateGetTexelBufferDescPtr(
        uint32_t      descSet,          // Descriptor set
        uint32_t      binding,          // Descriptor binding
        const Twine&  instName = ""     // [in] Name to give instruction(s)
    ) = 0;

    // Create a load of a F-mask descriptor. Returns a value of the type returned by GetFmaskDescPtrTy.
    virtual Value* CreateGetFmaskDescPtr(
        uint32_t      descSet,          // Descriptor set
        uint32_t      binding,          // Descriptor binding
        const Twine&  instName = ""     // [in] Name to give instruction(s)
    ) = 0;

    // Create a load of the push constants pointer.
    // This returns a pointer to the ResourceMappingNodeType::PushConst resource in the top-level user data table.
    virtual Value* CreateLoadPushConstantsPtr(
        Type*         pPushConstantsTy,   // [in] Type that the returned pointer will point to
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Create a buffer length query based on the specified descriptor.
    virtual Value* CreateGetBufferDescLength(
        Value* const  pBufferDesc,        // [in] The buffer descriptor to query.
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // -----------------------------------------------------------------------------------------------------------------
    // Image operations

    // Possible values for dimension argument for image methods.
    enum
    {
        Dim1D = 0,            // Coordinate: x
        Dim2D = 1,            // Coordinate: x, y
        Dim3D = 2,            // Coordinate: x, y, z
        DimCube = 3,          // Coordinate: x, y, face
        Dim1DArray = 4,       // Coordinate: x, slice
        Dim2DArray = 5,       // Coordinate: x, y, slice
        Dim2DMsaa = 6,        // Coordinate: x, y, fragid
        Dim2DArrayMsaa = 7,   // Coordinate: x, y, slice, fragid
        DimCubeArray = 8,     // Coordinate: x, y, face, slice (despite both SPIR-V and ISA
                              //    combining face and slice into one component)
    };

    // Get the number of coordinates for the specified dimension argument.
    static uint32_t GetImageNumCoords(
        uint32_t dim)   // Image dimension
    {
        switch (dim)
        {
        case Dim1D: return 1;
        case Dim2D: return 2;
        case Dim3D: return 3;
        case DimCube: return 3;
        case Dim1DArray: return 2;
        case Dim2DArray: return 3;
        case Dim2DMsaa: return 3;
        case Dim2DArrayMsaa: return 4;
        case DimCubeArray: return 4;
        }
        LLPC_NEVER_CALLED();
        return 0;
    }

    // Get the number of components of a size query for the specified dimension argument.
    static uint32_t GetImageQuerySizeComponentCount(
        uint32_t dim)   // Image dimension
    {
        switch (dim)
        {
        case Dim1D: return 1;
        case Dim2D: return 2;
        case Dim3D: return 3;
        case DimCube: return 2;
        case Dim1DArray: return 2;
        case Dim2DArray: return 3;
        case Dim2DMsaa: return 2;
        case Dim2DArrayMsaa: return 3;
        case DimCubeArray: return 3;
        }
        LLPC_NEVER_CALLED();
        return 0;
    }

    // Bit settings in flags argument for image methods.
    enum
    {
        ImageFlagCoherent = 1,              // Coherent memory access
        ImageFlagVolatile = 2,              // Volatile memory access
        ImageFlagSignedResult = 4,          // For a gather with integer result, whether it is signed
        ImageFlagNonUniformImage = 8,       // Whether the image descriptor is non-uniform
        ImageFlagNonUniformSampler = 0x10,  // Whether the sampler descriptor is non-uniform
        ImageFlagAddFragCoord = 0x20,       // Add FragCoord (converted to signed int) on to coordinate x,y.
                                            // Image load, store and atomic only.
        ImageFlagCheckMultiView = 0x40,     // If pipeline state enables multiview, use ViewIndex as coordinate z.
                                            // Otherwise, acts the same as ImageFlagAddFragCoord
    };

    // Address array indices for image sample and gather methods. Where an optional entry is missing (either
    // nullptr, or the array is not long enough for it), then it assumes a default value.
    enum
    {
        ImageAddressIdxCoordinate = 0,    // Coordinate - a scalar or vector of float or half exactly as wide as
                                          //    returned by GetImageNumCoords(dim)
        ImageAddressIdxProjective = 1,    // Projective coordinate - divided into each coordinate (image sample only)
                                          //  (optional; default no projective divide)
        ImageAddressIdxComponent = 2,     // Component - constant i32 component for gather
        ImageAddressIdxDerivativeX = 3,   // X derivative - vector of float or half with number of coordinates
                                          //  excluding array slice (optional; default is to use
                                          //  implicit derivatives).
        ImageAddressIdxDerivativeY = 4,   // Y derivative - vector of float or half with number of coordinates
                                          //  excluding array slice (optional; default is to use
                                          //  implicit derivatives).
        ImageAddressIdxLod = 5,           // float level of detail (optional; default is to use
                                          //  implicit computed LOD)
        ImageAddressIdxLodBias = 6,       // float bias to add to the computed LOD (optional;
                                          //  default 0.0)
        ImageAddressIdxLodClamp = 7,      // float value to clamp LOD to (optional; default
                                          //  no clamping)
        ImageAddressIdxOffset = 8,        // Offset to add to coordinates - scalar or vector of i32, padded with 0s
                                          //  if not wide enough (optional; default all 0s). Alternatively, for
                                          //  independent offsets in a gather, a 4-array of the same, which is
                                          //  implemented as four separate gather instructions
        ImageAddressIdxZCompare = 9,      // float Z-compare value (optional; default no Z-compare)
        ImageAddressCount = 10            // All image address indices are less than this
    };

    // Atomic operation, for use in CreateImageAtomic.
    enum
    {
        ImageAtomicSwap = 0,    // Atomic operation: swap
        ImageAtomicAdd = 2,     // Atomic operation: add
        ImageAtomicSub = 3,     // Atomic operation: subtract
        ImageAtomicSMin = 4,    // Atomic operation: signed minimum
        ImageAtomicUMin = 5,    // Atomic operation: unsigned minimum
        ImageAtomicSMax = 6,    // Atomic operation: signed maximum
        ImageAtomicUMax = 7,    // Atomic operation: unsigned maximum
        ImageAtomicAnd = 8,     // Atomic operation: and
        ImageAtomicOr = 9,      // Atomic operation: or
        ImageAtomicXor = 10     // Atomic operation: xor
    };

    // Create an image load.
    virtual Value* CreateImageLoad(
        Type*                   pResultTy,          // [in] Result type
        uint32_t                dim,                // Image dimension
        uint32_t                flags,              // ImageFlag* flags
        Value*                  pImageDesc,         // [in] Image descriptor or texel buffer descriptor.
        Value*                  pCoord,             // [in] Coordinates: scalar or vector i32, exactly right width
        Value*                  pMipLevel,          // [in] Mipmap level if doing load_mip, otherwise nullptr
        const Twine&            instName = "") = 0; // [in] Name to give instruction(s)

    // Create an image load with fmask. Dim must be 2DMsaa or 2DArrayMsaa. If the F-mask descriptor has a valid
    // format field, then it reads "fmask_texel_R", the R component of the texel read from the given coordinates
    // in the F-mask image, and calculates the sample number to use as the sample'th nibble (where sample=0 means
    // the least significant nibble) of fmask_texel_R. If the F-mask descriptor has an invalid format, then it
    // just uses the supplied sample number. The calculated sample is then appended to the supplied coordinates
    // for a normal image load.
    virtual Value* CreateImageLoadWithFmask(
        Type*                   pResultTy,          // [in] Result type
        uint32_t                dim,                // Image dimension, 2DMsaa or 2DArrayMsaa
        uint32_t                flags,              // ImageFlag* flags
        Value*                  pImageDesc,         // [in] Image descriptor
        Value*                  pFmaskDesc,         // [in] Fmask descriptor
        Value*                  pCoord,             // [in] Coordinates: scalar or vector i32, exactly right
                                                    //    width for given dimension excluding sample
        Value*                  pSampleNum,         // [in] Sample number, i32
        const Twine&            instName = "") = 0; // [in] Name to give instruction(s)

    // Create an image store.
    virtual Value* CreateImageStore(
        Value*                  pTexel,             // [in] Texel value to store; v4i16, v4i32, v4f16 or v4f32
        uint32_t                dim,                // Image dimension
        uint32_t                flags,              // ImageFlag* flags
        Value*                  pImageDesc,         // [in] Image descriptor or texel buffer descriptor
        Value*                  pCoord,             // [in] Coordinates: scalar or vector i32, exactly right width
        Value*                  pMipLevel,          // [in] Mipmap level if doing store_mip, otherwise nullptr
        const Twine&            instName = "") = 0; // [in] Name to give instruction(s)

    // Create an image sample.
    // The return type is specified by pResultTy as follows:
    // * If it is a struct, then the method generates a TFE (texel fail enable) operation. The first field is the
    //   texel type, and the second field is i32, where bit 0 is the TFE bit. Otherwise, the return type is the texel
    //   type.
    // * If the ZCompare address component is supplied, then the texel type is the scalar texel component
    //   type. Otherwise the texel type is a 4-vector of the texel component type.
    // * The texel component type is i32, f16 or f32.
    virtual Value* CreateImageSample(
        Type*                   pResultTy,          // [in] Result type
        uint32_t                dim,                // Image dimension
        uint32_t                flags,              // ImageFlag* flags
        Value*                  pImageDesc,         // [in] Image descriptor
        Value*                  pSamplerDesc,       // [in] Sampler descriptor
        ArrayRef<Value*>        address,            // Address and other arguments
        const Twine&            instName = "") = 0; // [in] Name to give instruction(s)

    // Create an image gather.
    // The return type is specified by pResultTy as follows:
    // * If it is a struct, then the method generates a TFE (texel fail enable) operation. The first field is the
    //   texel type, and the second field is i32, where bit 0 is the TFE bit. Otherwise, the return type is the texel
    //   type.
    // * The texel type is a 4-vector of the texel component type, which is i32, f16 or f32.
    virtual Value* CreateImageGather(
        Type*                   pResultTy,          // [in] Result type
        uint32_t                dim,                // Image dimension
        uint32_t                flags,              // ImageFlag* flags
        Value*                  pImageDesc,         // [in] Image descriptor
        Value*                  pSamplerDesc,       // [in] Sampler descriptor
        ArrayRef<Value*>        address,            // Address and other arguments
        const Twine&            instName = "") = 0; // [in] Name to give instruction(s)

    // Create an image atomic operation other than compare-and-swap. An add of +1 or -1, or a sub
    // of -1 or +1, is generated as inc or dec. Result type is the same as the input value type.
    // Normally pImageDesc is an image descriptor, as returned by CreateLoadImageDesc, and this method
    // creates an image atomic instruction. But pImageDesc can instead be a texel buffer descriptor, as
    // returned by CreateLoadTexelBufferDesc, in which case the method creates a buffer atomic instruction.
    virtual Value* CreateImageAtomic(
        uint32_t                atomicOp,           // Atomic op to create
        uint32_t                dim,                // Image dimension
        uint32_t                flags,              // ImageFlag* flags
        AtomicOrdering          ordering,           // Atomic ordering
        Value*                  pImageDesc,         // [in] Image descriptor or texel buffer descriptor
        Value*                  pCoord,             // [in] Coordinates: scalar or vector i32, exactly right width
        Value*                  pInputValue,        // [in] Input value: i32
        const Twine&            instName = "") = 0; // [in] Name to give instruction(s)

    // Create an image atomic compare-and-swap.
    // Normally pImageDesc is an image descriptor, as returned by CreateLoadImageDesc, and this method
    // creates an image atomic instruction. But pImageDesc can instead be a texel buffer descriptor, as
    // returned by CreateLoadTexelBufferDesc, in which case the method creates a buffer atomic instruction.
    virtual Value* CreateImageAtomicCompareSwap(
        uint32_t                dim,                // Image dimension
        uint32_t                flags,              // ImageFlag* flags
        AtomicOrdering          ordering,           // Atomic ordering
        Value*                  pImageDesc,         // [in] Image descriptor or texel buffer descriptor
        Value*                  pCoord,             // [in] Coordinates: scalar or vector i32, exactly right width
        Value*                  pInputValue,        // [in] Input value: i32
        Value*                  pComparatorValue,   // [in] Value to compare against: i32
        const Twine&            instName = "") = 0; // [in] Name to give instruction(s)

    // Create a query of the number of mipmap levels in an image. Returns an i32 value.
    virtual Value* CreateImageQueryLevels(
        uint32_t                dim,                // Image dimension
        uint32_t                flags,              // ImageFlag* flags
        Value*                  pImageDesc,         // [in] Image descriptor or texel buffer descriptor
        const Twine&            instName = "") = 0; // [in] Name to give instruction(s)

    // Create a query of the number of samples in an image. Returns an i32 value.
    virtual Value* CreateImageQuerySamples(
        uint32_t                dim,                // Image dimension
        uint32_t                flags,              // ImageFlag* flags
        Value*                  pImageDesc,         // [in] Image descriptor or texel buffer descriptor
        const Twine&            instName = "") = 0; // [in] Name to give instruction(s)

    // Create a query of size of an image at the specified LOD.
    // Returns an i32 scalar or vector of the width given by GetImageQuerySizeComponentCount.
    virtual Value* CreateImageQuerySize(
        uint32_t                dim,                // Image dimension
        uint32_t                flags,              // ImageFlag* flags
        Value*                  pImageDesc,         // [in] Image descriptor or texel buffer descriptor
        Value*                  pLod,               // [in] LOD
        const Twine&            instName = "") = 0; // [in] Name to give instruction(s)

    // Create a get of the LOD that would be used for an image sample with the given coordinates
    // and implicit LOD. Returns a v2f32 containing the layer number and the implicit level of
    // detail relative to the base level.
    virtual Value* CreateImageGetLod(
        uint32_t                dim,                // Image dimension
        uint32_t                flags,              // ImageFlag* flags
        Value*                  pImageDesc,         // [in] Image descriptor
        Value*                  pSamplerDesc,       // [in] Sampler descriptor
        Value*                  pCoord,             // [in] Coordinates: scalar or vector f32, exactly right
                                                    //    width without array layer
        const Twine&            instName = "") = 0; // [in] Name to give instruction(s)

    // -----------------------------------------------------------------------------------------------------------------
    // Shader input/output methods

    // Class that represents extra information on an input or output.
    // For an FS input, if HasInterpAux(), then CreateReadInput's pVertexIndex is actually an auxiliary value
    // for interpolation:
    //  - InterpLocCenter: auxiliary value is v2f32 offset from center of pixel
    //  - InterpLocSample: auxiliary value is i32 sample ID
    //  - InterpLocExplicit: auxiliary value is i32 vertex number
    class InOutInfo
    {
    public:
        // Interpolation mode
        enum
        {
            InterpModeSmooth   = 0,   // Smooth (perspective)
            InterpModeFlat     = 1,   // Flat
            InterpModeNoPersp  = 2,   // Linear (no perspective)
            InterpModeCustom   = 3,   // Custom
        };
        enum
        {
            InterpLocUnknown   = 0,   // Unknown
            InterpLocCenter    = 1,   // Center
            InterpLocCentroid  = 2,   // Centroid
            InterpLocSample    = 3,   // Sample
            InterpLocExplicit  = 4,   // Mode must be InterpModeCustom
        };

        InOutInfo() { data.u32All = 0; }
        InOutInfo(uint32_t data) { this->data.u32All = data; }
        InOutInfo(const InOutInfo& inOutInfo) { data.u32All = inOutInfo.GetData(); }

        uint32_t GetData() const { return data.u32All; }

        uint32_t GetInterpMode() const { return data.bits.interpMode; }
        void SetInterpMode(uint32_t mode) { data.bits.interpMode = mode; }

        uint32_t GetInterpLoc() const { return data.bits.interpLoc; }
        void SetInterpLoc(uint32_t loc) { data.bits.interpLoc = loc; }

        bool HasInterpAux() const { return data.bits.hasInterpAux; }
        void SetHasInterpAux(bool hasInterpAux = true) { data.bits.hasInterpAux = hasInterpAux; }

        bool HasStreamId() const { return data.bits.hasStreamId; }
        uint32_t GetStreamId() const { return data.bits.streamId; }
        void SetStreamId(uint32_t streamId) { data.bits.hasStreamId = true; data.bits.streamId = streamId; }

        bool IsSigned() const { return data.bits.isSigned; }
        void SetIsSigned(bool isSigned = true) { data.bits.isSigned = isSigned; }

        uint32_t GetArraySize() const { return data.bits.arraySize; }
        void SetArraySize(uint32_t arraySize) { data.bits.arraySize = arraySize; }

    private:
        union
        {
            struct
            {
                unsigned interpMode   : 4;  // FS input: interpolation mode
                unsigned interpLoc    : 3;  // FS input: interpolation location
                unsigned hasInterpAux : 1;  // FS input: there is an interpolation auxiliary value
                unsigned streamId     : 2;  // GS output: vertex stream ID (0 if none)
                unsigned hasStreamId  : 1;  // GS output: true if it has a stream ID
                unsigned isSigned     : 1;  // FS output: is signed integer. Determines whether i16-component output
                                            //    is zero- or sign-extended
                unsigned arraySize    : 4;  // Built-in array input: shader-defined array size. Must be set for
                                            //    a read or write of ClipDistance or CullDistance that is of the
                                            //    whole array or of an element with a variable index.
            }
            bits;
            uint32_t  u32All;
        } data;
    };

    // Define built-in kind enum.
    enum BuiltInKind
    {
#define BUILTIN(name, number, out, in, type) BuiltIn ## name = number,
#include "llpcBuilderBuiltIns.h"
#undef BUILTIN
    };

    // Create a read of (part of) a generic (user) input value, passed from the previous shader stage.
    // The result type is as specified by pResultTy, a scalar or vector type with no more than four elements.
    // A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
    // 64-bit components. Two consecutive locations together can contain up to a 4-vector of 64-bit components.
    // A non-constant pLocationOffset is currently only supported for TCS and TES, and for an FS custom-interpolated
    // input.
    virtual Value* CreateReadGenericInput(
        Type*         pResultTy,          // [in] Type of value to read
        uint32_t      location,           // Base location (row) of input
        Value*        pLocationOffset,    // [in] Location offset; must be within locationCount if variable
        Value*        pElemIdx,           // [in] Element index in vector. (This is the SPIR-V "component", except
                                          //      that it is half the component for 64-bit elements.)
        uint32_t      locationCount,      // Count of locations taken by the input. Ignored if pLocationOffset is const
        InOutInfo     inputInfo,          // Extra input info (FS interp info)
        Value*        pVertexIndex,       // [in] For TCS/TES/GS per-vertex input: vertex index;
                                          //      for FS custom interpolated input: auxiliary interpolation value;
                                          //      else nullptr
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Create a read of (part of) a generic (user) output value, returning the value last written in this shader stage.
    // The result type is as specified by pResultTy, a scalar or vector type with no more than four elements.
    // A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
    // 64-bit components. Two consecutive locations together can contain up to a 4-vector of 64-bit components.
    // This operation is only supported for TCS; other shader stages do not have per-vertex outputs, and
    // the frontend is expected to do its own cacheing of a written output if the shader wants to read it back again.
    virtual Value* CreateReadGenericOutput(
        Type*         pResultTy,          // [in] Type of value to read
        uint32_t      location,           // Base location (row) of output
        Value*        pLocationOffset,    // [in] Location offset; must be within locationCount if variable
        Value*        pElemIdx,           // [in] Element index in vector. (This is the SPIR-V "component", except
                                          //      that it is half the component for 64-bit elements.)
        uint32_t      locationCount,      // Count of locations taken by the output. Ignored if pLocationOffset is const
        InOutInfo     outputInfo,         // Extra output info (GS stream ID)
        Value*        pVertexIndex,       // [in] For TCS per-vertex output: vertex index; else nullptr
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Create a write of (part of) a generic (user) output value, setting the value to pass to the next shader stage.
    // The value to write must be a scalar or vector type with no more than four elements.
    // A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
    // 64-bit components. Two consecutive locations together can contain up to a 4-vector of 64-bit components.
    // A non-constant pLocationOffset is currently only supported for TCS.
    virtual Instruction* CreateWriteGenericOutput(
        Value*        pValueToWrite,      // [in] Value to write
        uint32_t      location,           // Base location (row) of output
        Value*        pLocationOffset,    // [in] Location offset; must be within locationCount if variable
        Value*        pElemIdx,           // [in] Element index in vector. (This is the SPIR-V "component", except
                                          //      that it is half the component for 64-bit elements.)
        uint32_t      locationCount,      // Count of locations taken by the output. Ignored if pLocationOffset is const
        InOutInfo     outputInfo,         // Extra output info (GS stream ID, FS integer signedness)
        Value*        pVertexIndex) = 0;  // [in] For TCS per-vertex output: vertex index; else nullptr

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
    virtual Instruction* CreateWriteXfbOutput(
        Value*        pValueToWrite,      // [in] Value to write
        bool          isBuiltIn,          // True for built-in, false for user output (ignored if not GS)
        uint32_t      location,           // Location (row) or built-in kind of output (ignored if not GS)
        uint32_t      xfbBuffer,          // XFB buffer number
        uint32_t      xfbStride,          // XFB stride
        Value*        pXfbOffset,         // [in] XFB byte offset
        InOutInfo     outputInfo) = 0;    // Extra output info (GS stream ID)

    // Get the type of a built-in. Where the built-in has a shader-defined array length (ClipDistance,
    // CullDistance, SampleMask), inOutInfo.GetArraySize() is used as the array size.
    Type* GetBuiltInTy(
        BuiltInKind   builtIn,            // Built-in kind, one of the BuiltIn* constants
        InOutInfo     inOutInfo);         // Extra input/output info (shader-defined array length)

    // Create a read of (part of) a built-in input value.
    // The type of the returned value is the fixed type of the specified built-in (see llpcBuilderBuiltIns.h),
    // or the element type if pIndex is not nullptr. For ClipDistance or CullDistance when pIndex is nullptr,
    // the array size is determined by inputInfo.GetArraySize().
    virtual Value* CreateReadBuiltInInput(
        BuiltInKind   builtIn,            // Built-in kind, one of the BuiltIn* constants
        InOutInfo     inputInfo,          // Extra input info (shader-defined array length)
        Value*        pVertexIndex,       // [in] For TCS/TES/GS per-vertex input: vertex index, else nullptr
        Value*        pIndex,             // [in] Array or vector index to access part of an input, else nullptr
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Create a read of (part of) a built-in output value.
    // The type of the returned value is the fixed type of the specified built-in (see llpcBuilderBuiltIns.h),
    // or the element type if pIndex is not nullptr.
    // This operation is only supported for TCS; other shader stages do not have per-vertex outputs, and
    // the frontend is expected to do its own cacheing of a written output if the shader wants to read it back again.
    virtual Value* CreateReadBuiltInOutput(
        BuiltInKind   builtIn,            // Built-in kind, one of the BuiltIn* constants
        InOutInfo     outputInfo,         // Extra output info (shader-defined array length)
        Value*        pVertexIndex,       // [in] For TCS per-vertex output: vertex index, else nullptr
        Value*        pIndex,             // [in] Array or vector index to access part of an input, else nullptr
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Create a write of (part of) a built-in output value.
    // The type of the value to write must be the fixed type of the specified built-in (see llpcBuilderBuiltIns.h),
    // or the element type if pIndex is not nullptr.
    virtual Instruction* CreateWriteBuiltInOutput(
        Value*        pValueToWrite,      // [in] Value to write
        BuiltInKind   builtIn,            // Built-in kind, one of the BuiltIn* constants
        InOutInfo     outputInfo,         // Extra output info (shader-defined array length; GS stream id)
        Value*        pVertexIndex,       // [in] For TCS per-vertex output: vertex index, else nullptr
        Value*        pIndex) = 0;        // [in] Array or vector index to access part of an input, else nullptr

    // -----------------------------------------------------------------------------------------------------------------
    // Matrix operations

    // Create a matrix transpose.
    virtual Value* CreateTransposeMatrix(
        Value* const pMatrix,            // [in] The matrix to transpose
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create matrix multiplication: matrix times scalar, resulting in matrix
    virtual Value* CreateMatrixTimesScalar(
        Value* const pMatrix,             // [in] The column major matrix, [n x <n x float>]
        Value* const pScalar,             // [in] The float scalar
        const Twine& instName = "") = 0;  // [in] Name to give instruction(s)

    // Create matrix multiplication: vector times matrix, resulting in vector
    virtual Value* CreateVectorTimesMatrix(
        Value* const pVector,              // [in] The float vector
        Value* const pMatrix,              // [in] The column major matrix, n x <n x float>
        const Twine& instName = "") = 0;   // [in] Name to give instruction(s)

    // Create matrix multiplication: matrix times vector, resulting in vector
    virtual Value* CreateMatrixTimesVector(
        Value* const pMatrix,             // [in] The column major matrix, n x <n x float>
        Value* const pVector,             // [in] The float vector
        const Twine& instName = "") = 0;  // [in] Name to give instruction(s)

    // Create matrix multiplication:  matrix times matrix, resulting in matrix
    virtual Value* CreateMatrixTimesMatrix(
        Value* const pMatrix1,             // [in] The float matrix 1
        Value* const pMatrix2,             // [in] The float matrix 2
        const Twine& instName = "") = 0 ;  // [in] Name to give instruction(s)

    // Create vector outer product operation, resulting in matrix
    virtual Value* CreateOuterProduct(
        Value* const pVector1,            // [in] The float vector 1
        Value* const pVector2,            // [in] The float vector 2
        const Twine& instName = "") = 0;  // [in] Name to give instruction(s)

    // Create matrix determinant operation. Matrix must be square
    virtual Value* CreateDeterminant(
        Value* const pMatrix,             // [in] Matrix
        const Twine& instName = "") = 0;  // [in] Name to give instruction(s)

    // Create matrix inverse operation. Matrix must be square. Result is undefined if the matrix
    // is singular or poorly conditioned (nearly singular).
    virtual Value* CreateMatrixInverse(
        Value* const pMatrix,             // [in] Matrix
        const Twine& instName = "") = 0;  // [in] Name to give instruction(s)

    // -----------------------------------------------------------------------------------------------------------------
    // Miscellaneous operations

    // In the GS, emit the current values of outputs (as written by CreateWriteBuiltIn and CreateWriteOutput) to
    // the current output primitive in the specified output-primitive stream.
    virtual Instruction* CreateEmitVertex(
        uint32_t                      streamId) = 0;      // Stream number, 0 if only one stream is present

    // In the GS, finish the current primitive and start a new one in the specified output-primitive stream.
    virtual Instruction* CreateEndPrimitive(
        uint32_t                      streamId) = 0;      // Stream number, 0 if only one stream is present

    // Create a workgroup control barrier.
    virtual Instruction* CreateBarrier() = 0;

    // Create a "kill". Only allowed in a fragment shader.
    virtual Instruction* CreateKill(
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Create a "readclock".
    virtual Instruction* CreateReadClock(
        bool          realtime,           // Whether to read real-time clock counter
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Create derivative calculation on float or vector of float or half
    virtual Value* CreateDerivative(
        Value*        pValue,               // [in] Input value
        bool          isDirectionY,         // False for derivative in X direction, true for Y direction
        bool          isFine,               // True for "fine" calculation, where the value in the current fragment
                                            //   is used. False for "coarse" calculation, where it might use fewer
                                            //   locations to calculate.
        const Twine&  instName = "") = 0;   // [in] Name to give instruction(s)

    // Create a demote to helper invocation operation. Only allowed in a fragment shader.
    virtual Instruction* CreateDemoteToHelperInvocation(
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // Create a helper invocation query. Only allowed in a fragment shader.
    virtual Value* CreateIsHelperInvocation(
        const Twine&  instName = "") = 0; // [in] Name to give instruction(s)

    // -----------------------------------------------------------------------------------------------------------------
    // Subgroup operations

    // Create a get subgroup size query.
    virtual Value* CreateGetSubgroupSize(
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup elect.
    virtual Value* CreateSubgroupElect(
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup all.
    virtual Value* CreateSubgroupAll(
        Value* const pValue,             // [in] The value to compare
        bool         wqm = false,        // Executed in WQM (whole quad mode)
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup any
    virtual Value* CreateSubgroupAny(
        Value* const pValue,             // [in] The value to compare
        bool         wqm = false,        // Executed in WQM (whole quad mode)
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup all equal.
    virtual Value* CreateSubgroupAllEqual(
        Value* const pValue,             // [in] The value to compare
        bool         wqm = false,        // Executed in WQM (whole quad mode)
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup broadcast.
    virtual Value* CreateSubgroupBroadcast(
        Value* const pValue,             // [in] The value to broadcast
        Value* const pIndex,             // [in] The index to broadcast from
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup broadcast first.
    virtual Value* CreateSubgroupBroadcastFirst(
        Value* const pValue,             // [in] The value to broadcast
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot.
    virtual Value* CreateSubgroupBallot(
        Value* const pValue,             // [in] The value to contribute
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup inverse ballot.
    virtual Value* CreateSubgroupInverseBallot(
        Value* const pValue,             // [in] The ballot value
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot bit extract.
    virtual Value* CreateSubgroupBallotBitExtract(
        Value* const pValue,             // [in] The ballot value
        Value* const pIndex,             // [in] The index to extract from the ballot
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot bit count.
    virtual Value* CreateSubgroupBallotBitCount(
        Value* const pValue,             // [in] The ballot value
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot inclusive bit count.
    virtual Value* CreateSubgroupBallotInclusiveBitCount(
        Value* const pValue,             // [in] The ballot value
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot exclusive bit count.
    virtual Value* CreateSubgroupBallotExclusiveBitCount(
        Value* const pValue,             // [in] The ballot value
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot find least significant bit.
    virtual Value* CreateSubgroupBallotFindLsb(
        Value* const pValue,             // [in] The ballot value
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup ballot find most significant bit.
    virtual Value* CreateSubgroupBallotFindMsb(
        Value* const pValue,             // [in] The ballot value
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup shuffle.
    virtual Value* CreateSubgroupShuffle(
        Value* const pValue,             // [in] The value to shuffle
        Value* const pIndex,             // [in] The index to shuffle from
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup shuffle xor.
    virtual Value* CreateSubgroupShuffleXor(
        Value* const pValue,             // [in] The value to shuffle
        Value* const pMask,              // [in] The mask to shuffle with
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup shuffle up.
    virtual Value* CreateSubgroupShuffleUp(
        Value* const pValue,             // [in] The value to shuffle
        Value* const pDelta,             // [in] The delta to shuffle up to
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup shuffle down.
    virtual Value* CreateSubgroupShuffleDown(
        Value* const pValue,             // [in] The value to shuffle
        Value* const pDelta,             // [in] The delta to shuffle down to
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup clustered reduction.
    virtual Value* CreateSubgroupClusteredReduction(
        GroupArithOp groupArithOp,       // The group arithmetic operation to perform
        Value* const pValue,             // [in] The value to perform on
        Value* const pClusterSize,       // [in] The cluster size
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup clustered inclusive scan.
    virtual Value* CreateSubgroupClusteredInclusive(
        GroupArithOp groupArithOp,       // The group arithmetic operation to perform
        Value* const pValue,             // [in] The value to perform on
        Value* const pClusterSize,       // [in] The cluster size
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup clustered exclusive scan.
    virtual Value* CreateSubgroupClusteredExclusive(
        GroupArithOp groupArithOp,       // The group arithmetic operation to perform
        Value* const pValue,             // [in] The value to perform on
        Value* const pClusterSize,       // [in] The cluster size
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup quad broadcast.
    virtual Value* CreateSubgroupQuadBroadcast(
        Value* const pValue,             // [in] The value to broadcast
        Value* const pIndex,             // [in] the index within the quad to broadcast from
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup quad swap horizontal.
    virtual Value* CreateSubgroupQuadSwapHorizontal(
        Value* const pValue,             // [in] The value to swap
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup quad swap vertical.
    virtual Value* CreateSubgroupQuadSwapVertical(
        Value* const pValue,             // [in] The value to swap
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup quad swap diagonal.
    virtual Value* CreateSubgroupQuadSwapDiagonal(
        Value* const pValue,             // [in] The value to swap
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup swizzle quad.
    virtual Value* CreateSubgroupSwizzleQuad(
        Value* const pValue,             // [in] The value to swizzle.
        Value* const pOffset,            // [in] The value to specify the swizzle offsets.
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup swizzle masked.
    virtual Value* CreateSubgroupSwizzleMask(
        Value* const pValue,             // [in] The value to swizzle.
        Value* const pMask,              // [in] The value to specify the swizzle masks.
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup write invocation.
    virtual Value* CreateSubgroupWriteInvocation(
        Value* const pInputValue,        // [in] The value to return for all but one invocations.
        Value* const pWriteValue,        // [in] The value to return for one invocation.
        Value* const pIndex,             // [in] The index of the invocation that gets the write value.
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // Create a subgroup mbcnt.
    virtual Value* CreateSubgroupMbcnt(
        Value* const pMask,              // [in] The mask to mbcnt with.
        const Twine& instName = "") = 0; // [in] Name to give instruction(s)

    // -----------------------------------------------------------------------------------------------------------------

protected:
    Builder(BuilderContext* pBuilderContext);

    friend BuilderContext;

    static Builder* CreateBuilderImpl(BuilderContext* pBuilderContext);
    static Builder* CreateBuilderRecorder(BuilderContext* pBuilderContext);

    // Const version of GetPipelineState. This is used in BuilderImpl, and we know it does not need allocating.
    PipelineState* GetPipelineState() const { return m_pPipelineState; }

    // Get PipelineState, allocating if necessary. If it is allocated here (rather than passed in by
    // BuilderImpl::SetPipelineState), then it is freed when the Builder is freed.
    PipelineState* GetPipelineState();

    // Get a constant of FP or vector of FP type from the given APFloat, converting APFloat semantics where necessary
    Constant* GetFpConstant(Type* pTy, APFloat value);

    // -----------------------------------------------------------------------------------------------------------------

    ShaderStage                     m_shaderStage = ShaderStageInvalid; // Current shader stage being built.
    std::unique_ptr<PipelineState>  m_pAllocatedPipelineState;          // Pipeline state allocated by this Builder
    PipelineState*                  m_pPipelineState = nullptr;         // Pipeline state to use in this Builder

    Type* GetTransposedMatrixTy(
        Type* const pMatrixType) const; // [in] The matrix type to tranpose

    typedef std::function<Value* (Builder& builder,
                                  ArrayRef<Value*> mappedArgs,
                                  ArrayRef<Value*> passthroughArgs)> PFN_MapToInt32Func;

    // Create a call that'll map the massage arguments to an i32 type (for functions that only take i32).
    Value* CreateMapToInt32(
        PFN_MapToInt32Func  pfnMapFunc,       // [in] Pointer to the function to call on each i32.
        ArrayRef<Value*>    mappedArgs,       // The arguments to massage into an i32 type.
        ArrayRef<Value*>    passthroughArgs); // The arguments to pass-through without massaging.
private:
    LLPC_DISALLOW_DEFAULT_CTOR(Builder)
    LLPC_DISALLOW_COPY_AND_ASSIGN(Builder)

    // -----------------------------------------------------------------------------------------------------------------

    BuilderContext* m_pBuilderContext;      // Builder context
};

} // Llpc
