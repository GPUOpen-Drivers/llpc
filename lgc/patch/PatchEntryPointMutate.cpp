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
 * @file  PatchEntryPointMutate.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchEntryPointMutate.
 ***********************************************************************************************************************
 */
#include "PatchEntryPointMutate.h"
#include "Gfx6Chip.h"
#include "Gfx9Chip.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#define DEBUG_TYPE "llpc-patch-entry-point-mutate"

using namespace llvm;
using namespace lgc;

namespace llvm {

namespace cl {

// -inreg-esgs-lds-size: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent with PAL's
// GS on-chip behavior. In the future, if PAL allows hardcoded ES-GS LDS size, this option could be deprecated.
opt<bool> InRegEsGsLdsSize("inreg-esgs-lds-size", desc("For GS on-chip, add esGsLdsSize in user data"), init(true));

} // namespace cl

} // namespace llvm

namespace lgc {

// =====================================================================================================================
// Initializes static members.
char PatchEntryPointMutate::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching opertions for entry-point mutation
ModulePass *createPatchEntryPointMutate() {
  return new PatchEntryPointMutate();
}

// =====================================================================================================================
PatchEntryPointMutate::PatchEntryPointMutate() : Patch(ID), m_hasTs(false), m_hasGs(false) {
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool PatchEntryPointMutate::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Entry-Point-Mutate\n");

  Patch::init(&module);

  m_pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);

  const unsigned stageMask = m_pipelineState->getShaderStageMask();
  m_hasTs = (stageMask & (shaderStageToMask(ShaderStageTessControl) | shaderStageToMask(ShaderStageTessEval))) != 0;
  m_hasGs = (stageMask & shaderStageToMask(ShaderStageGeometry)) != 0;

  // Process each shader in turn, but not the copy shader.
  auto pipelineShaders = &getAnalysis<PipelineShaders>();
  for (unsigned shaderStage = ShaderStageVertex; shaderStage < ShaderStageNativeStageCount; ++shaderStage) {
    m_entryPoint = pipelineShaders->getEntryPoint(static_cast<ShaderStage>(shaderStage));
    if (m_entryPoint) {
      m_shaderStage = static_cast<ShaderStage>(shaderStage);
      processShader();
    }
  }

  if (m_shaderStage == ShaderStageCompute) {
    // A compute shader is allowed to have subfunctions. Here we need to mutate all the subfunctions
    // to add on the same args that the shader has, so they are passed around to all code.
    mutateSubfunctions(m_entryPoint);
  }

  return true;
}

// =====================================================================================================================
// Process a single shader
void PatchEntryPointMutate::processShader() {
  // Create new entry-point from the original one (mutate it)
  // TODO: We should mutate entry-point arguments instead of clone a new entry-point.
  uint64_t inRegMask = 0;
  FunctionType *entryPointTy = generateEntryPointType(&inRegMask);

  Function *origEntryPoint = m_entryPoint;

  Function *entryPoint = Function::Create(entryPointTy, GlobalValue::ExternalLinkage, "", m_module);
  entryPoint->setCallingConv(origEntryPoint->getCallingConv());
  entryPoint->addFnAttr(Attribute::NoUnwind);
  entryPoint->takeName(origEntryPoint);
  m_entryPoint = entryPoint;

  ValueToValueMapTy valueMap;
  SmallVector<ReturnInst *, 8> retInsts;
  CloneFunctionInto(entryPoint, origEntryPoint, valueMap, false, retInsts);

  // Set Attributes on cloned function here as some are overwritten during CloneFunctionInto otherwise
  AttrBuilder builder;
  if (m_shaderStage == ShaderStageFragment) {
    auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
    SpiPsInputAddr spiPsInputAddr = {};

    spiPsInputAddr.bits.perspSampleEna =
        ((builtInUsage.smooth && builtInUsage.sample) || builtInUsage.baryCoordSmoothSample);
    spiPsInputAddr.bits.perspCenterEna = ((builtInUsage.smooth && builtInUsage.center) || builtInUsage.baryCoordSmooth);
    spiPsInputAddr.bits.perspCentroidEna =
        ((builtInUsage.smooth && builtInUsage.centroid) || builtInUsage.baryCoordSmoothCentroid);
    spiPsInputAddr.bits.perspPullModelEna =
        ((builtInUsage.smooth && builtInUsage.pullMode) || builtInUsage.baryCoordPullModel);
    spiPsInputAddr.bits.linearSampleEna =
        ((builtInUsage.noperspective && builtInUsage.sample) || builtInUsage.baryCoordNoPerspSample);
    spiPsInputAddr.bits.linearCenterEna =
        ((builtInUsage.noperspective && builtInUsage.center) || builtInUsage.baryCoordNoPersp);
    spiPsInputAddr.bits.linearCentroidEna =
        ((builtInUsage.noperspective && builtInUsage.centroid) || builtInUsage.baryCoordNoPerspCentroid);
    spiPsInputAddr.bits.posXFloatEna = builtInUsage.fragCoord;
    spiPsInputAddr.bits.posYFloatEna = builtInUsage.fragCoord;
    spiPsInputAddr.bits.posZFloatEna = builtInUsage.fragCoord;
    spiPsInputAddr.bits.posWFloatEna = builtInUsage.fragCoord;
    spiPsInputAddr.bits.frontFaceEna = builtInUsage.frontFacing;
    spiPsInputAddr.bits.ancillaryEna = builtInUsage.sampleId;
    spiPsInputAddr.bits.sampleCoverageEna = builtInUsage.sampleMaskIn;

    builder.addAttribute("InitialPSInputAddr", std::to_string(spiPsInputAddr.u32All));
  }

  // Set VGPR, SGPR, and wave limits
  auto shaderOptions = &m_pipelineState->getShaderOptions(m_shaderStage);
  auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);

  unsigned vgprLimit = shaderOptions->vgprLimit;
  unsigned sgprLimit = shaderOptions->sgprLimit;

  if (vgprLimit != 0) {
    builder.addAttribute("amdgpu-num-vgpr", std::to_string(vgprLimit));
    resUsage->numVgprsAvailable = std::min(vgprLimit, resUsage->numVgprsAvailable);
  }
  resUsage->numVgprsAvailable =
      std::min(resUsage->numVgprsAvailable, m_pipelineState->getTargetInfo().getGpuProperty().maxVgprsAvailable);

  if (sgprLimit != 0) {
    builder.addAttribute("amdgpu-num-sgpr", std::to_string(sgprLimit));
    resUsage->numSgprsAvailable = std::min(sgprLimit, resUsage->numSgprsAvailable);
  }
  resUsage->numSgprsAvailable =
      std::min(resUsage->numSgprsAvailable, m_pipelineState->getTargetInfo().getGpuProperty().maxSgprsAvailable);

  if (shaderOptions->maxThreadGroupsPerComputeUnit != 0) {
    std::string wavesPerEu = std::string("0,") + std::to_string(shaderOptions->maxThreadGroupsPerComputeUnit);
    builder.addAttribute("amdgpu-waves-per-eu", wavesPerEu);
  }

  if (shaderOptions->unrollThreshold != 0)
    builder.addAttribute("amdgpu-unroll-threshold", std::to_string(shaderOptions->unrollThreshold));
  else {
    // use a default unroll threshold of 700
    builder.addAttribute("amdgpu-unroll-threshold", "700");
  }

  AttributeList::AttrIndex attribIdx = AttributeList::AttrIndex(AttributeList::FunctionIndex);
  entryPoint->addAttributes(attribIdx, builder);

  // NOTE: Remove "readnone" attribute for entry-point. If GS is emtry, this attribute will allow
  // LLVM optimization to remove sendmsg(GS_DONE). It is unexpected.
  if (entryPoint->hasFnAttribute(Attribute::ReadNone))
    entryPoint->removeFnAttr(Attribute::ReadNone);

  // Update attributes of new entry-point
  for (auto arg = entryPoint->arg_begin(), end = entryPoint->arg_end(); arg != end; ++arg) {
    auto argIdx = arg->getArgNo();
    if (inRegMask & (1ull << argIdx))
      arg->addAttr(Attribute::InReg);
  }

  // Remove original entry-point
  origEntryPoint->dropAllReferences();
  origEntryPoint->eraseFromParent();
}

