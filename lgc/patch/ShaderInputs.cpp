/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ShaderInputs.cpp
 * @brief LGC source file: Handling of hardware-determined shader inputs (not user data, other than a few utility
 *        methods for special user data)
 ***********************************************************************************************************************
 */
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/ResourceUsage.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/BuilderBase.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Get name of a special user data value, one of the UserDataMapping values
//
// @param kind : The kind of special user data, a UserDataMapping enum value
const char *ShaderInputs::getSpecialUserDataName(UserDataMapping kind) {
  switch (kind) {
  case UserDataMapping::GlobalTable:
    return "GlobalTable";
  case UserDataMapping::SpillTable:
    return "SpillTable";
  case UserDataMapping::BaseVertex:
    return "BaseVertex";
  case UserDataMapping::BaseInstance:
    return "BaseInstance";
  case UserDataMapping::DrawIndex:
    return "DrawIndex";
  case UserDataMapping::Workgroup:
    return "Workgroup";
  case UserDataMapping::EsGsLdsSize:
    return "EsGsLdsSize";
  case UserDataMapping::ViewId:
    return "ViewId";
  case UserDataMapping::StreamOutTable:
    return "StreamOutTable";
  case UserDataMapping::VertexBufferTable:
    return "VertexBufferTable";
  case UserDataMapping::NggCullingData:
    return "NggCullingData";
  case UserDataMapping::MeshTaskDispatchDims:
    return "MeshTaskDispatchDims";
  case UserDataMapping::MeshTaskRingIndex:
    return "MeshTaskRingIndex";
  case UserDataMapping::MeshPipeStatsBuf:
    return "MeshPipeStatsBuf";
  case UserDataMapping::StreamOutControlBuf:
    return "StreamOutControlBuf";
  case UserDataMapping::ColorExportAddr:
    return "ColorExportAddr";
  default:
    return "";
  }
}

// =====================================================================================================================
// Get a special user data value by inserting a call to lgc.special.user.data
//
// @param kind : The kind of special user data, a UserDataMapping enum value
// @param builder : Builder to insert the call with
CallInst *ShaderInputs::getSpecialUserData(UserDataMapping kind, BuilderBase &builder) {
  Type *ty = builder.getInt32Ty();
  if (kind == UserDataMapping::NggCullingData)
    ty = builder.getInt64Ty();
  else if (kind == UserDataMapping::MeshTaskDispatchDims)
    ty = FixedVectorType::get(builder.getInt32Ty(), 3);
  else if (kind == UserDataMapping::Workgroup)
    ty = FixedVectorType::get(builder.getInt32Ty(), 3)->getPointerTo(ADDR_SPACE_CONST);
  return builder.CreateNamedCall((Twine(lgcName::SpecialUserData) + getSpecialUserDataName(kind)).str(), ty,
                                 builder.getInt32(static_cast<unsigned>(kind)), Attribute::ReadNone);
}

// =====================================================================================================================
// Get a special user data value as a pointer by inserting a call to lgc.special.user.data then extending it
//
// @param kind : The kind of special user data, a UserDataMapping enum value
// @param pointeeTy : Type that the pointer will point to
// @param builder : Builder to insert the call with
Value *ShaderInputs::getSpecialUserDataAsPointer(UserDataMapping kind, Type *pointeeTy, BuilderBase &builder) {
  Type *pointerTy = pointeeTy->getPointerTo(ADDR_SPACE_CONST);
  std::string callName = lgcName::SpecialUserData;
  callName += getSpecialUserDataName(kind);
  callName += ".";
  callName += getTypeName(pointerTy);
  Value *userDataValue = builder.CreateNamedCall(
      (Twine(lgcName::SpecialUserData) + getSpecialUserDataName(kind)).str(), pointerTy,
      {builder.getInt32(static_cast<unsigned>(kind)), builder.getInt32(HighAddrPc)}, Attribute::ReadNone);
  return builder.CreateIntToPtr(userDataValue, pointeeTy->getPointerTo(ADDR_SPACE_CONST));
}

// =====================================================================================================================
// Get VertexIndex
//
// @param builder : Builder to insert code with
Value *ShaderInputs::getVertexIndex(BuilderBase &builder, const LgcContext &lgcContext) {
  // VertexIndex = BaseVertex + VertexID
  Value *baseVertex = getSpecialUserData(UserDataMapping::BaseVertex, builder);
  Value *vertexId = getInput(ShaderInput::VertexId, builder, lgcContext);
  return builder.CreateAdd(baseVertex, vertexId, "VertexIndex");
}

// =====================================================================================================================
// Get InstanceIndex
//
// @param builder : Builder to insert code with
Value *ShaderInputs::getInstanceIndex(BuilderBase &builder, const LgcContext &lgcContext) {
  // InstanceIndex = BaseInstance + InstanceID
  Value *baseInstance = getSpecialUserData(UserDataMapping::BaseInstance, builder);
  Value *instanceId = getInput(ShaderInput::InstanceId, builder, lgcContext);
  return builder.CreateAdd(baseInstance, instanceId, "InstanceIndex");
}

