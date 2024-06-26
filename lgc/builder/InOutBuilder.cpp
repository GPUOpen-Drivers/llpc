/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  InOutBuilder.cpp
 * @brief LLPC source file: implementation of Builder methods for shader input and output
 ***********************************************************************************************************************
 */
#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/builder/BuilderImpl.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#define DEBUG_TYPE "lgc-builder-impl-inout"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Create a read of (part of) a generic (user) input value, passed from the previous shader stage.
// The result type is as specified by resultTy, a scalar or vector type with no more than four elements.
// A "location" contains four "components", each of which can contain a 16- or 32-bit scalar value. A
// 64-bit scalar value takes two components.
//
// @param resultTy : Type of value to read
// @param location : Base location (row) of input
// @param locationOffset : Variable location offset; must be within locationCount
// @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
// 64-bit elements.)
// @param locationCount : Count of locations taken by the input
// @param inputInfo : Extra input info (FS interp info)
// @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index; for FS custom interpolated input: auxiliary
// interpolation value; else nullptr
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateReadGenericInput(Type *resultTy, unsigned location, Value *locationOffset, Value *elemIdx,
                                           unsigned locationCount, InOutInfo inputInfo, Value *vertexIndex,
                                           const Twine &instName) {
  return readGenericInputOutput(false, resultTy, location, locationOffset, elemIdx, locationCount, inputInfo,
                                vertexIndex, instName);
}

