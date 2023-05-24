/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

// The ModuleBunch class, representing a bunch of modules, and a pass manager
// and analysis manager for it allowing you to run passes on it.

#include "lgc/ModuleBunch.h"
#include "llvm/IR/PassManagerImpl.h"
#include "llvm/IR/PrintPasses.h"

namespace llvm {

template class PassManager<ModuleBunch>;
template class AnalysisManager<ModuleBunch>;
template class AllAnalysesOn<ModuleBunch>;

} // namespace llvm

using namespace llvm;

// Add Module to ModuleBunch, taking ownership.
void ModuleBunch::addModule(std::unique_ptr<Module> module) {
  Modules.push_back(std::move(module));
}

// Renormalize ModuleBunch's array of Modules after manipulation by user.
// Invalidates modules() iterator.
void ModuleBunch::renormalize() {
  // Remove holes from where caller freed/released modules.
  Modules.erase(std::remove(Modules.begin(), Modules.end(), nullptr), Modules.end());
}

// Check that Modules list has been renormalized since caller removed/freed modules.
// Checks that there are no holes.
bool ModuleBunch::isNormalized() const {
  for (const std::unique_ptr<Module> &entry : Modules) {
    if (!entry)
      return false;
  }
  return true;
}

/// Print the ModuleBunch to an output stream. The extra args are passed as-is
/// to Module::print for each module.
void ModuleBunch::print(raw_ostream &OS, AssemblyAnnotationWriter *AAW, bool ShouldPreserveUseListOrder,
                        bool IsForDebug) const {
  for (const Module &M : *this)
    M.print(OS, AAW, ShouldPreserveUseListOrder, IsForDebug);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
/// Dump ModuleBunch to dbgs().
LLVM_DUMP_METHOD
void ModuleBunch::dump() const {
  print(dbgs(), nullptr, false, /*IsForDebug=*/true);
}
#endif

// Copied from IRPrintingPasses.cpp and edited.
PreservedAnalyses PrintModuleBunchPass::run(ModuleBunch &MB, ModuleBunchAnalysisManager &AM) {
  if (llvm::isFunctionInPrintList("*")) {
    if (!Banner.empty())
      OS << Banner << "\n";
    MB.print(OS, nullptr, ShouldPreserveUseListOrder);
  } else {
    bool BannerPrinted = false;
    for (const Module &M : MB) {
      for (const auto &F : M.functions()) {
        if (llvm::isFunctionInPrintList(F.getName())) {
          if (!BannerPrinted && !Banner.empty()) {
            OS << Banner << "\n";
            BannerPrinted = true;
          }
          F.print(OS);
        }
      }
    }
  }

  return PreservedAnalyses::all();
}

// Copied from FunctionAnalysisManagerModuleProxy in llvm/lib/IR/PassManager.cpp and edited.
template <>
bool ModuleAnalysisManagerModuleBunchProxy::Result::invalidate(ModuleBunch &Bunch, const PreservedAnalyses &PA,
                                                               ModuleBunchAnalysisManager::Invalidator &Inv) {
  // If literally everything is preserved, we're done.
  if (PA.areAllPreserved())
    return false; // This is still a valid proxy.

  // If this proxy isn't marked as preserved, then even if the result remains
  // valid, the key itself may no longer be valid, so we clear everything.
  //
  // Note that in order to preserve this proxy, a ModuleBunch pass must ensure that
  // the MAM has been completely updated to handle the deletion of modules.
  // Specifically, any MAM-cached results for those modules need to have been
  // forcibly cleared. When preserved, this proxy will only invalidate results
  // cached on modules *still in the ModuleBunch* at the end of the ModuleBunch pass.
  auto PAC = PA.getChecker<ModuleAnalysisManagerModuleBunchProxy>();
  if (!PAC.preserved() && !PAC.preservedSet<AllAnalysesOn<ModuleBunch>>()) {
    InnerAM->clear();
    return true;
  }

  // Directly check if the relevant set is preserved.
  bool AreModuleAnalysesPreserved = PA.allAnalysesInSetPreserved<AllAnalysesOn<Module>>();

  // Now walk all the modules to see if any inner analysis invalidation is
  // necessary.
  for (Module &M : Bunch) {
    std::optional<PreservedAnalyses> ModulePA;

    // Check to see whether the preserved set needs to be pruned based on
    // module-level analysis invalidation that triggers deferred invalidation
    // registered with the outer analysis manager proxy for this module.
    if (auto *OuterProxy = InnerAM->getCachedResult<ModuleBunchAnalysisManagerModuleProxy>(M))
      for (const auto &OuterInvalidationPair : OuterProxy->getOuterInvalidations()) {
        AnalysisKey *OuterAnalysisID = OuterInvalidationPair.first;
        const auto &InnerAnalysisIDs = OuterInvalidationPair.second;
        if (Inv.invalidate(OuterAnalysisID, Bunch, PA)) {
          if (!ModulePA)
            ModulePA = PA;
          for (AnalysisKey *InnerAnalysisID : InnerAnalysisIDs)
            ModulePA->abandon(InnerAnalysisID);
        }
      }

    // Check if we needed a custom PA set, and if so we'll need to run the
    // inner invalidation.
    if (ModulePA) {
      InnerAM->invalidate(M, *ModulePA);
      continue;
    }

    // Otherwise we only need to do invalidation if the original PA set didn't
    // preserve all module analyses.
    if (!AreModuleAnalysesPreserved)
      InnerAM->invalidate(M, PA);
  }

  // Return false to indicate that this result is still a valid proxy.
  return false;
}

// Copied from ModuleToFunctionPassAdaptor::printPipeline in llvm/lib/IR/PassManager.cpp and edited.
void ModuleBunchToModulePassAdaptor::printPipeline(raw_ostream &OS,
                                                   function_ref<StringRef(StringRef)> MapClassName2PassName) {
  OS << "module";
  if (EagerlyInvalidate)
    OS << "<eager-inv>";
  OS << "(";
  Pass->printPipeline(OS, MapClassName2PassName);
  OS << ")";
}

// Copied from ModuleToFunctionPassAdaptor::run in llvm/lib/IR/PassManager.cpp and edited.
PreservedAnalyses ModuleBunchToModulePassAdaptor::run(ModuleBunch &Bunch, ModuleBunchAnalysisManager &AM) {
  ModuleAnalysisManager &MAM = AM.getResult<ModuleAnalysisManagerModuleBunchProxy>(Bunch).getManager();

  // Request PassInstrumentation from analysis manager, will use it to run
  // instrumenting callbacks for the passes later.
  PassInstrumentation PI = AM.getResult<PassInstrumentationAnalysis>(Bunch);

  PreservedAnalyses PA = PreservedAnalyses::all();

  // TODO: Add real parallelism, with an API to provide threads to run module passes.
  // For now, run each distinct LLVMContext in a separate copy of the module pass manager,
  // so we can at least test users adding identical copies of the module pass manager.
  SmallPtrSet<LLVMContext *, 16> DoneContexts;
  for (unsigned StartIdx = 0; StartIdx != Bunch.size(); ++StartIdx) {
    Module &module = Bunch.begin()[StartIdx];
    LLVMContext *Context = &module.getContext();
    if (!DoneContexts.insert(Context).second)
      continue;

    // Use the single Pass if it was set. Otherwise call PassMaker to create a Pass each time
    // round the outer per-LLVMContext loop.
    std::unique_ptr<PassConceptT> AllocatedPass;
    PassConceptT *ThisPass = Pass.get();
    if (!ThisPass) {
      AllocatedPass = PassMaker();
      ThisPass = &*AllocatedPass;
    }

    for (unsigned Idx = StartIdx; Idx != Bunch.size(); ++Idx) {
      Module &M = Bunch.begin()[Idx];
      if (&M.getContext() != Context)
        continue;

      // Check the PassInstrumentation's BeforePass callbacks before running the
      // pass, skip its execution completely if asked to (callback returns
      // false).
      if (!PI.runBeforePass<Module>(*ThisPass, M))
        continue;

      PreservedAnalyses PassPA = ThisPass->run(M, MAM);
      PI.runAfterPass(*ThisPass, M, PassPA);

      // TODO: With real parallelism, the next two statements need to be under a mutex.
      // We know that the module pass couldn't have invalidated any other
      // module's analyses (that's the contract of a module pass), so
      // directly handle the module analysis manager's invalidation here.
      MAM.invalidate(M, EagerlyInvalidate ? PreservedAnalyses::none() : PassPA);

      // Then intersect the preserved set so that invalidation of module
      // analyses will eventually occur when the module pass completes.
      PA.intersect(std::move(PassPA));
    }
  }

  // The ModuleAnalysisManagerModuleBunchProxy is preserved because (we assume)
  // the module passes we ran didn't add or remove any modules.
  //
  // We also preserve all analyses on Modules, because we did all the
  // invalidation we needed to do above.
  PA.preserveSet<AllAnalysesOn<Module>>();
  PA.preserve<ModuleAnalysisManagerModuleBunchProxy>();
  return PA;
}

// Copied from lib/Passes/PassBuilder.cpp because it is private there.
std::optional<std::vector<PassBuilder::PipelineElement>> MbPassBuilder::parsePipelineText(StringRef Text) {
  std::vector<PipelineElement> ResultPipeline;

  SmallVector<std::vector<PipelineElement> *, 4> PipelineStack = {&ResultPipeline};
  for (;;) {
    std::vector<PipelineElement> &Pipeline = *PipelineStack.back();
    size_t Pos = Text.find_first_of(",()");
    Pipeline.push_back({Text.substr(0, Pos), {}});

    // If we have a single terminating name, we're done.
    if (Pos == Text.npos)
      break;

    char Sep = Text[Pos];
    Text = Text.substr(Pos + 1);
    if (Sep == ',')
      // Just a name ending in a comma, continue.
      continue;

    if (Sep == '(') {
      // Push the inner pipeline onto the stack to continue processing.
      PipelineStack.push_back(&Pipeline.back().InnerPipeline);
      continue;
    }

    assert(Sep == ')' && "Bogus separator!");
    // When handling the close parenthesis, we greedily consume them to avoid
    // empty strings in the pipeline.
    do {
      // If we try to pop the outer pipeline we have unbalanced parentheses.
      if (PipelineStack.size() == 1)
        return std::nullopt;

      PipelineStack.pop_back();
    } while (Text.consume_front(")"));

    // Check if we've finished parsing.
    if (Text.empty())
      break;

    // Otherwise, the end of an inner pipeline always has to be followed by
    // a comma, and then we can continue.
    if (!Text.consume_front(","))
      return std::nullopt;
  }

  if (PipelineStack.size() > 1)
    // Unbalanced paretheses.
    return std::nullopt;

  assert(PipelineStack.back() == &ResultPipeline && "Wrong pipeline at the bottom of the stack!");
  return {std::move(ResultPipeline)};
}

// Copied from PassBuilder.cpp.
static std::optional<int> parseRepeatPassName(StringRef Name) {
  if (!Name.consume_front("repeat<") || !Name.consume_back(">"))
    return std::nullopt;
  int Count;
  if (Name.getAsInteger(0, Count) || Count <= 0)
    return std::nullopt;
  return Count;
}

// Copied from PassBuilder.cpp.
/// Tests whether registered callbacks will accept a given pass name.
///
/// When parsing a pipeline text, the type of the outermost pipeline may be
/// omitted, in which case the type is automatically determined from the first
/// pass name in the text. This may be a name that is handled through one of the
/// callbacks. We check this through the oridinary parsing callbacks by setting
/// up a dummy PassManager in order to not force the client to also handle this
/// type of query.
template <typename PassManagerT, typename CallbacksT>
static bool callbacksAcceptPassName(StringRef Name, CallbacksT &Callbacks) {
  if (!Callbacks.empty()) {
    PassManagerT DummyPM;
    for (auto &CB : Callbacks)
      if (CB(Name, DummyPM, {}))
        return true;
  }
  return false;
}

// Copied from isModulePassName in PassBuilder.cpp and edited.
template <typename CallbacksT> static bool isModuleBunchPassName(StringRef Name, CallbacksT &Callbacks) {
  // Explicitly handle pass manager names.
  if (Name == "modulebunch")
    return true;
  if (Name == "module")
    return true;
  if (Name == "cgscc")
    return true;
  if (Name == "function" || Name == "function<eager-inv>")
    return true;
  if (Name == "coro-cond")
    return true;

  // Explicitly handle custom-parsed pass names.
  if (parseRepeatPassName(Name))
    return true;
  return callbacksAcceptPassName<ModuleBunchPassManager>(Name, Callbacks);
}

// Copied from the ModulePassManager overload in llvm/lib/Passes/PassBuilder.cpp and edited.
// Primary pass pipeline description parsing routine for a \c ModuleBunchPassManager
// FIXME: Should this routine accept a TargetMachine or require the caller to
// pre-populate the analysis managers with target-specific stuff?
Error MbPassBuilder::parsePassPipeline(ModuleBunchPassManager &MBPM, StringRef PipelineText) {
  auto Pipeline = parsePipelineText(PipelineText);
  if (!Pipeline || Pipeline->empty())
    return make_error<StringError>(formatv("invalid pipeline '{0}'", PipelineText).str(), inconvertibleErrorCode());

  // If the first name isn't at the modulebunch layer, wrap the pipeline up
  // automatically.
  StringRef FirstName = Pipeline->front().Name;

  if (!isModuleBunchPassName(FirstName, ModuleBunchPipelineParsingCallbacks)) {
    ModulePassManager MPM;
    if (Error Err = PassBuilder::parsePassPipeline(MPM, PipelineText))
      return Err;
    MBPM.addPass(createModuleBunchToModulePassAdaptor(std::move(MPM)));
    return Error::success();
  }

  if (auto Err = parseModuleBunchPassPipeline(MBPM, *Pipeline))
    return Err;
  return Error::success();
}

// Copied from PassBuilder::parseModulePassPipeline and edited.
Error MbPassBuilder::parseModuleBunchPassPipeline(ModuleBunchPassManager &MBPM, ArrayRef<PipelineElement> Pipeline) {
  for (const auto &Element : Pipeline) {
    if (auto Err = parseModuleBunchPass(MBPM, Element))
      return Err;
  }
  return Error::success();
}

// Copied from PassBuilder::parseModulePass and edited.
Error MbPassBuilder::parseModuleBunchPass(ModuleBunchPassManager &MBPM, const PipelineElement &E) {
  auto &Name = E.Name;
  auto &InnerPipeline = E.InnerPipeline;

  // First handle complex passes like the pass managers which carry pipelines.
  if (!InnerPipeline.empty()) {
    if (Name == "modulebunch") {
      ModuleBunchPassManager NestedMBPM;
      if (auto Err = parseModuleBunchPassPipeline(NestedMBPM, InnerPipeline))
        return Err;
      MBPM.addPass(std::move(NestedMBPM));
      return Error::success();
    }
    if (auto Count = parseRepeatPassName(Name)) {
      ModuleBunchPassManager NestedMBPM;
      if (auto Err = parseModuleBunchPassPipeline(NestedMBPM, InnerPipeline))
        return Err;
      MBPM.addPass(createRepeatedPass(*Count, std::move(NestedMBPM)));
      return Error::success();
    }
    // TODO:
    // For any other nested pass manager ("module", "function" etc) we want to invoke
    // parseModulePassPipeline etc, but we can't as it is private in PassBuilder. So
    // instead we need to reconstruct a text string and call parsePipelineText.
    report_fatal_error("Nested pipeline spec not handled yet");
  }

  for (auto &C : ModuleBunchPipelineParsingCallbacks)
    if (C(Name, MBPM, InnerPipeline))
      return Error::success();
  return make_error<StringError>(formatv("unknown modulebunch pass '{0}'", Name).str(), inconvertibleErrorCode());
}
