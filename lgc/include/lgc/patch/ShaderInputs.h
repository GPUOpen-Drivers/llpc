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
 * @file  ShaderInputs.h
 * @brief LGC header file: Handling of hardware-determined shader inputs (not user data, other than special user data)
 *
 * When it dispatches a wave and starts running a shader, the hardware sets up a number of SGPRs and VGPRs,
 * depending on which shader stage it is, and some configuration in SPI registers. The enum and class in this
 * file encapsulate that functionality.
 *
 * User data is included in the SGPRs set up at wave dispatch; user data is handled separately and is not
 * part of the functionality encapsulated here, except that a few utility methods for special user data are here.
 *
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/CommonDefs.h"
#include "lgc/LgcContext.h"
#include "lgc/state/AbiMetadata.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
namespace llvm {
class CallInst;
class Instruction;
class LLVMContext;
class Module;
class Type;
class Value;
} // namespace llvm

namespace lgc {

class BuilderBase;
class PipelineState;

// =====================================================================================================================
// Internal numbering for shader inputs
enum class ShaderInput : unsigned {
  // TasK/CS SGPRs
  WorkgroupId,       // WorkgroupId (v3i32)
  WorkgroupId2,      // WorkgroupId (v2i32)
  WorkgroupId1,      // WorkgroupId (i32)
  MultiDispatchInfo, // Multiple dispatch info, include TG_SIZE and etc.

  // FS SGPRs
  PrimMask, // Primitive mask

  // Appears in hardware HS, ES, VS SGPRs
  OffChipLdsBase, // Off-chip LDS buffer base

  // Hardware VS SGPRs
  StreamOutInfo,       // Stream-out info (ID, vertex count, enablement)
  StreamOutWriteIndex, // Stream-out write index
  StreamOutOffset0,    // Stream-out offset 0
  StreamOutOffset1,    // Stream-out offset 1
  StreamOutOffset2,    // Stream-out offset 2
  StreamOutOffset3,    // Stream-out offset 3

  // Unmerged hardware GS SGPRs
  GsVsOffset, // GS to VS offset
  GsWaveId,   // GS wave ID

  // Unmerged hardware ES SGPRs
  IsOffChip,  // is_off_chip
  EsGsOffset, // ES to GS offset

  // Unmerged hardware HS SGPRs
  TfBufferBase, // TF buffer base

  FirstVgpr, // Enums less than this are SGPRs

  // API VS VGPRs
  VertexId = FirstVgpr, // Vertex ID
  RelVertexId,          // Relative vertex ID (auto index)
  PrimitiveId,          // Primitive ID
  InstanceId,           // Instance ID

  // Appear in TCS and TES VGPRs
  PatchId,    // Patch ID
  RelPatchId, // Relative patch ID (in TCS, control point ID included)

  // TES VGPRs
  TessCoordX, // X of TessCoord (U) (float)
  TessCoordY, // Y of TessCoord (V) (float)

  // GS VGPRs
  EsGsOffset0,   // ES to GS offset (vertex 0)
  EsGsOffset1,   // ES to GS offset (vertex 1)
  GsPrimitiveId, // Primitive ID
  EsGsOffset2,   // ES to GS offset (vertex 2)
  EsGsOffset3,   // ES to GS offset (vertex 3)
  EsGsOffset4,   // ES to GS offset (vertex 4)
  EsGsOffset5,   // ES to GS offset (vertex 5)
  GsInstanceId,  // Invocation ID

  // FS VGPRs
  PerspInterpSample,    // Perspective sample (v2f32)
  PerspInterpCenter,    // Perspective center (v2f32)
  PerspInterpCentroid,  // Perspective centroid (v2f32)
  PerspInterpPullMode,  // Perspective pull-mode (v3f32)
  LinearInterpSample,   // Perspective sample (v2f32)
  LinearInterpCenter,   // Perspective center (v2f32)
  LinearInterpCentroid, // Perspective centroid (v2f32)
  LineStipple,          // Line stipple (float)
  FragCoordX,           // X of FragCoord (float)
  FragCoordY,           // Y of FragCoord (float)
  FragCoordZ,           // Z of FragCoord (float)
  FragCoordW,           // W of FragCoord (float)
  FrontFacing,          // Front facing
  Ancillary,            // Ancillary
  SampleCoverage,       // Sample coverage
  FixedXY,              // Fixed X/Y