// =====================================================================================================================
// Checks whether the specified resource mapping node is active.
//
// @param node : Resource mapping node
// @param isRootNode : TRUE if node is in root level
bool PatchEntryPointMutate::isResourceNodeActive(const ResourceNode *node, bool isRootNode) const {
  bool active = false;

  const ResourceUsage *resUsage1 = m_pipelineState->getShaderResourceUsage(m_shaderStage);
  const ResourceUsage *resUsage2 = nullptr;

  const auto gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  if (gfxIp.major >= 9) {
    // NOTE: For LS-HS/ES-GS merged shader, resource mapping nodes of the two shader stages are merged as a whole.
    // So we have to check activeness of both shader stages at the same time. Here, we determine the second shader
    // stage and get its resource usage accordingly.
    unsigned stageMask = m_pipelineState->getShaderStageMask();
    const bool hasTs =
        ((stageMask & (shaderStageToMask(ShaderStageTessControl) | shaderStageToMask(ShaderStageTessEval))) != 0);
    const bool hasGs = ((stageMask & shaderStageToMask(ShaderStageGeometry)) != 0);

    if (hasTs || hasGs) {
      const auto shaderStage1 = m_shaderStage;
      auto shaderStage2 = ShaderStageInvalid;

      if (shaderStage1 == ShaderStageVertex) {
        shaderStage2 = hasTs ? ShaderStageTessControl : (hasGs ? ShaderStageGeometry : ShaderStageInvalid);
      } else if (shaderStage1 == ShaderStageTessControl)
        shaderStage2 = ShaderStageVertex;
      else if (shaderStage1 == ShaderStageTessEval)
        shaderStage2 = hasGs ? ShaderStageGeometry : ShaderStageInvalid;
      else if (shaderStage1 == ShaderStageGeometry)
        shaderStage2 = hasTs ? ShaderStageTessEval : ShaderStageVertex;

      if (shaderStage2 != ShaderStageInvalid)
        resUsage2 = m_pipelineState->getShaderResourceUsage(shaderStage2);
    }
  }

  if (node->type == ResourceNodeType::PushConst && isRootNode) {
    active = resUsage1->pushConstSizeInBytes > 0;
    if (!active && resUsage2)
      active = resUsage2->pushConstSizeInBytes > 0;
  } else if (node->type == ResourceNodeType::DescriptorTableVaPtr) {
    // Check if any contained descriptor node is active
    for (unsigned i = 0; i < node->innerTable.size(); ++i) {
      if (isResourceNodeActive(&node->innerTable[i], false)) {
        active = true;
        break;
      }
    }
  } else if (node->type == ResourceNodeType::IndirectUserDataVaPtr) {
    // NOTE: We assume indirect user data is always active.
    active = true;
  } else if (node->type == ResourceNodeType::StreamOutTableVaPtr)
    active = true;
  else {
    assert(node->type != ResourceNodeType::DescriptorTableVaPtr &&
           node->type != ResourceNodeType::IndirectUserDataVaPtr);

    DescriptorPair descPair = {};
    descPair.descSet = node->set;
    descPair.binding = node->binding;

    active = resUsage1->descPairs.find(descPair.u64All) != resUsage1->descPairs.end();
    if (!active && resUsage2)
      active = resUsage2->descPairs.find(descPair.u64All) != resUsage2->descPairs.end();
  }

  return active;
}

