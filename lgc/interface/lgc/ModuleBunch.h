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

#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"

namespace llvm {

class ModuleBunch;

using ModuleBunchPassManager = PassManager<ModuleBunch>;
using ModuleBunchAnalysisManager = AnalysisManager<ModuleBunch>;
using ModuleAnalysisManagerModuleBunchProxy = InnerAnalysisManagerProxy<ModuleAnalysisManager, ModuleBunch>;
using ModuleBunchAnalysisManagerModuleProxy = OuterAnalysisManagerProxy<ModuleBunchAnalysisManager, Module>;

/// ModuleBunch is a pseudo-IR construct for a bunch of modules that we want to run passes on.
class ModuleBunch {
public:
  // Iterator for accessing the Modules in the ModuleBunch, without being able to free or replace
  // any Module. The iterator does a "double dereference" from pointer-to-unique-ptr-to-Module
  // down to Module &.
  using iterator = llvm::pointee_iterator<ArrayRef<std::unique_ptr<Module>>::iterator>;

  // Access the Modules in the ModuleBunch, without erasing/removing/replacing them.
  iterator begin() const { return iterator(Modules.begin()); }
  iterator end() const { return iterator(Modules.end()); }
  size_t size() const { return end() - begin(); }
  bool empty() const { return size() == 0; }

  // Access the array of Modules in the ModuleBunch, directly accessing the unique_ptrs
  // for erasing/removing/replacing them.
  // After doing that, call renormalize() to remove any holes.
  MutableArrayRef<std::unique_ptr<Module>> getMutableModules() {
    assert(isNormalized());
    return Modules;
  }

  // Add Module to ModuleBunch, taking ownership. Invalidates modules() iterator.
  void addModule(std::unique_ptr<Module> module);

  // Renormalize ModuleBunch's array of Modules after manipulation by user.
  // Invalidates modules() iterator.
  void renormalize();

  // Check that Modules list has been renormalized since caller removed/freed modules.
  bool isNormalized() const;

  // Print the ModuleBunch to an output stream. The extra args are passed as-is
  // to Module::print for each module.
  void print(raw_ostream &OS, AssemblyAnnotationWriter *AAW, bool ShouldPreserveUseListOrder = false,
             bool IsForDebug = false) const;

  // Dump the module to stderr (for debugging).
  void dump() const;

private:
  SmallVector<std::unique_ptr<Module>> Modules;
};

/// A raw_ostream inserter for ModuleBunch
inline raw_ostream &operator<<(raw_ostream &O, const ModuleBunch &MB) {
  MB.print(O, nullptr);
  return O;
}

extern template class PassManager<ModuleBunch>;
extern template class AnalysisManager<ModuleBunch>;
extern template class AllAnalysesOn<ModuleBunch>;

/// Trivial adaptor that maps from a ModuleBunch to its modules.
///
/// Designed to allow composition of a ModulePass(Manager) and
/// a ModuleBunchPassManager, by running the ModulePass(Manager) over every
/// module in the ModuleBunch.
///
/// Module passes run within this adaptor can rely on having exclusive access
/// to the module they are run over. They should not read or modify any other
/// modules! Other threads or systems may be manipulating other functions in
/// the ModuleBunch, and so their state should never be relied on.
///
/// Module passes can also read the ModuleBunch containing the module, but they
/// should not modify that ModuleBunch.
/// For example, a module pass is not permitted to add modules to the
/// ModuleBunch.
///
/// Note that although module passes can access ModuleBunch analyses, ModuleBunch
/// analyses are not invalidated while the module passes are running, so they
/// may be stale.  Module analyses will not be stale.
class ModuleBunchToModulePassAdaptor : public PassInfoMixin<ModuleBunchToModulePassAdaptor> {
public:
  using PassConceptT = detail::PassConcept<Module, ModuleAnalysisManager>;

  /// Construct with a function that returns a pass. It can then parallelize compilation by calling
  /// the function once for each parallel thread.
  explicit ModuleBunchToModulePassAdaptor(function_ref<std::unique_ptr<PassConceptT>()> PassMaker,
                                          bool EagerlyInvalidate = false)
      : PassMaker(PassMaker), EagerlyInvalidate(EagerlyInvalidate) {}

  /// Construct with a pass. It can then not parallelize compilation.
  explicit ModuleBunchToModulePassAdaptor(std::unique_ptr<PassConceptT> pass, bool eagerlyInvalidate)
      : Pass(std::move(pass)), EagerlyInvalidate(eagerlyInvalidate) {}

  /// Runs the module pass across every module in the ModuleBunch.
  PreservedAnalyses run(ModuleBunch &moduleBunch, ModuleBunchAnalysisManager &analysisMgr);
  void printPipeline(raw_ostream &os, function_ref<StringRef(StringRef)> mapClassName2PassName);

  static bool isRequired() { return true; }

private:
  std::unique_ptr<PassConceptT> Pass;
  function_ref<std::unique_ptr<PassConceptT>()> PassMaker;
  bool EagerlyInvalidate;
};

// A function to deduce a module pass type and create a unique_ptr of it for returning from the PassMaker
// function.
template <typename ModulePassT>
std::unique_ptr<ModuleBunchToModulePassAdaptor::PassConceptT>
createForModuleBunchToModulePassAdaptor(ModulePassT Pass) {
  using PassModelT = detail::PassModel<Module, ModulePassT, PreservedAnalyses, ModuleAnalysisManager>;
  return std::unique_ptr<ModuleBunchToModulePassAdaptor::PassConceptT>(new PassModelT(std::forward<ModulePassT>(Pass)));
}

// A function to deduce a module pass type and wrap it in the templated adaptor.
template <typename ModulePassT>
ModuleBunchToModulePassAdaptor createModuleBunchToModulePassAdaptor(ModulePassT Pass, bool EagerlyInvalidate = false) {
  return ModuleBunchToModulePassAdaptor(createForModuleBunchToModulePassAdaptor(std::move(Pass)), EagerlyInvalidate);
}

/// This class provides access to building LLVM's passes.
///
/// Currently implemented as a subclass of LLVM's PassBuilder. If we merge ModuleBunch
/// into LLVM, then the functionality here would be merged into PassBuilder.
class MbPassBuilder : public PassBuilder {
public:
  explicit MbPassBuilder(TargetMachine *TM = nullptr, PipelineTuningOptions PTO = PipelineTuningOptions(),
                         std::optional<PGOOptions> PGOOpt = std::nullopt, PassInstrumentationCallbacks *PIC = nullptr)
      : PassBuilder(TM, PTO, PGOOpt, PIC) {}

