/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerExecutionGraph.cpp
 * @brief LLPC source file: contains implementation of class Llpc::LowerExecutionGraph.
 ***********************************************************************************************************************
 */
#include "LowerExecutionGraph.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcExecutionGraphContext.h"
#include "compilerutils/CompilerUtils.h"
#include "compilerutils/TypeLowering.h"
#include "lgc/Builder.h"
#include "lgc/BuiltIns.h"
#include "lgc/LgcDialect.h"
#include "lgc/LgcWgDialect.h"
#include "lgc/Pipeline.h"
#include "lgc/RuntimeContext.h"
#include "llvm/IR/DerivedUser.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/Operator.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#define DEBUG_TYPE "lower-execution-graph"

using namespace CompilerUtils;
using namespace lgc;
using namespace lgc::wg;
using namespace llvm;
using namespace Llpc;
using namespace spv;
using namespace llvm_dialects;

namespace SPIRV {
extern const char *MetaNameSpirvOp;
} // namespace SPIRV

using namespace SPIRV;
namespace WorkGraphFunc {
enum : unsigned {
  ShaderPreamble = 0,            // Preamble function
  ShaderPostamble,               // Postamble function
  OutputAllocate,                // Allocates output records for a successor node.
  OutputCommit,                  // Commits previously-allocated output records for a successor node.
  OutputGetPayload,              // Retrieves the GPU VA of a specific output payload entry
  InputGetPayloadCount,          // Retrieves the input payload count.
  InputGetPayloadAtIndex,        // Retrieves the GPU address for an input payload at the specified index
  WorkgroupId,                   // Workgroup ID
  GlobalThreadId,                // Global Thread ID,
  ShaderEmptyInputPreamble,      // Empty input preamble
  IncrementEmptyOutputCount,     // Empty output count
  InitCrossGroupSharing,         // Init cross group sharing
  FinishCrossGroupSharing,       // Finish cross group sharing
  IsOutputNodePresent,           // Checks if an output node is valid
  GetRemainingRecursionDepth,    // Get remaining recursion depth
  IsThreadLaunchInvocationValid, // Is ThreadLaunch Invocation Valid
  Count
};
} // namespace WorkGraphFunc

const char *WorkGraphNames[] = {
    "AmdWorkGraphsShaderPreamble",                // ShaderPreamble
    "AmdWorkGraphsShaderPostamble",               // ShaderPostamble
    "AmdWorkGraphsOutputAllocate",                // OutputAllocate
    "AmdWorkGraphsOutputCommit",                  // OutputCommit
    "AmdWorkGraphsOutputGetPayload",              // OutputGetPayload
    "AmdWorkGraphsInputGetPayloadCount",          // InputGetPayloadCount
    "AmdWorkGraphsInputGetPayloadAtIndex",        // InputGetPayloadAtIndex
    "AmdWorkGraphsGroupId",                       // WorkgroupId
    "AmdWorkGraphsGlobalThreadId",                // GlobalThreadId
    "AmdWorkGraphsShaderEmptyInputPreamble",      // Empty input preamble
    "AmdWorkGraphsIncrementEmptyOutputCount",     // Empty output count
    "AmdWorkGraphsInitCrossGroupSharing",         // Init cross group sharing
    "AmdWorkGraphsFinishCrossGroupSharing",       // Finish cross group sharing
    "AmdWorkGraphsIsOutputNodePresent",           // Checks if an output node is valid
    "AmdWorkGraphsGetRemainingRecursionDepth",    // Current graphs recursion depth
    "AmdWorkGraphsIsThreadLaunchInvocationValid", // Is ThreadLaunch Invocation Valid
};

static const char *OutputArgNames[] = {"ShaderState", "Scope", "OutputIdx", "ArrayIdx", "Count"};
static const char *EntryFuncName = "shader"; // Execution graph entry name
const char *WorkgraphOutputCount = "WorkgraphOutputCount";
const char *WorkgraphGetLds = "WorkgraphGetLds";

namespace {

struct LoweringVisitorPayload {
  Llpc::LowerExecutionGraph &pass;
  TypeLowering typeLower;

  explicit LoweringVisitorPayload(Type *payloadArrayPtrType, Llpc::LowerExecutionGraph &pass)
      : pass(pass), typeLower(payloadArrayPtrType->getContext()) {
    typeLower.addRule([payloadArrayPtrType](TypeLowering &, Type * type) -> auto {
      SmallVector<Type *> lowered;
      auto &context = type->getContext();
      if (type->isPointerTy() && type->getPointerAddressSpace() == SPIRAS_PayloadArray) {
        lowered.push_back(PointerType::get(context, SPIRAS_Private));
        lowered.push_back(payloadArrayPtrType);
      } else if (isPayloadType(type)) {
        lowered.push_back(payloadArrayPtrType);
      }
      return lowered;
    });
    typeLower.addConstantRule([](TypeLowering &, Constant * c, ArrayRef<Type *> loweredTypes) -> auto {
      SmallVector<Constant *> lowered;
      if (auto *gv = dyn_cast<GlobalVariable>(c)) {
        if (gv->getAddressSpace() == SPIRAS_PayloadArray) {
          // Stand-in for an input payload array. We don't actually need the value for anything.
          lowered.push_back(PoisonValue::get(loweredTypes[0]));
        }
      }
      return lowered;
    });
  }
};

} // anonymous namespace

template <> struct llvm_dialects::VisitorPayloadProjection<LoweringVisitorPayload, Llpc::LowerExecutionGraph> {
  static Llpc::LowerExecutionGraph &project(LoweringVisitorPayload &payload) { return payload.pass; }
};

LLVM_DIALECTS_VISITOR_PAYLOAD_PROJECT_FIELD(LoweringVisitorPayload, typeLower)

