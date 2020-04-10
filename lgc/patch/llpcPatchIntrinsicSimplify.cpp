/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPatchIntrinsicSimplify.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchIntrinsicSimplify.
 ***********************************************************************************************************************
 */
#if defined(_MSC_VER) || defined(__MINGW32__)
// According to Microsoft, one must set _USE_MATH_DEFINES in order to get M_PI
// from the Visual C++ cmath / math.h headers:
// https://docs.microsoft.com/en-us/cpp/c-runtime-library/math-constants?view=vs-2019
#define _USE_MATH_DEFINES
#endif

#include <math.h>

#include "llvm/InitializePasses.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#include "llpcPatchIntrinsicSimplify.h"
#include "llpcPipelineState.h"

#define DEBUG_TYPE "llpc-patch-intrinsic-simplify"

using namespace llvm;
using namespace lgc;

// =====================================================================================================================
// Initializes static members.
char PatchIntrinsicSimplify::ID = 0;

// =====================================================================================================================
// Pass creator, creates the LLVM pass intrinsic simplifcations.
FunctionPass* lgc::createPatchIntrinsicSimplify()
{
    return new PatchIntrinsicSimplify();
}

// =====================================================================================================================
// Constructor.
PatchIntrinsicSimplify::PatchIntrinsicSimplify()
    :
    FunctionPass(ID)
{
}

