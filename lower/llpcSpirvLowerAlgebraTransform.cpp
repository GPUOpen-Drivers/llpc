/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerAlgebraTransform.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerAlgebraTransform.
 ***********************************************************************************************************************
 */
#include "hex_float.h"

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Analysis/ConstantFolding.h"

#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcSpirvLowerAlgebraTransform.h"

#define DEBUG_TYPE "llpc-spirv-lower-algebra-transform"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char SpirvLowerAlgebraTransform::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering opertions for algebraic transformation.
ModulePass* CreateSpirvLowerAlgebraTransform(bool enableConstFolding , bool enableFloatOpt)
{
    return new SpirvLowerAlgebraTransform(enableConstFolding, enableFloatOpt);
}

// =====================================================================================================================
SpirvLowerAlgebraTransform::SpirvLowerAlgebraTransform(
    bool enableConstFolding, // Whether enable constant folding
    bool enableFloatOpt)     // Whether enable floating point optimization
    :
    SpirvLower(ID),
    m_enableConstFolding(enableConstFolding),
    m_enableFloatOpt(enableFloatOpt),
    m_changed(false)
{
    initializeSpirvLowerAlgebraTransformPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerAlgebraTransform::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Algebra-Transform\n");

    SpirvLower::Init(&module);
    m_changed = false;

    auto fp16Control = m_pContext->GetShaderFloatControl(m_shaderStage, 16);
    auto fp32Control = m_pContext->GetShaderFloatControl(m_shaderStage, 32);
    auto fp64Control = m_pContext->GetShaderFloatControl(m_shaderStage, 64);

    if (m_enableConstFolding &&
        (fp16Control.denormFlushToZero || fp32Control.denormFlushToZero || fp64Control.denormFlushToZero))
    {
        // Do constant folding if we need flush denorm to zero.
        auto& targetLibInfo = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*m_pEntryPoint);
        auto& dataLayout = m_pModule->getDataLayout();

        for (auto& block : *m_pEntryPoint)
        {
            for (auto instIter = block.begin(), instEnd = block.end(); instIter != instEnd;)
            {
                Instruction* pInst = &(*instIter++);

                // DCE instruction if trivially dead.
                if (isInstructionTriviallyDead(pInst, &targetLibInfo))
                {
                    LLVM_DEBUG(dbgs() << "Algebriac transform: DCE: " << *pInst << '\n');
                    pInst->eraseFromParent();
                    m_changed = true;
                    continue;
                }

                // Skip Constant folding if it isn't floating point const expression
                auto pDestType = pInst->getType();
                if (pInst->use_empty() ||
                    (pInst->getNumOperands() == 0) ||
                    (pDestType->isFPOrFPVectorTy() == false) ||
                    (isa<Constant>(pInst->getOperand(0))== false))
                {
                    continue;
                }

                // ConstantProp instruction if trivially constant.
                if (Constant* pConst = ConstantFoldInstruction(pInst, dataLayout, &targetLibInfo))
                {
                    LLVM_DEBUG(dbgs() << "Algebriac transform: constant folding: " << *pConst << " from: " << *pInst
                        << '\n');
                    if ((pDestType->isHalfTy() && fp16Control.denormFlushToZero) ||
                        (pDestType->isFloatTy() && fp32Control.denormFlushToZero) ||
                        (pDestType->isDoubleTy() && fp64Control.denormFlushToZero))
                    {
                        // Replace denorm value with zero
                        if (pConst->isFiniteNonZeroFP() && (pConst->isNormalFP() == false))
                        {
                            pConst = ConstantFP::get(pDestType, 0.0);
                        }
                    }

                    pInst->replaceAllUsesWith(pConst);
                    if (isInstructionTriviallyDead(pInst, &targetLibInfo))
                    {
                        pInst->eraseFromParent();
                    }

                    m_changed = true;
                    continue;
                }
            }
        }
    }

    if (m_enableFloatOpt)
    {
        visit(m_pModule);
    }

    return m_changed;
}

