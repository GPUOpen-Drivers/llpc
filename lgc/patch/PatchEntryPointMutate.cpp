/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @brief The lgc::PatchEntryPointMutate pass determines the final user data layout of shaders.
 *
 * This consists of
 * - removing unused user data
 * - unspilling root descriptors if possible (moving from spill table into user data registers)
 * - unspilling push constants if we never need a pointer to them
 * - putting push constants into registers if no code needs a pointer to it
 * - figuring out where to put user data.
 *
 * The final user data is written into a limited number of sgprs starting with s0. If the user data does not fit in
 * there completely, the last i32 is changed to be a pointer to a spill table in memory, that contains the rest of the
 * user data.
 *
 * Root descriptors are dynamic uniform buffer descriptors in Vulkan, that can be changed without modifying a descriptor
 * set and rebuilding the pipeline. They get put into the spill table but can be unspilled.
 *
 * Special care is required for compute libraries. Similar to unlinked shader compilation, we do not know the final
 * layout for non-entrypoint shaders. For compute libraries, user data args must be passed to other functions, whose
 * implementation is unknown at compile time. Therefore, computation of user data arguments must be independent of any
 * instructions or uses. This is important, even for functions that have no calls, as we still need to compute the taken
 * arguments in a deterministic layout. For library functions, only a prefix of the user data is known at compile time.
 * There can be more user data at runtime, and that needs to be passed on to called functions. Therefore, we
 * - always pass all possible user data registers, even if they have no content for the current shader
 * - have a spill table pointer in the largest user data sgpr
 * - cannot remove unused user data as it might be used by a callee.
 ***********************************************************************************************************************
 */

#include "lgc/patch/PatchEntryPointMutate.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/AbiMetadata.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/AddressExtender.h"
#include "lgc/util/BuilderBase.h"
#include "lgc/util/CpsStackLowering.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/Analysis/AliasAnalysis.h" // for MemoryEffects
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>

#define DEBUG_TYPE "lgc-patch-entry-point-mutate"

using namespace llvm;
using namespace lgc;

namespace llvm {
namespace cl {

// -inreg-esgs-lds-size: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent with PAL's
// GS on-chip behavior. In the future, if PAL allows hardcoded ES-GS LDS size, this option could be deprecated.
opt<bool> InRegEsGsLdsSize("inreg-esgs-lds-size", desc("For GS on-chip, add esGsLdsSize in user data"), init(true));

} // namespace cl
} // namespace llvm

// =====================================================================================================================
PatchEntryPointMutate::PatchEntryPointMutate()
    : m_hasTs(false), m_hasGs(false),
      m_setInactiveChainArgId(Function::lookupIntrinsicID("llvm.amdgcn.set.inactive.chain.arg")) {
}

// =====================================================================================================================
PatchEntryPointMutate::UserDataArg::UserDataArg(llvm::Type *argTy, const llvm::Twine &name, unsigned userDataValue,
                                                unsigned *argIndex)
    : argTy(argTy), name(name.str()), userDataValue(userDataValue), argIndex(argIndex) {
  if (llvm::isa<llvm::PointerType>(argTy))
    argDwordSize = argTy->getPointerAddressSpace() == ADDR_SPACE_CONST_32BIT ? 1 : 2;
  else
    argDwordSize = argTy->getPrimitiveSizeInBits() / 32;
}

// =====================================================================================================================
PatchEntryPointMutate::UserDataArg::UserDataArg(llvm::Type *argTy, const llvm::Twine &name,
                                                UserDataMapping userDataValue, unsigned *argIndex)
    : UserDataArg(argTy, name, static_cast<unsigned>(userDataValue), argIndex) {
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchEntryPointMutate::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);
  runImpl(module, pipelineShaders, pipelineState);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param pipelineShaders : Pipeline shaders analysis result
// @param pipelineState : Pipeline state
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchEntryPointMutate::runImpl(Module &module, PipelineShadersResult &pipelineShaders,
                                    PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Entry-Point-Mutate\n");

  Patch::init(&module);

  m_pipelineState = pipelineState;

  const unsigned stageMask = m_pipelineState->getShaderStageMask();
  m_hasTs = (stageMask & (shaderStageToMask(ShaderStageTessControl) | shaderStageToMask(ShaderStageTessEval))) != 0;
  m_hasGs = (stageMask & shaderStageToMask(ShaderStageGeometry)) != 0;

  // Gather user data usage.
  gatherUserDataUsage(&module);

  // Create ShaderInputs object and gather shader input usage.
  ShaderInputs shaderInputs;
  shaderInputs.gatherUsage(module);
  setupComputeWithCalls(&module);

  if (m_pipelineState->isGraphics()) {
    // Process each shader in turn, but not the copy shader.
    for (unsigned shaderStage = 0; shaderStage < ShaderStageNativeStageCount; ++shaderStage) {
      m_entryPoint = pipelineShaders.getEntryPoint(static_cast<ShaderStage>(shaderStage));
      if (m_entryPoint) {
        // ToDo: This should always be skipped since we don't implement CPS metadata yet.
        assert(!lgc::cps::isCpsFunction(*m_entryPoint) && "CPS support not implemented yet");

        m_shaderStage = static_cast<ShaderStage>(shaderStage);
        processShader(&shaderInputs);
      }
    }
  } else {
    processComputeFuncs(&shaderInputs, module);
  }

  // Fix up user data uses to use entry args.
  fixupUserDataUses(*m_module);
  m_userDataUsage.clear();

  // Fix up shader input uses to use entry args.
  shaderInputs.fixupUses(*m_module, m_pipelineState, isComputeWithCalls());

  m_cpsShaderInputCache.clear();
  return true;
}

// =====================================================================================================================
// Split the input into pieces of i32.
static void splitIntoI32(const DataLayout &layout, IRBuilder<> &builder, ArrayRef<Value *> input,
                         SmallVector<Value *> &output) {
  for (auto *x : input) {
    Type *xType = x->getType();
    if (isa<StructType>(xType)) {
      StructType *structTy = cast<StructType>(xType);
      for (unsigned idx = 0; idx < structTy->getNumElements(); idx++)
        splitIntoI32(layout, builder, builder.CreateExtractValue(x, idx), output);
    } else if (auto *arrayTy = dyn_cast<ArrayType>(xType)) {
      auto *elemTy = arrayTy->getElementType();
      assert(layout.getTypeSizeInBits(elemTy) == 32 && "array of non-32bit type not supported");
      for (unsigned idx = 0; idx < arrayTy->getNumElements(); idx++) {
        auto *elem = builder.CreateExtractValue(x, idx);
        if (!elemTy->isIntegerTy())
          elem = builder.CreateBitCast(elem, builder.getInt32Ty());
        output.push_back(elem);
      }
    } else if (auto *vecTy = dyn_cast<FixedVectorType>(xType)) {
      Type *scalarTy = vecTy->getElementType();
      assert((scalarTy->getPrimitiveSizeInBits() & 0x3) == 0);
      unsigned scalarBytes = scalarTy->getPrimitiveSizeInBits() / 8;
      if (scalarBytes < 4) {
        // Use shufflevector for types like <i8 x 6>?
        llvm_unreachable("vector of type smaller than dword not supported yet.");
      } else {
        for (unsigned idx = 0; idx < (unsigned)vecTy->getNumElements(); idx++)
          splitIntoI32(layout, builder, builder.CreateExtractElement(x, idx), output);
      }
    } else {
      // pointer or primitive types
      assert(xType->isPointerTy() || xType->isIntegerTy() || xType->isFloatTy());
      unsigned size = layout.getTypeSizeInBits(xType).getFixedValue();
      if (xType->isPointerTy())
        x = builder.CreatePtrToInt(x, builder.getIntNTy(size));

      if (size > 32) {
        assert(size % 32 == 0);
        Value *vecDword = builder.CreateBitCast(x, FixedVectorType::get(builder.getInt32Ty(), size / 32));
        splitIntoI32(layout, builder, vecDword, output);
      } else {
        if (!xType->isIntegerTy())
          x = builder.CreateBitCast(x, builder.getInt32Ty());
        output.push_back(x);
      }
    }
  }
}

// =====================================================================================================================
// Merge the input into a single struct type.
static Value *mergeIntoStruct(IRBuilder<> &builder, ArrayRef<Value *> input) {
  SmallVector<Type *> types;
  for (auto *v : input)
    types.push_back(v->getType());
  Type *structTy = StructType::get(builder.getContext(), types);
  Value *val = PoisonValue::get(structTy);
  for (size_t e = 0; e != input.size(); ++e)
    val = builder.CreateInsertValue(val, input[e], e);
  return val;
}

// =====================================================================================================================
// Construct vectors of dword, the input should be i32 type.
static Value *mergeDwordsIntoVector(IRBuilder<> &builder, ArrayRef<Value *> input) {
  unsigned numElem = input.size();
  Type *vecTy = FixedVectorType::get(builder.getInt32Ty(), numElem);
  Value *vec = PoisonValue::get(vecTy);
  unsigned idx = 0;
  for (auto *src : input)
    vec = builder.CreateInsertElement(vec, src, idx++);
  return vec;
}

// =====================================================================================================================
// Lower as.continuation.reference call.
//
// @param asCpsReferenceOp: the instruction
void PatchEntryPointMutate::lowerAsCpsReference(cps::AsContinuationReferenceOp &asCpsReferenceOp) {
  IRBuilder<> builder(&asCpsReferenceOp);

  Function &callee = cast<Function>(*asCpsReferenceOp.getFn());
  auto level = cps::getCpsLevelFromFunction(callee);

  // Use GEP since that is easier for the backend to combine into a relocation fixup.
  Value *ref = builder.CreateConstGEP1_32(builder.getInt8Ty(), asCpsReferenceOp.getFn(), static_cast<uint32_t>(level));
  ref = builder.CreatePtrToInt(ref, builder.getInt32Ty());

  asCpsReferenceOp.replaceAllUsesWith(ref);
}

