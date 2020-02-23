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
 * @file  llpcBuilderImplInOut.cpp
 * @brief LLPC source file: implementation of Builder methods for shader input and output
 ***********************************************************************************************************************
 */
#include "lgc/llpcBuilderContext.h"
#include "llpcBuilderImpl.h"
#include "llpcInternal.h"
#include "llpcPipelineState.h"

#define DEBUG_TYPE "llpc-builder-impl-inout"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Create a read of (part of) a generic (user) input value, passed from the previous shader stage.
// The result type is as specified by pResultTy, a scalar or vector type with no more than four elements.
// A "location" contains four "components", each of which can contain a 16- or 32-bit scalar value. A
// 64-bit scalar value takes two components.
Value* BuilderImplInOut::CreateReadGenericInput(
    Type*         pResultTy,          // [in] Type of value to read
    uint32_t      location,           // Base location (row) of input
    Value*        pLocationOffset,    // [in] Variable location offset; must be within locationCount
    Value*        pElemIdx,           // [in] Element index in vector. (This is the SPIR-V "component", except
                                      //      that it is half the component for 64-bit elements.)
    uint32_t      locationCount,      // Count of locations taken by the input
    InOutInfo     inputInfo,          // Extra input info (FS interp info)
    Value*        pVertexIndex,       // [in] For TCS/TES/GS per-vertex input: vertex index;
                                      //      for FS custom interpolated input: auxiliary interpolation value;
                                      //      else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    return ReadGenericInputOutput(false,
                                  pResultTy,
                                  location,
                                  pLocationOffset,
                                  pElemIdx,
                                  locationCount,
                                  inputInfo,
                                  pVertexIndex,
                                  instName);
}

// =====================================================================================================================
// Create a read of (part of) a generic (user) output value, returning the value last written in this shader stage.
// The result type is as specified by pResultTy, a scalar or vector type with no more than four elements.
// A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
// 64-bit components. Two locations together can contain up to a 4-vector of 64-bit components.
// This operation is only supported for TCS.
Value* BuilderImplInOut::CreateReadGenericOutput(
    Type*         pResultTy,          // [in] Type of value to read
    uint32_t      location,           // Base location (row) of output
    Value*        pLocationOffset,    // [in] Location offset; must be within locationCount if variable
    Value*        pElemIdx,           // [in] Element index in vector. (This is the SPIR-V "component", except
                                      //      that it is half the component for 64-bit elements.)
    uint32_t      locationCount,      // Count of locations taken by the output. Ignored if pLocationOffset is const
    InOutInfo     outputInfo,         // Extra output info
    Value*        pVertexIndex,       // [in] For TCS per-vertex output: vertex index; else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    return ReadGenericInputOutput(true,
                                  pResultTy,
                                  location,
                                  pLocationOffset,
                                  pElemIdx,
                                  locationCount,
                                  outputInfo,
                                  pVertexIndex,
                                  instName);
}

