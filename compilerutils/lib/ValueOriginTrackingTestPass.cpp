/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "ValueOriginTrackingTestPass.h"
#include "compilerutils/CompilerUtils.h"
#include "compilerutils/ValueOriginTracking.h"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace compilerutils;

namespace {

cl::opt<unsigned> BytesPerSliceOption("value-origin-tracking-test-bytes-per-slice", cl::init(4));
cl::opt<unsigned> MaxBytesPerValueOption("value-origin-tracking-test-max-bytes-per-value", cl::init(512));
cl::opt<unsigned>
    FreezeModeOption("value-origin-tracking-test-freeze-mode",
                     cl::init(static_cast<unsigned>(ValueOriginTracker::Options::FreezeHandlingMode::Dynamic)));

// Parse assumptions made via calls to the assume function.
ValueOriginTracker::ValueOriginAssumptions parseAssumptions(Module &Module, Function &AssumeFunc) {
  ValueOriginTracker::ValueOriginAssumptions Result;
  SmallVector<Instruction *> ToBeRemoved;
  forEachCall(AssumeFunc, [&](CallInst &AssumptionCall) {
    ToBeRemoved.push_back(&AssumptionCall);
    unsigned NumArgs = AssumptionCall.arg_size();
    // We expect one arg for the value, and two per slice.
    if (NumArgs % 2 != 1)
      report_fatal_error("unexpected number of assumption args");
    // The value we put an assumption on
    Value *V = AssumptionCall.getArgOperand(0);
    Instruction *Inst = dyn_cast<Instruction>(V);
    if (Inst == nullptr)
      report_fatal_error("assumptions are only allowed on instructions");
    ValueOriginTracker::ValueInfo Assumption{};
    unsigned NumSlices = (NumArgs - 1) / 2;
    for (unsigned SliceIdx = 0; SliceIdx < NumSlices; ++SliceIdx) {
      unsigned SliceArgBeginIdx = 1 + 2 * SliceIdx;
      Value *ReferencedValueOrConstant = AssumptionCall.getArgOperand(SliceArgBeginIdx);
      if (isa<UndefValue>(ReferencedValueOrConstant)) {
        Assumption.Slices.push_back({ValueTracking::SliceStatus::UndefOrPoison});
      } else if (auto *CIValue = dyn_cast<ConstantInt>(ReferencedValueOrConstant)) {
        ValueTracking::SliceInfo SI{ValueTracking::SliceStatus::Constant};
        if (!CIValue->getType()->isIntegerTy(32))
          report_fatal_error("expected i32 constant");
        SI.ConstantValue = CIValue->getZExtValue();
        Assumption.Slices.push_back(SI);
      } else {
        // Dynamic value reference
        ValueTracking::SliceInfo SI{ValueTracking::SliceStatus::Dynamic};
        SI.DynamicValue = ReferencedValueOrConstant;
        Value *DynamicValueByteOffsetValue = AssumptionCall.getArgOperand(SliceArgBeginIdx + 1);
        auto *DynamicValueByteOffsetValueCI = dyn_cast<ConstantInt>(DynamicValueByteOffsetValue);
        if (DynamicValueByteOffsetValueCI == nullptr || !DynamicValueByteOffsetValueCI->getType()->isIntegerTy(32))
          report_fatal_error("expected i32 constant");
        SI.DynamicValueByteOffset = DynamicValueByteOffsetValueCI->getZExtValue();
        Assumption.Slices.push_back(SI);
      }
    }
    bool Inserted = Result.insert({Inst, Assumption}).second;
    if (!Inserted)
      report_fatal_error("value with duplicate assumption");
  });
  // Ensure assume calls are removed before starting the analysis, ensuring they don't impact it.
  for (auto *Inst : ToBeRemoved)
    Inst->eraseFromParent();
  return Result;
}

} // namespace

namespace compilerutils {

llvm::PreservedAnalyses ValueOriginTrackingTestPass::run(llvm::Module &Module,
                                                         llvm::ModuleAnalysisManager &AnalysisManager) {
  Function *AnalyzeFunc = Module.getFunction("analyze");
  if (!AnalyzeFunc)
    return PreservedAnalyses::all();

  ValueOriginTracker::ValueOriginAssumptions Assumptions;
  Function *AssumeFunc = Module.getFunction("assume");
  if (AssumeFunc) {
    Assumptions = parseAssumptions(Module, *AssumeFunc);
  }

  ValueOriginTracker::Options Opts{};
  Opts.FreezeMode = static_cast<ValueOriginTracker::Options::FreezeHandlingMode>(FreezeModeOption.getValue());
  Opts.BytesPerSlice = BytesPerSliceOption.getValue();
  Opts.MaxBytesPerValue = MaxBytesPerValueOption.getValue();
  ValueOriginTracker VOT{Module.getDataLayout(), Opts, Assumptions};

  // Traverse all functions instead of the users of AnalyzeFunc to group output by function
  // First collect values to be analyzed, then remove analyze calls, and then do the actual analysis.
  // This ensures analysis calls don't interfere with the analysis, e.g. when freeze handling depends on the number of
  // users.
  SmallVector<Instruction *> ToBeRemoved;
  struct AnalyzeCallsInfo {
    SmallVector<Value *> Operands;
  };
  struct FunctionInfo {
    Function *F;
    SmallVector<AnalyzeCallsInfo> AnalyzeCalls;
  };
  SmallVector<FunctionInfo> ToBeAnalyzed;

  for (auto &F : Module) {
    if (F.isDeclaration())
      continue;
    ToBeAnalyzed.push_back({});
    auto &FuncInfo = ToBeAnalyzed.back();
    FuncInfo.F = &F;

    for (auto &BB : F) {
      for (auto &I : BB) {
        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI || CI->getCalledOperand() != AnalyzeFunc) {
          continue;
        }
        ToBeRemoved.push_back(CI);
        FuncInfo.AnalyzeCalls.push_back({});
        auto &AnalyzeInfo = FuncInfo.AnalyzeCalls.back();

        for (Value *Op : CI->data_ops()) {
          AnalyzeInfo.Operands.push_back(Op);
        }
      }
    }
  }

  for (auto *Inst : ToBeRemoved)
    Inst->eraseFromParent();

  auto Prefix = "[VOT]: ";
  for (const auto &FuncInfo : ToBeAnalyzed) {
    outs() << Prefix << FuncInfo.F->getName() << "\n";
    for (const auto &AnalyzeInfo : FuncInfo.AnalyzeCalls) {
      for (Value *Op : AnalyzeInfo.Operands) {
        auto VI = VOT.getValueInfo(Op);
        outs() << Prefix << "(" << *Op << "): " << VI << "\n";
      }
      outs() << "\n";
    }
  }
  return PreservedAnalyses::all();
}

} // namespace compilerutils