// =====================================================================================================================
// Get a shader input value by inserting a call to lgc.shader.input
//
// @param kind : The kind of shader input, a ShaderInput enum value
// @param builder : Builder to insert code with
Value *ShaderInputs::getInput(ShaderInput kind, BuilderBase &builder, const LgcContext &lgcContext) {
  Type *ty = getInputType(kind, lgcContext);
  return builder.CreateNamedCall((Twine(lgcName::ShaderInput) + getInputName(kind)).str(), ty,
                                 builder.getInt32(static_cast<unsigned>(kind)), Attribute::ReadNone);
}

// =====================================================================================================================
// Get IR type of a particular shader input
//
// @param kind : The kind of shader input, a ShaderInput enum value
// @param context : LLVM context for getting types
Type *ShaderInputs::getInputType(ShaderInput inputKind, const LgcContext &lgcContext) {
  LLVMContext &context = lgcContext.getContext();
  switch (inputKind) {
  case ShaderInput::WorkgroupId:
    return FixedVectorType::get(Type::getInt32Ty(context), 3);
  case ShaderInput::WorkgroupId2:
    return FixedVectorType::get(Type::getInt32Ty(context), 2);
  case ShaderInput::WorkgroupId1:
    return Type::getInt32Ty(context);
  case ShaderInput::LocalInvocationId:
    if (lgcContext.getTargetInfo().getGfxIpVersion().major >= 11)
      return Type::getInt32Ty(context);
    return FixedVectorType::get(Type::getInt32Ty(context), 3);

  case ShaderInput::TessCoordX:
  case ShaderInput::TessCoordY:
  case ShaderInput::FragCoordX:
  case ShaderInput::FragCoordY:
  case ShaderInput::FragCoordZ:
  case ShaderInput::FragCoordW:
  case ShaderInput::LineStipple:
    return Type::getFloatTy(context);

  case ShaderInput::PerspInterpPullMode:
    return FixedVectorType::get(Type::getFloatTy(context), 3);

  case ShaderInput::PerspInterpSample:
  case ShaderInput::PerspInterpCenter:
  case ShaderInput::PerspInterpCentroid:
  case ShaderInput::LinearInterpSample:
  case ShaderInput::LinearInterpCenter:
  case ShaderInput::LinearInterpCentroid:
    return FixedVectorType::get(Type::getFloatTy(context), 2);

  default:
    return Type::getInt32Ty(context);
  }
}

// =====================================================================================================================
// Get name of shader input
//
// @param kind : The kind of shader input, a ShaderInput enum value
const char *ShaderInputs::getInputName(ShaderInput inputKind) {
  switch (inputKind) {
  case ShaderInput::WorkgroupId:
    return "WorkgroupId";
  case ShaderInput::WorkgroupId2:
    return "WorkgroupId2";
  case ShaderInput::WorkgroupId1:
    return "WorkgroupId1";
  case ShaderInput::MultiDispatchInfo:
    return "MultiDispatchInfo";
  case ShaderInput::PrimMask:
    return "PrimMask";
  case ShaderInput::OffChipLdsBase:
    return "OffChipLdsBase";
  case ShaderInput::StreamOutInfo:
    return "StreamOutInfo";
  case ShaderInput::StreamOutWriteIndex:
    return "StreamOutWriteIndex";
  case ShaderInput::StreamOutOffset0:
    return "StreamOutOffset0";
  case ShaderInput::StreamOutOffset1:
    return "StreamOutOffset1";
  case ShaderInput::StreamOutOffset2:
    return "StreamOutOffset2";
  case ShaderInput::StreamOutOffset3:
    return "StreamOutOffset3";
  case ShaderInput::GsVsOffset:
    return "GsVsOffset";
  case ShaderInput::GsWaveId:
    return "GsWaveId";
  case ShaderInput::IsOffChip:
    return "IsOffChip";
  case ShaderInput::EsGsOffset:
    return "EsGsOffset";
  case ShaderInput::TfBufferBase:
    return "TfBufferBase";
  case ShaderInput::VertexId:
    return "VertexId";
  case ShaderInput::RelVertexId:
    return "RelVertexId";
  case ShaderInput::PrimitiveId:
    return "PrimitiveId";
  case ShaderInput::InstanceId:
    return "InstanceId";
  case ShaderInput::PatchId:
    return "PatchId";
  case ShaderInput::RelPatchId:
    return "RelPatchId";
  case ShaderInput::TessCoordX:
    return "TessCoordX";
  case ShaderInput::TessCoordY:
    return "TessCoordY";
  case ShaderInput::EsGsOffset0:
    return "EsGsOffset0";
  case ShaderInput::EsGsOffset1:
    return "EsGsOffset1";
  case ShaderInput::GsPrimitiveId:
    return "GsPrimitiveId";
  case ShaderInput::EsGsOffset2:
    return "EsGsOffset2";
  case ShaderInput::EsGsOffset3:
    return "EsGsOffset3";
  case ShaderInput::EsGsOffset4:
    return "EsGsOffset4";
  case ShaderInput::EsGsOffset5:
    return "EsGsOffset5";
  case ShaderInput::GsInstanceId:
    return "GsInstanceId";
  case ShaderInput::PerspInterpSample:
    return "PerspInterpSample";
  case ShaderInput::PerspInterpCenter:
    return "PerspInterpCenter";
  case ShaderInput::PerspInterpCentroid:
    return "PerspInterpCentroid";
  case ShaderInput::PerspInterpPullMode:
    return "PerspInterpPullMode";
  case ShaderInput::LinearInterpSample:
    return "LinearInterpSample";
  case ShaderInput::LinearInterpCenter:
    return "LinearInterpCenter";
  case ShaderInput::LinearInterpCentroid:
    return "LinearInterpCentroid";
  case ShaderInput::LineStipple:
    return "LineStipple";
  case ShaderInput::FragCoordX:
    return "FragCoordX";
  case ShaderInput::FragCoordY:
    return "FragCoordY";
  case ShaderInput::FragCoordZ:
    return "FragCoordZ";
  case ShaderInput::FragCoordW:
    return "FragCoordW";
  case ShaderInput::FrontFacing:
    return "FrontFacing";
  case ShaderInput::Ancillary:
    return "Ancillary";
  case ShaderInput::SampleCoverage:
    return "SampleCoverage";
  case ShaderInput::FixedXY:
    return "FixedXY";
  case ShaderInput::LocalInvocationId:
    return "LocalInvocationId";
  default:
    llvm_unreachable("Unknown shader input kind");
  }
}

