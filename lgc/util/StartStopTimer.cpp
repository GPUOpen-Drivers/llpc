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
* @file  StartStopTimer.cpp
* @brief LLPC source file: pass to start or stop a timer
***********************************************************************************************************************
*/
#include "lgc/LgcContext.h"
#include "lgc/PassManager.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/Timer.h"

#define DEBUG_TYPE "lgc-start-stop-timer"

using namespace llvm;
using namespace lgc;

namespace {

// =====================================================================================================================
// Pass to start or stop a timer
class StartStopTimer : public PassInfoMixin<StartStopTimer> {
public:
  StartStopTimer() {}
  StartStopTimer(Timer *timer, bool starting) : m_timer(timer), m_starting(starting) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
    runImpl(M);
    return PreservedAnalyses::all();
  }

  bool runImpl(Module &module);

  static StringRef name() { return "Start or stop timer"; }

private:
  Timer *m_timer;  // The timer to start or stop when the pass is run
  bool m_starting; // True to start the timer, false to stop it
};

// =====================================================================================================================
// Legacy pass manager wrapper class
class LegacyStartStopTimer : public ModulePass {
public:
  static char ID;
  LegacyStartStopTimer() : ModulePass(ID) {}
  LegacyStartStopTimer(Timer *timer, bool starting) : ModulePass(ID), Impl(timer, starting) {}

  bool runOnModule(Module &module) override;

private:
  LegacyStartStopTimer(const LegacyStartStopTimer &) = delete;
  LegacyStartStopTimer &operator=(const LegacyStartStopTimer &) = delete;

  StartStopTimer Impl;
};

char LegacyStartStopTimer::ID = 0;

} // namespace

// =====================================================================================================================
// Create a start/stop timer pass. This is a static method in LgcContext, so it can be accessed by
// the front-end to add to its pass manager.
//
// @param timer : The timer to start or stop when the pass is run
// @param starting : True to start the timer, false to stop it
ModulePass *LgcContext::createStartStopTimer(Timer *timer, bool starting) {
  return new LegacyStartStopTimer(timer, starting);
}

// =====================================================================================================================
// Create a start/stop timer pass and add it to the pass manager.  This is a
// static method in LgcContext, so it can be accessed by the front-end to add
// to its pass manager.
//
// @param passMgr : Pass manager to add the pass to
// @param timer : The timer to start or stop when the pass is run
// @param starting : True to start the timer, false to stop it
void LgcContext::createAndAddStartStopTimer(lgc::PassManager &passMgr, Timer *timer, bool starting) {
  passMgr.addPass(StartStopTimer(timer, starting));
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool StartStopTimer::runImpl(Module &module) {
  if (m_starting)
    m_timer->startTimer();
  else
    m_timer->stopTimer();
  return false;
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool LegacyStartStopTimer::runOnModule(Module &module) {
  return Impl.runImpl(module);
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(LegacyStartStopTimer, DEBUG_TYPE, "Start or stop timer", false, false)