// =====================================================================================================================
// Visits binary operator instruction.
void SpirvLowerAlgebraTransform::visitBinaryOperator(
    BinaryOperator& binaryOp)  // [in] Binary operator instruction
{
    Instruction::BinaryOps opCode = binaryOp.getOpcode();

    auto pSrc1 = binaryOp.getOperand(0);
    auto pSrc2 = binaryOp.getOperand(1);
    bool src1IsConstZero = isa<ConstantAggregateZero>(pSrc1) ||
                          (isa<ConstantFP>(pSrc1) && cast<ConstantFP>(pSrc1)->isZero());
    bool src2IsConstZero = isa<ConstantAggregateZero>(pSrc2) ||
                          (isa<ConstantFP>(pSrc2) && cast<ConstantFP>(pSrc2)->isZero());
    Value* pDest = nullptr;

    auto fp16Control = m_pContext->GetShaderFloatControl(m_shaderStage, 16);
    auto fp32Control = m_pContext->GetShaderFloatControl(m_shaderStage, 32);
    auto fp64Control = m_pContext->GetShaderFloatControl(m_shaderStage, 64);

    if (opCode == Instruction::FAdd)
    {
        // Recursively find backward if the operand "does not" specify contract flags
        auto fastMathFlags = binaryOp.getFastMathFlags();
        if (fastMathFlags.allowContract())
        {
            bool hasNoContract = IsOperandNoContract(pSrc1) || IsOperandNoContract(pSrc2);
            bool allowContract = !hasNoContract;

            // Reassocation and contract should be same
            fastMathFlags.setAllowReassoc(allowContract);
            fastMathFlags.setAllowContract(allowContract);
            binaryOp.copyFastMathFlags(fastMathFlags);
        }
    }
    else if (opCode == Instruction::FSub)
    {
        if (src1IsConstZero)
        {
            // NOTE: Source1 is constant zero, we might be performing FNEG operation. This will be optimized
            // by backend compiler with sign bit reversed via XOR. Check floating-point controls.
            auto pDestTy = binaryOp.getType();
            if ((pDestTy->getScalarType()->isHalfTy() && fp16Control.denormFlushToZero) ||
                (pDestTy->getScalarType()->isFloatTy() && fp32Control.denormFlushToZero) ||
                (pDestTy->getScalarType()->isDoubleTy() && fp64Control.denormFlushToZero))
            {
                // Has to flush denormals, insert canonicalize to make a MUL (* 1.0) forcibly
                std::string instName = "llvm.canonicalize." + GetTypeName(pDestTy);
                auto pCanonical = EmitCall(m_pModule,
                                           instName,
                                           pDestTy,
                                           { UndefValue::get(pDestTy) }, // Will be replaced later
                                           NoAttrib,
                                           binaryOp.getNextNode());

                binaryOp.replaceAllUsesWith(pCanonical);
                pCanonical->setArgOperand(0, &binaryOp);

                m_changed = true;
            }
        }
    }
    else if (opCode == Instruction::FRem)
    {
        auto pDestTy = binaryOp.getType();
        if (pDestTy->getScalarType()->isHalfTy())
        {
            // TODO: FREM for float16 type is not well handled by backend compiler. We lower it here:
            // frem(x, y) = x - y * trunc(x/y)

            // -trunc(x * 1/y)
            Value* pTrunc = EmitCall(m_pModule,
                                     "llvm.amdgcn.rcp." + GetTypeName(pDestTy),
                                     pDestTy,
                                     { pSrc2 },
                                     NoAttrib,
                                     &binaryOp);
            pTrunc = BinaryOperator::CreateFMul(pTrunc, pSrc1, "", &binaryOp);
            pTrunc = EmitCall(m_pModule,
                              "llvm.trunc." + GetTypeName(pDestTy),
                              pDestTy,
                              { pTrunc },
                              NoAttrib,
                              &binaryOp);
            pTrunc = BinaryOperator::CreateFNeg(pTrunc, "", &binaryOp);

            // -trunc(x/y) * y + x
            auto pFRem = EmitCall(m_pModule,
                                  "llvm.fmuladd." + GetTypeName(pDestTy),
                                  pDestTy,
                                  { pTrunc, pSrc2, pSrc1 },
                                  NoAttrib,
                                  &binaryOp);

            binaryOp.replaceAllUsesWith(pFRem);
            binaryOp.dropAllReferences();
            binaryOp.eraseFromParent();

            m_changed = true;
        }
    }

    // NOTE: We can't do constant folding for the following floating operations if we have floating-point controls that
    // will flush denormals or preserve NaN.
    if ((fp16Control.denormFlushToZero == false) && (fp16Control.signedZeroInfNanPreserve == false) &&
        (fp32Control.denormFlushToZero == false) && (fp32Control.signedZeroInfNanPreserve == false) &&
        (fp64Control.denormFlushToZero == false) && (fp64Control.signedZeroInfNanPreserve == false))
    {
        switch (opCode)
        {
        case Instruction::FAdd:
            {
                if (src1IsConstZero)
                {
                    pDest = pSrc2;
                }
                else if (src2IsConstZero)
                {
                    pDest = pSrc1;
                }

                break;
            }
        case Instruction::FMul:
            {
                if (src1IsConstZero)
                {
                    pDest = pSrc1;
                }
                else if (src2IsConstZero)
                {
                    pDest = pSrc2;
                }
                break;
            }
        case Instruction::FDiv:
            {
                if (src1IsConstZero && (src2IsConstZero == false))
                {
                    pDest = pSrc1;
                }
                break;
            }
        case Instruction::FSub:
            {
                if (src2IsConstZero)
                {
                    pDest = pSrc1;
                }
                break;
            }
        default:
            {
                break;
            }
        }

        if (pDest != nullptr)
        {
            binaryOp.replaceAllUsesWith(pDest);
            binaryOp.dropAllReferences();
            binaryOp.eraseFromParent();

            m_changed = true;
        }
    }

    // Replace FDIV x, y with FDIV 1.0, y; MUL x if it isn't optimized
    if ((opCode == Instruction::FDiv) && (pDest == nullptr) && (pSrc1 != nullptr) && (pSrc2 != nullptr))
    {
        Constant* pOne = ConstantFP::get(binaryOp.getType(), 1.0);
        if (pSrc1 != pOne)
        {
            IRBuilder<> builder(*m_pContext);
            builder.SetInsertPoint(&binaryOp);
            builder.setFastMathFlags(binaryOp.getFastMathFlags());
            Value* pRcp = builder.CreateFDiv(ConstantFP::get(binaryOp.getType(), 1.0), pSrc2);
            Value* pFDiv = builder.CreateFMul(pSrc1, pRcp);

            binaryOp.replaceAllUsesWith(pFDiv);
            binaryOp.dropAllReferences();
            binaryOp.eraseFromParent();

            m_changed = true;
        }
    }
}