// =====================================================================================================================
// Lower calls to cps function as well as return instructions.
//
// @param func : the function to be processed
// @param shaderInputs: the ShaderInputs information for the parent function. This is only used for continufy based
// continuation transform, under which we still need to pass ShaderInput arguments(WorkgroupId/LocalInvocationId) during
// cps chain call.
bool PatchEntryPointMutate::lowerCpsOps(Function *func, ShaderInputs *shaderInputs) {
  SmallVector<cps::JumpOp *> cpsJumps;
  SmallVector<CallInst *> tobeErased;

  struct Payload {
    SmallVectorImpl<cps::JumpOp *> &jumps;
    SmallVectorImpl<CallInst *> &tobeErased;
    PatchEntryPointMutate *self;
  };
  Payload payload = {cpsJumps, tobeErased, this};

  static auto visitor = llvm_dialects::VisitorBuilder<Payload>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .add<cps::JumpOp>([](auto &payload, auto &op) { payload.jumps.push_back(&op); })
                            .add<cps::AsContinuationReferenceOp>([](auto &payload, auto &op) {
                              payload.self->lowerAsCpsReference(op);
                              payload.tobeErased.push_back(&op);
                            })
                            .build();
  visitor.visit(payload, *func);

  for (auto *call : tobeErased)
    call->eraseFromParent();

  bool isCpsFunc = cps::isCpsFunction(*func);
  if (!isCpsFunc && cpsJumps.empty())
    return false;

  if (!isCpsFunc) {
    IRBuilder<> builder(func->getContext());
    builder.SetInsertPointPastAllocas(func);
    Value *vspStorage = builder.CreateAlloca(builder.getPtrTy(getLoweredCpsStackAddrSpace()), ADDR_SPACE_PRIVATE);
    m_funcCpsStackMap[func] = vspStorage;
  }

  // Get the number of user-data arguments.
  unsigned numShaderArg;
  if (!isCpsFunc) {
    SmallVector<Type *, 8> argTys;
    SmallVector<std::string, 8> argNames;
    generateEntryPointArgTys(shaderInputs, nullptr, argTys, argNames, 0);
    numShaderArg = argTys.size();
    assert(!shaderInputs || argNames.back() == "LocalInvocationId");
  } else {
    numShaderArg = m_cpsShaderInputCache.getTypes().size();
  }

  // Exclude LocalInvocationId if shaderInputs is non-null (for Continufy based continuation).
  unsigned numUserdata = shaderInputs ? numShaderArg - 1 : numShaderArg;

  // Get all the return instructions.
  SmallVector<ReturnInst *> retInstrs;
  for (BasicBlock &block : *func)
    if (ReturnInst *ret = dyn_cast<ReturnInst>(block.getTerminator()))
      retInstrs.push_back(ret);

  auto *tailBlock = BasicBlock::Create(func->getContext(), "tail.block", func);

  SmallVector<CpsExitInfo> exitInfos;
  IRBuilder<> builder(func->getContext());

  // Lower cps jumps.
  unsigned stackSize = 0;
  for (auto *jump : cpsJumps) {
    unsigned stateSize = lowerCpsJump(func, jump, tailBlock, exitInfos);
    stackSize = std::max(stackSize, stateSize);
  }

  // Lower returns.
  for (auto *ret : retInstrs) {
    auto *vspTy = builder.getPtrTy(getLoweredCpsStackAddrSpace());
    exitInfos.push_back(CpsExitInfo(ret->getParent(), {builder.getInt32(0), PoisonValue::get(vspTy)}));
    builder.SetInsertPoint(ret);
    builder.CreateBr(tailBlock);
    ret->eraseFromParent();
  }

  size_t vgprNum = 0;
  for (const auto &exit : exitInfos)
    vgprNum = std::max(exit.vgpr.size(), vgprNum);

  SmallVector<Value *> newVgpr;
  // Put LocalInvocationId before {vcr, vsp}.
  if (shaderInputs)
    newVgpr.push_back(func->getArg(numUserdata));

  builder.SetInsertPoint(tailBlock);

  if (exitInfos.size() == 1) {
    newVgpr.append(exitInfos[0].vgpr);
  } else {
    for (size_t vgprIdx = 0; vgprIdx < vgprNum; vgprIdx++) {
      // We always have the leading two fixed vgpr arguments: vcr, vsp. The other remaining payloads are i32 type.
      Type *phiTy = vgprIdx < 2 ? exitInfos[0].vgpr[vgprIdx]->getType() : builder.getInt32Ty();
      PHINode *phi = builder.CreatePHI(phiTy, exitInfos.size());
      for (size_t exitIdx = 0; exitIdx < exitInfos.size(); exitIdx++) {
        if (vgprIdx < exitInfos[exitIdx].vgpr.size())
          phi->addIncoming(exitInfos[exitIdx].vgpr[vgprIdx], exitInfos[exitIdx].pred);
        else
          phi->addIncoming(PoisonValue::get(builder.getInt32Ty()), exitInfos[exitIdx].pred);
      }
      newVgpr.push_back(phi);
    }
  }
  // Packing VGPR arguments.
  Value *vgprArg = mergeIntoStruct(builder, newVgpr);

  // Packing SGPR arguments (user data) into vector of i32s.
  SmallVector<Value *> userData;
  for (unsigned idx = 0; idx != numUserdata; ++idx)
    userData.push_back(func->getArg(idx));

  const DataLayout &layout = func->getParent()->getDataLayout();
  SmallVector<Value *> userDataI32;
  splitIntoI32(layout, builder, userData, userDataI32);
  Value *userDataVec = mergeDwordsIntoVector(builder, userDataI32);
  //    tail:
  //      Merge vgpr values from different exits.
  //      Check if we have pending cps call
  //      If no cps call, jump to return block.
  //    chain:
  //      Jump to next cps function.
  //    ret:
  //      ret void
  unsigned waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
  Type *waveMaskTy = builder.getIntNTy(waveSize);
  // For continufy based continuation, the vgpr list: LocalInvocationId, vcr, vsp, ...
  unsigned vcrIndexInVgpr = shaderInputs ? 1 : 0;
  auto *vcr = builder.CreateExtractValue(vgprArg, vcrIndexInVgpr);
  auto *vcrTy = vcr->getType();

  if (isCpsFunc) {
    auto *vcrShaderArg = func->getArg(numShaderArg);
    // When we are working with LLVM version without the llvm.amdgcn.set.inactive.chain.arg, we cannot simply declare
    // it and call it. LLVM will misrecognize it as llvm.amdgcn.set.inactive, and lit-test would just fail. So here we
    // just call llvm.amdgcn.set.inactive to pass compilation and lit-test if no *set.inactive.chain.arg support.
    // TODO: Cleanup this when the related LLVM versions have the intrinsic definition.
    if (m_setInactiveChainArgId != Intrinsic::not_intrinsic)
      vcr = builder.CreateIntrinsic(vcrTy, m_setInactiveChainArgId, {vcr, vcrShaderArg});
    else
      vcr = builder.CreateIntrinsic(vcrTy, Intrinsic::amdgcn_set_inactive, {vcr, vcrShaderArg});
  }

  // Find first lane having non-null vcr, and use as next jump target.
  auto *vcrMask = builder.CreateICmpNE(vcr, builder.getInt32(0));
  auto *pendingBallot = builder.CreateIntrinsic(Intrinsic::amdgcn_ballot, waveMaskTy, vcrMask);
  Value *firstActive = builder.CreateIntrinsic(Intrinsic::cttz, waveMaskTy, {pendingBallot, builder.getTrue()});
  if (!waveMaskTy->isIntegerTy(32))
    firstActive = builder.CreateTrunc(firstActive, builder.getInt32Ty());
  auto *targetVcr = builder.CreateIntrinsic(builder.getInt32Ty(), Intrinsic::amdgcn_readlane, {vcr, firstActive});
  // Calculate the lane mask that take this specific target.
  auto *targetMask = builder.CreateICmpEQ(vcr, targetVcr);
  auto *execMask = builder.CreateIntrinsic(Intrinsic::amdgcn_ballot, waveMaskTy, targetMask);

  if (isCpsFunc) {
    targetVcr = builder.CreateUnaryIntrinsic(Intrinsic::amdgcn_wwm, targetVcr);
    execMask = builder.CreateUnaryIntrinsic(Intrinsic::amdgcn_wwm, execMask);
  }

  BasicBlock *chainBlock = nullptr;
  // We only need to insert the return block if there is any return in original function, otherwise we just insert
  // everything in the tail block.
  if (!retInstrs.empty()) {
    chainBlock = BasicBlock::Create(func->getContext(), "chain.block", func);
    auto *retBlock = BasicBlock::Create(func->getContext(), "ret.block", func);
    auto *isNullTarget = builder.CreateICmpEQ(targetVcr, builder.getInt32(0));
    builder.CreateCondBr(isNullTarget, retBlock, chainBlock);

    builder.SetInsertPoint(retBlock);
    builder.CreateRetVoid();
  }

  if (chainBlock)
    builder.SetInsertPoint(chainBlock);
  // Mask off metadata bits and setup jump target.
  Value *addr32 = builder.CreateAnd(targetVcr, builder.getInt32(~0x3fu));
  AddressExtender addressExtender(func);
  Value *jumpTarget = addressExtender.extend(addr32, builder.getInt32(HighAddrPc), builder.getPtrTy(), builder);

  Value *chainArgs[] = {jumpTarget, execMask, userDataVec, vgprArg, builder.getInt32(0)};

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 465197
  // Old version of the code
  Type *chainArgTys[] = {builder.getPtrTy(), builder.getIntNTy(waveSize), userDataVec->getType(), vgprArg->getType(),
                         builder.getInt32Ty()};
  FunctionType *chainFuncTy = FunctionType::get(builder.getVoidTy(), chainArgTys, true);
  auto chainFunc =
      Function::Create(chainFuncTy, GlobalValue::ExternalLinkage, "llvm.amdgcn.cs.chain", func->getParent());
  builder.CreateCall(chainFunc, chainArgs);
#else
  // New version of the code (also handles unknown version, which we treat as
  // latest)
  Type *chainTys[] = {builder.getPtrTy(), builder.getIntNTy(waveSize), userDataVec->getType(), vgprArg->getType()};
  auto *chainCall = builder.CreateIntrinsic(Intrinsic::amdgcn_cs_chain, chainTys, chainArgs);
  // Add inreg attribute for (fn, exec, sgprs).
  for (unsigned arg = 0; arg < 3; arg++)
    chainCall->addParamAttr(arg, Attribute::InReg);
#endif
  builder.CreateUnreachable();

  auto funcName = func->getName();
  // Lower cps stack operations
  CpsStackLowering stackLowering(func->getContext());
  stackLowering.lowerCpsStackOps(*func, m_funcCpsStackMap[func]);

  stackSize += stackLowering.getStackSize();
  // Set per-function .frontend_stack_size PAL metadata.
  auto &shaderFunctions = m_pipelineState->getPalMetadata()
                              ->getPipelineNode()
                              .getMap(true)[Util::Abi::PipelineMetadataKey::ShaderFunctions]
                              .getMap(true);
  shaderFunctions[funcName].getMap(true)[Util::Abi::HardwareStageMetadataKey::FrontendStackSize] = stackSize;

  return true;
}