// =====================================================================================================================
// Create a read of (part of) a perVertex input value, passed from the previous shader stage.
// The result type is as specified by resultTy, a scalar or vector type with no more than four elements.
// A "location" contains four "components", each of which can contain a 16- or 32-bit scalar value. A
// 64-bit scalar value takes two components.
//
// @param resultTy : Type of value to read
// @param location : Base location (row) of input
// @param locationOffset : Variable location offset; must be within locationCount
// @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
//                  64-bit elements.)
// @param locationCount : Count of locations taken by the input
// @param inputInfo : Extra input info (FS interp info)
// @param vertexIndex : Vertex index, for each vertex, it is unique. the range is 0-2, up to three vertices per
//                      primitive; for FS custom interpolated input: auxiliary interpolation value;
// @param instName : Name to give instruction(s)
//
// @returns Value of input
Value *BuilderImpl::CreateReadPerVertexInput(Type *resultTy, unsigned location, Value *locationOffset, Value *elemIdx,
                                             unsigned locationCount, InOutInfo inputInfo, Value *vertexIndex,
                                             const Twine &instName) {
  assert(!resultTy->isAggregateType());
  assert(inputInfo.getInterpMode() == InOutInfo::InterpModeCustom);
  assert(m_shaderStage == ShaderStage::Fragment);

  // Fold constant locationOffset into location.
  bool canFold = foldConstantLocationOffset(resultTy, location, locationOffset, elemIdx, locationCount, inputInfo);
  assert(canFold);
  (void(canFold)); // Unused

  // Mark the usage of the input/output.
  markGenericInputOutputUsage(false, location, locationCount, inputInfo, vertexIndex != nullptr);

  // Lambda to do the actual input read.
  auto readInput = [&](Value *vertexIndex) {
    return create<InputImportInterpolatedOp>(resultTy, false, location, locationOffset, elemIdx,
                                             PoisonValue::get(getInt32Ty()), InOutInfo::InterpModeCustom, vertexIndex);
  };

  unsigned vertexIndexInt = cast<ConstantInt>(vertexIndex)->getZExtValue();
  Value *result = nullptr;

  if (m_pipelineState->isUnlinked() || m_pipelineState->getOptions().dynamicTopology) {
    auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment);
    resUsage->builtInUsage.fs.useDynamicToplogy = true;
    auto numVertices = ShaderInputs::getSpecialUserData(UserDataMapping::CompositeData, BuilderBase::get(*this));
    numVertices = CreateIntrinsic(Intrinsic::amdgcn_ubfe, getInt32Ty(), {numVertices, getInt32(0), getInt32(2)});
    auto isTriangle = CreateICmpEQ(numVertices, getInt32(3));
    Instruction *InsertI = &*GetInsertPoint();
    Instruction *thenInst = nullptr;
    Instruction *elseInst = nullptr;
    SplitBlockAndInsertIfThenElse(isTriangle, InsertI, &thenInst, &elseInst);

    BasicBlock *thenBB = thenInst->getParent();
    BasicBlock *elseBB = elseInst->getParent();
    BasicBlock *tailBB = InsertI->getParent();

    Value *triValue = nullptr;
    {
      SetInsertPoint(thenInst);
      Value *isOne = nullptr;
      Value *isTwo = nullptr;
      getProvokingVertexInfo(&isOne, &isTwo);

      auto V0 = readInput(getInt32((vertexIndexInt + 0) % 3));
      auto V1 = readInput(getInt32((vertexIndexInt + 1) % 3));
      auto V2 = readInput(getInt32((vertexIndexInt + 2) % 3));
      triValue = CreateSelect(isOne, V1, CreateSelect(isTwo, V2, V0));
    }

    Value *pointOrLineValue = nullptr;
    {
      SetInsertPoint(elseInst);
      pointOrLineValue = readInput(vertexIndex);
    }

    {
      SetInsertPoint(&*tailBB->getFirstInsertionPt());
      auto phiInst = CreatePHI(resultTy, 2);
      phiInst->addIncoming(triValue, thenBB);
      phiInst->addIncoming(pointOrLineValue, elseBB);
      return phiInst;
    }
  }

  auto vertexCount = m_pipelineState->getVerticesPerPrimitive();
  switch (vertexCount) {
  case 1:
  case 2:
    result = readInput(vertexIndex);
    break;
  case 3: { // Triangle
    Value *isOne = nullptr;
    Value *isTwo = nullptr;
    getProvokingVertexInfo(&isOne, &isTwo);

    auto V0 = readInput(getInt32((vertexIndexInt + 0) % 3));
    auto V1 = readInput(getInt32((vertexIndexInt + 1) % 3));
    auto V2 = readInput(getInt32((vertexIndexInt + 2) % 3));
    result = CreateSelect(isOne, V1, CreateSelect(isTwo, V2, V0));
    break;
  }
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create a read of (part of) a generic (user) output value, returning the value last written in this shader stage.
// The result type is as specified by resultTy, a scalar or vector type with no more than four elements.
// A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
// 64-bit components. Two locations together can contain up to a 4-vector of 64-bit components.
// This operation is only supported for TCS.
//
// @param resultTy : Type of value to read
// @param location : Base location (row) of output
// @param locationOffset : Location offset; must be within locationCount if variable
// @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
// 64-bit elements.)
// @param locationCount : Count of locations taken by the output. Ignored if locationOffset is const
// @param outputInfo : Extra output info
// @param vertexIndex : For TCS per-vertex output: vertex index; else nullptr
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateReadGenericOutput(Type *resultTy, unsigned location, Value *locationOffset, Value *elemIdx,
                                            unsigned locationCount, InOutInfo outputInfo, Value *vertexIndex,
                                            const Twine &instName) {
  return readGenericInputOutput(true, resultTy, location, locationOffset, elemIdx, locationCount, outputInfo,
                                vertexIndex, instName);
}

// =====================================================================================================================
// Read (a part of) a user input/output value.
//
// @param isOutput : True if reading an output (currently only supported with TCS)
// @param resultTy : Type of value to read
// @param location : Base location (row) of input
// @param locationOffset : Variable location offset; must be within locationCount
// @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
// 64-bit elements.)
// @param locationCount : Count of locations taken by the input
// @param inOutInfo : Extra input/output info (FS interp info)
// @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index; for FS custom interpolated input: auxiliary
// interpolation value; else nullptr
// @param instName : Name to give instruction(s)
Value *BuilderImpl::readGenericInputOutput(bool isOutput, Type *resultTy, unsigned location, Value *locationOffset,
                                           Value *elemIdx, unsigned locationCount, InOutInfo inOutInfo,
                                           Value *vertexIndex, const Twine &instName) {
  assert(resultTy->isAggregateType() == false);
  assert(isOutput == false || m_shaderStage == ShaderStage::TessControl);

  // Fold constant locationOffset into location.
  bool directlyMapLocations =
      !foldConstantLocationOffset(resultTy, location, locationOffset, elemIdx, locationCount, inOutInfo);

  // Mark the usage of the input/output.
  markGenericInputOutputUsage(isOutput, location, locationCount, inOutInfo, vertexIndex != nullptr,
                              directlyMapLocations);

  // Generate the call for reading the input/output.
  Value *result = nullptr;
  switch (m_shaderStage.value()) {
  case ShaderStage::Vertex: {
    assert(locationOffset == getInt32(0));
    result =
        create<InputImportGenericOp>(resultTy, false, location, getInt32(0), elemIdx, PoisonValue::get(getInt32Ty()));
    break;
  }

  case ShaderStage::TessControl:
  case ShaderStage::TessEval: {
    assert(!isOutput || m_shaderStage == ShaderStage::TessControl);
    bool isPerPrimitive = vertexIndex == nullptr;

    if (!vertexIndex)
      vertexIndex = PoisonValue::get(getInt32Ty());

    if (isOutput)
      result = create<OutputImportGenericOp>(resultTy, isPerPrimitive, location, locationOffset, elemIdx, vertexIndex);
    else
      result = create<InputImportGenericOp>(resultTy, isPerPrimitive, location, locationOffset, elemIdx, vertexIndex);
    break;
  }

  case ShaderStage::Geometry: {
    assert(cast<ConstantInt>(locationOffset)->isZero());
    assert(vertexIndex);
    result = create<InputImportGenericOp>(resultTy, false, location, locationOffset, elemIdx, vertexIndex);
    break;
  }

  case ShaderStage::Fragment: {
    if (inOutInfo.isPerPrimitive()) {
      assert(locationOffset == getInt32(0));
      result = create<InputImportGenericOp>(resultTy, true, location, locationOffset, elemIdx,
                                            PoisonValue::get(getInt32Ty()));
    } else {
      auto [interpMode, interpValue] = getInterpModeAndValue(inOutInfo, vertexIndex);
      result = create<InputImportInterpolatedOp>(resultTy, false, location, locationOffset, elemIdx,
                                                 PoisonValue::get(getInt32Ty()), interpMode, interpValue);
    }
    break;
  }

  default:
    llvm_unreachable("Should never be called!");
  }

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create a write of (part of) a generic (user) output value, setting the value to pass to the next shader stage.
// The value to write must be a scalar or vector type with no more than four elements.
// A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
// 64-bit components. Two locations together can contain up to a 4-vector of 64-bit components.
// A non-constant locationOffset is currently only supported for TCS.
//
// @param valueToWrite : Value to write
// @param location : Base location (row) of output
// @param locationOffset : Location offset; must be within locationCount if variable
// @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
// 64-bit elements.)
// @param locationCount : Count of locations taken by the output. Ignored if locationOffset is const
// @param outputInfo : Extra output info (GS stream ID, FS integer signedness)
// @param vertexOrPrimitiveIndex : For TCS/mesh shader per-vertex output: vertex index; for mesh shader per-primitive
//                                 output: primitive index; else nullptr
Instruction *BuilderImpl::CreateWriteGenericOutput(Value *valueToWrite, unsigned location, Value *locationOffset,
                                                   Value *elemIdx, unsigned locationCount, InOutInfo outputInfo,
                                                   Value *vertexOrPrimitiveIndex) {
  assert(valueToWrite->getType()->isAggregateType() == false);

  // Fold constant locationOffset into location.
  bool directlyMapLocations = !foldConstantLocationOffset(valueToWrite->getType(), location, locationOffset, elemIdx,
                                                          locationCount, outputInfo);

  // Mark the usage of the output.
  markGenericInputOutputUsage(/*isOutput=*/true, location, locationCount, outputInfo, vertexOrPrimitiveIndex != nullptr,
                              directlyMapLocations);

  // Set up the args for the call writing the output.
  SmallVector<Value *, 6> args;
  switch (m_shaderStage.value()) {
  case ShaderStage::Vertex:
  case ShaderStage::TessEval: {
    // VS: @lgc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
    // TES: @lgc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
    assert(locationOffset == getInt32(0));
    args.push_back(getInt32(location));
    args.push_back(elemIdx);
    break;
  }

  case ShaderStage::TessControl:
  case ShaderStage::Mesh: {
    // TCS: @lgc.output.export.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexIdx,
    //                                        %Type% outputValue)
    // Mesh: @lgc.output.export.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexOrPrimitiveIdx, i1
    //                                         perPrimitive, %Type% outputValue)
    args.push_back(getInt32(location));
    args.push_back(locationOffset);
    args.push_back(elemIdx);
    args.push_back(vertexOrPrimitiveIndex ? vertexOrPrimitiveIndex : getInt32(InvalidValue));
    if (m_shaderStage == ShaderStage::Mesh)
      args.push_back(getInt1(outputInfo.isPerPrimitive()));
    break;
  }

  case ShaderStage::Geometry: {
    // GS: @lgc.output.export.generic.%Type%(i32 location, i32 elemIdx, i32 streamId, %Type% outputValue)
    unsigned streamId = outputInfo.hasStreamId() ? outputInfo.getStreamId() : InvalidValue;
    assert(locationOffset == getInt32(0));
    args.push_back(getInt32(location));
    args.push_back(elemIdx);
    args.push_back(getInt32(streamId));
    break;
  }

  case ShaderStage::Fragment: {
    // Mark fragment output type.
    markFsOutputType(valueToWrite->getType(), location, outputInfo);

    // FS: @lgc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
    assert(locationOffset == getInt32(0));
    args.push_back(getInt32(location));
    args.push_back(elemIdx);
    break;
  }

  default:
    llvm_unreachable("Should never be called!");
    break;
  }
  args.push_back(valueToWrite);

  std::string llpcCallName = lgcName::OutputExportGeneric;
  addTypeMangling(nullptr, args, llpcCallName);
  return CreateNamedCall(llpcCallName, getVoidTy(), args, {});
}

// =====================================================================================================================
// Mark usage for a generic (user) input or output
//
// @param isOutput : False for input, true for output
// @param location : Input/output base location
// @param locationCount : Count of locations taken by the input/output
// @param inOutInfo : Extra input/output information
// @param hasVertexOrPrimIndex : Whether this input or output takes a vertex or primitive index. For TCS/TES/GS/mesh
//                               shader, this is the vertex index for per-vertex input/output; for mesh shader, this
//                               is the primitive index for per-primitive output; for FS custom-interpolated input,
//                               this is the auxiliary value.
// @param directlyMapLocations : Directly map locations to new ones with trivial map (keep location/component
//                               unchanged). This is for dynamic indexing for arrayed input/output when locations of
//                               their elements are dynamic indexed.
void BuilderImpl::markGenericInputOutputUsage(bool isOutput, unsigned location, unsigned locationCount,
                                              InOutInfo &inOutInfo, bool hasVertexOrPrimIndex,
                                              bool directlyMapLocations) {
  auto resUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage.value());

  // Mark the input or output locations as in use.
  std::map<InOutLocationInfo, InOutLocationInfo> *inOutLocInfoMap = nullptr;
  std::map<unsigned, unsigned> *perPatchInOutLocMap = nullptr;
  std::map<unsigned, unsigned> *perPrimitiveInOutLocMap = nullptr;
  if (!isOutput) {
    if (inOutInfo.isPerPrimitive()) {
      // Per-primitive input
      assert(m_shaderStage == ShaderStage::Fragment); // Must be FS
      perPrimitiveInOutLocMap = &resUsage->inOutUsage.perPrimitiveInputLocMap;
    } else if (m_shaderStage != ShaderStage::TessEval || hasVertexOrPrimIndex) {
      // Per-vertex input
      inOutLocInfoMap = &resUsage->inOutUsage.inputLocInfoMap;
    } else {
      // Per-patch input
      assert(m_shaderStage == ShaderStage::TessEval); // Must be TES
      perPatchInOutLocMap = &resUsage->inOutUsage.perPatchInputLocMap;
    }
  } else {
    if (inOutInfo.isPerPrimitive()) {
      // Per-primitive output
      assert(m_shaderStage == ShaderStage::Mesh); // Must be mesh shader
      perPrimitiveInOutLocMap = &resUsage->inOutUsage.perPrimitiveOutputLocMap;
    } else if (m_shaderStage != ShaderStage::TessControl || hasVertexOrPrimIndex) {
      // Per-vertex output
      inOutLocInfoMap = &resUsage->inOutUsage.outputLocInfoMap;
    } else {
      // Per-patch output
      assert(m_shaderStage == ShaderStage::TessControl); // Must be TCS
      perPatchInOutLocMap = &resUsage->inOutUsage.perPatchOutputLocMap;
    }
  }

  if (!(m_shaderStage == ShaderStage::Geometry && isOutput)) {
    // Not GS output
    bool keepAllLocations = false;
    if (getPipelineState()->isUnlinked()) {
      if (isOutput) {
        // Keep all locations if the next stage of the output is fragment shader or is unspecified
        if (m_shaderStage != ShaderStage::Fragment) {
          auto nextStage = m_pipelineState->getNextShaderStage(m_shaderStage.value());
          keepAllLocations = nextStage == ShaderStage::Fragment || !nextStage;
        }
      } else {
        // Keep all locations if it is the input of fragment shader
        keepAllLocations = m_shaderStage == ShaderStage::Fragment;
      }
    }

    if (inOutLocInfoMap) {
      // Handle per-vertex input/output
      if (keepAllLocations) {
        // If keeping all locations, add location map entries whose locations are before this input/output
        for (unsigned i = 0; i < location; ++i) {
          InOutLocationInfo origLocationInfo;
          origLocationInfo.setLocation(i);
          if (inOutLocInfoMap->count(origLocationInfo) == 0) {
            // Add this location map entry only if it doesn't exist
            auto &newLocationInfo = (*inOutLocInfoMap)[origLocationInfo];
            newLocationInfo.setData(InvalidValue);
          }
        }
      }

      // Add location map entries for this input/output

      // NOTE: For TCS input/output, TES input, and mesh shader output, their components could be separately indexed.
      // We have to reserve all components in the location map and mark all of them as active. Otherwise, this might
      // lead to failed searching when we try to find the location map info for this input/output.
      bool reserveAllComponents = m_shaderStage == ShaderStage::TessControl ||
                                  (m_shaderStage == ShaderStage::TessEval && !isOutput) ||
                                  (m_shaderStage == ShaderStage::Mesh && isOutput);

      for (unsigned i = 0; i < locationCount; ++i) {
        if (reserveAllComponents) {
          unsigned numComponents = 0;
          if (inOutInfo.getNumComponents() > 4) {
            assert(locationCount % 2 == 0);        // Must have even number of locations for 64-bit data type
            assert(inOutInfo.getComponent() == 0); // Start component must be 0 in this case
            if (i % 2 == 0)
              numComponents = 4;
            else
              numComponents = inOutInfo.getNumComponents() - 4;
          } else {
            numComponents = inOutInfo.getComponent() + inOutInfo.getNumComponents();
          }
          assert(numComponents >= 1 && numComponents <= 4); // Valid number of components for a location is 1~4

          for (unsigned j = 0; j < numComponents; ++j) {
            InOutLocationInfo origLocationInfo;
            origLocationInfo.setLocation(location + i);
            origLocationInfo.setComponent(j);

            auto &newLocationInfo = (*inOutLocInfoMap)[origLocationInfo];
            if (directlyMapLocations) {
              // Force to map the location trivially
              newLocationInfo.setLocation(location + i);
              newLocationInfo.setComponent(j);
            } else
              newLocationInfo.setData(InvalidValue);
          }
        } else {
          InOutLocationInfo origLocationInfo;
          origLocationInfo.setLocation(location + i);
          origLocationInfo.setComponent(inOutInfo.getComponent());
          auto &newLocationInfo = (*inOutLocInfoMap)[origLocationInfo];
          if (directlyMapLocations) {
            // Directly map the locations (trivial map) without further calculation
            newLocationInfo.setLocation(location + i);
            newLocationInfo.setComponent(inOutInfo.getComponent());
          } else
            newLocationInfo.setData(InvalidValue);
        }
      }
    }

    if (perPatchInOutLocMap) {
      // Handle per-patch input/output
      if (keepAllLocations) {
        // If keeping all locations, add location map entries whose locations are before this input/output
        for (unsigned i = 0; i < location; ++i) {
          // Add this location map entry only if it doesn't exist
          if (perPatchInOutLocMap->count(i) == 0)
            (*perPatchInOutLocMap)[i] = InvalidValue;
        }
      }

      // Add location map entries for this input/output
      for (unsigned i = 0; i < locationCount; ++i)
        (*perPatchInOutLocMap)[location + i] =
            directlyMapLocations ? location + i
                                 : InvalidValue; // Directly map the locations (trivial map) without further calculation
    }

    if (perPrimitiveInOutLocMap) {
      // Handle per-primitive input/output
      if (keepAllLocations) {
        // If keeping all locations, add location map entries whose locations are before this input/output
        for (unsigned i = 0; i < location; ++i) {
          // Add this location map entry only if it doesn't exist
          if (perPrimitiveInOutLocMap->count(i) == 0)
            (*perPrimitiveInOutLocMap)[i] = InvalidValue;
        }
      }

      // Add location map entries for this input/output
      for (unsigned i = 0; i < locationCount; ++i)
        (*perPrimitiveInOutLocMap)[location + i] =
            directlyMapLocations ? location + i
                                 : InvalidValue; // Directly map the locations (trivial map) without further calculation
    }
  } else {
    // GS output
    for (unsigned i = 0; i < locationCount; ++i) {
      InOutLocationInfo outLocationInfo;
      outLocationInfo.setLocation(location + i);
      outLocationInfo.setComponent(inOutInfo.getComponent());
      outLocationInfo.setStreamId(inOutInfo.getStreamId()); // Include the stream ID in the map key.
      auto &newLocationInfo = (*inOutLocInfoMap)[outLocationInfo];
      newLocationInfo.setData(InvalidValue);
    }
  }

  if (!isOutput && m_shaderStage == ShaderStage::Fragment) {
    // Mark usage for interpolation info.
    markInterpolationInfo(inOutInfo);
  }
}

// =====================================================================================================================
// Mark interpolation info for FS input.
//
// @param interpInfo : Interpolation info (location and mode)
void BuilderImpl::markInterpolationInfo(InOutInfo &interpInfo) {
  assert(m_shaderStage == ShaderStage::Fragment);

  auto resUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage.value());
  switch (interpInfo.getInterpMode()) {
  case InOutInfo::InterpModeCustom:
    return;
  case InOutInfo::InterpModeSmooth:
    resUsage->builtInUsage.fs.smooth = true;
    break;
  case InOutInfo::InterpModeFlat:
    resUsage->builtInUsage.fs.flat = true;
    break;
  case InOutInfo::InterpModeNoPersp:
    resUsage->builtInUsage.fs.noperspective = true;
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  // When per-sample shading is enabled, force nonperspective and smooth input with center-based interpolation to do
  // per-sample interpolation.
  // NOTE: if the input is used by interpolation functions (has auxiliary value), we should not modify its interpLoc
  // because it is used for modifyAuxInterpValue.
  if (getPipelineState()->getOptions().enableInterpModePatch &&
      interpInfo.getInterpLoc() == InOutInfo::InterpLocCenter && !interpInfo.hasInterpAux() &&
      (resUsage->builtInUsage.fs.smooth || resUsage->builtInUsage.fs.noperspective))
    interpInfo.setInterpLoc(InOutInfo::InterpLocSample);

  switch (interpInfo.getInterpLoc()) {
  case InOutInfo::InterpLocCenter:
    resUsage->builtInUsage.fs.center = true;
    break;
  case InOutInfo::InterpLocCentroid:
    resUsage->builtInUsage.fs.center = true;
    resUsage->builtInUsage.fs.centroid = true;
    break;
  case InOutInfo::InterpLocSample:
    resUsage->builtInUsage.fs.sample = true;
    resUsage->builtInUsage.fs.runAtSampleRate = true;
    break;
  default:
    break;
  }
}

// =====================================================================================================================
// Mark fragment output type
//
// @param outputTy : Output type
// @param location : Output location
// @param outputInfo : Extra output info (whether the output is signed)
void BuilderImpl::markFsOutputType(Type *outputTy, unsigned location, InOutInfo outputInfo) {
  assert(m_shaderStage == ShaderStage::Fragment);

  // Collect basic types of fragment outputs
  BasicType basicTy = BasicType::Unknown;

  Type *compTy = outputTy->getScalarType();
  const unsigned bitWidth = compTy->getScalarSizeInBits();
  const bool signedness = outputInfo.isSigned();

  if (compTy->isIntegerTy()) {
    // Integer type
    if (bitWidth == 8)
      basicTy = signedness ? BasicType::Int8 : BasicType::Uint8;
    else if (bitWidth == 16)
      basicTy = signedness ? BasicType::Int16 : BasicType::Uint16;
    else {
      assert(bitWidth == 32);
      basicTy = signedness ? BasicType::Int : BasicType::Uint;
    }
  } else if (compTy->isFloatingPointTy()) {
    // Floating-point type
    if (bitWidth == 16)
      basicTy = BasicType::Float16;
    else {
      assert(bitWidth == 32);
      basicTy = BasicType::Float;
    }
  } else
    llvm_unreachable("Should never be called!");

  auto resUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage.value());
  resUsage->inOutUsage.fs.outputTypes[location] = basicTy;
}

