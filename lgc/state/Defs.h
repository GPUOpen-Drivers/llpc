/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  state/Defs.h
 * @brief LLPC header file: LGC internal definitions
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/BuiltIns.h"

namespace lgc {

// Internal built-ins for fragment input interpolation (I/J)
static const BuiltInKind BuiltInInterpPerspSample = static_cast<BuiltInKind>(0x10000000);
static const BuiltInKind BuiltInInterpPerspCenter = static_cast<BuiltInKind>(0x10000001);
static const BuiltInKind BuiltInInterpPerspCentroid = static_cast<BuiltInKind>(0x10000002);
static const BuiltInKind BuiltInInterpPullMode = static_cast<BuiltInKind>(0x10000003);
static const BuiltInKind BuiltInInterpLinearSample = static_cast<BuiltInKind>(0x10000004);
static const BuiltInKind BuiltInInterpLinearCenter = static_cast<BuiltInKind>(0x10000005);
static const BuiltInKind BuiltInInterpLinearCentroid = static_cast<BuiltInKind>(0x10000006);

// Internal built-ins for sample position emulation
static const BuiltInKind BuiltInSamplePosOffset = static_cast<BuiltInKind>(0x10000007);
static const BuiltInKind BuiltInNumSamples = static_cast<BuiltInKind>(0x10000008);
static const BuiltInKind BuiltInSamplePatternIdx = static_cast<BuiltInKind>(0x10000009);
static const BuiltInKind BuiltInWaveId = static_cast<BuiltInKind>(0x1000000A);

} // namespace lgc
