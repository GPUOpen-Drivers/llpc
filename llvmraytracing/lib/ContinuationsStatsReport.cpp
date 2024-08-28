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

//===- ContinuationsStatsReport.cpp - Continuations statistics reporting ------------------===//
//
// A pass that gets the following statistics from a continuations module:
//  * Report payload sizes
//  * Report system data sizes
//  * Report continuation state sizes
//
// This pass is designed to be ran after the cleanup passes, since this is
// where all required information for analysis is available.
// The metadata can be safely omitted after running this pass.
//===----------------------------------------------------------------------===//

#include "llvmraytracing/Continuations.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include <optional>

using namespace llvm;
using namespace lgc::rt;

#define DEBUG_TYPE "continuations-stats-report"

enum class PayloadRegisterSizeReportingMode : uint8_t { Disabled = 0, MaxOutgoing, ByJump };

static cl::opt<bool> ReportContStateSizes("report-cont-state-sizes",
                                          cl::desc("Report continuation state sizes for entry functions."),
                                          cl::init(false));

static cl::opt<PayloadRegisterSizeReportingMode> ReportPayloadRegisterSizes(
    "report-payload-register-sizes", cl::init(PayloadRegisterSizeReportingMode::Disabled),
    cl::desc("Report payload VGPR sizes for functions."),
    cl::values(clEnumValN(PayloadRegisterSizeReportingMode::Disabled, "disabled", "Disable payload size reporting"),
               clEnumValN(PayloadRegisterSizeReportingMode::MaxOutgoing, "max",
                          "Report incoming and maximum outgoing payload sizes"),
               clEnumValN(PayloadRegisterSizeReportingMode::ByJump, "byjump",
                          "Reporting incoming register sizes and payload size for each jump")));

static cl::opt<bool> ReportSystemDataSizes("report-system-data-sizes",
                                           cl::desc("Report incoming system data sizes for functions."),
                                           cl::init(false));

static cl::opt<bool> ReportAllSizes("report-all-continuation-sizes",
                                    cl::desc("Report continuation state, payload and system data sizes."),
                                    cl::init(false));

namespace {
class ContinuationsStatsReportPassImpl final {
public:
  ContinuationsStatsReportPassImpl(Module &M);
  void run();

private:
  void collectProcessableFunctions();
  void reportContStateSizes();
  void reportPayloadRegisterSizes();
  void reportSystemDataSizes();

  struct FunctionData {
    std::optional<RayTracingShaderStage> Stage = std::nullopt;
    Type *SystemDataTy = nullptr;
  };

  Module &Mod;
  MapVector<Function *, FunctionData> ToProcess;
};
} // namespace

ContinuationsStatsReportPassImpl::ContinuationsStatsReportPassImpl(Module &Mod) : Mod{Mod} {
}

void ContinuationsStatsReportPassImpl::run() {
  if (ReportPayloadRegisterSizes == PayloadRegisterSizeReportingMode::Disabled && !ReportSystemDataSizes &&
      !ReportContStateSizes && !ReportAllSizes)
    return;

  collectProcessableFunctions();

  if (ReportAllSizes || ReportPayloadRegisterSizes != PayloadRegisterSizeReportingMode::Disabled)
    reportPayloadRegisterSizes();

  if (ReportAllSizes || ReportSystemDataSizes)
    reportSystemDataSizes();

  if (ReportAllSizes || ReportContStateSizes)
    reportContStateSizes();
}

void ContinuationsStatsReportPassImpl::collectProcessableFunctions() {
  for (Function &F : Mod) {
    if (F.isDeclaration())
      continue;

    auto Stage = getLgcRtShaderStage(&F);
    if (!Stage || Stage == RayTracingShaderStage::KernelEntry)
      continue;

    if (!llvm::isStartFunc(&F)) {
      FunctionData Data;
      Data.Stage = Stage;

      // Extract the actual system data type from the { systemData, padding,
      // payload } struct returned by await.
      Data.SystemDataTy = F.getArg(F.arg_size() - 1)->getType()->getStructElementType(0);

      [[maybe_unused]] bool DidInsert = ToProcess.insert({&F, std::move(Data)}).second;
      assert(DidInsert);

      continue;
    }

    const uint32_t SystemDataArgumentIndex = lgc::cps::isCpsFunction(F) ? CpsArgIdxSystemData : 1;
    switch (Stage.value()) {
    case RayTracingShaderStage::RayGeneration:
    case RayTracingShaderStage::Intersection:
    case RayTracingShaderStage::AnyHit:
    case RayTracingShaderStage::ClosestHit:
    case RayTracingShaderStage::Miss:
    case RayTracingShaderStage::Callable:
    case RayTracingShaderStage::Traversal: {
      FunctionData Data;
      Data.Stage = Stage;
      Data.SystemDataTy = F.getFunctionType()->getParamType(SystemDataArgumentIndex);
      assert(Data.SystemDataTy->isStructTy() && "SystemData should be of struct type!");

      [[maybe_unused]] bool DidInsert = ToProcess.insert({&F, std::move(Data)}).second;
      assert(DidInsert);
      break;
    }
    default:
      break;
    }
  }
}