// =====================================================================================================================
// Gather usage of shader inputs from before PatchEntryPointMutate
//
// @param module : IR module
void ShaderInputs::gatherUsage(Module &module) {
  for (auto &func : module) {
    if (!func.isDeclaration() || !func.getName().startswith(lgcName::ShaderInput))
      continue;
    for (User *user : func.users()) {
      CallInst *call = cast<CallInst>(user);
      ShaderStage stage = getShaderStage(call->getFunction());
      assert(stage != ShaderStageCopyShader);
      getShaderInputUsage(stage, static_cast<ShaderInput>(cast<ConstantInt>(call->getArgOperand(0))->getZExtValue()))
          ->users.push_back(call);
    }
  }
}

// =====================================================================================================================
// Fix up uses of shader inputs to use entry args directly
//
// @param module : IR module
// @param computeWithIndirectCall : whether compute shader has indirect call
void ShaderInputs::fixupUses(Module &module, PipelineState *pipelineState, bool computeWithIndirectCall) {
  // For each function definition...
  for (Function &func : module) {
    if (func.isDeclaration())
      continue;

    ShaderStage stage = getShaderStage(&func);
    ShaderInputsUsage *inputsUsage = getShaderInputsUsage(stage);

    // Use for compute shader
    bool useWorkgroupIds[3] = {false};
    bool useWholeWorkgroupId = false;
    bool needCheckWorkgroupId = false;

    for (unsigned kind = 0; kind != static_cast<unsigned>(ShaderInput::Count); ++kind) {
      ShaderInputUsage *inputUsage = inputsUsage->inputs[kind].get();
      if (!inputUsage)
        continue;
      Value *value = nullptr;
      {
        if (inputUsage->entryArgIdx != 0)
          value = getFunctionArgument(&func, inputUsage->entryArgIdx);
        else
          continue;
      }

      auto inputKind = static_cast<ShaderInput>(kind);
      value->setName(getInputName(inputKind));
      // We can possibly optimize workgroupId generation for entry-point compute shader.
      needCheckWorkgroupId = (inputKind == ShaderInput::WorkgroupId) &&
                             (func.getCallingConv() == CallingConv::AMDGPU_CS) && !computeWithIndirectCall;
      for (Instruction *&call : inputUsage->users) {
        if (call && call->getFunction() == &func) {
          if (needCheckWorkgroupId) {
            for (auto user : call->users()) {
              if (auto extractInst = dyn_cast<ExtractElementInst>(user)) {
                if (auto indexInst = dyn_cast<ConstantInt>(extractInst->getIndexOperand())) {
                  unsigned index = indexInst->getZExtValue();
                  assert(index < 3);
                  useWorkgroupIds[index] = true;
                  continue;
                }
              }

              useWholeWorkgroupId = true;
              break;
            }
          }
          call->replaceAllUsesWith(value);
          call->eraseFromParent();
          call = nullptr;
        }
      }

      // The new ShaderInputs scheme means that InOutBuilder or PatchResourceCollect no longer needs to set
      // the builtInUsage field for an input that is generated using ShaderInputs::getInput() and/or
      // ShaderInputs::getSpecialUserData() (before PatchEntryPointMutate), and we can remove that
      // builtInUsage field.
      //
      // However, in some cases, the builtInUsage field is used in NggPrimShader and/or Gfx*ConfigBuilder
      // (both run later on) to tell that the input is in use. For those cases, we must keep the builtInUsage
      // field, and set it here.
      // Add code here as built-ins are moved from PatchInOutImportExport to InOutBuilder.
      auto &builtInUsage = pipelineState->getShaderResourceUsage(stage)->builtInUsage;
      switch (stage) {
      case ShaderStageVertex:
        switch (static_cast<ShaderInput>(kind)) {
        case ShaderInput::VertexId:
          // Tell NggPrimShader to copy VertexId through LDS.
          builtInUsage.vs.vertexIndex = true;
          break;
        case ShaderInput::InstanceId:
          // Tell NggPrimShader to copy InstanceId through LDS, and tell Gfx*ConfigBuilder to set
          // SPI_SHADER_PGM_RSRC1_VS.VGPR_COMP_CNT to enable it.
          builtInUsage.vs.instanceIndex = true;
          break;
        default:
          break;
        }
        break;
      default:
        break;
      }
    }
    if (needCheckWorkgroupId && !useWholeWorkgroupId) {
      if (!useWorkgroupIds[0])
        func.addFnAttr("amdgpu-no-workgroup-id-x");
      if (!useWorkgroupIds[1])
        func.addFnAttr("amdgpu-no-workgroup-id-y");
      if (!useWorkgroupIds[2])
        func.addFnAttr("amdgpu-no-workgroup-id-z");
    }
  }
}