namespace Llpc {

static constexpr unsigned MaxGridCount = 65535; // Max dispatch grid count

// =====================================================================================================================
LowerExecutionGraph::LowerExecutionGraph(Pipeline *pipeline)
    : m_pipeline(pipeline), m_graphLds(nullptr), m_threadLaunch(false) {
  for (unsigned i = 0; i < WorkGraphFunc::Count; ++i) {
    m_workGraphLibFuncNames[WorkGraphNames[i]] = i;
  }
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on (empty on entry)
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LowerExecutionGraph::run(Module &module, ModuleAnalysisManager &analysisManager) {

  LLVM_DEBUG(dbgs() << "Run the pass Lower-execution-graph\n");
  SpirvLower::init(&module);

  auto &graphContext = ExeGraphRuntimeContext::get(module.getContext());
  const auto graphLibModule = graphContext.theModule;
  if (!graphLibModule)
    return PreservedAnalyses::all();

  m_graphLibFuncs.resize(WorkGraphFunc::Count);
  for (unsigned i = 0; i < WorkGraphFunc::Count; ++i) {
    Function *func = graphLibModule->getFunction(WorkGraphNames[i]);
    m_graphLibFuncs[i] = func;
  }

  m_payloadArrayPtrType = getOutputRecordsTy();

  m_metaEnqueueId = m_context->getMDKindID(lgc::wg::ShaderEnqueue);
  MDNode *modeMetadata = m_entryPoint->getMetadata(m_metaEnqueueId);
  if (!modeMetadata)
    return PreservedAnalyses::none();

  m_entryPoint->setName(EntryFuncName);
  m_entryPoint->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  m_entryPoint->setLinkage(GlobalValue::ExternalLinkage);

  assert(modeMetadata->getNumOperands() == std::size(m_enqueueModes.U32All) + 1); // +1 for inputSharedWithName
  unsigned ndx;
  for (ndx = 0; ndx < std::size(m_enqueueModes.U32All); ++ndx) {
    auto metaOp = cast<ConstantAsMetadata>(modeMetadata->getOperand(ndx));
    m_enqueueModes.U32All[ndx] = cast<ConstantInt>(metaOp->getValue())->getZExtValue();
  }
  m_inputSharedWithName = cast<MDString>(modeMetadata->getOperand(ndx))->getString();
  TypeLowering typeLower(*m_context);
  m_typeLowering = &typeLower;
  const auto funcVisitor = llvm_dialects::VisitorBuilder<LowerExecutionGraph>()
                               .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                               .add(&LowerExecutionGraph::visitRegisterOutputNode)
                               .build();
  funcVisitor.visit(*this, module);
  m_typeLowering->finishCleanup();

  initInputPayloadInfo(m_enqueueModes);
  m_builder->SetInsertPointPastAllocas(m_entryPoint);
  initAllocVariables(m_builder);
  // Call ShaderPreamble
  // NOTE: according to the PAL comment notes to the EmptyInputPreamble, for dynamic dispatch workgroup, implied by
  // the MaxNumWorkgroupsAMDX is not zero, dynamic expansion nodes cannot have zero-byte payloads because the grid
  // size is 12 bytes.
  CrossModuleInliner inliner;
  auto gprsVariable =
      (m_inputPayloadInfo.payloadSize == 0 && m_enqueueModes.modes.maxNumWorkgroupsX == 0 &&
       m_enqueueModes.modes.maxNumWorkgroupsY == 0)
          ? inliner.inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::ShaderEmptyInputPreamble], {}).returnValue
          : inliner.inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::ShaderPreamble], {}).returnValue;

  // Keep the gprs variable from ShaderPreamble call
  m_builder->CreateStore(gprsVariable, m_outputAllocateArgs[OutputAllocateArg::ShaderState]);

  // Create input counts number
  auto inputsCount = inliner
                         .inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::InputGetPayloadCount],
                                     {m_outputAllocateArgs[OutputAllocateArg::ShaderState]})
                         .returnValue;
  m_builder->CreateStore(inputsCount, m_builtInVariables[WorkGraphBuiltIns::CoalescedInputCount]);

  auto remaining = inliner
                       .inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::GetRemainingRecursionDepth],
                                   {m_outputAllocateArgs[OutputAllocateArg::ShaderState]})
                       .returnValue;
  m_builder->CreateStore(remaining, m_builtInVariables[WorkGraphBuiltIns::RemainingRecursionLevels]);

  unsigned shaderIndex =
      m_inputPayloadInfo.arrayIndex != InvalidValue ? m_inputPayloadInfo.arrayIndex : m_enqueueModes.modes.shaderIndex;
  m_builder->CreateStore(m_builder->getInt32(shaderIndex), m_builtInVariables[WorkGraphBuiltIns::ShaderIndex]);
  auto shaderMode = Pipeline::getComputeShaderMode(module);
  m_threadLaunch = isThreadLaunchNode(shaderMode, m_enqueueModes, m_inputPayloadInfo);
  auto zero = m_builder->getInt32(0);
  auto constVec = ConstantVector::get({zero, zero, zero});

  if (m_threadLaunch) {
    auto valid =
        cast<Instruction>(inliner
                              .inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::IsThreadLaunchInvocationValid],
                                          {m_outputAllocateArgs[OutputAllocateArg::ShaderState]})
                              .returnValue);
    auto nextPos = valid->getNextNode();
    Instruction *terminator = SplitBlockAndInsertIfElse(valid, m_builder->GetInsertPoint(), false);
    m_builder->SetInsertPoint(terminator);
    m_builder->CreateRetVoid();
    terminator->eraseFromParent();

    m_builder->SetInsertPoint(nextPos);
    m_localInvocationIndex =
        new GlobalVariable(*m_module, zero->getType(), false, GlobalVariable::ExternalLinkage, nullptr, "localIndex",
                           nullptr, GlobalValue::NotThreadLocal, SPIRAS_Private);
    m_builder->CreateStore(zero, m_localInvocationIndex);
    shaderMode.workgroupSizeX = 32;
    Pipeline::setComputeShaderMode(module, shaderMode);
  }
  if (m_enqueueModes.modes.isCoalescing) {
    // Create WorkgroupId
    m_builder->CreateStore(constVec, m_builtInVariables[WorkGraphBuiltIns::WorkgroupId]);
    // Create GlobalInvocationId
    Value *localInvocationId =
        m_threadLaunch
            ? constVec
            : m_builder->CreateReadBuiltInInput(static_cast<lgc::BuiltInKind>(lgc::BuiltInLocalInvocationId));
    m_builder->CreateStore(localInvocationId, m_builtInVariables[WorkGraphBuiltIns::GlobalInvocationId]);

  } else {
    // Create WorkgroupId
    auto workGroupId = inliner
                           .inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::WorkgroupId],
                                       {m_outputAllocateArgs[OutputAllocateArg::ShaderState]})
                           .returnValue;
    m_builder->CreateStore(workGroupId, m_builtInVariables[WorkGraphBuiltIns::WorkgroupId]);

    // Create GlobalInvocationId
    auto globalInvocationId = inliner
                                  .inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::GlobalThreadId],
                                              {m_outputAllocateArgs[OutputAllocateArg::ShaderState]})
                                  .returnValue;
    m_builder->CreateStore(globalInvocationId, m_builtInVariables[WorkGraphBuiltIns::GlobalInvocationId]);
  }

  SmallVector<Instruction *, 4> rets;
  getFuncRets(m_entryPoint, rets);
  for (auto ret : rets) {
    m_builder->SetInsertPoint(ret);
    inliner.inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::ShaderPostamble],
                       {m_outputAllocateArgs[OutputAllocateArg::ShaderState]});
  }
  LoweringVisitorPayload payload(m_payloadArrayPtrType, *this);
  m_typeLowering = &payload.typeLower;
  static const auto visitor = llvm_dialects::VisitorBuilder<LoweringVisitorPayload>()
                                  .nest<LowerExecutionGraph>([](auto &b) {
                                    b.add(&LowerExecutionGraph::visitLoad);
                                    b.add(&LowerExecutionGraph::visitStore);
                                    b.add(&LowerExecutionGraph::visitAlloca);
                                    b.add(&LowerExecutionGraph::visitGetElementPtr);
                                    b.add(&LowerExecutionGraph::visitIndexPayloadArray);
                                    b.add(&LowerExecutionGraph::visitAllocateNodePayloads);
                                    b.add(&LowerExecutionGraph::visitEnqueueNodePayloads);
                                    b.add(&LowerExecutionGraph::visitPayloadArrayLength);
                                    b.add(&LowerExecutionGraph::visitIsNodePayloadValid);
                                    b.add(&LowerExecutionGraph::visitFinishWritingNodePayload);
                                  })
                                  .nest(&TypeLowering::registerVisitors)
                                  .build();

  visitor.visit(payload, *m_module);
  payload.typeLower.finishPhis();
  payload.typeLower.finishCleanup();
  m_typeLowering = nullptr;
  buildExecGraphNodeMetadata(m_enqueueModes, m_inputPayloadInfo);
  lowerGlobals(m_metaEnqueueId, m_context->getMDKindID(gSPIRVMD::InOut));
  unsigned outputCount = m_nodeNamesIdx.size();
  createGraphLds(outputCount);
  // Post visit dialects after Workgraph library functions inlined
  static const auto postVisitor = llvm_dialects::VisitorBuilder<LowerExecutionGraph>()
                                      .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                                      .add(&LowerExecutionGraph::visitGraphGetLds)
                                      .add(&LowerExecutionGraph::visitOutputCount)
                                      .build();
  postVisitor.visit(*this, *m_module);

  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Pre-parse to the RegisterOutputNodeOp to get number of node types/names, and setup m_nodeNamesIdx