// =====================================================================================================================
// Generates the type for the new entry-point based on already-collected info in LLVM context.
//
// @param [out] inRegMask : "Inreg" bit mask for the arguments
FunctionType *PatchEntryPointMutate::generateEntryPointType(uint64_t *inRegMask) const {
  unsigned userDataIdx = 0;
  std::vector<Type *> argTys;

  auto userDataNodes = m_pipelineState->getUserDataNodes();
  auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
  auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);

  // Global internal table
  *inRegMask |= 1ull << argTys.size();
  argTys.push_back(Type::getInt32Ty(*m_context));
  ++userDataIdx;

  // TODO: We need add per shader table per real usage after switch to PAL new interface.
  // if (pResUsage->perShaderTable)
  {
    *inRegMask |= 1ull << argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context));
    ++userDataIdx;
  }

  auto &builtInUsage = resUsage->builtInUsage;
  auto &entryArgIdxs = intfData->entryArgIdxs;
  entryArgIdxs.initialized = true;

  // Estimated available user data count
  unsigned maxUserDataCount = m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount;
  unsigned availUserDataCount = maxUserDataCount - userDataIdx;
  unsigned requiredRemappedUserDataCount = 0; // Maximum required user data
  unsigned requiredUserDataCount = 0;         // Maximum required user data without remapping
  bool useFixedLayout = (m_shaderStage == ShaderStageCompute);
  bool reserveVbTable = false;
  bool reserveStreamOutTable = false;
  bool reserveEsGsLdsSize = false;
  bool needSpill = false;

  if (userDataNodes.size() > 0) {
    for (unsigned i = 0; i < userDataNodes.size(); ++i) {
      auto node = &userDataNodes[i];
      // NOTE: Per PAL request, the value of IndirectTableEntry is the node offset + 1.
      // and indirect user data should not be counted in possible spilled user data.
      if (node->type == ResourceNodeType::IndirectUserDataVaPtr) {
        // Only the vertex shader needs a vertex buffer table.
        if (m_shaderStage == ShaderStageVertex)
          reserveVbTable = true;
        else if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 9) {
          // On GFX9+, the shader stage that the vertex shader is merged in to needs a vertex buffer
          // table, to ensure that the merged shader gets one.
          if (m_shaderStage == ShaderStageTessControl || (m_shaderStage == ShaderStageGeometry && !m_hasTs))
            reserveVbTable = true;
        }
        continue;
      }

      if (node->type == ResourceNodeType::StreamOutTableVaPtr) {
        // Only the last shader stage before fragment (ignoring copy shader) needs a stream out table.
        if ((m_pipelineState->getShaderStageMask() &
             (shaderStageToMask(ShaderStageFragment) - shaderStageToMask(m_shaderStage))) ==
            shaderStageToMask(m_shaderStage))
          reserveStreamOutTable = true;
        else if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 9) {
          // On GFX9+, the shader stage that the last shader is merged in to needs a stream out
          // table, to ensure that the merged shader gets one.
          if (m_shaderStage == ShaderStageTessEval || (m_shaderStage == ShaderStageVertex && !m_hasTs))
            reserveStreamOutTable = true;
        }
        continue;
      }

      if (!isResourceNodeActive(node, true))
        continue;

      switch (node->type) {
      case ResourceNodeType::DescriptorBuffer:
      case ResourceNodeType::DescriptorResource:
      case ResourceNodeType::DescriptorSampler:
      case ResourceNodeType::DescriptorTexelBuffer:
      case ResourceNodeType::DescriptorFmask:
        // Where an image or sampler descriptor appears in the top-level table, it is accesssed
        // via the spill table, rather than directly placed in sgprs.
        needSpill = true;
        break;
      case ResourceNodeType::PushConst:
        intfData->pushConst.resNodeIdx = i;
        break;
      default:
        break;
      }

      requiredUserDataCount = std::max(requiredUserDataCount, node->offsetInDwords + node->sizeInDwords);
      requiredRemappedUserDataCount += node->sizeInDwords;
    }
  }

  auto enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;

  const bool enableNgg = m_pipelineState->isGraphics() ? m_pipelineState->getNggControl()->enableNgg : false;

  switch (m_shaderStage) {
  case ShaderStageVertex:
  case ShaderStageTessControl: {
    if (enableMultiView)
      availUserDataCount -= 1;

    // Reserve register for "IndirectUserDataVaPtr"
    if (reserveVbTable)
      availUserDataCount -= 1;

    // Reserve for stream-out table
    if (reserveStreamOutTable)
      availUserDataCount -= 1;

    // NOTE: On GFX9+, Vertex shader (LS) and tessellation control shader (HS) are merged into a single shader.
    // The user data count of tessellation control shader should be same as vertex shader.
    auto currResUsage = resUsage;
    if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 9 && m_shaderStage == ShaderStageTessControl &&
        (m_pipelineState->getShaderStageMask() & shaderStageToMask(ShaderStageVertex)))
      currResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);

    if (currResUsage->builtInUsage.vs.baseVertex || currResUsage->builtInUsage.vs.baseInstance)
      availUserDataCount -= 2;

    if (currResUsage->builtInUsage.vs.drawIndex)
      availUserDataCount -= 1;

    // NOTE: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent
    // with PAL's GS on-chip behavior (VS is in NGG primitive shader).
    const auto gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
    if ((gfxIp.major >= 9 && (m_pipelineState->isGsOnChip() && cl::InRegEsGsLdsSize)) || (enableNgg && !m_hasTs)) {
      availUserDataCount -= 1;
      reserveEsGsLdsSize = true;
    }
    break;
  }
  case ShaderStageTessEval: {
    if (enableMultiView)
      availUserDataCount -= 1;

    // Reserve for stream-out table
    if (reserveStreamOutTable)
      availUserDataCount -= 1;

    // NOTE: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent
    // with PAL's GS on-chip behavior (TES is in NGG primitive shader).
    if (enableNgg) {
      availUserDataCount -= 1;
      reserveEsGsLdsSize = true;
    }

    break;
  }
  case ShaderStageGeometry: {
    if (enableMultiView)
      availUserDataCount -= 1;

    // NOTE: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent
    // with PAL's GS on-chip behavior. i.e. GS is GFX8
    if ((m_pipelineState->isGsOnChip() && cl::InRegEsGsLdsSize) || enableNgg) {
      // NOTE: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent
      // with PAL's GS on-chip behavior.
      availUserDataCount -= 1;
      reserveEsGsLdsSize = true;
    }

    break;
  }
  case ShaderStageFragment: {
    // Do nothing
    break;
  }
  case ShaderStageCompute: {
    // Emulate gl_NumWorkGroups via user data registers
    if (builtInUsage.cs.numWorkgroups)
      availUserDataCount -= 2;
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  // NOTE: We have to spill user data to memory when available user data is less than required.
  if (useFixedLayout) {
    assert(m_shaderStage == ShaderStageCompute);
    needSpill |= (requiredUserDataCount > InterfaceData::MaxCsUserDataCount);
    availUserDataCount = InterfaceData::MaxCsUserDataCount;
  } else {
    needSpill |= (requiredRemappedUserDataCount > availUserDataCount - needSpill);
    intfData->spillTable.offsetInDwords = InvalidValue;
    if (needSpill) {
      // Spill table need an addtional user data
      --availUserDataCount;
    }
  }

  // Allocate register for stream-out buffer table
  if (reserveStreamOutTable) {
    for (unsigned i = 0; i < userDataNodes.size(); ++i) {
      auto node = &userDataNodes[i];
      if (node->type == ResourceNodeType::StreamOutTableVaPtr) {
        assert(node->sizeInDwords == 1);
        switch (m_shaderStage) {
        case ShaderStageVertex: {
          intfData->userDataUsage.vs.streamOutTablePtr = userDataIdx;
          intfData->entryArgIdxs.vs.streamOutData.tablePtr = argTys.size();
          break;
        }
        case ShaderStageTessEval: {
          intfData->userDataUsage.tes.streamOutTablePtr = userDataIdx;
          intfData->entryArgIdxs.tes.streamOutData.tablePtr = argTys.size();
          break;
        }
        // Allocate dummpy stream-out register for Geometry shader
        case ShaderStageGeometry: {
          break;
        }
        default: {
          llvm_unreachable("Should never be called!");
          break;
        }
        }
        *inRegMask |= 1ull << argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context));
        ++userDataIdx;
        break;
      }
    }
  }

  // Descriptor table and vertex buffer table
  unsigned actualAvailUserDataCount = 0;
  for (unsigned i = 0; i < userDataNodes.size(); ++i) {
    auto node = &userDataNodes[i];

    // "IndirectUserDataVaPtr" can't be spilled, it is treated as internal user data
    if (node->type == ResourceNodeType::IndirectUserDataVaPtr)
      continue;

    if (node->type == ResourceNodeType::StreamOutTableVaPtr)
      continue;

    if (!isResourceNodeActive(node, true))
      continue;

    if (useFixedLayout) {
      // NOTE: For fixed user data layout (for compute shader), we could not pack those user data and dummy
      // entry-point arguments are added once DWORD offsets of user data are not continuous.
      assert(m_shaderStage == ShaderStageCompute);

      while (userDataIdx < (node->offsetInDwords + InterfaceData::CsStartUserData) &&
             userDataIdx < (availUserDataCount + InterfaceData::CsStartUserData)) {
        *inRegMask |= 1ull << argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context));
        ++userDataIdx;
        ++actualAvailUserDataCount;
      }
    }

    if (actualAvailUserDataCount + node->sizeInDwords <= availUserDataCount) {
      // User data isn't spilled
      assert(i < InterfaceData::MaxDescTableCount);
      intfData->entryArgIdxs.resNodeValues[i] = argTys.size();
      *inRegMask |= 1ull << argTys.size();
      actualAvailUserDataCount += node->sizeInDwords;
      switch (node->type) {
      case ResourceNodeType::DescriptorTableVaPtr: {
        argTys.push_back(Type::getInt32Ty(*m_context));

        assert(node->sizeInDwords == 1);

        auto shaderOptions = &m_pipelineState->getShaderOptions(m_shaderStage);
        if (shaderOptions->updateDescInElf && m_shaderStage == ShaderStageFragment) {
          // Put set number to register first, will update offset after merge ELFs
          // For partial pipeline compile, only fragment shader needs to adjust offset of root descriptor
          // If there are more individual shader compile in future, we can add more stages here
          intfData->userDataMap[userDataIdx] = DescRelocMagic | node->innerTable[0].set;
        } else
          intfData->userDataMap[userDataIdx] = node->offsetInDwords;

        ++userDataIdx;
        break;
      }

      case ResourceNodeType::DescriptorBuffer:
      case ResourceNodeType::DescriptorResource:
      case ResourceNodeType::DescriptorSampler:
      case ResourceNodeType::DescriptorTexelBuffer:
      case ResourceNodeType::DescriptorFmask:
        // Where a descriptor appears in the top-level table, it is accesssed
        // via the spill table, rather than directly placed in sgprs.
        assert(needSpill);
        if (intfData->spillTable.offsetInDwords == InvalidValue)
          intfData->spillTable.offsetInDwords = node->offsetInDwords;
        break;

      case ResourceNodeType::PushConst:
      case ResourceNodeType::DescriptorBufferCompact: {
        argTys.push_back(VectorType::get(Type::getInt32Ty(*m_context), node->sizeInDwords));
        for (unsigned j = 0; j < node->sizeInDwords; ++j)
          intfData->userDataMap[userDataIdx + j] = node->offsetInDwords + j;
        userDataIdx += node->sizeInDwords;
        break;
      }
      default: {
        llvm_unreachable("Should never be called!");
        break;
      }
      }
    } else if (needSpill && intfData->spillTable.offsetInDwords == InvalidValue)
      intfData->spillTable.offsetInDwords = node->offsetInDwords;
  }

  // Internal user data
  if (needSpill && useFixedLayout) {
    // Add spill table
    assert(intfData->spillTable.offsetInDwords != InvalidValue);
    assert(userDataIdx <= (InterfaceData::MaxCsUserDataCount + InterfaceData::CsStartUserData));
    while (userDataIdx <= (InterfaceData::MaxCsUserDataCount + InterfaceData::CsStartUserData)) {
      *inRegMask |= 1ull << argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context));
      ++userDataIdx;
    }
    intfData->userDataUsage.spillTable = userDataIdx - 1;
    intfData->entryArgIdxs.spillTable = argTys.size() - 1;

    intfData->spillTable.sizeInDwords = requiredUserDataCount;
  }

  switch (m_shaderStage) {
  case ShaderStageVertex:
  case ShaderStageTessControl: {
    // NOTE: On GFX9+, Vertex shader (LS) and tessellation control shader (HS) are merged into a single shader.
    // The user data count of tessellation control shader should be same as vertex shader.
    auto currIntfData = intfData;
    auto currResUsage = resUsage;

    if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 9 && m_shaderStage == ShaderStageTessControl &&
        (m_pipelineState->getShaderStageMask() & shaderStageToMask(ShaderStageVertex))) {
      currIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
      currResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);
    }

    // NOTE: The user data to emulate gl_ViewIndex is somewhat common. To make it consistent for GFX9
    // merged shader, we place it prior to any other special user data.
    if (enableMultiView) {
      *inRegMask |= 1ull << argTys.size();
      entryArgIdxs.vs.viewIndex = argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context)); // View Index
      currIntfData->userDataUsage.vs.viewIndex = userDataIdx;
      ++userDataIdx;
    }

    if (reserveEsGsLdsSize) {
      *inRegMask |= 1ull << argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context));
      currIntfData->userDataUsage.vs.esGsLdsSize = userDataIdx;
      ++userDataIdx;
    }

    for (unsigned i = 0; i < userDataNodes.size(); ++i) {
      auto node = &userDataNodes[i];
      if (node->type == ResourceNodeType::IndirectUserDataVaPtr) {
        assert(node->sizeInDwords == 1);
        currIntfData->userDataUsage.vs.vbTablePtr = userDataIdx;
        currIntfData->entryArgIdxs.vs.vbTablePtr = argTys.size();
        *inRegMask |= 1ull << argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context));
        ++userDataIdx;
        break;
      }
    }

    if (currResUsage->builtInUsage.vs.baseVertex || currResUsage->builtInUsage.vs.baseInstance) {
      *inRegMask |= 1ull << argTys.size();
      entryArgIdxs.vs.baseVertex = argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context)); // Base vertex
      currIntfData->userDataUsage.vs.baseVertex = userDataIdx;
      ++userDataIdx;

      *inRegMask |= 1ull << argTys.size();
      entryArgIdxs.vs.baseInstance = argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context)); // Base instance
      currIntfData->userDataUsage.vs.baseInstance = userDataIdx;
      ++userDataIdx;
    }

    if (currResUsage->builtInUsage.vs.drawIndex) {
      *inRegMask |= 1ull << argTys.size();
      entryArgIdxs.vs.drawIndex = argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context)); // Draw index
      currIntfData->userDataUsage.vs.drawIndex = userDataIdx;
      ++userDataIdx;
    }

    break;
  }
  case ShaderStageTessEval: {
    // NOTE: The user data to emulate gl_ViewIndex is somewhat common. To make it consistent for GFX9
    // merged shader, we place it prior to any other special user data.
    if (enableMultiView) {
      *inRegMask |= 1ull << argTys.size();
      entryArgIdxs.tes.viewIndex = argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context)); // View Index
      intfData->userDataUsage.tes.viewIndex = userDataIdx;
      ++userDataIdx;
    }

    if (reserveEsGsLdsSize) {
      *inRegMask |= 1ull << argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context));
      intfData->userDataUsage.tes.esGsLdsSize = userDataIdx;
      ++userDataIdx;
    }
    break;
  }
  case ShaderStageGeometry: {
    // NOTE: The user data to emulate gl_ViewIndex is somewhat common. To make it consistent for GFX9
    // merged shader, we place it prior to any other special user data.
    if (enableMultiView) {
      *inRegMask |= 1ull << argTys.size();
      entryArgIdxs.gs.viewIndex = argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context)); // View Index
      intfData->userDataUsage.gs.viewIndex = userDataIdx;
      ++userDataIdx;
    }

    if (reserveEsGsLdsSize) {
      *inRegMask |= 1ull << argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context));
      intfData->userDataUsage.gs.esGsLdsSize = userDataIdx;
      ++userDataIdx;
    }
    break;
  }
  case ShaderStageCompute: {
    // Emulate gl_NumWorkGroups via user data registers
    if (builtInUsage.cs.numWorkgroups) {
      // NOTE: Pointer must be placed in even index according to LLVM backend compiler.
      if ((userDataIdx % 2) != 0) {
        *inRegMask |= 1ull << argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context));
        userDataIdx += 1;
      }

      auto numWorkgroupsPtrTy = PointerType::get(VectorType::get(Type::getInt32Ty(*m_context), 3), ADDR_SPACE_CONST);
      *inRegMask |= 1ull << argTys.size();
      entryArgIdxs.cs.numWorkgroupsPtr = argTys.size();
      argTys.push_back(numWorkgroupsPtrTy); // NumWorkgroupsPtr
      intfData->userDataUsage.cs.numWorkgroupsPtr = userDataIdx;
      userDataIdx += 2;
    }
    break;
  }
  case ShaderStageFragment: {
    // Do nothing
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  if (needSpill && !useFixedLayout) {
    *inRegMask |= 1ull << argTys.size();
    intfData->entryArgIdxs.spillTable = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context));

    intfData->userDataUsage.spillTable = userDataIdx++;
    intfData->spillTable.sizeInDwords = requiredUserDataCount;
  }
  intfData->userDataCount = userDataIdx;

  const auto &xfbStrides = resUsage->inOutUsage.xfbStrides;

  const bool enableXfb = resUsage->inOutUsage.enableXfb;

  // NOTE: Here, we start to add system values, they should be behind user data.
  switch (m_shaderStage) {
  case ShaderStageVertex: {
    if (m_hasGs && !m_hasTs) // VS acts as hardware ES
    {
      *inRegMask |= 1ull << argTys.size();
      entryArgIdxs.vs.esGsOffset = argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offset
    } else if (!m_hasGs && !m_hasTs)                  // VS acts as hardware VS
    {
      if (enableXfb) // If output to stream-out buffer
      {
        *inRegMask |= 1ull << argTys.size();
        entryArgIdxs.vs.streamOutData.streamInfo = argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context)); // Stream-out info (ID, vertex count, enablement)

        *inRegMask |= 1ull << argTys.size();
        entryArgIdxs.vs.streamOutData.writeIndex = argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context)); // Stream-out write Index

        for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
          if (xfbStrides[i] > 0) {
            *inRegMask |= 1ull << argTys.size();
            entryArgIdxs.vs.streamOutData.streamOffsets[i] = argTys.size();
            argTys.push_back(Type::getInt32Ty(*m_context)); // Stream-out offset
          }
        }
      }
    }

    // NOTE: The order of these arguments is determined by hardware.
    entryArgIdxs.vs.vertexId = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Vertex ID

    entryArgIdxs.vs.relVertexId = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Relative vertex ID (auto index)

    entryArgIdxs.vs.primitiveId = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Primitive ID

    entryArgIdxs.vs.instanceId = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Instance ID

    break;
  }
  case ShaderStageTessControl: {
    if (m_pipelineState->isTessOffChip()) {
      *inRegMask |= 1ull << argTys.size();
      entryArgIdxs.tcs.offChipLdsBase = argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context)); // Off-chip LDS buffer base
    }

    *inRegMask |= 1ull << argTys.size();
    entryArgIdxs.tcs.tfBufferBase = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // TF buffer base

    entryArgIdxs.tcs.patchId = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Patch ID

    entryArgIdxs.tcs.relPatchId = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Relative patch ID (control point ID included)

    break;
  }
  case ShaderStageTessEval: {
    if (m_hasGs) // TES acts as hardware ES
    {
      if (m_pipelineState->isTessOffChip()) {
        *inRegMask |= 1ull << argTys.size();
        entryArgIdxs.tes.offChipLdsBase = argTys.size(); // Off-chip LDS buffer base
        argTys.push_back(Type::getInt32Ty(*m_context));

        *inRegMask |= 1ull << argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context)); // If is_offchip enabled
      }
      *inRegMask |= 1ull << argTys.size();
      entryArgIdxs.tes.esGsOffset = argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offset
    } else                                            // TES acts as hardware VS
    {
      if (m_pipelineState->isTessOffChip() || enableXfb) {
        *inRegMask |= 1ull << argTys.size();
        entryArgIdxs.tes.streamOutData.streamInfo = argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context));
      }

      if (enableXfb) {
        *inRegMask |= 1ull << argTys.size();
        entryArgIdxs.tes.streamOutData.writeIndex = argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context)); // Stream-out write Index

        for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
          if (xfbStrides[i] > 0) {
            *inRegMask |= 1ull << argTys.size();
            entryArgIdxs.tes.streamOutData.streamOffsets[i] = argTys.size();
            argTys.push_back(Type::getInt32Ty(*m_context)); // Stream-out offset
          }
        }
      }

      if (m_pipelineState->isTessOffChip()) // Off-chip LDS buffer base
      {
        *inRegMask |= 1ull << argTys.size();
        entryArgIdxs.tes.offChipLdsBase = argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context));
      }
    }

    entryArgIdxs.tes.tessCoordX = argTys.size();
    argTys.push_back(Type::getFloatTy(*m_context)); // X of TessCoord (U)

    entryArgIdxs.tes.tessCoordY = argTys.size();
    argTys.push_back(Type::getFloatTy(*m_context)); // Y of TessCoord (V)

    entryArgIdxs.tes.relPatchId = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Relative patch ID

    entryArgIdxs.tes.patchId = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Patch ID

    break;
  }
  case ShaderStageGeometry: {
    *inRegMask |= 1ull << argTys.size();
    entryArgIdxs.gs.gsVsOffset = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // GS to VS offset

    *inRegMask |= 1ull << argTys.size();
    entryArgIdxs.gs.waveId = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // GS wave ID

    // TODO: We should make the arguments according to real usage.
    entryArgIdxs.gs.esGsOffsets[0] = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offset (vertex 0)

    entryArgIdxs.gs.esGsOffsets[1] = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offset (vertex 1)

    entryArgIdxs.gs.primitiveId = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Primitive ID

    entryArgIdxs.gs.esGsOffsets[2] = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offset (vertex 2)

    entryArgIdxs.gs.esGsOffsets[3] = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offset (vertex 3)

    entryArgIdxs.gs.esGsOffsets[4] = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offset (vertex 4)

    entryArgIdxs.gs.esGsOffsets[5] = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offset (vertex 5)

    entryArgIdxs.gs.invocationId = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Invocation ID
    break;
  }
  case ShaderStageFragment: {
    *inRegMask |= 1ull << argTys.size();
    entryArgIdxs.fs.primMask = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Primitive mask

    entryArgIdxs.fs.perspInterp.sample = argTys.size();
    argTys.push_back(VectorType::get(Type::getFloatTy(*m_context), 2)); // Perspective sample

    entryArgIdxs.fs.perspInterp.center = argTys.size();
    argTys.push_back(VectorType::get(Type::getFloatTy(*m_context), 2)); // Perspective center

    entryArgIdxs.fs.perspInterp.centroid = argTys.size();
    argTys.push_back(VectorType::get(Type::getFloatTy(*m_context), 2)); // Perspective centroid

    entryArgIdxs.fs.perspInterp.pullMode = argTys.size();
    argTys.push_back(VectorType::get(Type::getFloatTy(*m_context), 3)); // Perspective pull-mode

    entryArgIdxs.fs.linearInterp.sample = argTys.size();
    argTys.push_back(VectorType::get(Type::getFloatTy(*m_context), 2)); // Linear sample

    entryArgIdxs.fs.linearInterp.center = argTys.size();
    argTys.push_back(VectorType::get(Type::getFloatTy(*m_context), 2)); // Linear center

    entryArgIdxs.fs.linearInterp.centroid = argTys.size();
    argTys.push_back(VectorType::get(Type::getFloatTy(*m_context), 2)); // Linear centroid

    argTys.push_back(Type::getFloatTy(*m_context)); // Line stipple

    entryArgIdxs.fs.fragCoord.x = argTys.size();
    argTys.push_back(Type::getFloatTy(*m_context)); // X of FragCoord

    entryArgIdxs.fs.fragCoord.y = argTys.size();
    argTys.push_back(Type::getFloatTy(*m_context)); // Y of FragCoord

    entryArgIdxs.fs.fragCoord.z = argTys.size();
    argTys.push_back(Type::getFloatTy(*m_context)); // Z of FragCoord

    entryArgIdxs.fs.fragCoord.w = argTys.size();
    argTys.push_back(Type::getFloatTy(*m_context)); // W of FragCoord

    entryArgIdxs.fs.frontFacing = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Front facing

    entryArgIdxs.fs.ancillary = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Ancillary

    entryArgIdxs.fs.sampleCoverage = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Sample coverage

    argTys.push_back(Type::getInt32Ty(*m_context)); // Fixed X/Y

    break;
  }
  case ShaderStageCompute: {
    // Add system values in SGPR
    *inRegMask |= 1ull << argTys.size();
    entryArgIdxs.cs.workgroupId = argTys.size();
    argTys.push_back(VectorType::get(Type::getInt32Ty(*m_context), 3)); // WorkgroupId

    *inRegMask |= 1ull << argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // Multiple dispatch info, include TG_SIZE and etc.

    // Add system value in VGPR
    entryArgIdxs.cs.localInvocationId = argTys.size();
    argTys.push_back(VectorType::get(Type::getInt32Ty(*m_context), 3)); // LocalInvociationId
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  return FunctionType::get(Type::getVoidTy(*m_context), argTys, false);
}