// =====================================================================================================================
// Tables of shader inputs for the various shader stages, separately for SGPRs and VGPRs
// This represents a kind of idealized GFX8 model, before GFX9+ shader merging or GFX10+ NGG.

struct ShaderInputDesc {
  ShaderInput inputKind; // Shader input kind
  size_t entryArgIdx;    // Offset in InterfaceData struct to store entry arg index, else 0
  bool always;           // True if this register is always added as an argument; false to check usage
};

// SGPRs: Task (identical to CS)
static const ShaderInputDesc TaskSgprInputs[] = {
    {ShaderInput::WorkgroupId, offsetof(InterfaceData, entryArgIdxs.task.workgroupId), true},
    {ShaderInput::MultiDispatchInfo, offsetof(InterfaceData, entryArgIdxs.task.multiDispatchInfo), true},
};

// SGPRs: VS as hardware ES
static const ShaderInputDesc VsAsEsSgprInputs[] = {
    {ShaderInput::EsGsOffset, offsetof(InterfaceData, entryArgIdxs.vs.esGsOffset), true},
};

// SGPRs: VS as hardware VS
static const ShaderInputDesc VsAsVsSgprInputs[] = {
    {ShaderInput::StreamOutInfo, offsetof(InterfaceData, entryArgIdxs.vs.streamOutData.streamInfo)},
    {ShaderInput::StreamOutWriteIndex, offsetof(InterfaceData, entryArgIdxs.vs.streamOutData.writeIndex)},
    {ShaderInput::StreamOutOffset0, offsetof(InterfaceData, entryArgIdxs.vs.streamOutData.streamOffsets[0])},
    {ShaderInput::StreamOutOffset1, offsetof(InterfaceData, entryArgIdxs.vs.streamOutData.streamOffsets[1])},
    {ShaderInput::StreamOutOffset2, offsetof(InterfaceData, entryArgIdxs.vs.streamOutData.streamOffsets[2])},
    {ShaderInput::StreamOutOffset3, offsetof(InterfaceData, entryArgIdxs.vs.streamOutData.streamOffsets[3])},
};

// SGPRs: TES as hardware ES
static const ShaderInputDesc TesAsEsSgprInputs[] = {
    {ShaderInput::OffChipLdsBase, offsetof(InterfaceData, entryArgIdxs.tes.offChipLdsBase)},
    {ShaderInput::IsOffChip, 0},
    {ShaderInput::EsGsOffset, offsetof(InterfaceData, entryArgIdxs.tes.esGsOffset), true},
};

// SGPRs: TES as hardware VS
static const ShaderInputDesc TesAsVsSgprInputs[] = {
    {ShaderInput::StreamOutInfo, offsetof(InterfaceData, entryArgIdxs.tes.streamOutData.streamInfo)},
    {ShaderInput::StreamOutWriteIndex, offsetof(InterfaceData, entryArgIdxs.tes.streamOutData.writeIndex)},
    {ShaderInput::StreamOutOffset0, offsetof(InterfaceData, entryArgIdxs.tes.streamOutData.streamOffsets[0])},
    {ShaderInput::StreamOutOffset1, offsetof(InterfaceData, entryArgIdxs.tes.streamOutData.streamOffsets[1])},
    {ShaderInput::StreamOutOffset2, offsetof(InterfaceData, entryArgIdxs.tes.streamOutData.streamOffsets[2])},
    {ShaderInput::StreamOutOffset3, offsetof(InterfaceData, entryArgIdxs.tes.streamOutData.streamOffsets[3])},
    {ShaderInput::OffChipLdsBase, offsetof(InterfaceData, entryArgIdxs.tes.offChipLdsBase)},
};

