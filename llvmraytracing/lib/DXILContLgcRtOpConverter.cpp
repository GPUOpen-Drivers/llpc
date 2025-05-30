/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- DXILContLgcRtOpConverter.cpp - Convert DXIL to lgc.rt dialect -===//
//
// A pass that converts dx.op call instructions to lgc.rt dialect operations.
//
//===----------------------------------------------------------------------===//

#include "DXILEnums.h"
#include "llvmraytracing/Continuations.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <map>
#include <type_traits>

#define DEBUG_TYPE "dxil-cont-lgc-rt-op-converter"

namespace {

using namespace llvm;
using namespace llvmraytracing;

static void analyzeShaderKinds(Module &M, MapVector<Function *, DXILShaderKind> &ShaderKinds) {
  auto *EntryPoints = M.getNamedMetadata("dx.entryPoints");
  if (!EntryPoints)
    return;
  for (auto *EntryMD : EntryPoints->operands()) {
    auto *C = mdconst::extract_or_null<Constant>(EntryMD->getOperand(0));
    // Strip bitcasts
    while (auto *Expr = dyn_cast_or_null<ConstantExpr>(C)) {
      if (Expr->getOpcode() == Instruction::BitCast)
        C = Expr->getOperand(0);
      else
        C = nullptr;
    }
    auto *F = extractFunctionOrNull(EntryMD->getOperand(0));
    if (!F)
      continue;
    auto *Props = cast_or_null<MDTuple>(EntryMD->getOperand(4));
    if (!Props)
      continue;

    // Iterate through tag-value pairs
    for (size_t I = 0; I < Props->getNumOperands(); I += 2) {
      auto Tag = mdconst::extract<ConstantInt>(Props->getOperand(I))->getZExtValue();
      if (Tag != 8) // kDxilShaderKindTag
        continue;
      auto KindI = mdconst::extract<ConstantInt>(Props->getOperand(I + 1))->getZExtValue();
      auto Kind = static_cast<DXILShaderKind>(KindI);
      ShaderKinds[F] = Kind;
    }
  }
}

class DXILContLgcRtOpConverterPassImpl final {
public:
  DXILContLgcRtOpConverterPassImpl(Module &Module);
  llvm::PreservedAnalyses run();

private:
  llvm_dialects::Builder Builder;
  Module &M;
  const llvm::DataLayout &DL;

  bool convertDxOp(llvm::Function &Func);
  using OpCallbackType = std::function<llvm::Value *(llvm::CallInst &, DXILContLgcRtOpConverterPassImpl *)>;
  std::optional<OpCallbackType> getCallbackByOpName(StringRef OpName);