// =====================================================================================================================
// Visits call instruction.
void SpirvLowerAlgebraTransform::visitCallInst(
    CallInst& callInst) // [in] Call instruction
{
    auto pCallee = callInst.getCalledFunction();

    bool forceFMul = false;

    if (pCallee->isIntrinsic() && (pCallee->getIntrinsicID() == Intrinsic::fabs))
    {
        // NOTE: FABS will be optimized by backend compiler with sign bit removed via AND.
        forceFMul = true;
    }
    else
    {
        // Disable fast math for gl_Position.
        // TODO: Having this here is not good, as it requires us to know implementation details of Builder.
        // We need to find a neater way to do it.
        auto calleeName = pCallee->getName();
        uint32_t builtIn = InvalidValue;
        Value* pValueWritten = nullptr;
        if (calleeName.startswith(LlpcName::OutputExportBuiltIn))
        {
            builtIn = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
            pValueWritten = callInst.getOperand(callInst.getNumArgOperands() - 1);
        }
        else if (calleeName.startswith("llpc.call.write.builtin"))
        {
            builtIn = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
            pValueWritten = callInst.getOperand(0);
        }
        if (builtIn == BuiltInPosition)
        {
            DisableFastMath(pValueWritten);
        }
    }

    // TODO: Check floating-point controls and insert a MUL to force denormal flush. This ought to
    // be done in backend compiler.
    if (forceFMul)
    {
        auto fp16Control = m_pContext->GetShaderFloatControl(m_shaderStage, 16);
        auto fp32Control = m_pContext->GetShaderFloatControl(m_shaderStage, 32);
        auto fp64Control = m_pContext->GetShaderFloatControl(m_shaderStage, 64);

        auto pDestTy = callInst.getType();
        if ((pDestTy->getScalarType()->isHalfTy() && fp16Control.denormFlushToZero) ||
            (pDestTy->getScalarType()->isFloatTy() && fp32Control.denormFlushToZero) ||
            (pDestTy->getScalarType()->isDoubleTy() && fp64Control.denormFlushToZero))
        {
            // Has to flush denormals, insert canonicalize to make a MUL (* 1.0) forcibly
            std::string instName = "llvm.canonicalize." + GetTypeName(pDestTy);
            auto pCanonical = EmitCall(m_pModule,
                                        instName,
                                        pDestTy,
                                        { UndefValue::get(pDestTy) }, // Will be replaced later
                                        NoAttrib,
                                        callInst.getNextNode());

                callInst.replaceAllUsesWith(pCanonical);
                pCanonical->setArgOperand(0, &callInst);

            m_changed = true;
        }
    }
}