// SGPRs: TCS
static const ShaderInputDesc TcsSgprInputs[] = {
    {ShaderInput::OffChipLdsBase, offsetof(InterfaceData, entryArgIdxs.tcs.offChipLdsBase)},
    {ShaderInput::TfBufferBase, offsetof(InterfaceData, entryArgIdxs.tcs.tfBufferBase), true},
};

// SGPRs: GS
static const ShaderInputDesc GsSgprInputs[] = {
    {ShaderInput::GsVsOffset, offsetof(InterfaceData, entryArgIdxs.gs.gsVsOffset), true},
    {ShaderInput::GsWaveId, offsetof(InterfaceData, entryArgIdxs.gs.gsWaveId), true},
};

// SGPRs: FS
static const ShaderInputDesc FsSgprInputs[] = {
    {ShaderInput::PrimMask, offsetof(InterfaceData, entryArgIdxs.fs.primMask), true},
};

// SGPRs: CS
static ShaderInputDesc CsSgprInputs[] = {
    {ShaderInput::WorkgroupId, offsetof(InterfaceData, entryArgIdxs.cs.workgroupId), true},
    {ShaderInput::WorkgroupId2, offsetof(InterfaceData, entryArgIdxs.cs.workgroupId)},
    {ShaderInput::WorkgroupId1, offsetof(InterfaceData, entryArgIdxs.cs.workgroupId)},
    {ShaderInput::MultiDispatchInfo, offsetof(InterfaceData, entryArgIdxs.cs.multiDispatchInfo), true},
};

// VGPRs: Task (identical to CS)
static const ShaderInputDesc TaskVgprInputs[] = {
    {ShaderInput::LocalInvocationId, offsetof(InterfaceData, entryArgIdxs.task.localInvocationId), true},
};

// VGPRs: VS
static const ShaderInputDesc VsVgprInputs[] = {
    {ShaderInput::VertexId, offsetof(InterfaceData, entryArgIdxs.vs.vertexId), true},
    {ShaderInput::RelVertexId, offsetof(InterfaceData, entryArgIdxs.vs.relVertexId), true},
    {ShaderInput::PrimitiveId, offsetof(InterfaceData, entryArgIdxs.vs.primitiveId), true},
    {ShaderInput::InstanceId, offsetof(InterfaceData, entryArgIdxs.vs.instanceId), true},
};

// VGPRs: TCS
static const ShaderInputDesc TcsVgprInputs[] = {
    {ShaderInput::PatchId, offsetof(InterfaceData, entryArgIdxs.tcs.patchId), true},
    {ShaderInput::RelPatchId, offsetof(InterfaceData, entryArgIdxs.tcs.relPatchId), true},
};

// VGPRs: TES
static const ShaderInputDesc TesVgprInputs[] = {
    {ShaderInput::TessCoordX, offsetof(InterfaceData, entryArgIdxs.tes.tessCoordX), true},
    {ShaderInput::TessCoordY, offsetof(InterfaceData, entryArgIdxs.tes.tessCoordY), true},
    {ShaderInput::RelPatchId, offsetof(InterfaceData, entryArgIdxs.tes.relPatchId), true},
    {ShaderInput::PatchId, offsetof(InterfaceData, entryArgIdxs.tes.patchId), true},
};

// VGPRs: GS
static const ShaderInputDesc GsVgprInputs[] = {
    {ShaderInput::EsGsOffset0, offsetof(InterfaceData, entryArgIdxs.gs.esGsOffsets[0]), true},
    {ShaderInput::EsGsOffset1, offsetof(InterfaceData, entryArgIdxs.gs.esGsOffsets[1]), true},
    {ShaderInput::GsPrimitiveId, offsetof(InterfaceData, entryArgIdxs.gs.primitiveId), true},
    {ShaderInput::EsGsOffset2, offsetof(InterfaceData, entryArgIdxs.gs.esGsOffsets[2]), true},
    {ShaderInput::EsGsOffset3, offsetof(InterfaceData, entryArgIdxs.gs.esGsOffsets[3]), true},
    {ShaderInput::EsGsOffset4, offsetof(InterfaceData, entryArgIdxs.gs.esGsOffsets[4]), true},
    {ShaderInput::EsGsOffset5, offsetof(InterfaceData, entryArgIdxs.gs.esGsOffsets[5]), true},
    {ShaderInput::GsInstanceId, offsetof(InterfaceData, entryArgIdxs.gs.invocationId), true},
};