// =====================================================================================================================
// A compute shader is allowed to have subfunctions. Here we need to mutate all the subfunctions
// to add on the same args that the shader has, so they are passed around to all code.
//
// @param entryPoint : Shader entry-point function
void PatchEntryPointMutate::mutateSubfunctions(Function *entryPoint) {
  // Mutate subfunctions by adding pEntryTy's args to the front.
  FunctionType *entryTy = entryPoint->getFunctionType();
  bool haveNonCallUses = false;
  SmallVector<Function *, 4> oldFuncs;
  std::map<Function *, Function *> funcMap;
  std::map<Type *, Type *> typeMap;
  SmallVector<CallInst *, 8> calls;
  for (auto &func : *m_module) {
    if (!func.isDeclaration() && func.getLinkage() == GlobalValue::InternalLinkage)
      oldFuncs.push_back(&func);
  }
  for (Function *oldFunc : oldFuncs) {
    // Create the new function type.
    auto oldFuncTy = oldFunc->getFunctionType();
    SmallVector<Type *, 8> paramTys;
    paramTys.insert(paramTys.end(), entryTy->params().begin(), entryTy->params().end());
    paramTys.insert(paramTys.end(), oldFuncTy->params().begin(), oldFuncTy->params().end());
    auto newFuncTy = FunctionType::get(oldFuncTy->getReturnType(), paramTys, oldFuncTy->isVarArg());
    // Create the new function.
    auto newFunc = Function::Create(newFuncTy, GlobalValue::InternalLinkage, oldFunc->getAddressSpace(), "", m_module);
    newFunc->takeName(oldFunc);
    funcMap[oldFunc] = newFunc;
    typeMap[oldFuncTy] = newFuncTy;

    // Transfer the code onto the new function.
    while (!oldFunc->empty()) {
      BasicBlock *block = &oldFunc->front();
      block->removeFromParent();
      block->insertInto(newFunc);
    }

    // Copy attributes and shader stage from the old function.
    newFunc->setAttributes(oldFunc->getAttributes());
    newFunc->addFnAttr(Attribute::AlwaysInline);
    setShaderStage(newFunc, getShaderStage(oldFunc));

    // Copy "inreg" arg attributes from the entry-point, to indicate which args are uniform.
    for (unsigned idx = 0, end = entryPoint->arg_size(); idx != end; ++idx) {
      if (entryPoint->getArg(idx)->hasInRegAttr())
        newFunc->getArg(idx)->addAttr(Attribute::InReg);
    }

    // Transfer uses of old args to new args.
    for (unsigned idx = 0, end = oldFunc->arg_size(); idx != end; ++idx) {
      Argument *oldArg = oldFunc->getArg(idx);
      Argument *newArg = newFunc->getArg(idx + entryTy->params().size());
      newArg->setName(oldArg->getName());
      oldArg->replaceAllUsesWith(newArg);
    }

    // Find uses. Remember direct calls; insert a bitcast for non-call uses (which occur for indirect
    // calls).
    SmallVector<Use *, 4> nonCallUses;
    for (auto &use : oldFunc->uses()) {
      User *user = use.getUser();
      if (auto call = dyn_cast<CallInst>(user)) {
        if (call->isCallee(&use)) {
          // Use in direct call.
          calls.push_back(call);
          continue;
        }
      }
      nonCallUses.push_back(&use);
    }
    if (!nonCallUses.empty()) {
      auto castNewFunc = ConstantExpr::getBitCast(newFunc, oldFunc->getType());
      haveNonCallUses = true;
      for (Use *use : nonCallUses)
        *use = castNewFunc;
    }
  }

  if (haveNonCallUses) {
    // There are some non-call uses of the functions, i.e. some indirect calls. That means we have to
    // scan the code to find the calls.
    calls.clear();
    for (Function &func : *m_module) {
      for (BasicBlock &block : func) {
        for (Instruction &inst : block) {
          if (auto call = dyn_cast<CallInst>(&inst))
            calls.push_back(call);
        }
      }
    }
  }

  // Mutate the calls.
  for (CallInst *call : calls) {
    // Mutate the call by adding extra args on the front using the enclosing func's corresponding args.
    IRBuilder<> builder(call);
    Value *callee = call->getCalledOperand();
    if (auto calledFunc = dyn_cast<Function>(callee)) {
      // This is a direct call.
      auto it = funcMap.find(calledFunc);
      if (it == funcMap.end()) {
        // It is a direct call to a function that was not mutated, which must be an external
        // (e.g. an intrinsic). Do not mutate the call.
        continue;
      }
      callee = it->second;
    } else {
      // For an indirect call, the new callee needs to be a bitcast of the old one.
      callee = builder.CreateBitCast(
          callee, typeMap[call->getFunctionType()]->getPointerTo(callee->getType()->getPointerAddressSpace()));
    }

    SmallVector<Value *, 8> args;
    for (unsigned idx = 0; idx != entryTy->params().size(); ++idx)
      args.push_back(call->getFunction()->getArg(idx));
    for (unsigned idx = 0; idx != call->getNumArgOperands(); ++idx)
      args.push_back(call->getArgOperand(idx));
    auto newCall = builder.CreateCall(callee, args);
    call->replaceAllUsesWith(newCall);
    call->eraseFromParent();
  }

  // Erase the old functions.
  for (Function *oldFunc : oldFuncs)
    oldFunc->eraseFromParent();
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of LLVM patching opertions for entry-point mutation.
INITIALIZE_PASS(PatchEntryPointMutate, DEBUG_TYPE, "Patch LLVM for entry-point mutation", false, false)