// =====================================================================================================================
// Try to fold constant location offset if possible. This function also updates the field 'numComponents' of 'inOutInfo'
// if it is not specified by computing an appropriate value.
//
// @param inOutTy : Type of this input/output
// @param [in/out] location : Base location of this input/output
// @param [in/out] locationOffset : Variable location offset; must be within locationCount
// @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
//                  64-bit elements.)
// @param [out] locationCount : Count of locations taken by this input/output
// @param [in/out] inOutInfo : Extra input/output info
// @returns : True if we can successfully fold the constant location offset.
bool BuilderImpl::foldConstantLocationOffset(Type *inOutTy, unsigned &location, Value *&locationOffset, Value *elemIdx,
                                             unsigned &locationCount, InOutInfo &inOutInfo) {
  // First, compute 'numComponents' if not specified
  if (inOutInfo.getNumComponents() == 0) {
    // Get the initial value from the type of this input/output.
    unsigned numComponents = inOutTy->isVectorTy() ? cast<FixedVectorType>(inOutTy)->getNumElements() : 1;

    // Then consider component indexing like this vecN[i].
    assert(isa<ConstantInt>(elemIdx)); // For dynamic component indexing, NumComponents must be specified by frontend.
    unsigned componentIndex = cast<ConstantInt>(elemIdx)->getZExtValue();
    // Take component offset into account. The provided component index actually includes this value. We must subtract
    // it to get real component index.
    if (inOutTy->getScalarSizeInBits() == 64) {
      assert(inOutInfo.getComponent() % 2 == 0); // Must be even
      componentIndex -= inOutInfo.getComponent() / 2;
    } else {
      componentIndex -= inOutInfo.getComponent();
    }
    numComponents = std::max(numComponents, componentIndex + 1);

    // For 64-bit data types, vector element or scalar is considered to occupy two components. Revise it.
    if (inOutTy->getScalarSizeInBits() == 64)
      numComponents *= 2;

    inOutInfo.setNumComponents(numComponents);
  }

  // Then, try to fold constant locationOffset into location. Currently, a variable locationOffset is only supported in
  // TCS, TES, mesh shader, and FS custom interpolation.
  if (!isa<ConstantInt>(locationOffset))
    return false;

  const unsigned constantLocationOffset = cast<ConstantInt>(locationOffset)->getZExtValue();
  location += constantLocationOffset;
  locationOffset = getInt32(0);
  assert(inOutInfo.getNumComponents() >= 1);
  locationCount = divideCeil(inOutInfo.getNumComponents(), 4);

  return true;
}

// =====================================================================================================================
// Get the mode and interp value for an FS "interpolated" (per-vertex attribute) read.
//
// @param inputInfo : InOutInfo containing interp mode and location
// @param auxInterpValue : Optional aux interp value from CreateReadInput (ignored for centroid location)
std::tuple<unsigned, llvm::Value *> BuilderImpl::getInterpModeAndValue(InOutInfo inputInfo,
                                                                       llvm::Value *auxInterpValue) {
  if (inputInfo.getInterpLoc() == InOutInfo::InterpLocExplicit) {
    // Pass-through explicit HW vertex index.
    assert(inputInfo.hasInterpAux());
    assert(inputInfo.getInterpMode() == InOutInfo::InterpModeCustom);
    return {InOutInfo::InterpModeCustom, auxInterpValue};
  }

  if (inputInfo.getInterpMode() == InOutInfo::InterpModeFlat)
    return {InOutInfo::InterpModeFlat, PoisonValue::get(getInt32Ty())};

  unsigned interpLoc = inputInfo.getInterpLoc();

  if (auxInterpValue && inputInfo.getInterpLoc() == InOutInfo::InterpLocSample) {
    // auxInterpValue is the explicit sample ID, convert to an offset from the center
    auxInterpValue = readBuiltIn(false, BuiltInSamplePosOffset, {}, auxInterpValue, nullptr, "");
    interpLoc = InOutInfo::InterpLocCenter;
  }

  auto resUsage = getPipelineState()->getShaderResourceUsage(ShaderStage::Fragment);

  if (inputInfo.getInterpMode() == InOutInfo::InterpModeSmooth) {
    if (auxInterpValue) {
      assert(interpLoc == InOutInfo::InterpLocCenter);
      return {InOutInfo::InterpModeSmooth, evalIjOffsetSmooth(auxInterpValue)};
    }

    BuiltInKind builtInId;
    switch (interpLoc) {
    case InOutInfo::InterpLocCentroid:
      builtInId = BuiltInInterpPerspCentroid;
      resUsage->builtInUsage.fs.centroid = true;
      break;
    case InOutInfo::InterpLocSample:
      builtInId = BuiltInInterpPerspSample;
      resUsage->builtInUsage.fs.sample = true;
      break;
    case InOutInfo::InterpLocCenter:
      builtInId = BuiltInInterpPerspCenter;
      resUsage->builtInUsage.fs.center = true;
      break;
    default:
      llvm_unreachable("unexpected interpLoc");
    }
    resUsage->builtInUsage.fs.smooth = true;

    return {InOutInfo::InterpModeSmooth, readBuiltIn(false, builtInId, {}, nullptr, nullptr, "")};
  }

  assert(inputInfo.getInterpMode() == InOutInfo::InterpModeNoPersp);

  BuiltInKind builtInId;

  switch (interpLoc) {
  case InOutInfo::InterpLocCentroid:
    assert(!auxInterpValue);
    builtInId = BuiltInInterpLinearCentroid;
    resUsage->builtInUsage.fs.centroid = true;
    break;
  case InOutInfo::InterpLocSample:
    assert(!auxInterpValue);
    builtInId = BuiltInInterpLinearSample;
    resUsage->builtInUsage.fs.sample = true;
    break;
  case InOutInfo::InterpLocCenter:
    builtInId = BuiltInInterpLinearCenter;
    resUsage->builtInUsage.fs.center = true;
    break;
  default:
    llvm_unreachable("unexpected interpLoc");
  }
  resUsage->builtInUsage.fs.noperspective = true;

  Value *interpValue = readBuiltIn(false, builtInId, {}, nullptr, nullptr, "");
  if (auxInterpValue)
    interpValue = adjustIj(interpValue, auxInterpValue);
  return {InOutInfo::InterpModeSmooth, interpValue};
}

// =====================================================================================================================
// Evaluate I,J for interpolation: center offset, smooth (perspective) version
//
// @param offset : Offset value, <2 x float> or <2 x half>
Value *BuilderImpl::evalIjOffsetSmooth(Value *offset) {
  // Get <I/W, J/W, 1/W>
  Value *pullModel = readBuiltIn(false, BuiltInInterpPullMode, {}, nullptr, nullptr, "");
  // Adjust each coefficient by offset.
  Value *adjusted = adjustIj(pullModel, offset);
  // Extract <I/W, J/W, 1/W> part of that
  Value *ijDivW = CreateShuffleVector(adjusted, adjusted, ArrayRef<int>{0, 1});
  Value *rcpW = CreateExtractElement(adjusted, 2);
  // Get W by making a reciprocal of 1/W
  Value *w = CreateFDiv(ConstantFP::get(getFloatTy(), 1.0), rcpW);
  w = CreateVectorSplat(2, w);
  return CreateFMul(ijDivW, w);
}

// =====================================================================================================================
// Adjust I,J values by offset.
// This adjusts value by its X and Y derivatives times the X and Y components of offset.
// If value is a vector, this is done component-wise.
//
// @param value : Value to adjust, float or vector of float
// @param offset : Offset to adjust by, <2 x float> or <2 x half>
Value *BuilderImpl::adjustIj(Value *value, Value *offset) {
  offset = CreateFPExt(offset, FixedVectorType::get(getFloatTy(), 2));
  Value *offsetX = CreateExtractElement(offset, uint64_t(0));
  Value *offsetY = CreateExtractElement(offset, 1);
  if (auto vecTy = dyn_cast<FixedVectorType>(value->getType())) {
    offsetX = CreateVectorSplat(vecTy->getNumElements(), offsetX);
    offsetY = CreateVectorSplat(vecTy->getNumElements(), offsetY);
  }
  Value *derivX = CreateDerivative(value, /*isY=*/false, /*isFine=*/true);
  Value *derivY = CreateDerivative(value, /*isY=*/true, /*isFine=*/true);
  Value *adjustX = CreateFAdd(value, CreateFMul(derivX, offsetX));
  Value *adjustY = CreateFAdd(adjustX, CreateFMul(derivY, offsetY));
  return adjustY;
}

