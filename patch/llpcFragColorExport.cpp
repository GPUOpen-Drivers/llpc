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
 * @file  llpcFragColorExport.cpp
 * @brief LLPC source file: contains implementation of class Llpc::FragColorExport.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-frag-color-export"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "llpcDebug.h"
#include "llpcFragColorExport.h"
#include "llpcIntrinsDefs.h"
#include "llpcPipelineState.h"
#include "llpcTargetInfo.h"

using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
FragColorExport::FragColorExport(
    PipelineState*  pPipelineState, // [in] Pipeline state
    Module*         pModule)        // [in] LLVM module
    :
    m_pPipelineState(pPipelineState),
    m_pModule(pModule),
    m_pContext(pModule ? &pModule->getContext() : nullptr)
{
}

// =====================================================================================================================
// Executes fragment color export operations based on the specified output type and its location.
Value* FragColorExport::Run(
    Value*       pOutput,       // [in] Fragment color output
    uint32_t     location,      // Location of fragment color output
    Instruction* pInsertPos)    // [in] Where to insert fragment color export instructions
{
    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment);

    Type* pOutputTy = pOutput->getType();
    const uint32_t origLoc = pResUsage->inOutUsage.fs.outputOrigLocs[location];

    ExportFormat expFmt = EXP_FORMAT_ZERO;
    if (m_pPipelineState->GetColorExportState().dualSourceBlendEnable)
    {
        // Dual source blending is enabled
        expFmt= ComputeExportFormat(pOutputTy, 0);
    }
    else
    {
        expFmt = ComputeExportFormat(pOutputTy, origLoc);
    }

    pResUsage->inOutUsage.fs.expFmts[location] = expFmt;
    if (expFmt == EXP_FORMAT_ZERO)
    {
        // Clear channel mask if shader export format is ZERO
        pResUsage->inOutUsage.fs.cbShaderMask &= ~(0xF << (4 * origLoc));
    }

    const uint32_t bitWidth = pOutputTy->getScalarSizeInBits();
    BasicType outputType = pResUsage->inOutUsage.fs.outputTypes[origLoc];
    const bool signedness = ((outputType == BasicType::Int8) ||
                             (outputType == BasicType::Int16) ||
                             (outputType == BasicType::Int));

    auto pCompTy = pOutputTy->isVectorTy() ? pOutputTy->getVectorElementType() : pOutputTy;
    uint32_t compCount = pOutputTy->isVectorTy() ? pOutputTy->getVectorNumElements() : 1;

    Value* comps[4] = { nullptr };
    if (compCount == 1)
    {
        comps[0] = pOutput;
    }
    else
    {
        for (uint32_t i = 0; i < compCount; ++i)
        {
            comps[i] = ExtractElementInst::Create(pOutput,
                                                  ConstantInt::get(Type::getInt32Ty(*m_pContext), i),
                                                  "",
                                                  pInsertPos);
        }
    }

    bool comprExp = false;
    bool needPack = false;

    const auto pUndefFloat     = UndefValue::get(Type::getFloatTy(*m_pContext));
    const auto pUndefFloat16   = UndefValue::get(Type::getHalfTy(*m_pContext));
    const auto pUndefFloat16x2 = UndefValue::get(VectorType::get(Type::getHalfTy(*m_pContext), 2));

    switch (expFmt)
    {
    case EXP_FORMAT_ZERO:
        {
            break;
        }
    case EXP_FORMAT_32_R:
        {
            compCount = 1;
            comps[0] = ConvertToFloat(comps[0], signedness, pInsertPos);
            comps[1] = pUndefFloat;
            comps[2] = pUndefFloat;
            comps[3] = pUndefFloat;
            break;
        }
    case EXP_FORMAT_32_GR:
        {
            if (compCount >= 2)
            {
                compCount = 2;
                comps[0] = ConvertToFloat(comps[0], signedness, pInsertPos);
                comps[1] = ConvertToFloat(comps[1], signedness, pInsertPos);
                comps[2] = pUndefFloat;
                comps[3] = pUndefFloat;
            }
            else
            {
                compCount = 1;
                comps[0] = ConvertToFloat(comps[0], signedness, pInsertPos);
                comps[1] = pUndefFloat;
                comps[2] = pUndefFloat;
                comps[3] = pUndefFloat;
            }
            break;
        }
    case EXP_FORMAT_32_AR:
        {
            if (compCount == 4)
            {
                compCount = 2;
                comps[0] = ConvertToFloat(comps[0], signedness, pInsertPos);
                comps[1] = ConvertToFloat(comps[3], signedness, pInsertPos);
                comps[2] = pUndefFloat;
                comps[3] = pUndefFloat;
            }
            else
            {
                compCount = 1;
                comps[0] = ConvertToFloat(comps[0], signedness, pInsertPos);
                comps[1] = pUndefFloat;
                comps[2] = pUndefFloat;
                comps[3] = pUndefFloat;
            }
            break;
        }
    case EXP_FORMAT_32_ABGR:
       {
            for (uint32_t i = 0; i < compCount; ++i)
            {
                comps[i] = ConvertToFloat(comps[i], signedness, pInsertPos);
            }

            for (uint32_t i = compCount; i < 4; ++i)
            {
                comps[i] = pUndefFloat;
            }
            break;
        }
    case EXP_FORMAT_FP16_ABGR:
        {
            comprExp = true;

            if (bitWidth == 8)
            {
                needPack = true;

                // Cast i8 to float16
                LLPC_ASSERT(pCompTy->isIntegerTy());
                for (uint32_t i = 0; i < compCount; ++i)
                {
                    if (signedness)
                    {
                        // %comp = sext i8 %comp to i16
                        comps[i] = new SExtInst(comps[i], Type::getInt16Ty(*m_pContext), "", pInsertPos);
                    }
                    else
                    {
                        // %comp = zext i8 %comp to i16
                        comps[i] = new ZExtInst(comps[i], Type::getInt16Ty(*m_pContext), "", pInsertPos);
                    }

                    // %comp = bitcast i16 %comp to half
                    comps[i] = new BitCastInst(comps[i], Type::getHalfTy(*m_pContext), "", pInsertPos);
                }

                for (uint32_t i = compCount; i < 4; ++i)
                {
                    comps[i] = pUndefFloat16;
                }
            }
            else if (bitWidth == 16)
            {
                needPack = true;

                if (pCompTy->isIntegerTy())
                {
                    // Cast i16 to float16
                    for (uint32_t i = 0; i < compCount; ++i)
                    {
                        // %comp = bitcast i16 %comp to half
                        comps[i] = new BitCastInst(comps[i], Type::getHalfTy(*m_pContext), "", pInsertPos);
                    }
                }

                for (uint32_t i = compCount; i < 4; ++i)
                {
                    comps[i] = pUndefFloat16;
                }
            }
            else
            {
                if (pCompTy->isIntegerTy())
                {
                    // Cast i32 to float
                    for (uint32_t i = 0; i < compCount; ++i)
                    {
                        // %comp = bitcast i32 %comp to float
                        comps[i] = new BitCastInst(comps[i], Type::getFloatTy(*m_pContext), "", pInsertPos);
                    }
                }

                for (uint32_t i = compCount; i < 4; ++i)
                {
                    comps[i] = pUndefFloat;
                }

                Attribute::AttrKind attribs[] = {
                    Attribute::ReadNone
                };

                // Do packing
                comps[0] = EmitCall("llvm.amdgcn.cvt.pkrtz",
                                    VectorType::get(Type::getHalfTy(*m_pContext), 2),
                                    { comps[0], comps[1] },
                                    attribs,
                                    pInsertPos);

                if (compCount > 2)
                {
                    comps[1] = EmitCall("llvm.amdgcn.cvt.pkrtz",
                                        VectorType::get(Type::getHalfTy(*m_pContext), 2),
                                        { comps[2], comps[3] },
                                        attribs,
                                        pInsertPos);
                }
                else
                {
                    comps[1] = pUndefFloat16x2;
                }
            }

            break;
        }
    case EXP_FORMAT_UNORM16_ABGR:
    case EXP_FORMAT_SNORM16_ABGR:
        {
            comprExp = true;
            needPack = true;

            for (uint32_t i = 0; i < compCount; ++i)
            {
                // Convert the components to float value if necessary
                comps[i] = ConvertToFloat(comps[i], signedness, pInsertPos);
            }

            LLPC_ASSERT(compCount <= 4);
            // Make even number of components;
            if ((compCount % 2) != 0)
            {
                comps[compCount] = ConstantFP::get(Type::getFloatTy(*m_pContext), 0.0);
                compCount++;
            }

            StringRef funcName = (expFmt == EXP_FORMAT_SNORM16_ABGR) ?
                ("llvm.amdgcn.cvt.pknorm.i16") : ("llvm.amdgcn.cvt.pknorm.u16");

            for (uint32_t i = 0; i < compCount; i += 2)
            {
                Value* pComps = EmitCall(funcName,
                                         VectorType::get(Type::getInt16Ty(*m_pContext), 2),
                                         { comps[i], comps[i + 1] },
                                         NoAttrib,
                                         pInsertPos);

                pComps = new BitCastInst(pComps, VectorType::get(Type::getHalfTy(*m_pContext), 2), "", pInsertPos);

                comps[i] = ExtractElementInst::Create(pComps,
                                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                                        "",
                                                        pInsertPos);

                comps[i + 1] = ExtractElementInst::Create(pComps,
                                                            ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                            "",
                                                            pInsertPos);

            }

            for (uint32_t i = compCount; i < 4; ++i)
            {
                comps[i] = pUndefFloat16;
            }

            break;
        }
    case EXP_FORMAT_UINT16_ABGR:
    case EXP_FORMAT_SINT16_ABGR:
        {
            comprExp = true;
            needPack = true;

            for (uint32_t i = 0; i < compCount; ++i)
            {
                // Convert the components to int value if necessary
                comps[i] = ConvertToInt(comps[i], signedness, pInsertPos);
            }

            LLPC_ASSERT(compCount <= 4);
            // Make even number of components;
            if ((compCount % 2) != 0)
            {
                comps[compCount] = ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                compCount++;
            }

            StringRef funcName = (expFmt == EXP_FORMAT_SINT16_ABGR) ?
                ("llvm.amdgcn.cvt.pk.i16") : ("llvm.amdgcn.cvt.pk.u16");

            for (uint32_t i = 0; i < compCount; i += 2)
            {
                Value* pComps = EmitCall(funcName,
                                         VectorType::get(Type::getInt16Ty(*m_pContext), 2),
                                         { comps[i], comps[i + 1] },
                                         NoAttrib,
                                         pInsertPos);

                pComps = new BitCastInst(pComps, VectorType::get(Type::getHalfTy(*m_pContext), 2), "", pInsertPos);

                comps[i] = ExtractElementInst::Create(pComps,
                                                        ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                                        "",
                                                        pInsertPos);

                comps[i + 1] = ExtractElementInst::Create(pComps,
                                                            ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                            "",
                                                            pInsertPos);
            }

            for (uint32_t i = compCount; i < 4; ++i)
            {
                comps[i] = pUndefFloat16;
            }

            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }

    Value* pExport = nullptr;

    if (expFmt == EXP_FORMAT_ZERO)
    {
        // Do nothing
    }
    else if (comprExp)
    {
        // 16-bit export (compressed)
        if (needPack)
        {
            // Do packing

            // %comp[0] = insertelement <2 x half> undef, half %comp[0], i32 0
            comps[0] = InsertElementInst::Create(pUndefFloat16x2,
                                                 comps[0],
                                                 ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                                 "",
                                                 pInsertPos);

            // %comp[0] = insertelement <2 x half> %comp[0], half %comp[1], i32 1
            comps[0] = InsertElementInst::Create(comps[0],
                                                 comps[1],
                                                 ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                 "",
                                                 pInsertPos);

            if (compCount > 2)
            {
                // %comp[1] = insertelement <2 x half> undef, half %comp[2], i32 0
                comps[1] = InsertElementInst::Create(pUndefFloat16x2,
                                                     comps[2],
                                                     ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                                                     "",
                                                     pInsertPos);

                // %comp[1] = insertelement <2 x half> %comp[1], half %comp[3], i32 1
                comps[1] = InsertElementInst::Create(comps[1],
                                                     comps[3],
                                                     ConstantInt::get(Type::getInt32Ty(*m_pContext), 1),
                                                     "",
                                                     pInsertPos);
            }
            else
            {
                comps[1] = pUndefFloat16x2;
            }
        }

        Value* args[] = {
            ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_MRT_0 + location), // tgt
            ConstantInt::get(Type::getInt32Ty(*m_pContext), (compCount > 2) ? 0xF : 0x3), // en
            comps[0],                                                                     // src0
            comps[1],                                                                     // src1
            ConstantInt::get(Type::getInt1Ty(*m_pContext), false),                        // done
            ConstantInt::get(Type::getInt1Ty(*m_pContext), true)                          // vm
        };

        pExport = EmitCall("llvm.amdgcn.exp.compr.v2f16", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
    }
    else
    {
        // 32-bit export
        Value* args[] = {
            ConstantInt::get(Type::getInt32Ty(*m_pContext), EXP_TARGET_MRT_0 + location), // tgt
            ConstantInt::get(Type::getInt32Ty(*m_pContext), (1 << compCount) - 1),        // en
            comps[0],                                                                     // src0
            comps[1],                                                                     // src1
            comps[2],                                                                     // src2
            comps[3],                                                                     // src3
            ConstantInt::get(Type::getInt1Ty(*m_pContext), false),                        // done
            ConstantInt::get(Type::getInt1Ty(*m_pContext), true)                          // vm
        };

        pExport = EmitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_pContext), args, NoAttrib, pInsertPos);
    }

    return pExport;
}