// =====================================================================================================================
// Visits fptrunc instruction.
void SpirvLowerAlgebraTransform::visitFPTruncInst(
    FPTruncInst& fptruncInst)   // [in] Fptrunc instruction
{
    auto fp16Control = m_pContext->GetShaderFloatControl(m_shaderStage, 16);
    if (fp16Control.roundingModeRTZ)
    {
        auto pSrc = fptruncInst.getOperand(0);
        auto pSrcTy = pSrc->getType();
        auto pDestTy = fptruncInst.getDestTy();

        if ((pSrcTy->getScalarType()->isDoubleTy()) && (pDestTy->getScalarType()->isHalfTy()))
        {
            // NOTE: doubel -> float16 conversion is done in backend compiler with RTE rounding. Thus, we have to split
            // it with two phases to disable such lowering if we need RTZ rounding.
            auto pFloatTy =
                pSrcTy->isVectorTy() ? VectorType::get(m_pContext->FloatTy(), pSrcTy->getVectorNumElements()) :
                                       m_pContext->FloatTy();
            auto pFloatValue = new FPTruncInst(pSrc, pFloatTy, "", &fptruncInst);
            auto pDest = new FPTruncInst(pFloatValue, pDestTy, "", &fptruncInst);

            fptruncInst.replaceAllUsesWith(pDest);
            fptruncInst.dropAllReferences();
            fptruncInst.eraseFromParent();

            m_changed = true;
        }
    }
}

// =====================================================================================================================
// Recursively finds backward if the FPMathOperator operand does not specifiy "contract" flag.
bool SpirvLowerAlgebraTransform::IsOperandNoContract(
    Value *pOperand)  // [in] Operand to check
{
    if (isa<BinaryOperator>(pOperand))
    {
        auto pInst = dyn_cast<BinaryOperator>(pOperand);

        if (isa<FPMathOperator>(pOperand))
        {
            auto fastMathFlags = pInst->getFastMathFlags();
            bool allowContract = fastMathFlags.allowContract();
            if (fastMathFlags.any() && (allowContract == false))
            {
                return true;
            }
        }

        for (auto opIt = pInst->op_begin(), pEnd = pInst->op_end();
            opIt != pEnd; ++opIt)
        {
            return IsOperandNoContract(*opIt);
        }
    }
    return false;
}

// =====================================================================================================================
// Disable fast math for all values related with the specified value
void SpirvLowerAlgebraTransform::DisableFastMath(
    Value* pValue)   // [in] Value to disable fast math
{
    std::set<Instruction*> allValues;
    std::list<Instruction*> workSet;
    if (isa<Instruction>(pValue))
    {
        allValues.insert(cast<Instruction>(pValue));
        workSet.push_back(cast<Instruction>(pValue));
    }

    auto it = workSet.begin();
    while (workSet.empty() == false)
    {
        if (isa<FPMathOperator>(*it))
        {
            // Reset fast math flags to default
            auto pInst = cast<Instruction>(*it);
            llvm::FastMathFlags fastMathFlags;
            pInst->copyFastMathFlags(fastMathFlags);
        }

        for (Value* pOperand : (*it)->operands())
        {
            if (isa<Instruction>(pOperand))
            {
                // Add new values
                auto pInst = cast<Instruction>(pOperand);
                if (allValues.find(pInst) == allValues.end())
                {
                    allValues.insert(pInst);
                    workSet.push_back(pInst);
                }
            }
        }

        it = workSet.erase(it);
    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for algebraic transformation.
INITIALIZE_PASS(SpirvLowerAlgebraTransform, DEBUG_TYPE,
                "Lower SPIR-V algebraic transforms", false, false)