void ContinuationsStatsReportPassImpl::reportContStateSizes() {
  for (auto &[Func, FuncData] : ToProcess) {
    auto OptStateSize = ContHelper::ContinuationStateByteCount::tryGetValue(Func);
    if (!OptStateSize.has_value())
      continue;

    dbgs() << "Continuation state size of \"" << Func->getName() << "\" (" << FuncData.Stage
           << "): " << OptStateSize.value() << " bytes\n";
  }
}

void ContinuationsStatsReportPassImpl::reportPayloadRegisterSizes() {
  using FuncJumpMapTy = DenseMap<Function *, SmallVector<std::pair<lgc::cps::JumpOp *, uint32_t>>>;

  static const auto Visitor = llvm_dialects::VisitorBuilder<FuncJumpMapTy>()
                                  .add<lgc::cps::JumpOp>([](FuncJumpMapTy &ByJumpRegisterCounts, auto &CInst) {
                                    auto RegCount = ContHelper::OutgoingRegisterCount::tryGetValue(&CInst).value();
                                    ByJumpRegisterCounts[CInst.getFunction()].push_back({&CInst, RegCount});
                                  })
                                  .build();

  FuncJumpMapTy ByJumpRegisterCounts;
  Visitor.visit(ByJumpRegisterCounts, Mod);

  DenseMap<Function *, uint32_t> MaxOutgoingRegisterCounts;
  if (ReportPayloadRegisterSizes == PayloadRegisterSizeReportingMode::MaxOutgoing) {
    // Accumulate all outgoing payload sizes per function.
    for (auto &[Func, Jumps] : ByJumpRegisterCounts) {
      for (auto &[Jump, RegCount] : Jumps) {
        MaxOutgoingRegisterCounts[Func] = std::max(MaxOutgoingRegisterCounts[Func], RegCount);
      }
    }
  }

  const StringRef SizeSuffix = " dwords";
  const auto ReportIncomingPayload = [&](Function &Func, std::optional<uint32_t> OptIncomingPayloadRegisterCount,
                                         DXILShaderKind ShaderKind, StringRef ReportSuffix, bool AppendSizeSuffix) {
    dbgs() << ReportSuffix << " \"" << Func.getName() << "\" (" << ShaderKind << "): ";
    if (OptIncomingPayloadRegisterCount.has_value()) {
      dbgs() << OptIncomingPayloadRegisterCount.value();
      if (AppendSizeSuffix)
        dbgs() << SizeSuffix;
    } else {
      dbgs() << "(no incoming payload)";
    }
  };

  for (auto &[Func, FuncData] : ToProcess) {
    DXILShaderKind ShaderKind = ShaderStageHelper::rtShaderStageToDxilShaderKind(FuncData.Stage.value());
    auto OptIncomingPayloadRegisterCount = ContHelper::IncomingRegisterCount::tryGetValue(Func);
    bool HasIncomingPayload = OptIncomingPayloadRegisterCount.has_value();

    if (ReportPayloadRegisterSizes == PayloadRegisterSizeReportingMode::ByJump) {
      auto It = ByJumpRegisterCounts.find(Func);
      bool HasOutgoingPayload = (It != ByJumpRegisterCounts.end());

      if (!HasIncomingPayload && !HasOutgoingPayload)
        continue;

      ReportIncomingPayload(*Func, OptIncomingPayloadRegisterCount, ShaderKind, "Incoming payload VGPR size of", true);
      dbgs() << "\n";

      if (HasOutgoingPayload) {
        dbgs() << "Outgoing payload VGPR size by jump:\n";
        for (auto &[Jump, RegCount] : It->second)
          dbgs() << *Jump << ": " << RegCount << SizeSuffix << '\n';
      }
    } else {
      auto It = MaxOutgoingRegisterCounts.find(Func);
      bool HasOutgoingPayload = (It != MaxOutgoingRegisterCounts.end());

      if (!HasIncomingPayload && !HasOutgoingPayload)
        continue;

      ReportIncomingPayload(*Func, OptIncomingPayloadRegisterCount, ShaderKind,
                            "Incoming and max outgoing payload VGPR size of", false);
      dbgs() << " and ";
      if (HasOutgoingPayload) {
        dbgs() << It->second;
      } else {
        dbgs() << "(no outgoing payload)";
      }
      dbgs() << SizeSuffix << '\n';
    }
  }
}

void ContinuationsStatsReportPassImpl::reportSystemDataSizes() {
  for (const auto &[F, FuncData] : ToProcess) {
    if (FuncData.SystemDataTy == nullptr)
      continue;
    auto SystemDataBytes = Mod.getDataLayout().getTypeStoreSize(FuncData.SystemDataTy);

    dbgs() << "Incoming system data of \"" << F->getName() << "\" (" << FuncData.Stage << ") is \""
           << FuncData.SystemDataTy->getStructName() << "\", size:  " << SystemDataBytes << " bytes\n";
  }
}

PreservedAnalyses ContinuationsStatsReportPass::run(Module &Mod, ModuleAnalysisManager &AnalysisManager) {
  ContinuationsStatsReportPassImpl Impl{Mod};
  Impl.run();
  return PreservedAnalyses::all();
}
