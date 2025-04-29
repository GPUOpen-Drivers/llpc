/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- StatisticsReporter.cpp - Report statistics relevant to Continuations -------------------===//

#include "llvmraytracing/StatisticsReporter.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "lgc/LgcRtDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include <cstdint>

using namespace lgc::rt;
using namespace llvm;

namespace llvm {
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
} // namespace llvm

bool StatisticsReporter::shouldReport() const {
  return ReportPayloadRegisterSizes != PayloadRegisterSizeReportingMode::Disabled || ReportSystemDataSizes ||
         ReportContStateSizes || ReportAllSizes;
}

void StatisticsReporter::reportContStateSizes(const FunctionData &FuncData) {
  auto OptStateSize = ContHelper::ContinuationStateByteCount::tryGetValue(FuncData.Func);
  if (!OptStateSize.has_value())
    return;

  dbgs() << "Continuation state size of \"" << FuncData.Func->getName() << "\" (" << FuncData.Stage
         << "): " << OptStateSize.value() << " bytes\n";
}

void StatisticsReporter::reportPayloadRegisterSizes(const FunctionData &FuncData) {
  using JumpPayloadVecTy = SmallVector<std::pair<lgc::cps::JumpOp *, uint32_t>>;

  static const auto Visitor = llvm_dialects::VisitorBuilder<JumpPayloadVecTy>()
                                  .add<lgc::cps::JumpOp>([](JumpPayloadVecTy &JumpPayloadSizes, auto &CInst) {
                                    auto RegCount = ContHelper::OutgoingRegisterCount::tryGetValue(&CInst).value();
                                    JumpPayloadSizes.push_back({&CInst, RegCount});
                                  })
                                  .build();

  Function *F = FuncData.Func;
  JumpPayloadVecTy ByJumpRegisterCounts;
  Visitor.visit(ByJumpRegisterCounts, *F);

  uint32_t MaxOutgoingRegisterCount{0};
  if (ReportPayloadRegisterSizes == PayloadRegisterSizeReportingMode::MaxOutgoing) {
    // Accumulate all outgoing payload sizes per function.
    for (auto &[Jump, RegCount] : ByJumpRegisterCounts)
      MaxOutgoingRegisterCount = std::max(MaxOutgoingRegisterCount, RegCount);
  }

  const StringRef SizeSuffix = " dwords";
  const auto ReportIncomingPayload = [&](std::optional<uint32_t> OptIncomingPayloadRegisterCount,
                                         lgc::rt::RayTracingShaderStage ShaderStage, StringRef ReportSuffix,
                                         bool AppendSizeSuffix) {
    dbgs() << ReportSuffix << " \"" << F->getName() << "\" (" << ShaderStage << "): ";
    if (OptIncomingPayloadRegisterCount.has_value()) {
      dbgs() << OptIncomingPayloadRegisterCount.value();
      if (AppendSizeSuffix)
        dbgs() << SizeSuffix;
    } else {
      dbgs() << "(no incoming payload)";
    }
  };

  auto OptIncomingPayloadRegisterCount = F->getArg(CpsArgIdxWithStackPtr::Payload)->getType()->getArrayNumElements();
  bool HasIncomingPayload = OptIncomingPayloadRegisterCount != 0;

  const bool HasOutgoingPayload = !ByJumpRegisterCounts.empty();
  if (!HasIncomingPayload && !HasOutgoingPayload)
    return;

  if (ReportPayloadRegisterSizes == PayloadRegisterSizeReportingMode::ByJump) {
    ReportIncomingPayload(OptIncomingPayloadRegisterCount, *FuncData.Stage, "Incoming payload VGPR size of", true);
    dbgs() << "\n";

    if (HasOutgoingPayload) {
      dbgs() << "Outgoing payload VGPR size by jump:\n";
      for (auto &[Jump, RegCount] : ByJumpRegisterCounts)
        dbgs() << *Jump << ": " << RegCount << SizeSuffix << '\n';
    }
  } else {
    ReportIncomingPayload(OptIncomingPayloadRegisterCount, *FuncData.Stage,
                          "Incoming and max outgoing payload VGPR size of", false);
    dbgs() << " and ";
    if (HasOutgoingPayload) {
      dbgs() << MaxOutgoingRegisterCount;
    } else {
      dbgs() << "(no outgoing payload)";
    }
    dbgs() << SizeSuffix << '\n';
  }
}

void StatisticsReporter::reportSystemDataSizes(const FunctionData &FuncData) {
  Type *SystemDataTy = FuncData.Func->getFunctionType()->getParamType(CpsArgIdxWithStackPtr::SystemData);
  assert(SystemDataTy->isStructTy() && "SystemData should be of struct type!");
  if (SystemDataTy == nullptr)
    return;

  const Function *F = FuncData.Func;
  auto SystemDataBytes = F->getDataLayout().getTypeStoreSize(SystemDataTy);

  dbgs() << "Incoming system data of \"" << F->getName() << "\" (" << FuncData.Stage << ") is \""
         << SystemDataTy->getStructName() << "\", size:  " << SystemDataBytes << " bytes\n";
}

void StatisticsReporter::report(Function &Func) {
  if (!shouldReport() || Func.isDeclaration())
    return;

  FunctionData FuncData;
  FuncData.Stage = getLgcRtShaderStage(&Func);
  if (!FuncData.Stage || FuncData.Stage == RayTracingShaderStage::KernelEntry)
    return;

  FuncData.Func = &Func;

  if (ReportAllSizes || ReportContStateSizes)
    reportContStateSizes(FuncData);

  if (ReportAllSizes || ReportPayloadRegisterSizes != PayloadRegisterSizeReportingMode::Disabled)
    reportPayloadRegisterSizes(FuncData);

  if (ReportAllSizes || ReportSystemDataSizes)
    reportSystemDataSizes(FuncData);
}