  template <typename T> Value *handleSimpleCall(CallInst &CI);
  template <typename T> Value *handleSimpleCallHitObj(CallInst &CI);
  Value *handleTraceRayOp(CallInst &CI);
  Value *handleReportHitOp(CallInst &CI);
  Value *handleCallShaderOp(CallInst &CI);
  Value *getArgIndexOfArrayOperand(CallInst &CI, unsigned MaxElements);
  template <typename T, unsigned MaxElements = 3> Value *handleVecResult(CallInst &CI);
  template <typename T, unsigned MaxElements = 3> Value *handleVecResultHitObj(CallInst &CI);
  template <typename Op, unsigned MaxRows = 3, unsigned MaxColumns = 4> Value *handleMatrixResult(CallInst &CI);
  Value *createVec3(Value *X, Value *Y, Value *Z);
  void addDXILPayloadTypeToCall(Function &DXILFunc, CallInst &CI);
  bool prepareEntryPointShaders();
  void setupLocalRootIndex(Function *F);
};

// Get the corresponding callback index in the callback table.
std::optional<DXILContLgcRtOpConverterPassImpl::OpCallbackType>
DXILContLgcRtOpConverterPassImpl::getCallbackByOpName(StringRef OpName) {
  using namespace lgc::rt;
#define LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK(Op, Callback)                                                           \
  if (OpName.starts_with(Op))                                                                                          \
    return std::bind(&DXILContLgcRtOpConverterPassImpl::Callback, this, std::placeholders::_1);

  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("acceptHitAndEndSearch", handleSimpleCall<AcceptHitAndEndSearchOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("ignoreHit", handleSimpleCall<IgnoreHitOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("instanceID", handleSimpleCallHitObj<InstanceIdOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("instanceIndex", handleSimpleCallHitObj<InstanceIndexOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("geometryIndex", handleSimpleCallHitObj<GeometryIndexOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("hitKind", handleSimpleCallHitObj<HitKindOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("primitiveIndex", handleSimpleCallHitObj<PrimitiveIndexOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("rayFlags", handleSimpleCallHitObj<RayFlagsOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("rayTMin", handleSimpleCallHitObj<RayTminOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("rayTCurrent", handleSimpleCallHitObj<RayTcurrentOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("objectRayDirection", handleVecResultHitObj<ObjectRayDirectionOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("objectRayOrigin", handleVecResultHitObj<ObjectRayOriginOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("dispatchRaysDimensions", handleVecResult<DispatchRaysDimensionsOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("dispatchRaysIndex", handleVecResult<DispatchRaysIndexOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("worldRayDirection", handleVecResultHitObj<WorldRayDirectionOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("worldRayOrigin", handleVecResultHitObj<WorldRayOriginOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("objectToWorld", handleMatrixResult<ObjectToWorldOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("worldToObject", handleMatrixResult<WorldToObjectOp>)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("traceRay", handleTraceRayOp)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("reportHit", handleReportHitOp)
  LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK("callShader", handleCallShaderOp)

#undef LGC_RT_CALLBACK_TABLE_TRY_GET_CALLBACK

  return std::nullopt;
}

/// Handle a simple call without any arguments, replace the uses with the new
/// op.
template <typename Op> Value *DXILContLgcRtOpConverterPassImpl::handleSimpleCall(CallInst &CI) {
  static_assert(std::is_base_of<llvm::CallInst, Op>());

  Builder.SetInsertPoint(&CI);
  return Builder.create<Op>();
}

/// Handle a simple call without any arguments, replace the uses with the new
/// op.
template <typename Op> Value *DXILContLgcRtOpConverterPassImpl::handleSimpleCallHitObj(CallInst &CI) {
  static_assert(std::is_base_of<llvm::CallInst, Op>());

  Builder.SetInsertPoint(&CI);

  Type *allocaPtrTy = CI.getDataLayout().getAllocaPtrType(Builder.getContext());
  auto *globalHitObjectOp = Builder.create<lgc::rt::GlobalHitObjectOp>(allocaPtrTy);
  return Builder.create<Op>(globalHitObjectOp);
}

/// Create a lgc.rt.trace.ray op from a dx.op.traceRay call.
Value *DXILContLgcRtOpConverterPassImpl::handleTraceRayOp(CallInst &CI) {
  assert(CI.arg_size() == static_cast<unsigned>(TraceRayArgIndex::Count) && "Invalid argument size!");

  Builder.SetInsertPoint(&CI);

  Value *AccelStructHandle = CI.getArgOperand(TraceRayArgIndex::AccelStruct);
  Value *RayFlags = CI.getArgOperand(TraceRayArgIndex::RayFlags);
  Value *InstanceInclusionMask = CI.getArgOperand(TraceRayArgIndex::InstanceInclusionMask);
  Value *RayContributionToHitGroupIndex = CI.getArgOperand(TraceRayArgIndex::RayContributionToHitGroupIndex);
  Value *MultiplierForGeometryContribution = CI.getArgOperand(TraceRayArgIndex::MultiplierForGeometryContribution);
  Value *MissShaderIndex = CI.getArgOperand(TraceRayArgIndex::MissShaderIndex);
  Value *Origin = createVec3(CI.getArgOperand(TraceRayArgIndex::OriginX), CI.getArgOperand(TraceRayArgIndex::OriginY),
                             CI.getArgOperand(TraceRayArgIndex::OriginZ));
  Value *TMin = CI.getArgOperand(TraceRayArgIndex::TMin);
  Value *Dir = createVec3(CI.getArgOperand(TraceRayArgIndex::DirX), CI.getArgOperand(TraceRayArgIndex::DirY),
                          CI.getArgOperand(TraceRayArgIndex::DirZ));
  Value *TMax = CI.getArgOperand(TraceRayArgIndex::TMax);
  Value *Payload = CI.getArgOperand(TraceRayArgIndex::Payload);

  Function *AccelStructGetter = getAccelStructAddr(*CI.getModule(), AccelStructHandle->getType());
  Value *AccelStructAddr = Builder.CreateCall(AccelStructGetter, AccelStructHandle);

  // TODO: This only creates a Paq array with the size of the payload data for
  // now.
  Type *PaqTy = getFuncArgPtrElementType(CI.getCalledFunction(), static_cast<int>(TraceRayArgIndex::Payload));
  SmallVector<Constant *, 1> PaqArgs;
  if (PaqTy)
    PaqArgs.push_back(ConstantInt::get(Builder.getInt32Ty(), DL.getTypeAllocSize(PaqTy).getKnownMinValue()));

  Constant *PaqArr = ConstantArray::get(ArrayType::get(Builder.getInt32Ty(), 1), PaqArgs);

  auto *Op = Builder.create<lgc::rt::TraceRayOp>(AccelStructAddr, RayFlags, InstanceInclusionMask,
                                                 RayContributionToHitGroupIndex, MultiplierForGeometryContribution,
                                                 MissShaderIndex, Origin, TMin, Dir, TMax, Payload, PaqArr);

  addDXILPayloadTypeToCall(*CI.getCalledFunction(), *Op);

  return Op;
}

/// Create a lgc.rt.report.hit op from a dx.op.reportHit call.
Value *DXILContLgcRtOpConverterPassImpl::handleReportHitOp(CallInst &CI) {
  assert(CI.arg_size() == static_cast<unsigned>(ReportHitArgIndex::Count) && "Invalid argument size!");

  Builder.SetInsertPoint(&CI);
  Value *THit = CI.getArgOperand(ReportHitArgIndex::THit);
  Value *HitKind = CI.getArgOperand(ReportHitArgIndex::HitKind);
  Value *Attributes = CI.getArgOperand(ReportHitArgIndex::Attributes);
  auto AttributeSizeBytes = DL.getTypeAllocSize(
      getFuncArgPtrElementType(CI.getCalledFunction(), static_cast<int>(ReportHitArgIndex::Attributes)));

  auto *Op = Builder.create<lgc::rt::ReportHitOp>(THit, HitKind, Attributes, AttributeSizeBytes);

  addDXILPayloadTypeToCall(*CI.getCalledFunction(), *Op);

  return Op;
}

/// Create a lgc.rt.call.callable.shader op from a dx.op.callShader call.
Value *DXILContLgcRtOpConverterPassImpl::handleCallShaderOp(CallInst &CI) {
  assert(CI.arg_size() == static_cast<unsigned>(CallShaderArgIndex::Count) && "Invalid argument size!");

  Builder.SetInsertPoint(&CI);
  Value *ShaderIndex = CI.getArgOperand(CallShaderArgIndex::ShaderIndex);
  Value *Param = CI.getArgOperand(CallShaderArgIndex::Param);

  auto ParamSizeBytes = DL.getTypeAllocSize(
      getFuncArgPtrElementType(CI.getCalledFunction(), static_cast<int>(CallShaderArgIndex::Param)));

  auto *Op = Builder.create<lgc::rt::CallCallableShaderOp>(ShaderIndex, Param, ParamSizeBytes.getKnownMinValue());

  addDXILPayloadTypeToCall(*CI.getCalledFunction(), *Op);

  return Op;
}

/// Return Index of the array operand of the call instruction.
Value *DXILContLgcRtOpConverterPassImpl::getArgIndexOfArrayOperand(CallInst &CI, unsigned MaxElements) {
  constexpr int ArrayIndexArgPosition = 1;
  assert(CI.getNumOperands() > ArrayIndexArgPosition && "Invalid number of operands!");

  Value *Index = CI.getOperand(ArrayIndexArgPosition);
  if (!Index) {
    report_fatal_error("DXILContLgcRtOpConverterPassImpl::getArgIndexOfArrayOperand: Invalid operand index "
                       "at position " +
                       Twine(ArrayIndexArgPosition));
  }

  if (auto *Constant = dyn_cast<ConstantInt>(Index)) {
    unsigned ElementIndex = Constant->getZExtValue();
    if (ElementIndex >= MaxElements) {
      report_fatal_error("DXILContLgcRtOpConverterPassImpl::getArgIndexOfArrayOperand: "
                         "Operand at position " +
                         Twine(ArrayIndexArgPosition) + " is out of bounds (max: " + Twine(MaxElements) + ")!");
    }
  }
  return Index;
}

/// Helper to convert single-value operations from DXIL to vector return type
/// operations from the lgc.rt dialect:
/// %val = call dx.op(..., arrayIndex)
/// will be converted to the following
/// sequence:
/// %val = call lgc.rt.op(...)
/// %extract.index = extractelement %val, arrayIndex
template <typename Op, unsigned MaxElements> Value *DXILContLgcRtOpConverterPassImpl::handleVecResult(CallInst &CI) {
  static_assert(std::is_base_of<llvm::CallInst, Op>());

  Value *Index = getArgIndexOfArrayOperand(CI, MaxElements);
  Builder.SetInsertPoint(&CI);
  Value *DialectOp = Builder.create<Op>();
  return Builder.CreateExtractElement(DialectOp, Index, DialectOp->getName() + "extract");
}

/// Helper to convert single-value operations from DXIL to vector return type
/// operations from the lgc.rt dialect:
/// %val = call dx.op(..., arrayIndex)
/// will be converted to the following
/// sequence:
/// %val = call lgc.rt.op(HitObject)
/// %extract.index = extractelement %val, arrayIndex
template <typename Op, unsigned MaxElements>
Value *DXILContLgcRtOpConverterPassImpl::handleVecResultHitObj(CallInst &CI) {
  static_assert(std::is_base_of<llvm::CallInst, Op>());

  Value *Index = getArgIndexOfArrayOperand(CI, MaxElements);
  Builder.SetInsertPoint(&CI);
  Type *allocaPtrTy = CI.getDataLayout().getAllocaPtrType(Builder.getContext());
  auto *globalHitObjectOp = Builder.create<lgc::rt::GlobalHitObjectOp>(allocaPtrTy);
  Value *DialectOp = Builder.create<Op>(globalHitObjectOp);
  return Builder.CreateExtractElement(DialectOp, Index, DialectOp->getName() + "extract");
}

/// Helper to convert single-value matrix operations from DXIL to matrix return
/// type operations from the lgc.rt dialect:
/// In DXIL, those access 3x4 matrices, while in the lgc.rt dialect
/// the operations access 4x3 matrices.
/// %val = call dx.op(..., row, column)
/// will be converted to the following sequence:
/// %alloca = alloca [4 x <3 x type>]
/// %val = call [4 x <3 x type>] lgc.rt.op(...)
/// store %alloca, %val
/// %col.gep = getelementptr [4 x <3 x type>] %alloca, 0, %col
/// %col.gep.load = load <3 x type>, %col.gep
/// %row.index = extractelement type %row.gep.load, col
template <typename Op, unsigned MaxRows, unsigned MaxColumns>
Value *DXILContLgcRtOpConverterPassImpl::handleMatrixResult(CallInst &CI) {
  static_assert(std::is_base_of<llvm::CallInst, Op>());

  constexpr unsigned RowArgumentIndex = 1;
  constexpr unsigned ColumnArgumentIndex = 2;

  assert(CI.getNumOperands() > std::max(ColumnArgumentIndex, RowArgumentIndex) && "Invalid number of operands!");

  auto TryExtractIndexOperand = [&](unsigned ArgumentIndex, unsigned UpperBound) -> Value * {
    Value *Index = CI.getOperand(ArgumentIndex);
    if (!Index) {
      report_fatal_error("DXILContLgcRtOpConverterPassImpl::handleMatrixResult: "
                         "Invalid operand index "
                         "at position " +
                         Twine(ArgumentIndex));
    }

    if (auto *Constant = dyn_cast<ConstantInt>(Index)) {
      unsigned ConstantIndex = Constant->getZExtValue();
      if (ConstantIndex >= UpperBound) {
        report_fatal_error("DXILContLgcRtOpConverterPassImpl::handleMatrixResult: "
                           "Operand with value " +
                           Twine(ConstantIndex) + " is out of bounds (upper bound: " + Twine(UpperBound) +
                           ", xMax, yMax = (" + Twine(MaxColumns) + ", " + Twine(MaxRows) + "))!");
      }
    }

    return Index;
  };

  auto *Row = TryExtractIndexOperand(RowArgumentIndex, MaxRows);
  auto *Column = TryExtractIndexOperand(ColumnArgumentIndex, MaxColumns);

  Builder.SetInsertPoint(&CI);
  Type *allocaPtrTy = CI.getDataLayout().getAllocaPtrType(Builder.getContext());
  auto *globalHitObjectOp = Builder.create<lgc::rt::GlobalHitObjectOp>(allocaPtrTy);
  auto *DialectOp = Builder.create<Op>(globalHitObjectOp);
  AllocaInst *Alloca = nullptr;

  {
    IRBuilder<>::InsertPointGuard Guard(Builder);
    Builder.SetInsertPoint(&*CI.getFunction()->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
    Alloca = Builder.CreateAlloca(DialectOp->getType());
  }

  Builder.CreateStore(DialectOp, Alloca);

  Value *InnerVecGEP = Builder.CreateGEP(DialectOp->getType(), Alloca, {Builder.getInt32(0), Column}, "col.gep");
  Value *InnerVecLoad = Builder.CreateLoad(DialectOp->getType()->getArrayElementType(), InnerVecGEP, "col.gep.load");
  return Builder.CreateExtractElement(InnerVecLoad, Row, InnerVecLoad->getName() + ".row");
}

/// Helper to create a vec3 from three elements.
Value *DXILContLgcRtOpConverterPassImpl::createVec3(Value *X, Value *Y, Value *Z) {
  assert(X->getType() == Y->getType() && "DXILContLgcRtOpConverterPassImpl::createVec3: Invalid types for X and Y!");
  assert(X->getType() == Z->getType() && "DXILContLgcRtOpConverterPassImpl::createVec3: Invalid types for X and Z!");

  auto *Vec = Builder.CreateInsertElement(FixedVectorType::get(X->getType(), 3), X, static_cast<uint64_t>(0));
  Vec = Builder.CreateInsertElement(Vec, Y, 1);
  return Builder.CreateInsertElement(Vec, Z, 2);
}

/// Helper to add the type of the DXIL payload to the lgc.rt callsite if it does
/// not exist.
void DXILContLgcRtOpConverterPassImpl::addDXILPayloadTypeToCall(Function &DXILFunc, CallInst &CI) {
  // This should not happen theoretically.
  if (DXILFunc.arg_empty()) {
    report_fatal_error("DXILContLgcRtOpConverter::addDXILPayloadTypeToCall: DXIL "
                       "function " +
                       DXILFunc.getName() + " has no arguments.\n");
  }

  auto *PayloadPtr = DXILFunc.getArg(DXILFunc.arg_size() - 1);
  auto *PayloadPtrTy = TypedArgTy::get(PayloadPtr).getPointerElementType();

  // Store a poison value as metadata with the given type.
  ContHelper::setPayloadTypeMetadata(&CI, PayloadPtrTy);
}

bool DXILContLgcRtOpConverterPassImpl::convertDxOp(Function &Func) {
  auto FuncName = Func.getName();
  constexpr const char CalleePrefix[] = "dx.op.";
  if (!FuncName.starts_with(CalleePrefix))
    return false;

  StringRef OpName = FuncName.substr(std::strlen(CalleePrefix));
  assert(!OpName.empty() && "Invalid op name");

  LLVM_DEBUG(dbgs() << "DXILContLgcRtOpConverter: Handling operation dx.op." << OpName << '\n');

  // Try to find the corresponding callback by the OpName.
  auto Callback = getCallbackByOpName(OpName);
  if (Callback == std::nullopt) {
    return false;
  }

  bool Changed = false;
  for (Use &Use : make_early_inc_range(Func.uses())) {
    if (auto *CI = dyn_cast<CallInst>(Use.getUser())) {
      if (CI->isCallee(&Use)) {
        Value *NewOp = (*Callback)(*CI, this);

        if (!NewOp)
          report_fatal_error("DXILContLgcRtOpConverterPassImpl::visitFunction: unexpected "
                             "nullptr when trying to replace instruction!");

        if (CI->hasName())
          NewOp->takeName(CI);

        CI->replaceAllUsesWith(NewOp);
        CI->eraseFromParent();

        Changed = true;
      }
    }
  }

  return Changed;
}

void DXILContLgcRtOpConverterPassImpl::setupLocalRootIndex(Function *F) {
  Builder.SetInsertPointPastAllocas(F);
  auto *LocalIndex = Builder.create<lgc::rt::ShaderIndexOp>();
  Builder.create<lgc::ilcps::SetLocalRootIndexOp>(LocalIndex);
}

// Do preparation transformations to entry-point shaders.
bool DXILContLgcRtOpConverterPassImpl::prepareEntryPointShaders() {
  bool Changed = false;
  MapVector<Function *, DXILShaderKind> ShaderKinds;
  analyzeShaderKinds(M, ShaderKinds);

  for (auto &[Func, Kind] : ShaderKinds) {
    auto Stage = ShaderStageHelper::dxilShaderKindToRtShaderStage(Kind);

    // Ignore non-raytracing shader stages
    if (!Stage.has_value())
      continue;

    Changed = true;
    // Set lgc.rt shader stage metadata.
    lgc::rt::setLgcRtShaderStage(Func, Stage);
    // Set local root index in entry block.
    setupLocalRootIndex(Func);
    // Set payload type metadata.
    switch (Kind) {
    case DXILShaderKind::AnyHit:
    case DXILShaderKind::ClosestHit:
    case DXILShaderKind::Miss:
    case DXILShaderKind::Callable: {
      Type *PayloadTy = getFuncArgPtrElementType(Func, 0);
      assert(PayloadTy && "Shader must have a payload argument");
      Func->setMetadata(ContHelper::MDContPayloadTyName,
                        MDNode::get(Func->getContext(), {ConstantAsMetadata::get(PoisonValue::get(PayloadTy))}));
      break;
    }
    default:
      break;
    }
  }
  return Changed;
}

DXILContLgcRtOpConverterPassImpl::DXILContLgcRtOpConverterPassImpl(Module &Module)
    : Builder{Module.getContext()}, M{Module}, DL{M.getDataLayout()} {
}

PreservedAnalyses DXILContLgcRtOpConverterPassImpl::run() {
  bool Changed = false;

  Changed |= prepareEntryPointShaders();

  for (Function &F : M.functions()) {
    if (!F.isDeclaration())
      continue;

    Changed |= convertDxOp(F);
  }

  return Changed ? PreservedAnalyses::all() : PreservedAnalyses::none();
}

} // namespace

namespace llvm {

PreservedAnalyses DXILContLgcRtOpConverterPass::run(Module &Module, ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass dxil-cont-lgc-rt-op-converter\n");
  AnalysisManager.getResult<DialectContextAnalysis>(Module);

  DXILContLgcRtOpConverterPassImpl Impl(Module);
  return Impl.run();
}

} // namespace llvm