//
// @param inst : the instruction to lower
void LowerExecutionGraph::visitRegisterOutputNode(lgc::wg::RegisterOutputNodeOp &inst) {
  static const unsigned remappedScopes[3] = {WorkCreationScope::Workgroup, WorkCreationScope::Subgroup,
                                             WorkCreationScope::Invocation};
  unsigned scope = inst.getScope();
  assert(scope == ScopeWorkgroup || scope == ScopeSubgroup || scope == ScopeInvocation);
  unsigned remappedScope = remappedScopes[scope - ScopeWorkgroup];

  auto payloadNameVar = cast<GlobalVariable>(inst.getPayloadName());
  auto payloadName = cast<ConstantDataArray>(payloadNameVar->getInitializer())->getAsString();

  unsigned payloadSize = inst.getPayloadSize();
  unsigned payloadMaxCount = inst.getPayloadMaxCount();
  unsigned payloadId = inst.getPayloadId();
  unsigned limitsSharedWith = inst.getLimitsSharedWith();
  bool trackFinishWriting = inst.getTrackFinishWriting();
  unsigned payloadArrayTyId = inst.getArrayTypeId();

  auto nameIter = m_nodeNamesIdx.find(payloadName);
  if (nameIter == m_nodeNamesIdx.end()) {
    m_nodeNamesIdx[payloadName] = {payloadMaxCount,     payloadSize,      payloadId,
                                   limitsSharedWith,    remappedScope,    trackFinishWriting,
                                   inst.getArraySize(), payloadArrayTyId, 0};
    nameIter = m_nodeNamesIdx.find(payloadName);
  } else {
    // Add up the payloadMaxCount for the same output node
    nameIter->second.payloadCount += payloadMaxCount;
    nameIter->second.payloadSize = std::max(nameIter->second.payloadSize, payloadSize);
  }
  m_typeLowering->eraseInstruction(&inst);
}

