/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  MutateEntryPoint.cpp
 * @brief The lgc::MutateEntryPoint pass determines the final user data layout of shaders.
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

#include "lgc/lowering/MutateEntryPoint.h"
#include "ShaderMerger.h"
#include "compilerutils/CompilerUtils.h"
#include "llpc/GpurtEnums.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcDialect.h"
#include "lgc/LgcRtDialect.h"
#include "lgc/lowering/ShaderInputs.h"
#include "lgc/lowering/SystemValues.h"
#include "lgc/state/AbiMetadata.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/AddressExtender.h"
#include "lgc/util/BuilderBase.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/Analysis/AliasAnalysis.h" // for MemoryEffects
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <optional>

#define DEBUG_TYPE "lgc-mutate-entry-point"

using namespace llvm;
using namespace lgc;
using namespace cps;

static cl::opt<bool> UseInitWholeWave("lgc-use-init-whole-wave",
                                      cl::desc("Use the llvm.amdgcn.init.whole.wave intrinsic"), cl::init(false));

// =====================================================================================================================
bool MutateEntryPoint::useInitWholeWave() const {
  return UseInitWholeWave && m_initWholeWaveId != llvm::Intrinsic::not_intrinsic;
}

bool MutateEntryPoint::useDeadInsteadOfPoison() const {
  return m_deadId != llvm::Intrinsic::not_intrinsic;
}

// =====================================================================================================================
MutateEntryPoint::MutateEntryPoint() : m_hasTs(false), m_hasGs(false) {
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 513481
  m_setInactiveChainArgId = Function::lookupIntrinsicID("llvm.amdgcn.set.inactive.chain.arg");
  m_initWholeWaveId = Function::lookupIntrinsicID("llvm.amdgcn.init.whole.wave");
  m_deadId = Function::lookupIntrinsicID("llvm.amdgcn.dead");
#else
  m_setInactiveChainArgId = Intrinsic::lookupIntrinsicID("llvm.amdgcn.set.inactive.chain.arg");
  m_initWholeWaveId = Intrinsic::lookupIntrinsicID("llvm.amdgcn.init.whole.wave");
  m_deadId = Intrinsic::lookupIntrinsicID("llvm.amdgcn.dead");
#endif
}

// =====================================================================================================================
MutateEntryPoint::UserDataArg::UserDataArg(llvm::Type *argTy, const llvm::Twine &name, unsigned userDataValue,
                                           unsigned *argIndex)
    : argTy(argTy), name(name.str()), userDataValue(userDataValue), argIndex(argIndex) {
  if (llvm::isa<llvm::PointerType>(argTy))
    argDwordSize = argTy->getPointerAddressSpace() == ADDR_SPACE_CONST_32BIT ? 1 : 2;
  else
    argDwordSize = argTy->getPrimitiveSizeInBits() / 32;
}

// =====================================================================================================================
MutateEntryPoint::UserDataArg::UserDataArg(llvm::Type *argTy, const llvm::Twine &name, UserDataMapping userDataValue,
                                           unsigned *argIndex)
    : UserDataArg(argTy, name, static_cast<unsigned>(userDataValue), argIndex) {
}

