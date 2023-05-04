/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
/**
 ***********************************************************************************************************************
 * @file  PassManager.h
 * @brief LLPC header file: contains declaration of class lgc::LegacyPassManager.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/ModuleBunch.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class LLVMContext;
class TargetMachine;
} // namespace llvm

namespace lgc {

class LgcContext;

// =====================================================================================================================
// Public interface of LLPC middle-end's legacy::PassManager override
class LegacyPassManager : public llvm::legacy::PassManager {
public:
  static LegacyPassManager *Create();
  virtual ~LegacyPassManager() {}
  virtual void stop() = 0;
  virtual void setPassIndex(unsigned *passIndex) = 0;
};

// =====================================================================================================================
// Public interface of LLPC middle-end's PassManager override -- Module pass manager edition
class PassManager : public llvm::ModulePassManager {
public:
  // Create an LGC PassManager using the TargetMachine and LLVMContext from the given lgcContext.
  static std::unique_ptr<PassManager> Create(LgcContext *lgcContext);

  virtual ~PassManager() = default;
  template <typename PassBuilderT> bool registerFunctionAnalysis(PassBuilderT &&PassBuilder) {
    return m_functionAnalysisManager.registerPass(std::forward<PassBuilderT>(PassBuilder));
  }
  template <typename PassBuilderT> bool registerModuleAnalysis(PassBuilderT &&passBuilder) {
    return m_moduleAnalysisManager.registerPass(std::forward<PassBuilderT>(passBuilder));
  }
  // Register a pass to identify it with a short name in the pass manager
  virtual void registerPass(llvm::StringRef passName, llvm::StringRef className) = 0;
  virtual void run(llvm::Module &module) = 0;
  virtual void setPassIndex(unsigned *passIndex) = 0;
  virtual bool stopped() const = 0;

  virtual llvm::PassInstrumentationCallbacks &getInstrumentationCallbacks() = 0;

protected:
  llvm::FunctionAnalysisManager m_functionAnalysisManager;
  llvm::ModuleAnalysisManager m_moduleAnalysisManager;
};

// =====================================================================================================================
// Public interface of LLPC middle-end's PassManager override -- ModuleBunch pass manager edition
class MbPassManager : public llvm::PassManager<llvm::ModuleBunch> {
public:
  // Create an LGC PassManager using the given TargetMachine.
  static std::unique_ptr<MbPassManager> Create(llvm::TargetMachine *targetMachine);

  virtual ~MbPassManager() = default;
  template <typename PassBuilderT> bool registerFunctionAnalysis(PassBuilderT &&PassBuilder) {
    return m_functionAnalysisManager.registerPass(std::forward<PassBuilderT>(PassBuilder));
  }
  template <typename PassBuilderT> bool registerModuleAnalysis(PassBuilderT &&passBuilder) {
    return m_moduleAnalysisManager.registerPass(std::forward<PassBuilderT>(passBuilder));
  }
  template <typename PassBuilderT> bool registerModuleBunchAnalysis(PassBuilderT &&passBuilder) {
    return m_moduleBunchAnalysisManager.registerPass(std::forward<PassBuilderT>(passBuilder));
  }
  // Register a pass to identify it with a short name in the pass manager
  virtual void registerPass(llvm::StringRef passName, llvm::StringRef className) = 0;
  virtual void run(llvm::ModuleBunch &moduleBunch) = 0;
  virtual bool stopped() const = 0;

  virtual llvm::PassInstrumentationCallbacks &getInstrumentationCallbacks() = 0;

protected:
  llvm::FunctionAnalysisManager m_functionAnalysisManager;
  llvm::ModuleAnalysisManager m_moduleAnalysisManager;
  llvm::ModuleBunchAnalysisManager m_moduleBunchAnalysisManager;
};

} // namespace lgc