// =====================================================================================================================
// Lower an allocate.node.payloads op
//
// @param inst : the instruction to lower
void LowerExecutionGraph::visitAllocateNodePayloads(lgc::wg::AllocateNodePayloadsOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto payloadNameVar = cast<GlobalVariable>(inst.getPayloadName());
  auto payloadName = cast<ConstantDataArray>(payloadNameVar->getInitializer())->getAsString();
  auto nameIter = m_nodeNamesIdx.find(payloadName);

  auto baseIndex = inst.getBaseIndex();

  m_builder->CreateStore(m_builder->getInt32(nameIter->second.scope), m_outputAllocateArgs[OutputAllocateArg::Scope]);

  // MapVector will keep the index of insertion, so the OutputIndex would be index of Output payload nodes names array.
  // Each array member must have a unique node name, array index is the specific shader in that array.
  auto OutputIndex = nameIter - m_nodeNamesIdx.begin();
  m_builder->CreateStore(m_builder->getInt32(OutputIndex), m_outputAllocateArgs[OutputAllocateArg::OutputIdx]);

  m_builder->CreateStore(inst.getPayloadCount(), m_outputAllocateArgs[OutputAllocateArg::Count]);

  Value *nodeIdx = inst.getNodeIndex();
  nodeIdx = m_builder->CreateAdd(nodeIdx, baseIndex);
  const bool recursiveNode =
      (m_enqueueModes.modes.maxNodeRecursion > 0) && (payloadName == m_inputPayloadInfo.nodeName);
  if (recursiveNode) {
    // NOTE: Always needs to be 0 for recursive calls since recursive output ports always have an
    // array index offset equal to the parent. No need to check the array index provided by the
    // app since the only legal case is self-recursion (the node calling itself, same name, same index).
    nodeIdx = m_builder->getInt32(0);
  }
  m_builder->CreateStore(nodeIdx, m_outputAllocateArgs[OutputAllocateArg::ArrayIdx]);

  // Call OutputAllocate
  SmallVector<Value *, 5> args;
  for (auto arg : m_outputAllocateArgs) {
    args.push_back(arg);
  }
  CrossModuleInliner inliner;
  Value *outputRecords = nullptr;
  if (nameIter->second.payloadSize == 0) {
    inliner.inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::IncrementEmptyOutputCount], args);
    outputRecords = PoisonValue::get(getOutputRecordsTy());
  } else {
    outputRecords = inliner.inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::OutputAllocate], args).returnValue;
  }
  auto dummyValue = ConstantPointerNull::get(PointerType::get(*m_context, SPIRAS_Private));
  m_typeLowering->replaceInstruction(&inst, {dummyValue, outputRecords});
}

