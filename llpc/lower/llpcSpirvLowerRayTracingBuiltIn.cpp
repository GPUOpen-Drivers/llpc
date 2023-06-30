/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerRayTracingBuiltIn.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerRayTracingBuiltIn.
 ***********************************************************************************************************************
 */

#include "llpcSpirvLowerRayTracingBuiltIn.h"
#include "SPIRVInternal.h"
#include "gpurt-compiler.h"
#include "llpcContext.h"
#include "llpcRayTracingContext.h"
#include "llpcSpirvLowerRayTracing.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/Builder.h"
#include "lgc/Pipeline.h"

#define DEBUG_TYPE "llpc-spirv-lower-ray-tracing-builtin"

using namespace llvm;
using namespace Llpc;
using namespace spv;

namespace RtName {
extern const char *TraceRayHitAttributes;
extern const char *TraceRaySetTraceParams;
extern const char *ShaderTable;
} // namespace RtName

namespace Llpc {

// =====================================================================================================================
SpirvLowerRayTracingBuiltIn::SpirvLowerRayTracingBuiltIn() {
  memset(m_traceParams, 0, sizeof(m_traceParams));
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerRayTracingBuiltIn::run(Module &module, ModuleAnalysisManager &analysisManager) {
  runImpl(module);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool SpirvLowerRayTracingBuiltIn::runImpl(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Ray-Tracing-BuiltIn\n");
  m_module = &module;
  m_context = static_cast<Context *>(&m_module->getContext());
  m_builder = m_context->getBuilder();
  m_shaderStage = getShaderStageFromModule(m_module);
  const auto *rtState = m_context->getPipelineContext()->getRayTracingState();

  lgc::ComputeShaderMode mode = {};
  mode.workgroupSizeX = rtState->threadGroupSizeX;
  mode.workgroupSizeY = rtState->threadGroupSizeY;
  mode.workgroupSizeZ = rtState->threadGroupSizeZ;
  lgc::Pipeline::setComputeShaderMode(module, mode);

  for (auto funcIt = module.begin(), funcEnd = module.end(); funcIt != funcEnd;) {
    Function *func = &*funcIt++;
    if (func->getLinkage() == GlobalValue::ExternalLinkage && !func->empty()) {
      if (func->getName().startswith(module.getName()))
        m_entryPoint = func;
      else {
        func->dropAllReferences();
        func->eraseFromParent();
      }
    }
  }

  assert(m_entryPoint);

  auto traceParamstrLen = strlen(RtName::TraceRaySetTraceParams);
  Instruction *insertPos = &*(m_entryPoint->begin()->getFirstNonPHIOrDbgOrAlloca());
  for (auto globalIt = m_module->global_begin(), end = m_module->global_end(); globalIt != end;) {
    GlobalVariable *global = &*globalIt++;
    if (global->getType()->getAddressSpace() != SPIRAS_Private)
      continue;

    if (global->getName().startswith(RtName::TraceRaySetTraceParams)) {
      int index = 0;
      global->getName().substr(traceParamstrLen).consumeInteger(0, index);
      m_traceParams[index] = global;
    }
  }

  for (auto globalIt = m_module->global_begin(), end = m_module->global_end(); globalIt != end;) {
    GlobalVariable *global = &*globalIt++;
    if (global->getType()->getAddressSpace() != SPIRAS_Input)
      continue;

    Value *input = processBuiltIn(global, insertPos);
    if (!input)
      continue;

    removeConstantExpr(m_context, global);
    for (auto user = global->user_begin(), end = global->user_end(); user != end; ++user) {
      // NOTE: "Getelementptr" and "bitcast" will propagate the address space of pointer value (input variable)
      // to the element pointer value (destination). We have to clear the address space of this element pointer
      // value. The original pointer value has been lowered and therefore the address space is invalid now.
      Instruction *inst = dyn_cast<Instruction>(*user);
      if (inst) {
        Type *instTy = inst->getType();
        if (isa<PointerType>(instTy) && instTy->getPointerAddressSpace() == SPIRAS_Input) {
          assert(isa<GetElementPtrInst>(inst) || isa<BitCastInst>(inst));
          Type *newInstTy = PointerType::getWithSamePointeeType(cast<PointerType>(instTy), SPIRAS_Private);
          inst->mutateType(newInstTy);
        }
      }
    }

    global->mutateType(input->getType()); // To clear address space for pointer to make replacement valid
    global->replaceAllUsesWith(input);
    global->eraseFromParent();
  }

  LLVM_DEBUG(dbgs() << "After the pass Spirv-Lower-Ray-Tracing-BuiltIn " << module);
  return true;
}

// =====================================================================================================================
// Processes ray tracing "call" builtIn instruction.
//
// @param global : Global variable
// @param insertPos : Where to insert instructions
Value *SpirvLowerRayTracingBuiltIn::processBuiltIn(GlobalVariable *global, Instruction *insertPos) {
  ShaderInOutMetadata inputMeta = {};
  MDNode *metaNode = global->getMetadata(gSPIRVMD::InOut);
  auto meta = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));
  unsigned startOperand = 0;
  Type *globalTy = global->getValueType();
  if (globalTy->isArrayTy()) {
    assert(meta->getNumOperands() == 4);
    startOperand += 2;
  }
  inputMeta.U64All[0] = cast<ConstantInt>(meta->getOperand(startOperand))->getZExtValue();
  inputMeta.U64All[1] = cast<ConstantInt>(meta->getOperand(startOperand + 1))->getZExtValue();
  assert(inputMeta.IsBuiltIn);

  unsigned builtInId = inputMeta.Value;
  Value *input = nullptr;
  m_builder->SetInsertPoint(insertPos);
  bool nonRayTracingBuiltIn = false;
  switch (builtInId) {
  case BuiltInLaunchIdKHR: {
    auto builtIn = lgc::BuiltInGlobalInvocationId;
    lgc::InOutInfo inputInfo = {};
    input = m_builder->CreateReadBuiltInInput(builtIn, inputInfo, nullptr, nullptr, "");
    break;
  }
  case BuiltInPrimitiveId: {
    input = m_traceParams[TraceParam::PrimitiveIndex];
    break;
  }
  case BuiltInHitKindKHR: {
    input = m_traceParams[TraceParam::Kind];
    break;
  }
  case BuiltInIncomingRayFlagsKHR: {
    input = m_traceParams[TraceParam::RayFlags];
    break;
  }
  case BuiltInRayTminKHR: {
    input = m_traceParams[TraceParam::TMin];
    break;
  }
  case BuiltInWorldRayOriginKHR: {
    input = m_traceParams[TraceParam::Origin];
    break;
  }
  case BuiltInWorldRayDirectionKHR: {
    input = m_traceParams[TraceParam::Dir];
    break;
  }
  case BuiltInRayGeometryIndexKHR: {
    input = m_traceParams[TraceParam::GeometryIndex];
    break;
  }
  case BuiltInHitTNV:
  case BuiltInRayTmaxKHR: {
    input = m_traceParams[TraceParam::TMax];
    break;
  }
  case BuiltInCullMaskKHR: {
    input = m_traceParams[TraceParam::InstanceInclusionMask];
    break;
  }
  case BuiltInHitTriangleVertexPositionsKHR: {
    input = m_traceParams[TraceParam::HitTriangleVertexPositions];
    break;
  }
  case BuiltInObjectToWorldKHR:
  case BuiltInWorldToObjectKHR:
  case BuiltInObjectRayOriginKHR:
  case BuiltInObjectRayDirectionKHR:
  case BuiltInInstanceCustomIndexKHR:
  case BuiltInInstanceId:
    break;
  default: {
    nonRayTracingBuiltIn = true;
    break;
  }
  }

  if (nonRayTracingBuiltIn)
    return nullptr;

  const auto &dataLayout = m_module->getDataLayout();

  if (!input) {
    // Note: allocate proxy for the BuiltInObjectToWorldKHR, BuiltInWorldToObjectKHR, BuiltInObjectRayOriginKHR,
    // BuiltInObjectRayDirectionKHR, BuiltInInstanceCustomIndexKHR, BuiltInInstanceId these builtIn are processed in the
    // previous ray-tracing pass
    auto proxy = new AllocaInst(globalTy, dataLayout.getAllocaAddrSpace(), LlpcName::InputProxyPrefix, insertPos);
    input = proxy;
  } else if (!input->getType()->isPointerTy()) {
    Instruction *inst = dyn_cast<Instruction>(input);
    auto proxy = new AllocaInst(input->getType(), dataLayout.getAllocaAddrSpace(),
                                LlpcName::InputProxyPrefix + input->getName(), inst);
    new StoreInst(input, proxy, insertPos);
    input = proxy;
  }

  return input;
}

} // namespace Llpc
