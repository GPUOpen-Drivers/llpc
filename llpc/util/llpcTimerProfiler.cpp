/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "llpcTimerProfiler.h"
#include "llpc.h"
#include "lgc/LgcContext.h"
#include "lgc/PassManager.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace llvm {

namespace cl {

// -enable-time-profile : profile the compile time of pipeline
opt<bool> EnableTimerProfile("enable-timer-profile", desc("profile the compile time of pipeline"), init(false));

} // namespace cl

} // namespace llvm

namespace Llpc {

// =====================================================================================================================
//
// @param hash64 : Hash code
// @param descriptionPrefix : Profiler description prefix string
// @param enableMask : Mask of enabled phase timers
TimerProfiler::TimerProfiler(uint64_t hash64, const char *descriptionPrefix, unsigned enableMask)
    : m_total("", "", getDummyTimeRecords()), m_phases("", "", getDummyTimeRecords()) {
  if (TimePassesIsEnabled || cl::EnableTimerProfile) {
    std::string hashString;
    raw_string_ostream ostream(hashString);
    ostream << format("0x%016" PRIX64, hash64);
    ostream.flush();

    // Init whole timer
    m_total.setName("llpc", (Twine(descriptionPrefix) + Twine(" ") + hashString).str());
    m_wholeTimer.init("llpc-total", (Twine(descriptionPrefix) + Twine(" Total ") + hashString).str(), m_total);

    // Init phase timers
    m_phases.setName("llpc", (Twine(descriptionPrefix) + Twine(" Phases ") + hashString).str());
    if (enableMask & (1 << TimerTranslate)) {
      m_phaseTimers[TimerTranslate].init(
          "llpc-translate", (Twine(descriptionPrefix) + Twine(" Translate ") + hashString).str(), m_phases);
    }

    if (enableMask & (1 << TimerLower)) {
      m_phaseTimers[TimerLower].init("llpc-lower", (Twine(descriptionPrefix) + Twine(" Lower ") + hashString).str(),
                                     m_phases);
    }
    if (enableMask & (1 << TimerLoadBc)) {
      m_phaseTimers[TimerLoadBc].init("llpc-load", (Twine(descriptionPrefix) + Twine(" Load ") + hashString).str(),
                                      m_phases);
    }
    if (enableMask & (1 << TimerPatch)) {
      m_phaseTimers[TimerPatch].init("llpc-patch", (Twine(descriptionPrefix) + Twine(" Patch ") + hashString).str(),
                                     m_phases);
    }
    if (enableMask & (1 << TimerOpt)) {
      m_phaseTimers[TimerOpt].init("llpc-opt", (Twine(descriptionPrefix) + Twine(" Optimization ") + hashString).str(),
                                   m_phases);
    }

    if (enableMask & (1 << TimerCodeGen)) {
      m_phaseTimers[TimerCodeGen].init("llpc-codegen",
                                       (Twine(descriptionPrefix) + Twine(" CodeGen ") + hashString).str(), m_phases);
    }

    // Start whole timer
    m_wholeTimer.startTimer();
  }
}

// =====================================================================================================================
TimerProfiler::~TimerProfiler() {
  if (TimePassesIsEnabled || cl::EnableTimerProfile) {
    // Stop whole timer
    m_wholeTimer.stopTimer();
  }
}

// =====================================================================================================================
// Adds pass to start or stop timer in PassManager
//
// @param passMgr : Pass Manager
// @param timerKind : Kind of phase timer
// @param start : Start or  stop timer
void TimerProfiler::addTimerStartStopPass(lgc::PassManager &passMgr, TimerKind timerKind, bool start) {
  if (TimePassesIsEnabled || cl::EnableTimerProfile)
    lgc::LgcContext::createAndAddStartStopTimer(passMgr, &m_phaseTimers[timerKind], start);
}

// =====================================================================================================================
// Starts or stop specified timer.
//
// @param timerKind : Kind of phase timer
// @param start : Start or  stop timer
void TimerProfiler::startStopTimer(TimerKind timerKind, bool start) {
  if (TimePassesIsEnabled || cl::EnableTimerProfile) {
    if (start)
      m_phaseTimers[timerKind].startTimer();
    else
      m_phaseTimers[timerKind].stopTimer();
  }
}

// =====================================================================================================================
// Gets a specific timer. Returns nullptr if TimePassesIsEnabled isn't enabled.
//
// @param timerKind : Kind of phase timer
Timer *TimerProfiler::getTimer(TimerKind timerKind) {
  return TimePassesIsEnabled || cl::EnableTimerProfile ? &m_phaseTimers[timerKind] : nullptr;
}

// =====================================================================================================================
// Gets dummy TimeRecords.
const StringMap<TimeRecord> &TimerProfiler::getDummyTimeRecords() {
  static StringMap<TimeRecord> DummyTimeRecords;
  if ((TimePassesIsEnabled || cl::EnableTimerProfile) && DummyTimeRecords.empty()) {
    // NOTE: It is a workaround to get fixed layout in timer reports. Please remove it if we find a better solution.
    // LLVM timer skips the field if it is zero in all timers, it causes the layout of the report isn't stable when
    // compile multiple pipelines. so we add a dummy record to force all fields is shown.
    // But LLVM TimeRecord can't be initialized explicitly. We have to use HackedTimeRecord to force update the value
    // in TimeRecord.
    TimeRecord timeRecord;
    struct HackedTimeRecord {
      double t1;
      double t2;
      double t3;
      ssize_t m1;
      uint64_t i1;
    } hackedTimeRecord = {1e-100, 1e-100, 1e-100, 0, 0};
    static_assert(sizeof(timeRecord) == sizeof(hackedTimeRecord), "Unexpected Size!");
    memcpy(&timeRecord, &hackedTimeRecord, sizeof(TimeRecord));
    DummyTimeRecords["DUMMY"] = timeRecord;
  }

  return DummyTimeRecords;
}

} // namespace Llpc