// =====================================================================================================================
// Lower an enqueue.node.payloads op
//
// @param inst : the instruction to lower
void LowerExecutionGraph::visitEnqueueNodePayloads(lgc::wg::EnqueueNodePayloadsOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto *payloadArrayPtr = m_typeLowering->getValue(inst.getPayloads())[0];
  auto payloadNameVar = cast<GlobalVariable>(inst.getPayloadName());
  auto payloadName = cast<ConstantDataArray>(payloadNameVar->getInitializer())->getAsString();

  auto nameIter = m_nodeNamesIdx.find(payloadName);
  assert(nameIter != m_nodeNamesIdx.end());
  m_builder->CreateStore(m_builder->getInt32(nameIter->second.scope), m_outputAllocateArgs[OutputAllocateArg::Scope]);

  SmallVector<Value *, 3> args = {m_outputAllocateArgs[OutputAllocateArg::ShaderState],
                                  m_outputAllocateArgs[OutputAllocateArg::Scope], payloadArrayPtr};
  CrossModuleInliner inliner;
  if (nameIter->second.trackFinishWriting) {
    inliner.inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::InitCrossGroupSharing], args);
  }

  inliner.inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::OutputCommit], args);

  m_typeLowering->eraseInstruction(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Lower a finish.writing.node.payload op
//
// @param inst : the instruction to lower
void LowerExecutionGraph::visitFinishWritingNodePayload(wg::FinishWritingNodePayloadOp &inst) {
  m_builder->SetInsertPoint(&inst);
  CrossModuleInliner inliner;
  inst.replaceAllUsesWith(inliner
                              .inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::FinishCrossGroupSharing],
                                          {m_outputAllocateArgs[OutputAllocateArg::ShaderState]})
                              .returnValue);

  m_typeLowering->eraseInstruction(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Lower a payload.array.length op
//
// @param inst : the instruction to lower
void LowerExecutionGraph::visitPayloadArrayLength(wg::PayloadArrayLengthOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *nodeCount = nullptr;
  if (inst.getInput()) {
    nodeCount =
        m_builder->CreateLoad(m_builder->getInt32Ty(), m_builtInVariables[WorkGraphBuiltIns::CoalescedInputCount]);
  } else {
    // Output variable
    auto *payloadArrayPtr = m_typeLowering->getValue(inst.getPayloads())[0];
    Value *args[] = {m_builder->getInt32(0), m_builder->getInt32(4)};
    nodeCount = m_builder->CreateGEP(m_payloadArrayPtrType, payloadArrayPtr, args);
    nodeCount = m_builder->CreateLoad(m_builder->getInt32Ty(), nodeCount);
  }
  inst.replaceAllUsesWith(nodeCount);
  m_typeLowering->eraseInstruction(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Lower a LoadInst instruction
//
// @param inst : the instruction to lower
VisitorResult LowerExecutionGraph::visitLoad(LoadInst &inst) {
  m_builder->SetInsertPoint(&inst);
  if (inst.getPointerOperandType()->getPointerAddressSpace() == SPIRAS_PayloadArray) {
    Value *outputRecord = m_typeLowering->getValue(inst.getPointerOperand())[1];
    m_typeLowering->replaceInstruction(&inst, outputRecord);
  }
  return VisitorResult::Stop;
}

// =====================================================================================================================
// Lower a StoreInst instruction
//
// @param inst : the instruction to lower
VisitorResult LowerExecutionGraph::visitStore(StoreInst &inst) {
  m_builder->SetInsertPoint(&inst);
  if (inst.getPointerOperandType()->getPointerAddressSpace() == SPIRAS_PayloadArray) {
    auto ptrOperand = inst.getPointerOperand();
    Value *newPtrOperand = m_typeLowering->getValue(ptrOperand)[0];
    Value *newVal = m_typeLowering->getValue(inst.getValueOperand())[0];
    m_builder->CreateStore(newVal, newPtrOperand, inst.isVolatile());
    m_typeLowering->eraseInstruction(&inst);
  }
  return VisitorResult::Stop;
}

// =====================================================================================================================
// Lower an AllocInst
//
// @param inst : the instruction to lower
VisitorResult LowerExecutionGraph::visitAlloca(AllocaInst &inst) {
  m_builder->SetInsertPoint(&inst);
  if (inst.getAddressSpace() == SPIRAS_PayloadArray) {
    Type *allocTy = replacePayloadType(inst.getAllocatedType());
    auto newAlloc = m_builder->CreateAlloca(allocTy);
    auto dummyValue = PoisonValue::get(m_payloadArrayPtrType);
    m_typeLowering->replaceInstruction(&inst, {newAlloc, dummyValue});
  }
  return VisitorResult::Stop;
}

// =====================================================================================================================
// Lower a GetElementPtrInst
//
// @param inst : the instruction to lower
VisitorResult LowerExecutionGraph::visitGetElementPtr(GetElementPtrInst &inst) {
  m_builder->SetInsertPoint(&inst);
  if (inst.getAddressSpace() == SPIRAS_PayloadArray) {
    Type *gepTy = replacePayloadType(inst.getSourceElementType());
    Value *srcElement = m_typeLowering->getValue(inst.getPointerOperand())[0];
    Value *newGep = nullptr;
    SmallVector<Value *, 8> indices(inst.idx_begin(), inst.idx_end());
    if (inst.isInBounds())
      newGep = m_builder->CreateInBoundsGEP(gepTy, srcElement, indices);
    else
      newGep = m_builder->CreateGEP(gepTy, srcElement, indices);
    auto dummyValue = PoisonValue::get(m_payloadArrayPtrType);
    m_typeLowering->replaceInstruction(&inst, {newGep, dummyValue});
  }
  return VisitorResult::Stop;
}

// =====================================================================================================================
// Recursive replace {} to the OutputRecordType in the aggregation type
//
// @param ty : The type to replace
Type *LowerExecutionGraph::replacePayloadType(Type *ty) {
  if (isPayloadType(ty)) {
    return m_payloadArrayPtrType;
  } else if (ty->isStructTy()) {
    SmallVector<Type *> elemTys;
    for (unsigned i = 0; i < ty->getStructNumElements(); ++i)
      elemTys.push_back(replacePayloadType(ty->getStructElementType(i)));
    return StructType::get(*m_context, elemTys);
  } else if (ty->isArrayTy()) {
    return ArrayType::get(replacePayloadType(ty->getArrayElementType()), ty->getArrayNumElements());
  } else
    return ty;
}

// =====================================================================================================================
// Lower an is.node.payload.valid
//
// @param inst : the instruction to lower
void LowerExecutionGraph::visitIsNodePayloadValid(wg::IsNodePayloadValidOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto payloadNameVar = cast<GlobalVariable>(inst.getPayloadName());
  auto payloadName = cast<ConstantDataArray>(payloadNameVar->getInitializer())->getAsString();
  auto nameIter = m_nodeNamesIdx.find(payloadName);
  auto OutputIndex = nameIter - m_nodeNamesIdx.begin();
  m_builder->CreateStore(m_builder->getInt32(OutputIndex), m_outputAllocateArgs[OutputAllocateArg::OutputIdx]);
  m_builder->CreateStore(inst.getNodeIndex(), m_outputAllocateArgs[OutputAllocateArg::ArrayIdx]);
  Value *args[] = {m_outputAllocateArgs[OutputAllocateArg::ShaderState],
                   m_outputAllocateArgs[OutputAllocateArg::OutputIdx],
                   m_outputAllocateArgs[OutputAllocateArg::ArrayIdx]};
  CrossModuleInliner inliner;
  Value *isValid =
      inliner.inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::IsOutputNodePresent], args).returnValue;
  inst.replaceAllUsesWith(isValid);

  m_typeLowering->eraseInstruction(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Create global variables
//
// @param builder : The builder to create variable
void LowerExecutionGraph::initAllocVariables(lgc::Builder *builder) {
  Type *tys[] = {getShaderStateTy(), m_builder->getInt32Ty(), m_builder->getInt32Ty(), m_builder->getInt32Ty(),
                 m_builder->getInt32Ty()};

  for (unsigned i = 0; i < m_outputAllocateArgs.size(); ++i) {
    m_outputAllocateArgs[i] = m_builder->CreateAlloca(tys[i], nullptr, Twine(OutputArgNames[i]));
  }
  m_tempVariable = m_builder->CreateAlloca(m_builder->getInt32Ty(), nullptr, Twine("tempVariable"));
  auto int32x3Ty = FixedVectorType::get(m_builder->getInt32Ty(), 3);
  Type *builtInTys[] = {m_builder->getInt32Ty(), int32x3Ty, int32x3Ty, m_builder->getInt32Ty(), m_builder->getInt32Ty(),
                        m_builder->getInt32Ty()};

  for (unsigned i = 0; i < WorkGraphBuiltIns::Count; ++i) {
    m_builtInVariables[i] =
        new GlobalVariable(*m_module, builtInTys[i], false, GlobalVariable::ExternalLinkage, nullptr,
                           Twine("builtIn") + std::to_string(i), nullptr, GlobalValue::NotThreadLocal, SPIRAS_Private);
  }
}

// =====================================================================================================================
// Get AmdWorkGraphsShaderState type
Type *LowerExecutionGraph::getShaderStateTy() {
  return m_graphLibFuncs[WorkGraphFunc::ShaderPreamble]->getReturnType();
}

// =====================================================================================================================
// Get OutputRecords type
Type *LowerExecutionGraph::getOutputRecordsTy() {
  return m_graphLibFuncs[WorkGraphFunc::OutputAllocate]->getReturnType();
}

// =====================================================================================================================
// Get all the function ReturnInst
//
// @param func : Function to gather ReturnInst
// @param rets : Returned vector of  ReturnInst instructions
void LowerExecutionGraph::getFuncRets(Function *func, SmallVector<Instruction *, 4> &rets) {
  for (auto &block : *func) {
    auto blockTerm = block.getTerminator();
    if (blockTerm != nullptr && isa<ReturnInst>(blockTerm))
      rets.push_back(blockTerm);
  }
}

// =====================================================================================================================
// Lower the builtin and workgraph global variables
//
// @param enqueueMetaId : Metadata for the workgraph variables
// @param inoutMetaId : Metadata for the built-in variables
void LowerExecutionGraph::lowerGlobals(unsigned enqueueMetaId, unsigned inoutMetaId) {
  for (Function *func : m_funcsToLower) {
    func->dropAllReferences();
    func->eraseFromParent();
  }

  SmallVector<Instruction *, 4> geps;
  for (auto globalIt = m_module->global_begin(), end = m_module->global_end(); globalIt != end;) {
    GlobalVariable *global = &*globalIt++;
    auto meta = global->getMetadata(enqueueMetaId);
    if (meta != nullptr) {
      global->eraseFromParent();
    } else if ((meta = global->getMetadata(inoutMetaId)) != nullptr) {
      processBuiltinGlobals(global, meta);
    }
  }
}

// =====================================================================================================================
// Lower the built-in global variables
//
// @param global : Global variables to lower
// @param metadata : Metadata for the built-in variables
void LowerExecutionGraph::processBuiltinGlobals(GlobalVariable *global, MDNode *metadata) {
  auto meta = mdconst::dyn_extract<Constant>(metadata->getOperand(0));
  unsigned startOperand = 0;
  Type *globalTy = global->getValueType();
  if (globalTy->isArrayTy()) {
    assert(meta->getNumOperands() == 4);
    startOperand += 2;
  }
  ShaderInOutMetadata inputMeta = {};
  inputMeta.U64All[0] = cast<ConstantInt>(meta->getOperand(startOperand))->getZExtValue();
  inputMeta.U64All[1] = cast<ConstantInt>(meta->getOperand(startOperand + 1))->getZExtValue();
  llvm::GlobalVariable *replacement = nullptr;
  switch (inputMeta.Value) {
  case spv::BuiltInWorkgroupId:
    replacement = m_builtInVariables[WorkGraphBuiltIns::WorkgroupId];
    break;
  case spv::BuiltInGlobalInvocationId:
    replacement = m_builtInVariables[WorkGraphBuiltIns::GlobalInvocationId];
    break;
  case spv::BuiltInLocalInvocationId:
  case spv::BuiltInLocalInvocationIndex: {
    if (!m_threadLaunch)
      return;
    replacement = inputMeta.Value == spv::BuiltInLocalInvocationId
                      ? m_builtInVariables[WorkGraphBuiltIns::GlobalInvocationId]
                      : m_localInvocationIndex;
    break;
  }
  case spv::BuiltInShaderIndexAMDX:
    replacement = m_builtInVariables[WorkGraphBuiltIns::ShaderIndex];
    break;
  case spv::BuiltInRemainingRecursionLevelsAMDX: {
    replacement = m_builtInVariables[WorkGraphBuiltIns::RemainingRecursionLevels];
  } break;
  default:
    // For other builtin Globals, return
    return;
  }
  global->mutateType(replacement->getType());
  replaceGlobal(m_context, global, replacement);
}

// =====================================================================================================================
// Fill m_inputPayloadInfo with payload metadata and ShaderEnqueue mode
//
// @param enqueueModes : Workgraph shader enqueue modes
void LowerExecutionGraph::initInputPayloadInfo(const lgc::wg::ShaderEnqueueMode &enqueueModes) {
  m_inputPayloadInfo = {"", InvalidValue, 0, 0, false, InvalidValue, InvalidValue, InvalidValue, InvalidValue};
  auto moduleMetadata = m_module->getNamedMetadata(lgc::wg::ShaderEnqueue);
  MDNode *payloadMeta = moduleMetadata->getOperand(moduleMetadata->getNumOperands() - 1);
  m_inputPayloadInfo.nodeName = cast<MDString>(payloadMeta->getOperand(0))->getString();
  auto arrayIndexMeta = cast<ConstantAsMetadata>(payloadMeta->getOperand(1));
  m_inputPayloadInfo.arrayIndex = cast<ConstantInt>(arrayIndexMeta->getValue())->getZExtValue();

  if (moduleMetadata->getNumOperands() > 1) {
    payloadMeta = moduleMetadata->getOperand(0);
    auto maxPayloadMeta = cast<ConstantAsMetadata>(payloadMeta->getOperand(0));
    m_inputPayloadInfo.payloadCount = cast<ConstantInt>(maxPayloadMeta->getValue())->getZExtValue();
    auto payloadSizeMeta = cast<ConstantAsMetadata>(payloadMeta->getOperand(1));
    m_inputPayloadInfo.payloadSize = cast<ConstantInt>(payloadSizeMeta->getValue())->getZExtValue();
    auto trackFinishWritingMeta = cast<ConstantAsMetadata>(payloadMeta->getOperand(2));
    m_inputPayloadInfo.trackFinishWriting = cast<ConstantInt>(trackFinishWritingMeta->getValue())->isOne();
    auto dynamicDispatchMeta = cast<ConstantAsMetadata>(payloadMeta->getOperand(3));
    m_inputPayloadInfo.dynamicDispatch = cast<ConstantInt>(dynamicDispatchMeta->getValue())->getZExtValue();
    auto nodeTypeMeta = cast<ConstantAsMetadata>(payloadMeta->getOperand(4));
    m_inputPayloadInfo.nodeType = cast<ConstantInt>(nodeTypeMeta->getValue())->getZExtValue();
    auto vbTableOffsetMeta = cast<ConstantAsMetadata>(payloadMeta->getOperand(5));
    m_inputPayloadInfo.vbTableOffset = cast<ConstantInt>(vbTableOffsetMeta->getValue())->getZExtValue();
    auto indexBufferOffsetMeta = cast<ConstantAsMetadata>(payloadMeta->getOperand(6));
    m_inputPayloadInfo.indexBufferOffset = cast<ConstantInt>(indexBufferOffsetMeta->getValue())->getZExtValue();
  }
}

// =====================================================================================================================
// Build the ExecutionGraph PAL metadata
//
// @param enqueueModes : ShaderEnqueueMode mode
// @param payloads : Payload size and count
void LowerExecutionGraph::buildExecGraphNodeMetadata(const ShaderEnqueueMode &enqueueModes,
                                                     const InputPayloadInfo &payloads) {

  lgc::GraphNodeMetadata graphNodeMeta = {};
  graphNodeMeta.payloadMaxCount = payloads.payloadCount;
  graphNodeMeta.payloadSize = payloads.payloadSize;
  graphNodeMeta.maxRecursionDepth = enqueueModes.modes.maxNodeRecursion;
  graphNodeMeta.node.name = payloads.nodeName;
  graphNodeMeta.node.arrayIndex =
      payloads.arrayIndex != InvalidValue ? payloads.arrayIndex : enqueueModes.modes.shaderIndex;
  graphNodeMeta.inputSharedWith.name = m_inputSharedWithName;
  graphNodeMeta.inputSharedWith.arrayIndex = enqueueModes.modes.inputSharedWithArrayIndex;
  graphNodeMeta.payloadFlags.crossGroupSharing = payloads.trackFinishWriting;

  if (payloads.dynamicDispatch != InvalidValue) {
    graphNodeMeta.dynamicDispatchGrid.componentCount = payloads.dynamicDispatch >> 24;
    graphNodeMeta.dynamicDispatchGrid.bitsPerComponent = (payloads.dynamicDispatch >> 16) & 0xff;
    graphNodeMeta.dynamicDispatchGrid.offset = payloads.dynamicDispatch & 0xffff;
  } else {
    graphNodeMeta.dynamicDispatchGrid.componentCount = 3;
    graphNodeMeta.dynamicDispatchGrid.bitsPerComponent = (sizeof(unsigned) << 3);
    graphNodeMeta.dynamicDispatchGrid.offset = 0;
  }

  graphNodeMeta.outputs.resize(m_nodeNamesIdx.size());
  unsigned outIdx = 0;
  for (auto &nodeName : m_nodeNamesIdx) {
    NodeShaderOutputInfo &outputInfo = graphNodeMeta.outputs[outIdx++];

    bool recursiveNode = (enqueueModes.modes.maxNodeRecursion > 0) && (nodeName.first == graphNodeMeta.node.name);
    outputInfo.node.arrayIndex = recursiveNode ? graphNodeMeta.node.arrayIndex : 0;
    outputInfo.arrayCount = recursiveNode ? 1 : UINT_MAX;
    // NOTE: It is a workaround of test issue; revisit once the spec has been updated
    outputInfo.payloadMaxCount = std::min(nodeName.second.payloadCount, 256u);
    outputInfo.payloadSize = nodeName.second.payloadSize;
    outputInfo.payloadFlags.crossGroupSharing = nodeName.second.trackFinishWriting;
    // Copy name
    outputInfo.node.name = nodeName.first.str();

    bool validPayloadIdToShare = nodeName.second.limitSharedWith != InvalidValue;
    outputInfo.budgetSharedWith.enable = validPayloadIdToShare;
    outputInfo.budgetSharedWith.index = validPayloadIdToShare ? getOutputIndex(nodeName.second.limitSharedWith) : 0;
  }

  // Determine the graph node type
  // If static dispatch size is provided -> Fixed expansion
  // If coalescing mode is provided -> Coalescing
  // Otherwise -> Dynamic expansion
  if (enqueueModes.modes.staticNumWorkgroupsX != 0) {
    assert(enqueueModes.modes.staticNumWorkgroupsX != 0 && enqueueModes.modes.staticNumWorkgroupsY != 0 &&
           enqueueModes.modes.staticNumWorkgroupsZ != 0);
    assert(enqueueModes.modes.maxNumWorkgroupsX == 0 && enqueueModes.modes.maxNumWorkgroupsY == 0 &&
           enqueueModes.modes.maxNumWorkgroupsZ == 0);
    assert(enqueueModes.modes.isCoalescing == false);
    graphNodeMeta.nodeType = GraphNodeTypeFixedExpansion;

    graphNodeMeta.dispatchGridX = enqueueModes.modes.staticNumWorkgroupsX;
    graphNodeMeta.dispatchGridY = enqueueModes.modes.staticNumWorkgroupsY;
    graphNodeMeta.dispatchGridZ = enqueueModes.modes.staticNumWorkgroupsZ;
  } else if (enqueueModes.modes.isCoalescing) {
    assert(enqueueModes.modes.staticNumWorkgroupsX == 0 && enqueueModes.modes.staticNumWorkgroupsY == 0 &&
           enqueueModes.modes.staticNumWorkgroupsZ == 0);
    assert(enqueueModes.modes.maxNumWorkgroupsX == 0 && enqueueModes.modes.maxNumWorkgroupsY == 0 &&
           enqueueModes.modes.maxNumWorkgroupsZ == 0);
    graphNodeMeta.nodeType = m_threadLaunch ? GraphNodeTypeThreadLaunch : GraphNodeTypeCoalescing;
  } else {
    assert(enqueueModes.modes.staticNumWorkgroupsX == 0 && enqueueModes.modes.staticNumWorkgroupsY == 0 &&
           enqueueModes.modes.staticNumWorkgroupsZ == 0);
    assert(enqueueModes.modes.isCoalescing == false);
    graphNodeMeta.nodeType = GraphNodeTypeDynamicExpansion;
    graphNodeMeta.dispatchGridX = enqueueModes.modes.maxNumWorkgroupsX;
    graphNodeMeta.dispatchGridY = enqueueModes.modes.maxNumWorkgroupsY;
    graphNodeMeta.dispatchGridZ = enqueueModes.modes.maxNumWorkgroupsZ;
    // Payload not explicitly declared, but it must exist and contain at least the dispatch size
    if (graphNodeMeta.payloadSize == 0) {
      graphNodeMeta.payloadSize = 12;
      graphNodeMeta.payloadMaxCount = 1;
    }

    // The shader didn't provide MaxNumWorkgroupsAMDX, fall back to the max limit
    if (graphNodeMeta.dispatchGridX == 0) {
      graphNodeMeta.dispatchGridX = MaxGridCount;
      graphNodeMeta.dispatchGridY = MaxGridCount;
      graphNodeMeta.dispatchGridZ = MaxGridCount;
    }
  }

  // Affects PatchPreparePipelineAbi::setAbiEntryNames() for compute shaders.
  m_pipeline->setGraphMetadata(graphNodeMeta);
}

// =====================================================================================================================
// Get output node index

// @param payloadId : Output payload id
unsigned LowerExecutionGraph::getOutputIndex(unsigned id) {
  unsigned outIdx = 0;
  for (auto &nodeName : m_nodeNamesIdx) {
    // The SPIR-V spec expects the decoration to refer to an array type's id.
    // String name's id is a fallback for glslang compatibility.
    if ((nodeName.second.arrayTypeId == id) || (nodeName.second.payloadId == id))
      return outIdx;
    outIdx++;
  }
  llvm_unreachable("Should find payloadId");
  return outIdx;
}

// =====================================================================================================================
// Lower dialect IndexPayloadArrayOp
//
// @param [in] inst : IndexPayloadArrayOp to lower
void LowerExecutionGraph::visitIndexPayloadArray(lgc::wg::IndexPayloadArrayOp &inst) {
  m_builder->SetInsertPoint(&inst);
  CrossModuleInliner inliner;
  Value *payloadAddr = nullptr;
  bool isInput = cast<ConstantInt>(inst.getInput())->isOne();
  if (isInput) {
    Value *indexValue =
        m_threadLaunch ? m_builder->CreateReadBuiltInInput(lgc::BuiltInLocalInvocationIndex) : inst.getIndex();
    m_builder->CreateStore(indexValue, m_tempVariable);

    Value *args[] = {m_outputAllocateArgs[OutputAllocateArg::ShaderState], m_tempVariable};
    payloadAddr =
        inliner.inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::InputGetPayloadAtIndex], args).returnValue;
  } else {
    m_builder->CreateStore(inst.getIndex(), m_tempVariable);
    auto payloadArray = m_typeLowering->getValue(inst.getPayloadArray())[0];
    Value *args[] = {payloadArray, m_tempVariable};
    payloadAddr = inliner.inlineCall(*m_builder, m_graphLibFuncs[WorkGraphFunc::OutputGetPayload], args).returnValue;
  }
  payloadAddr = m_builder->CreateIntToPtr(payloadAddr, PointerType::get(*m_context, SPIRAS_Global));
  // TODO: currently recursive set GEP chain load/store as volatile to make payload access
  // coherent, aka, load glc/dlc.
  // correctly represent memory model semantics once backend is ready
  std::function<void(Value *)> setLoadStore = [&](Value *nodearray) {
    for (Use &use : nodearray->uses()) {
      Instruction *chainedUser = cast<Instruction>(use.getUser());
      if (auto loadInst = dyn_cast<LoadInst>(chainedUser)) {
        loadInst->setVolatile(true);
      } else if (auto storeInst = dyn_cast<StoreInst>(chainedUser)) {
        storeInst->setVolatile(true);
      } else {
        auto gepInst = cast<GetElementPtrInst>(chainedUser);
        gepInst->mutateType(nodearray->getType());
        setLoadStore(gepInst);
      }
    }
  };

  setLoadStore(&inst);
  inst.replaceAllUsesWith(payloadAddr);
  m_typeLowering->eraseInstruction(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Get input payload

// @param enqueueMetaId : shader enqueue metadata ID
GlobalVariable *LowerExecutionGraph::getInputPayload(unsigned enqueueMetaId) {
  for (auto &global : m_module->globals()) {
    if (global.getMetadata(enqueueMetaId)) {
      return &global;
    }
  }
  return nullptr;
}

// =====================================================================================================================
// Is thread node

// @param shaderMode : compute shader mode
// @param enqueueModes : enqueue mode
// @param payload : payload
bool LowerExecutionGraph::isThreadLaunchNode(const lgc::ComputeShaderMode &shaderMode,
                                             const ShaderEnqueueMode &enqueueModes, const InputPayloadInfo &payloads) {

  // Workgroup size is 1, 1, 1
  bool threadLaunch = shaderMode.workgroupSizeX == 1;
  threadLaunch = threadLaunch && (shaderMode.workgroupSizeY == 1);
  threadLaunch = threadLaunch && (shaderMode.workgroupSizeZ == 1);
  // Must be coalescing node.
  threadLaunch = threadLaunch && enqueueModes.modes.isCoalescing;
  // If there is input payload, then input payload count is 1
  threadLaunch = threadLaunch && (payloads.payloadCount <= 1);

  // Less than 8 allocation nodes
  threadLaunch = threadLaunch && (m_nodeNamesIdx.size() < 8);
  // Compute shader does not use lds
  for (auto &global : m_module->globals()) {
    if (global.getAddressSpace() == SPIRAS_Local) {
      threadLaunch = false;
      break;
    }
  }
  return threadLaunch;
}

// =====================================================================================================================
// Create Lds memory for the output graph nodes

// @param outputCount : Number of node output
void LowerExecutionGraph::createGraphLds(unsigned outputCount) {
  if (m_graphLds == nullptr) {
    // - base_wptr_transfer
    // - last_group_transfer
    // - allocation_counts[num_outputs]
    auto ldsSize = outputCount + 2;
    auto ldsTy = ArrayType::get(m_builder->getInt32Ty(), ldsSize);
    m_graphLds = new GlobalVariable(*m_module, ldsTy, false, GlobalValue::ExternalLinkage, nullptr, "GraphLds", nullptr,
                                    GlobalValue::NotThreadLocal, SPIRAS_Local);
  }
}

// =====================================================================================================================
// Create OutputCountOp used for the execution graph library
//
// @param [in] inst : OutputCountOp to lower
void LowerExecutionGraph::visitOutputCount(wg::OutputCountOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto outputCount = m_builder->getInt32(m_nodeNamesIdx.size());
  inst.replaceAllUsesWith(outputCount);
}

// =====================================================================================================================
// Visit GraphgetLdsOp used for the execution graph library
//
// @param [in] inst : GraphGetLdsOp to lower
void LowerExecutionGraph::visitGraphGetLds(wg::GraphGetLdsOp &inst) {
  auto retTy = PointerType::get(m_builder->getInt32Ty(), SPIRAS_Local);
  m_builder->SetInsertPoint(&inst);
  assert(m_graphLds != nullptr);
  auto ldsPtr = m_builder->CreateGEP(m_builder->getInt32Ty(), m_graphLds, m_builder->getInt32(0));
  ldsPtr = m_builder->CreateBitCast(ldsPtr, retTy);
  inst.replaceAllUsesWith(ldsPtr);
}

} // namespace Llpc