// =====================================================================================================================
// Read (a part of) a user input/output value.
Value* BuilderImplInOut::ReadGenericInputOutput(
    bool          isOutput,           // True if reading an output (currently only supported with TCS)
    Type*         pResultTy,          // [in] Type of value to read
    uint32_t      location,           // Base location (row) of input
    Value*        pLocationOffset,    // [in] Variable location offset; must be within locationCount
    Value*        pElemIdx,           // [in] Element index in vector. (This is the SPIR-V "component", except
                                      //      that it is half the component for 64-bit elements.)
    uint32_t      locationCount,      // Count of locations taken by the input
    InOutInfo     inOutInfo,          // Extra input/output info (FS interp info)
    Value*        pVertexIndex,       // [in] For TCS/TES/GS per-vertex input: vertex index;
                                      //      for FS custom interpolated input: auxiliary interpolation value;
                                      //      else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    assert(pResultTy->isAggregateType() == false);
    assert((isOutput == false) || (m_shaderStage == ShaderStageTessControl));

    // Fold constant pLocationOffset into location. (Currently a variable pLocationOffset is only supported in
    // TCS, TES, and FS custom interpolation.)
    if (auto pConstLocOffset = dyn_cast<ConstantInt>(pLocationOffset))
    {
        location += pConstLocOffset->getZExtValue();
        pLocationOffset = getInt32(0);
        locationCount = (pResultTy->getPrimitiveSizeInBits() + 127U) / 128U;
    }

    // Mark the usage of the input/output.
    MarkGenericInputOutputUsage(isOutput, location, locationCount, inOutInfo, pVertexIndex);

    // Generate LLPC call for reading the input/output.
    StringRef baseCallName = lgcName::InputImportGeneric;
    SmallVector<Value*, 6> args;
    switch (m_shaderStage)
    {
    case ShaderStageVertex:
        {
            // VS:  @llpc.input.import.generic.%Type%(i32 location, i32 elemIdx)
            assert(pLocationOffset == getInt32(0));
            args.push_back(getInt32(location));
            args.push_back(pElemIdx);
            break;
        }

    case ShaderStageTessControl:
    case ShaderStageTessEval:
        {
            // TCS: @llpc.{input|output}.import.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexIdx)
            // TES: @llpc.input.import.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexIdx)
            args.push_back(getInt32(location));
            args.push_back(pLocationOffset);
            args.push_back(pElemIdx);
            args.push_back((pVertexIndex != nullptr) ? pVertexIndex : getInt32(InvalidValue));
            if (isOutput)
            {
                baseCallName = lgcName::OutputImportGeneric;
            }
            break;
        }

    case ShaderStageGeometry:
        {
            // GS:  @llpc.input.import.generic.%Type%(i32 location, i32 elemIdx, i32 vertexIdx)
            assert(pLocationOffset == getInt32(0));
            args.push_back(getInt32(location));
            args.push_back(pElemIdx);
            args.push_back((pVertexIndex != nullptr) ? pVertexIndex : getInt32(InvalidValue));
            break;
        }

    case ShaderStageFragment:
        {
            // FS:  @llpc.input.import.generic.%Type%(i32 location, i32 elemIdx, i32 interpMode, i32 interpLoc)
            //      @llpc.input.import.interpolant.%Type%(i32 location, i32 locOffset, i32 elemIdx,
            //                                            i32 interpMode, <2 x float> | i32 auxInterpValue)
            if (inOutInfo.HasInterpAux())
            {
                // Prepare arguments for import interpolant call
                Value* pAuxInterpValue = ModifyAuxInterpValue(pVertexIndex, inOutInfo);
                baseCallName  = lgcName::InputImportInterpolant;
                args.push_back(getInt32(location));
                args.push_back(pLocationOffset);
                args.push_back(pElemIdx);
                args.push_back(getInt32(inOutInfo.GetInterpMode()));
                args.push_back(pAuxInterpValue);
            }
            else
            {
                assert(pLocationOffset == getInt32(0));
                args.push_back(getInt32(location));
                args.push_back(pElemIdx);
                args.push_back(getInt32(inOutInfo.GetInterpMode()));
                args.push_back(getInt32(inOutInfo.GetInterpLoc()));
            }
            break;
        }

    default:
        llvm_unreachable("Should never be called!");
        break;
    }

    std::string callName(baseCallName);
    AddTypeMangling(pResultTy, args, callName);
    Value* pResult = EmitCall(callName,
                              pResultTy,
                              args,
                              Attribute::ReadOnly,
                              &*GetInsertPoint());

    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create a write of (part of) a generic (user) output value, setting the value to pass to the next shader stage.
// The value to write must be a scalar or vector type with no more than four elements.
// A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
// 64-bit components. Two locations together can contain up to a 4-vector of 64-bit components.
// A non-constant pLocationOffset is currently only supported for TCS.
Instruction* BuilderImplInOut::CreateWriteGenericOutput(
    Value*        pValueToWrite,      // [in] Value to write
    uint32_t      location,           // Base location (row) of output
    Value*        pLocationOffset,    // [in] Location offset; must be within locationCount if variable
    Value*        pElemIdx,           // [in] Element index in vector. (This is the SPIR-V "component", except
                                      //      that it is half the component for 64-bit elements.)
    uint32_t      locationCount,      // Count of locations taken by the output. Ignored if pLocationOffset is const
    InOutInfo     outputInfo,         // Extra output info (GS stream ID, FS integer signedness)
    Value*        pVertexIndex)       // [in] For TCS per-vertex output: vertex index; else nullptr
{
    assert(pValueToWrite->getType()->isAggregateType() == false);

    // Fold constant pLocationOffset into location. (Currently a variable pLocationOffset is only supported in
    // TCS.)
    if (auto pConstLocOffset = dyn_cast<ConstantInt>(pLocationOffset))
    {
        location += pConstLocOffset->getZExtValue();
        pLocationOffset = getInt32(0);
        locationCount = (pValueToWrite->getType()->getPrimitiveSizeInBits() + 127U) / 128U;
    }

    // Mark the usage of the output.
    MarkGenericInputOutputUsage(/*isOutput=*/true, location, locationCount, outputInfo, pVertexIndex);

    // Set up the args for the llpc call.
    SmallVector<Value*, 6> args;
    switch (m_shaderStage)
    {
    case ShaderStageVertex:
    case ShaderStageTessEval:
        {
            // VS:  @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
            // TES: @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
            assert(pLocationOffset == getInt32(0));
            args.push_back(getInt32(location));
            args.push_back(pElemIdx);
            break;
        }

    case ShaderStageTessControl:
        {
            // TCS: @llpc.output.export.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexIdx,
            //                                         %Type% outputValue)
            args.push_back(getInt32(location));
            args.push_back(pLocationOffset);
            args.push_back(pElemIdx);
            args.push_back((pVertexIndex != nullptr) ? pVertexIndex : getInt32(InvalidValue));
            break;
        }

    case ShaderStageGeometry:
        {
            // GS:  @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, i32 streamId, %Type% outputValue)
            uint32_t streamId = outputInfo.HasStreamId() ? outputInfo.GetStreamId() : InvalidValue;
            assert(pLocationOffset == getInt32(0));
            args.push_back(getInt32(location));
            args.push_back(pElemIdx);
            args.push_back(getInt32(streamId));
            break;
        }

    case ShaderStageFragment:
        {
            // Mark fragment output type.
            MarkFsOutputType(pValueToWrite->getType(), location, outputInfo);

            // FS:  @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
            assert(pLocationOffset == getInt32(0));
            args.push_back(getInt32(location));
            args.push_back(pElemIdx);
            break;
        }

    default:
        llvm_unreachable("Should never be called!");
        break;
    }
    args.push_back(pValueToWrite);

    std::string llpcCallName = lgcName::OutputExportGeneric;
    AddTypeMangling(nullptr, args, llpcCallName);
    return EmitCall(llpcCallName,
                    getVoidTy(),
                    args,
                    {},
                    &*GetInsertPoint());
}

// =====================================================================================================================
// Mark usage for a generic (user) input or output
void BuilderImplInOut::MarkGenericInputOutputUsage(
    bool          isOutput,       // False for input, true for output
    uint32_t      location,       // Input/output base location
    uint32_t      locationCount,  // Count of locations taken by the input
    InOutInfo     inOutInfo,      // Extra input/output information
    Value*        pVertexIndex)  // [in] For TCS/TES/GS per-vertex input/output: vertex index;
                                  //      for FS custom-interpolated input: auxiliary value;
                                  //      else nullptr.
                                  //      (This is just used to tell whether an input/output is per-vertex.)
{
    auto pResUsage = GetPipelineState()->GetShaderResourceUsage(m_shaderStage);

    // Mark the input or output locations as in use.
    std::map<uint32_t, uint32_t>* pInOutLocMap = nullptr;
    if (isOutput == false)
    {
        if ((m_shaderStage != ShaderStageTessEval) || (pVertexIndex != nullptr))
        {
            // Normal input
            pInOutLocMap = &pResUsage->inOutUsage.inputLocMap;
        }
        else
        {
            // TES per-patch input
            pInOutLocMap = &pResUsage->inOutUsage.perPatchInputLocMap;
        }
    }
    else
    {
        if ((m_shaderStage != ShaderStageTessControl) || (pVertexIndex != nullptr))
        {
            // Normal output
            pInOutLocMap = &pResUsage->inOutUsage.outputLocMap;
        }
        else
        {
            // TCS per-patch output
            pInOutLocMap = &pResUsage->inOutUsage.perPatchOutputLocMap;
        }
    }

    if ((isOutput == false) || (m_shaderStage != ShaderStageGeometry))
    {
        bool keepAllLocations = false;
        if (GetBuilderContext()->BuildingRelocatableElf())
        {
            if (m_shaderStage == ShaderStageVertex && isOutput)
            {
                keepAllLocations = true;
            }
            if (m_shaderStage == ShaderStageFragment && !isOutput)
            {
                keepAllLocations = true;
            }
        }
        uint32_t startLocation = (keepAllLocations ? 0 : location);
        // Non-GS-output case.
        for (uint32_t i = startLocation; i < location + locationCount; ++i)
        {
            (*pInOutLocMap)[i] = InvalidValue;
        }
    }
    else
    {
        // GS output. We include the stream ID with the location in the map key.
        for (uint32_t i = 0; i < locationCount; ++i)
        {
            GsOutLocInfo outLocInfo = {};
            outLocInfo.location = location + i;
            outLocInfo.streamId = inOutInfo.GetStreamId();
            (*pInOutLocMap)[outLocInfo.u32All] = InvalidValue;
        }
    }

    if ((isOutput == false) && (m_shaderStage == ShaderStageFragment))
    {
        // Mark usage for interpolation info.
        MarkInterpolationInfo(inOutInfo);
    }
}

// =====================================================================================================================
// Mark interpolation info for FS input.
void BuilderImplInOut::MarkInterpolationInfo(
    InOutInfo     interpInfo)   // Interpolation info (location and mode)
{
    assert(m_shaderStage == ShaderStageFragment);

    auto pResUsage = GetPipelineState()->GetShaderResourceUsage(m_shaderStage);
    switch (interpInfo.GetInterpMode())
    {
    case InOutInfo::InterpModeCustom:
        return;
    case InOutInfo::InterpModeSmooth:
        pResUsage->builtInUsage.fs.smooth = true;
        break;
    case InOutInfo::InterpModeFlat:
        pResUsage->builtInUsage.fs.flat = true;
        break;
    case InOutInfo::InterpModeNoPersp:
        pResUsage->builtInUsage.fs.noperspective = true;
        break;
    default:
        llvm_unreachable("Should never be called!");
        break;
    }

    switch (interpInfo.GetInterpLoc())
    {
    case InOutInfo::InterpLocCenter:
        pResUsage->builtInUsage.fs.center = true;
        break;
    case InOutInfo::InterpLocCentroid:
        pResUsage->builtInUsage.fs.center = true;
        pResUsage->builtInUsage.fs.centroid = true;
        break;
    case InOutInfo::InterpLocSample:
        pResUsage->builtInUsage.fs.sample = true;
        pResUsage->builtInUsage.fs.runAtSampleRate = true;
        break;
    default:
        break;
    }
}

// =====================================================================================================================
// Mark fragment output type
void BuilderImplInOut::MarkFsOutputType(
    Type*     pOutputTy,      // [in] Output type
    uint32_t  location,       // Output location
    InOutInfo outputInfo)     // Extra output info (whether the output is signed)
{
    assert(m_shaderStage == ShaderStageFragment);

    // Collect basic types of fragment outputs
    BasicType basicTy = BasicType::Unknown;

    Type* pCompTy = pOutputTy->getScalarType();
    const uint32_t bitWidth = pCompTy->getScalarSizeInBits();
    const bool signedness = outputInfo.IsSigned();

    if (pCompTy->isIntegerTy())
    {
        // Integer type
        if (bitWidth == 8)
        {
            basicTy = signedness ? BasicType::Int8 : BasicType::Uint8;
        }
        else if (bitWidth == 16)
        {
            basicTy = signedness ? BasicType::Int16 : BasicType::Uint16;
        }
        else
        {
            assert(bitWidth == 32);
            basicTy = signedness ? BasicType::Int : BasicType::Uint;
        }
    }
    else if (pCompTy->isFloatingPointTy())
    {
        // Floating-point type
        if (bitWidth == 16)
        {
            basicTy = BasicType::Float16;
        }
        else
        {
            assert(bitWidth == 32);
            basicTy = BasicType::Float;
        }
    }
    else
    {
        llvm_unreachable("Should never be called!");
    }

    auto pResUsage = GetPipelineState()->GetShaderResourceUsage(m_shaderStage);
    pResUsage->inOutUsage.fs.outputTypes[location] = basicTy;
}

// =====================================================================================================================
// Modify auxiliary interp value according to custom interp mode
Value* BuilderImplInOut::ModifyAuxInterpValue(
    Value*    pAuxInterpValue,  // [in] Aux interp value from CreateReadInput (ignored for centroid location)
    InOutInfo inputInfo)        // InOutInfo containing interp mode and location
{
    if (inputInfo.GetInterpLoc() != InOutInfo::InterpLocExplicit)
    {
        // Add intrinsic to calculate I/J for interpolation function
        std::string evalInstName;
        auto pResUsage = GetPipelineState()->GetShaderResourceUsage(ShaderStageFragment);

        if (inputInfo.GetInterpLoc() == InOutInfo::InterpLocCentroid)
        {
            Value* evalArg = nullptr;

            evalInstName = lgcName::InputImportBuiltIn;
            if (inputInfo.GetInterpMode() == InOutInfo::InterpModeNoPersp)
            {
                evalInstName += "InterpLinearCentroid";
                evalArg = getInt32(BuiltInInterpLinearCentroid);
                pResUsage->builtInUsage.fs.noperspective = true;
                pResUsage->builtInUsage.fs.centroid = true;
            }
            else
            {
                evalInstName += "InterpPerspCentroid";
                evalArg = getInt32(BuiltInInterpPerspCentroid);
                pResUsage->builtInUsage.fs.smooth = true;
                pResUsage->builtInUsage.fs.centroid = true;
            }

            pAuxInterpValue = EmitCall(evalInstName,
                                       VectorType::get(getFloatTy(), 2),
                                       { evalArg },
                                       Attribute::ReadOnly,
                                       &*GetInsertPoint());
        }
        else
        {
            // Generate code to evaluate the I,J coordinates.
            if (inputInfo.GetInterpLoc() == InOutInfo::InterpLocSample)
            {
                pAuxInterpValue = ReadBuiltIn(false, BuiltInSamplePosOffset, {}, pAuxInterpValue, nullptr, "");
            }
            if (inputInfo.GetInterpMode() == InOutInfo::InterpModeNoPersp)
            {
                pAuxInterpValue = EvalIJOffsetNoPersp(pAuxInterpValue);
            }
            else
            {
                pAuxInterpValue = EvalIJOffsetSmooth(pAuxInterpValue);
            }
        }
    }
    else
    {
        assert(inputInfo.GetInterpMode() == InOutInfo::InterpModeCustom);
    }
    return pAuxInterpValue;
}

// =====================================================================================================================
// Evaluate I,J for interpolation: center offset, linear (no perspective) version
Value* BuilderImplInOut::EvalIJOffsetNoPersp(
    Value*  pOffset)    // [in] Offset value, <2 x float> or <2 x half>
{
    Value* pCenter = ReadBuiltIn(false, BuiltInInterpLinearCenter, {}, nullptr, nullptr, "");
    return AdjustIJ(pCenter, pOffset);
}

// =====================================================================================================================
// Evaluate I,J for interpolation: center offset, smooth (perspective) version
Value* BuilderImplInOut::EvalIJOffsetSmooth(
    Value*  pOffset)    // [in] Offset value, <2 x float> or <2 x half>
{
    // Get <I/W, J/W, 1/W>
    Value* pPullModel = ReadBuiltIn(false, BuiltInInterpPullMode, {}, nullptr, nullptr, "");
    // Adjust each coefficient by offset.
    Value* pAdjusted = AdjustIJ(pPullModel, pOffset);
    // Extract <I/W, J/W, 1/W> part of that
    Value* pIJDivW = CreateShuffleVector(pAdjusted, pAdjusted, { 0, 1 });
    Value* pRcpW = CreateExtractElement(pAdjusted, 2);
    // Get W by making a reciprocal of 1/W
    Value* pW = CreateFDiv(ConstantFP::get(getFloatTy(), 1.0), pRcpW);
    pW = CreateVectorSplat(2, pW);
    return CreateFMul(pIJDivW, pW);
}

// =====================================================================================================================
// Adjust I,J values by offset.
// This adjusts pValue by its X and Y derivatives times the X and Y components of pOffset.
// If pValue is a vector, this is done component-wise.
Value* BuilderImplInOut::AdjustIJ(
    Value*  pValue,     // [in] Value to adjust, float or vector of float
    Value*  pOffset)    // [in] Offset to adjust by, <2 x float> or <2 x half>
{
    pOffset = CreateFPExt(pOffset, VectorType::get(getFloatTy(), 2));
    Value* pOffsetX = CreateExtractElement(pOffset, uint64_t(0));
    Value* pOffsetY = CreateExtractElement(pOffset, 1);
    if (auto pVecTy = dyn_cast<VectorType>(pValue->getType()))
    {
        pOffsetX = CreateVectorSplat(pVecTy->getNumElements(), pOffsetX);
        pOffsetY = CreateVectorSplat(pVecTy->getNumElements(), pOffsetY);
    }
    Value* pDerivX = CreateDerivative(pValue, /*isY=*/false, /*isFine=*/true);
    Value* pDerivY = CreateDerivative(pValue, /*isY=*/true, /*isFine=*/true);
    Value* pAdjustX = CreateFAdd(pValue, CreateFMul(pDerivX, pOffsetX));
    Value* pAdjustY = CreateFAdd(pAdjustX, CreateFMul(pDerivY, pOffsetY));
    return pAdjustY;
}

// =====================================================================================================================
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
Instruction* BuilderImplInOut::CreateWriteXfbOutput(
    Value*        pValueToWrite,      // [in] Value to write
    bool          isBuiltIn,          // True for built-in, false for user output (ignored if not GS)
    uint32_t      location,           // Location (row) or built-in kind of output (ignored if not GS)
    uint32_t      xfbBuffer,          // XFB buffer ID
    uint32_t      xfbStride,          // XFB stride
    Value*        pXfbOffset,         // [in] XFB byte offset
    InOutInfo     outputInfo)         // Extra output info (GS stream ID)
{
    // Can currently only cope with constant pXfbOffset.
    assert(isa<ConstantInt>(pXfbOffset));

    // Ignore if not in last-vertex-stage shader (excluding copy shader).
    auto stagesAfterThisOneMask = -ShaderStageToMask(static_cast<ShaderStage>(m_shaderStage + 1));
    if ((GetPipelineState()->GetShaderStageMask() & ~ShaderStageToMask(ShaderStageFragment) &
          ~ShaderStageToMask(ShaderStageCopyShader) & stagesAfterThisOneMask) != 0)
    {
        return nullptr;
    }

    // Mark the usage of the XFB buffer.
    auto pResUsage = GetPipelineState()->GetShaderResourceUsage(m_shaderStage);
    uint32_t streamId = outputInfo.HasStreamId() ? outputInfo.GetStreamId() : 0;
    assert(xfbBuffer < MaxTransformFeedbackBuffers);
    assert(streamId < MaxGsStreams);
    pResUsage->inOutUsage.xfbStrides[xfbBuffer] = xfbStride;
    pResUsage->inOutUsage.enableXfb = true;
    pResUsage->inOutUsage.streamXfbBuffers[streamId] |= 1 << xfbBuffer;

    if (m_shaderStage == ShaderStageGeometry)
    {
        // Mark the XFB output for copy shader generation.
        GsOutLocInfo outLocInfo = {};
        outLocInfo.location = location;
        outLocInfo.isBuiltIn = isBuiltIn;
        outLocInfo.streamId = streamId;

        XfbOutInfo xfbOutInfo = {};
        xfbOutInfo.xfbBuffer = xfbBuffer;
        xfbOutInfo.xfbOffset = cast<ConstantInt>(pXfbOffset)->getZExtValue();
        xfbOutInfo.is16bit = (pValueToWrite->getType()->getScalarSizeInBits() == 16);
        xfbOutInfo.xfbExtraOffset = 0;

        auto pResUsage = GetPipelineState()->GetShaderResourceUsage(ShaderStageGeometry);
        pResUsage->inOutUsage.gs.xfbOutsInfo[outLocInfo.u32All] = xfbOutInfo.u32All;
        if (pValueToWrite->getType()->getPrimitiveSizeInBits() > 128)
        {
            ++outLocInfo.location;
            xfbOutInfo.xfbOffset += 32;
            pResUsage->inOutUsage.gs.xfbOutsInfo[outLocInfo.u32All] = xfbOutInfo.u32All;
        }
    }

    // XFB: @llpc.output.export.xfb.%Type%(i32 xfbBuffer, i32 xfbOffset, i32 xfbExtraOffset, %Type% outputValue)
    SmallVector<Value*, 4> args;
    std::string instName = lgcName::OutputExportXfb;
    args.push_back(getInt32(xfbBuffer));
    args.push_back(pXfbOffset);
    args.push_back(getInt32(0));
    args.push_back(pValueToWrite);
    AddTypeMangling(nullptr, args, instName);
    return EmitCall(instName,
                    getVoidTy(),
                    args,
                    {},
                    &*GetInsertPoint());
}

// =====================================================================================================================
// Create a read of (part of) a built-in input value.
// The type of the returned value is the fixed type of the specified built-in (see llpcBuilderBuiltInDefs.h),
// or the element type if pIndex is not nullptr. For ClipDistance or CullDistance when pIndex is nullptr,
// the array size is determined by inputInfo.GetArraySize().
Value* BuilderImplInOut::CreateReadBuiltInInput(
    BuiltInKind   builtIn,            // Built-in kind, one of the BuiltIn* constants
    InOutInfo     inputInfo,          // Extra input info (shader-defined array size)
    Value*        pVertexIndex,       // [in] For TCS/TES/GS per-vertex input: vertex index, else nullptr
    Value*        pIndex,             // [in] Array or vector index to access part of an input, else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    assert(IsBuiltInInput(builtIn));
    return ReadBuiltIn(false, builtIn, inputInfo, pVertexIndex, pIndex, instName);
}

// =====================================================================================================================
// Create a read of (part of) a built-in output value.
// The type of the returned value is the fixed type of the specified built-in (see llpcBuilderBuiltInDefs.h),
// or the element type if pIndex is not nullptr.
Value* BuilderImplInOut::CreateReadBuiltInOutput(
    BuiltInKind   builtIn,            // Built-in kind, one of the BuiltIn* constants
    InOutInfo     outputInfo,         // Extra output info (shader-defined array size)
    Value*        pVertexIndex,       // [in] For TCS/TES/GS per-vertex input: vertex index, else nullptr
    Value*        pIndex,             // [in] Array or vector index to access part of an input, else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    // Currently this only copes with reading an output in TCS.
    assert(m_shaderStage == ShaderStageTessControl);
    assert(IsBuiltInOutput(builtIn));
    return ReadBuiltIn(true, builtIn, outputInfo, pVertexIndex, pIndex, instName);
}

// =====================================================================================================================
// Read (part of) a built-in value
Value* BuilderImplInOut::ReadBuiltIn(
    bool          isOutput,           // True to read built-in output, false to read built-in input
    BuiltInKind   builtIn,            // Built-in kind, one of the BuiltIn* constants
    InOutInfo     inOutInfo,          // Extra input/output info (shader-defined array size)
    Value*        pVertexIndex,       // [in] For TCS/TES/GS per-vertex input: vertex index, else nullptr
                                      //    Special case for FS BuiltInSamplePosOffset: sample number. That special
                                      //    case only happens when ReadBuiltIn is called from ModifyAuxInterpValue.
    Value*        pIndex,             // [in] Array or vector index to access part of an input, else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    // Mark usage.
    uint32_t arraySize = inOutInfo.GetArraySize();
    if (auto pConstIndex = dyn_cast_or_null<ConstantInt>(pIndex))
    {
        arraySize = pConstIndex->getZExtValue() + 1;
    }

    if (isOutput == false)
    {
        MarkBuiltInInputUsage(builtIn, arraySize);
    }
    else
    {
        MarkBuiltInOutputUsage(builtIn, arraySize, InvalidValue);
    }

    // Get the built-in type.
    Type* pResultTy = GetBuiltInTy(builtIn, inOutInfo);
    if (pIndex != nullptr)
    {
        if (isa<ArrayType>(pResultTy))
        {
            pResultTy = pResultTy->getArrayElementType();
        }
        else
        {
            pResultTy = pResultTy->getVectorElementType();
        }
    }

    // Handle the subgroup mask built-ins directly.
    if ((builtIn == BuiltInSubgroupEqMask)            ||
        (builtIn == BuiltInSubgroupGeMask)            ||
        (builtIn == BuiltInSubgroupGtMask)            ||
        (builtIn == BuiltInSubgroupLeMask)            ||
        (builtIn == BuiltInSubgroupLtMask))
    {
        Value* pResult = nullptr;
        Value* pLocalInvocationId = ReadBuiltIn(false, BuiltInSubgroupLocalInvocationId, {}, nullptr, nullptr, "");
        if (GetPipelineState()->GetShaderWaveSize(m_shaderStage) == 64)
        {
            pLocalInvocationId = CreateZExt(pLocalInvocationId, getInt64Ty());
        }

        switch (builtIn)
        {
        case BuiltInSubgroupEqMask:
            pResult = CreateShl(ConstantInt::get(pLocalInvocationId->getType(), 1), pLocalInvocationId);
            break;
        case BuiltInSubgroupGeMask:
            pResult = CreateShl(ConstantInt::get(pLocalInvocationId->getType(), -1), pLocalInvocationId);
            break;
        case BuiltInSubgroupGtMask:
            pResult = CreateShl(ConstantInt::get(pLocalInvocationId->getType(), -2), pLocalInvocationId);
            break;
        case BuiltInSubgroupLeMask:
            pResult = CreateSub(CreateShl(ConstantInt::get(pLocalInvocationId->getType(), 2), pLocalInvocationId),
                                ConstantInt::get(pLocalInvocationId->getType(), 1));
            break;
        case BuiltInSubgroupLtMask:
            pResult = CreateSub(CreateShl(ConstantInt::get(pLocalInvocationId->getType(), 1), pLocalInvocationId),
                                ConstantInt::get(pLocalInvocationId->getType(), 1));
            break;
        default:
            llvm_unreachable("Should never be called!");
        }
        if (GetPipelineState()->GetShaderWaveSize(m_shaderStage) == 64)
        {
            pResult = CreateInsertElement(Constant::getNullValue(VectorType::get(getInt64Ty(), 2)),
                                          pResult,
                                          uint64_t(0));
            pResult = CreateBitCast(pResult, pResultTy);
        }
        else
        {
            pResult = CreateInsertElement(ConstantInt::getNullValue(pResultTy), pResult, uint64_t(0));
        }
        pResult->setName(instName);
        return pResult;
    }

    // For now, this just generates a call to llpc.input.import.builtin. A future commit will
    // change it to generate IR more directly here.
    // A vertex index is valid only in TCS, TES, GS.
    // Currently we can only cope with an array/vector index in TCS/TES.
    SmallVector<Value*, 4> args;
    args.push_back(getInt32(builtIn));
    switch (m_shaderStage)
    {
    case ShaderStageTessControl:
    case ShaderStageTessEval:
        args.push_back((pIndex != nullptr) ? pIndex : getInt32(InvalidValue));
        args.push_back((pVertexIndex != nullptr) ? pVertexIndex : getInt32(InvalidValue));
        break;
    case ShaderStageGeometry:
        assert(pIndex == nullptr);
        args.push_back((pVertexIndex != nullptr) ? pVertexIndex : getInt32(InvalidValue));
        break;
    case ShaderStageFragment:
        if (builtIn == BuiltInSamplePosOffset)
        {
            // Special case for BuiltInSamplePosOffset: pVertexIndex is the sample number.
            // That special case only happens when ReadBuiltIn is called from ModifyAuxInterpValue.
            Value* pSampleNum = pVertexIndex;
            pVertexIndex = nullptr;
            args.push_back(pSampleNum);
        }
        assert((pIndex == nullptr) && (pVertexIndex == nullptr));
        break;
    default:
        assert((pIndex == nullptr) && (pVertexIndex == nullptr));
        break;
    }

    std::string callName = isOutput ? lgcName::OutputImportBuiltIn : lgcName::InputImportBuiltIn;
    callName += GetBuiltInName(builtIn);
    AddTypeMangling(pResultTy, args, callName);
    Value* pResult = EmitCall(callName,
                              pResultTy,
                              args,
                              Attribute::ReadOnly,
                              &*GetInsertPoint());

    if (instName.isTriviallyEmpty())
    {
        pResult->setName(GetBuiltInName(builtIn));
    }
    else
    {
        pResult->setName(instName);
    }

    return pResult;
}

// =====================================================================================================================
// Create a write of (part of) a built-in output value.
// The type of the value to write must be the fixed type of the specified built-in (see llpcBuilderBuiltInDefs.h),
// or the element type if pIndex is not nullptr.
Instruction* BuilderImplInOut::CreateWriteBuiltInOutput(
    Value*        pValueToWrite,      // [in] Value to write
    BuiltInKind   builtIn,            // Built-in kind, one of the BuiltIn* constants
    InOutInfo     outputInfo,         // Extra output info (shader-defined array size; GS stream id)
    Value*        pVertexIndex,       // [in] For TCS per-vertex output: vertex index, else nullptr
    Value*        pIndex)             // [in] Array or vector index to access part of an input, else nullptr
{
    // Mark usage.
    uint32_t streamId = outputInfo.HasStreamId() ? outputInfo.GetStreamId() : InvalidValue;
    uint32_t arraySize = outputInfo.GetArraySize();
    if (auto pConstIndex = dyn_cast_or_null<ConstantInt>(pIndex))
    {
        arraySize = pConstIndex->getZExtValue() + 1;
    }
    MarkBuiltInOutputUsage(builtIn, arraySize, streamId);

#ifndef NDEBUG
    // Assert we have the right type. Allow for ClipDistance/CullDistance being a different array size.
    Type* pExpectedTy = GetBuiltInTy(builtIn, outputInfo);
    if (pIndex != nullptr)
    {
        if (isa<ArrayType>(pExpectedTy))
        {
            pExpectedTy = pExpectedTy->getArrayElementType();
        }
        else
        {
            pExpectedTy = pExpectedTy->getVectorElementType();
        }
    }
    assert((pExpectedTy == pValueToWrite->getType()) ||
                (((builtIn == BuiltInClipDistance) || (builtIn == BuiltInCullDistance)) &&
                 (pValueToWrite->getType()->getArrayElementType() == pExpectedTy->getArrayElementType())));
#endif // NDEBUG

    // For now, this just generates a call to llpc.output.export.builtin. A future commit will
    // change it to generate IR more directly here.
    // A vertex index is valid only in TCS.
    // Currently we can only cope with an array/vector index in TCS.
    //
    // VS:  @llpc.output.export.builtin.%BuiltIn%(i32 builtInId, %Type% outputValue)
    // TCS: @llpc.output.export.builtin.%BuiltIn%.%Type%(i32 builtInId, i32 elemIdx, i32 vertexIdx,
    //                                                   %Type% outputValue)
    // TES: @llpc.output.export.builtin.%BuiltIn%.%Type%(i32 builtInId, %Type% outputValue)
    // GS:  @llpc.output.export.builtin.%BuiltIn%(i32 builtInId, i32 streamId, %Type% outputValue)
    // FS:  @llpc.output.export.builtin.%BuiltIn%(i32 builtInId, %Type% outputValue)
    SmallVector<Value*, 4> args;
    args.push_back(getInt32(builtIn));
    switch (m_shaderStage)
    {
    case ShaderStageTessControl:
        args.push_back((pIndex != nullptr) ? pIndex : getInt32(InvalidValue));
        args.push_back((pVertexIndex != nullptr) ? pVertexIndex : getInt32(InvalidValue));
        break;
    case ShaderStageGeometry:
        assert((pIndex == nullptr) && (pVertexIndex == nullptr));
        args.push_back(getInt32(streamId));
        break;
    default:
        assert((pIndex == nullptr) && (pVertexIndex == nullptr));
        break;
    }
    args.push_back(pValueToWrite);

    std::string callName = lgcName::OutputExportBuiltIn;
    callName += GetBuiltInName(builtIn);
    AddTypeMangling(nullptr, args, callName);
    return cast<Instruction>(EmitCall(callName,
                             getVoidTy(),
                             args,
                             {},
                             &*GetInsertPoint()));
}

// =====================================================================================================================
// Get the type of a built-in. This overrides the one in Builder to additionally recognize the internal built-ins.
Type* BuilderImplInOut::GetBuiltInTy(
    BuiltInKind   builtIn,            // Built-in kind
    InOutInfo     inOutInfo)          // Extra input/output info (shader-defined array size)
{
    switch (static_cast<uint32_t>(builtIn))
    {
    case BuiltInSamplePosOffset:
    case BuiltInInterpLinearCenter:
        return VectorType::get(getFloatTy(), 2);
    case BuiltInInterpPullMode:
        return VectorType::get(getFloatTy(), 3);
    default:
        return Builder::GetBuiltInTy(builtIn, inOutInfo);
    }
}

// =====================================================================================================================
// Get name of built-in
StringRef BuilderImplInOut::GetBuiltInName(
    BuiltInKind   builtIn)            // Built-in type, one of the BuiltIn* constants
{
    switch (static_cast<uint32_t>(builtIn))
    {
#define BUILTIN(name, number, out, in, type) \
    case BuiltIn ## name: return # name;
#include "lgc/llpcBuilderBuiltInDefs.h"
#undef BUILTIN

    // Internal built-ins.
    case BuiltInSamplePosOffset:
        return "SamplePosOffset";
    case BuiltInInterpLinearCenter:
        return "InterpLinearCenter";
    case BuiltInInterpPullMode:
        return "InterpPullMode";

    default:
        llvm_unreachable("Should never be called!");
        return "unknown";
    }
}

// =====================================================================================================================
// Mark usage of a built-in input
void BuilderImplInOut::MarkBuiltInInputUsage(
    BuiltInKind builtIn,      // Built-in ID
    uint32_t    arraySize)    // Number of array elements for ClipDistance and CullDistance. (Multiple calls to
                              //    this function for this built-in might have different array sizes; we take the max)
{
    auto& pUsage = GetPipelineState()->GetShaderResourceUsage(m_shaderStage)->builtInUsage;
    assert(((builtIn != BuiltInClipDistance) && (builtIn != BuiltInCullDistance)) || (arraySize != 0));
    switch (m_shaderStage)
    {
    case ShaderStageVertex:
        {
            switch (builtIn)
            {
            case BuiltInVertexIndex:
                pUsage.vs.vertexIndex = true;
                pUsage.vs.baseVertex = true;
                break;
            case BuiltInInstanceIndex:
                pUsage.vs.instanceIndex = true;
                pUsage.vs.baseInstance = true;
                break;
            case BuiltInBaseVertex: pUsage.vs.baseVertex = true; break;
            case BuiltInBaseInstance: pUsage.vs.baseInstance = true; break;
            case BuiltInDrawIndex: pUsage.vs.drawIndex = true; break;
            case BuiltInPrimitiveId: pUsage.vs.primitiveId = true; break;
            case BuiltInViewIndex: pUsage.vs.viewIndex = true; break;
            default: break;
            }
            break;
        }

    case ShaderStageTessControl:
        {
            switch (builtIn)
            {
            case BuiltInPointSize: pUsage.tcs.pointSizeIn = true; break;
            case BuiltInPosition: pUsage.tcs.positionIn = true; break;
            case BuiltInClipDistance:
                pUsage.tcs.clipDistanceIn = std::max(pUsage.tcs.clipDistanceIn, arraySize);
                break;
            case BuiltInCullDistance:
                pUsage.tcs.cullDistanceIn = std::max(pUsage.tcs.cullDistanceIn, arraySize);
                break;
            case BuiltInPatchVertices: pUsage.tcs.patchVertices = true; break;
            case BuiltInPrimitiveId: pUsage.tcs.primitiveId = true; break;
            case BuiltInInvocationId: pUsage.tcs.invocationId = true; break;
            default: break;
            }
            break;
        }

    case ShaderStageTessEval:
        {
            switch (builtIn)
            {
            case BuiltInPointSize: pUsage.tes.pointSizeIn = true; break;
            case BuiltInPosition: pUsage.tes.positionIn = true; break;
            case BuiltInClipDistance:
                pUsage.tes.clipDistanceIn = std::max(pUsage.tes.clipDistanceIn, arraySize);
                break;
            case BuiltInCullDistance:
                pUsage.tes.cullDistanceIn = std::max(pUsage.tes.cullDistanceIn, arraySize);
                break;
            case BuiltInPatchVertices: pUsage.tes.patchVertices = true; break;
            case BuiltInPrimitiveId: pUsage.tes.primitiveId = true; break;
            case BuiltInTessCoord: pUsage.tes.tessCoord = true; break;
            case BuiltInTessLevelOuter: pUsage.tes.tessLevelOuter = true; break;
            case BuiltInTessLevelInner: pUsage.tes.tessLevelInner = true; break;
            case BuiltInViewIndex: pUsage.tes.viewIndex = true; break;
            default: break;
            }
            break;
        }

    case ShaderStageGeometry:
        {
            switch (builtIn)
            {
            case BuiltInPointSize: pUsage.gs.pointSizeIn = true; break;
            case BuiltInPosition: pUsage.gs.positionIn = true; break;
            case BuiltInClipDistance:
                pUsage.gs.clipDistanceIn = std::max(pUsage.gs.clipDistanceIn, arraySize);
                break;
            case BuiltInCullDistance:
                pUsage.gs.cullDistanceIn = std::max(pUsage.gs.cullDistanceIn, arraySize);
                break;
            case BuiltInPrimitiveId: pUsage.gs.primitiveIdIn = true; break;
            case BuiltInInvocationId: pUsage.gs.invocationId = true; break;
            case BuiltInViewIndex: pUsage.gs.viewIndex = true; break;
            default: break;
            }
            break;
        }

    case ShaderStageFragment:
        {
            switch (static_cast<uint32_t>(builtIn))
            {
            case BuiltInFragCoord: pUsage.fs.fragCoord = true; break;
            case BuiltInFrontFacing: pUsage.fs.frontFacing = true; break;
            case BuiltInClipDistance:
                pUsage.fs.clipDistance = std::max(pUsage.fs.clipDistance, arraySize);
                break;
            case BuiltInCullDistance:
                pUsage.fs.cullDistance = std::max(pUsage.fs.cullDistance, arraySize);
                break;
            case BuiltInPointCoord:
                pUsage.fs.pointCoord = true;
                // NOTE: gl_PointCoord is emulated via a general input. Those qualifiers therefore have to
                // be marked as used.
                pUsage.fs.smooth = true;
                if (GetPipelineState()->GetRasterizerState().perSampleShading)
                {
                    pUsage.fs.sample = true;
                }
                else
                {
                    pUsage.fs.center = true;
                }
                break;
            case BuiltInPrimitiveId: pUsage.fs.primitiveId = true; break;
            case BuiltInSampleId:
                pUsage.fs.sampleId = true;
                pUsage.fs.runAtSampleRate = true;
                break;
            case BuiltInSamplePosition:
                pUsage.fs.samplePosition = true;
                // NOTE: gl_SamplePostion is derived from gl_SampleID
                pUsage.fs.sampleId = true;
                pUsage.fs.runAtSampleRate = true;
                break;
            case BuiltInSampleMask: pUsage.fs.sampleMaskIn = true; break;
            case BuiltInLayer: pUsage.fs.layer = true; break;
            case BuiltInViewportIndex: pUsage.fs.viewportIndex = true; break;
            case BuiltInHelperInvocation: pUsage.fs.helperInvocation = true; break;
            case BuiltInViewIndex: pUsage.fs.viewIndex = true; break;
            case BuiltInBaryCoordNoPersp: pUsage.fs.baryCoordNoPersp = true; break;
            case BuiltInBaryCoordNoPerspCentroid: pUsage.fs.baryCoordNoPerspCentroid = true; break;
            case BuiltInBaryCoordNoPerspSample: pUsage.fs.baryCoordNoPerspSample = true; break;
            case BuiltInBaryCoordSmooth: pUsage.fs.baryCoordSmooth = true; break;
            case BuiltInBaryCoordSmoothCentroid: pUsage.fs.baryCoordSmoothCentroid = true; break;
            case BuiltInBaryCoordSmoothSample: pUsage.fs.baryCoordSmoothSample = true; break;
            case BuiltInBaryCoordPullModel: pUsage.fs.baryCoordPullModel = true; break;

            // Internal built-ins.
            case BuiltInInterpLinearCenter:
                pUsage.fs.noperspective = true;
                pUsage.fs.center = true;
                break;
            case BuiltInInterpPullMode:
                pUsage.fs.smooth = true;
                pUsage.fs.pullMode = true;
                break;
            case BuiltInSamplePosOffset: pUsage.fs.runAtSampleRate = true; break;

            default: break;
            }
            break;
        }

    case ShaderStageCompute:
        {
            switch (builtIn)
            {
                case BuiltInNumWorkgroups: pUsage.cs.numWorkgroups = true; break;
                case BuiltInLocalInvocationId: pUsage.cs.localInvocationId = true; break;
                case BuiltInWorkgroupId: pUsage.cs.workgroupId = true; break;
                case BuiltInNumSubgroups: pUsage.cs.numSubgroups = true; break;
                case BuiltInSubgroupId: pUsage.cs.subgroupId = true; break;
                default: break;
            }
            break;
        }

    default:
        break;
    }

    switch (builtIn)
    {
        case BuiltInSubgroupSize: pUsage.common.subgroupSize = true; break;
        case BuiltInSubgroupLocalInvocationId: pUsage.common.subgroupLocalInvocationId = true; break;
        case BuiltInSubgroupEqMask: pUsage.common.subgroupEqMask = true; break;
        case BuiltInSubgroupGeMask: pUsage.common.subgroupGeMask = true; break;
        case BuiltInSubgroupGtMask: pUsage.common.subgroupGtMask = true; break;
        case BuiltInSubgroupLeMask: pUsage.common.subgroupLeMask = true; break;
        case BuiltInSubgroupLtMask: pUsage.common.subgroupLtMask = true; break;
        case BuiltInDeviceIndex: pUsage.common.deviceIndex = true; break;
        default: break;
    }
}

// =====================================================================================================================
// Mark usage of a built-in output
void BuilderImplInOut::MarkBuiltInOutputUsage(
    BuiltInKind builtIn,      // Built-in ID
    uint32_t    arraySize,    // Number of array elements for ClipDistance and CullDistance. (Multiple calls to this
                              //    function for this built-in might have different array sizes; we take the max)
    uint32_t    streamId)     // GS stream ID, or InvalidValue if not known
{
    auto& pUsage = GetPipelineState()->GetShaderResourceUsage(m_shaderStage)->builtInUsage;
    assert(((builtIn != BuiltInClipDistance) && (builtIn != BuiltInCullDistance)) || (arraySize != 0));
    switch (m_shaderStage)
    {
    case ShaderStageVertex:
        {
            switch (builtIn)
            {
            case BuiltInPointSize: pUsage.vs.pointSize = true; break;
            case BuiltInPosition: pUsage.vs.position = true; break;
            case BuiltInClipDistance:
                pUsage.vs.clipDistance = std::max(pUsage.vs.clipDistance, arraySize);
                break;
            case BuiltInCullDistance:
                pUsage.vs.cullDistance = std::max(pUsage.vs.cullDistance, arraySize);
                break;
            case BuiltInViewportIndex: pUsage.vs.viewportIndex = true; break;
            case BuiltInLayer: pUsage.vs.layer = true; break;
            default: break;
            }
            break;
        }

    case ShaderStageTessControl:
        {
            switch (builtIn)
            {
            case BuiltInPointSize: pUsage.tcs.pointSize = true; break;
            case BuiltInPosition: pUsage.tcs.position = true; break;
            case BuiltInClipDistance:
                pUsage.tcs.clipDistance = std::max(pUsage.tcs.clipDistance, arraySize);
                break;
            case BuiltInCullDistance:
                pUsage.tcs.cullDistance = std::max(pUsage.tcs.cullDistance, arraySize);
                break;
            case BuiltInTessLevelOuter: pUsage.tcs.tessLevelOuter = true; break;
            case BuiltInTessLevelInner: pUsage.tcs.tessLevelInner = true; break;
            default: break;
            }
            break;
        }

    case ShaderStageTessEval:
        {
            switch (builtIn)
            {
            case BuiltInPointSize: pUsage.tes.pointSize = true; break;
            case BuiltInPosition: pUsage.tes.position = true; break;
            case BuiltInClipDistance:
                pUsage.tes.clipDistance = std::max(pUsage.tes.clipDistance, arraySize);
                break;
            case BuiltInCullDistance:
                pUsage.tes.cullDistance = std::max(pUsage.tes.cullDistance, arraySize);
                break;
            case BuiltInViewportIndex: pUsage.tes.viewportIndex = true; break;
            case BuiltInLayer: pUsage.tes.layer = true; break;
            default: break;
            }
            break;
        }

    case ShaderStageGeometry:
        {
            switch (builtIn)
            {
            case BuiltInPointSize: pUsage.gs.pointSize = true; break;
            case BuiltInPosition: pUsage.gs.position = true; break;
            case BuiltInClipDistance:
                pUsage.gs.clipDistance = std::max(pUsage.gs.clipDistance, arraySize);
                break;
            case BuiltInCullDistance:
                pUsage.gs.cullDistance = std::max(pUsage.gs.cullDistance, arraySize);
                break;
            case BuiltInPrimitiveId: pUsage.gs.primitiveId = true; break;
            case BuiltInViewportIndex: pUsage.gs.viewportIndex = true; break;
            case BuiltInLayer: pUsage.gs.layer = true; break;
            default: break;
            }
            // Collect raster stream ID for the export of built-ins
            if (streamId != InvalidValue)
            {
                GetPipelineState()->GetShaderResourceUsage(m_shaderStage)->inOutUsage.gs.rasterStream = streamId;
            }
            break;
        }

    case ShaderStageFragment:
        {
            switch (builtIn)
            {
            case BuiltInFragDepth: pUsage.fs.fragDepth = true; break;
            case BuiltInSampleMask: pUsage.fs.sampleMask = true; break;
            case BuiltInFragStencilRef: pUsage.fs.fragStencilRef = true; break;
            default: break;
            }
            break;
        }

    default:
        break;
    }
}

#ifndef NDEBUG
// =====================================================================================================================
// Get a bitmask of which shader stages are valid for a built-in to be an input or output of
uint32_t BuilderImplInOut::GetBuiltInValidMask(
    BuiltInKind builtIn,    // Built-in kind, one of the BuiltIn* constants
    bool        isOutput)   // True to get the mask for output rather than input
{
    // See llpcBuilderBuiltInDefs.h for an explanation of the letter codes.
    enum class StageValidMask: uint32_t
    {
        C = (1 << ShaderStageCompute),
        D = (1 << ShaderStageTessEval),
        H = (1 << ShaderStageTessControl),
        HD = (1 << ShaderStageTessControl) | (1 << ShaderStageTessEval),
        HDG = (1 << ShaderStageTessControl) | (1 << ShaderStageTessEval) | (1 << ShaderStageGeometry),
        HDGP = (1 << ShaderStageTessControl) | (1 << ShaderStageTessEval) |
               (1 << ShaderStageGeometry) | (1 << ShaderStageFragment),
        HG = (1 << ShaderStageTessControl) | (1 << ShaderStageGeometry),
        MG = (1 << ShaderStageGeometry),
        MVDG = (1 << ShaderStageVertex) | (1 << ShaderStageTessEval) | (1 << ShaderStageGeometry),
        MVHDG = (1 << ShaderStageVertex) | (1 << ShaderStageTessControl) |
                   (1 << ShaderStageTessEval) | (1 << ShaderStageGeometry),
        N = 0,
        P = (1 << ShaderStageFragment),
        TMC = (1 << ShaderStageCompute),
        TMV = (1 << ShaderStageVertex), TMVHDGPC = (1 << ShaderStageVertex) | (1 << ShaderStageTessControl) |
              (1 << ShaderStageTessEval) | (1 << ShaderStageGeometry) |
              (1 << ShaderStageFragment) | (1 << ShaderStageCompute),
        V = (1 << ShaderStageVertex),
        VDG = (1 << ShaderStageVertex) | (1 << ShaderStageTessEval) | (1 << ShaderStageGeometry),
    };

    uint32_t validMask = 0;
    switch (builtIn)
    {
#define BUILTIN(name, number, out, in, type) \
    case BuiltIn ## name: \
        validMask = static_cast<uint32_t>(StageValidMask::in) | (static_cast<uint32_t>(StageValidMask::out) << 16); \
        break;
#include "lgc/llpcBuilderBuiltInDefs.h"
#undef BUILTIN
    default:
        llvm_unreachable("Should never be called!");
        break;
    }
    return isOutput ? (validMask >> 16) : (validMask & 0xFFFF);
}

// =====================================================================================================================
// Determine whether a built-in is an input for a particular shader stage.
bool BuilderImplInOut::IsBuiltInInput(
    BuiltInKind   builtIn)    // Built-in type, one of the BuiltIn* constants
{
    return (GetBuiltInValidMask(builtIn, false) >> m_shaderStage) & 1;
}

// =====================================================================================================================
// Determine whether a built-in is an output for a particular shader stage.
bool BuilderImplInOut::IsBuiltInOutput(
    BuiltInKind   builtIn)    // Built-in type, one of the BuiltIn* constants
{
    return (GetBuiltInValidMask(builtIn, true) >> m_shaderStage) & 1;
}
#endif // NDEBUG

