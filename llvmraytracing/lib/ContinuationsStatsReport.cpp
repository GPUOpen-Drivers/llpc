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
#include <optional>

using namespace llvm;
using namespace lgc::rt;

#define DEBUG_TYPE "continuations-stats-report"

static cl::opt<bool> ReportContStateSizes("report-cont-state-sizes",
                                          cl::desc("Report continuation state sizes for entry functions."),
                                          cl::init(false));

static cl::opt<bool> ReportPayloadRegisterSizes("report-payload-register-sizes",
                                                cl::desc("Report payload VGPR sizes for functions."), cl::init(false));

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
  if (!ReportPayloadRegisterSizes && !ReportSystemDataSizes && !ReportContStateSizes && !ReportAllSizes)
    return;

  collectProcessableFunctions();

  if (ReportAllSizes || ReportPayloadRegisterSizes)
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
    case RayTracingShaderStage::Callable: {
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
  static const auto Visitor = llvm_dialects::VisitorBuilder<DenseMap<Function *, unsigned>>()
                                  .addSet<lgc::ilcps::ContinueOp, lgc::ilcps::WaitContinueOp, lgc::cps::JumpOp>(
                                      [](auto &FuncOutgoingRegCountMap, auto &CInst) {
                                        auto RegCount = ContHelper::OutgoingRegisterCount::tryGetValue(&CInst).value();
                                        FuncOutgoingRegCountMap[CInst.getFunction()] =
                                            std::max(FuncOutgoingRegCountMap[CInst.getFunction()], RegCount);
                                      })
                                  .build();

  DenseMap<Function *, unsigned> MaxOutgoingRegisterCounts;
  Visitor.visit(MaxOutgoingRegisterCounts, Mod);

  for (auto &[Func, FuncData] : ToProcess) {
    DXILShaderKind ShaderKind = ShaderStageHelper::rtShaderStageToDxilShaderKind(FuncData.Stage.value());
    auto OptIncomingPayloadRegisterCount = ContHelper::IncomingRegisterCount::tryGetValue(Func);
    bool HasIncomingPayload = OptIncomingPayloadRegisterCount.has_value();
    auto It = MaxOutgoingRegisterCounts.find(Func);
    bool HasOutgoingPayload = (It != MaxOutgoingRegisterCounts.end());

    if (!HasIncomingPayload && !HasOutgoingPayload)
      continue;

    dbgs() << "Incoming and max outgoing payload VGPR size of \"" << Func->getName() << "\" (" << ShaderKind << "): ";
    if (HasIncomingPayload) {
      dbgs() << OptIncomingPayloadRegisterCount.value() * RegisterBytes;
    } else {
      dbgs() << "(no incoming payload)";
    }
    dbgs() << " and ";
    if (HasOutgoingPayload) {
      dbgs() << It->second * RegisterBytes;
    } else {
      dbgs() << "(no outgoing payload)";
    }
    dbgs() << " bytes\n";
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