// =====================================================================================================================
// Determines the shader export format for a particular fragment color output. Value should be used to do programming
// for SPI_SHADER_COL_FORMAT.
ExportFormat FragColorExport::ComputeExportFormat(
    Type*    pOutputTy,  // [in] Type of fragment data output
    uint32_t location    // Location of fragment data output
    ) const
{
    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();
    auto pGpuWorkarounds = &m_pPipelineState->GetTargetInfo().GetGpuWorkarounds();
    uint32_t outputMask = pOutputTy->isVectorTy() ? (1 << pOutputTy->getVectorNumElements()) - 1 : 1;
    const auto pCbState = &m_pPipelineState->GetColorExportState();
    const auto pTarget = &m_pPipelineState->GetColorExportFormat(location);
    // NOTE: Alpha-to-coverage only takes effect for outputs from color target 0.
    const bool enableAlphaToCoverage = (pCbState->alphaToCoverageEnable && (location == 0));

    const bool blendEnabled = pTarget->blendEnable;

    const bool isUnorm = (pTarget->nfmt == BufNumFormatUnorm);
    const bool isSnorm = (pTarget->nfmt == BufNumFormatSnorm);
    bool isFloat = (pTarget->nfmt == BufNumFormatFloat);
    const bool isUint = (pTarget->nfmt == BufNumFormatUint);
    const bool isSint = (pTarget->nfmt == BufNumFormatSint);
    const bool isSrgb = (pTarget->nfmt == BufNumFormatSrgb);

    if ((pTarget->dfmt == BufDataFormat8_8_8) || (pTarget->dfmt == BufDataFormat8_8_8_Bgr))
    {
        // These three-byte formats are handled by pretending they are float.
        isFloat = true;
    }

    const uint32_t maxCompBitCount = GetMaxComponentBitCount(pTarget->dfmt);

    const bool hasAlpha = HasAlpha(pTarget->dfmt);
    const bool alphaExport = ((outputMask == 0xF) &&
                              (hasAlpha || pTarget->blendSrcAlphaToColor || enableAlphaToCoverage));

    const CompSetting compSetting = ComputeCompSetting(pTarget->dfmt);

    // Start by assuming EXP_FORMAT_ZERO (no exports)
    ExportFormat expFmt = EXP_FORMAT_ZERO;

    bool gfx8RbPlusEnable = false;
    if ((gfxIp.major == 8) && (gfxIp.minor == 1))
    {
        gfx8RbPlusEnable = true;
    }

    if (pTarget->dfmt == BufDataFormatInvalid)
    {
        expFmt = EXP_FORMAT_ZERO;
    }
    else if ((compSetting == CompSetting::OneCompRed) &&
             (alphaExport == false)                   &&
             (isSrgb == false)                        &&
             ((gfx8RbPlusEnable == false) || (maxCompBitCount == 32)))
    {
        // NOTE: When Rb+ is enabled, "R8 UNORM" and "R16 UNORM" shouldn't use "EXP_FORMAT_32_R", instead
        // "EXP_FORMAT_FP16_ABGR" and "EXP_FORMAT_UNORM16_ABGR" should be used for 2X exporting performance.
        expFmt = EXP_FORMAT_32_R;
    }
    else if (((isUnorm || isSnorm) && (maxCompBitCount <= 10)) ||
             (isFloat && (maxCompBitCount <= 16)) ||
             (isSrgb && (maxCompBitCount == 8)))
    {
        expFmt = EXP_FORMAT_FP16_ABGR;
    }
    else if (isSint &&
             ((maxCompBitCount == 16) ||
              ((pGpuWorkarounds->gfx6.cbNoLt16BitIntClamp == false) && (maxCompBitCount < 16))) &&
             (enableAlphaToCoverage == false))
    {
        // NOTE: On some hardware, the CB will not properly clamp its input if the shader export format is "UINT16"
        // "SINT16" and the CB format is less than 16 bits per channel. On such hardware, the workaround is picking
        // an appropriate 32-bit export format. If this workaround isn't necessary, then we can choose this higher
        // performance 16-bit export format in this case.
        expFmt = EXP_FORMAT_SINT16_ABGR;
    }
    else if (isSnorm && (maxCompBitCount == 16) && (blendEnabled == false))
    {
        expFmt = EXP_FORMAT_SNORM16_ABGR;
    }
    else if (isUint &&
             ((maxCompBitCount == 16) ||
              ((pGpuWorkarounds->gfx6.cbNoLt16BitIntClamp == false) && (maxCompBitCount < 16))) &&
             (enableAlphaToCoverage == false))
    {
        // NOTE: On some hardware, the CB will not properly clamp its input if the shader export format is "UINT16"
        // "SINT16" and the CB format is less than 16 bits per channel. On such hardware, the workaround is picking
        // an appropriate 32-bit export format. If this workaround isn't necessary, then we can choose this higher
        // performance 16-bit export format in this case.
        expFmt = EXP_FORMAT_UINT16_ABGR;
    }
    else if (isUnorm && (maxCompBitCount == 16) && (blendEnabled == false))
    {
        expFmt = EXP_FORMAT_UNORM16_ABGR;
    }
    else if (((isUint || isSint) ||
              (isFloat && (maxCompBitCount > 16)) ||
              ((isUnorm || isSnorm) && (maxCompBitCount == 16)))  &&
             ((compSetting == CompSetting::OneCompRed) ||
              (compSetting == CompSetting::OneCompAlpha) ||
              (compSetting == CompSetting::TwoCompAlphaRed)))
    {
        expFmt = EXP_FORMAT_32_AR;
    }
    else if (((isUint || isSint) ||
              (isFloat && (maxCompBitCount > 16)) ||
              ((isUnorm || isSnorm) && (maxCompBitCount == 16)))  &&
             (compSetting == CompSetting::TwoCompGreenRed) && (alphaExport == false))
    {
        expFmt = EXP_FORMAT_32_GR;
    }
    else if (((isUnorm || isSnorm) && (maxCompBitCount == 16)) ||
             (isUint || isSint) ||
             (isFloat && (maxCompBitCount >  16)))
    {
        expFmt = EXP_FORMAT_32_ABGR;
    }

    return expFmt;
}

