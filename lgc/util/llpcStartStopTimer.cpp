/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  llpcStartStopTimer.cpp
* @brief LLPC source file: pass to start or stop a timer
***********************************************************************************************************************
*/
#include "lgc/llpcBuilderContext.h"
#include "llpcInternal.h"

#include "llvm/Support/Timer.h"
#include "llvm/Pass.h"

#define DEBUG_TYPE "llpc-start-stop-timer"

using namespace llvm;
using namespace lgc;

namespace
{

// =====================================================================================================================
// Pass to start or stop a timer
class StartStopTimer : public ModulePass
{
public:
    static char ID;
    StartStopTimer() : ModulePass(ID) {}
    StartStopTimer(Timer* timer, bool starting) : ModulePass(ID), m_timer(timer), m_starting(starting)
    {
    }

    bool runOnModule(Module& module) override;

private:
    StartStopTimer(const StartStopTimer&) = delete;
    StartStopTimer& operator =(const StartStopTimer&) = delete;

    // -----------------------------------------------------------------------------------------------------------------

    Timer*  m_timer;   // The timer to start or stop when the pass is run
    bool    m_starting; // True to start the timer, false to stop it
};

char StartStopTimer::ID = 0;

} // lgc

// =====================================================================================================================
// Create a start/stop timer pass. This is a static method in BuilderContext, so it can be accessed by
// the front-end to add to its pass manager.
ModulePass* BuilderContext::createStartStopTimer(
    Timer*  timer,   // The timer to start or stop when the pass is run
    bool    starting) // True to start the timer, false to stop it
{
    return new StartStopTimer(timer, starting);
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
bool StartStopTimer::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    if (m_starting)
    {
        m_timer->startTimer();
    }
    else
    {
        m_timer->stopTimer();
    }
    return false;
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(StartStopTimer, DEBUG_TYPE, "Start or stop timer", false, false)

