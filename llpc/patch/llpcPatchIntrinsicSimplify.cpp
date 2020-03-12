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
 * @brief LLPC source file: contains implementation of class Llpc::PatchIntrinsicSimplify.
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
#include "llpcTargetInfo.h"

#define DEBUG_TYPE "llpc-patch-intrinsic-simplify"

using namespace llvm;
using namespace Llpc;

// =====================================================================================================================
// Initializes static members.
char PatchIntrinsicSimplify::ID = 0;

// =====================================================================================================================
// Pass creator, creates the LLVM pass intrinsic simplifcations.
FunctionPass* Llpc::CreatePatchIntrinsicSimplify()
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

    m_pModule = func.getParent();

    m_gfxIp = getAnalysis<PipelineStateWrapper>().GetPipelineState(m_pModule)
        ->GetTargetInfo().GetGfxIpVersion();

    m_pScalarEvolution = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    m_pContext = &func.getContext();

    // We iterate over users of intrinsics which should be less work than
    // iterating over all instructions in the module.
    for (Function& otherFunc : m_pModule->functions())
    {
        // Skip non intrinsics.
        if (otherFunc.isIntrinsic() == false)
        {
            continue;
        }

        for (Value* const pUser : otherFunc.users())
        {
            IntrinsicInst* const pIntrinsicCall = dyn_cast<IntrinsicInst>(pUser);

            if (pIntrinsicCall == nullptr)
            {
                continue;
            }

            // Skip calls not from our own function.
            if (pIntrinsicCall->getFunction() != &func)
            {
                continue;
            }

            // Record intrinsic only if it can be simplified.
            if (CanSimplify(*pIntrinsicCall))
            {
                candidateCalls.push_back(pIntrinsicCall);
            }
        }
    }

    // Process all intrinsics which can be simplified.
    for (IntrinsicInst* const pIntrinsicCall : candidateCalls) {
        Value* const pSimplifiedValue = Simplify(*pIntrinsicCall);

        // We did not simplify the intrinsic call.
        if (pSimplifiedValue == nullptr)
        {
            continue;
        }

        changed = true;

        pIntrinsicCall->replaceAllUsesWith(pSimplifiedValue);
        pIntrinsicCall->eraseFromParent();
        m_pScalarEvolution->eraseValueFromMap(pIntrinsicCall);
    }

    return changed;
}

// =====================================================================================================================
// Check if a value is safely derived from a 16-bit value.
bool PatchIntrinsicSimplify::CanSafelyConvertTo16Bit(
    Value& value // [in] The value to check
    ) const
{
    Type* pValueTy = value.getType();
    if (pValueTy->isHalfTy() || pValueTy->isIntegerTy(16))
    {
        // The value is already 16-bit, so we don't want to convert to 16-bit again!
        return false;
    }
    else if (ConstantFP* const pConstFloat = dyn_cast<ConstantFP>(&value))
    {
        // We need to check that if we cast the index down to a half, we do not lose precision.
        APFloat floatValue(pConstFloat->getValueAPF());
        bool losesInfo = true;
        floatValue.convert(APFloat::IEEEhalf(), APFloat::rmTowardZero, &losesInfo);
        return (losesInfo == false);
    }
    else if (isa<FPExtInst>(&value) || isa<SExtInst>(&value) || isa<ZExtInst>(&value))
    {
        Value* const pCastSrc = cast<Instruction>(&value)->getOperand(0);
        Type* const pCastSrcTy = pCastSrc->getType();
        if (pCastSrcTy->isHalfTy() || pCastSrcTy->isIntegerTy(16))
        {
            return true;
        }
    }
    else
    {
        // Bail out if the type is not able to be used in scalar evolution.
        if (m_pScalarEvolution->isSCEVable(pValueTy) == false)
        {
            return false;
        }

        const SCEV* const pScev = m_pScalarEvolution->getSCEV(&value);

        if (pValueTy->isIntegerTy() && (m_pScalarEvolution->getUnsignedRangeMax(pScev).ule(UINT16_MAX)))
        {
            return true;
        }
    }

    return false;
}

// =====================================================================================================================
// Convert a value to 16-bit.
Value* PatchIntrinsicSimplify::ConvertTo16Bit(
    Value& value, // [in] The value to convert
    IRBuilder<>& builder // [in] IRBuilder to use for instruction constructing
    ) const
{
    Type* pValueTy = value.getType();
    if (isa<FPExtInst>(&value) || isa<SExtInst>(&value) || isa<ZExtInst>(&value))
    {
        return cast<Instruction>(&value)->getOperand(0);
    }
    else if (pValueTy->isIntegerTy())
    {
        return builder.CreateIntCast(&value, Type::getInt16Ty(*m_pContext), false);
    }
    else if (pValueTy->isFloatingPointTy())
    {
        return builder.CreateFPCast(&value, Type::getHalfTy(*m_pContext));
    }

    LLPC_NEVER_CALLED();
    return nullptr;
}