// =====================================================================================================================
// Executes this LGC lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses MutateEntryPoint::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);

  LLVM_DEBUG(dbgs() << "Run the pass Mutate-Entry-Point\n");

  LgcLowering::init(&module);

  m_pipelineState = pipelineState;

  const auto stageMask = m_pipelineState->getShaderStageMask();
  m_hasTs = stageMask.contains_any({ShaderStage::TessControl, ShaderStage::TessEval});
  m_hasGs = stageMask.contains(ShaderStage::Geometry);

  // Gather user data usage.
  gatherUserDataUsage(&module);

  // Create ShaderInputs object and gather shader input usage.
  ShaderInputs shaderInputs;
  shaderInputs.gatherUsage(module);
  setupComputeWithCalls(&module);

  if (m_pipelineState->isGraphics()) {
    // Process each shader in turn, but not the copy shader.
    for (auto stage : ShaderStagesNative) {
      m_entryPoint = pipelineShaders.getEntryPoint(stage);
      if (m_entryPoint) {
        // ToDo: This should always be skipped since we don't implement CPS metadata yet.
        assert(!lgc::cps::isCpsFunction(*m_entryPoint) && "CPS support not implemented yet");

        m_shaderStage = stage;
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

  if (!m_pipelineState->isGraphics())
    processCsGroupMemcpy(module);

  processDriverTableLoad(module);

  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Merge the input into a single struct type.
//
// @param builder : IR builder
// @param input : An array of inputs to be structure members
// @returns : A structure-typed value with inputs as its members
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
//
// @param builder : IR builder
// @param input : An array of i32 scalar inputs
// @returns : An arrayed value of inputs
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
// Process LoadDriverTableEntryOp.
//
// @param module : LLVM module
void MutateEntryPoint::processDriverTableLoad(Module &module) {
  SmallVector<CallInst *> callsToRemove;

  struct Payload {
    SmallVectorImpl<CallInst *> &callsToRemove;
    MutateEntryPoint *self;
  };

  Payload payload = {callsToRemove, this};

  static auto visitor = llvm_dialects::VisitorBuilder<Payload>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .add<LoadDriverTableEntryOp>([](auto &payload, auto &op) {
                              payload.self->lowerDriverTableLoad(op);
                              payload.callsToRemove.push_back(&op);
                            })
                            .build();
  visitor.visit(payload, module);

  for (auto call : payload.callsToRemove)
    call->eraseFromParent();
}

// =====================================================================================================================
// Lower LoadDriverTableEntryOp.
//
// @param loadDriverTablePtrOp : Call instruction to load driver table pointer
void MutateEntryPoint::lowerDriverTableLoad(LoadDriverTableEntryOp &loadDriverTablePtrOp) {
  BuilderBase builder(&loadDriverTablePtrOp);
  Function *entryPoint = loadDriverTablePtrOp.getFunction();
  builder.SetInsertPoint(&loadDriverTablePtrOp);

  PipelineSystemValues pipelineSysValues;
  pipelineSysValues.initialize(m_pipelineState);

  unsigned offset = loadDriverTablePtrOp.getOffset();
  Value *desc = pipelineSysValues.get(entryPoint)->loadDescFromDriverTable(offset, builder);
  loadDriverTablePtrOp.replaceAllUsesWith(desc);
}

// =====================================================================================================================
// Process GroupMemcpyOp.
//
// @param module : LLVM module
void MutateEntryPoint::processCsGroupMemcpy(Module &module) {
  SmallVector<CallInst *> callsToRemove;

  struct Payload {
    SmallVectorImpl<CallInst *> &callsToRemove;
    MutateEntryPoint *self;
  };

  Payload payload = {callsToRemove, this};

  static auto visitor = llvm_dialects::VisitorBuilder<Payload>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .add<GroupMemcpyOp>([](auto &payload, auto &op) {
                              payload.self->lowerCsGroupMemcpy(op);
                              payload.callsToRemove.push_back(&op);
                            })
                            .build();
  visitor.visit(payload, module);

  for (auto call : payload.callsToRemove)
    call->eraseFromParent();
}

// =====================================================================================================================
// Lower GroupMemcpyOp - Copy memory using threads in a workgroup (scope=2) or subgroup (scope=3).
//
// @param groupMemcpyOp : Call instruction to do group memory copy
void MutateEntryPoint::lowerCsGroupMemcpy(GroupMemcpyOp &groupMemcpyOp) {
  BuilderBase builder(groupMemcpyOp.getContext());
  Function *entryPoint = groupMemcpyOp.getFunction();
  builder.SetInsertPoint(&groupMemcpyOp);

  unsigned scopeSize = 0;
  Value *threadIndex = nullptr;

  auto scope = groupMemcpyOp.getScope();
  if (scope == MemcpyScopeWorkGroup) {
    unsigned workgroupSize[3] = {};
    auto shaderModes = m_pipelineState->getShaderModes();
    assert(getShaderStage(entryPoint) == ShaderStage::Compute);

    Module &module = *groupMemcpyOp.getModule();
    workgroupSize[0] = shaderModes->getComputeShaderMode(module).workgroupSizeX;
    workgroupSize[1] = shaderModes->getComputeShaderMode(module).workgroupSizeY;
    workgroupSize[2] = shaderModes->getComputeShaderMode(module).workgroupSizeZ;

    scopeSize = workgroupSize[0] * workgroupSize[1] * workgroupSize[2];

    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Compute)->entryArgIdxs.cs;
    Value *threadIdInGroup = getFunctionArgument(entryPoint, entryArgIdxs.localInvocationId);
    Value *threadIdComp[3];

    auto gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
    if (gfxIp.major < 11) {
      for (unsigned idx = 0; idx < 3; idx++)
        threadIdComp[idx] = builder.CreateExtractElement(threadIdInGroup, idx);
    } else {
      // The local invocation ID is packed to VGPR0 on GFX11+ with the following layout:
      //
      //   +-----------------------+-----------------------+-----------------------+
      //   | Local Invocation ID Z | Local Invocation ID Y | Local Invocation ID X |
      //   | [29:20]               | [19:10]               | [9:0]                 |
      //   +-----------------------+-----------------------+-----------------------+
      // localInvocationIdZ = localInvocationId[29:20]
      threadIdComp[2] = builder.CreateAnd(builder.CreateLShr(threadIdInGroup, 20), 0x3FF, "localInvocationIdZ");
      // localInvocationIdY = localInvocationId[19:10]
      threadIdComp[1] = builder.CreateAnd(builder.CreateLShr(threadIdInGroup, 10), 0x3FF, "localInvocationIdY");
      // localInvocationIdX = localInvocationId[9:0]
      threadIdComp[0] = builder.CreateAnd(threadIdInGroup, 0x3FF, "localInvocationIdX");
    }

    // LocalInvocationIndex is
    // (LocalInvocationId.Z * WorkgroupSize.Y + LocalInvocationId.Y) * WorkGroupSize.X + LocalInvocationId.X
    // tidigCompCnt is not always set to 2(xyz) if groupSizeY and/or groupSizeZ are 1. See RegisterMetadataBuilder.cpp.
    threadIndex = builder.getInt32(0);
    if (workgroupSize[2] > 1)
      threadIndex = builder.CreateMul(threadIdComp[2], builder.getInt32(workgroupSize[1]));
    if (workgroupSize[1] > 1)
      threadIndex =
          builder.CreateMul(builder.CreateAdd(threadIndex, threadIdComp[1]), builder.getInt32(workgroupSize[0]));
    threadIndex = builder.CreateAdd(threadIndex, threadIdComp[0]);
  } else {
    llvm_unreachable("Unsupported scope!");
  }

  processGroupMemcpy(groupMemcpyOp, builder, threadIndex, scopeSize);
}

// =====================================================================================================================
// Common code to do the memory copy part of GroupMemcpyOp, used by MeshTaskShader and PatchEntryPointMutate.
//
// @param groupMemcpyOp : Call instruction to do group memory copy
// @param builder : The IR builder for inserting instructions
// @param threadIndex : Current thread index
// @param scopeSize : The copy size in bytes for specified scope (currently workgroup only, maybe subgroup).
void MutateEntryPoint::processGroupMemcpy(GroupMemcpyOp &groupMemcpyOp, BuilderBase &builder, Value *threadIndex,
                                          unsigned scopeSize) {
  auto dst = groupMemcpyOp.getDst();
  auto src = groupMemcpyOp.getSrc();
  auto len = groupMemcpyOp.getSize();

  // Copy in 16-bytes if possible
  unsigned wideDwords = 4;
  // If either pointer is in LDS, copy in 8-bytes
  if (src->getType()->getPointerAddressSpace() == ADDR_SPACE_LOCAL ||
      dst->getType()->getPointerAddressSpace() == ADDR_SPACE_LOCAL)
    wideDwords = 2;

  unsigned baseOffset = 0;

  auto copyFunc = [&](Type *copyTy, unsigned copySize) {
    Value *offset =
        builder.CreateAdd(builder.getInt32(baseOffset), builder.CreateMul(threadIndex, builder.getInt32(copySize)));
    Value *dstPtr = builder.CreateGEP(builder.getInt8Ty(), dst, offset);
    Value *srcPtr = builder.CreateGEP(builder.getInt8Ty(), src, offset);
    Value *data = builder.CreateLoad(copyTy, srcPtr);
    builder.CreateStore(data, dstPtr);
  };

  unsigned wideDwordsCopySize = sizeof(unsigned) * wideDwords;
  Type *wideDwordsTy = ArrayType::get(builder.getInt32Ty(), wideDwords);
  while (baseOffset + wideDwordsCopySize * scopeSize <= len) {
    copyFunc(wideDwordsTy, wideDwordsCopySize);
    baseOffset += wideDwordsCopySize * scopeSize;
  }

  unsigned dwordCopySize = sizeof(unsigned);
  Type *dwordTy = builder.getInt32Ty();
  while (baseOffset + dwordCopySize * scopeSize <= len) {
    copyFunc(dwordTy, dwordCopySize);
    baseOffset += dwordCopySize * scopeSize;
  }

  unsigned remainingBytes = len - baseOffset;

  if (remainingBytes) {
    assert(remainingBytes % 4 == 0);
    BasicBlock *afterBlock = groupMemcpyOp.getParent();
    BasicBlock *beforeBlock = splitBlockBefore(afterBlock, &groupMemcpyOp, nullptr, nullptr, nullptr);
    beforeBlock->takeName(afterBlock);
    afterBlock->setName(Twine(beforeBlock->getName()) + ".afterGroupMemcpyTail");

    // Split to create a tail copy block, empty except for an unconditional branch to afterBlock.
    BasicBlock *copyBlock = splitBlockBefore(afterBlock, &groupMemcpyOp, nullptr, nullptr, nullptr, ".groupMemcpyTail");
    // Change the branch at the end of beforeBlock to be conditional.
    beforeBlock->getTerminator()->eraseFromParent();
    builder.SetInsertPoint(beforeBlock);

    Value *indexInRange = builder.CreateICmpULT(threadIndex, builder.getInt32(remainingBytes / 4));

    builder.CreateCondBr(indexInRange, copyBlock, afterBlock);
    // Create the copy instructions.
    builder.SetInsertPoint(copyBlock->getTerminator());
    copyFunc(dwordTy, dwordCopySize);
  }
}

// =====================================================================================================================
// Lower as.continuation.reference call.
//
// @param asCpsReferenceOp: the instruction
void MutateEntryPoint::lowerAsCpsReference(cps::AsContinuationReferenceOp &asCpsReferenceOp) {
  BuilderBase builder(&asCpsReferenceOp);

  Value *reloc = nullptr;
  Function &callee = *cast<Function>(asCpsReferenceOp.getFn());

  if (isDynamicVgprEnabled()) {
    auto funcName = callee.getName();
    std::string relocName = "_dvgpr$" + funcName.str();
    reloc = builder.CreateRelocationConstant(relocName);
  }

  Value *loweredReference = lgc::cps::lowerAsContinuationReference(builder, asCpsReferenceOp, reloc);

  assert(asCpsReferenceOp.getType()->getIntegerBitWidth() == 32);

  loweredReference =
      builder.CreateAdd(loweredReference, builder.getIntN(loweredReference->getType()->getScalarSizeInBits(),
                                                          static_cast<uint64_t>(cps::getCpsLevelFromFunction(callee))));

  asCpsReferenceOp.replaceAllUsesWith(loweredReference);
}

// =====================================================================================================================
// Lower calls to cps function as well as return instructions.
//
// @param func : the function to be processed
// @param shaderInputs: the ShaderInputs information for the parent function. This is only used for continufy based
// continuation transform, under which we still need to pass ShaderInput arguments(WorkgroupId/LocalInvocationId) during
// cps chain call.
bool MutateEntryPoint::lowerCpsOps(Function *func, ShaderInputs *shaderInputs) {
  struct Payload {
    SmallVector<cps::JumpOp *> jumps;
    SmallVector<CallInst *> tobeErased;
    MutateEntryPoint *self = nullptr;
  };
  Payload payload;
  payload.self = this;

  static auto visitor = llvm_dialects::VisitorBuilder<Payload>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .add<cps::JumpOp>([](auto &payload, auto &op) { payload.jumps.push_back(&op); })
                            .add<cps::AsContinuationReferenceOp>([](auto &payload, auto &op) {
                              payload.self->lowerAsCpsReference(op);
                              payload.tobeErased.push_back(&op);
                            })
                            .build();
  visitor.visit(payload, *func);

  for (auto *call : payload.tobeErased)
    call->eraseFromParent();

  bool isCpsFunc = cps::isCpsFunction(*func);
  if (!isCpsFunc && payload.jumps.empty())
    return false;

  // Get the number of user-data arguments.
  const auto &mode = m_pipelineState->getShaderModes()->getComputeShaderMode();
  bool haveLocalInvocationId = !mode.noLocalInvocationIdInCalls;
  unsigned numShaderArg;
  unsigned numUserdata;
  if (!isCpsFunc) {
    SmallVector<Type *, 8> argTys;
    SmallVector<std::string, 8> argNames;
    generateEntryPointArgTys(shaderInputs, nullptr, argTys, argNames, 0);
    assert(argNames.back() == "LocalInvocationId");
    numShaderArg = haveLocalInvocationId ? argTys.size() - 1 : argTys.size();
    numUserdata = argTys.size() - 1;
  } else {
    numShaderArg = m_cpsShaderInputCache.getTypes().size();
    numUserdata = haveLocalInvocationId ? numShaderArg - 1 : numShaderArg;
    if (isDynamicVgprEnabled()) {
      numUserdata--;
      assert(m_cpsShaderInputCache.getNames().back() == "MaxOutgoingVgprCount");
      assert(haveLocalInvocationId == (m_cpsShaderInputCache.getNames()[numShaderArg - 2] == "LocalInvocationId"));
    } else {
      assert(haveLocalInvocationId == (m_cpsShaderInputCache.getNames().back() == "LocalInvocationId"));
    }
  }

  // Get all the return instructions.
  SmallVector<ReturnInst *> retInstrs;
  for (BasicBlock &block : *func)
    if (ReturnInst *ret = dyn_cast<ReturnInst>(block.getTerminator()))
      retInstrs.push_back(ret);

  auto *tailBlock = BasicBlock::Create(func->getContext(), "tail.block", func);

  SmallVector<CpsExitInfo> exitInfos;
  IRBuilder<> builder(func->getContext());

  // If init.whole.wave is available, generate a new entry block to initialize the whole wave:
  // entry.block:
  //    %orig.exec = llvm.amdgcn.init.whole.wave()
  //    br %orig.exec, %func, %tail.block
  // func:
  //    ...
  //    br %tail.block
  //  tail.block:
  //    ...
  bool useIWW = useInitWholeWave();
  bool handleInactiveVgprs = isCpsFunc && useIWW;
  Value *dead = PoisonValue::get(builder.getInt32Ty());
  if (handleInactiveVgprs) {
    auto *entryBlock = &func->getEntryBlock();
    BasicBlock *shaderBlock = entryBlock->splitBasicBlock(entryBlock->getFirstNonPHIOrDbgOrAlloca());
    builder.SetInsertPoint(entryBlock, entryBlock->getFirstNonPHIOrDbgOrAlloca());

    // For the extra VGPR args, we'll have to preserve the values in the inactive
    // lanes. This is achieved by adding the original values to Phi nodes in the
    // tail block - but first we will have to split them into i32. Do this in
    // the entry block, before inserting the init.whole.wave intrinsic.
    SmallVector<Value *> remainingArgs;
    for (Argument &arg : drop_begin(func->args(), numShaderArg))
      remainingArgs.push_back(&arg);

    SmallVector<Value *> vgprArgs;
    compilerutils::splitIntoI32(func->getParent()->getDataLayout(), builder, remainingArgs, vgprArgs);

    exitInfos.push_back(CpsExitInfo(entryBlock, std::move(vgprArgs), /*containsInactiveVgprs=*/true));

    if (useDeadInsteadOfPoison())
      dead = builder.CreateIntrinsic(builder.getInt32Ty(), m_deadId, {});

    // Now we can finally insert the init.whole.wave intrinsic.
    auto *originalExec = builder.CreateIntrinsic(builder.getInt1Ty(), m_initWholeWaveId, {});
    builder.CreateCondBr(originalExec, shaderBlock, tailBlock);

    // Remove the unconditional branch inserted by splitBB().
    entryBlock->getTerminator()->eraseFromParent();
  }

  // Lower cps jumps.
  for (auto *jump : payload.jumps)
    lowerCpsJump(func, jump, tailBlock, exitInfos);

  // Lower returns.
  for (auto *ret : retInstrs) {
    auto *dummyI32 = PoisonValue::get(builder.getInt32Ty());
    exitInfos.push_back(CpsExitInfo(ret->getParent(), {builder.getInt32(0), dummyI32, dummyI32}));
    builder.SetInsertPoint(ret);
    builder.CreateBr(tailBlock);
    ret->eraseFromParent();
  }

  size_t vgprNum = 0;
  size_t activeVgprNum = 0; // Only relevant when using init.whole.wave.
  for (const auto &exit : exitInfos) {
    vgprNum = std::max(exit.vgpr.size(), vgprNum);

    // When using init.whole.wave, the exitInfo for the entry block will include the inactive VGPR args.
    // Skip that when determining the number of active VGPRs.
    if (handleInactiveVgprs && !exit.containsInactiveVgprs)
      activeVgprNum = std::max(exit.vgpr.size(), activeVgprNum);
  }

  SmallVector<Value *> newVgpr;
  // Put LocalInvocationId before {vcr, csp, shaderIndex}.
  if (haveLocalInvocationId)
    newVgpr.push_back(func->getArg(numUserdata));

  builder.SetInsertPoint(tailBlock);

  if (exitInfos.size() == 1) {
    newVgpr.append(exitInfos[0].vgpr);
  } else {
    for (size_t vgprIdx = 0; vgprIdx < vgprNum; vgprIdx++) {
      // We always have the leading three fixed vgpr arguments: csp, shaderIndex, vcr. The other remaining payloads are
      // i32 type.
      Type *phiTy = vgprIdx < 3 ? exitInfos[0].vgpr[vgprIdx]->getType() : builder.getInt32Ty();
      PHINode *phi = builder.CreatePHI(phiTy, exitInfos.size());
      for (size_t exitIdx = 0; exitIdx < exitInfos.size(); exitIdx++) {
        if (vgprIdx < exitInfos[exitIdx].vgpr.size())
          phi->addIncoming(exitInfos[exitIdx].vgpr[vgprIdx], exitInfos[exitIdx].pred);
        else
          phi->addIncoming(dead, exitInfos[exitIdx].pred);
      }
      newVgpr.push_back(phi);
    }
  }
  // Packing VGPR arguments.
  Value *vgprArg = mergeIntoStruct(builder, newVgpr);

  // Packing SGPR arguments (user data + internal used SGPRs) into vector of i32s.
  SmallVector<Value *> sgprArgs;
  for (unsigned idx = 0; idx != numUserdata; ++idx)
    sgprArgs.push_back(func->getArg(idx));

  //    tail:
  //      Merge vgpr values from different exits.
  //      Check if we have pending cps call
  //      If no cps call, jump to return block.
  //    chain:
  //      Jump to next cps function.
  //    ret:
  //      ret void
  unsigned waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage.value());
  Type *waveMaskTy = builder.getIntNTy(waveSize);
  // For continufy based continuation, the vgpr list: LocalInvocationId(optional), vcr, csp, ...
  unsigned vcrIndexInVgpr = haveLocalInvocationId ? 1 : 0;
  auto *vcr = builder.CreateExtractValue(vgprArg, vcrIndexInVgpr);
  auto *vcrTy = vcr->getType();
  Value *pendingBallot = nullptr;
  if (isCpsFunc) {
    auto *vcrShaderArg = func->getArg(numShaderArg);
    // When we are working with LLVM version without the llvm.amdgcn.set.inactive.chain.arg, we cannot simply declare
    // it and call it. LLVM will misrecognize it as llvm.amdgcn.set.inactive, and lit-test would just fail. So here we
    // just call llvm.amdgcn.set.inactive to pass compilation and lit-test if no *set.inactive.chain.arg support.
    // TODO: Cleanup this when the related LLVM versions have the intrinsic definition.
    if (!useIWW) {
      if (m_setInactiveChainArgId != Intrinsic::not_intrinsic)
        vcr = builder.CreateIntrinsic(vcrTy, m_setInactiveChainArgId, {vcr, vcrShaderArg});
      else
        vcr = builder.CreateIntrinsic(vcrTy, Intrinsic::amdgcn_set_inactive, {vcr, vcrShaderArg});
    }

    auto level = builder.CreateAnd(vcr, builder.getInt32(0x7));
    auto funcLevel = static_cast<unsigned>(cps::getCpsLevelFromFunction(*func));
    static const std::vector<CpsSchedulingLevel> priorities[] = {
        // RayGen: Continue with RayGen or hit shaders
        {CpsSchedulingLevel::Traversal, CpsSchedulingLevel::ClosestHit_Miss_Callable, CpsSchedulingLevel::RayGen},
        // ClosestHit_Miss_Callable: Continue with hit shaders, then resume RayGen
        {CpsSchedulingLevel::Traversal, CpsSchedulingLevel::RayGen, CpsSchedulingLevel::ClosestHit_Miss_Callable},
        // Traversal: Call Intersection or AnyHit, then call hit shaders or continue with RayGen
        // Traversal can continue with traversal when it wants to wait, so try that last
        {CpsSchedulingLevel::Traversal, CpsSchedulingLevel::RayGen, CpsSchedulingLevel::ClosestHit_Miss_Callable,
         CpsSchedulingLevel::AnyHit_CombinedIntersection_AnyHit, CpsSchedulingLevel::Intersection},
        // AnyHit_CombinedIntersection_AnyHit: Continue with AnyHit, then resume Traversal
        {CpsSchedulingLevel::Traversal, CpsSchedulingLevel::Intersection,
         CpsSchedulingLevel::AnyHit_CombinedIntersection_AnyHit},
        // Intersection: Continue with Intersection, then resume Traversal
        {CpsSchedulingLevel::Traversal, CpsSchedulingLevel::AnyHit_CombinedIntersection_AnyHit,
         CpsSchedulingLevel::Intersection}};
    // Get non-zero level execution Mask
    pendingBallot = takeLevel(level, builder, waveMaskTy, priorities[funcLevel - 1]);
  } else {
    // Find first lane having non-null vcr, and use as next jump target.
    auto *vcrMask = builder.CreateICmpNE(vcr, builder.getInt32(0));
    pendingBallot = builder.CreateIntrinsic(Intrinsic::amdgcn_ballot, waveMaskTy, vcrMask);
  }

  Value *firstActive = builder.CreateIntrinsic(Intrinsic::cttz, waveMaskTy, {pendingBallot, builder.getTrue()});
  if (!waveMaskTy->isIntegerTy(32))
    firstActive = builder.CreateTrunc(firstActive, builder.getInt32Ty());
  auto *targetVcr = builder.CreateIntrinsic(builder.getInt32Ty(), Intrinsic::amdgcn_readlane, {vcr, firstActive});
  // Calculate the lane mask that take this specific target.
  auto *targetMask = builder.CreateICmpEQ(vcr, targetVcr);
  auto *execMask = builder.CreateIntrinsic(Intrinsic::amdgcn_ballot, waveMaskTy, targetMask);

  if (isCpsFunc && !useIWW) {
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
  // Insert jumpTarget computations in the tailBlock, since that is closer to where they will be used.
  // These operations are expected to only use SGPRs, so it should be safe to run with or without all lanes
  // enabled (i.e. regardless of useIWW's value).
  AddressExtender addressExtender(func, tailBlock);
  Value *jumpTarget = addressExtender.extend(addr32, builder.getInt32(HighAddrPc), builder.getPtrTy(), builder);

  Value *numVgpr = nullptr;
  if (isDynamicVgprEnabled()) {
    // dVGPRs only support wave 32 mode.
    assert(waveSize == 32);
    // The required number of VGPR blocks minus 1 is stored in 3~5 bit of continuation reference.
    numVgpr = builder.CreateAnd(targetVcr, builder.getInt32(0x38u));
    // Each block means 16 VGPRs
    // numVgpr = (vcr[bit 3..5] >> 3 + 1) * 16 -> numVgpr = vcr[bit 3..5] << 1 + 16
    numVgpr = builder.CreateShl(numVgpr, 1);
    numVgpr = builder.CreateAdd(numVgpr, builder.getInt32(16));

    // Take the maximum number of VGPRs that may be live out of any shader in the pipeline into consideration.
    // The number is stored in the last SGPR argument.
    if (auto maxOutgoingVgprCount = cps::tryGetMaxOutgoingVgprCount(*func)) {
      // NOTE: If this metadata is set, it means that this is kernel entry and it will initialize the SGPR of max
      // outgoing VGPR count.
      assert(!isCpsFunc);
      sgprArgs.push_back(builder.getInt32(maxOutgoingVgprCount.value()));
    } else {
      // Max outgoing VGPR count is the last argument.
      assert(func->getArg(numShaderArg - 1)->getName() == "MaxOutgoingVgprCount");
      sgprArgs.push_back(func->getArg(numShaderArg - 1));
    }
    numVgpr = builder.CreateBinaryIntrinsic(Intrinsic::umax, numVgpr, sgprArgs.back());

    // Always pass %addr32, %execMask and %num_vgprs to fallback function using the last 3 SGPRs.
    sgprArgs.push_back(addr32);
    sgprArgs.push_back(execMask);
    sgprArgs.push_back(numVgpr);
  }

  const DataLayout &layout = func->getParent()->getDataLayout();
  SmallVector<Value *> sgprI32;
  compilerutils::splitIntoI32(layout, builder, sgprArgs, sgprI32);
  Value *sgprVec = mergeDwordsIntoVector(builder, sgprI32);

  SmallVector<Value *> chainArgs = {jumpTarget, execMask, sgprVec, vgprArg};

  if (isDynamicVgprEnabled()) {
    // Bit 0 of flags set to 1 means dVGPR mode enabled
    chainArgs.push_back(builder.getInt32(1));
    chainArgs.push_back(numVgpr);
    chainArgs.push_back(builder.getInt32(~0u)); // fallback_exec

    auto fallbackFunc = createRetryVgprAllocFunc(cast<FixedVectorType>(sgprVec->getType()));
    chainArgs.push_back(fallbackFunc);
  } else {
    // No flags
    chainArgs.push_back(builder.getInt32(0));
  }

  Type *chainTys[] = {builder.getPtrTy(), builder.getIntNTy(waveSize), sgprVec->getType(), vgprArg->getType()};
  auto *chainCall = builder.CreateIntrinsic(Intrinsic::amdgcn_cs_chain, chainTys, chainArgs);
  // Add inreg attribute for (fn, exec, sgprs).
  for (unsigned arg = 0; arg < 3; arg++)
    chainCall->addParamAttr(arg, Attribute::InReg);
  builder.CreateUnreachable();

  auto *doc = m_pipelineState->getPalMetadata()->getDocument();
  auto funcName = doc->getNode(func->getName(), /*copy=*/true);

  // Set per-function .frontend_stack_size PAL metadata.
  unsigned stackSize = ContHelper::tryGetStackSize(func).value_or(0);
  auto &shaderFunctions = m_pipelineState->getPalMetadata()
                              ->getPipelineNode()
                              .getMap(true)[Util::Abi::PipelineMetadataKey::ShaderFunctions]
                              .getMap(true);
  shaderFunctions[funcName].getMap(true)[Util::Abi::HardwareStageMetadataKey::FrontendStackSize] = stackSize;
  if (isDynamicVgprEnabled()) {
    // There are 8 VGPRs reserved for amdgpu_cs_chain call.
    unsigned outgoingVgprNum = handleInactiveVgprs ? activeVgprNum : vgprNum;
    shaderFunctions[funcName].getMap(true)[Util::Abi::HardwareStageMetadataKey::OutgoingVgprCount] =
        outgoingVgprNum + 8;
  }

  return true;
}

// =====================================================================================================================
// Create a function to do retry vgpr alloc
//
// @param sgprsTy : the types of SGPRs used for llvm.amdgcn.cs.chain call
Function *lgc::MutateEntryPoint::createRetryVgprAllocFunc(FixedVectorType *sgprsTy) {
  IRBuilder<> builder(*m_context);

  std::string funcName = "retry_vgpr_alloc.";
  funcName += getTypeName(sgprsTy);

  // If function already exists, just return it.
  if (auto func = m_module->getFunction(funcName))
    return func;

  auto funcTy = FunctionType::get(builder.getVoidTy(), {sgprsTy}, false);
  auto func = Function::Create(funcTy, GlobalValue::ExternalLinkage, funcName, m_module);
  func->addParamAttr(0, Attribute::InReg);
  func->setCallingConv(CallingConv::AMDGPU_CS_ChainPreserve);
  auto bb = BasicBlock::Create(func->getContext(), "", func);
  builder.SetInsertPoint(bb);

  auto sgprs = func->getArg(0);
  auto sgprCount = sgprsTy->getNumElements();
  // NOTE: %addr32, %execMask and %num_vgprs are always placed at the last of SGPRs.
  auto addr32 = builder.CreateExtractElement(sgprs, sgprCount - 3);
  auto execMask = builder.CreateExtractElement(sgprs, sgprCount - 2);
  auto numVgprs = builder.CreateExtractElement(sgprs, sgprCount - 1);

  AddressExtender addressExtender(func);
  Value *jumpTarget = addressExtender.extend(addr32, builder.getInt32(HighAddrPc), builder.getPtrTy(), builder);

  // The retry function uses amdgpu_cs_chain_preserve calling convention, no VGPRs passing is required
  Value *vgprs = PoisonValue::get(StructType::get(*m_context));

  Value *chainArgs[] = {jumpTarget, execMask, sgprs, vgprs, builder.getInt32(1), numVgprs, builder.getInt32(~0u), func};

  // Sleep a little so as not to overwhelm the instruction fetch
  // TODO: Experiment and pick ideal sleep time on real hardware.
  constexpr unsigned RetrySleepCount = 2;
  builder.CreateIntrinsic(Intrinsic::amdgcn_s_sleep, {}, builder.getInt32(RetrySleepCount));

  // TODO: Release extraneous VGPRs on failure so that other waves have a higher chance of making progress (may be done
  // in LLVM)

  Type *chainTys[] = {builder.getPtrTy(), builder.getInt32Ty(), sgprs->getType(), vgprs->getType()};
  auto *chainCall = builder.CreateIntrinsic(Intrinsic::amdgcn_cs_chain, chainTys, chainArgs);
  // Add inreg attribute for (fn, exec, sgprs).
  for (unsigned arg = 0; arg < 3; arg++)
    chainCall->addParamAttr(arg, Attribute::InReg);

  builder.CreateUnreachable();

  return func;
}

// =====================================================================================================================
// Mutate the argument list of the cps function
//
// Mutate the function type from:
// void @func(args...)
// into:
// amdgpu_cs_chain void @func(fixed_shader_args, i32 %vcr, i32 %csp, args...)
//
// @param func : the cps function to be mutated
// @param fixedShaderArgTys : the types of the fixed shader arguments(userdata + possibly shader inputs)
// @param argNames : the name string of the fixed shader arguments
Function *MutateEntryPoint::lowerCpsFunction(Function *func, ArrayRef<Type *> fixedShaderArgTys,
                                             ArrayRef<std::string> argNames) {
  IRBuilder<> builder(func->getContext());
  AttributeList oldAttrs = func->getAttributes();

  SmallVector<Type *> newArgTys;
  newArgTys.append(fixedShaderArgTys.begin(), fixedShaderArgTys.end());
  newArgTys.push_back(builder.getInt32Ty());
  auto remainingArgs = func->getFunctionType()->params();
  newArgTys.append(remainingArgs.begin(), remainingArgs.end());

  // If init.whole.wave is available, we need to pad the argument list up to the maximum number of VGPRs used for this
  // pipeline, so that we can preserve the inactive lanes for these VGPRs.
  int numInactiveVgprs = 0;
  bool useIWW = useInitWholeWave();
  if (useIWW) {
    SmallVector<Type *> remainingVgprArgs;
    for (unsigned idx = 0; idx < remainingArgs.size(); ++idx)
      if (!oldAttrs.getParamAttrs(idx).hasAttribute(Attribute::InReg))
        remainingVgprArgs.push_back(remainingArgs[idx]);

    const DataLayout &layout = func->getParent()->getDataLayout();
    std::optional<unsigned> argBound = lgc::cps::getMaxArgumentVgprs(*func->getParent());
    if (!argBound.has_value())
      report_fatal_error("Missing lgc.cps.maxArgumentVgprs metadata");

    numInactiveVgprs = *argBound - lgc::cps::getArgumentDwordCount(layout, remainingVgprArgs);

    if (numInactiveVgprs < 0)
      report_fatal_error("Invalid number of inactive VGPRs, check lgc.cps.maxArgumentVgprs");

    for (int i = 0; i < numInactiveVgprs; ++i)
      newArgTys.push_back(builder.getInt32Ty());
  }

  FunctionType *newFuncTy = FunctionType::get(builder.getVoidTy(), newArgTys, false);
  auto newFunc = createFunctionHelper(newFuncTy, func->getLinkage(), func->getParent());
  newFunc->copyAttributesFrom(func);
  newFunc->copyMetadata(func, 0);
  newFunc->takeName(func);
  // Always insert the new function after the old function
  func->getParent()->getFunctionList().insertAfter(func->getIterator(), newFunc);

  // Setup the argument attributes
  AttributeSet emptyAttrSet;
  AttributeSet inRegAttrSet = emptyAttrSet.addAttribute(func->getContext(), Attribute::InReg);

  bool haveLocalInvocationId = !m_pipelineState->getShaderModes()->getComputeShaderMode().noLocalInvocationIdInCalls;
  assert(haveLocalInvocationId == (argNames.back() == "LocalInvocationId") ||
         (argNames[argNames.size() - 2] == "LocalInvocationId"));

  SmallVector<AttributeSet, 8> argAttrs;
  unsigned numUserdataArg = haveLocalInvocationId ? fixedShaderArgTys.size() - 1 : fixedShaderArgTys.size();
  if (isDynamicVgprEnabled())
    numUserdataArg--;

  for (unsigned idx = 0; idx != numUserdataArg; ++idx)
    argAttrs.push_back(inRegAttrSet);

  // %LocalInvocationId when required
  if (haveLocalInvocationId)
    argAttrs.push_back(emptyAttrSet);

  if (isDynamicVgprEnabled())
    argAttrs.push_back(inRegAttrSet);

  // %vcr attribute
  argAttrs.push_back(emptyAttrSet);
  // %csp attribute
  argAttrs.push_back(emptyAttrSet);
  for (unsigned idx = 0; idx != func->getFunctionType()->getNumParams(); ++idx)
    argAttrs.push_back(oldAttrs.getParamAttrs(idx));
  newFunc->setAttributes(
      AttributeList::get(func->getContext(), oldAttrs.getFnAttrs(), oldAttrs.getRetAttrs(), argAttrs));

  // Move all the basic blocks from the original function into the new one.
  newFunc->splice(newFunc->begin(), func);

  builder.SetInsertPointPastAllocas(newFunc);

  // Set name string for arguments.
  SmallVector<std::string> newArgNames(argNames);
  newArgNames.push_back("vcr");
  for (unsigned idx = 0; idx < newArgNames.size(); idx++)
    newFunc->getArg(idx)->setName(newArgNames[idx]);

  // Replace old arguments with new ones.
  unsigned argOffsetInNew = fixedShaderArgTys.size() + 1;
  for (unsigned idx = 0; idx < func->arg_size(); idx++) {
    Value *oldArg = func->getArg(idx);
    Value *newArg = newFunc->getArg(idx + argOffsetInNew);
    newArg->setName(oldArg->getName());
    oldArg->replaceAllUsesWith(newArg);
  }

  if (useIWW) {
    for (unsigned idx = newFunc->arg_size() - numInactiveVgprs; idx < newFunc->arg_size(); idx++)
      newFunc->getArg(idx)->setName("inactive.vgpr");
  }

  setShaderStage(newFunc, getShaderStage(func));
  newFunc->setAlignment(Align(128));
  newFunc->setCallingConv(CallingConv::AMDGPU_CS_Chain);
  return newFunc;
}

// =====================================================================================================================
// Take the level from priorities list

// @param level : the level to select
// @param builder: IRBuilder to build instructions
// @param waveMaskTy : Wave Mask type
// @param priorities : Priorities list
Value *MutateEntryPoint::takeLevel(Value *level, IRBuilder<> &builder, Type *waveMaskTy,
                                   ArrayRef<CpsSchedulingLevel> priorities) {
  auto levelMask = builder.CreateICmpNE(level, builder.getInt32(0));
  Value *levelBallot = builder.CreateIntrinsic(Intrinsic::amdgcn_ballot, waveMaskTy, levelMask);
  Value *cond = nullptr;

  for (auto cpsLevel : priorities) {
    auto lvMask = builder.CreateICmpEQ(level, builder.getInt32(static_cast<unsigned>(cpsLevel)));
    Value *lvBallot = builder.CreateIntrinsic(Intrinsic::amdgcn_ballot, waveMaskTy, lvMask);
    cond = builder.CreateICmpNE(lvBallot, builder.getInt32(0));
    levelBallot = builder.CreateSelect(cond, lvBallot, levelBallot);
  }
  return levelBallot;
}

// =====================================================================================================================
// Lower cps.jump, fill cps exit information and branch to tailBlock.
// This assume the arguments of the parent function are setup correctly.
//
// @param parent : the parent function of the cps.jump operation
// @param jumpOp : the call instruction of cps.jump
// @param [in/out] exitInfos : the vector of cps exit information to be filled
void MutateEntryPoint::lowerCpsJump(Function *parent, cps::JumpOp *jumpOp, BasicBlock *tailBlock,
                                    SmallVectorImpl<CpsExitInfo> &exitInfos) {
  IRBuilder<> builder(parent->getContext());
  const DataLayout &layout = parent->getParent()->getDataLayout();
  // Translate @lgc.cps.jump(CR %target, i32 %levels, i32 %csp, ...) into:
  // @llvm.amdgcn.cs.chain(ptr %fn, i{32,64} %exec, T %sgprs, U %vgprs, i32 immarg %flags, ...)
  builder.SetInsertPoint(jumpOp);

  // Add extra args specific to the target function.
  SmallVector<Value *> remainingArgs{jumpOp->getTail()};

  // Packing VGPR arguments {vcr, csp, shaderRecIdx, rcr, args...}
  SmallVector<Value *> vgprArgs;
  vgprArgs.push_back(jumpOp->getTarget());
  vgprArgs.push_back(jumpOp->getCsp());
  vgprArgs.push_back(jumpOp->getShaderIndex());
  vgprArgs.push_back(jumpOp->getRcr());
  compilerutils::splitIntoI32(layout, builder, remainingArgs, vgprArgs);

  // Fill exit information.
  exitInfos.push_back(CpsExitInfo(jumpOp->getParent(), std::move(vgprArgs)));
  // Branch to tailBlock.
  auto *oldTerm = jumpOp->getParent()->getTerminator();
  assert(isa<UnreachableInst>(oldTerm));
  oldTerm->eraseFromParent();
  builder.CreateBr(tailBlock);

  jumpOp->eraseFromParent();
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
void MutateEntryPoint::setupComputeWithCalls(Module *module) {
  m_computeWithCalls = false;

  if (m_pipelineState->isComputeLibrary()) {
    m_computeWithCalls = true;
    return;
  }

  // We have a compute pipeline. Check whether there are any non-shader-entry-point functions (other than lgc.*
  // functions and intrinsics).
  for (Function &func : *module) {
    if (func.isDeclaration() && func.getIntrinsicID() == Intrinsic::not_intrinsic &&
        !func.getName().starts_with(lgcName::InternalCallPrefix) && !func.user_empty()) {
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
void MutateEntryPoint::gatherUserDataUsage(Module *module) {
  // Gather special ops requiring user data.
  static const auto visitor =
      llvm_dialects::VisitorBuilder<MutateEntryPoint>()
          .add<UserDataOp>([](MutateEntryPoint &self, UserDataOp &op) {
            auto stage = getShaderStage(op.getFunction());
            assert(stage != ShaderStage::CopyShader);
            auto userDataUsage = self.getUserDataUsage(stage.value());
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
              self.m_pipelineState->getPalMetadata()->setUserDataSpillUsage(op.getOffset() / 4, stage);
            }
          })
          .add<LoadUserDataOp>([](MutateEntryPoint &self, LoadUserDataOp &op) {
            auto stage = getShaderStage(op.getFunction());
            assert(stage != ShaderStage::CopyShader);
            auto *userDataUsage = self.getUserDataUsage(stage.value());

            UserDataLoad load;
            load.load = &op;
            load.dwordOffset = op.getOffset() / 4;
            load.dwordSize = self.m_module->getDataLayout().getTypeStoreSize(op.getType()) / 4;

            userDataUsage->loads.push_back(load);
            userDataUsage->addLoad(load.dwordOffset, load.dwordSize);
          })
          .add<WriteXfbOutputOp>([](MutateEntryPoint &self, WriteXfbOutputOp &op) {
            auto lastVertexStage = self.m_pipelineState->getLastVertexProcessingStage();
            lastVertexStage = lastVertexStage == ShaderStage::CopyShader ? ShaderStage::Geometry : lastVertexStage;
            self.getUserDataUsage(lastVertexStage.value())->usesStreamOutTable = true;
          })
          .build();

  visitor.visit(*this, *module);

  for (Function &func : *module) {
    if (!func.isDeclaration())
      continue;

    if (func.getName().starts_with(lgcName::SpecialUserData)) {
      for (User *user : func.users()) {
        CallInst *call = cast<CallInst>(user);
        auto stage = getShaderStage(call->getFunction());
        assert(stage != ShaderStage::CopyShader);
        auto &specialUserData = getUserDataUsage(stage.value())->specialUserData;
        unsigned index = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
        specialUserData[index].users.push_back(call);
      }
    }
  }

  if (m_pipelineState->enableSwXfb()) {
    // NOTE: For GFX11+, SW emulated stream-out will always use stream-out buffer descriptors and stream-out buffer
    // offsets to calculate numbers of written primitives/dwords and update the counters.
    auto lastVertexStage = m_pipelineState->getLastVertexProcessingStage();
    lastVertexStage = lastVertexStage == ShaderStage::CopyShader ? ShaderStage::Geometry : lastVertexStage;
    getUserDataUsage(lastVertexStage.value())->usesStreamOutTable = true;
  }
}

// =====================================================================================================================
// Load a value of a simple type from user data at the given dwordOffset.
Value *MutateEntryPoint::loadUserData(const UserDataUsage &userDataUsage, Value *spillTable, Type *type,
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
void MutateEntryPoint::fixupUserDataUses(Module &module) {
  BuilderBase builder(module.getContext());

  // For each function definition...
  for (Function &func : module) {
    if (func.isDeclaration())
      continue;

    auto stage = getShaderStage(&func);

    if (!stage)
      continue;

    auto userDataUsage = getUserDataUsage(stage.value());

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
    for (auto &it : userDataUsage->specialUserData) {
      SpecialUserDataNodeUsage &specialUserData = it.second;
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
void MutateEntryPoint::processShader(ShaderInputs *shaderInputs) {
  // Create new entry-point from the original one
  SmallVector<Type *, 8> argTys;
  SmallVector<std::string, 8> argNames;
  uint64_t inRegMask = generateEntryPointArgTys(shaderInputs, nullptr, argTys, argNames, 0, true);

  Function *origEntryPoint = m_entryPoint;

  // Create the new function and transfer code and attributes to it.
  Function *entryPoint =
      addFunctionArgs(origEntryPoint, origEntryPoint->getFunctionType()->getReturnType(), argTys, argNames, inRegMask);

  // We always deal with pre-merge functions here, so set the fitting pre-merge calling conventions.
  switch (m_shaderStage.value()) {
  case ShaderStage::Task:
    entryPoint->setCallingConv(CallingConv::AMDGPU_CS);
    break;
  case ShaderStage::Mesh:
    entryPoint->setCallingConv(CallingConv::AMDGPU_GS);
    break;
  case ShaderStage::Vertex:
    if (m_pipelineState->hasShaderStage(ShaderStage::TessControl))
      entryPoint->setCallingConv(CallingConv::AMDGPU_LS);
    else if (m_pipelineState->hasShaderStage(ShaderStage::Geometry))
      entryPoint->setCallingConv(CallingConv::AMDGPU_ES);
    else
      entryPoint->setCallingConv(CallingConv::AMDGPU_VS);
    break;
  case ShaderStage::TessControl:
    entryPoint->setCallingConv(CallingConv::AMDGPU_HS);
    break;
  case ShaderStage::TessEval:
    if (m_pipelineState->hasShaderStage(ShaderStage::Geometry))
      entryPoint->setCallingConv(CallingConv::AMDGPU_ES);
    else
      entryPoint->setCallingConv(CallingConv::AMDGPU_VS);
    break;
  case ShaderStage::Geometry:
    entryPoint->setCallingConv(CallingConv::AMDGPU_GS);
    break;
  case ShaderStage::Fragment:
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
void MutateEntryPoint::processComputeFuncs(ShaderInputs *shaderInputs, Module &module) {
  m_shaderStage = ShaderStage::Compute;

  // We no longer support compute shader fixed layout required before PAL interface version 624.
  if (m_pipelineState->getLgcContext()->getPalAbiVersion() < 624)
    report_fatal_error("Compute shader not supported before PAL version 624");

  // Process each function definition.
  SmallVector<Function *, 4> origFuncs;
  for (Function &func : module) {
    if (func.isDeclaration()) {
      if (!func.isIntrinsic() && !func.getName().starts_with(lgcName::InternalCallPrefix)) {
        // This is the declaration of a callable function that is defined in a different module.
        func.setCallingConv(CallingConv::AMDGPU_Gfx);
      }
    } else {
      origFuncs.push_back(&func);
    }
  }

  SmallVector<Type *, 20> shaderInputTys;
  SmallVector<std::string, 20> shaderInputNames;
  ArrayRef<Type *> calleeArgTys;
  ArrayRef<std::string> calleeArgNames;
  uint64_t inRegMask;

  auto StackAddrspaceMD = ContHelper::tryGetStackAddrspace(module);
  auto StackAddrspace = StackAddrspaceMD.value_or(ContStackAddrspace::ScratchLLPC);
  m_cpsStackAddrspace = static_cast<unsigned>(StackAddrspace);

  for (Function *origFunc : origFuncs) {
    auto *origType = origFunc->getFunctionType();

    // Create the new function and transfer code and attributes to it.
    Function *newFunc = nullptr;
    // For continufy based ray-tracing, we still need to add shader inputs like workgroupId and LocalInvocationId.
    // TODO: All codes related to noLocalInvocationIdInCalls should be removed once we don't pass LocalInvocationId in
    // legacy/continufy RT any more.
    bool haveLocalInvocationIdInCalls =
        !m_pipelineState->getShaderModes()->getComputeShaderMode().noLocalInvocationIdInCalls;
    if (cps::isCpsFunction(*origFunc)) {
      assert(origType->getReturnType()->isVoidTy());
      if (!m_cpsShaderInputCache.isAvailable()) {
        generateEntryPointArgTys(shaderInputs, nullptr, shaderInputTys, shaderInputNames, 0, false);
        assert(shaderInputNames.back() == "LocalInvocationId");
        if (!haveLocalInvocationIdInCalls) {
          shaderInputTys.pop_back();
          shaderInputNames.pop_back();
        }

        if (isDynamicVgprEnabled()) {
          // Add MaxOutgoingVgprCount as the last argument.
          // NOTE: Not doing this in generateEntryPointArgTys() as `MaxOutgoingVgprCount` is not essentially a userdata,
          // and it only exists in CPS functions.
          shaderInputTys.push_back(Type::getInt32Ty(module.getContext()));
          shaderInputNames.push_back("MaxOutgoingVgprCount");
        }

        m_cpsShaderInputCache.set(shaderInputTys, shaderInputNames);
      }
      newFunc = lowerCpsFunction(origFunc, m_cpsShaderInputCache.getTypes(), m_cpsShaderInputCache.getNames());
    } else {
      if (shaderInputTys.empty()) {
        inRegMask = generateEntryPointArgTys(shaderInputs, origFunc, shaderInputTys, shaderInputNames,
                                             origType->getNumParams(), true);
        calleeArgTys = ArrayRef(shaderInputTys);
        calleeArgNames = ArrayRef(shaderInputNames);
        const bool isEntryPoint = isShaderEntryPoint(origFunc);
        if (!isEntryPoint && m_pipelineState->getShaderModes()->getComputeShaderMode().noLocalInvocationIdInCalls) {
          assert(calleeArgNames.back() == "LocalInvocationId");
          calleeArgTys = calleeArgTys.drop_back();
          calleeArgNames = calleeArgNames.drop_back();
        }
      }

      const bool isEntryPoint = isShaderEntryPoint(origFunc);
      newFunc =
          addFunctionArgs(origFunc, origType->getReturnType(), isEntryPoint ? ArrayRef(shaderInputTys) : calleeArgTys,
                          isEntryPoint ? ArrayRef(shaderInputNames) : calleeArgNames, inRegMask, AddFunctionArgsAppend);
      newFunc->setCallingConv(isEntryPoint ? CallingConv::AMDGPU_CS : CallingConv::AMDGPU_Gfx);
    }
    // Set Attributes on new function.
    setFuncAttrs(newFunc);

    origFunc->replaceAllUsesWith(newFunc);
    // Remove original function.
    origFunc->eraseFromParent();

    if (lowerCpsOps(newFunc, shaderInputs))
      continue;

    int argOffset = origType->getNumParams();
    if (isComputeWithCalls())
      processCalls(*newFunc, calleeArgTys, calleeArgNames, inRegMask, argOffset);
  }
}
// =====================================================================================================================
// Process all real function calls and passes arguments to them.
//
// @param [in/out] module : Module
void MutateEntryPoint::processCalls(Function &func, ArrayRef<Type *> shaderInputTys,
                                    ArrayRef<std::string> shaderInputNames, uint64_t inRegMask, unsigned argOffset) {
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
        if (calledFunc->isIntrinsic() || calledFunc->getName().starts_with(lgcName::InternalCallPrefix))
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
      CallInst *newCall = builder.CreateCall(calledTy, calledVal, args);
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
void MutateEntryPoint::setFuncAttrs(Function *entryPoint) {
  AttrBuilder builder(entryPoint->getContext());
  if (m_shaderStage == ShaderStage::Fragment) {
    auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment)->builtInUsage.fs;
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
    spiPsInputAddr.bits.ancillaryEna |= builtInUsage.primType;
    spiPsInputAddr.bits.lineStippleTexEna |= builtInUsage.lineStipple;
    spiPsInputAddr.bits.sampleCoverageEna = builtInUsage.sampleMaskIn;

    builder.addAttribute("InitialPSInputAddr", std::to_string(spiPsInputAddr.u32All));

    bool hasDepthExport = builtInUsage.sampleMask || builtInUsage.fragStencilRef || builtInUsage.fragDepth;
    builder.addAttribute("amdgpu-depth-export", hasDepthExport ? "1" : "0");

    bool hasColorExport = false;
    // SpiShaderColFormat / mmSPI_SHADER_COL_FORMAT is used for fully compiled shaders
    unsigned colFormat = EXP_FORMAT_ZERO;
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
  auto shaderOptions = &m_pipelineState->getShaderOptions(m_shaderStage.value());
  auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage.value());

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
    unsigned tgSize;
    if (m_shaderStage == ShaderStage::Compute || m_shaderStage == ShaderStage::Task) {
      const auto &mode = m_pipelineState->getShaderModes()->getComputeShaderMode();
      tgSize = std::max(1u, mode.workgroupSizeX * mode.workgroupSizeY * mode.workgroupSizeZ);
    } else if (m_shaderStage == ShaderStage::Mesh) {
      const auto &mode = m_pipelineState->getShaderModes()->getMeshShaderMode();
      tgSize = std::max(1u, mode.workgroupSizeX * mode.workgroupSizeY * mode.workgroupSizeZ);
    } else {
      // Graphics shader stages don't have thread groups at an API level
      tgSize = 1;
    }
    unsigned numWavesPerTg = divideCeil(tgSize, m_pipelineState->getShaderWaveSize(m_shaderStage.value()));
    unsigned maxWavesPerCu = numWavesPerTg * shaderOptions->maxThreadGroupsPerComputeUnit;
    unsigned maxWavesPerSimd = divideCeil(maxWavesPerCu, 2);
    std::string wavesPerEu = std::string("1,") + std::to_string(maxWavesPerSimd);
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
    if (m_shaderStage == ShaderStage::Fragment || m_shaderStage == ShaderStage::Compute)
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

  if (shaderOptions->promoteAllocaRegLimit != 0)
    builder.addAttribute("amdgpu-promote-alloca-to-vector-max-regs",
                         std::to_string(shaderOptions->promoteAllocaRegLimit));
  if (shaderOptions->promoteAllocaRegRatio != 0)
    builder.addAttribute("amdgpu-promote-alloca-to-vector-vgpr-ratio",
                         std::to_string(shaderOptions->promoteAllocaRegRatio));

  entryPoint->addFnAttrs(builder);

  // NOTE: Remove "readnone" attribute for entry-point. If GS is empty, this attribute will allow
  // LLVM optimization to remove sendmsg(GS_DONE). It is unexpected.
  entryPoint->setMemoryEffects(MemoryEffects::unknown());
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
uint64_t MutateEntryPoint::generateEntryPointArgTys(ShaderInputs *shaderInputs, Function *origFunc,
                                                    SmallVectorImpl<Type *> &argTys,
                                                    SmallVectorImpl<std::string> &argNames, unsigned argOffset,
                                                    bool updateUserDataMap) {

  uint64_t inRegMask = 0;
  IRBuilder<> builder(*m_context);
  auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage.value());
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
      assert((!isUnlinkedDescriptorSetValue(userDataArg.userDataValue) || dwordSize == 1) &&
             "Expecting descriptor set values to be one dword.  The linker cannot handle anything else.");
      if (isSystemUserData) {
        auto &specialUserData = getUserDataUsage(m_shaderStage.value())->specialUserData;
        specialUserData[userDataArg.userDataValue].entryArgIdx = argTys.size() + argOffset;
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
    if (m_pipelineState->getShaderWaveSize(m_shaderStage.value()) == 32 &&
        (m_shaderStage == ShaderStage::Compute || m_shaderStage == ShaderStage::Fragment ||
         m_shaderStage == ShaderStage::Mesh)) {
      unsigned userDataLimit = m_shaderStage == ShaderStage::Mesh ? 8 : 16;

      while (userDataIdx < userDataLimit) {
        argTys.push_back(builder.getInt32Ty());
        argNames.push_back(("dummyInit" + Twine(userDataIdx)).str());
        userDataIdx += 1;
      }
    }
  }

  intfData->userDataCount = userDataIdx;
  inRegMask = (1ull << argTys.size()) - 1;

  // Push the fixed system (not user data) register args.
  if (shaderInputs)
    inRegMask |= shaderInputs->getShaderArgTys(m_pipelineState, m_shaderStage.value(), origFunc, m_computeWithCalls,
                                               argTys, argNames, argOffset);

  if (updateUserDataMap) {
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
    m_pipelineState->setUserDataMap(m_shaderStage.value(), userDataMap);
  }

  return inRegMask;
}

// =====================================================================================================================
// @param userDataValue : The value to be written into a user data entry.
// @returns : True if the user data value corresponds to a special system user data value.
bool MutateEntryPoint::isSystemUserDataValue(unsigned userDataValue) const {
  if (userDataValue < static_cast<unsigned>(UserDataMapping::GlobalTable)) {
    return false;
  }
  return userDataValue < static_cast<unsigned>(UserDataMapping::DescriptorSet0);
}

// =====================================================================================================================
// @param userDataValue : The value to be written into a user data entry.
// @returns : True if the user data value corresponds to an unlinked descriptor set.
bool MutateEntryPoint::isUnlinkedDescriptorSetValue(unsigned userDataValue) const {
  if (userDataValue < static_cast<unsigned>(UserDataMapping::DescriptorSet0)) {
    return false;
  }
  return userDataValue <= static_cast<unsigned>(UserDataMapping::DescriptorSetMax);
}

// =====================================================================================================================
// Map from PipelineLinkKind to UserDataMapping
static UserDataMapping toUserDataMapping(PipelineLinkKind kind) {
  return UserDataMapping(unsigned(UserDataMapping::PipelineLinkStart) + unsigned(kind));
}

// =====================================================================================================================
// Add a UserDataArg to the appropriate vector for each special argument (e.g. ViewId) needed in user data SGPRs.
// In here, we need to check whether an argument is needed in two ways:
// 1. Whether a flag is set saying it will be needed after MutateEntryPoint
// 2. Whether there is an actual use of the special user data value (lgc.special.user.data call) generated
//    before MutateEntryPoint, which we check with userDataUsage->isSpecialUserDataUsed().
//
// @param userDataArgs : Vector to add args to when they need to go before user data nodes (just streamout)
// @param specialUserDataArgs : Vector to add args to when they need to go after user data nodes (all the rest)
// @param builder : IRBuilder to get types from
void MutateEntryPoint::addSpecialUserDataArgs(SmallVectorImpl<UserDataArg> &userDataArgs,
                                              SmallVectorImpl<UserDataArg> &specialUserDataArgs, IRBuilder<> &builder) {

  auto userDataUsage = getUserDataUsage(m_shaderStage.value());
  auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage.value());
  auto &entryArgIdxs = intfData->entryArgIdxs;
  bool enableNgg = m_pipelineState->isGraphics() ? m_pipelineState->getNggControl()->enableNgg : false;

  if (m_shaderStage == ShaderStage::Vertex || m_shaderStage == ShaderStage::TessControl ||
      m_shaderStage == ShaderStage::TessEval || m_shaderStage == ShaderStage::Geometry) {
    // Shader stage in the vertex-processing half of a graphics pipeline.
    // We need to ensure that the layout is the same between two shader stages that will be merged on GFX9+,
    // that is, VS-TCS, VS-GS (if no tessellation), TES-GS.

    // NOTE: The user data to emulate gl_ViewIndex is somewhat common. To make it consistent for GFX9
    // merged shader, we place it prior to any other special user data.
    if (m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable) {
      unsigned *argIdx = nullptr;
      auto userDataValue = UserDataMapping::ViewId;
      switch (m_shaderStage.value()) {
      case ShaderStage::Vertex:
        argIdx = &entryArgIdxs.vs.viewId;
        break;
      case ShaderStage::TessControl:
        argIdx = &entryArgIdxs.tcs.viewId;
        break;
      case ShaderStage::TessEval:
        argIdx = &entryArgIdxs.tes.viewId;
        break;
      case ShaderStage::Geometry:
        argIdx = &entryArgIdxs.gs.viewId;
        break;
      default:
        llvm_unreachable("Unexpected shader stage");
      }
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "viewId", userDataValue, argIdx));
    }

    if (getMergedShaderStage(m_shaderStage.value()) == getMergedShaderStage(ShaderStage::Vertex)) {
      // This is the VS, or the shader that VS is merged into on GFX9+.
      auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStage::Vertex);
      auto vsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Vertex);

      // Vertex buffer table.
      if (userDataUsage->isSpecialUserDataUsed(UserDataMapping::VertexBufferTable)) {
        specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "vertexBufferTable",
                                                  UserDataMapping::VertexBufferTable,
                                                  &vsIntfData->entryArgIdxs.vs.vbTablePtr));
      }

      // Base vertex and base instance.
      if (vsResUsage->builtInUsage.vs.baseVertex || vsResUsage->builtInUsage.vs.baseInstance ||
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

      // Vertex fetch table (uber fetch).
      if (userDataUsage->isSpecialUserDataUsed(toUserDataMapping(PipelineLinkKind::VertexFetchTable))) {
        specialUserDataArgs.push_back(
            UserDataArg(builder.getInt32Ty(), "uberFetchTable", toUserDataMapping(PipelineLinkKind::VertexFetchTable)));
      }
    }

    if ((m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 11) && !m_hasGs && !m_hasTs &&
        m_pipelineState->enableXfb() &&
        (m_pipelineState->getOptions().dynamicTopology || m_pipelineState->isUnlinked())) {
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "compositeData", UserDataMapping::CompositeData,
                                                &intfData->entryArgIdxs.vs.compositeData));
    }
  } else if (m_shaderStage == ShaderStage::Compute) {
    // Pass the gl_NumWorkgroups pointer in user data registers.
    // Always enable this, even if unused, if compute library is in use.
    // Unlike all the special user data values above, which go after the user data node args, this goes before.
    // That is to ensure that, with a compute pipeline using a library, library code knows where to find it
    // even if it thinks that the user data layout is a prefix of what the pipeline thinks it is.
    if (isComputeWithCalls() || userDataUsage->isSpecialUserDataUsed(UserDataMapping::Workgroup)) {
      auto numWorkgroupsPtrTy = PointerType::get(FixedVectorType::get(builder.getInt32Ty(), 3), ADDR_SPACE_CONST);
      userDataArgs.push_back(UserDataArg(numWorkgroupsPtrTy, "numWorkgroupsPtr", UserDataMapping::Workgroup, nullptr));
    }
  } else if (m_shaderStage == ShaderStage::Task) {
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
  } else if (m_shaderStage == ShaderStage::Mesh) {
    if (m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->builtInUsage.mesh.drawIndex) {
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
    if (m_pipelineState->needSwMeshPipelineStats()) {
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "meshPipeStatsBuf",
                                                UserDataMapping::MeshPipeStatsBuf,
                                                &intfData->entryArgIdxs.mesh.pipeStatsBuf));
    }
    {
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "meshTaskRingIndex",
                                                UserDataMapping::MeshTaskRingIndex,
                                                &intfData->entryArgIdxs.mesh.baseRingEntryIndex));
    }
  } else if (m_shaderStage == ShaderStage::Fragment) {
    if (m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable &&
        m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment)->builtInUsage.fs.viewIndex) {
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

    bool useDynamicSampleInfo =
        (m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment)->builtInUsage.fs.runAtSampleRate ||
         m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment)->builtInUsage.fs.samplePosOffset) &&
        (m_pipelineState->isUnlinked() || m_pipelineState->getRasterizerState().dynamicSampleInfo);
    if (userDataUsage->isSpecialUserDataUsed(UserDataMapping::CompositeData) || useDynamicSampleInfo) {
      specialUserDataArgs.push_back(UserDataArg(builder.getInt32Ty(), "compositeData", UserDataMapping::CompositeData,
                                                &intfData->entryArgIdxs.fs.compositeData));
    }
  }

  // Allocate register for stream-out buffer table, to go before the user data node args (unlike all the ones
  // above, which go after the user data node args).
  if (userDataUsage->usesStreamOutTable || userDataUsage->isSpecialUserDataUsed(UserDataMapping::StreamOutTable)) {
    if (enableNgg || !m_pipelineState->hasShaderStage(ShaderStage::CopyShader) && m_pipelineState->enableXfb()) {
      // If no NGG, stream out table will be set to copy shader's user data entry, we should not set it duplicately.
      unsigned *tablePtr = nullptr;

      switch (m_shaderStage.value()) {
      case ShaderStage::Vertex:
        tablePtr = &intfData->entryArgIdxs.vs.streamOutData.tablePtr;
        break;
      case ShaderStage::TessEval:
        tablePtr = &intfData->entryArgIdxs.tes.streamOutData.tablePtr;
        break;
      case ShaderStage::Geometry:
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

  if (m_pipelineState->enableSwXfb() ||
      (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 12 && m_pipelineState->enablePrimStats())) {
    // NOTE: For GFX11+, the SW stream-out needs an additional special user data SGPR to store the stream-out control
    // buffer address. And for GFX12+, we still need this special user data SGPR when we enable primitive statistics
    // counting. This is because primitive counters in GDS are removed and are replaced by those defined in stream-out
    // control buffer.
    unsigned *controlBufPtr = nullptr;

    switch (m_shaderStage.value()) {
    case ShaderStage::Vertex:
      controlBufPtr = &intfData->entryArgIdxs.vs.streamOutData.controlBufPtr;
      break;
    case ShaderStage::TessEval:
      controlBufPtr = &intfData->entryArgIdxs.tes.streamOutData.controlBufPtr;
      break;
    case ShaderStage::Geometry:
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
void MutateEntryPoint::finalizeUserDataArgs(SmallVectorImpl<UserDataArg> &userDataArgs,
                                            ArrayRef<UserDataArg> specialUserDataArgs, IRBuilder<> &builder) {
  auto userDataUsage = getUserDataUsage(m_shaderStage.value());

  // In compute-with-calls, we need to ensure that the compute shader and library code agree that s15 is the spill
  // table pointer, even if it is not needed, because library code does not know whether a spill table pointer is
  // needed in the pipeline. Thus we cannot use s15 for anything else. Using the single-arg UserDataArg
  // constructor like this means that the arg is not used, so it will not be set up in PAL metadata.
  bool spill = userDataUsage->haveDynamicUserDataLoads || m_computeWithCalls;

  // Figure out how many sgprs we have available for userDataArgs.
  // We have s0-s31 (s0-s15 for <=GFX8, or for a compute/task shader on any chip) for everything, so take off the number
  // of registers used by specialUserDataArgs.
  unsigned userDataAvailable = (m_shaderStage == ShaderStage::Compute || m_shaderStage == ShaderStage::Task)
                                   ? InterfaceData::MaxCsUserDataCount
                                   : m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount;

  // FIXME Restricting user data as the backend does not support more sgprs as arguments
  unsigned maxCsUserDataCount = InterfaceData::MaxCsUserDataCount;

  if (m_computeWithCalls)
    userDataAvailable = std::min(userDataAvailable, maxCsUserDataCount);

  for (const auto &userDataArg : specialUserDataArgs)
    userDataAvailable -= userDataArg.argDwordSize;
  // ... and the one used by the spill table if already added.
  if (spill)
    userDataAvailable -= 1;

  unsigned userDataEnd = 0;
  for (const auto &userDataArg : userDataArgs)
    userDataEnd += userDataArg.argDwordSize;
  assert(userDataEnd < userDataAvailable && "too many system value user data args");

  if (m_pipelineState->getOptions().forceUserDataSpill) {
    // Force all user data to be spilled; should only be used by indirect RT.
    assert(m_pipelineState->getOptions().rtIndirectMode != RayTracingIndirectMode::NotIndirect);
    spill = true;
    userDataAvailable = userDataEnd;
  }

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
    // If there are user data to set or all users data are forced to be spilled, call setUserDataSpillUsage to update
    // spill_threshold correctly.
    if (userDataSgprs < userDataDwords || m_pipelineState->getOptions().forceUserDataSpill)
      m_pipelineState->getPalMetadata()->setUserDataSpillUsage(userDataSgprs, m_shaderStage);

    // We must conservatively assume that there are functions with dynamic push constant accesses, and that therefore
    // the push constants must be fully available in the spill region even if they fit (partially) into SGPRs.
    const ResourceNode *node = m_pipelineState->findSingleRootResourceNode(ResourceNodeType::PushConst, m_shaderStage);
    if (node)
      m_pipelineState->getPalMetadata()->setUserDataSpillUsage(node->offsetInDwords, m_shaderStage);
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
            m_pipelineState->getPalMetadata()->setUserDataSpillUsage(lastIdx, m_shaderStage);

            // Retry since the current load may now fit.
            continue;
          }
        }

        m_pipelineState->getPalMetadata()->setUserDataSpillUsage(i, m_shaderStage);

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
  if (spill) {
    userDataArgs.emplace_back(builder.getInt32Ty(), "spillTable", UserDataMapping::SpillTable,
                              &userDataUsage->spillTableEntryArgIdx);
  }
  // Make sure the special user data is placed after generic user data because the special user data
  // of shader debug address must be in the tail of all user data.
  userDataArgs.insert(userDataArgs.end(), specialUserDataArgs.begin(), specialUserDataArgs.end());
}

// =====================================================================================================================
// Get UserDataUsage struct for the merged shader stage that contains the given shader stage
//
// @param stage : Shader stage
MutateEntryPoint::UserDataUsage *MutateEntryPoint::getUserDataUsage(ShaderStageEnum stage) {
  stage = getMergedShaderStage(stage);
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
ShaderStageEnum MutateEntryPoint::getMergedShaderStage(ShaderStageEnum stage) const {
  switch (stage) {
  case ShaderStage::Vertex:
    if (m_pipelineState->hasShaderStage(ShaderStage::TessControl))
      return ShaderStage::TessControl;
    LLVM_FALLTHROUGH;
  case ShaderStage::TessEval:
    if (m_pipelineState->hasShaderStage(ShaderStage::Geometry))
      return ShaderStage::Geometry;
    break;
  default:
    break;
  }
  return stage;
}

// =====================================================================================================================
bool MutateEntryPoint::isComputeWithCalls() const {
  return m_computeWithCalls;
}

// =====================================================================================================================
bool MutateEntryPoint::UserDataUsage::isSpecialUserDataUsed(UserDataMapping kind) {
  auto it = specialUserData.find(unsigned(kind));
  if (it == specialUserData.end())
    return false;
  return !it->second.users.empty();
}

// =====================================================================================================================
void MutateEntryPoint::UserDataUsage::addLoad(unsigned dwordOffset, unsigned dwordSize) {
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
