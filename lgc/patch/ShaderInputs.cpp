/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/state/PipelineState.h"
#include "lgc/state/ResourceUsage.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Get IR type of a particular shader input
//
// @param kind : The kind of shader input, a ShaderInput enum value
// @param context : LLVM context for getting types
Type *ShaderInputs::getInputType(ShaderInput inputKind, LLVMContext &context) {
  switch (inputKind) {
  case ShaderInput::WorkgroupId:
  case ShaderInput::LocalInvocationId:
    return VectorType::get(Type::getInt32Ty(context), 3);

  case ShaderInput::TessCoordX:
  case ShaderInput::TessCoordY:
  case ShaderInput::FragCoordX:
  case ShaderInput::FragCoordY:
  case ShaderInput::FragCoordZ:
  case ShaderInput::FragCoordW:
  case ShaderInput::LineStipple:
    return Type::getFloatTy(context);

  case ShaderInput::PerspInterpPullMode:
    return VectorType::get(Type::getFloatTy(context), 3);

  case ShaderInput::PerspInterpSample:
  case ShaderInput::PerspInterpCenter:
  case ShaderInput::PerspInterpCentroid:
  case ShaderInput::LinearInterpSample:
  case ShaderInput::LinearInterpCenter:
  case ShaderInput::LinearInterpCentroid:
    return VectorType::get(Type::getFloatTy(context), 2);

  default:
    return Type::getInt32Ty(context);
  }
}

// =====================================================================================================================
// Gather usage of shader inputs from before PatchEntryPointMutate
//
// @param module : IR module
void ShaderInputs::gatherUsage(Module &module) {
}

// =====================================================================================================================
// Fix up uses of shader inputs to use entry args directly
//
// @param module : IR module
void ShaderInputs::fixupUses(Module &module) {
}

// =====================================================================================================================
// Tables of shader inputs for the various shader stages, separately for SGPRs and VGPRs
// This represents a kind of idealized GFX8 model, before GFX9+ shader merging or GFX10+ NGG.

struct ShaderInputDesc {
  ShaderInput inputKind; // Shader input kind
  size_t entryArgIdx;    // Offset in InterfaceData struct to store entry arg index, else 0
  bool always;           // True if this register is always added as an argument; false to check usage
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
    {ShaderInput::GsWaveId, offsetof(InterfaceData, entryArgIdxs.gs.waveId), true},
};

// SGPRs: FS
static const ShaderInputDesc FsSgprInputs[] = {
    {ShaderInput::PrimMask, offsetof(InterfaceData, entryArgIdxs.fs.primMask), true},
};

// SGPRs: CS
static const ShaderInputDesc CsSgprInputs[] = {
    {ShaderInput::WorkgroupId, offsetof(InterfaceData, entryArgIdxs.cs.workgroupId), true},
    {ShaderInput::MultiDispatchInfo, 0, true},
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
// @param [in/out] argTys : Argument types vector to add to
// @return : Bitmap with bits set for SGPR arguments so caller can set "inreg" attribute on the args
uint64_t ShaderInputs::getShaderArgTys(PipelineState *pipelineState, ShaderStage shaderStage,
                                       SmallVectorImpl<Type *> &argTys) {

  bool hasTs = pipelineState->hasShaderStage(ShaderStageTessControl);
  bool hasGs = pipelineState->hasShaderStage(ShaderStageGeometry);

  uint64_t inRegMask = 0;
  auto intfData = pipelineState->getShaderInterfaceData(shaderStage);
  auto resUsage = pipelineState->getShaderResourceUsage(shaderStage);
  const auto &xfbStrides = resUsage->inOutUsage.xfbStrides;
  const bool enableXfb = resUsage->inOutUsage.enableXfb;

  // Enable optional shader inputs as required.
  switch (shaderStage) {
  case ShaderStageVertex:
    if (enableXfb && (!hasGs && !hasTs)) {
      // Stream-out in VS as hardware VS
      getShaderInputUsage(shaderStage, ShaderInput::StreamOutInfo)->enable();
      getShaderInputUsage(shaderStage, ShaderInput::StreamOutWriteIndex)->enable();
      for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
        if (xfbStrides[i] > 0)
          getShaderInputUsage(shaderStage, static_cast<unsigned>(ShaderInput::StreamOutOffset0) + i)->enable();
      }
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
      // TES as hardware VS: handle stream-out. StreamOutInfo is required for off-chip even if there is no
      // stream-out.
      if (pipelineState->isTessOffChip() || enableXfb)
        getShaderInputUsage(shaderStage, ShaderInput::StreamOutInfo)->enable();
      if (enableXfb) {
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

  // Determine which of the tables above to use for SGPR and VGPR inputs.
  ArrayRef<ShaderInputDesc> sgprInputDescs;
  ArrayRef<ShaderInputDesc> vgprInputDescs;

  switch (shaderStage) {
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
      ShaderInputUsage *inputUsage = &*inputsUsage->inputs[static_cast<unsigned>(inputDesc.inputKind)];
      // We don't want this input if it is not marked "always" and it is not used.
      if (!inputDesc.always && (!inputUsage || inputUsage->users.empty()))
        continue;
      if (!doingVgprs) {
        // Mark this arg as "inreg" to get it into an SGPR.
        inRegMask |= 1ull << argTys.size();
      }
      // Store the argument index.
      if (inputDesc.entryArgIdx != 0)
        *reinterpret_cast<unsigned *>((reinterpret_cast<char *>(intfData) + inputDesc.entryArgIdx)) = argTys.size();
      if (inputUsage)
        inputUsage->entryArgIdx = argTys.size();
      // Add the argument type.
      argTys.push_back(getInputType(inputDesc.inputKind, *m_context));
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
    inputsUsage->inputs[inputKind].reset(new ShaderInputUsage());
  return &*inputsUsage->inputs[inputKind];
}