// =====================================================================================================================
// Get the analysis usage.
void PatchIntrinsicSimplify::getAnalysisUsage(
    AnalysisUsage& analysisUsage // [out] The analysis usage for this pass.
    ) const
{
    analysisUsage.addRequired<ScalarEvolutionWrapperPass>();
    analysisUsage.addPreserved<ScalarEvolutionWrapperPass>();
    analysisUsage.addRequired<PipelineStateWrapper>();
    analysisUsage.setPreservesCFG();
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM function.
bool PatchIntrinsicSimplify::runOnFunction(
    Function& func) // [in,out] LLVM function to be run on.
{
    SmallVector<IntrinsicInst*, 32> candidateCalls;
    bool changed = false;

    m_module = func.getParent();

    m_gfxIp = getAnalysis<PipelineStateWrapper>().getPipelineState(m_module)
        ->getTargetInfo().getGfxIpVersion();

    m_scalarEvolution = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    m_context = &func.getContext();

    // We iterate over users of intrinsics which should be less work than
    // iterating over all instructions in the module.
    for (Function& otherFunc : m_module->functions())
    {
        // Skip non intrinsics.
        if (!otherFunc.isIntrinsic())
            continue;

        for (Value* const user : otherFunc.users())
        {
            IntrinsicInst* const intrinsicCall = dyn_cast<IntrinsicInst>(user);

            if (!intrinsicCall )
                continue;

            // Skip calls not from our own function.
            if (intrinsicCall->getFunction() != &func)
                continue;

            // Record intrinsic only if it can be simplified.
            if (canSimplify(*intrinsicCall))
                candidateCalls.push_back(intrinsicCall);
        }
    }

    // Process all intrinsics which can be simplified.
    for (IntrinsicInst* const intrinsicCall : candidateCalls) {
        Value* const simplifiedValue = simplify(*intrinsicCall);

        // We did not simplify the intrinsic call.
        if (!simplifiedValue )
            continue;

        changed = true;

        intrinsicCall->replaceAllUsesWith(simplifiedValue);
        m_scalarEvolution->eraseValueFromMap(intrinsicCall);
        intrinsicCall->eraseFromParent();
    }

    return changed;
}

// =====================================================================================================================
// Check if a value is safely derived from a 16-bit value.
bool PatchIntrinsicSimplify::canSafelyConvertTo16Bit(
    Value& value // [in] The value to check
    ) const
{
    Type* valueTy = value.getType();
    if (valueTy->isHalfTy() || valueTy->isIntegerTy(16))
    {
        // The value is already 16-bit, so we don't want to convert to 16-bit again!
        return false;
    }
    else if (ConstantFP* const constFloat = dyn_cast<ConstantFP>(&value))
    {
        // We need to check that if we cast the index down to a half, we do not lose precision.
        APFloat floatValue(constFloat->getValueAPF());
        bool losesInfo = true;
        floatValue.convert(APFloat::IEEEhalf(), APFloat::rmTowardZero, &losesInfo);
        return !losesInfo;
    }
    else if (isa<FPExtInst>(&value) || isa<SExtInst>(&value) || isa<ZExtInst>(&value))
    {
        Value* const castSrc = cast<Instruction>(&value)->getOperand(0);
        Type* const castSrcTy = castSrc->getType();
        if (castSrcTy->isHalfTy() || castSrcTy->isIntegerTy(16))
            return true;
    }
    else
    {
        // Bail out if the type is not able to be used in scalar evolution.
        if (!m_scalarEvolution->isSCEVable(valueTy))
            return false;

        const SCEV* const scev = m_scalarEvolution->getSCEV(&value);

        if (valueTy->isIntegerTy() && m_scalarEvolution->getUnsignedRangeMax(scev).ule(UINT16_MAX))
            return true;
    }

    return false;
}

// =====================================================================================================================
// Convert a value to 16-bit.
Value* PatchIntrinsicSimplify::convertTo16Bit(
    Value& value, // [in] The value to convert
    IRBuilder<>& builder // [in] IRBuilder to use for instruction constructing
    ) const
{
    Type* valueTy = value.getType();
    if (isa<FPExtInst>(&value) || isa<SExtInst>(&value) || isa<ZExtInst>(&value))
        return cast<Instruction>(&value)->getOperand(0);
    else if (valueTy->isIntegerTy())
        return builder.CreateIntCast(&value, Type::getInt16Ty(*m_context), false);
    else if (valueTy->isFloatingPointTy())
        return builder.CreateFPCast(&value, Type::getHalfTy(*m_context));

    llvm_unreachable("Should never be called!");
    return nullptr;
}

// =====================================================================================================================
// Simplify image intrinsics.
Value* PatchIntrinsicSimplify::simplifyImage(
    IntrinsicInst& intrinsicCall,     // [in] The intrinsic call to simplify
    ArrayRef<unsigned>   coordOperandIndices // Operand indices of image coordinate
    ) const
{
    // If we're not on GFX9 or above, bail.
    if (m_gfxIp.major < 9)
        return nullptr;

    bool floatCoord = false;

    for (unsigned operandIndex : coordOperandIndices)
    {
        Value* const coord = intrinsicCall.getOperand(operandIndex);
        // If the values are not derived from 16-bit values, we cannot optimize.
        if (!canSafelyConvertTo16Bit(*coord))
            return nullptr;

        assert(operandIndex == coordOperandIndices[0]
            || floatCoord == coord->getType()->isFloatingPointTy());
        floatCoord = coord->getType()->isFloatingPointTy();
    }

    Type* const coordType = floatCoord ? Type::getHalfTy(*m_context) : Type::getInt16Ty(*m_context);

    Function* const intrinsic = Intrinsic::getDeclaration(m_module,
                                                           intrinsicCall.getIntrinsicID(),
                                                           {intrinsicCall.getType(), coordType});

    assert(intrinsic );

    SmallVector<Value*, 8> args(intrinsicCall.arg_operands());

    IRBuilder<> builder(&intrinsicCall);

    for (unsigned operandIndex : coordOperandIndices)
        args[operandIndex] = convertTo16Bit(*intrinsicCall.getOperand(operandIndex), builder);

    return builder.CreateCall(intrinsic, args);
}

// =====================================================================================================================
// Simplify a trigonometric intrinsic.
Value* PatchIntrinsicSimplify::simplifyTrigonometric(
    IntrinsicInst& intrinsicCall // [in] The intrinsic call to simplify
    ) const
{
    // The sin and cos function in the hardware are dividing by 2*PI beforehand.
    // This means      sin(x * 2 * PI) = amdgcn.sin(x)
    // and                      sin(x) = amdgcn.sin(x / (2 * PI))
    // We can switch to using our amdgcn trignonometric functions directly if the input conforms to the pattern:
    // <trigonometric-function>(x * (2 * PI))
    // <trigonometric-function>(x / (1 / (2 * PI)))

    BinaryOperator* const binOp = dyn_cast<BinaryOperator>(intrinsicCall.getOperand(0));

    // If the clamped value was not a binary operator, bail.
    if (!binOp )
        return nullptr;

    ConstantFP* const constMultiplicator = dyn_cast<ConstantFP>(binOp->getOperand(1));

    // If the multiplicator was not a constant, bail.
    if (!constMultiplicator )
        return nullptr;

    APFloat multiplicator(constMultiplicator->getValueAPF());

    bool losesInfo = false;

    switch (binOp->getOpcode())
    {
    case BinaryOperator::FMul:
        break;
    case BinaryOperator::FDiv:
        {
            APFloat one(1.0);
            one.convert(multiplicator.getSemantics(), APFloat::rmTowardZero, &losesInfo);
            multiplicator = one / multiplicator;
            break;
        }
    default:
        return nullptr;
    }

    APFloat pi(M_PI);
    pi.convert(multiplicator.getSemantics(), APFloat::rmTowardZero, &losesInfo);

    const APFloat twoPi = pi + pi;
    APFloat diff = twoPi - multiplicator;

    // Absolute value the result.
    diff.clearSign();

    APFloat tolerance(0.0001);
    tolerance.convert(multiplicator.getSemantics(), APFloat::rmTowardZero, &losesInfo);

    // If the value specified as two * pi was not nearly equal to ours, bail.
    if (diff.compare(tolerance) != APFloat::cmpLessThan)
        return nullptr;

    Intrinsic::ID intrinsicId = Intrinsic::not_intrinsic;

    switch (intrinsicCall.getIntrinsicID())
    {
    case Intrinsic::cos:
        intrinsicId = Intrinsic::amdgcn_cos;
        break;
    case Intrinsic::sin:
        intrinsicId = Intrinsic::amdgcn_sin;
        break;
    default:
        return nullptr;
    }

    Type* intrinsicType = intrinsicCall.getType();

    Function* const intrinsic = Intrinsic::getDeclaration(m_module,
                                                           intrinsicId,
                                                           {intrinsicType, intrinsicType});

    assert(intrinsic );

    Value* leftOperand = binOp->getOperand(0);

    // If we're not on GFX9 or above, we need to add a clamp from 0..1 (using fract).
    if (m_gfxIp.major < 9)
    {
        Function* const fractIntrinsic = Intrinsic::getDeclaration(m_module,
                                                           Intrinsic::amdgcn_fract,
                                                           {intrinsicType, intrinsicType});
        assert(fractIntrinsic );

        CallInst* const fractCall = CallInst::Create(fractIntrinsic, leftOperand, "", &intrinsicCall);
        leftOperand = fractCall;
    }

    CallInst* const newCall = CallInst::Create(intrinsic, leftOperand, "", &intrinsicCall);

    return newCall;
}

// =====================================================================================================================
// Check if an intrinsic can be simplified.
bool PatchIntrinsicSimplify::canSimplify(
    IntrinsicInst& intrinsicCall // [in] The intrinsic call to simplify
    ) const
{
    switch (intrinsicCall.getIntrinsicID())
    {
    case Intrinsic::amdgcn_image_load_1d:
    case Intrinsic::amdgcn_image_load_2d:
    case Intrinsic::amdgcn_image_load_3d:
    case Intrinsic::amdgcn_image_sample_1d:
    case Intrinsic::amdgcn_image_sample_2d:
    case Intrinsic::amdgcn_image_sample_l_1d:
    case Intrinsic::amdgcn_image_sample_3d:
    case Intrinsic::amdgcn_image_sample_l_2d:
    case Intrinsic::amdgcn_image_sample_l_3d:
    case Intrinsic::cos:
    case Intrinsic::sin:
        return true;
    default:
        return false;
    }
}

// =====================================================================================================================
// Simplify an intrinsic.
Value* PatchIntrinsicSimplify::simplify(
    IntrinsicInst& intrinsicCall // [in] The intrinsic call to simplify
    ) const
{
    switch (intrinsicCall.getIntrinsicID())
    {
    case Intrinsic::amdgcn_image_load_1d:
    case Intrinsic::amdgcn_image_sample_1d:
        return simplifyImage(intrinsicCall, {1});
    case Intrinsic::amdgcn_image_load_2d:
    case Intrinsic::amdgcn_image_sample_2d:
    case Intrinsic::amdgcn_image_sample_l_1d:
        return simplifyImage(intrinsicCall, {1, 2});
    case Intrinsic::amdgcn_image_load_3d:
    case Intrinsic::amdgcn_image_sample_3d:
    case Intrinsic::amdgcn_image_sample_l_2d:
        return simplifyImage(intrinsicCall, {1, 2, 3});
    case Intrinsic::amdgcn_image_sample_l_3d:
        return simplifyImage(intrinsicCall, {1, 2, 3, 4});
    case Intrinsic::cos:
    case Intrinsic::sin:
        return simplifyTrigonometric(intrinsicCall);
    default:
        return nullptr;
    }
}

// =====================================================================================================================
// Initializes the pass of LLVM patching operations for specifying intrinsic simplifications.
INITIALIZE_PASS_BEGIN(PatchIntrinsicSimplify, DEBUG_TYPE,
    "Patch LLVM for intrinsic simplifications", false, false)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(PipelineStateWrapper)
INITIALIZE_PASS_END(PatchIntrinsicSimplify, DEBUG_TYPE,
    "Patch LLVM for intrinsic simplifications", false, false)