// =====================================================================================================================
// This is the helper function for the algorithm to determine the shader export format.
CompSetting FragColorExport::ComputeCompSetting(
    BufDataFormat dfmt) // Color attachment data format
{
    CompSetting compSetting = CompSetting::Invalid;
    switch (GetNumChannels(dfmt))
    {
    case 1:
        compSetting = CompSetting::OneCompRed;
        break;
    case 2:
        compSetting = CompSetting::TwoCompGreenRed;
        break;
    }
    return compSetting;
}

// =====================================================================================================================
// Get the number of channels
uint32_t FragColorExport::GetNumChannels(
    BufDataFormat dfmt) // Color attachment data format
{
    switch (dfmt)
    {
    case BufDataFormatInvalid:
    case BufDataFormatReserved:
    case BufDataFormat8:
    case BufDataFormat16:
    case BufDataFormat32:
    case BufDataFormat64:
        return 1;
    case BufDataFormat4_4:
    case BufDataFormat8_8:
    case BufDataFormat16_16:
    case BufDataFormat32_32:
    case BufDataFormat64_64:
        return 2;
    case BufDataFormat8_8_8:
    case BufDataFormat8_8_8_Bgr:
    case BufDataFormat10_11_11:
    case BufDataFormat11_11_10:
    case BufDataFormat32_32_32:
    case BufDataFormat64_64_64:
    case BufDataFormat5_6_5:
    case BufDataFormat5_6_5_Bgr:
        return 3;
    case BufDataFormat10_10_10_2:
    case BufDataFormat2_10_10_10:
    case BufDataFormat8_8_8_8:
    case BufDataFormat16_16_16_16:
    case BufDataFormat32_32_32_32:
    case BufDataFormat8_8_8_8_Bgra:
    case BufDataFormat2_10_10_10_Bgra:
    case BufDataFormat64_64_64_64:
    case BufDataFormat4_4_4_4:
    case BufDataFormat4_4_4_4_Bgra:
    case BufDataFormat5_6_5_1:
    case BufDataFormat5_6_5_1_Bgra:
    case BufDataFormat1_5_6_5:
    case BufDataFormat5_9_9_9:
        return 4;
    }
    return 0;
}