// VGPRs: FS
static const ShaderInputDesc FsVgprInputs[] = {
    {ShaderInput::PerspInterpSample, offsetof(InterfaceData, entryArgIdxs.fs.perspInterp.sample), true},
    {ShaderInput::PerspInterpCenter, offsetof(InterfaceData, entryArgIdxs.fs.perspInterp.center), true},
    {ShaderInput::PerspInterpCentroid, offsetof(InterfaceData, entryArgIdxs.fs.perspInterp.centroid), true},
    {ShaderInput::PerspInterpPullMode, offsetof(InterfaceData, entryArgIdxs.fs.perspInterp.pullMode), true},
    {ShaderInput::LinearInterpSample, offsetof(InterfaceData, entryArgIdxs.fs.linearInterp.sample), true},
    {ShaderInput::LinearInterpCenter, offsetof(InterfaceData, entryArgIdxs.fs.linearInterp.center), true},
    {ShaderInput::LinearInterpCentroid, offsetof(InterfaceData, entryArgIdxs.fs.linearInterp.centroid), true},
    {ShaderInput::LineStipple, 0, true},
    {ShaderInput::FragCoordX, offsetof(InterfaceData, entryArgIdxs.fs.fragCoord.x), true},
    {ShaderInput::FragCoordY, offsetof(InterfaceData, entryArgIdxs.fs.fragCoord.y), true},
    {ShaderInput::FragCoordZ, offsetof(InterfaceData, entryArgIdxs.fs.fragCoord.z), true},
    {ShaderInput::FragCoordW, offsetof(InterfaceData, entryArgIdxs.fs.fragCoord.w), true},
    {ShaderInput::FrontFacing, offsetof(InterfaceData, entryArgIdxs.fs.frontFacing), true},
    {ShaderInput::Ancillary, offsetof(InterfaceData, entryArgIdxs.fs.ancillary), true},
    {ShaderInput::SampleCoverage, offsetof(InterfaceData, entryArgIdxs.fs.sampleCoverage), true},
    {ShaderInput::FixedXY, 0, true},
};

// VGPRs: CS
static const ShaderInputDesc CsVgprInputs[] = {
    {ShaderInput::LocalInvocationId, offsetof(InterfaceData, entryArgIdxs.cs.localInvocationId), true},
};