// =====================================================================================================================
// Create a write to an XFB (transform feedback / streamout) buffer.
// The value to write must be a scalar or vector type with no more than four elements.
// A non-constant xfbOffset is not currently supported.
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
// @param isBuiltIn : True for built-in, false for user output
// @param location : Location (row) or built-in kind of output
// @param xfbBuffer : XFB buffer ID
// @param xfbStride : XFB stride
// @param xfbOffset : XFB byte offset
// @param outputInfo : Extra output info (GS stream ID)
Instruction *BuilderImpl::CreateWriteXfbOutput(Value *valueToWrite, bool isBuiltIn, unsigned location,
                                               unsigned xfbBuffer, unsigned xfbStride, Value *xfbOffset,
                                               InOutInfo outputInfo) {
  // xfbStride must be a non-zero value
  assert(xfbStride > 0);
  // Can currently only cope with constant xfbOffset.
  assert(isa<ConstantInt>(xfbOffset));

  // Ignore if not in last-vertex-stage shader (excluding copy shader).
  auto stagesAfterThisOneMask = -(ShaderStageMask(m_shaderStage.value()).m_value << 1);
  if (((getPipelineState()->getShaderStageMask() & ~ShaderStageMask(ShaderStage::Fragment) &
        ~ShaderStageMask(ShaderStage::CopyShader))
           .m_value &
       stagesAfterThisOneMask) != 0)
    return nullptr;

  // Mark the usage of the XFB buffer.
  auto resUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage.value());
  unsigned streamId = outputInfo.hasStreamId() ? outputInfo.getStreamId() : 0;
  assert(xfbBuffer < MaxTransformFeedbackBuffers);
  assert(streamId < MaxGsStreams);

  if (m_shaderStage == ShaderStage::Geometry && getPipelineState()->enableSwXfb()) {
    // NOTE: For SW-emulated stream-out, we disable GS output packing. This is because
    // the packing operation might cause a vector components belong to different vectors after the
    // packing. In handling of SW-emulated stream-out, we expect components of the same vector
    // should stay in it corresponding to a location all the time.
    getPipelineState()->setPackOutput(ShaderStage::Geometry, false);
    getPipelineState()->setPackInput(ShaderStage::Fragment, false);
  }

  // Collect the XFB output.
  XfbOutInfo xfbOutInfo = {};
  xfbOutInfo.streamId = streamId;
  xfbOutInfo.xfbBuffer = xfbBuffer;
  xfbOutInfo.xfbOffset = cast<ConstantInt>(xfbOffset)->getZExtValue();
  xfbOutInfo.is16bit = valueToWrite->getType()->getScalarSizeInBits() == 16;

  // For packed generic GS output, the XFB output should be scalarized to align with the scalarized GS output
  if (getPipelineState()->canPackOutput(m_shaderStage.value()) && !isBuiltIn) {
    Type *elementTy = valueToWrite->getType();
    unsigned scalarizeBy = 1;
    if (auto vectorTy = dyn_cast<FixedVectorType>(elementTy)) {
      scalarizeBy = vectorTy->getNumElements();
      elementTy = vectorTy->getElementType();
    }
    unsigned bitWidth = elementTy->getPrimitiveSizeInBits();
    if (bitWidth == 64) {
      // Scalarized as dword
      bitWidth = 32;
      scalarizeBy *= 2;
    }
    assert(scalarizeBy <= 8); // At most <8 x dword>
    unsigned xfbOffset = xfbOutInfo.xfbOffset;
    for (unsigned i = 0; i < scalarizeBy; ++i) {
      InOutLocationInfo outLocInfo;
      outLocInfo.setLocation(location + i / 4);
      outLocInfo.setComponent(outputInfo.getComponent() + i % 4);
      outLocInfo.setStreamId(streamId);
      outLocInfo.setBuiltIn(isBuiltIn);
      xfbOutInfo.xfbOffset = xfbOffset + i * bitWidth / 8;
      resUsage->inOutUsage.locInfoXfbOutInfoMap[outLocInfo] = xfbOutInfo;
    }
  } else {
    InOutLocationInfo outLocInfo;
    outLocInfo.setLocation(location);
    outLocInfo.setComponent(outputInfo.getComponent());
    outLocInfo.setBuiltIn(isBuiltIn);
    outLocInfo.setStreamId(streamId);
    resUsage->inOutUsage.locInfoXfbOutInfoMap[outLocInfo] = xfbOutInfo;

    if (valueToWrite->getType()->getPrimitiveSizeInBits() > 128) {
      outLocInfo.setLocation(location + 1);
      xfbOutInfo.xfbOffset += 16; // <4 x dword>
      resUsage->inOutUsage.locInfoXfbOutInfoMap[outLocInfo] = xfbOutInfo;
    }
  }

  // XFB: @lgc.output.export.xfb.%Type%(i32 xfbBuffer, i32 xfbOffset, i32 streamId, %Type% outputValue)
  SmallVector<Value *, 4> args;
  std::string instName = lgcName::OutputExportXfb;
  args.push_back(getInt32(xfbBuffer));
  args.push_back(xfbOffset);
  args.push_back(getInt32(streamId));
  args.push_back(valueToWrite);
  addTypeMangling(nullptr, args, instName);
  return CreateNamedCall(instName, getVoidTy(), args, {});
}

// =====================================================================================================================
// Create a read of barycoord input value.
// The type of the returned value is the fixed type of the specified built-in (see BuiltInDefs.h),
//
// @param builtIn : Built-in kind, BuiltInBaryCoord or BuiltInBaryCoordNoPerspKHR
// @param inputInfo : Extra input info
// @param auxInterpValue : Auxiliary value of interpolation
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateReadBaryCoord(BuiltInKind builtIn, InOutInfo inputInfo, llvm::Value *auxInterpValue,
                                        const llvm::Twine &instName) {
  assert(builtIn == lgc::BuiltInBaryCoord || builtIn == lgc::BuiltInBaryCoordNoPerspKHR);

  markBuiltInInputUsage(builtIn, 0, inputInfo);

  // Force override to per-sample interpolation.
  if (getPipelineState()->getOptions().enableInterpModePatch && !auxInterpValue &&
      inputInfo.getInterpLoc() != InOutInfo::InterpLocCentroid) {
    auxInterpValue = readBuiltIn(false, BuiltInSampleId, {}, nullptr, nullptr, "");
    inputInfo.setInterpLoc(InOutInfo::InterpLocSample);
  } else if (inputInfo.getInterpLoc() == InOutInfo::InterpLocSample) {
    // gl_BaryCoord is decorated with 'sample'
    auto &usage = getPipelineState()->getShaderResourceUsage(m_shaderStage.value())->builtInUsage;
    usage.fs.sample = true;
    usage.fs.runAtSampleRate = true;
  }

  inputInfo.setInterpMode(builtIn == lgc::BuiltInBaryCoord ? InOutInfo::InterpModeSmooth
                                                           : InOutInfo::InterpModeNoPersp);

  auto [interpMode, interpValue] = getInterpModeAndValue(inputInfo, auxInterpValue);
  assert(interpMode == InOutInfo::InterpModeSmooth);
  (void)interpMode;

  return normalizeBaryCoord(inputInfo, interpValue);
}

// =====================================================================================================================
// Create a read of (part of) a built-in input value.
// The type of the returned value is the fixed type of the specified built-in (see BuiltInDefs.h),
// or the element type if index is not nullptr. For ClipDistance or CullDistance when index is nullptr,
// the array size is determined by inputInfo.GetArraySize().
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param inputInfo : Extra input info (shader-defined array size)
// @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index, else nullptr
// @param index : Array or vector index to access part of an input, else nullptr
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateReadBuiltInInput(BuiltInKind builtIn, InOutInfo inputInfo, Value *vertexIndex, Value *index,
                                           const Twine &instName) {
  assert(isBuiltInInput(builtIn));
  Value *builtInVal = readBuiltIn(false, builtIn, inputInfo, vertexIndex, index, instName);
  if (builtIn == BuiltInViewIndex)
    // View index can only use bit[3:0] of view ID register.
    builtInVal = CreateAnd(builtInVal, getInt32(0xF));
  return builtInVal;
}

// =====================================================================================================================
// Create a read of (part of) a built-in output value.
// The type of the returned value is the fixed type of the specified built-in (see BuiltInDefs.h),
// or the element type if index is not nullptr.
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param outputInfo : Extra output info (shader-defined array size)
// @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index, else nullptr
// @param index : Array or vector index to access part of an input, else nullptr
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateReadBuiltInOutput(BuiltInKind builtIn, InOutInfo outputInfo, Value *vertexIndex, Value *index,
                                            const Twine &instName) {
  // Currently this only copes with reading an output in TCS.
  assert(m_shaderStage == ShaderStage::TessControl);
  assert(isBuiltInOutput(builtIn));
  return readBuiltIn(true, builtIn, outputInfo, vertexIndex, index, instName);
}

// =====================================================================================================================
// Read (part of) a built-in value
//
// @param isOutput : True to read built-in output, false to read built-in input
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param inOutInfo : Extra input/output info (shader-defined array size)
// @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index, else nullptr Special case for FS
// BuiltInSamplePosOffset: sample number. That special case only happens when ReadBuiltIn is called from
// ModifyAuxInterpValue.
// @param index : Array or vector index to access part of an input, else nullptr
// @param instName : Name to give instruction(s)
Value *BuilderImpl::readBuiltIn(bool isOutput, BuiltInKind builtIn, InOutInfo inOutInfo, Value *vertexIndex,
                                Value *index, const Twine &instName) {
  // Mark usage.
  unsigned arraySize = inOutInfo.getArraySize();
  if (auto constIndex = dyn_cast_or_null<ConstantInt>(index))
    arraySize = constIndex->getZExtValue() + 1;

  if (!isOutput)
    markBuiltInInputUsage(builtIn, arraySize, inOutInfo);
  else
    markBuiltInOutputUsage(builtIn, arraySize, InvalidValue);

  // Get the built-in type.
  Type *resultTy = getBuiltInTy(builtIn, inOutInfo);
  if (index) {
    if (isa<ArrayType>(resultTy))
      resultTy = resultTy->getArrayElementType();
    else
      resultTy = cast<VectorType>(resultTy)->getElementType();
  }

  // Handle certain common built-ins directly.
  if (Value *result = readCommonBuiltIn(builtIn, resultTy, instName))
    return result;

  if ((m_shaderStage == ShaderStage::Compute || m_shaderStage == ShaderStage::Task) && !isOutput) {
    // We handle compute shader or task shader inputs directly.
    return readCsBuiltIn(builtIn, instName);
  }

  if (m_shaderStage == ShaderStage::Vertex && !isOutput) {
    // We can handle some vertex shader inputs directly.
    Value *result = readVsBuiltIn(builtIn, instName);
    if (result)
      return result;
  }

  // For now, this just generates a call to lgc.input.import.builtin. A future commit will
  // change it to generate IR more directly here.
  // A vertex index is valid only in TCS, TES, GS.
  // Currently we can only cope with an array/vector index in TCS/TES.
  SmallVector<Value *, 4> args;
  args.push_back(getInt32(builtIn));
  switch (m_shaderStage.value()) {
  case ShaderStage::TessControl:
  case ShaderStage::TessEval:
    args.push_back(index ? index : getInt32(InvalidValue));
    args.push_back(vertexIndex ? vertexIndex : getInt32(InvalidValue));
    break;
  case ShaderStage::Geometry:
    assert(!index);
    args.push_back(vertexIndex ? vertexIndex : getInt32(InvalidValue));
    break;
  case ShaderStage::Mesh:
    assert(!vertexIndex);
    args.push_back(index ? index : getInt32(InvalidValue));
    break;
  case ShaderStage::Fragment:
    if (builtIn == BuiltInSamplePosOffset) {
      // Special case for BuiltInSamplePosOffset: vertexIndex is the sample number.
      // That special case only happens when ReadBuiltIn is called from ModifyAuxInterpValue.
      Value *sampleNum = vertexIndex;
      vertexIndex = nullptr;
      args.push_back(sampleNum);
    }
    assert(!index && !vertexIndex);
    break;
  default:
    assert(!index && !vertexIndex);
    break;
  }

  std::string callName = isOutput ? lgcName::OutputImportBuiltIn : lgcName::InputImportBuiltIn;
  callName += PipelineState::getBuiltInName(builtIn);
  addTypeMangling(resultTy, args, callName);
  Value *result = CreateNamedCall(callName, resultTy, args, {Attribute::ReadOnly, Attribute::WillReturn});

  if (instName.isTriviallyEmpty())
    result->setName(PipelineState::getBuiltInName(builtIn));
  else
    result->setName(instName);

  return result;
}

