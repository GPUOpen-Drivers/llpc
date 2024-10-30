/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "ValueSpecializationTestPass.h"
#include "compilerutils/CompilerUtils.h"
#include "compilerutils/ValueOriginTracking.h"
#include "compilerutils/ValueSpecialization.h"
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace CompilerUtils;

namespace {

enum TestFlags { SkipValueTrackingCheck = 0x1, AllowFailure = 0x2, Invalid = 0x4 };

struct ValueSpecializationInfo {
  llvm::Value *Val;
  llvm::SmallVector<ValueSpecializer::DwordSpecializationInfo> DwordInfos;
  unsigned NumToBeReplacedDwords = 0;
  TestFlags Flags;
};

// Syntax:
//   call @specialize(i32 %flags, <ty> %val, i32 dw0Status, i32 dw0Constant, [i32 dw1Status, i32 dw1Constant, ...])
ValueSpecializationInfo parseSpecializeCall(llvm::CallInst &CI) {
  unsigned NumArgs = CI.arg_size();
  if (NumArgs % 2 != 0)
    report_fatal_error("Unexpected num args for specialize");
  unsigned NumDwords = (NumArgs - 2) / 2;

  llvm::SmallVector<ValueSpecializer::DwordSpecializationInfo> DwordInfos;
  unsigned NumReplacedDwords = 0;
  DwordInfos.reserve(NumDwords);
  for (unsigned DwordIdx = 0; DwordIdx < NumDwords; ++DwordIdx) {
    llvm::Value *KindValue = CI.getArgOperand(2 + 2 * DwordIdx);
    if (!isa<ConstantInt>(KindValue))
      report_fatal_error("Unexpected non-integer kind argument");
    auto KindInt = cast<ConstantInt>(KindValue)->getZExtValue();
    if (KindInt >= static_cast<uint32_t>(ValueSpecializer::SpecializationKind::Count))
      report_fatal_error("Invalid specialization kind");
    auto Kind = static_cast<ValueSpecializer::SpecializationKind>(KindInt);
    uint32_t Constant = 0;
    if (Kind == ValueSpecializer::SpecializationKind::Constant) {
      llvm::Value *ConstantValueValue = CI.getArgOperand(2 + 2 * DwordIdx + 1);
      if (!isa<ConstantInt>(ConstantValueValue))
        report_fatal_error("Unexpected non-integer constant value argument");
      auto ConstantValueInt = cast<ConstantInt>(ConstantValueValue)->getZExtValue();
      if (ConstantValueInt >= UINT32_MAX)
        report_fatal_error("Too large constant value");
      Constant = static_cast<uint32_t>(ConstantValueInt);
    }
    DwordInfos.push_back({Kind, Constant});
    if (Kind != ValueSpecializer::SpecializationKind::None)
      ++NumReplacedDwords;
  }
  Value *TestFlagsValue = CI.getArgOperand(0);
  if (!isa<ConstantInt>(TestFlagsValue))
    report_fatal_error("Unexpected non-integer constant value argument");
  auto TestFlagsInt = cast<ConstantInt>(TestFlagsValue)->getZExtValue();
  if (TestFlagsInt >= static_cast<uint64_t>(TestFlags::Invalid))
    report_fatal_error("Invalid test flags value");
  return {CI.getArgOperand(1), DwordInfos, NumReplacedDwords, static_cast<TestFlags>(TestFlagsInt)};
}

} // namespace

namespace CompilerUtils {

llvm::PreservedAnalyses ValueSpecializationTestPass::run(llvm::Module &Module,
                                                         llvm::ModuleAnalysisManager &AnalysisManager) {
  Function *SpecializeFunc = Module.getFunction("specialize");
  if (!SpecializeFunc)
    return PreservedAnalyses::all();

  // First collect all specialization requests grouped by BB (because we re-use the specializer per BB),
  // then remove all specialization intrinsic calls, then do the actual specialization.
  struct BBInfo {
    SmallVector<ValueSpecializationInfo> SpecializationRequests;
  };
  SmallVector<BBInfo> SpecializationRequestsByBB;

  SmallVector<CallInst *> ToBeDeleted;

  for (auto &F : Module) {
    for (auto &BB : F) {
      SpecializationRequestsByBB.push_back({});
      for (auto &Inst : BB) {
        auto *CI = dyn_cast<CallInst>(&Inst);
        if (!CI || CI->getCalledOperand() != SpecializeFunc) {
          continue;
        }
        ToBeDeleted.push_back(CI);
        SpecializationRequestsByBB.back().SpecializationRequests.push_back(parseSpecializeCall(*CI));
      }
    }
  }

  for (auto *CI : ToBeDeleted)
    CI->eraseFromParent();

  for (const auto &BBInfo : SpecializationRequestsByBB) {
    // Use one specializer per BB, and re-use insertion points.
    ValueSpecializer VS(Module);

    for (const auto &VSI : BBInfo.SpecializationRequests) {
      bool ReplaceUses = true;
      bool PreserveInsertionPoint = true;
      const auto [Replacement, NumReplacedDwords] =
          VS.replaceDwords(VSI.Val, VSI.DwordInfos, ReplaceUses, PreserveInsertionPoint);

      if (!(VSI.Flags & TestFlags::AllowFailure) && NumReplacedDwords != VSI.NumToBeReplacedDwords)
        report_fatal_error("Less than expected replacements");
      if (NumReplacedDwords != 0 && Replacement == nullptr)
        report_fatal_error("Missing replacement result");

      if (Replacement && !(VSI.Flags & TestFlags::SkipValueTrackingCheck)) {
        // Run value tracking analysis on the replacement result, and check that it matches the requested replacements
        // Use Forward freeze handling mode. This is the most relaxed one and helps avoiding mismatches
        // caused by conservative analysis of freeze.
        ValueOriginTracker::Options Opts{};
        Opts.FreezeMode = ValueOriginTracker::Options::FreezeHandlingMode::Forward;
        Opts.MaxBytesPerValue = 256;
        Opts.BytesPerSlice = 4;
        ValueOriginTracker VOT{Module.getDataLayout(), Opts};
        const ValueTracking::ValueInfo VI = VOT.getValueInfo(Replacement);
        if (VI.Slices.size() != VSI.DwordInfos.size())
          report_fatal_error("Size mismatch");
        for (unsigned DwordIdx = 0; DwordIdx < VI.Slices.size(); ++DwordIdx) {
          const ValueTracking::SliceInfo &SI = VI.Slices[DwordIdx];
          const ValueSpecializer::DwordSpecializationInfo &DSI = VSI.DwordInfos[DwordIdx];
          if (DSI.Kind == ValueSpecializer::SpecializationKind::Constant) {
            if (SI.Status != ValueTracking::SliceStatus::Constant || SI.ConstantValue != DSI.ConstantValue)
              report_fatal_error("Failed constant specialization");
          }
          if (DSI.Kind == ValueSpecializer::SpecializationKind::FrozenPoison) {
            if (SI.Status != ValueTracking::SliceStatus::UndefOrPoison) {
              report_fatal_error("Failed frozen poison specialization");
            }
          }
        }
      }

      dbgs() << "[VS]: Replaced " << NumReplacedDwords << " dwords in ";
      VSI.Val->printAsOperand(dbgs());
      if (Replacement) {
        dbgs() << ", replaced by ";
        Replacement->printAsOperand(dbgs());
      }
      dbgs() << "\n";
    }
  }

  return PreservedAnalyses::none();
}

} // namespace CompilerUtils