// =====================================================================================================================
// Mutate the argument list of the cps function
//
// Mutate the function type from:
// void @func({} state, args...)
// into:
// amdgpu_cs_chain void @func(fixed_shader_args, i32 %vcr, ptr addrspace(5) %vsp, args...)
//
// @param func : the cps function to be mutated
// @param fixedShaderArgTys : the types of the fixed shader arguments(userdata + possibly shader inputs)
// @param argNames : the name string of the fixed shader arguments
// @param isContinufy : whether the function is output of Continufy pass.
Function *PatchEntryPointMutate::lowerCpsFunction(Function *func, ArrayRef<Type *> fixedShaderArgTys,
                                                  ArrayRef<std::string> argNames, bool isContinufy) {
  Value *state = func->getArg(0);
  const DataLayout &layout = func->getParent()->getDataLayout();
  IRBuilder<> builder(func->getContext());
  SmallVector<Type *> newArgTys;
  newArgTys.append(fixedShaderArgTys.begin(), fixedShaderArgTys.end());
  newArgTys.append({builder.getInt32Ty(), builder.getPtrTy(getLoweredCpsStackAddrSpace())});
  auto remainingArgs = func->getFunctionType()->params().drop_front(1);
  newArgTys.append(remainingArgs.begin(), remainingArgs.end());
  FunctionType *newFuncTy = FunctionType::get(builder.getVoidTy(), newArgTys, false);
  auto newFunc = Function::Create(newFuncTy, func->getLinkage());
  newFunc->copyAttributesFrom(func);
  newFunc->copyMetadata(func, 0);
  newFunc->takeName(func);
  // Always insert the new function after the old function
  func->getParent()->getFunctionList().insertAfter(func->getIterator(), newFunc);

  // Setup the argument attributes
  AttributeSet emptyAttrSet;
  AttributeSet inRegAttrSet = emptyAttrSet.addAttribute(func->getContext(), Attribute::InReg);

  AttributeList oldAttrs = func->getAttributes();
  SmallVector<AttributeSet, 8> argAttrs;
  assert(!isContinufy || argNames.back() == "LocalInvocationId");
  unsigned numUserdataArg = isContinufy ? fixedShaderArgTys.size() - 1 : fixedShaderArgTys.size();
  for (unsigned idx = 0; idx != numUserdataArg; ++idx)
    argAttrs.push_back(inRegAttrSet);

  // %LocalInvocationId for Continufy path.
  if (isContinufy)
    argAttrs.push_back(emptyAttrSet);

  // %vcr attribute
  argAttrs.push_back(emptyAttrSet);
  // %vsp attribute
  argAttrs.push_back(emptyAttrSet);
  for (unsigned idx = 1; idx != func->getFunctionType()->getNumParams(); ++idx)
    argAttrs.push_back(oldAttrs.getParamAttrs(idx));
  newFunc->setAttributes(
      AttributeList::get(func->getContext(), oldAttrs.getFnAttrs(), oldAttrs.getRetAttrs(), argAttrs));

  // Move all the basic blocks from the original function into the new one.
  newFunc->splice(newFunc->begin(), func);

  builder.SetInsertPointPastAllocas(newFunc);
  Value *vspStorage = builder.CreateAlloca(builder.getPtrTy(getLoweredCpsStackAddrSpace()), ADDR_SPACE_PRIVATE);
  m_funcCpsStackMap[newFunc] = vspStorage;

  // Function arguments: {fixed_shader_arguments, vcr, vsp, original_func_arguments_exclude_state}
  Value *vsp = newFunc->getArg(fixedShaderArgTys.size() + 1);
  if (!state->getType()->isEmptyTy()) {
    // Get stack address of pushed state and load it from continuation stack.
    unsigned stateSize = layout.getTypeStoreSize(state->getType());
    vsp = builder.CreateConstInBoundsGEP1_32(builder.getInt8Ty(), vsp, -alignTo(stateSize, continuationStackAlignment));
    Value *newState = builder.CreateAlignedLoad(state->getType(), vsp, Align(continuationStackAlignment), "cps.state");
    state->replaceAllUsesWith(newState);
  }
  builder.CreateStore(vsp, vspStorage);

  // Set name string for arguments.
  SmallVector<std::string> newArgNames(argNames);
  newArgNames.append({"vcr", "vsp"});
  for (unsigned idx = 0; idx < newArgNames.size(); idx++)
    newFunc->getArg(idx)->setName(newArgNames[idx]);

  // Replace old arguments with new ones (excluding the very first `state`).
  unsigned argOffsetInNew = fixedShaderArgTys.size() + 2;
  for (unsigned idx = 1; idx < func->arg_size(); idx++) {
    Value *oldArg = func->getArg(idx);
    Value *newArg = newFunc->getArg(idx - 1 + argOffsetInNew);
    newArg->setName(oldArg->getName());
    oldArg->replaceAllUsesWith(newArg);
  }
  setShaderStage(newFunc, getShaderStage(func));
  newFunc->setAlignment(Align(64));
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 465196
  // Old version of the code
#else
  // New version of the code (also handles unknown version, which we treat as
  // latest)
  newFunc->setCallingConv(CallingConv::AMDGPU_CS_Chain);
#endif
  return newFunc;
}

// =====================================================================================================================
// Lower cps.jump, fill cps exit information and branch to tailBlock. Return the state size.
// This assume the arguments of the parent function are setup correctly.
//
// @param parent : the parent function of the cps.jump operation
// @param jumpOp : the call instruction of cps.jump
// @param [in/out] exitInfos : the vector of cps exit information to be filled
unsigned PatchEntryPointMutate::lowerCpsJump(Function *parent, cps::JumpOp *jumpOp, BasicBlock *tailBlock,
                                             SmallVectorImpl<CpsExitInfo> &exitInfos) {
  IRBuilder<> builder(parent->getContext());
  const DataLayout &layout = parent->getParent()->getDataLayout();
  // Translate @lgc.cps.jump(CR %target, i32 %levels, T %state, ...) into:
  // @llvm.amdgcn.cs.chain(ptr %fn, i{32,64} %exec, T %sgprs, U %vgprs, i32 immarg %flags, ...)
  Value *vcr = jumpOp->getTarget();
  builder.SetInsertPoint(jumpOp);

  // Pushing state onto stack and get new vsp.
  Value *state = jumpOp->getState();
  Value *vsp = builder.CreateAlignedLoad(builder.getPtrTy(getLoweredCpsStackAddrSpace()), m_funcCpsStackMap[parent],
                                         Align(getLoweredCpsStackPointerSize(layout)));
  unsigned stateSize = 0;
  if (!state->getType()->isEmptyTy()) {
    stateSize = layout.getTypeStoreSize(state->getType());
    builder.CreateStore(state, vsp);
    // Make vsp properly aligned across cps function.
    stateSize = alignTo(stateSize, continuationStackAlignment);
    vsp = builder.CreateConstGEP1_32(builder.getInt8Ty(), vsp, stateSize);
  }

  // Add extra args specific to the target function.
  SmallVector<Value *> remainingArgs;
  for (Value *arg : drop_begin(jumpOp->args(), 3))
    remainingArgs.push_back(arg);

  // Packing VGPR arguments {vcr, vsp, args...}
  SmallVector<Value *> vgprArgs;
  vgprArgs.push_back(vcr);
  vgprArgs.push_back(vsp);
  splitIntoI32(layout, builder, remainingArgs, vgprArgs);

  // Fill exit information.
  exitInfos.push_back(CpsExitInfo(jumpOp->getParent(), std::move(vgprArgs)));
  // Branch to tailBlock.
  auto *oldTerm = jumpOp->getParent()->getTerminator();
  assert(isa<UnreachableInst>(oldTerm));
  oldTerm->eraseFromParent();
  builder.CreateBr(tailBlock);

  jumpOp->eraseFromParent();
  return stateSize;
}

// =====================================================================================================================
// Set up compute-with-calls flag. It is set for either of these two cases:
// 1. a compute library;
// 2. a compute pipeline that does indirect calls or calls to external application shader functions.
//
// When set, this pass behaves differently, not attempting to omit unused shader inputs, since all shader inputs
// are potentially used in other functions. It also modifies each call to pass the shader inputs between functions.
//
// @param module : IR module
void PatchEntryPointMutate::setupComputeWithCalls(Module *module) {
  m_computeWithCalls = false;

  if (m_pipelineState->isComputeLibrary()) {
    m_computeWithCalls = true;
    return;
  }

  // We have a compute pipeline. Check whether there are any non-shader-entry-point functions (other than lgc.*
  // functions and intrinsics).
  for (Function &func : *module) {
    if (func.isDeclaration() && func.getIntrinsicID() == Intrinsic::not_intrinsic &&
        !func.getName().startswith(lgcName::InternalCallPrefix) && !func.user_empty()) {
      m_computeWithCalls = true;
      return;
    }

    // Search for indirect calls between application shaders.
    for (const BasicBlock &block : func) {
      for (const Instruction &inst : block) {
        if (auto *call = dyn_cast<CallInst>(&inst)) {
          if (isa<cps::JumpOp>(call) || call->getCallingConv() == CallingConv::SPIR_FUNC) {
            m_computeWithCalls = true;
            return;
          }
        }
      }
    }
  }
}