// =====================================================================================================================
// Get provoking vertex value
//
// @param isOne : true if provoking vertex value is 1
// @param isTwo : true if provoking vertex value is 2
void BuilderImpl::getProvokingVertexInfo(llvm::Value **isOne, llvm::Value **isTwo) {
  // Provoking Vertex Info is a PS SGPR, it contains the 2-bit provoking vertex for up to 16 primitives in a PS wave.
  // quadId = landID / 4;
  // provokingVertex = provokingVtxInfo[quadId * 2 : quadId * 2 + 2];
  auto provokingVtxInfo =
      ShaderInputs::getInput(ShaderInput::ProvokingVtxInfo, BuilderBase::get(*this), *getLgcContext());

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 479645
  auto laneID = CreateGetLaneNumber();
  auto quadId = CreateSDiv(laneID, getInt32(4));
  auto provokingVertex = CreateIntrinsic(Intrinsic::amdgcn_ubfe, getInt32Ty(),
                                         {provokingVtxInfo, CreateMul(quadId, getInt32(2)), getInt32(2)});

  *isOne = CreateICmpEQ(provokingVertex, getInt32(1));
  *isTwo = CreateICmpEQ(provokingVertex, getInt32(2));

#else
  // Extract 2-bit vertex index from provokingVtxInfo
  auto isTwoMask = CreateAnd(provokingVtxInfo, getInt32(0xAAAAAAAA));
  auto isOneMask =
      CreateAnd(CreateAnd(provokingVtxInfo, getInt32(0x55555555)), CreateLShr(CreateNot(isTwoMask), getInt32(1)));
  isTwoMask = CreateIntrinsic(Intrinsic::amdgcn_s_bitreplicate, {}, isTwoMask);
  isOneMask = CreateIntrinsic(Intrinsic::amdgcn_s_bitreplicate, {}, isOneMask);
  isTwoMask = CreateIntrinsic(getInt64Ty(), Intrinsic::amdgcn_s_wqm, isTwoMask);
  isOneMask = CreateIntrinsic(getInt64Ty(), Intrinsic::amdgcn_s_wqm, isOneMask);
  *isTwo = CreateIntrinsic(getInt1Ty(), Intrinsic::amdgcn_inverse_ballot, isTwoMask);
  *isOne = CreateIntrinsic(getInt1Ty(), Intrinsic::amdgcn_inverse_ballot, isOneMask);
#endif
}

// =====================================================================================================================
// Reorder and normalize the barycoord
//
// @param iJCoord : IJ coordinates provided for the HW interpolation view
// @returns : gl_Barycoord
Value *BuilderImpl::normalizeBaryCoord(InOutInfo inputInfo, Value *iJCoord) {
  auto baryType = FixedVectorType::get(getFloatTy(), 3);
  auto zero = ConstantFP::get(getFloatTy(), 0.0);
  auto one = ConstantFP::get(getFloatTy(), 1.0);

  Value *hwCoord[3] = {};
  hwCoord[0] = CreateExtractElement(iJCoord, uint64_t(0));
  hwCoord[1] = CreateExtractElement(iJCoord, 1);
  hwCoord[2] = CreateFSub(CreateFSub(one, hwCoord[0]), hwCoord[1]);

  if (m_pipelineState->isUnlinked() || m_pipelineState->getOptions().dynamicTopology) {
    auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment);
    resUsage->builtInUsage.fs.useDynamicToplogy = true;
    auto numVertices = ShaderInputs::getSpecialUserData(UserDataMapping::CompositeData, BuilderBase::get(*this));
    numVertices = CreateIntrinsic(Intrinsic::amdgcn_ubfe, getInt32Ty(), {numVertices, getInt32(0), getInt32(2)});
    auto currentBlock = GetInsertBlock();
    auto endBlock = currentBlock->splitBasicBlock(numVertices->getNextNode());
    currentBlock->getTerminator()->eraseFromParent();
    SetInsertPoint(currentBlock);
    auto switchInst = CreateSwitch(numVertices, endBlock, 3);
    BasicBlock *case0 = BasicBlock::Create(getContext(), "case0", currentBlock->getParent(), endBlock);
    BasicBlock *case1 = BasicBlock::Create(getContext(), "case1", currentBlock->getParent(), endBlock);
    BasicBlock *case2 = BasicBlock::Create(getContext(), "case2", currentBlock->getParent(), endBlock);
    switchInst->addCase(getInt32(1), case0);
    switchInst->addCase(getInt32(2), case1);
    switchInst->addCase(getInt32(3), case2);

    Value *pointCoord = ConstantVector::get({one, zero, zero});
    {
      SetInsertPoint(case0);
      CreateBr(endBlock);
    }

    Value *lineCoord = ConstantVector::get({zero, zero, zero});
    {
      SetInsertPoint(case1);
      auto yCoord = CreateFAdd(hwCoord[0], hwCoord[1]);
      lineCoord = CreateInsertElement(lineCoord, hwCoord[2], uint64_t(0));
      lineCoord = CreateInsertElement(lineCoord, yCoord, 1);
      CreateBr(endBlock);
    }

    Value *triCoord = PoisonValue::get(baryType);
    {
      SetInsertPoint(case2);
      Value *isOne = nullptr;
      Value *isTwo = nullptr;
      getProvokingVertexInfo(&isOne, &isTwo);

      Value *barycoord1 = CreateInsertElement(PoisonValue::get(baryType), hwCoord[0], uint64_t(0));
      barycoord1 = CreateInsertElement(barycoord1, hwCoord[1], 1);
      barycoord1 = CreateInsertElement(barycoord1, hwCoord[2], 2);

      Value *barycoord0 = CreateShuffleVector(barycoord1, ArrayRef<int>({2, 0, 1}));
      Value *barycoord2 = CreateShuffleVector(barycoord1, ArrayRef<int>({1, 2, 0}));
      triCoord = CreateSelect(isOne, barycoord1, CreateSelect(isTwo, barycoord2, barycoord0));
      CreateBr(endBlock);
    }

    {
      SetInsertPoint(&*endBlock->getFirstInsertionPt());
      auto phiInst = CreatePHI(baryType, 4);
      phiInst->addIncoming(pointCoord, case0);
      phiInst->addIncoming(lineCoord, case1);
      phiInst->addIncoming(triCoord, case2);
      phiInst->addIncoming(PoisonValue::get(baryType), currentBlock);
      return phiInst;
    }
  }

  auto vertexCount = m_pipelineState->getVerticesPerPrimitive();
  switch (vertexCount) {
  case 1:
    // Points
    return ConstantVector::get({one, zero, zero});
  case 2: {
    // Lines
    // The weight of vertex0 is (1 - i - j), the weight of vertex1 is (i + j).
    auto yCoord = CreateFAdd(hwCoord[0], hwCoord[1]);
    Value *barycoord = CreateInsertElement(ConstantVector::get({zero, zero, zero}), hwCoord[2], uint64_t(0));
    barycoord = CreateInsertElement(barycoord, yCoord, 1);
    return barycoord;
  }
  case 3: {
    Value *isOne = nullptr;
    Value *isTwo = nullptr;
    getProvokingVertexInfo(&isOne, &isTwo);

    Value *barycoord1 = CreateInsertElement(PoisonValue::get(baryType), hwCoord[0], uint64_t(0));
    barycoord1 = CreateInsertElement(barycoord1, hwCoord[1], 1);
    barycoord1 = CreateInsertElement(barycoord1, hwCoord[2], 2);

    if (inputInfo.isProvokingVertexModeDisabled()) {
      // return the original i,j,k w/o any adjustment
      return barycoord1;
    }

    Value *barycoord0 = CreateShuffleVector(barycoord1, ArrayRef<int>({2, 0, 1}));
    Value *barycoord2 = CreateShuffleVector(barycoord1, ArrayRef<int>({1, 2, 0}));
    return CreateSelect(isOne, barycoord1, CreateSelect(isTwo, barycoord2, barycoord0));
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }
  return PoisonValue::get(baryType);
}

// =====================================================================================================================
// Read and directly handle certain built-ins that are common between shader stages
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param resultTy : Expected result type
// @param instName : Name to give instruction(s)
// @returns : Value of input; nullptr if not handled here
Value *BuilderImpl::readCommonBuiltIn(BuiltInKind builtIn, llvm::Type *resultTy, const Twine &instName) {
  switch (builtIn) {

  case BuiltInSubgroupEqMask:
  case BuiltInSubgroupGeMask:
  case BuiltInSubgroupGtMask:
  case BuiltInSubgroupLeMask:
  case BuiltInSubgroupLtMask: {
    // Handle the subgroup mask built-ins directly.
    Value *result = nullptr;
    Value *localInvocationId = readBuiltIn(false, BuiltInSubgroupLocalInvocationId, {}, nullptr, nullptr, "");
    if (getPipelineState()->getShaderSubgroupSize(m_shaderStage.value()) == 64)
      localInvocationId = CreateZExt(localInvocationId, getInt64Ty());

    switch (builtIn) {
    case BuiltInSubgroupEqMask:
      result = CreateShl(ConstantInt::get(localInvocationId->getType(), 1), localInvocationId);
      break;
    case BuiltInSubgroupGeMask:
      result = CreateShl(ConstantInt::get(localInvocationId->getType(), -1), localInvocationId);
      break;
    case BuiltInSubgroupGtMask:
      result = CreateShl(ConstantInt::get(localInvocationId->getType(), -2), localInvocationId);
      break;
    case BuiltInSubgroupLeMask:
      result = CreateSub(CreateShl(ConstantInt::get(localInvocationId->getType(), 2), localInvocationId),
                         ConstantInt::get(localInvocationId->getType(), 1));
      break;
    case BuiltInSubgroupLtMask:
      result = CreateSub(CreateShl(ConstantInt::get(localInvocationId->getType(), 1), localInvocationId),
                         ConstantInt::get(localInvocationId->getType(), 1));
      break;
    default:
      llvm_unreachable("Should never be called!");
    }
    if (getPipelineState()->getShaderSubgroupSize(m_shaderStage.value()) == 64) {
      result = CreateInsertElement(Constant::getNullValue(FixedVectorType::get(getInt64Ty(), 2)), result, uint64_t(0));
      result = CreateBitCast(result, resultTy);
    } else
      result = CreateInsertElement(ConstantInt::getNullValue(resultTy), result, uint64_t(0));
    result->setName(instName);
    return result;
  }

  case BuiltInSubgroupSize:
    // SubgroupSize is a constant.
    return getInt32(getPipelineState()->getShaderSubgroupSize(m_shaderStage.value()));

  case BuiltInSubgroupLocalInvocationId:
    // SubgroupLocalInvocationId is the lane number within the wave.
    return CreateGetLaneNumber();

  case BuiltInDeviceIndex:
    return getInt32(getPipelineState()->getDeviceIndex());

  default:
    // Built-in not handled here.
    return nullptr;
  }
}

