/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
  * @file  llpcTimerProfiler.cpp
  * @brief LLPC header file: contains the implementation of LLPC utility class TimerProfiler.
  ***********************************************************************************************************************
  */

#include "llvm/ADT/Twine.h"
#include "llvm/Pass.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include "llpc.h"
#include "llpcInternal.h"
#include "llpcPassManager.h"
#include "llpcTimerProfiler.h"

using namespace llvm;

namespace llvm
{

namespace cl
{

// -enable-time-profile : profile the compile time of pipeline
opt<bool> EnableTimerProfile("enable-timer-profile", desc("profile the compile time of pipeline"), init(false));

} // cl

} // llvm

namespace Llpc
{

// =====================================================================================================================
TimerProfiler::TimerProfiler(
    uint64_t      hash64,              // Hash code
    const char*   pDescriptionPrefix,  // [in] Profiler description prefix string
    uint32_t      enableMask)           // Mask of enabled phase timers
    :
    m_total("", "", GetDummyTimeRecords()),
    m_phases("", "", GetDummyTimeRecords())
{
    if (TimePassesIsEnabled || cl::EnableTimerProfile)
    {
        std::string hashString;
        raw_string_ostream ostream(hashString);
        ostream << format("0x%016" PRIX64, hash64);
        ostream.flush();

        // Init whole timer
        m_total.setName("llpc", (Twine(pDescriptionPrefix) + Twine(" ") + hashString).str());
        m_wholeTimer.init("llpc-total", (Twine(pDescriptionPrefix) + Twine(" Total ") + hashString).str(), m_total);

        // Init phase timers
        m_phases.setName("llpc", (Twine(pDescriptionPrefix) + Twine(" Phases ") + hashString).str());
        if (enableMask & (1 << TimerTranslate))
        {
            m_phaseTimers[TimerTranslate].init("llpc-translate",
                                               (Twine(pDescriptionPrefix) +Twine(" Translate ") + hashString).str(),
                                               m_phases);
        }

        if (enableMask & (1 << TimerLower))
        {
            m_phaseTimers[TimerLower].init("llpc-lower",
                                           (Twine(pDescriptionPrefix) + Twine(" Lower ") + hashString).str(),
                                           m_phases);
        }
        if (enableMask & (1 << TimerLoadBc))
        {
            m_phaseTimers[TimerLoadBc].init("llpc-load",
                                            (Twine(pDescriptionPrefix) + Twine(" Load ") + hashString).str(),
                                            m_phases);
        }
        if (enableMask & (1 << TimerPatch))
        {
            m_phaseTimers[TimerPatch].init("llpc-patch",
                                           (Twine(pDescriptionPrefix) + Twine(" Patch ") + hashString).str(),
                                           m_phases);
        }
        if (enableMask & (1 << TimerOpt))
        {
            m_phaseTimers[TimerOpt].init("llpc-opt",
                                         (Twine(pDescriptionPrefix) + Twine(" Optimization ") + hashString).str(),
                                         m_phases);
        }

        if (enableMask & (1 << TimerCodeGen))
        {
            m_phaseTimers[TimerCodeGen].init("llpc-codegen",
                                             (Twine(pDescriptionPrefix) + Twine(" CodeGen ") + hashString).str(),
                                             m_phases);
        }

        // Start whole timer
        m_wholeTimer.startTimer();
    }
}

// =====================================================================================================================
TimerProfiler::~TimerProfiler()
{
    if (TimePassesIsEnabled || cl::EnableTimerProfile)
    {
        // Stop whole timer
        m_wholeTimer.stopTimer();
    }
}

// =====================================================================================================================
// Adds pass to start or stop timer in PassManager
void TimerProfiler::AddTimerStartStopPass(
    PassManager* pPassMgr,       // Pass Manager
    TimerKind    timerKind,      // Kind of phase timer
    bool         start)          // Start or  stop timer
{
    if (TimePassesIsEnabled || cl::EnableTimerProfile)
    {
        pPassMgr->add(CreateStartStopTimer(&m_phaseTimers[timerKind], start));
    }
}

// =====================================================================================================================
// Starts or stop specified timer.
void TimerProfiler::StartStopTimer(
    TimerKind timerKind,         // Kind of phase timer
    bool      start)             // Start or  stop timer
{
    if (TimePassesIsEnabled || cl::EnableTimerProfile)
    {
        if (start)
        {
            m_phaseTimers[timerKind].startTimer();
        }
        else
        {
            m_phaseTimers[timerKind].stopTimer();
        }
    }
}

// =====================================================================================================================
// Gets a specific timer. Returns nullptr if TimePassesIsEnabled isn't enabled.
llvm::Timer* TimerProfiler::GetTimer(
    TimerKind    timerKind)           // Kind of phase timer
{
    return (TimePassesIsEnabled || cl::EnableTimerProfile) ? &m_phaseTimers[timerKind] : nullptr;
}

// =====================================================================================================================
// Gets dummy TimeRecords.
const StringMap<TimeRecord>& TimerProfiler::GetDummyTimeRecords()
{
    static StringMap<TimeRecord> dummyTimeRecords;
    if ((TimePassesIsEnabled || cl::EnableTimerProfile) && dummyTimeRecords.empty())
    {
        // NOTE: It is a workaround to get fixed layout in timer reports. Please remove it if we find a better solution.
        // LLVM timer skips the field if it is zero in all timers, it causes the layout of the report isn't stable when
        // compile multiple pipelines. so we add a dummy record to force all fields is shown.
        // But LLVM TimeRecord can't be initialized explicitly. We have to use HackedTimeRecord to force update the vaule
        // in TimeRecord.
        TimeRecord timeRecord;
        struct HackedTimeRecord
        {
            double t1;
            double t2;
            double t3;
            ssize_t m1;
        } hackedTimeRecord = { 1e-100, 1e-100, 1e-100, 0 };
        static_assert(sizeof(timeRecord) == sizeof(hackedTimeRecord), "Unexpected Size!");
        memcpy(&timeRecord, &hackedTimeRecord, sizeof(TimeRecord));
        dummyTimeRecords["DUMMY"] = timeRecord;
    }

    return dummyTimeRecords;
}

} // Llpc