// =====================================================================================================================
// Gather user data usage in all shaders
//
// @param module : IR module
void PatchEntryPointMutate::gatherUserDataUsage(Module *module) {
  // Gather special ops requiring user data.
  static const auto visitor =
      llvm_dialects::VisitorBuilder<PatchEntryPointMutate>()
          .add<UserDataOp>([](PatchEntryPointMutate &self, UserDataOp &op) {
            ShaderStage stage = getShaderStage(op.getFunction());
            assert(stage != ShaderStageCopyShader);
            auto userDataUsage = self.getUserDataUsage(stage);
            userDataUsage->userDataOps.push_back(&op);

            // Attempt to find all loads with a constant dword-aligned offset and push into
            // userDataUsage->pushConstOffsets. If we fail, set userDataUsage->pushConstSpill to indicate that we need
            // to keep the pointer to the push const, derived as an offset into the spill table.
            bool haveDynamicUser = false;
            SmallVector<std::pair<Instruction *, unsigned>, 4> worklist;
            worklist.push_back({&op, op.getOffset()});
            while (!worklist.empty()) {
              auto [inst, offset] = worklist.pop_back_val();
              for (User *user : inst->users()) {
                if (auto bitcast = dyn_cast<BitCastInst>(user)) {
                  // See through a bitcast.
                  worklist.push_back({bitcast, offset});
                  continue;
                }
                if (isa<LoadInst>(user)) {
                  if (user->getType()->isAggregateType()) {
                    haveDynamicUser = true;
                    continue;
                  }
                  unsigned byteSize = self.m_module->getDataLayout().getTypeStoreSize(user->getType());
                  if (byteSize % 4 != 0 || offset % 4 != 0) {
                    haveDynamicUser = true;
                    continue;
                  }

                  // This is a scalar or vector load with dword-aligned size at a fixed dword offset. We may be able to
                  // get it from a user data argument
                  UserDataLoad load;
                  load.load = cast<Instruction>(user);
                  load.dwordOffset = offset / 4;
                  load.dwordSize = byteSize / 4;
                  userDataUsage->loads.push_back(load);

                  userDataUsage->addLoad(load.dwordOffset, load.dwordSize);
                  continue;
                }
                if (auto gep = dyn_cast<GetElementPtrInst>(user)) {
                  // For a gep, calculate the new constant offset.
                  APInt gepOffset(64, 0);
                  if (gep->accumulateConstantOffset(self.m_module->getDataLayout(), gepOffset)) {
                    unsigned gepByteOffset = gepOffset.getZExtValue();
                    worklist.push_back({gep, offset + gepByteOffset});
                    continue;
                  }
                }
                haveDynamicUser = true;
              }
            }

            if (haveDynamicUser) {
              userDataUsage->haveDynamicUserDataLoads = true;
              self.m_pipelineState->getPalMetadata()->setUserDataSpillUsage(op.getOffset() / 4);
            }
          })
          .add<LoadUserDataOp>([](PatchEntryPointMutate &self, LoadUserDataOp &op) {
            ShaderStage stage = getShaderStage(op.getFunction());
            assert(stage != ShaderStageCopyShader);
            auto *userDataUsage = self.getUserDataUsage(stage);

            UserDataLoad load;
            load.load = &op;
            load.dwordOffset = op.getOffset() / 4;
            load.dwordSize = self.m_module->getDataLayout().getTypeStoreSize(op.getType()) / 4;

            userDataUsage->loads.push_back(load);
            userDataUsage->addLoad(load.dwordOffset, load.dwordSize);
          })
          .build();

  visitor.visit(*this, *module);

  for (Function &func : *module) {
    if (!func.isDeclaration())
      continue;

    if (func.getName().startswith(lgcName::SpecialUserData)) {
      for (User *user : func.users()) {
        CallInst *call = cast<CallInst>(user);
        ShaderStage stage = getShaderStage(call->getFunction());
        assert(stage != ShaderStageCopyShader);
        auto &specialUserData = getUserDataUsage(stage)->specialUserData;
        unsigned index = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue() -
                         static_cast<unsigned>(UserDataMapping::GlobalTable);
        specialUserData.resize(std::max(specialUserData.size(), size_t(index + 1)));
        specialUserData[index].users.push_back(call);
      }
      continue;
    }

    if ((func.getName().startswith(lgcName::OutputExportXfb) && !func.use_empty()) || m_pipelineState->enableSwXfb()) {
      // NOTE: For GFX11+, SW emulated stream-out will always use stream-out buffer descriptors and stream-out buffer
      // offsets to calculate numbers of written primitives/dwords and update the counters.  auto lastVertexStage =
      auto lastVertexStage = m_pipelineState->getLastVertexProcessingStage();
      lastVertexStage = lastVertexStage == ShaderStageCopyShader ? ShaderStageGeometry : lastVertexStage;
      getUserDataUsage(lastVertexStage)->usesStreamOutTable = true;
    }
  }
}

// =====================================================================================================================
// Load a value of a simple type from user data at the given dwordOffset.
Value *PatchEntryPointMutate::loadUserData(const UserDataUsage &userDataUsage, Value *spillTable, Type *type,
                                           unsigned dwordOffset, BuilderBase &builder) {
  Function *func = builder.GetInsertBlock()->getParent();
  unsigned dwordSize = m_module->getDataLayout().getTypeStoreSize(type) / 4;
  if (dwordOffset + dwordSize <= userDataUsage.entryArgIdxs.size()) {
    SmallVector<Value *> dwords;
    for (unsigned i = 0; i != dwordSize; ++i) {
      unsigned entryArgIdx = userDataUsage.entryArgIdxs[dwordOffset + i];
      if (!entryArgIdx)
        break;
      dwords.push_back(getFunctionArgument(func, entryArgIdx));
    }
    if (dwords.size() == dwordSize) {
      Value *result;
      if (dwords.size() > 1) {
        result = PoisonValue::get(FixedVectorType::get(builder.getInt32Ty(), dwords.size()));
        for (unsigned i = 0; i != dwords.size(); ++i)
          result = builder.CreateInsertElement(result, dwords[i], i);
      } else {
        result = dwords[0];
      }
      if (type != result->getType()) {
        if (isa<PointerType>(type)) {
          if (dwordSize != 1)
            result = builder.CreateBitCast(result, builder.getIntNTy(32 * dwordSize));
          result = builder.CreateIntToPtr(result, type);
        } else {
          result = builder.CreateBitCast(result, type);
        }
      }
      return result;
    }
  }

  assert(spillTable);
  Value *ptr = builder.CreateConstGEP1_32(builder.getInt8Ty(), spillTable, dwordOffset * 4);
  auto *load = builder.CreateLoad(type, ptr);
  load->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(*m_context, {}));
  return load;
}

// =====================================================================================================================
// Fix up user data uses in all shaders: For unspilled ones, use the entry arg directly; for spilled ones,
// insert a load from the spill table, shared for the function.
// This uses the entryArgIdx fields in UserDataUsage; each one was set as follows:
// 1. addUserDataArgs constructed a UserDataArg for it, giving it a pointer to the applicable entryArgIdx field;
// 2. In determineUnspilledUserDataArgs, where it decides to unspill (i.e. keep in shader entry SGPR), it stores the
//    argument index into that pointed to value;
// 3. In this function, we use the entryArgIdx field to get the argument index. If it is 0, then the item was
//    spilled.
//
// @param module : IR module
void PatchEntryPointMutate::fixupUserDataUses(Module &module) {
  BuilderBase builder(module.getContext());

  // For each function definition...
  for (Function &func : module) {
    if (func.isDeclaration())
      continue;

    ShaderStage stage = getShaderStage(&func);
    auto userDataUsage = getUserDataUsage(stage);

    // If needed, generate code for the spill table pointer (as pointer to i8) at the start of the function.
    Instruction *spillTable = nullptr;
    AddressExtender addressExtender(&func);
    if (userDataUsage->spillTableEntryArgIdx != 0) {
      builder.SetInsertPoint(addressExtender.getFirstInsertionPt());
      Argument *arg = getFunctionArgument(&func, userDataUsage->spillTableEntryArgIdx);
      spillTable = addressExtender.extendWithPc(arg, builder.getPtrTy(ADDR_SPACE_CONST), builder);
    }

    // Handle direct uses of the spill table that were generated in DescBuilder.
    for (auto *&call : userDataUsage->userDataOps) {
      if (!call || call->getFunction() != &func)
        continue;

      auto *op = cast<UserDataOp>(call);
      call = nullptr;

      if (spillTable) {
        builder.SetInsertPoint(op);
        Value *ptr = builder.CreateConstGEP1_32(builder.getInt8Ty(), spillTable, op->getOffset());
        op->replaceAllUsesWith(ptr);
      } else {
        // We don't actually have a spill table, which means that all (transitive) users of this op are ultimately
        // no-ops or fixed-offset loads that will be replaced separately.
        op->replaceAllUsesWith(PoisonValue::get(op->getType()));
      }
      op->eraseFromParent();
    }

    // Handle generic fixed-offset user data loads.
    for (auto &load : userDataUsage->loads) {
      if (!load.load || load.load->getFunction() != &func)
        continue;

      builder.SetInsertPoint(load.load);
      load.load->replaceAllUsesWith(
          loadUserData(*userDataUsage, spillTable, load.load->getType(), load.dwordOffset, builder));
      load.load->eraseFromParent();
      load.load = nullptr;
    }

    // Special user data from lgc.special.user.data calls
    for (unsigned idx = 0; idx != userDataUsage->specialUserData.size(); ++idx) {
      auto &specialUserData = userDataUsage->specialUserData[idx];
      if (!specialUserData.users.empty()) {
        assert(specialUserData.entryArgIdx != 0);
        Value *arg = getFunctionArgument(&func, specialUserData.entryArgIdx);

        for (Instruction *&inst : specialUserData.users) {
          if (inst && inst->getFunction() == &func) {
            Value *replacementVal = arg;
            auto call = dyn_cast<CallInst>(inst);
            if (call->arg_size() >= 2) {
              // There is a second operand, used by ShaderInputs::getSpecialUserDataAsPoint to indicate that we
              // need to extend the loaded 32-bit value to a 64-bit pointer, using either PC or the provided
              // high half.
              builder.SetInsertPoint(call);
              Value *highHalf = call->getArgOperand(1);
              replacementVal = addressExtender.extend(replacementVal, highHalf, call->getType(), builder);
            }
            inst->replaceAllUsesWith(replacementVal);
            inst->eraseFromParent();
            inst = nullptr;
          }
        }
      }
    }
  }
}