  /// Parse a textual pass pipeline description into a \c
  /// ModulePassManager.
  ///
  /// The format of the textual pass pipeline description looks something like:
  ///
  ///   modulebunch(module(function(instcombine,sroa),dce,cgscc(inliner,function(...)),...))
  ///
  /// Pass managers have ()s describing the nest structure of passes. All passes
  /// are comma separated. As a special shortcut, if the very first pass is not
  /// a modulebunch pass (as a modulebunch pass manager is), this will automatically form
  /// the shortest stack of pass managers that allow inserting that first pass.
  /// So, assuming module passes 'mpassN', function passes 'fpassN', CGSCC passes
  /// 'cgpassN', and loop passes 'lpassN', all of these are valid:
  ///
  ///   mpass1,mpass2,mpass3
  ///   fpass1,fpass2,fpass3
  ///   cgpass1,cgpass2,cgpass3
  ///   lpass1,lpass2,lpass3
  ///
  /// And they are equivalent to the following (resp.):
  ///
  ///   modulebunch(module(mpass1,mpass2,mpass3))
  ///   modulebunch(module(function(fpass1,fpass2,fpass3)))
  ///   modulebunch(module(cgscc(cgpass1,cgpass2,cgpass3)))
  ///   modulebunch(module(function(loop(lpass1,lpass2,lpass3))))
  ///
  /// This shortcut is especially useful for debugging and testing small pass
  /// combinations.
  ///
  /// The sequence of passes aren't necessarily the exact same kind of pass.
  /// You can mix different levels implicitly if adaptor passes are defined to
  /// make them work. For example,
  ///
  ///   mpass1,fpass1,fpass2,mpass2,lpass1
  ///
  /// This pipeline uses only one pass manager: the top-level modulebunch manager.
  /// fpass1,fpass2 and lpass1 are added into the the top-level modulebunch manager
  /// using only adaptor passes. No nested function/loop pass managers are
  /// added. The purpose is to allow easy pass testing when the user
  /// specifically want the pass to run under a adaptor directly. This is
  /// preferred when a pipeline is largely of one type, but one or just a few
  /// passes are of different types(See PassBuilder.cpp for examples).
  Error parsePassPipeline(ModuleBunchPassManager &passMgr, StringRef pipelineText);

  /// Register pipeline parsing callbacks with this pass builder instance.
  /// Using these callbacks, callers can parse both a single pass name, as well
  /// as entire sub-pipelines, and populate the PassManager instance
  /// accordingly.
  void registerPipelineParsingCallback(
      const std::function<bool(StringRef Name, ModuleBunchPassManager &, ArrayRef<PipelineElement>)> &C) {
    ModuleBunchPipelineParsingCallbacks.push_back(C);
  }

  // Forward other overloads of registerPipelineParsingCallback to PassBuilder.
  void registerPipelineParsingCallback(
      const std::function<bool(StringRef Name, ModulePassManager &, ArrayRef<PipelineElement>)> &C) {
    PassBuilder::registerPipelineParsingCallback(C);
  }

  void registerPipelineParsingCallback(
      const std::function<bool(StringRef Name, FunctionPassManager &, ArrayRef<PipelineElement>)> &C) {
    PassBuilder::registerPipelineParsingCallback(C);
  }

  void registerPipelineParsingCallback(
      const std::function<bool(StringRef Name, LoopPassManager &, ArrayRef<PipelineElement>)> &C) {
    PassBuilder::registerPipelineParsingCallback(C);
  }

private:
  static std::optional<std::vector<PipelineElement>> parsePipelineText(StringRef Text);

  Error parseModuleBunchPassPipeline(ModuleBunchPassManager &MBPM, ArrayRef<PipelineElement> Pipeline);

  Error parseModuleBunchPass(ModuleBunchPassManager &MBPM, const PipelineElement &E);

  SmallVector<std::function<bool(StringRef, ModuleBunchPassManager &, ArrayRef<PipelineElement>)>, 2>
      ModuleBunchPipelineParsingCallbacks;
};

// Copied from PrintModulePass in IRPrintingPasses.h and edited.
/// ModuleBunch pass to print the IR of the modules.
class PrintModuleBunchPass : public llvm::PassInfoMixin<PrintModuleBunchPass> {
  raw_ostream &OS;
  std::string Banner;
  bool ShouldPreserveUseListOrder;

public:
  PrintModuleBunchPass() : OS(dbgs()) {}
  PrintModuleBunchPass(raw_ostream &OS, const std::string &Banner, bool ShouldPreserveUseListOrder)
      : OS(OS), Banner(Banner), ShouldPreserveUseListOrder(ShouldPreserveUseListOrder) {}

  PreservedAnalyses run(ModuleBunch &MB, AnalysisManager<ModuleBunch> &);
  static bool isRequired() { return true; }
};

} // namespace llvm