// =====================================================================================================================
// Read compute/task shader input
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param instName : Name to give instruction(s)
// @returns : Value of input; nullptr if not handled here
Value *BuilderImpl::readCsBuiltIn(BuiltInKind builtIn, const Twine &instName) {
  auto &shaderMode = m_pipelineState->getShaderModes()->getComputeShaderMode();
  switch (builtIn) {

  case BuiltInWorkgroupSize:
    // WorkgroupSize is a constant vector supplied by shader mode.
    return ConstantVector::get({getInt32(shaderMode.workgroupSizeX), getInt32(shaderMode.workgroupSizeY),
                                getInt32(shaderMode.workgroupSizeZ)});

  case BuiltInNumWorkgroups: {
    if (m_shaderStage == ShaderStage::Task) {
      // For task shader, NumWorkgroups is a v3i32 special user data (three SGPRs).
      return ShaderInputs::getSpecialUserData(UserDataMapping::MeshTaskDispatchDims, BuilderBase::get(*this));
    }

    // For compute shader, NumWorkgroups is a v3i32 loaded from an address pointed to by a special user data item.
    assert(m_shaderStage == ShaderStage::Compute);
    Value *numWorkgroupPtr = ShaderInputs::getSpecialUserData(UserDataMapping::Workgroup, BuilderBase::get(*this));
    LoadInst *load = CreateLoad(FixedVectorType::get(getInt32Ty(), 3), numWorkgroupPtr);
    load->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(getContext(), {}));
    return load;
  }

  case BuiltInWorkgroupId: {
    // WorkgroupId is a v3i32 shader input (three SGPRs set up by hardware).
    Value *workgroupId = ShaderInputs::getInput(ShaderInput::WorkgroupId, BuilderBase::get(*this), *getLgcContext());

    auto options = m_pipelineState->getOptions();

    // If thread group swizzle is enabled, we insert a call here and later lower it to code.
    if (options.threadGroupSwizzleMode != ThreadGroupSwizzleMode::Default) {
      // The calculation requires NumWorkgroups and WorkgroupId.
      workgroupId = CreateNamedCall(lgcName::SwizzleWorkgroupId, workgroupId->getType(),
                                    {readCsBuiltIn(BuiltInNumWorkgroups), workgroupId}, {});
    }

    // If the buffer descriptor set and binding are given, it means that we will do the reverse thread group
    // optimization.
    if (options.reverseThreadGroupBufferBinding && options.reverseThreadGroupBufferDescSet) {
      Value *reversedWorkgroupId = CreateSub(CreateSub(readCsBuiltIn(BuiltInNumWorkgroups), workgroupId),
                                             ConstantVector::get({getInt32(1), getInt32(1), getInt32(1)}));

      // Load control bit from internal buffer
      auto bufferDesc = create<lgc::LoadBufferDescOp>(options.reverseThreadGroupBufferDescSet,
                                                      options.reverseThreadGroupBufferBinding, getInt32(0), 0);
      auto controlBitPtr = CreateInBoundsGEP(getInt8Ty(), bufferDesc, getInt32(0));
      auto controlBit = CreateTrunc(CreateLoad(getInt32Ty(), controlBitPtr), getInt1Ty());

      workgroupId = CreateSelect(controlBit, reversedWorkgroupId, workgroupId);
    }

    return workgroupId;
  }

  case BuiltInLocalInvocationId:
  case BuiltInUnswizzledLocalInvocationId: {
    // LocalInvocationId is a v3i32 shader input (three VGPRs set up in hardware).
    Value *localInvocationId =
        ShaderInputs::getInput(ShaderInput::LocalInvocationId, BuilderBase::get(*this), *getLgcContext());

    // On GFX11, it is a single VGPR and we need to extract the three components.
    if (getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 11) {
      static const unsigned mask = 0x3ff;
      Value *unpackedLocalInvocationId = PoisonValue::get(FixedVectorType::get(getInt32Ty(), 3));

      // X = PackedId[9:0]
      unpackedLocalInvocationId =
          CreateInsertElement(unpackedLocalInvocationId, CreateAnd(localInvocationId, getInt32(mask)), uint64_t(0));

      // Y = PackedId[19:10]
      localInvocationId = CreateLShr(localInvocationId, getInt32(10));
      unpackedLocalInvocationId =
          CreateInsertElement(unpackedLocalInvocationId, CreateAnd(localInvocationId, getInt32(mask)), 1);

      // Z = PackedId[29:20], PackedId[31:30] set to 0 by hardware
      localInvocationId = CreateLShr(localInvocationId, getInt32(10));
      unpackedLocalInvocationId = CreateInsertElement(unpackedLocalInvocationId, localInvocationId, 2);

      localInvocationId = unpackedLocalInvocationId;
    }

    // Unused dimensions need zero-initializing.
    if (shaderMode.workgroupSizeZ <= 1) {
      if (shaderMode.workgroupSizeY <= 1)
        localInvocationId = CreateInsertElement(localInvocationId, getInt32(0), 1);
      localInvocationId = CreateInsertElement(localInvocationId, getInt32(0), 2);
    }

    if (m_shaderStage == ShaderStage::Compute) {
      // Reconfigure the workgroup layout later if it's necessary.
      if (!getPipelineState()->isComputeLibrary()) {
        // Insert a call that later on might get lowered to code to reconfigure the workgroup.
        localInvocationId = CreateNamedCall(
            lgcName::ReconfigureLocalInvocationId, localInvocationId->getType(),
            {localInvocationId, getInt32(builtIn == BuiltInUnswizzledLocalInvocationId)}, Attribute::ReadNone);
      }
    }

    return localInvocationId;
  }

  case BuiltInNumSubgroups: {
    // workgroupSize = gl_WorkGroupSize.x * gl_WorkGroupSize.y * gl_WorkGroupSize.z
    // gl_NumSubgroups = (workgroupSize + gl_SubGroupSize - 1) / gl_SubgroupSize
    auto &mode = m_pipelineState->getShaderModes()->getComputeShaderMode();
    unsigned workgroupSize = mode.workgroupSizeX * mode.workgroupSizeY * mode.workgroupSizeZ;
    unsigned subgroupSize = m_pipelineState->getShaderSubgroupSize(m_shaderStage.value());
    unsigned numSubgroups = (workgroupSize + subgroupSize - 1) / subgroupSize;
    return getInt32(numSubgroups);
  }

  case BuiltInGlobalInvocationId: {
    // GlobalInvocationId is WorkgroupId * WorkgroupSize + LocalInvocationId
    Value *result = CreateMul(readCsBuiltIn(BuiltInWorkgroupId), readCsBuiltIn(BuiltInWorkgroupSize));
    return CreateAdd(result, readCsBuiltIn(BuiltInLocalInvocationId));
  }

  case BuiltInLocalInvocationIndex:
  case BuiltInUnswizzledLocalInvocationIndex: {
    // LocalInvocationIndex is
    // (WorkgroupSize.Y * LocalInvocationId.Z + LocalInvocationId.Y) * WorkGroupSize.X + LocalInvocationId.X
    Value *workgroupSize = readCsBuiltIn(BuiltInWorkgroupSize);
    Value *localInvocationId = nullptr;
    if (builtIn == BuiltInLocalInvocationIndex) {
      localInvocationId = readCsBuiltIn(BuiltInLocalInvocationId, "readLocalInvocationId");
    } else {
      localInvocationId = readCsBuiltIn(BuiltInUnswizzledLocalInvocationId, "readHWLocalInvocationId");
    }
    Value *input = CreateMul(CreateExtractElement(workgroupSize, 1), CreateExtractElement(localInvocationId, 2));
    input = CreateAdd(input, CreateExtractElement(localInvocationId, 1));
    input = CreateMul(CreateExtractElement(workgroupSize, uint64_t(0)), input);
    input = CreateAdd(input, CreateExtractElement(localInvocationId, uint64_t(0)));
    return input;
  }

  case BuiltInSubgroupId: {
    GfxIpVersion gfxIp = getPipelineState()->getTargetInfo().getGfxIpVersion();
    // From Navi21, it should load the subgroupid from sgpr initialized at wave launch.
    {
      if (gfxIp >= GfxIpVersion({10, 3})) {
        Value *multiDispatchInfo =
            ShaderInputs::getInput(ShaderInput::MultiDispatchInfo, BuilderBase::get(*this), *getLgcContext());

        // waveId = dispatchInfo[24:20]
        Value *waveIdInSubgroup = CreateAnd(CreateLShr(multiDispatchInfo, 20), 0x1F, "waveIdInSubgroup");
        return waveIdInSubgroup;
      } else {
        // Before Navi21, it should read the value before swizzling which is correct to calculate subgroup id.
        Value *localInvocationIndex = readCsBuiltIn(BuiltInUnswizzledLocalInvocationIndex);
        unsigned subgroupSize = getPipelineState()->getShaderSubgroupSize(m_shaderStage.value());
        return CreateLShr(localInvocationIndex, getInt32(Log2_32(subgroupSize)));
      }
    }
  }

  case BuiltInDrawIndex: {
    assert(m_shaderStage == ShaderStage::Task); // Task shader only
    return ShaderInputs::getSpecialUserData(UserDataMapping::DrawIndex, BuilderBase::get(*this));
  }

  default:
    // Not handled. This should never happen; we need to handle all CS built-ins here because the old way of
    // handling them (caller will handle with lgc.input.import.builtin, which is then lowered in
    // PatchInOutImportExport) does not work with compute-with-calls.
    llvm_unreachable("Unhandled CS built-in");
    return nullptr;
  }
}

// =====================================================================================================================
// Read vertex shader input
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param instName : Name to give instruction(s)
// @returns : Value of input; nullptr if not handled here
Value *BuilderImpl::readVsBuiltIn(BuiltInKind builtIn, const Twine &instName) {
  BuilderBase &builder = BuilderBase::get(*this);
  switch (builtIn) {
  case BuiltInBaseVertex:
    return ShaderInputs::getSpecialUserData(UserDataMapping::BaseVertex, builder);
  case BuiltInBaseInstance:
    return ShaderInputs::getSpecialUserData(UserDataMapping::BaseInstance, builder);
  case BuiltInDrawIndex:
    return ShaderInputs::getSpecialUserData(UserDataMapping::DrawIndex, builder);
  case BuiltInVertexIndex:
    return ShaderInputs::getVertexIndex(builder, *getLgcContext());
  case BuiltInInstanceIndex:
    return ShaderInputs::getInstanceIndex(builder, *getLgcContext());
  case BuiltInViewIndex:
    if (m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable)
      return ShaderInputs::getSpecialUserData(UserDataMapping::ViewId, builder);
    return builder.getInt32(0);
  case BuiltInVertexId:
    return ShaderInputs::getInput(ShaderInput::VertexId, builder, *getLgcContext());
  case BuiltInInstanceId:
    return ShaderInputs::getInput(ShaderInput::InstanceId, builder, *getLgcContext());
  default:
    // Not handled; caller will handle with lgc.input.import.builtin, which is then lowered in PatchInOutImportExport.
    return nullptr;
  }
}