// =====================================================================================================================
// Checks whether the alpha channel is present in the specified color attachment format.
bool FragColorExport::HasAlpha(
    BufDataFormat dfmt) // Color attachment data format
{
    switch (dfmt)
    {
    case BufDataFormat10_10_10_2:
    case BufDataFormat2_10_10_10:
    case BufDataFormat8_8_8_8:
    case BufDataFormat16_16_16_16:
    case BufDataFormat32_32_32_32:
    case BufDataFormat8_8_8_8_Bgra:
    case BufDataFormat2_10_10_10_Bgra:
    case BufDataFormat64_64_64_64:
    case BufDataFormat4_4_4_4:
    case BufDataFormat4_4_4_4_Bgra:
    case BufDataFormat5_6_5_1:
    case BufDataFormat5_6_5_1_Bgra:
    case BufDataFormat1_5_6_5:
    case BufDataFormat5_9_9_9:
        return true;
    default:
        return false;
    }
}

// =====================================================================================================================
// Gets the maximum bit-count of any component in specified color attachment format.
uint32_t FragColorExport::GetMaxComponentBitCount(
    BufDataFormat dfmt) // Color attachment data format
{
    switch (dfmt)
    {
    case BufDataFormatInvalid:
    case BufDataFormatReserved:
        return 0;
    case BufDataFormat4_4:
    case BufDataFormat4_4_4_4:
    case BufDataFormat4_4_4_4_Bgra:
        return 4;
    case BufDataFormat5_6_5:
    case BufDataFormat5_6_5_Bgr:
    case BufDataFormat5_6_5_1:
    case BufDataFormat5_6_5_1_Bgra:
    case BufDataFormat1_5_6_5:
        return 6;
    case BufDataFormat8:
    case BufDataFormat8_8:
    case BufDataFormat8_8_8:
    case BufDataFormat8_8_8_Bgr:
    case BufDataFormat8_8_8_8:
    case BufDataFormat8_8_8_8_Bgra:
        return 8;
    case BufDataFormat5_9_9_9:
        return 9;
    case BufDataFormat10_10_10_2:
    case BufDataFormat2_10_10_10:
    case BufDataFormat2_10_10_10_Bgra:
        return 10;
    case BufDataFormat10_11_11:
    case BufDataFormat11_11_10:
        return 11;
    case BufDataFormat16:
    case BufDataFormat16_16:
    case BufDataFormat16_16_16_16:
        return 16;
    case BufDataFormat32:
    case BufDataFormat32_32:
    case BufDataFormat32_32_32:
    case BufDataFormat32_32_32_32:
        return 32;
    case BufDataFormat64:
    case BufDataFormat64_64:
    case BufDataFormat64_64_64:
    case BufDataFormat64_64_64_64:
        return 64;
    }
    return 0;
}