// =====================================================================================================================
// Process a single shader
//
// @param shaderInputs : ShaderInputs object representing hardware-provided shader inputs
void PatchEntryPointMutate::processShader(ShaderInputs *shaderInputs) {
  // Create new entry-point from the original one
  SmallVector<Type *, 8> argTys;
  SmallVector<std::string, 8> argNames;
  uint64_t inRegMask = generateEntryPointArgTys(shaderInputs, nullptr, argTys, argNames, 0, true);

  Function *origEntryPoint = m_entryPoint;

  // Create the new function and transfer code and attributes to it.
  Function *entryPoint =
      addFunctionArgs(origEntryPoint, origEntryPoint->getFunctionType()->getReturnType(), argTys, argNames, inRegMask);

  // We always deal with pre-merge functions here, so set the fitting pre-merge calling conventions.
  switch (m_shaderStage) {
  case ShaderStageTask:
    entryPoint->setCallingConv(CallingConv::AMDGPU_CS);
    break;
  case ShaderStageMesh:
    entryPoint->setCallingConv(CallingConv::AMDGPU_GS);
    break;
  case ShaderStageVertex:
    if (m_pipelineState->hasShaderStage(ShaderStageTessControl))
      entryPoint->setCallingConv(CallingConv::AMDGPU_LS);
    else if (m_pipelineState->hasShaderStage(ShaderStageGeometry))
      entryPoint->setCallingConv(CallingConv::AMDGPU_ES);
    else
      entryPoint->setCallingConv(CallingConv::AMDGPU_VS);
    break;
  case ShaderStageTessControl:
    entryPoint->setCallingConv(CallingConv::AMDGPU_HS);
    break;
  case ShaderStageTessEval:
    if (m_pipelineState->hasShaderStage(ShaderStageGeometry))
      entryPoint->setCallingConv(CallingConv::AMDGPU_ES);
    else
      entryPoint->setCallingConv(CallingConv::AMDGPU_VS);
    break;
  case ShaderStageGeometry:
    entryPoint->setCallingConv(CallingConv::AMDGPU_GS);
    break;
  case ShaderStageFragment:
    entryPoint->setCallingConv(CallingConv::AMDGPU_PS);
    break;
  default:
    llvm_unreachable("unexpected shader stage for graphics shader");
  }

  // Set Attributes on new function.
  setFuncAttrs(entryPoint);

  // Remove original entry-point
  origEntryPoint->eraseFromParent();
}

// =====================================================================================================================
// Process all functions in a compute pipeline or library.
//
// @param shaderInputs : ShaderInputs object representing hardware-provided shader inputs
// @param [in/out] module : Module
void PatchEntryPointMutate::processComputeFuncs(ShaderInputs *shaderInputs, Module &module) {
  m_shaderStage = ShaderStageCompute;

  // We no longer support compute shader fixed layout required before PAL interface version 624.
  if (m_pipelineState->getLgcContext()->getPalAbiVersion() < 624)
    report_fatal_error("Compute shader not supported before PAL version 624");

  // Process each function definition.
  SmallVector<Function *, 4> origFuncs;
  for (Function &func : module) {
    if (func.isDeclaration()) {
      if (!func.isIntrinsic() && !func.getName().startswith(lgcName::InternalCallPrefix)) {
        // This is the declaration of a callable function that is defined in a different module.
        func.setCallingConv(CallingConv::AMDGPU_Gfx);
      }
    } else {
      origFuncs.push_back(&func);
    }
  }

  for (Function *origFunc : origFuncs) {
    auto *origType = origFunc->getFunctionType();
    // Determine what args need to be added on to all functions.
    SmallVector<Type *, 20> shaderInputTys;
    SmallVector<std::string, 20> shaderInputNames;
    uint64_t inRegMask;

    // Create the new function and transfer code and attributes to it.
    Function *newFunc = nullptr;
    // For continufy based ray-tracing, we still need to add shader inputs like workgroupId and LocalInvocationId.
    bool isContinufy = m_pipelineState->getOptions().rtIndirectMode == RayTracingIndirectMode::ContinuationsContinufy;
    ShaderInputs *cpsShaderInputs = isContinufy ? shaderInputs : nullptr;
    if (cps::isCpsFunction(*origFunc)) {
      assert(origType->getReturnType()->isVoidTy());
      if (!m_cpsShaderInputCache.isAvailable()) {
        generateEntryPointArgTys(cpsShaderInputs, nullptr, shaderInputTys, shaderInputNames, 0);
        m_cpsShaderInputCache.set(shaderInputTys, shaderInputNames);
      }
      newFunc =
          lowerCpsFunction(origFunc, m_cpsShaderInputCache.getTypes(), m_cpsShaderInputCache.getNames(), isContinufy);
    } else {
      inRegMask = generateEntryPointArgTys(shaderInputs, origFunc, shaderInputTys, shaderInputNames,
                                           origType->getNumParams(), true);
      newFunc = addFunctionArgs(origFunc, origType->getReturnType(), shaderInputTys, shaderInputNames, inRegMask,
                                AddFunctionArgsAppend);
      const bool isEntryPoint = isShaderEntryPoint(newFunc);
      newFunc->setCallingConv(isEntryPoint ? CallingConv::AMDGPU_CS : CallingConv::AMDGPU_Gfx);
    }
    // Set Attributes on new function.
    setFuncAttrs(newFunc);

    origFunc->replaceAllUsesWith(newFunc);
    // Remove original function.
    origFunc->eraseFromParent();

    if (lowerCpsOps(newFunc, cpsShaderInputs))
      continue;

    int argOffset = origType->getNumParams();
    if (isComputeWithCalls())
      processCalls(*newFunc, shaderInputTys, shaderInputNames, inRegMask, argOffset);
  }
}
// =====================================================================================================================
// Process all real function calls and passes arguments to them.
//
// @param [in/out] module : Module
void PatchEntryPointMutate::processCalls(Function &func, SmallVectorImpl<Type *> &shaderInputTys,
                                         SmallVectorImpl<std::string> &shaderInputNames, uint64_t inRegMask,
                                         unsigned argOffset) {
  // This is one of:
  // - a compute pipeline with non-inlined functions;
  // - a compute pipeline with calls to library functions;
  // - a compute library.
  // We need to scan the code and modify each call to append the extra args.
  IRBuilder<> builder(func.getContext());
  for (BasicBlock &block : func) {
    // Use early increment iterator, so we can safely erase the instruction.
    for (Instruction &inst : make_early_inc_range(block)) {
      auto call = dyn_cast<CallInst>(&inst);
      if (!call)
        continue;
      // Got a call. Skip it if it calls an intrinsic or an internal lgc.* function.
      Value *calledVal = call->getCalledOperand();
      Function *calledFunc = dyn_cast<Function>(calledVal);
      if (calledFunc) {
        if (calledFunc->isIntrinsic() || calledFunc->getName().startswith(lgcName::InternalCallPrefix))
          continue;
      } else if (call->isInlineAsm()) {
        continue;
      }
      // Build a new arg list, made of the ABI args shared by all functions (user data and hardware shader
      // inputs), plus the original args on the call.
      SmallVector<Type *, 20> argTys;
      SmallVector<Value *, 20> args;
      for (unsigned idx = 0; idx != call->arg_size(); ++idx) {
        argTys.push_back(call->getArgOperand(idx)->getType());
        args.push_back(call->getArgOperand(idx));
      }
      for (unsigned idx = 0; idx != shaderInputTys.size(); ++idx) {
        argTys.push_back(func.getArg(idx + argOffset)->getType());
        args.push_back(func.getArg(idx + argOffset));
      }
      // Get the new called value as a bitcast of the old called value. If the old called value is already
      // the inverse bitcast, just drop that bitcast.
      // If the old called value was a function declaration, we did not insert a bitcast
      FunctionType *calledTy = FunctionType::get(call->getType(), argTys, false);
      builder.SetInsertPoint(call);
      Type *calledPtrTy = calledTy->getPointerTo(calledVal->getType()->getPointerAddressSpace());
      auto bitCast = dyn_cast<BitCastOperator>(calledVal);
      Value *newCalledVal = nullptr;
      if (bitCast && bitCast->getOperand(0)->getType() == calledPtrTy)
        newCalledVal = bitCast->getOperand(0);
      else
        newCalledVal = builder.CreateBitCast(calledVal, calledPtrTy);
      // Create the call.
      CallInst *newCall = builder.CreateCall(calledTy, newCalledVal, args);
      newCall->setCallingConv(CallingConv::AMDGPU_Gfx);

      // Mark sgpr arguments as inreg
      for (unsigned idx = 0; idx != shaderInputTys.size(); ++idx) {
        if ((inRegMask >> idx) & 1)
          newCall->addParamAttr(idx + call->arg_size(), Attribute::InReg);
      }

      // Replace and erase the old one.
      call->replaceAllUsesWith(newCall);
      call->eraseFromParent();
    }
  }
}

// =====================================================================================================================
// Set Attributes on new function
void PatchEntryPointMutate::setFuncAttrs(Function *entryPoint) {
  AttrBuilder builder(entryPoint->getContext());
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
    spiPsInputAddr.bits.ancillaryEna |= builtInUsage.shadingRate;
    spiPsInputAddr.bits.sampleCoverageEna = builtInUsage.sampleMaskIn;

    builder.addAttribute("InitialPSInputAddr", std::to_string(spiPsInputAddr.u32All));

    bool hasDepthExport = builtInUsage.sampleMask || builtInUsage.fragStencilRef || builtInUsage.fragDepth;
    builder.addAttribute("amdgpu-depth-export", hasDepthExport ? "1" : "0");

    bool hasColorExport = false;
    // SpiShaderColFormat / mmSPI_SHADER_COL_FORMAT is used for fully compiled shaders
    unsigned colFormat = EXP_FORMAT_ZERO;
    if (m_pipelineState->useRegisterFieldFormat()) {
      auto &colFormatNode = m_pipelineState->getPalMetadata()
                                ->getPipelineNode()
                                .getMap(true)[Util::Abi::PipelineMetadataKey::GraphicsRegisters]
                                .getMap(true)[Util::Abi::GraphicsRegisterMetadataKey::SpiShaderColFormat]
                                .getMap(true);
      for (auto iter = colFormatNode.begin(); iter != colFormatNode.end(); ++iter) {
        if (iter->second.getUInt() != EXP_FORMAT_ZERO) {
          colFormat = iter->second.getUInt();
          break;
        }
      }
    } else {
      colFormat = m_pipelineState->getPalMetadata()->getRegister(mmSPI_SHADER_COL_FORMAT);
    }
    if (colFormat != EXP_FORMAT_ZERO)
      hasColorExport = true;

    if (!hasColorExport) {
      // getColorExportCount() is used for partially compiled shaders
      const unsigned colorExportCount = m_pipelineState->getPalMetadata()->getColorExportCount();
      if (colorExportCount > static_cast<unsigned>(hasDepthExport))
        hasColorExport = true;
    }

    builder.addAttribute("amdgpu-color-export", hasColorExport ? "1" : "0");
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
    std::string wavesPerEu = std::string("1,") + std::to_string(shaderOptions->maxThreadGroupsPerComputeUnit);
    builder.addAttribute("amdgpu-waves-per-eu", wavesPerEu);
  }

  if (shaderOptions->unrollThreshold != 0)
    builder.addAttribute("amdgpu-unroll-threshold", std::to_string(shaderOptions->unrollThreshold));
  else {
    // use a default unroll threshold of 700
    builder.addAttribute("amdgpu-unroll-threshold", "700");
  }

  if (shaderOptions->ldsSpillLimitDwords != 0) {
    // Sanity check: LDS spilling is only supported in Fragment and Compute.
    if (m_shaderStage == ShaderStageFragment || m_shaderStage == ShaderStageCompute)
      builder.addAttribute("amdgpu-lds-spill-limit-dwords", std::to_string(shaderOptions->ldsSpillLimitDwords));
  }

  if (shaderOptions->disableCodeSinking)
    builder.addAttribute("disable-code-sinking");

  if (shaderOptions->nsaThreshold != 0)
    builder.addAttribute("amdgpu-nsa-threshold", std::to_string(shaderOptions->nsaThreshold));

  // Disable backend heuristics which would allow shaders to have lower occupancy. Heed the favorLatencyHiding tuning
  // option instead.
  builder.addAttribute("amdgpu-memory-bound", shaderOptions->favorLatencyHiding ? "true" : "false");
  builder.addAttribute("amdgpu-wave-limiter", "false");

  entryPoint->addFnAttrs(builder);

  // NOTE: Remove "readnone" attribute for entry-point. If GS is empty, this attribute will allow
  // LLVM optimization to remove sendmsg(GS_DONE). It is unexpected.
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 440919
  // Old version of the code
  if (entryPoint->hasFnAttribute(Attribute::ReadNone))
    entryPoint->removeFnAttr(Attribute::ReadNone);
