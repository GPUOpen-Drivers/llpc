/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilderImplMisc.cpp
 * @brief LLPC source file: implementation of miscellaneous Builder methods
 ***********************************************************************************************************************
 */
#include "llpcBuilderImpl.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "llpc-builder-impl-misc"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// In the GS, emit the current values of outputs (as written by CreateWriteBuiltIn and CreateWriteOutput) to
// the current output primitive in the specified output-primitive stream number.
Instruction* BuilderImplMisc::CreateEmitVertex(
    uint32_t                streamId)           // Stream number, 0 if only one stream is present
{
    assert(m_shaderStage == ShaderStageGeometry);

    // Get GsWaveId
    std::string callName = lgcName::InputImportBuiltIn;
    callName += "GsWaveId.i32.i32";
    Value* pGsWaveId = EmitCall(callName,
                                getInt32Ty(),
                                getInt32(BuiltInWaveId),
                                {},
                                &*GetInsertPoint());

    // Do the sendmsg.
    // [9:8] = stream, [5:4] = 2 (emit), [3:0] = 2 (GS)
    uint32_t msg = (streamId << GS_EMIT_CUT_STREAM_ID_SHIFT) | GS_EMIT;
    return CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, { getInt32(msg), pGsWaveId }, nullptr);
}

// =====================================================================================================================
// In the GS, finish the current primitive and start a new one in the specified output-primitive stream.
Instruction* BuilderImplMisc::CreateEndPrimitive(
    uint32_t                streamId)           // Stream number, 0 if only one stream is present
{
    assert(m_shaderStage == ShaderStageGeometry);

    // Get GsWaveId
    std::string callName = lgcName::InputImportBuiltIn;
    callName += "GsWaveId.i32.i32";
    Value* pGsWaveId = EmitCall(callName,
                                getInt32Ty(),
                                getInt32(BuiltInWaveId),
                                {},
                                &*GetInsertPoint());

    // Do the sendmsg.
    // [9:8] = stream, [5:4] = 1 (cut), [3:0] = 2 (GS)
    uint32_t msg = (streamId << GS_EMIT_CUT_STREAM_ID_SHIFT) | GS_CUT;
    return CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, { getInt32(msg), pGsWaveId }, nullptr);
}

// =====================================================================================================================
// Create a workgroup control barrier.
Instruction* BuilderImplMisc::CreateBarrier()
{
    return CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
}

// =====================================================================================================================
// Create a "kill". Only allowed in a fragment shader.
Instruction* BuilderImplMisc::CreateKill(
    const Twine& instName) // [in] Name to give instruction(s)
{
    // This tells the config builder to set KILL_ENABLE in DB_SHADER_CONTROL.
    // Doing it here is suboptimal, as it does not allow for subsequent middle-end optimizations removing the
    // section of code containing the kill.
    auto pResUsage = GetPipelineState()->GetShaderResourceUsage(ShaderStageFragment);
    pResUsage->builtInUsage.fs.discard = true;

    return CreateIntrinsic(Intrinsic::amdgcn_kill, {}, getFalse(), nullptr, instName);
}

// =====================================================================================================================
// Create a demote to helper invocation operation. Only allowed in a fragment shader.
Instruction* BuilderImplMisc::CreateDemoteToHelperInvocation(
    const Twine& instName) // [in] Name to give instruction(s)
{
    // Treat a demote as a kill for the purposes of disabling middle-end optimizations.
    auto pResUsage = GetPipelineState()->GetShaderResourceUsage(ShaderStageFragment);
    pResUsage->builtInUsage.fs.discard = true;

    return CreateIntrinsic(Intrinsic::amdgcn_wqm_demote, {}, getFalse(), nullptr, instName);
}

// =====================================================================================================================
// Create a helper invocation query. Only allowed in a fragment shader.
Value* BuilderImplMisc::CreateIsHelperInvocation(
    const Twine& instName) // [in] Name to give instruction(s)
{
    auto pIsNotHelper = CreateIntrinsic(Intrinsic::amdgcn_wqm_helper, {}, {}, nullptr, instName);
    return CreateNot(pIsNotHelper);
}

// =====================================================================================================================
// Create a "readclock".
Instruction* BuilderImplMisc::CreateReadClock(
    bool         realtime,  // Whether to read real-time clock counter
    const Twine& instName)  // [in] Name to give instruction(s)
{
    CallInst* pReadClock = nullptr;
    if (realtime)
    {
        pReadClock = CreateIntrinsic(Intrinsic::amdgcn_s_memrealtime, {}, {}, nullptr, instName);
    }
    else
    {
        pReadClock = CreateIntrinsic(Intrinsic::amdgcn_s_memtime, {}, {}, nullptr, instName);
    }
    pReadClock->addAttribute(AttributeList::FunctionIndex, Attribute::ReadOnly);

    // NOTE: The inline ASM is to prevent optimization of backend compiler.
    InlineAsm* pAsmFunc =
        InlineAsm::get(FunctionType::get(getInt64Ty(), { getInt64Ty() }, false), "; %1", "=r,0", true);

    pReadClock = CreateCall(pAsmFunc, { pReadClock });

    return pReadClock;
}