// =====================================================================================================================
// Converts an output component value to its floating-point representation. This function is a "helper" in computing
// the export value based on shader export format.
Value* FragColorExport::ConvertToFloat(
    Value*       pValue,        // [in] Output component value
    bool         signedness,    // Whether the type is signed (valid for integer type)
    Instruction* pInsertPos     // [in] Where to insert conversion instructions
    ) const
{
    Type* pValueTy = pValue->getType();
    LLPC_ASSERT(pValueTy->isFloatingPointTy() || pValueTy->isIntegerTy()); // Should be floating-point/integer scalar

    const uint32_t bitWidth = pValueTy->getScalarSizeInBits();
    if (bitWidth == 8)
    {
        LLPC_ASSERT(pValueTy->isIntegerTy());
        if (signedness)
        {
            // %value = sext i8 %value to i32
            pValue = new SExtInst(pValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
        }
        else
        {
            // %value = zext i8 %value to i32
            pValue = new ZExtInst(pValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
        }

        // %value = bitcast i32 %value to float
        pValue = new BitCastInst(pValue, Type::getFloatTy(*m_pContext), "", pInsertPos);
    }
    else if (bitWidth == 16)
    {
        if (pValueTy->isFloatingPointTy())
        {
            // %value = fpext half %value to float
            pValue = new FPExtInst(pValue, Type::getFloatTy(*m_pContext), "", pInsertPos);
        }
        else
        {
            if (signedness)
            {
                // %value = sext i16 %value to i32
                pValue = new SExtInst(pValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
            }
            else
            {
                // %value = zext i16 %value to i32
                pValue = new ZExtInst(pValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
            }

            // %value = bitcast i32 %value to float
            pValue = new BitCastInst(pValue, Type::getFloatTy(*m_pContext), "", pInsertPos);
        }
    }
    else
    {
        LLPC_ASSERT(bitWidth == 32); // The valid bit width is 16 or 32
        if (pValueTy->isIntegerTy())
        {
            // %value = bitcast i32 %value to float
            pValue = new BitCastInst(pValue, Type::getFloatTy(*m_pContext), "", pInsertPos);
        }
    }

    return pValue;
}

// =====================================================================================================================
// Converts an output component value to its integer representation. This function is a "helper" in computing the
// export value based on shader export format.
Value* FragColorExport::ConvertToInt(
    Value*       pValue,        // [in] Output component value
    bool         signedness,    // Whether the type is signed (valid for integer type)
    Instruction* pInsertPos     // [in] Where to insert conversion instructions
    ) const
{
    Type* pValueTy = pValue->getType();
    LLPC_ASSERT(pValueTy->isFloatingPointTy() || pValueTy->isIntegerTy()); // Should be floating-point/integer scalar

    const uint32_t bitWidth = pValueTy->getScalarSizeInBits();
    if (bitWidth == 8)
    {
        LLPC_ASSERT(pValueTy->isIntegerTy());

        if (signedness)
        {
            // %value = sext i8 %value to i32
            pValue = new SExtInst(pValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
        }
        else
        {
            // %value = zext i8 %value to i32
            pValue = new ZExtInst(pValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
        }
    }
    else if (bitWidth == 16)
    {
        if (pValueTy->isFloatingPointTy())
        {
            // %value = bicast half %value to i16
            pValue = new BitCastInst(pValue, Type::getInt16Ty(*m_pContext), "", pInsertPos);
        }

        if (signedness)
        {
            // %value = sext i16 %value to i32
            pValue = new SExtInst(pValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
        }
        else
        {
            // %value = zext i16 %value to i32
            pValue = new ZExtInst(pValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
        }
    }
    else
    {
        LLPC_ASSERT(bitWidth == 32); // The valid bit width is 16 or 32
        if (pValueTy->isFloatingPointTy())
        {
            // %value = bitcast float %value to i32
            pValue = new BitCastInst(pValue, Type::getInt32Ty(*m_pContext), "", pInsertPos);
        }
    }

    return pValue;
}

} // Llpc