// =====================================================================================================================
// Create a write of (part of) a built-in output value.
// The type of the value to write must be the fixed type of the specified built-in (see BuiltInDefs.h),
// or the element type if index is not nullptr.
//
// @param valueToWrite : Value to write
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param outputInfo : Extra output info (shader-defined array size; GS stream id)
// @param vertexOrPrimitiveIndex : For TCS/mesh shader per-vertex output: vertex index; for mesh shader per-primitive
//                                 output: primitive index; else nullptr
// @param index : Array or vector index to access part of an input, else nullptr
Instruction *BuilderImpl::CreateWriteBuiltInOutput(Value *valueToWrite, BuiltInKind builtIn, InOutInfo outputInfo,
                                                   Value *vertexOrPrimitiveIndex, Value *index) {
  // Mark usage.
  unsigned streamId = outputInfo.hasStreamId() ? outputInfo.getStreamId() : InvalidValue;
  unsigned arraySize = outputInfo.getArraySize();
  if (auto constIndex = dyn_cast_or_null<ConstantInt>(index))
    arraySize = constIndex->getZExtValue() + 1;
  markBuiltInOutputUsage(builtIn, arraySize, streamId);

#ifndef NDEBUG
  // Assert we have the right type. Allow for ClipDistance/CullDistance being a different array size.
  Type *expectedTy = getBuiltInTy(builtIn, outputInfo);
  if (builtIn == BuiltInPrimitivePointIndices || builtIn == BuiltInPrimitiveLineIndices ||
      builtIn == BuiltInPrimitiveTriangleIndices) {
    // The built-ins PrimitivePointIndices, PrimitiveLineIndices, and PrimitiveTriangleIndices are output arrays
    // for primitive-based indexing. Writing to the whole array is disallowed.
    assert(m_shaderStage == ShaderStage::Mesh && vertexOrPrimitiveIndex);
    assert(expectedTy->isArrayTy());
    expectedTy = cast<ArrayType>(expectedTy)->getElementType();
  }
  if (index) {
    if (isa<ArrayType>(expectedTy))
      expectedTy = cast<ArrayType>(expectedTy)->getElementType();
    else
      expectedTy = cast<VectorType>(expectedTy)->getElementType();
  }
  assert(expectedTy == valueToWrite->getType() ||
         ((builtIn == BuiltInClipDistance || builtIn == BuiltInCullDistance) &&
          valueToWrite->getType()->getArrayElementType() == expectedTy->getArrayElementType()));
#endif

  // For now, this just generates a call to lgc.output.export.builtin. A future commit will
  // change it to generate IR more directly here.
  // A vertex/primitive index is valid only in TCS and mesh shader.
  // Currently we can only cope with an array/vector index in TCS.
  //
  // VS: @lgc.output.export.builtin.%BuiltIn%(i32 builtInId, %Type% outputValue)
  // TCS: @lgc.output.export.builtin.%BuiltIn%.%Type%(i32 builtInId, i32 elemIdx, i32 vertexIdx, %Type% outputValue)
  // TES: @lgc.output.export.builtin.%BuiltIn%.%Type%(i32 builtInId, %Type% outputValue)
  // GS: @lgc.output.export.builtin.%BuiltIn%(i32 builtInId, i32 streamId, %Type% outputValue)
  // Mesh: @lgc.output.export.builtin.%BuiltIn%.%Type%(i32 builtInId, i32 elemIdx, i32 vertexOrPrimitiveIdx,
  //                                                   i1 perPrimitive, %Type% outputValue)
  // FS: @lgc.output.export.builtin.%BuiltIn%(i32 builtInId, %Type% outputValue)
  SmallVector<Value *, 4> args;
  args.push_back(getInt32(builtIn));
  switch (m_shaderStage.value()) {
  case ShaderStage::TessControl:
  case ShaderStage::Mesh:
    args.push_back(index ? index : getInt32(InvalidValue));
    args.push_back(vertexOrPrimitiveIndex ? vertexOrPrimitiveIndex : getInt32(InvalidValue));
    if (m_shaderStage == ShaderStage::Mesh)
      args.push_back(getInt1(outputInfo.isPerPrimitive()));
    break;
  case ShaderStage::Geometry:
    assert(!index && !vertexOrPrimitiveIndex);
    args.push_back(getInt32(streamId));
    break;
  default:
    assert(!index && !vertexOrPrimitiveIndex);
    break;
  }
  args.push_back(valueToWrite);

  std::string callName = lgcName::OutputExportBuiltIn;
  callName += PipelineState::getBuiltInName(builtIn);
  addTypeMangling(nullptr, args, callName);
  return cast<Instruction>(CreateNamedCall(callName, getVoidTy(), args, {}));
}

// =====================================================================================================================
// Get the type of a built-in. This overrides the one in Builder to additionally recognize the internal built-ins.
//
// @param builtIn : Built-in kind
// @param inOutInfo : Extra input/output info (shader-defined array size)
Type *BuilderImpl::getBuiltInTy(BuiltInKind builtIn, InOutInfo inOutInfo) {
  switch (static_cast<unsigned>(builtIn)) {
  case BuiltInSamplePosOffset:
  case BuiltInInterpPerspCenter:
  case BuiltInInterpLinearCenter:
  case BuiltInInterpPerspCentroid:
  case BuiltInInterpLinearCentroid:
  case BuiltInInterpPerspSample:
  case BuiltInInterpLinearSample:
    return FixedVectorType::get(getFloatTy(), 2);
  case BuiltInInterpPullMode:
    return FixedVectorType::get(getFloatTy(), 3);
  default:
    return BuilderDefs::getBuiltInTy(builtIn, inOutInfo);
  }
}