// =====================================================================================================================
// Create derivative calculation on float or vector of float or half
Value* BuilderImplMisc::CreateDerivative(
    Value*        pValue,       // [in] Input value
    bool          isDirectionY, // False for derivative in X direction, true for Y direction
    bool          isFine,       // True for "fine" calculation, where the value in the current fragment is used.
                                // False for "coarse" calculation, where it might use fewer locations to calculate.
    const Twine&  instName)     // [in] Name to give instruction(s)
{
    uint32_t tableIdx = isDirectionY * 2 + isFine;
    Value* pResult = nullptr;
    if (SupportDpp())
    {
        // DPP (GFX8+) version.
        // For quad pixels, quad_perm:[pix0,pix1,pix2,pix3] = [0,1,2,3]
        // Table of first dpp_ctrl, in order coarseX, fineX, coarseY, fineY
        static const uint32_t firstDppCtrl[4] =
        {
            0x55, // CoarseX: [0,1,2,3] -> [1,1,1,1]
            0xF5, // FineX:   [0,1,2,3]->[1,1,3,3]
            0xAA, // CoarseY: [0,1,2,3] -> [2,2,2,2]
            0xEE, // FineY:   [0,1,2,3]->[2,3,2,3]
        };
        // Table of second dpp_ctrl, in order coarseX, fineX, coarseY, fineY
        static const uint32_t secondDppCtrl[4] =
        {
            0x00, // CoarseX: [0,1,2,3]->[0,0,0,0]
            0xA0, // FineX:   [0,1,2,3]->[0,0,2,2]
            0x00, // CoarseY: [0,1,2,3]->[0,0,0,0]
            0x44, // FineY:   [0,1,2,3]->[0,1,0,1]
        };
        uint32_t perm1 = firstDppCtrl[tableIdx];
        uint32_t perm2 = secondDppCtrl[tableIdx];
        pResult = Scalarize(pValue,
                            [this, perm1, perm2](Value* pValue)
                            {
                               Type* pValTy = pValue->getType();
                               pValue = CreateBitCast(pValue, getIntNTy(pValTy->getPrimitiveSizeInBits()));
                               pValue = CreateZExtOrTrunc(pValue, getInt32Ty());
                               Value* pFirstVal = CreateIntrinsic(Intrinsic::amdgcn_mov_dpp,
                                                                  getInt32Ty(),
                                                                  {
                                                                     pValue,
                                                                     getInt32(perm1),
                                                                     getInt32(15),
                                                                     getInt32(15),
                                                                     getTrue()
                                                                  });
                               pFirstVal = CreateZExtOrTrunc(pFirstVal, getIntNTy(pValTy->getPrimitiveSizeInBits()));
                               pFirstVal = CreateBitCast(pFirstVal, pValTy);
                               Value* pSecondVal = CreateIntrinsic(Intrinsic::amdgcn_mov_dpp,
                                                                   getInt32Ty(),
                                                                   {
                                                                      pValue,
                                                                      getInt32(perm2),
                                                                      getInt32(15),
                                                                      getInt32(15),
                                                                      getTrue()
                                                                   });
                               pSecondVal = CreateZExtOrTrunc(pSecondVal, getIntNTy(pValTy->getPrimitiveSizeInBits()));
                               pSecondVal = CreateBitCast(pSecondVal, pValTy);
                               Value* pResult = CreateFSub(pFirstVal, pSecondVal);
                               return CreateUnaryIntrinsic(Intrinsic::amdgcn_wqm, pResult);
                            });
    }
    else
    {
        // ds_swizzle (pre-GFX8) version

        // Table of first swizzle control, in order coarseX, fineX, coarseY, fineY
        static const uint32_t firstSwizzleCtrl[4] =
        {
            0x8055, // CoarseX: Broadcast channel 1 to whole quad
            0x80F5, // FineX: Swizzle channels in quad (1 -> 0, 1 -> 1, 3 -> 2, 3 -> 3)
            0x80AA, // CoarseY: Broadcast channel 2 to whole quad
            0x80EE, // FineY: Swizzle channels in quad (2 -> 0, 3 -> 1, 2 -> 2, 3 -> 3)
        };
        // Table of second swizzle control, in order coarseX, fineX, coarseY, fineY
        static const uint32_t secondSwizzleCtrl[4] =
        {
            0x8000, // CoarseX: Broadcast channel 0 to whole quad
            0x80A0, // FineX: Swizzle channels in quad (0 -> 0, 0 -> 1, 2 -> 2, 2 -> 3)
            0x8000, // CoarseY: Broadcast channel 0 to whole quad
            0x8044, // FineY: Swizzle channels in quad (0 -> 0, 1 -> 1, 0 -> 2, 1 -> 3)
        };
        uint32_t perm1 = firstSwizzleCtrl[tableIdx];
        uint32_t perm2 = secondSwizzleCtrl[tableIdx];
        pResult = Scalarize(pValue,
                            [this, perm1, perm2](Value* pValue)
                            {
                               Type* pValTy = pValue->getType();
                               pValue = CreateBitCast(pValue, getIntNTy(pValTy->getPrimitiveSizeInBits()));
                               pValue = CreateZExtOrTrunc(pValue, getInt32Ty());
                               Value* pFirstVal = CreateIntrinsic(Intrinsic::amdgcn_ds_swizzle,
                                                                  {},
                                                                  { pValue, getInt32(perm1)});
                               pFirstVal = CreateZExtOrTrunc(pFirstVal, getIntNTy(pValTy->getPrimitiveSizeInBits()));
                               pFirstVal = CreateBitCast(pFirstVal, pValTy);
                               Value* pSecondVal = CreateIntrinsic(Intrinsic::amdgcn_ds_swizzle,
                                                                   {},
                                                                   { pValue, getInt32(perm2) });
                               pSecondVal = CreateZExtOrTrunc(pSecondVal, getIntNTy(pValTy->getPrimitiveSizeInBits()));
                               pSecondVal = CreateBitCast(pSecondVal, pValTy);
                               Value* pResult = CreateFSub(pFirstVal, pSecondVal);
                               return CreateUnaryIntrinsic(Intrinsic::amdgcn_wqm, pResult);
                            });
    }
    pResult->setName(instName);
    return pResult;
}