// =====================================================================================================================
// Simplify image intrinsics.
Value* PatchIntrinsicSimplify::SimplifyImage(
    IntrinsicInst& intrinsicCall,     // [in] The intrinsic call to simplify
    ArrayRef<uint32_t>   coordOperandIndices // Operand indices of image coordinate
    ) const
{
    // If we're not on GFX9 or above, bail.
    if (m_gfxIp.major < 9)
    {
        return nullptr;
    }

    bool floatCoord = false;

    for (uint32_t operandIndex : coordOperandIndices)
    {
        Value* const pCoord = intrinsicCall.getOperand(operandIndex);
        // If the values are not derived from 16-bit values, we cannot optimize.
        if (CanSafelyConvertTo16Bit(*pCoord) == false)
        {
            return nullptr;
        }

        LLPC_ASSERT(operandIndex == coordOperandIndices[0]
            || floatCoord == pCoord->getType()->isFloatingPointTy());
        floatCoord = pCoord->getType()->isFloatingPointTy();
    }

    Type* const pCoordType = floatCoord ? Type::getHalfTy(*m_pContext) : Type::getInt16Ty(*m_pContext);

    Function* const pIntrinsic = Intrinsic::getDeclaration(m_pModule,
                                                           intrinsicCall.getIntrinsicID(),
                                                           {intrinsicCall.getType(), pCoordType});

    LLPC_ASSERT(pIntrinsic != nullptr);

    SmallVector<Value*, 8> args(intrinsicCall.arg_operands());

    IRBuilder<> builder(&intrinsicCall);

    for (uint32_t operandIndex : coordOperandIndices)
    {
        args[operandIndex] = ConvertTo16Bit(*intrinsicCall.getOperand(operandIndex), builder);
    }

    return builder.CreateCall(pIntrinsic, args);
}

// =====================================================================================================================
// Simplify a trigonometric intrinsic.
Value* PatchIntrinsicSimplify::SimplifyTrigonometric(
    IntrinsicInst& intrinsicCall // [in] The intrinsic call to simplify
    ) const
{
    // The sin and cos function in the hardware are dividing by 2*PI beforehand.
    // This means      sin(x * 2 * PI) = amdgcn.sin(x)
    // and                      sin(x) = amdgcn.sin(x / (2 * PI))
    // We can switch to using our amdgcn trignonometric functions directly if the input conforms to the pattern:
    // <trigonometric-function>(x * (2 * PI))
    // <trigonometric-function>(x / (1 / (2 * PI)))

    BinaryOperator* const pBinOp = dyn_cast<BinaryOperator>(intrinsicCall.getOperand(0));

    // If the clamped value was not a binary operator, bail.
    if (pBinOp == nullptr)
    {
        return nullptr;
    }

    ConstantFP* const pConstMultiplicator = dyn_cast<ConstantFP>(pBinOp->getOperand(1));

    // If the multiplicator was not a constant, bail.
    if (pConstMultiplicator == nullptr)
    {
        return nullptr;
    }

    APFloat multiplicator(pConstMultiplicator->getValueAPF());

    bool losesInfo = false;

    switch (pBinOp->getOpcode())
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
    {
        return nullptr;
    }

    Intrinsic::ID intrinsic = Intrinsic::not_intrinsic;

    switch (intrinsicCall.getIntrinsicID())
    {
    case Intrinsic::cos:
        intrinsic = Intrinsic::amdgcn_cos;
        break;
    case Intrinsic::sin:
        intrinsic = Intrinsic::amdgcn_sin;
        break;
    default:
        return nullptr;
    }

    Type* pIntrinsicType = intrinsicCall.getType();

    Function* const pIntrinsic = Intrinsic::getDeclaration(m_pModule,
                                                           intrinsic,
                                                           {pIntrinsicType, pIntrinsicType});

    LLPC_ASSERT(pIntrinsic != nullptr);

    Value* pLeftOperand = pBinOp->getOperand(0);

    // If we're not on GFX9 or above, we need to add a clamp from 0..1 (using fract).
    if (m_gfxIp.major < 9)
    {
        Function* const pFractIntrinsic = Intrinsic::getDeclaration(m_pModule,
                                                           Intrinsic::amdgcn_fract,
                                                           {pIntrinsicType, pIntrinsicType});
        LLPC_ASSERT(pFractIntrinsic != nullptr);

        CallInst* const pFractCall = CallInst::Create(pFractIntrinsic, pLeftOperand, "", &intrinsicCall);
        pLeftOperand = pFractCall;
    }

    CallInst* const pNewCall = CallInst::Create(pIntrinsic, pLeftOperand, "", &intrinsicCall);

    return pNewCall;
}

// =====================================================================================================================
// Check if an intrinsic can be simplified.
bool PatchIntrinsicSimplify::CanSimplify(
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
Value* PatchIntrinsicSimplify::Simplify(
    IntrinsicInst& intrinsicCall // [in] The intrinsic call to simplify
    ) const
{
    switch (intrinsicCall.getIntrinsicID())
    {
    case Intrinsic::amdgcn_image_load_1d:
    case Intrinsic::amdgcn_image_sample_1d:
        return SimplifyImage(intrinsicCall, {1});
    case Intrinsic::amdgcn_image_load_2d:
    case Intrinsic::amdgcn_image_sample_2d:
    case Intrinsic::amdgcn_image_sample_l_1d:
        return SimplifyImage(intrinsicCall, {1, 2});
    case Intrinsic::amdgcn_image_load_3d:
    case Intrinsic::amdgcn_image_sample_3d:
    case Intrinsic::amdgcn_image_sample_l_2d:
        return SimplifyImage(intrinsicCall, {1, 2, 3});
    case Intrinsic::amdgcn_image_sample_l_3d:
        return SimplifyImage(intrinsicCall, {1, 2, 3, 4});
    case Intrinsic::cos:
    case Intrinsic::sin:
        return SimplifyTrigonometric(intrinsicCall);
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
