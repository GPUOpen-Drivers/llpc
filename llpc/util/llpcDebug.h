/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcDebug.h
 * @brief LLPC header file: contains definitions of LLPC debug utilities.
 ***********************************************************************************************************************
 */
#pragma once

#include <stdint.h>

// Output error message
#define LLPC_ERRS(_msg) { if (EnableErrs()) { outs() << "ERROR: " << _msg; outs().flush(); } }

// Output general message
#define LLPC_OUTS(_msg) { if (EnableOuts()) { outs() << _msg; } }

namespace llvm { class raw_ostream; }
namespace llvm { class raw_fd_ostream; }

namespace MetroHash { struct Hash; };

namespace Llpc
{

// Gets the value of option "enable-outs"
bool EnableOuts();

// Gets the value of option "enable-errs"
bool EnableErrs();

// Redirects the output of logs, It affects the behavior of llvm::outs(), dbgs() and errs().
void redirectLogOutput(
    bool              restoreToDefault,
    unsigned          optionCount,
    const char*const* options);

// Enable/disable the output for debugging.
void enableDebugOutput(bool restore);

} // Llpc