#else
  // New version of the code (also handles unknown version, which we treat as
  // latest)
  entryPoint->setMemoryEffects(MemoryEffects::unknown());
#endif
}

// =====================================================================================================================
// Generates the type for the new entry-point based on already-collected info.
// This is what decides what SGPRs and VGPRs are passed to the shader at wave dispatch:
//
// * (For a GFX9+ merged shader or NGG primitive shader, the 8 system SGPRs at the start are not accounted for here.)
// * The "user data" SGPRs, up to 32 (GFX9+ non-compute shader) or 16 (compute shader or <=GFX8). Many of the values
//   here are pointers, but are passed as a single 32-bit register and then expanded to 64-bit in the shader code:
//   - The "global information table", containing various descriptors such as the inter-shader rings
//   - The streamout table if needed
//   - Nodes from the root user data layout, including pointers to descriptor sets.
//   - Various other system values set up by PAL, such as the vertex buffer table and the vertex base index
//   - The spill table pointer if needed. This is typically in the last register (s15 or s31), but not necessarily.
// * The system value SGPRs and VGPRs determined by hardware, some of which are enabled or disabled by bits in SPI
//   registers.
//
// In GFX9+ shader merging, shaders have not yet been merged, and this function is called for each
// unmerged shader stage. The code here needs to ensure that it gets the same SGPR user data layout for
// both shaders that are going to be merged (VS-HS, VS-GS if no tessellation, ES-GS).
//
// @param shaderInputs : ShaderInputs object representing hardware-provided shader inputs (may be null)
// @param origFunc : The original entry point function
// @param [out] argTys : The argument types for the new function type
// @param [out] argNames : The argument names corresponding to the argument types
// @param [out] argNames : The argument names corresponding to the argument types
// @param argOffset : The argument index offset to apply to the user data arguments
// @param updateUserDataMap : whether the user data map should be updated
// @returns inRegMask : "Inreg" bit mask for the arguments, with a bit set to indicate that the corresponding
//                          arg needs to have an "inreg" attribute to put the arg into SGPRs rather than VGPRs
//
uint64_t PatchEntryPointMutate::generateEntryPointArgTys(ShaderInputs *shaderInputs, Function *origFunc,
                                                         SmallVectorImpl<Type *> &argTys,
                                                         SmallVectorImpl<std::string> &argNames, unsigned argOffset,
                                                         bool updateUserDataMap) {

  uint64_t inRegMask = 0;
  IRBuilder<> builder(*m_context);
  auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
  auto &entryArgIdxs = intfData->entryArgIdxs;
  entryArgIdxs.initialized = true;

  // First we collect the user data args in two vectors:
  // - userDataArgs: global table and streamout table, followed by the nodes from the root user
  //   data layout (excluding vertex buffer and streamout tables). Some of them may need to be spilled due to
  //   running out of entry SGPRs
  // - specialUserDataArgs: special values that go at the end, such as ViewId.
  //
  // The UserDataArg for each arg pushed into these vectors contains:
  // - argTy: The IR type of the arg
  // - argDwordSize: Size of the arg in dwords
  // - userDataValue: The PAL metadata value to be passed to PalMetadata::setUserDataEntry, or Invalid for none
  // - argIndex: Pointer to the location where we will store the actual arg number, or nullptr

  SmallVector<UserDataArg, 8> userDataArgs;
  SmallVector<UserDataArg, 4> specialUserDataArgs;

  // Global internal table
  userDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "globalTable", UserDataMapping::GlobalTable));

  addSpecialUserDataArgs(userDataArgs, specialUserDataArgs, builder);

  finalizeUserDataArgs(userDataArgs, specialUserDataArgs, builder);

  // Scan userDataArgs: for each one:
  // * add it to the arg type array
  // * set user data PAL metadata
  // * store the arg index into the pointer provided to the xxxArgs.push()
  // * if it's special user data, also store the arg index into the specialUserData entry.
  unsigned userDataIdx = 0;
  for (const auto &userDataArg : userDataArgs) {
    if (userDataArg.argIndex)
      *userDataArg.argIndex = argTys.size() + argOffset;
    unsigned dwordSize = userDataArg.argDwordSize;
    if (userDataArg.userDataValue != static_cast<unsigned>(UserDataMapping::Invalid)) {
      // Most of user data metadata entries is 1 except for root push descriptors.
      bool isSystemUserData = isSystemUserDataValue(userDataArg.userDataValue);
      unsigned numEntries = isSystemUserData ? 1 : dwordSize;
      assert((!isUnlinkedDescriptorSetValue(userDataArg.userDataValue) || dwordSize == 1) &&
             "Expecting descriptor set values to be one dword.  The linker cannot handle anything else.");
      if (!m_pipelineState->useRegisterFieldFormat())
        m_pipelineState->getPalMetadata()->setUserDataEntry(m_shaderStage, userDataIdx, userDataArg.userDataValue,
                                                            numEntries);
      if (isSystemUserData) {
        unsigned index = userDataArg.userDataValue - static_cast<unsigned>(UserDataMapping::GlobalTable);
        auto &specialUserData = getUserDataUsage(m_shaderStage)->specialUserData;
        if (index < specialUserData.size())
          specialUserData[index].entryArgIdx = argTys.size() + argOffset;
      }
    }
    argTys.push_back(userDataArg.argTy);
    argNames.push_back(userDataArg.name);
    userDataIdx += dwordSize;
  }

  if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx11.waUserSgprInitBug) {
    // Add dummy user data to bring the total to 16 SGPRS if hardware workaround
    // is required

    // Only applies to wave32
    // TODO: Can we further exclude PS if LDS_GROUP_SIZE == 0
    if (m_pipelineState->getShaderWaveSize(m_shaderStage) == 32 &&
        (m_shaderStage == ShaderStageCompute || m_shaderStage == ShaderStageFragment ||
         m_shaderStage == ShaderStageMesh)) {
      unsigned userDataLimit = m_shaderStage == ShaderStageMesh ? 8 : 16;

      while (userDataIdx < userDataLimit) {
        argTys.push_back(builder.getInt32Ty());
        argNames.push_back(("dummyInit" + Twine(userDataIdx)).str());
        userDataIdx += 1;
      }
    }
  }

  // NOTE: We encounter a HW defect on GFX9. When there is only one user SGPR (corresponds to global table, s0),
  // the SGPR corresponding to scratch offset (s2) of PS is incorrectly initialized. This leads to invalid scratch
  // memory access, causing GPU hang. Thus, we detect such case and add a dummy user SGPR in order not to map scratch
  // offset to s2.
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major == 9 && m_shaderStage == ShaderStageFragment) {
    if (userDataIdx == 1) {
      argTys.push_back(builder.getInt32Ty());
      argNames.push_back("dummyInit");
      userDataIdx += 1;
    }
  }

  intfData->userDataCount = userDataIdx;
  inRegMask = (1ull << argTys.size()) - 1;

  // Push the fixed system (not user data) register args.
  if (shaderInputs)
    inRegMask |= shaderInputs->getShaderArgTys(m_pipelineState, m_shaderStage, origFunc, m_computeWithCalls, argTys,
                                               argNames, argOffset);

  if (updateUserDataMap && m_pipelineState->useRegisterFieldFormat()) {
    constexpr unsigned NumUserSgprs = 32;
    constexpr unsigned InvalidMapVal = static_cast<unsigned>(UserDataMapping::Invalid);
    SmallVector<unsigned, NumUserSgprs> userDataMap;
    userDataMap.resize(NumUserSgprs, InvalidMapVal);
    userDataIdx = 0;
    for (const auto &userDataArg : userDataArgs) {
      unsigned dwordSize = userDataArg.argDwordSize;
      if (userDataArg.userDataValue != InvalidMapVal) {
        bool isSystemUserData = isSystemUserDataValue(userDataArg.userDataValue);
        unsigned numEntries = isSystemUserData ? 1 : dwordSize;
        unsigned userDataValue = userDataArg.userDataValue;
        unsigned idx = userDataIdx;
        while (numEntries--)
          userDataMap[idx++] = userDataValue++;
      }
      userDataIdx += dwordSize;
    }
    m_pipelineState->setUserDataMap(m_shaderStage, userDataMap);
  }

  return inRegMask;
}

// =====================================================================================================================
// @param userDataValue : The value to be written into a user data entry.
// @returns : True if the user data value corresponds to a special system user data value.
bool PatchEntryPointMutate::isSystemUserDataValue(unsigned userDataValue) const {
  if (userDataValue < static_cast<unsigned>(UserDataMapping::GlobalTable)) {
    return false;
  }
  return userDataValue < static_cast<unsigned>(UserDataMapping::DescriptorSet0);
}

