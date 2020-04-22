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

#include "Gfx6Chip.h"
#include "Gfx9Chip.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
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

namespace {

// =====================================================================================================================
// The entry-point mutation pass
class PatchEntryPointMutate : public Patch {
public:
  PatchEntryPointMutate();
  PatchEntryPointMutate(const PatchEntryPointMutate &) = delete;
  PatchEntryPointMutate &operator=(const PatchEntryPointMutate &) = delete;

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineStateWrapper>();
    analysisUsage.addRequired<PipelineShaders>();
    // Does not preserve PipelineShaders because it replaces the entrypoints.
  }

  virtual bool runOnModule(Module &module) override;

  static char ID; // ID of this pass

private:
  // A shader entry-point user data argument
  struct UserDataArg {
    UserDataArg(Type *argTy, unsigned userDataValue = static_cast<unsigned>(Util::Abi::UserDataMapping::Invalid),
                unsigned *argIndex = nullptr, bool isPadding = false)
        : argTy(argTy), userDataValue(userDataValue), argIndex(argIndex), isPadding(isPadding), mustSpill(false) {
      if (isa<PointerType>(argTy))
        argDwordSize = argTy->getPointerAddressSpace() == ADDR_SPACE_CONST_32BIT ? 1 : 2;
      else
        argDwordSize = argTy->getPrimitiveSizeInBits() / 32;
    }

    UserDataArg(Type *argTy, Util::Abi::UserDataMapping userDataValue, unsigned *argIndex = nullptr,
                bool isPadding = false)
        : UserDataArg(argTy, static_cast<unsigned>(userDataValue), argIndex, isPadding) {}

    Type *argTy;            // IR type of the argument
    unsigned argDwordSize;  // Size of argument in dwords
    unsigned userDataValue; // PAL metadata user data value, ~0U (Util::Abi::UserDataMapping::Invalid) for none
    unsigned *argIndex;     // Where to store arg index once it is allocated, nullptr for none
    bool isPadding;         // Whether this is a padding arg to maintain fixed layout
    bool mustSpill;         // Whether this is an arg that must be spilled
  };

  void processShader();

  FunctionType *generateEntryPointType(uint64_t *inRegMask) const;

  void addSpecialUserDataArgs(SmallVectorImpl<UserDataArg> &userDataArgs,
                              SmallVectorImpl<UserDataArg> &specialUserDataArgs, IRBuilder<> &builder) const;
  void addUserDataArgs(SmallVectorImpl<UserDataArg> &userDataArgs, IRBuilder<> &builder) const;
  void determineUnspilledUserDataArgs(ArrayRef<UserDataArg> userDataArgs, ArrayRef<UserDataArg> specialUserDataArgs,
                                      IRBuilder<> &builder, SmallVectorImpl<UserDataArg> &unspilledArgs) const;

  uint64_t pushFixedShaderArgTys(SmallVectorImpl<Type *> &argTys) const;

  bool isResourceNodeActive(const ResourceNode *node, bool isRootNode) const;

  bool m_hasTs;                             // Whether the pipeline has tessllation shader
  bool m_hasGs;                             // Whether the pipeline has geometry shader
  PipelineState *m_pipelineState = nullptr; // Pipeline state from PipelineStateWrapper pass
};

} // anonymous namespace

