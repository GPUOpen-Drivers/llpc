/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcDebug.cpp
 * @brief LLPC source file: contains implementation of LLPC debug utility functions.
 ***********************************************************************************************************************
 */
#include "llpcDebug.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "llpc-debug"

namespace llvm {

namespace cl {

// -enable-outs: enable general message output (to stdout or external file).
opt<bool> EnableOuts("enable-outs",
                     desc("Enable LLPC-specific debug dump output (to stdout or external file) (default: false)"),
                     init(false));

// -v: alias for -enable-outs
opt<bool> Verbose("v", desc("Enable LLPC-specific debug dump output (to stdout or external file) (default: false)"),
                  init(false));

// -enable-errs: enable error message output (to stderr or external file).
opt<bool> EnableErrs("enable-errs", desc("Enable error message output (to stdout or external file) (default: true)"),
                     init(true));

// -log-file-dbgs: name of the file to log info from dbg()
opt<std::string> LogFileDbgs("log-file-dbgs", desc("Name of the file to log info from dbgs()"), value_desc("filename"),
                             init(""));

// -log-file-outs: name of the file to log info from LLPC_OUTS() and LLPC_ERRS()
opt<std::string> LogFileOuts("log-file-outs", desc("Name of the file to log info from LLPC_OUTS() and LLPC_ERRS()"),
                             value_desc("filename"), init(""));

} // namespace cl

} // namespace llvm

using namespace llvm;

namespace Llpc {
// =====================================================================================================================
// Gets the value of option "allow-out".
bool EnableOuts() {
  return cl::EnableOuts || cl::Verbose;
}

// =====================================================================================================================
// Gets the value of option "allow-err".
bool EnableErrs() {
  return cl::EnableErrs;
}

// =====================================================================================================================
// Redirects the output of logs. It affects the behavior of llvm::outs(), dbgs() and errs().
//
// NOTE: This function redirects log output by modify the underlying static raw_fd_ostream object in outs() and errs().
// With this method, we can redirect logs in all environments, include both standalone compiler and Vulkan ICD. and we
// can restore the output on all platform, which is very useful when app crashes or hits an assert.
// CAUTION: The behavior isn't changed if app outputs logs to STDOUT or STDERR directly.
//
// @param restoreToDefault : Restore the default behavior of outs() and errs() if it is true
// @param optionCount : Count of compilation-option strings
// @param options : An array of compilation-option strings
void redirectLogOutput(bool restoreToDefault, unsigned optionCount, const char *const *options) {
  static raw_fd_ostream *DbgFile = nullptr;
  static raw_fd_ostream *OutFile = nullptr;
  static uint8_t DbgFileBak[sizeof(raw_fd_ostream)] = {};
  static uint8_t OutFileBak[sizeof(raw_fd_ostream)] = {};

  if (restoreToDefault) {
    // Restore default raw_fd_ostream objects
    if (DbgFile) {
      memcpy((void *)&errs(), DbgFileBak, sizeof(raw_fd_ostream));
      DbgFile->close();
      DbgFile = nullptr;
    }

    if (OutFile) {
      memcpy((void *)&outs(), OutFileBak, sizeof(raw_fd_ostream));
      OutFile->close();
      OutFile = nullptr;
    }
  } else {
    // Redirect errs() for dbgs()
    if (!cl::LogFileDbgs.empty()) {
      // NOTE: Checks whether errs() is needed in compilation
      // Until now, option -debug, -debug-only and -print-* need use debug output
      bool needDebugOut = ::llvm::DebugFlag;
      for (unsigned i = 1; !needDebugOut && i < optionCount; ++i) {
        StringRef option = options[i];
        if (option.startswith("-debug") || option.startswith("-print"))
          needDebugOut = true;
      }

      if (needDebugOut) {
        std::error_code errCode;

        static raw_fd_ostream NewDbgFile(cl::LogFileDbgs.c_str(), errCode, sys::fs::OF_Text);
        assert(!errCode);
        if (!DbgFile) {
          NewDbgFile.SetUnbuffered();
          memcpy((void *)DbgFileBak, (void *)&errs(), sizeof(raw_fd_ostream));
          memcpy((void *)&errs(), (void *)&NewDbgFile, sizeof(raw_fd_ostream));
          DbgFile = &NewDbgFile;
        }
      }
    }

    // Redirect outs() for LLPC_OUTS() and LLPC_ERRS()
    if ((cl::EnableOuts || cl::EnableErrs) && !cl::LogFileOuts.empty()) {
      if (cl::LogFileOuts == cl::LogFileDbgs && DbgFile) {
        memcpy((void *)OutFileBak, (void *)&outs(), sizeof(raw_fd_ostream));
        memcpy((void *)&outs(), (void *)DbgFile, sizeof(raw_fd_ostream));
        OutFile = DbgFile;
      } else {
        std::error_code errCode;

        static raw_fd_ostream NewOutFile(cl::LogFileOuts.c_str(), errCode, sys::fs::OF_Text);
        assert(!errCode);
        if (!OutFile) {
          NewOutFile.SetUnbuffered();
          memcpy((void *)OutFileBak, (void *)&outs(), sizeof(raw_fd_ostream));
          memcpy((void *)&outs(), (void *)&NewOutFile, sizeof(raw_fd_ostream));
          OutFile = &NewOutFile;
        }
      }
    }
  }
}

// =====================================================================================================================
// Enables/disables the output for debugging. TRUE for enable, FALSE for disable.
//
// @param restore : Whether to enable debug output
void enableDebugOutput(bool restore) {
  static raw_null_ostream NullStream;
  static uint8_t DbgStream[sizeof(raw_fd_ostream)] = {};

  if (restore) {
    // Restore default raw_fd_ostream objects
    memcpy((void *)&errs(), DbgStream, sizeof(raw_fd_ostream));
  } else {
    // Redirect errs() for dbgs()
    memcpy((void *)DbgStream, (void *)&errs(), sizeof(raw_fd_ostream));
    memcpy((void *)&errs(), (void *)&NullStream, sizeof(NullStream));
  }
}

} // namespace Llpc