// =====================================================================================================================
// Get argument types for shader inputs
//
// @param pipelineState : Pipeline state
// @param shaderStage : Shader stage
// @param origFunc : The original entry point function
// @param isComputeWithCalls : Whether this is compute pipeline with calls or compute library
// @param [in/out] argTys : Argument types vector to add to
// @param [in/out] argNames : Argument names vector to add to
// @returns : Bitmap with bits set for SGPR arguments so caller can set "inreg" attribute on the args
uint64_t ShaderInputs::getShaderArgTys(PipelineState *pipelineState, ShaderStage shaderStage, Function *origFunc,
                                       bool isComputeWithCalls, SmallVectorImpl<Type *> &argTys,
                                       SmallVectorImpl<std::string> &argNames, unsigned argOffset) {

  bool hasTs = pipelineState->hasShaderStage(ShaderStageTessControl);
  bool hasGs = pipelineState->hasShaderStage(ShaderStageGeometry);

  uint64_t inRegMask = 0;
  auto intfData = pipelineState->getShaderInterfaceData(shaderStage);
  const auto &xfbStrides = pipelineState->getXfbBufferStrides();
  // NOTE: For GFX11+, stream-out is actually SW-emulated in NGG primitive shader and we doesn't have relevant shader
  // inputs for HW stream-out.
  const unsigned gfxIp = pipelineState->getTargetInfo().getGfxIpVersion().major;
  const bool enableHwXfb = pipelineState->enableXfb() && gfxIp <= 10;

  // Enable optional shader inputs as required.
  switch (shaderStage) {
  case ShaderStageVertex:
    if (enableHwXfb && (!hasGs && !hasTs)) {
      // HW stream-out in VS as hardware VS
      getShaderInputUsage(shaderStage, ShaderInput::StreamOutInfo)->enable();
      getShaderInputUsage(shaderStage, ShaderInput::StreamOutWriteIndex)->enable();
      for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
        if (xfbStrides[i] > 0)
          getShaderInputUsage(shaderStage, static_cast<unsigned>(ShaderInput::StreamOutOffset0) + i)->enable();
      }
    }
    if (pipelineState->getPalMetadata()->getVertexFetchCount() != 0) {
      // This is an unlinked compile that will need a fetch shader. We need to add the vertex ID and
      // instance ID, even if they appear unused here.
      getShaderInputUsage(shaderStage, ShaderInput::VertexId)->enable();
      getShaderInputUsage(shaderStage, ShaderInput::InstanceId)->enable();
    }
    break;
  case ShaderStageTessControl:
    if (pipelineState->isTessOffChip()) {
      // LDS buffer base for off-chip
      getShaderInputUsage(shaderStage, ShaderInput::OffChipLdsBase)->enable();
    }
    break;
  case ShaderStageTessEval:
    if (pipelineState->isTessOffChip()) {
      // LDS buffer base for off-chip
      getShaderInputUsage(shaderStage, ShaderInput::OffChipLdsBase)->enable();
      // is_off_chip register. Enabling it here only has an effect when TES is hardware ES.
      getShaderInputUsage(shaderStage, ShaderInput::IsOffChip)->enable();
    }
    if (!hasGs) {
      // TES as hardware VS: handle HW stream-out. StreamOutInfo is required for off-chip even if there is no
      // stream-out.
      if (pipelineState->isTessOffChip() || enableHwXfb)
        getShaderInputUsage(shaderStage, ShaderInput::StreamOutInfo)->enable();
      if (enableHwXfb) {
        getShaderInputUsage(shaderStage, ShaderInput::StreamOutWriteIndex)->enable();
        for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
          if (xfbStrides[i] > 0)
            getShaderInputUsage(shaderStage, static_cast<unsigned>(ShaderInput::StreamOutOffset0) + i)->enable();
        }
      }
    }
    break;
  default:
    break;
  }

  const unsigned id = static_cast<unsigned>(ShaderInput::WorkgroupId);
  CsSgprInputs[id].always = true;
  if (shaderStage == ShaderStageCompute && !isComputeWithCalls && origFunc &&
      pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
    CsSgprInputs[id].always = false;
    tryOptimizeWorkgroupId(pipelineState, shaderStage, origFunc);
  }

  // Determine which of the tables above to use for SGPR and VGPR inputs.
  ArrayRef<ShaderInputDesc> sgprInputDescs;
  ArrayRef<ShaderInputDesc> vgprInputDescs;

  switch (shaderStage) {
  case ShaderStageTask:
    sgprInputDescs = TaskSgprInputs;
    vgprInputDescs = TaskVgprInputs;
    break;
  case ShaderStageVertex:
    if (!hasTs) {
      if (hasGs)
        sgprInputDescs = VsAsEsSgprInputs;
      else
        sgprInputDescs = VsAsVsSgprInputs;
    }
    vgprInputDescs = VsVgprInputs;
    break;
  case ShaderStageTessControl:
    sgprInputDescs = TcsSgprInputs;
    vgprInputDescs = TcsVgprInputs;
    break;
  case ShaderStageTessEval:
    if (hasGs)
      sgprInputDescs = TesAsEsSgprInputs;
    else
      sgprInputDescs = TesAsVsSgprInputs;
    vgprInputDescs = TesVgprInputs;
    break;
  case ShaderStageGeometry:
    sgprInputDescs = GsSgprInputs;
    vgprInputDescs = GsVgprInputs;
    break;
  case ShaderStageMesh:
    // NOTE: Mesh shader is finally mapped to HW GS in fast launch mode. Therefore, we don't add SGPR and VGPR inputs
    // here. Instead, this is deferred to mesh shader lowering in later phase.
    break;
  case ShaderStageFragment:
    sgprInputDescs = FsSgprInputs;
    vgprInputDescs = FsVgprInputs;
    break;
  case ShaderStageCompute:
    sgprInputDescs = CsSgprInputs;
    vgprInputDescs = CsVgprInputs;
    break;
  default:
    llvm_unreachable("Invalid shader stage");
  }

  // Add the type of each used shader input.
  ArrayRef<ShaderInputDesc> inputDescs = sgprInputDescs;
  for (unsigned doingVgprs = 0; doingVgprs != 2; ++doingVgprs) {
    for (const auto &inputDesc : inputDescs) {
      // Get the ShaderInputUsage for this input, if it exists.
      ShaderInputsUsage *inputsUsage = getShaderInputsUsage(shaderStage);
      assert(inputDesc.inputKind < ShaderInput::Count);
      ShaderInputUsage *inputUsage = inputsUsage->inputs[static_cast<unsigned>(inputDesc.inputKind)].get();
      // We don't want this input if it is not marked "always" and it is not used.
      if (!inputDesc.always && (!inputUsage || inputUsage->users.empty()))
        continue;
      if (!doingVgprs) {
        // Mark this arg as "inreg" to get it into an SGPR.
        inRegMask |= 1ull << argTys.size();
      }
      // Store the argument index.
      if (inputDesc.entryArgIdx != 0)
        *reinterpret_cast<unsigned *>((reinterpret_cast<char *>(intfData) + inputDesc.entryArgIdx)) =
            argTys.size() + argOffset;
      if (inputUsage)
        inputUsage->entryArgIdx = argTys.size() + argOffset;
      // Add the argument type.
      argTys.push_back(getInputType(inputDesc.inputKind, *pipelineState->getLgcContext()));
      argNames.push_back(ShaderInputs::getInputName(inputDesc.inputKind));
    }
    inputDescs = vgprInputDescs;
  }

  return inRegMask;
}

// =====================================================================================================================
// Get ShaderInputsUsage struct for the given shader stage
//
// @param stage : Shader stage
ShaderInputs::ShaderInputsUsage *ShaderInputs::getShaderInputsUsage(ShaderStage stage) {
  m_shaderInputsUsage.resize(std::max(m_shaderInputsUsage.size(), static_cast<size_t>(stage) + 1));
  return &m_shaderInputsUsage[stage];
}