// =====================================================================================================================
// Initializes static members.
char PatchEntryPointMutate::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching opertions for entry-point mutation
ModulePass *lgc::createPatchEntryPointMutate() {
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

  // Create new function, empty for now.
  Function *entryPoint = Function::Create(entryPointTy, GlobalValue::ExternalLinkage, "", m_module);
  entryPoint->setCallingConv(origEntryPoint->getCallingConv());
  entryPoint->takeName(origEntryPoint);

  // Transfer code from old function to new function.
  while (!origEntryPoint->empty()) {
    BasicBlock *block = &origEntryPoint->front();
    block->removeFromParent();
    block->insertInto(entryPoint);
  }

  // Copy attributes and shader stage from the old function.
  entryPoint->setAttributes(origEntryPoint->getAttributes());
  entryPoint->addFnAttr(Attribute::NoUnwind);
  setShaderStage(entryPoint, getShaderStage(origEntryPoint));

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
// Generates the type for the new entry-point based on already-collected info.
// This is what decides what SGPRs and VGPRs are passed to the shader at wave dispatch:
//
// * (For a GFX9+ merged shader or NGG primitive shader, the 8 system SGPRs at the start are not accounted for here.)
// * The "user data" SGPRs, up to 32 (GFX9+ non-compute shader) or 16 (compute shader or <=GFX8). Many of the values
//   here are pointers, but are passed as a single 32-bit register and then expanded to 64-bit in the shader code:
//   - The "global information table", containing various descriptors such as the inter-shader rings
//   - The "per-shader table", which is added here but appears to be unused
//   - The streamout table if needed
//   - Nodes from the root user data layout, including pointers to descriptor sets. In a compute shader, these
//     must have the same layout as the root user data layout, even if that would mean leaving gaps in register
//     usage, and are limited to 10 SGPRs s2-s11
//   - For a compute shader, the spill table pointer if needed, which always goes into s12. (This is possibly
//     not a strict requirement of PAL, but it might avoid extraneous HW register writes.)
//   - For a compute shader, the NumWorkgroupsPtr register pair if needed, which must start at a 2-aligned
//     register number, although it can be earlier than s14 if there is no spill table pointer
//   - Various other system values set up by PAL, such as the vertex buffer table and the vertex base index
//   - For a graphics shader, the spill table pointer if needed. This is typically in the last register
//     (s15 or s31), but not necessarily.
// * The system value SGPRs and VGPRs determined by hardware, some of which are enabled or disabled by bits in SPI
//   registers.
//
// In GFX9+ shader merging, shaders have not yet been merged, and this function is called for each
// unmerged shader stage. The code here needs to ensure that it gets the same SGPR user data layout for
// both shaders that are going to be merged (VS-HS, VS-GS if no tessellation, ES-GS).
//
// @param [out] inRegMask : "Inreg" bit mask for the arguments, with a bit set to indicate that the corresponding
//                          arg needs to have an "inreg" attribute to put the arg into SGPRs rather than VGPRs
// @return : The newly-constructed function type
//
FunctionType *PatchEntryPointMutate::generateEntryPointType(uint64_t *inRegMask) const {
  SmallVector<Type *, 8> argTys;

  IRBuilder<> builder(*m_context);
  auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
  auto &entryArgIdxs = intfData->entryArgIdxs;
  entryArgIdxs.initialized = true;

  // First we collect the user data args in two vectors:
  // - userDataArgs: global table, per-shader table and streamout table, followed by the nodes from the root user
  //   data layout (excluding vertex buffer and streamout tables). Some of these are marked mustSpill; some of them
  //   may need to be spilled anyway due to running out of entry SGPRs
  // - specialUserDataArgs: special values that go at the end, such as ViewId.
  //
  // The UserDataArg for each arg pushed into these vectors contains:
  // - argTy: The IR type of the arg
  // - argDwordSize: Size of the arg in dwords
  // - userDataValue: The PAL metadata value to be passed to PalMetadata::setUserDataEntry, or Invalid for none
  // - argIndex: Pointer to the location where we will store the actual arg number, or nullptr
  // - isPadding: Whether this is a padding argument for compute shader fixed layout
  // - mustSpill: The node must be loaded from the spill table; do not attempt to unspill

  SmallVector<UserDataArg, 8> userDataArgs;
  SmallVector<UserDataArg, 4> specialUserDataArgs;

  // Global internal table
  userDataArgs.push_back(UserDataArg(builder.getInt32Ty(), Util::Abi::UserDataMapping::GlobalTable));

  // Per-shader table
  // TODO: We need add per shader table per real usage after switch to PAL new interface.
  // if (pResUsage->perShaderTable)
  userDataArgs.push_back(UserDataArg(builder.getInt32Ty()));

  addSpecialUserDataArgs(userDataArgs, specialUserDataArgs, builder);

  addUserDataArgs(userDataArgs, builder);

  // Determine which user data args are going to be "unspilled", and put them in unspilledArgs.
  SmallVector<UserDataArg, 8> unspilledArgs;
  determineUnspilledUserDataArgs(userDataArgs, specialUserDataArgs, builder, unspilledArgs);

  // Scan unspilledArgs: for each one:
  // * add it to the arg type array
  // * set user data PAL metadata
  // * store the arg index into the pointer provided to the xxxArgs.push().
  unsigned userDataIdx = 0;
  for (const auto &userDataArg : unspilledArgs) {
    if (userDataArg.argIndex)
      *userDataArg.argIndex = argTys.size();
    argTys.push_back(userDataArg.argTy);
    unsigned dwordSize = userDataArg.argDwordSize;
    if (userDataArg.userDataValue != static_cast<unsigned>(Util::Abi::UserDataMapping::Invalid)) {
      m_pipelineState->getPalMetadata()->setUserDataEntry(m_shaderStage, userDataIdx, userDataArg.userDataValue,
                                                          dwordSize);
    }
    userDataIdx += dwordSize;
  }

  intfData->userDataCount = userDataIdx;
  *inRegMask = (1ull << argTys.size()) - 1;

  // Push the fixed system (not user data) register args.
  *inRegMask |= pushFixedShaderArgTys(argTys);

  return FunctionType::get(Type::getVoidTy(*m_context), argTys, false);
}

// =====================================================================================================================
// Add a UserDataArg to the appropriate vector for each special argument (e.g. ViewId) needed in user data SGPRs
//
// @param userDataArgs : Vector to add args to when they need to go before user data nodes (just streamout)
// @param specialUserDataArgs : Vector to add args to when they need to go after user data nodes (all the rest)
// @param builder : IRBuilder to get types from
void PatchEntryPointMutate::addSpecialUserDataArgs(SmallVectorImpl<UserDataArg> &userDataArgs,
                                                   SmallVectorImpl<UserDataArg> &specialUserDataArgs,
                                                   IRBuilder<> &builder) const {

  auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
  auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);
  auto &entryArgIdxs = intfData->entryArgIdxs;
  auto &builtInUsage = resUsage->builtInUsage;
  bool enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;
  bool enableNgg = m_pipelineState->isGraphics() ? m_pipelineState->getNggControl()->enableNgg : false;

  // See if there is a vertex buffer and/or streamout in the root user data layout.
  bool reserveVertexBufferTable = false;
  bool reserveStreamOutTable = false;
  for (auto &node : m_pipelineState->getUserDataNodes()) {
    if (node.type == ResourceNodeType::IndirectUserDataVaPtr) {
      reserveVertexBufferTable = true;
    } else if (node.type == ResourceNodeType::StreamOutTableVaPtr) {
      // Only the last shader stage before fragment (ignoring copy shader) needs a stream out table.
      if (m_shaderStage == m_pipelineState->getLastVertexProcessingStage() || m_shaderStage == ShaderStageGeometry)
        reserveStreamOutTable = true;
      else if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 9) {
        // On GFX9+, the shader stage that the last shader is merged in to needs a stream out
        // table, to ensure that the merged shader gets one.
        if (m_shaderStage == ShaderStageTessEval || (m_shaderStage == ShaderStageVertex && !m_hasTs))
          reserveStreamOutTable = true;
      }
    }
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
    // merged shader, we place it prior to any other special user data. We only add it to PAL metadata
    // for VS, not TCS.
    if (enableMultiView) {
      auto userDataValue = Util::Abi::UserDataMapping::Invalid;
      if (m_shaderStage == ShaderStageVertex)
        userDataValue = Util::Abi::UserDataMapping::ViewId;
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), userDataValue, &entryArgIdxs.vs.viewIndex));
    }

    // NOTE: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent
    // with PAL's GS on-chip behavior (VS is in NGG primitive shader).
    const auto gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
    if ((gfxIp.major >= 9 && (m_pipelineState->isGsOnChip() && cl::InRegEsGsLdsSize)) || (enableNgg && !m_hasTs)) {
      auto userDataValue = Util::Abi::UserDataMapping::EsGsLdsSize;
      // For a standalone TCS (which can only happen in unit testing, not in a real pipeline), don't add
      // the PAL metadata for it, for consistency with the old code.
      if (!m_pipelineState->hasShaderStage(ShaderStageVertex))
        userDataValue = Util::Abi::UserDataMapping::Invalid;
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), userDataValue));
    }

    // Vertex buffer table. Only add to PAL metadata for VS, in case this is a <=GFX8 unmerged TCS.
    if (reserveVertexBufferTable) {
      auto userDataValue = Util::Abi::UserDataMapping::Invalid;
      if (m_shaderStage == ShaderStageVertex)
        userDataValue = Util::Abi::UserDataMapping::VertexBufferTable;
      specialUserDataArgs.push_back(
          UserDataArg(builder.getInt32Ty(), userDataValue, &currIntfData->entryArgIdxs.vs.vbTablePtr));
    }

    // Base vertex and base instance. Only add to PAL metadata for VS, in case this is a <=GFX8 unmerged TCS.
    if (currResUsage->builtInUsage.vs.baseVertex || currResUsage->builtInUsage.vs.baseInstance) {
      auto userDataValue = Util::Abi::UserDataMapping::Invalid;
      if (m_shaderStage == ShaderStageVertex)
        userDataValue = Util::Abi::UserDataMapping::BaseVertex;
      specialUserDataArgs.push_back(
          UserDataArg(builder.getInt32Ty(), userDataValue, &currIntfData->entryArgIdxs.vs.baseVertex));
      if (m_shaderStage == ShaderStageVertex)
        userDataValue = Util::Abi::UserDataMapping::BaseInstance;
      specialUserDataArgs.push_back(
          UserDataArg(builder.getInt32Ty(), userDataValue, &currIntfData->entryArgIdxs.vs.baseInstance));
    }

    // Draw index. Only add to PAL metadata for VS, in case this is a <=GFX8 unmerged TCS.
    if (currResUsage->builtInUsage.vs.drawIndex) {
      auto userDataValue = Util::Abi::UserDataMapping::Invalid;
      if (m_shaderStage == ShaderStageVertex)
        userDataValue = Util::Abi::UserDataMapping::DrawIndex;
      specialUserDataArgs.push_back(
          UserDataArg(builder.getInt32Ty(), userDataValue, &currIntfData->entryArgIdxs.vs.drawIndex));
    }

    break;
  }
  case ShaderStageTessEval: {
    // NOTE: The user data to emulate gl_ViewIndex is somewhat common. To make it consistent for GFX9
    // merged shader, we place it prior to any other special user data.
    if (enableMultiView) {
      specialUserDataArgs.push_back(
          UserDataArg(builder.getInt32Ty(), Util::Abi::UserDataMapping::ViewId, &entryArgIdxs.tes.viewIndex));
    }

    // NOTE: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent
    // with PAL's GS on-chip behavior (TES is in NGG primitive shader).
    if (enableNgg)
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), Util::Abi::UserDataMapping::EsGsLdsSize));
    break;
  }
  case ShaderStageGeometry: {
    // NOTE: The user data to emulate gl_ViewIndex is somewhat common. To make it consistent for GFX9
    // merged shader, we place it prior to any other special user data.
    if (enableMultiView) {
      specialUserDataArgs.push_back(
          UserDataArg(builder.getInt32Ty(), Util::Abi::UserDataMapping::ViewId, &entryArgIdxs.gs.viewIndex));
    }

    // NOTE: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent
    // with PAL's GS on-chip behavior. i.e. GS is GFX8, or GS is in NGG primitive shader
    if ((m_pipelineState->isGsOnChip() && cl::InRegEsGsLdsSize) || enableNgg)
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), Util::Abi::UserDataMapping::EsGsLdsSize));
    break;
  }
  case ShaderStageCompute: {
    // Emulate gl_NumWorkGroups via user data registers.
    // Later code needs to ensure that this starts on an even numbered register.
    if (builtInUsage.cs.numWorkgroups) {
      auto numWorkgroupsPtrTy = PointerType::get(VectorType::get(builder.getInt32Ty(), 3), ADDR_SPACE_CONST);
      specialUserDataArgs.push_back(
          UserDataArg(numWorkgroupsPtrTy, Util::Abi::UserDataMapping::Workgroup, &entryArgIdxs.cs.numWorkgroupsPtr));
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

  // Allocate register for stream-out buffer table, to go before the user data node args (unlike all the ones
  // above, which go after the user data node args).
  // If the user data layout has an entry for the stream-out buffer table, but it is not used, then we
  // still reserve space for it but we do not add it to PAL metadata. This is probably unnecessary, but it
  // is what the old code did. On <=GFX8, we add it to PAL metadata even if unused.
  if (reserveStreamOutTable) {
    auto userDataValue = Util::Abi::UserDataMapping::Invalid;
    if (resUsage->inOutUsage.enableXfb || m_pipelineState->getTargetInfo().getGfxIpVersion().major < 9)
      userDataValue = Util::Abi::UserDataMapping::StreamOutTable;
    switch (m_shaderStage) {
    case ShaderStageVertex:
      userDataArgs.push_back(
          UserDataArg(builder.getInt32Ty(), userDataValue, &intfData->entryArgIdxs.vs.streamOutData.tablePtr));
      break;
    case ShaderStageTessEval:
      userDataArgs.push_back(
          UserDataArg(builder.getInt32Ty(), userDataValue, &intfData->entryArgIdxs.tes.streamOutData.tablePtr));
      break;
    // Allocate dummy stream-out register for Geometry shader
    case ShaderStageGeometry:
      userDataArgs.push_back(UserDataArg(builder.getInt32Ty()));
      break;
    default:
      llvm_unreachable("Should never be called!");
      break;
    }
  }
}

// =====================================================================================================================
// Add a UserDataArg to the vector for each user data node needed in user data SGPRs.
//
// This function does not check if it needs to spill because of running out of SGPRs. However it does
// check if a node needs to be loaded from the spill table anyway because of its type, and in that case marks
// it as "mustSpill".
//
// This function handles CS "fixed layout" by inserting an extra padding arg before a real user data arg if
// necessary.
//
// @param userDataArgs : Vector to add args to
// @param builder : IRBuilder to get types from
void PatchEntryPointMutate::addUserDataArgs(SmallVectorImpl<UserDataArg> &userDataArgs, IRBuilder<> &builder) const {

  // Add entries from the root user data layout (not vertex buffer or streamout, and not unused ones).
  auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
  // TODO Shader compilation: This should not be conditional on isUnlinked(). Once we have shader compilation
  // without user data layout working, we can instead make it conditional on having a user data layout.
  bool useFixedLayout = m_shaderStage == ShaderStageCompute && !m_pipelineState->isUnlinked();

  // userDataIdx counts how many SGPRs we have already added, so we can tell if we need padding before
  // an arg in a compute shader. We initialize with userDataArgs.size(), relying on the fact that all the
  // ones added so far (global table, per-shader table, streamout table for graphics) use one register each.
  unsigned userDataIdx = userDataArgs.size();

  for (unsigned i = 0; i != m_pipelineState->getUserDataNodes().size(); ++i) {
    const ResourceNode &node = m_pipelineState->getUserDataNodes()[i];
    if (node.type == ResourceNodeType::IndirectUserDataVaPtr || node.type == ResourceNodeType::StreamOutTableVaPtr)
      continue;
    if (!isResourceNodeActive(&node, true))
      continue;

    unsigned *argIndex = &intfData->entryArgIdxs.resNodeValues[i];
    unsigned userDataValue = node.offsetInDwords;
    bool mustSpill = false;
    switch (node.type) {

    case ResourceNodeType::DescriptorTableVaPtr:
      if (m_pipelineState->getShaderOptions(m_shaderStage).updateDescInElf &&
          (m_shaderStage == ShaderStageFragment || m_shaderStage == ShaderStageVertex ||
           m_shaderStage == ShaderStageCompute)) {
        // Put set number to register first, will update offset after merge ELFs
        // For partial pipeline compile, only fragment shader needs to adjust offset of root descriptor
        // If there are more individual shader compile in future, we can add more stages here
        userDataValue = DescRelocMagic | node.innerTable[0].set;
      }
      break;

    case ResourceNodeType::PushConst:
    case ResourceNodeType::DescriptorBufferCompact:
      break;

    default:
      // Any other descriptor type in the root table is accessed via the spill table.
      mustSpill = true;
    }

    if (useFixedLayout && userDataIdx != node.offsetInDwords + InterfaceData::CsStartUserData) {
      // With useFixedLayout, we need a padding arg before the node's arg.
      assert(node.offsetInDwords + InterfaceData::CsStartUserData > userDataIdx);
      userDataArgs.push_back(UserDataArg(
          VectorType::get(builder.getInt32Ty(), node.offsetInDwords + InterfaceData::CsStartUserData - userDataIdx),
          Util::Abi::UserDataMapping::Invalid, nullptr, /*isPadding=*/true));
      userDataIdx = node.offsetInDwords + InterfaceData::CsStartUserData;
    }
    // Now the node arg itself.
    Type *argTy = builder.getInt32Ty();
    if (node.sizeInDwords != 1)
      argTy = VectorType::get(argTy, node.sizeInDwords);
    userDataArgs.push_back(UserDataArg(argTy, userDataValue, argIndex));
    if (mustSpill)
      userDataArgs.back().mustSpill = true;
    else
      userDataIdx += node.sizeInDwords;
  }
}

// =====================================================================================================================
// Determine which user data args are going to be "unspilled" (passed in shader entry SGPRs rather than loaded
// from spill table)
//
// @param userDataArgs : First array of UserDataArg structs for candidate args
// @param specialUserDataArgs : Second array of UserDataArg structs for candidate args
// @param builder : IRBuilder to get types from
// @param [out] unspilledArgs : Output vector of UserDataArg structs that will be "unspilled". Mostly these are
//                              copied from the input arrays, plus an extra one for the spill table pointer if
//                              needed. For compute shader fixed layout there may be extra nodes for padding.
void PatchEntryPointMutate::determineUnspilledUserDataArgs(ArrayRef<UserDataArg> userDataArgs,
                                                           ArrayRef<UserDataArg> specialUserDataArgs,
                                                           IRBuilder<> &builder,
                                                           SmallVectorImpl<UserDataArg> &unspilledArgs) const {

  auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
  SmallVector<UserDataArg, 1> spillTableArg;

  // Figure out how many sgprs we have available for userDataArgs.
  unsigned userDataEnd = 0;
  if (m_shaderStage == ShaderStageCompute) {
    // For a compute shader, s0-s1 are taken by the global table, and we need to get the user data nodes
    // into s2-s11. There are enough registers available after that for the spill table arg and
    // specialUserDataArgs up to s15, so we can ignore them here.
    userDataEnd = InterfaceData::CsStartUserData + InterfaceData::MaxCsUserDataCount;
  } else {
    // For a graphics shader, we have s0-s31 (s0-s15 for <=GFX8) for everything, so take off the number
    // of registers used by specialUserDataArgs.
    userDataEnd = m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount;
    for (auto &userDataArg : specialUserDataArgs)
      userDataEnd -= userDataArg.argDwordSize;
  }

  // See if we need to spill any user data nodes in userDataArgs, copying the unspilled ones across to unspilledArgs.
  bool useFixedLayout = m_shaderStage == ShaderStageCompute;
  bool fixedLayoutSpilling = false;
  unsigned userDataIdx = 0;

  for (const UserDataArg &userDataArg : userDataArgs) {
    unsigned afterUserDataIdx = userDataIdx + userDataArg.argDwordSize;
    if (fixedLayoutSpilling || userDataArg.mustSpill || afterUserDataIdx > userDataEnd) {
      // Spill this node. Allocate the spill table arg.
      if (spillTableArg.empty()) {
        spillTableArg.push_back(UserDataArg(builder.getInt32Ty(), Util::Abi::UserDataMapping::SpillTable,
                                            &intfData->entryArgIdxs.spillTable));
        // Only decrement the number of available sgprs in a graphics shader. In a compute shader,
        // the spill table arg goes in s12, beyond the s2-s11 range allowed for user data.
        if (m_shaderStage != ShaderStageCompute)
          --userDataEnd;

        if (userDataIdx > userDataEnd) {
          // We over-ran the available SGPRs by filling them up and then realizing we needed a spill table pointer.
          // Remove the last unspilled node (and any padding arg before that), and ensure that spill usage is
          // set correctly so that PAL metadata spill threshold is correct.
          userDataIdx -= unspilledArgs.back().argDwordSize;
          m_pipelineState->getPalMetadata()->setUserDataSpillUsage(unspilledArgs.back().userDataValue);
          unspilledArgs.pop_back();
          if (!unspilledArgs.empty() && unspilledArgs.back().isPadding) {
            userDataIdx -= unspilledArgs.back().argDwordSize;
            unspilledArgs.pop_back();
          }
        }
      }
      // Ensure that spillUsage includes this offset. (We might be on a compute shader padding node, in which
      // case userDataArg.userDataValue is Invalid, and this call has no effect.)
      m_pipelineState->getPalMetadata()->setUserDataSpillUsage(userDataArg.userDataValue);

      if (!userDataArg.mustSpill && useFixedLayout) {
        // On a compute shader, if we spilled because we ran out of SGPRs (rather than because the node is
        // marked "mustspill"), stop trying to allocate nodes to SGPRs. If we didn't do this, a later node
        // that is smaller than the current one might succeed in not spilling, but that would be wrong
        // because it would not have the right padding before it for fixed layout.
        fixedLayoutSpilling = true;
      }

      continue;
    }
    // Keep this node on the unspilled list.
    userDataIdx = afterUserDataIdx;
    unspilledArgs.push_back(userDataArg);
  }

  // Remove trailing padding nodes (compute shader).
  while (!unspilledArgs.empty() && unspilledArgs.back().isPadding) {
    userDataIdx -= unspilledArgs.back().argDwordSize;
    unspilledArgs.pop_back();
  }

  // Add the spill table pointer and the special args to unspilledArgs. How that is done is different between
  // a compute shader and a graphics shader.
  if (m_shaderStage == ShaderStageCompute) {
    // For compute shader:
    // If we need the spill table pointer, it must go into s12. Insert padding if necessary.
    if (!spillTableArg.empty()) {
      if (userDataIdx != userDataEnd) {
        assert(userDataIdx <= userDataEnd);
        unspilledArgs.push_back(UserDataArg(VectorType::get(builder.getInt32Ty(), userDataEnd - userDataIdx),
                                            Util::Abi::UserDataMapping::Invalid, nullptr, /*padding=*/true));
      }
      unspilledArgs.push_back(spillTableArg.front());
      userDataIdx = userDataEnd + 1;
    }
    if (!specialUserDataArgs.empty()) {
      // The special args start with workgroupSize, which needs to start at a 2-aligned reg. Insert a single padding
      // reg if needed.
      if (userDataIdx & 1) {
        unspilledArgs.push_back(
            UserDataArg(builder.getInt32Ty(), Util::Abi::UserDataMapping::Invalid, nullptr, /*padding=*/true));
      }
      unspilledArgs.insert(unspilledArgs.end(), specialUserDataArgs.begin(), specialUserDataArgs.end());
    }
  } else {
    // For graphics shader: add the special user data args, then the spill table pointer (if any).
    unspilledArgs.insert(unspilledArgs.end(), specialUserDataArgs.begin(), specialUserDataArgs.end());
    unspilledArgs.insert(unspilledArgs.end(), spillTableArg.begin(), spillTableArg.end());
  }
}

// =====================================================================================================================
// Push argument types for fixed system shader arguments
//
// @param [in/out] argTys : Argument types vector to add to
// @return : Bitmap with bits set for SGPR arguments so caller can set "inreg" attribute on the args
uint64_t PatchEntryPointMutate::pushFixedShaderArgTys(SmallVectorImpl<Type *> &argTys) const {
  uint64_t inRegMask = 0;
  auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
  auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);
  auto &entryArgIdxs = intfData->entryArgIdxs;
  const auto &xfbStrides = resUsage->inOutUsage.xfbStrides;
  const bool enableXfb = resUsage->inOutUsage.enableXfb;

  switch (m_shaderStage) {
  case ShaderStageVertex: {
    if (m_hasGs && !m_hasTs) // VS acts as hardware ES
    {
      inRegMask |= 1ull << argTys.size();
      entryArgIdxs.vs.esGsOffset = argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offset
    } else if (!m_hasGs && !m_hasTs)                  // VS acts as hardware VS
    {
      if (enableXfb) // If output to stream-out buffer
      {
        inRegMask |= 1ull << argTys.size();
        entryArgIdxs.vs.streamOutData.streamInfo = argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context)); // Stream-out info (ID, vertex count, enablement)

        inRegMask |= 1ull << argTys.size();
        entryArgIdxs.vs.streamOutData.writeIndex = argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context)); // Stream-out write Index

        for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
          if (xfbStrides[i] > 0) {
            inRegMask |= 1ull << argTys.size();
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
      inRegMask |= 1ull << argTys.size();
      entryArgIdxs.tcs.offChipLdsBase = argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context)); // Off-chip LDS buffer base
    }

    inRegMask |= 1ull << argTys.size();
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
        inRegMask |= 1ull << argTys.size();
        entryArgIdxs.tes.offChipLdsBase = argTys.size(); // Off-chip LDS buffer base
        argTys.push_back(Type::getInt32Ty(*m_context));

        inRegMask |= 1ull << argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context)); // If is_offchip enabled
      }
      inRegMask |= 1ull << argTys.size();
      entryArgIdxs.tes.esGsOffset = argTys.size();
      argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offset
    } else                                            // TES acts as hardware VS
    {
      if (m_pipelineState->isTessOffChip() || enableXfb) {
        inRegMask |= 1ull << argTys.size();
        entryArgIdxs.tes.streamOutData.streamInfo = argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context));
      }

      if (enableXfb) {
        inRegMask |= 1ull << argTys.size();
        entryArgIdxs.tes.streamOutData.writeIndex = argTys.size();
        argTys.push_back(Type::getInt32Ty(*m_context)); // Stream-out write Index

        for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
          if (xfbStrides[i] > 0) {
            inRegMask |= 1ull << argTys.size();
            entryArgIdxs.tes.streamOutData.streamOffsets[i] = argTys.size();
            argTys.push_back(Type::getInt32Ty(*m_context)); // Stream-out offset
          }
        }
      }

      if (m_pipelineState->isTessOffChip()) // Off-chip LDS buffer base
      {
        inRegMask |= 1ull << argTys.size();
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
    inRegMask |= 1ull << argTys.size();
    entryArgIdxs.gs.gsVsOffset = argTys.size();
    argTys.push_back(Type::getInt32Ty(*m_context)); // GS to VS offset

    inRegMask |= 1ull << argTys.size();
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
    inRegMask |= 1ull << argTys.size();
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
    inRegMask |= 1ull << argTys.size();
    entryArgIdxs.cs.workgroupId = argTys.size();
    argTys.push_back(VectorType::get(Type::getInt32Ty(*m_context), 3)); // WorkgroupId

    inRegMask |= 1ull << argTys.size();
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

  return inRegMask;
}

// =====================================================================================================================
// Initializes the pass of LLVM patching opertions for entry-point mutation.
INITIALIZE_PASS(PatchEntryPointMutate, DEBUG_TYPE, "Patch LLVM for entry-point mutation", false, false)