// =====================================================================================================================
// @param userDataValue : The value to be written into a user data entry.
// @returns : True if the user data value corresponds to an unlinked descriptor set.
bool PatchEntryPointMutate::isUnlinkedDescriptorSetValue(unsigned userDataValue) const {
  if (userDataValue < static_cast<unsigned>(UserDataMapping::DescriptorSet0)) {
    return false;
  }
  return userDataValue <= static_cast<unsigned>(UserDataMapping::DescriptorSetMax);
}

// =====================================================================================================================
// Add a UserDataArg to the appropriate vector for each special argument (e.g. ViewId) needed in user data SGPRs.
// In here, we need to check whether an argument is needed in two ways:
// 1. Whether a flag is set saying it will be needed after PatchEntryPointMutate
// 2. Whether there is an actual use of the special user data value (lgc.special.user.data call) generated
//    before PatchEntryPointMutate, which we check with userDataUsage->isSpecialUserDataUsed().
//
// @param userDataArgs : Vector to add args to when they need to go before user data nodes (just streamout)
// @param specialUserDataArgs : Vector to add args to when they need to go after user data nodes (all the rest)
// @param builder : IRBuilder to get types from
void PatchEntryPointMutate::addSpecialUserDataArgs(SmallVectorImpl<UserDataArg> &userDataArgs,
                                                   SmallVectorImpl<UserDataArg> &specialUserDataArgs,
                                                   IRBuilder<> &builder) {

  auto userDataUsage = getUserDataUsage(m_shaderStage);
  auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
  auto &entryArgIdxs = intfData->entryArgIdxs;
  bool enableNgg = m_pipelineState->isGraphics() ? m_pipelineState->getNggControl()->enableNgg : false;

  if (m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessControl ||
      m_shaderStage == ShaderStageTessEval || m_shaderStage == ShaderStageGeometry) {
    // Shader stage in the vertex-processing half of a graphics pipeline.
    // We need to ensure that the layout is the same between two shader stages that will be merged on GFX9+,
    // that is, VS-TCS, VS-GS (if no tessellation), TES-GS.

    // NOTE: The user data to emulate gl_ViewIndex is somewhat common. To make it consistent for GFX9
    // merged shader, we place it prior to any other special user data.
    if (m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable) {
      unsigned *argIdx = nullptr;
      auto userDataValue = UserDataMapping::ViewId;
      switch (m_shaderStage) {
      case ShaderStageVertex:
        argIdx = &entryArgIdxs.vs.viewId;
        break;
      case ShaderStageTessControl:
        argIdx = &entryArgIdxs.tcs.viewId;
        break;
      case ShaderStageTessEval:
        argIdx = &entryArgIdxs.tes.viewId;
        break;
      case ShaderStageGeometry:
        argIdx = &entryArgIdxs.gs.viewId;
        break;
      default:
        llvm_unreachable("Unexpected shader stage");
      }
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "viewId", userDataValue, argIdx));
    }

    // NOTE: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent
    // with PAL's GS on-chip behavior (VS is in NGG primitive shader).
    bool wantEsGsLdsSize = false;
    switch (getMergedShaderStage(m_shaderStage)) {
    case ShaderStageVertex:
      wantEsGsLdsSize = enableNgg;
      break;
    case ShaderStageTessControl:
      wantEsGsLdsSize = false;
      break;
    case ShaderStageTessEval:
      wantEsGsLdsSize = enableNgg;
      break;
    case ShaderStageGeometry:
      wantEsGsLdsSize = (m_pipelineState->isGsOnChip() && cl::InRegEsGsLdsSize) || enableNgg;
      break;
    default:
      llvm_unreachable("Unexpected shader stage");
    }
    if (wantEsGsLdsSize) {
      auto userDataValue = UserDataMapping::EsGsLdsSize;
      // For a standalone TCS (which can only happen in unit testing, not in a real pipeline), don't add
      // the PAL metadata for it, for consistency with the old code.
      if (m_shaderStage == ShaderStageVertex && !m_pipelineState->hasShaderStage(ShaderStageVertex))
        userDataValue = UserDataMapping::Invalid;
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "esGsLdsSize", userDataValue));
    }

    if (getMergedShaderStage(m_shaderStage) == getMergedShaderStage(ShaderStageVertex)) {
      // This is the VS, or the shader that VS is merged into on GFX9+.
      auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
      auto vsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);

      // Detect whether this is an unlinked compile that will need a fetch shader. If so, we need to
      // add the vertex buffer table and base vertex and base instance, even if they appear unused here.
      bool willHaveFetchShader = m_pipelineState->getPalMetadata()->getVertexFetchCount() != 0;

      // Vertex buffer table.
      if (willHaveFetchShader || userDataUsage->isSpecialUserDataUsed(UserDataMapping::VertexBufferTable)) {
        specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "vertexBufferTable",
                                                  UserDataMapping::VertexBufferTable,
                                                  &vsIntfData->entryArgIdxs.vs.vbTablePtr));
      }

      // Base vertex and base instance.
      if (willHaveFetchShader || vsResUsage->builtInUsage.vs.baseVertex || vsResUsage->builtInUsage.vs.baseInstance ||
          userDataUsage->isSpecialUserDataUsed(UserDataMapping::BaseVertex) ||
          userDataUsage->isSpecialUserDataUsed(UserDataMapping::BaseInstance)) {
        specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "baseVertex", UserDataMapping::BaseVertex,
                                                  &vsIntfData->entryArgIdxs.vs.baseVertex));
        specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "baseInstance", UserDataMapping::BaseInstance,
                                                  &vsIntfData->entryArgIdxs.vs.baseInstance));
      }

      // Draw index.
      if (userDataUsage->isSpecialUserDataUsed(UserDataMapping::DrawIndex))
        specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "drawIndex", UserDataMapping::DrawIndex));
    }

  } else if (m_shaderStage == ShaderStageCompute) {
    // Pass the gl_NumWorkgroups pointer in user data registers.
    // Always enable this, even if unused, if compute library is in use.
    // Unlike all the special user data values above, which go after the user data node args, this goes before.
    // That is to ensure that, with a compute pipeline using a library, library code knows where to find it
    // even if it thinks that the user data layout is a prefix of what the pipeline thinks it is.
    if (isComputeWithCalls() || userDataUsage->isSpecialUserDataUsed(UserDataMapping::Workgroup)) {
      auto numWorkgroupsPtrTy = PointerType::get(FixedVectorType::get(builder.getInt32Ty(), 3), ADDR_SPACE_CONST);
      userDataArgs.push_back(UserDataArg(numWorkgroupsPtrTy, "numWorkgroupsPtr", UserDataMapping::Workgroup, nullptr));
    }
  } else if (m_shaderStage == ShaderStageTask) {
    // Draw index.
    if (userDataUsage->isSpecialUserDataUsed(UserDataMapping::DrawIndex))
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "drawIndex", UserDataMapping::DrawIndex));

    specialUserDataArgs.push_back(UserDataArg(FixedVectorType::get(builder.getInt32Ty(), 3), "meshTaskDispatchDims",
                                              UserDataMapping::MeshTaskDispatchDims,
                                              &intfData->entryArgIdxs.task.dispatchDims));
    specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "meshTaskRingIndex",
                                              UserDataMapping::MeshTaskRingIndex,
                                              &intfData->entryArgIdxs.task.baseRingEntryIndex));
    if (m_pipelineState->needSwMeshPipelineStats()) {
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "meshPipeStatsBuf",
                                                UserDataMapping::MeshPipeStatsBuf,
                                                &intfData->entryArgIdxs.task.pipeStatsBuf));
    }
  } else if (m_shaderStage == ShaderStageMesh) {
    if (m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->builtInUsage.mesh.drawIndex) {
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "drawIndex", UserDataMapping::DrawIndex,
                                                &intfData->entryArgIdxs.mesh.drawIndex));
    }
    if (m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable) {
      specialUserDataArgs.push_back(
          UserDataArg(builder.getInt32Ty(), "viewId", UserDataMapping::ViewId, &intfData->entryArgIdxs.mesh.viewId));
    }
    specialUserDataArgs.push_back(UserDataArg(FixedVectorType::get(builder.getInt32Ty(), 3), "meshTaskDispatchDims",
                                              UserDataMapping::MeshTaskDispatchDims,
                                              &intfData->entryArgIdxs.mesh.dispatchDims));
    specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "meshTaskRingIndex",
                                              UserDataMapping::MeshTaskRingIndex,
                                              &intfData->entryArgIdxs.mesh.baseRingEntryIndex));
    if (m_pipelineState->needSwMeshPipelineStats()) {
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "meshPipeStatsBuf",
                                                UserDataMapping::MeshPipeStatsBuf,
                                                &intfData->entryArgIdxs.mesh.pipeStatsBuf));
    }
  } else if (m_shaderStage == ShaderStageFragment) {
    if (m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable &&
        m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs.viewIndex) {
      // NOTE: Only add special user data of view index when multi-view is enabled and gl_ViewIndex is used in fragment
      // shader.
      specialUserDataArgs.push_back(
          UserDataArg(builder.getInt32Ty(), "viewId", UserDataMapping::ViewId, &intfData->entryArgIdxs.fs.viewId));
    }

    if (userDataUsage->isSpecialUserDataUsed(UserDataMapping::ColorExportAddr)) {
      assert(m_pipelineState->isUnlinked() && m_pipelineState->getOptions().enableColorExportShader);
      specialUserDataArgs.push_back(
          UserDataArg(builder.getInt32Ty(), "colorExpAddr", UserDataMapping::ColorExportAddr));
    }

    if (m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs.runAtSampleRate &&
        (m_pipelineState->isUnlinked() || m_pipelineState->getRasterizerState().dynamicSampleInfo)) {
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "sampleInfo", UserDataMapping::SampleInfo,
                                                &intfData->entryArgIdxs.fs.sampleInfo));
    }
  }

  // Allocate register for stream-out buffer table, to go before the user data node args (unlike all the ones
  // above, which go after the user data node args).
  if (userDataUsage->usesStreamOutTable || userDataUsage->isSpecialUserDataUsed(UserDataMapping::StreamOutTable)) {
    if (enableNgg || !m_pipelineState->hasShaderStage(ShaderStageCopyShader) && m_pipelineState->enableXfb()) {
      // If no NGG, stream out table will be set to copy shader's user data entry, we should not set it duplicately.
      unsigned *tablePtr = nullptr;

      switch (m_shaderStage) {
      case ShaderStageVertex:
        tablePtr = &intfData->entryArgIdxs.vs.streamOutData.tablePtr;
        break;
      case ShaderStageTessEval:
        tablePtr = &intfData->entryArgIdxs.tes.streamOutData.tablePtr;
        break;
      case ShaderStageGeometry:
        if (m_pipelineState->enableSwXfb()) {
          tablePtr = &intfData->entryArgIdxs.gs.streamOutData.tablePtr;
        } else {
          assert(m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 10);
          // Allocate dummy stream-out register for geometry shader
          userDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "dummyStreamOut"));
        }

        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }

      if (tablePtr) {
        userDataArgs.push_back(
            UserDataArg(builder.getInt32Ty(), "streamOutTable", UserDataMapping::StreamOutTable, tablePtr));
      }
    }
  }

  // NOTE: For GFX11+, the SW stream-out needs an additional special user data SGPR to store the stream-out control
  // buffer address.
  if (m_pipelineState->enableSwXfb()) {
    unsigned *controlBufPtr = nullptr;

    switch (m_shaderStage) {
    case ShaderStageVertex:
      controlBufPtr = &intfData->entryArgIdxs.vs.streamOutData.controlBufPtr;
      break;
    case ShaderStageTessEval:
      controlBufPtr = &intfData->entryArgIdxs.tes.streamOutData.controlBufPtr;
      break;
    case ShaderStageGeometry:
      controlBufPtr = &intfData->entryArgIdxs.gs.streamOutData.controlBufPtr;
      break;
    default:
      // Ignore other shader stages
      break;
    }

    if (controlBufPtr) {
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "streamOutControlBuf",
                                                UserDataMapping::StreamOutControlBuf, controlBufPtr));
    }
  }
}