  // Task/CS VGPRs
  LocalInvocationId, // LocalInvocationId (v3i32)
  Count
};

// =====================================================================================================================
// Class for handling shader inputs (other than user data)
//
// From BuilderImpl up to just before PatchEntryPointMutate, static methods in this class can be used to
// generate code to access shader inputs. That generates an lgc.shader.input.* call for each access.
//
// The PatchEntryPointMutate pass creates a ShaderInputs object, and uses a method on it to gather already-
// generated uses of shader inputs, and another method to create arguments for the shader function based
// on that, and on usage that will happen after PatchEntryPointMutate.
//
// The resulting shader function has input arguments that represent a kind of idealized GFX8 shader,
// before GFX9+ shader merging and/or GFX10+ NGG primitive shader formation.
//
class ShaderInputs {
public:
  // -------------------------------------------------------------------------------------------------------------------
  // Static methods called any time

  // Get name of a special user data value, one of the UserDataMapping values
  static const char *getSpecialUserDataName(unsigned kind) {
    return getSpecialUserDataName(static_cast<UserDataMapping>(kind));
  }
  static const char *getSpecialUserDataName(UserDataMapping kind);

  // Get IR type of a particular shader input
  static llvm::Type *getInputType(ShaderInput inputKind, const LgcContext &lgcContext);

  // Get name of shader input
  static const char *getInputName(ShaderInput inputKind);

  // -------------------------------------------------------------------------------------------------------------------
  // Static methods called before PatchEntryPointMutate

  // Get a special user data value by inserting a call to lgc.special.user.data
  static llvm::CallInst *getSpecialUserData(UserDataMapping kind, BuilderBase &builder);

  // Get a special user data value as a pointer by inserting a call to lgc.special.user.data then extending it
  static llvm::Value *getSpecialUserDataAsPointer(UserDataMapping kind, llvm::Type *pointeeTy, BuilderBase &builder);

  // Get VertexIndex
  static llvm::Value *getVertexIndex(BuilderBase &builder, const LgcContext &lgcContext);

  // Get InstanceIndex
  static llvm::Value *getInstanceIndex(BuilderBase &builder, const LgcContext &lgcContext);

  // Get a shader input value by inserting a call to lgc.shader.input
  static llvm::Value *getInput(ShaderInput kind, BuilderBase &builder, const LgcContext &lgcContext);

  // -------------------------------------------------------------------------------------------------------------------
  // Object methods called during PatchEntryPointMutate

  // Gather usage of shader inputs from before PatchEntryPointMutate
  void gatherUsage(llvm::Module &module);

  // Fix up uses of shader inputs to use entry args directly
  void fixupUses(llvm::Module &module, PipelineState *pipelineState);

  // Get argument types for shader inputs
  uint64_t getShaderArgTys(PipelineState *pipelineState, ShaderStage shaderStage, llvm::Function *origFunc,
                           bool isComputeWithCalls, llvm::SmallVectorImpl<llvm::Type *> &argTys,
                           llvm::SmallVectorImpl<std::string> &argNames, unsigned argOffset);

private:
  // Usage for one system shader input in one shader stage
  struct ShaderInputUsage {
    void enable() { users.push_back(nullptr); }
    unsigned entryArgIdx = 0;
    llvm::SmallVector<llvm::Instruction *, 4> users;
  };

  // Per-shader-stage gathered usage of system shader inputs
  struct ShaderInputsUsage {
    std::unique_ptr<ShaderInputUsage> inputs[static_cast<unsigned>(ShaderInput::Count)];
  };

  // Get ShaderInputsUsage struct for the given shader stage
  ShaderInputsUsage *getShaderInputsUsage(ShaderStage stage);

  // Get (create if necessary) ShaderInputUsage struct for the given system shader input in the given shader stage
  ShaderInputUsage *getShaderInputUsage(ShaderStage stage, ShaderInput inputKind) {
    return getShaderInputUsage(stage, static_cast<unsigned>(inputKind));
  }
  ShaderInputUsage *getShaderInputUsage(ShaderStage stage, unsigned inputKind);

  // Try to optimize to use the accurate workgroupID arguments and set the function attribute for corresponding
  // amdgpu-no-workgroup-id-*
  void tryOptimizeWorkgroupId(PipelineState *pipelineState, ShaderStage shaderStage, llvm::Function *origFunc);

  llvm::SmallVector<ShaderInputsUsage, ShaderStageCountInternal> m_shaderInputsUsage;
};

} // namespace lgc