// =====================================================================================================================
// Get (create if necessary) ShaderInputUsage struct for the given system shader input in the given shader stage
//
// @param stage : Shader stage
// @param inputKind : ShaderInput enum value for the shader input
ShaderInputs::ShaderInputUsage *ShaderInputs::getShaderInputUsage(ShaderStage stage, unsigned inputKind) {
  ShaderInputsUsage *inputsUsage = getShaderInputsUsage(stage);
  if (!inputsUsage->inputs[inputKind])
    inputsUsage->inputs[inputKind] = std::make_unique<ShaderInputUsage>();
  return &*inputsUsage->inputs[inputKind];
}

// =====================================================================================================================
// Try to optimize to use the accurate workgroupID arguments and set the function attribute for corresponding
// amdgpu-no-workgroup-id-*. If partial components of WorkgroupID are used, the argument type will be aligned and the
// relevant users will be re-targeted to the new generated lgc.shader.input.workgroupId.*. The related users of <3xi32>
// workgroupId will be removed. We set amdgpu-no-workgroup-id-* function attribute to control the register tgid_*_en
// later.
//
// @param pipelineState : Pipeline state
// @param shaderStage : Shader stage
// @param origFunc : The original entry point function
void ShaderInputs::tryOptimizeWorkgroupId(PipelineState *pipelineState, ShaderStage shaderStage, Function *origFunc) {
  assert(shaderStage == ShaderStageCompute);
  bool useWholeWorkgroupId = false;
  SmallVector<Instruction *> extractVec3[3];
  SmallVector<Instruction *> workgroupIdCallInsts;
  unsigned kindId = static_cast<unsigned>(ShaderInput::WorkgroupId);
  ShaderInputUsage *inputUsage = getShaderInputsUsage(shaderStage)->inputs[kindId].get();

  unsigned usedCompCount = 0;
  // Check if the whole or partial components are used
  if (inputUsage) {
    for (Instruction *&call : inputUsage->users) {
      if (!call)
        continue;
      for (auto user : call->users()) {
        if (auto extractInst = dyn_cast<ExtractElementInst>(user)) {
          if (auto indexInst = dyn_cast<ConstantInt>(extractInst->getIndexOperand())) {
            unsigned index = indexInst->getZExtValue();
            assert(index < 3);
            extractVec3[index].push_back(extractInst);
            continue;
          }
        }
        useWholeWorkgroupId = true;
        break;
      }
      if (!useWholeWorkgroupId)
        workgroupIdCallInsts.push_back(call);
    }
    if (!useWholeWorkgroupId) {
      for (auto insts : extractVec3) {
        if (!insts.empty())
          ++usedCompCount;
      }
    }
    if (usedCompCount == 3)
      useWholeWorkgroupId = true;
  }
  if (useWholeWorkgroupId)
    return;

  if (extractVec3[0].empty())
    origFunc->addFnAttr("amdgpu-no-workgroup-id-x");
  if (extractVec3[1].empty())
    origFunc->addFnAttr("amdgpu-no-workgroup-id-y");
  if (extractVec3[2].empty())
    origFunc->addFnAttr("amdgpu-no-workgroup-id-z");

  if (!inputUsage)
    return;

  BuilderBase builder(pipelineState->getContext());
  builder.SetInsertPoint(workgroupIdCallInsts.front());
  // Clear the original default <3xi32> workgroupId
  inputUsage->users.clear();
  inputUsage = nullptr;

  if (usedCompCount == 1) {
    // The processing of using one component
    auto workgroupId1 =
        static_cast<CallInst *>(getInput(ShaderInput::WorkgroupId1, builder, *pipelineState->getLgcContext()));
    getShaderInputUsage(shaderStage, ShaderInput::WorkgroupId1)->users.push_back(workgroupId1);

    for (auto instSet : extractVec3) {
      for (auto inst : instSet) {
        inst->replaceAllUsesWith(workgroupId1);
        inst->eraseFromParent();
      }
    }
  } else if (usedCompCount == 2) {
    // The processing of using two components
    auto workgroupId2 =
        static_cast<CallInst *>(getInput(ShaderInput::WorkgroupId2, builder, *pipelineState->getLgcContext()));
    getShaderInputUsage(shaderStage, ShaderInput::WorkgroupId2)->users.push_back(workgroupId2);

    Value *extractVec2[2] = {builder.CreateExtractElement(workgroupId2, static_cast<uint64_t>(0)),
                             builder.CreateExtractElement(workgroupId2, 1)};
    unsigned index = 0;
    for (auto instSet : extractVec3) {
      for (auto inst : instSet) {
        inst->replaceAllUsesWith(extractVec2[index]);
        inst->eraseFromParent();
      }
      if (!instSet.empty())
        ++index;
    }
  } else {
    assert(usedCompCount == 0);
  }

  for (auto call : workgroupIdCallInsts)
    call->eraseFromParent();
}