// =====================================================================================================================
// Determine the final list of user data args and whether we require a spill table.
//
// @param [in/out] userDataArgs : Input the array of prefix "system value" user data arguments; outputs the final list
//                                of user data arguments
// @param specialUserDataArgs : list of suffix "system value" user data arguments
// @param builder : IRBuilder to get types from
void PatchEntryPointMutate::finalizeUserDataArgs(SmallVectorImpl<UserDataArg> &userDataArgs,
                                                 ArrayRef<UserDataArg> specialUserDataArgs, IRBuilder<> &builder) {
  auto userDataUsage = getUserDataUsage(m_shaderStage);

  // In compute-with-calls, we need to ensure that the compute shader and library code agree that s15 is the spill
  // table pointer, even if it is not needed, because library code does not know whether a spill table pointer is
  // needed in the pipeline. Thus we cannot use s15 for anything else. Using the single-arg UserDataArg
  // constructor like this means that the arg is not used, so it will not be set up in PAL metadata.
  bool spill = userDataUsage->haveDynamicUserDataLoads || m_computeWithCalls;

  // Figure out how many sgprs we have available for userDataArgs.
  // We have s0-s31 (s0-s15 for <=GFX8, or for a compute/task shader on any chip) for everything, so take off the number
  // of registers used by specialUserDataArgs.
  unsigned userDataAvailable = (m_shaderStage == ShaderStageCompute || m_shaderStage == ShaderStageTask)
                                   ? InterfaceData::MaxCsUserDataCount
                                   : m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount;

  // FIXME Restricting user data as the backend does not support more sgprs as arguments
  if (m_computeWithCalls && userDataAvailable > 16)
    userDataAvailable = 16;

  for (const auto &userDataArg : specialUserDataArgs)
    userDataAvailable -= userDataArg.argDwordSize;
  // ... and the one used by the spill table if already added.
  if (spill)
    userDataAvailable -= 1;

  unsigned userDataEnd = 0;
  for (const auto &userDataArg : userDataArgs)
    userDataEnd += userDataArg.argDwordSize;
  assert(userDataEnd < userDataAvailable && "too many system value user data args");

  if (m_computeWithCalls) {
    // In compute with calls, the user data layout must be the same across all shaders and therefore cannot depend
    // on an individual shader's usage pattern.
    unsigned userDataSgprs = userDataAvailable - userDataEnd;
    unsigned userDataDwords = 0;
    for (const auto &node : m_pipelineState->getUserDataNodes())
      userDataDwords = std::max(userDataDwords, node.offsetInDwords + node.sizeInDwords);

    userDataUsage->entryArgIdxs.resize(userDataDwords);
    for (unsigned i = 0; i != userDataSgprs; ++i) {
      if (i < userDataDwords)
        userDataArgs.emplace_back(builder.getInt32Ty(), "userdata" + Twine(i), i, &userDataUsage->entryArgIdxs[i]);
      else
        userDataArgs.emplace_back(builder.getInt32Ty(), "pad" + Twine(i));
    }
    if (userDataSgprs < userDataDwords)
      m_pipelineState->getPalMetadata()->setUserDataSpillUsage(userDataSgprs);

    // We must conservatively assume that there are functions with dynamic push constant accesses, and that therefore
    // the push constants must be fully available in the spill region even if they fit (partially) into SGPRs.
    const ResourceNode *node = m_pipelineState->findSingleRootResourceNode(ResourceNodeType::PushConst, m_shaderStage);
    if (node)
      m_pipelineState->getPalMetadata()->setUserDataSpillUsage(node->offsetInDwords);
  } else {
    // Greedily fit as many generic user data arguments as possible.
    // Pre-allocate entryArgIdxs since we rely on stable pointers.
    userDataUsage->entryArgIdxs.resize(userDataUsage->loadSizes.size());

    unsigned lastIdx = 0;
    unsigned lastSize = 0;
    for (unsigned i = 0; i < userDataUsage->loadSizes.size();) {
      unsigned size = userDataUsage->loadSizes[i];
      if (size == 0) {
        ++i;
        continue;
      }

      if (userDataEnd + size > userDataAvailable) {
        // We ran out of SGPR space -- need to spill.
        if (!spill) {
          --userDataAvailable;
          spill = true;
          if (userDataEnd > userDataAvailable) {
            // No space left for the spill table, we need to backtrack.
            assert(lastSize > 0);
            userDataArgs.erase(userDataArgs.end() - lastSize, userDataArgs.end());
            userDataEnd -= lastSize;
            assert(userDataEnd <= userDataAvailable);
            m_pipelineState->getPalMetadata()->setUserDataSpillUsage(lastIdx);

            // Retry since the current load may now fit.
            continue;
          }
        }

        m_pipelineState->getPalMetadata()->setUserDataSpillUsage(i);

        if (userDataEnd >= userDataAvailable)
          break; // All SGPRs in use, may as well give up.

        // Subsequent loads may be smaller and could still fit.
        ++i;
        continue;
      }

      lastSize = size;
      lastIdx = i;
      for (;;) {
        userDataArgs.emplace_back(builder.getInt32Ty(), "userdata" + Twine(i), i, &userDataUsage->entryArgIdxs[i]);
        ++userDataEnd;
        ++i;
        --size;

        if (!size)
          break;

        // Depending on the order in which loads were originally added, we may still have some unsplit overlapping
        // loads registered. Split them now.
        if (userDataUsage->loadSizes[i] && userDataUsage->loadSizes[i] > size)
          userDataUsage->addLoad(i + size, userDataUsage->loadSizes[i] - size);
      }
    }
  }

  // Add the special args and the spill table pointer (if any).
  // (specialUserDataArgs is empty for compute, and thus for compute-with-calls.)
  userDataArgs.insert(userDataArgs.end(), specialUserDataArgs.begin(), specialUserDataArgs.end());
  if (spill) {
    userDataArgs.emplace_back(builder.getInt32Ty(), "spillTable", UserDataMapping::SpillTable,
                              &userDataUsage->spillTableEntryArgIdx);
  }
}

// =====================================================================================================================
// Get UserDataUsage struct for the merged shader stage that contains the given shader stage
//
// @param stage : Shader stage
PatchEntryPointMutate::UserDataUsage *PatchEntryPointMutate::getUserDataUsage(ShaderStage stage) {
  stage = getMergedShaderStage(stage);
  m_userDataUsage.resize(std::max(m_userDataUsage.size(), static_cast<size_t>(stage) + 1));
  if (!m_userDataUsage[stage])
    m_userDataUsage[stage] = std::make_unique<UserDataUsage>();
  return &*m_userDataUsage[stage];
}

// =====================================================================================================================
// Get the shader stage that the given shader stage is merged into.
// For GFX9+:
// VS -> TCS (if it exists)
// VS -> GS (if it exists)
// TES -> GS (if it exists)
//
// @param stage : Shader stage
ShaderStage PatchEntryPointMutate::getMergedShaderStage(ShaderStage stage) const {
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 9) {
    switch (stage) {
    case ShaderStageVertex:
      if (m_pipelineState->hasShaderStage(ShaderStageTessControl))
        return ShaderStageTessControl;
      LLVM_FALLTHROUGH;
    case ShaderStageTessEval:
      if (m_pipelineState->hasShaderStage(ShaderStageGeometry))
        return ShaderStageGeometry;
      break;
    default:
      break;
    }
  }
  return stage;
}

// =====================================================================================================================
bool PatchEntryPointMutate::isComputeWithCalls() const {
  return m_computeWithCalls;
}

// =====================================================================================================================
bool PatchEntryPointMutate::UserDataUsage::isSpecialUserDataUsed(UserDataMapping kind) {
  unsigned index = static_cast<unsigned>(kind) - static_cast<unsigned>(UserDataMapping::GlobalTable);
  return specialUserData.size() > index && !specialUserData[index].users.empty();
}

// =====================================================================================================================
void PatchEntryPointMutate::UserDataUsage::addLoad(unsigned dwordOffset, unsigned dwordSize) {
  assert(dwordOffset + dwordSize <= 256 && "shader uses a user data region that is too large");

  if (dwordOffset + dwordSize > loadSizes.size())
    loadSizes.resize(dwordOffset + dwordSize);

  while (dwordSize != 0) {
    if (!loadSizes[dwordOffset]) {
      loadSizes[dwordOffset] = dwordSize;
      return;
    }

    // Split our load or the pre-existing load, whichever is larger.
    unsigned max = std::max(dwordSize, loadSizes[dwordOffset]);
    unsigned min = std::min(dwordSize, loadSizes[dwordOffset]);
    loadSizes[dwordOffset] = min;
    dwordOffset += min;
    dwordSize = max - min;
  }
}