// =====================================================================================================================
// Mark usage of a built-in input. This is only needed where a built-in is handled by generating lgc.import.input
// to be lowered in PatchInOutImportExport, and not when it is directly generated here using
// ShaderInputs::getInput() and/or ShaderInputs::getSpecialUserData().
//
// @param builtIn : Built-in ID
// @param arraySize : Number of array elements for ClipDistance and CullDistance. (Multiple calls to this function for
// this built-in might have different array sizes; we take the max)
// @param inOutInfo : Extra input/output info (shader-defined array size)
void BuilderImpl::markBuiltInInputUsage(BuiltInKind &builtIn, unsigned arraySize, InOutInfo inOutInfo) {
  auto &usage = getPipelineState()->getShaderResourceUsage(m_shaderStage.value())->builtInUsage;
  assert((builtIn != BuiltInClipDistance && builtIn != BuiltInCullDistance) || arraySize != 0);
  switch (m_shaderStage.value()) {
  case ShaderStage::Vertex: {
    switch (builtIn) {
    case BuiltInPrimitiveId:
      usage.vs.primitiveId = true;
      break;
    case BuiltInViewIndex:
      usage.vs.viewIndex = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStage::TessControl: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.tcs.pointSizeIn = true;
      break;
    case BuiltInPosition:
      usage.tcs.positionIn = true;
      break;
    case BuiltInClipDistance:
      usage.tcs.clipDistanceIn = std::max(usage.tcs.clipDistanceIn, arraySize);
      break;
    case BuiltInCullDistance:
      usage.tcs.cullDistanceIn = std::max(usage.tcs.cullDistanceIn, arraySize);
      break;
    case BuiltInPatchVertices:
      usage.tcs.patchVertices = true;
      break;
    case BuiltInPrimitiveId:
      usage.tcs.primitiveId = true;
      break;
    case BuiltInInvocationId:
      usage.tcs.invocationId = true;
      break;
    case BuiltInViewIndex:
      usage.tcs.viewIndex = true;
      break;
    case BuiltInLayer:
      usage.tcs.layerIn = true;
      break;
    case BuiltInViewportIndex:
      usage.tcs.viewportIndexIn = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStage::TessEval: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.tes.pointSizeIn = true;
      break;
    case BuiltInPosition:
      usage.tes.positionIn = true;
      break;
    case BuiltInClipDistance:
      usage.tes.clipDistanceIn = std::max(usage.tes.clipDistanceIn, arraySize);
      break;
    case BuiltInCullDistance:
      usage.tes.cullDistanceIn = std::max(usage.tes.cullDistanceIn, arraySize);
      break;
    case BuiltInPatchVertices:
      usage.tes.patchVertices = true;
      break;
    case BuiltInPrimitiveId:
      usage.tes.primitiveId = true;
      break;
    case BuiltInTessCoord:
      usage.tes.tessCoord = true;
      break;
    case BuiltInTessLevelOuter:
      usage.tes.tessLevelOuter = true;
      break;
    case BuiltInTessLevelInner:
      usage.tes.tessLevelInner = true;
      break;
    case BuiltInViewIndex:
      usage.tes.viewIndex = true;
      break;
    case BuiltInLayer:
      usage.tes.layerIn = true;
      break;
    case BuiltInViewportIndex:
      usage.tes.viewportIndexIn = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStage::Geometry: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.gs.pointSizeIn = true;
      break;
    case BuiltInPosition:
      usage.gs.positionIn = true;
      break;
    case BuiltInClipDistance:
      usage.gs.clipDistanceIn = std::max(usage.gs.clipDistanceIn, arraySize);
      break;
    case BuiltInCullDistance:
      usage.gs.cullDistanceIn = std::max(usage.gs.cullDistanceIn, arraySize);
      break;
    case BuiltInPrimitiveId:
      usage.gs.primitiveIdIn = true;
      break;
    case BuiltInInvocationId:
      usage.gs.invocationId = true;
      break;
    case BuiltInViewIndex:
      usage.gs.viewIndex = true;
      break;
    case BuiltInLayer:
      usage.gs.layerIn = true;
      break;
    case BuiltInViewportIndex:
      usage.gs.viewportIndexIn = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStage::Mesh: {
    switch (builtIn) {
    case BuiltInDrawIndex:
      usage.mesh.drawIndex = true;
      break;
    case BuiltInViewIndex:
      usage.mesh.viewIndex = true;
      break;
    case BuiltInNumWorkgroups:
      usage.mesh.numWorkgroups = true;
      break;
    case BuiltInWorkgroupId:
      usage.mesh.workgroupId = true;
      break;
    case BuiltInLocalInvocationId:
      usage.mesh.localInvocationId = true;
      break;
    case BuiltInGlobalInvocationId:
      usage.mesh.globalInvocationId = true;
      break;
    case BuiltInLocalInvocationIndex:
      usage.mesh.localInvocationIndex = true;
      break;
    case BuiltInSubgroupId:
      usage.mesh.subgroupId = true;
      break;
    case BuiltInNumSubgroups:
      usage.mesh.numSubgroups = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStage::Fragment: {
    switch (static_cast<unsigned>(builtIn)) {
    case BuiltInFragCoord:
      usage.fs.fragCoord = true;
      if (inOutInfo.getInterpMode() == InOutInfo::InterpLocSample)
        usage.fs.fragCoordIsSample = true;
      break;
    case BuiltInFrontFacing:
      usage.fs.frontFacing = true;
      break;
    case BuiltInClipDistance:
      usage.fs.clipDistance = std::max(usage.fs.clipDistance, arraySize);
      break;
    case BuiltInCullDistance:
      usage.fs.cullDistance = std::max(usage.fs.cullDistance, arraySize);
      break;
    case BuiltInPointCoord:
      usage.fs.pointCoord = true;
      // NOTE: gl_PointCoord is emulated via a general input. Those qualifiers therefore have to
      // be marked as used.
      usage.fs.smooth = true;
      if (getPipelineState()->getRasterizerState().perSampleShading)
        usage.fs.sample = true;
      else
        usage.fs.center = true;
      break;
    case BuiltInPrimitiveId:
      usage.fs.primitiveId = true;
      break;
    case BuiltInSampleId:
      usage.fs.sampleId = true;
      usage.fs.runAtSampleRate = true;
      break;
    case BuiltInSamplePosition:
      usage.fs.samplePosition = true;
      // NOTE: gl_SamplePosition is derived from gl_SampleID
      usage.fs.sampleId = true;
      usage.fs.runAtSampleRate = true;
      break;
    case BuiltInSampleMask:
      usage.fs.sampleMaskIn = true;
      break;
    case BuiltInLayer:
      usage.fs.layer = true;
      break;
    case BuiltInViewportIndex:
      usage.fs.viewportIndex = true;
      break;
    case BuiltInShadingRate:
      usage.fs.shadingRate = true;
      break;
    case BuiltInHelperInvocation:
      usage.fs.helperInvocation = true;
      break;
    case BuiltInViewIndex:
      usage.fs.viewIndex = true;
      break;
    case BuiltInBaryCoordNoPersp:
      usage.fs.baryCoordNoPersp = true;
      if (getPipelineState()->getOptions().enableInterpModePatch) {
        usage.fs.baryCoordNoPerspSample = true;
        builtIn = BuiltInBaryCoordNoPerspSample;
      }
      break;
    case BuiltInBaryCoordNoPerspCentroid:
      usage.fs.baryCoordNoPerspCentroid = true;
      break;
    case BuiltInBaryCoordNoPerspSample:
      usage.fs.baryCoordNoPerspSample = true;
      break;
    case BuiltInBaryCoordSmooth:
      usage.fs.baryCoordSmooth = true;
      if (getPipelineState()->getOptions().enableInterpModePatch) {
        usage.fs.baryCoordSmoothSample = true;
        builtIn = BuiltInBaryCoordSmoothSample;
      }
      break;
    case BuiltInBaryCoordSmoothCentroid:
      usage.fs.baryCoordSmoothCentroid = true;
      break;
    case BuiltInBaryCoordSmoothSample:
      usage.fs.baryCoordSmoothSample = true;
      break;
    case BuiltInBaryCoordPullModel:
      usage.fs.baryCoordPullModel = true;
      break;
    case BuiltInBaryCoord:
      usage.fs.baryCoord = true;
      break;
    case BuiltInBaryCoordNoPerspKHR:
      usage.fs.baryCoordNoPerspKHR = true;
      break;

    // Internal built-ins.
    case BuiltInInterpLinearCenter:
      usage.fs.noperspective = true;
      usage.fs.center = true;
      break;
    case BuiltInInterpPullMode:
      usage.fs.smooth = true;
      usage.fs.pullMode = true;
      break;
    case BuiltInSamplePosOffset:
      usage.fs.runAtSampleRate = true;
      break;

    default:
      break;
    }
    break;
  }

  default:
    break;
  }
}

// =====================================================================================================================
// Mark usage of a built-in output
//
// @param builtIn : Built-in ID
// @param arraySize : Number of array elements for ClipDistance and CullDistance. (Multiple calls to this function for
// this built-in might have different array sizes; we take the max)
// @param streamId : GS stream ID, or InvalidValue if not known
void BuilderImpl::markBuiltInOutputUsage(BuiltInKind builtIn, unsigned arraySize, unsigned streamId) {
  auto &usage = getPipelineState()->getShaderResourceUsage(m_shaderStage.value())->builtInUsage;
  assert((builtIn != BuiltInClipDistance && builtIn != BuiltInCullDistance) || arraySize != 0);
  switch (m_shaderStage.value()) {
  case ShaderStage::Vertex: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.vs.pointSize = true;
      break;
    case BuiltInPosition:
      usage.vs.position = true;
      break;
    case BuiltInClipDistance:
      usage.vs.clipDistance = std::max(usage.vs.clipDistance, arraySize);
      break;
    case BuiltInCullDistance:
      usage.vs.cullDistance = std::max(usage.vs.cullDistance, arraySize);
      break;
    case BuiltInViewportIndex:
      usage.vs.viewportIndex = true;
      break;
    case BuiltInLayer:
      usage.vs.layer = true;
      break;
    case BuiltInPrimitiveShadingRate:
      usage.vs.primitiveShadingRate = true;
      break;
    case BuiltInEdgeFlag:
      usage.vs.edgeFlag = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStage::TessControl: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.tcs.pointSize = true;
      break;
    case BuiltInPosition:
      usage.tcs.position = true;
      break;
    case BuiltInClipDistance:
      usage.tcs.clipDistance = std::max(usage.tcs.clipDistance, arraySize);
      break;
    case BuiltInCullDistance:
      usage.tcs.cullDistance = std::max(usage.tcs.cullDistance, arraySize);
      break;
    case BuiltInTessLevelOuter:
      usage.tcs.tessLevelOuter = true;
      break;
    case BuiltInTessLevelInner:
      usage.tcs.tessLevelInner = true;
      break;
    case BuiltInLayer:
      usage.tcs.layer = true;
      break;
    case BuiltInViewportIndex:
      usage.tcs.viewportIndex = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStage::TessEval: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.tes.pointSize = true;
      break;
    case BuiltInPosition:
      usage.tes.position = true;
      break;
    case BuiltInClipDistance:
      usage.tes.clipDistance = std::max(usage.tes.clipDistance, arraySize);
      break;
    case BuiltInCullDistance:
      usage.tes.cullDistance = std::max(usage.tes.cullDistance, arraySize);
      break;
    case BuiltInViewportIndex:
      usage.tes.viewportIndex = true;
      break;
    case BuiltInLayer:
      usage.tes.layer = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStage::Geometry: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.gs.pointSize = true;
      break;
    case BuiltInPosition:
      usage.gs.position = true;
      break;
    case BuiltInClipDistance:
      usage.gs.clipDistance = std::max(usage.gs.clipDistance, arraySize);
      break;
    case BuiltInCullDistance:
      usage.gs.cullDistance = std::max(usage.gs.cullDistance, arraySize);
      break;
    case BuiltInPrimitiveId:
      usage.gs.primitiveId = true;
      break;
    case BuiltInViewportIndex:
      usage.gs.viewportIndex = true;
      break;
    case BuiltInLayer:
      usage.gs.layer = true;
      break;
    case BuiltInPrimitiveShadingRate:
      usage.gs.primitiveShadingRate = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStage::Mesh: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.mesh.pointSize = true;
      break;
    case BuiltInPosition:
      usage.mesh.position = true;
      break;
    case BuiltInClipDistance:
      usage.mesh.clipDistance = std::max(usage.mesh.clipDistance, arraySize);
      break;
    case BuiltInCullDistance:
      usage.mesh.cullDistance = std::max(usage.mesh.cullDistance, arraySize);
      break;
    case BuiltInPrimitiveId:
      usage.mesh.primitiveId = true;
      break;
    case BuiltInViewportIndex:
      usage.mesh.viewportIndex = true;
      break;
    case BuiltInLayer:
      usage.mesh.layer = true;
      break;
    case BuiltInCullPrimitive:
      usage.mesh.cullPrimitive = true;
      break;
    case BuiltInPrimitiveShadingRate:
      usage.mesh.primitiveShadingRate = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStage::Fragment: {
    switch (builtIn) {
    case BuiltInFragDepth:
      usage.fs.fragDepth = true;
      break;
    case BuiltInSampleMask:
      usage.fs.sampleMask = true;
      break;
    case BuiltInFragStencilRef:
      usage.fs.fragStencilRef = true;
      break;
    default:
      break;
    }
    break;
  }

  default:
    break;
  }
}

#ifndef NDEBUG
// See BuiltInDefs.h for an explanation of the letter codes.
namespace {
namespace StageValidMask {
constexpr const ShaderStageMask C(ShaderStage::Compute);
constexpr const ShaderStageMask D(ShaderStage::TessEval);
constexpr const ShaderStageMask H(ShaderStage::TessControl);
constexpr const ShaderStageMask G(ShaderStage::Geometry);
constexpr const ShaderStageMask HD({ShaderStage::TessControl, ShaderStage::TessEval});
constexpr const ShaderStageMask HDG({ShaderStage::TessControl, ShaderStage::TessEval, ShaderStage::Geometry});
constexpr const ShaderStageMask HDGP({ShaderStage::TessControl, ShaderStage::TessEval, ShaderStage::Geometry,
                                      ShaderStage::Fragment});
constexpr const ShaderStageMask HG({ShaderStage::TessControl, ShaderStage::Geometry});
constexpr const ShaderStageMask M(ShaderStage::Mesh);
constexpr const ShaderStageMask MG({ShaderStage::Mesh, ShaderStage::Geometry});
constexpr const ShaderStageMask MVG({ShaderStage::Mesh, ShaderStage::Vertex, ShaderStage::Geometry});
constexpr const ShaderStageMask MVHDG({ShaderStage::Mesh, ShaderStage::Vertex, ShaderStage::TessControl,
                                       ShaderStage::TessEval, ShaderStage::Geometry});
constexpr const ShaderStageMask MVHDGP({ShaderStage::Mesh, ShaderStage::Vertex, ShaderStage::TessControl,
                                        ShaderStage::TessEval, ShaderStage::Geometry, ShaderStage::Fragment});
constexpr const ShaderStageMask N;
constexpr const ShaderStageMask P(ShaderStage::Fragment);
constexpr const ShaderStageMask TMC({ShaderStage::Task, ShaderStage::Mesh, ShaderStage::Compute});
constexpr const ShaderStageMask TMV({ShaderStage::Task, ShaderStage::Mesh, ShaderStage::Vertex});
constexpr const ShaderStageMask TMVHDGPC({ShaderStage::Task, ShaderStage::Mesh, ShaderStage::Vertex,
                                          ShaderStage::TessControl, ShaderStage::TessEval, ShaderStage::Geometry,
                                          ShaderStage::Fragment, ShaderStage::Compute});
constexpr const ShaderStageMask V(ShaderStage::Vertex);
} // namespace StageValidMask
} // anonymous namespace

// =====================================================================================================================
// Get a bitmask of which shader stages are valid for a built-in to be an input or output of
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param isOutput : True to get the mask for output rather than input
ShaderStageMask BuilderImpl::getBuiltInValidMask(BuiltInKind builtIn, bool isOutput) {
  ShaderStageMask validMaskIn;
  ShaderStageMask validMaskOut;
  switch (builtIn) {
#define BUILTIN(name, number, out, in, type)                                                                           \
  case BuiltIn##name:                                                                                                  \
    validMaskIn = StageValidMask::in;                                                                                  \
    validMaskOut = StageValidMask::out;                                                                                \
    break;
#include "lgc/BuiltInDefs.h"
#undef BUILTIN
  default:
    llvm_unreachable("Should never be called!");
    break;
  }
  return isOutput ? validMaskOut : validMaskIn;
}

// =====================================================================================================================
// Determine whether a built-in is an input for a particular shader stage.
//
// @param builtIn : Built-in type, one of the BuiltIn* constants
bool BuilderImpl::isBuiltInInput(BuiltInKind builtIn) {
  return getBuiltInValidMask(builtIn, false).contains(m_shaderStage.value());
}

// =====================================================================================================================
// Determine whether a built-in is an output for a particular shader stage.
//
// @param builtIn : Built-in type, one of the BuiltIn* constants
bool BuilderImpl::isBuiltInOutput(BuiltInKind builtIn) {
  return getBuiltInValidMask(builtIn, true).contains(m_shaderStage.value());
}
#endif
