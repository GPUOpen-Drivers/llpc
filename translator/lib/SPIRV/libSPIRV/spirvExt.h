//===- SPIRVReader.cpp - Converts SPIR-V to LLVM ----------------*- C++ -*-===//
//
//                     The LLVM/SPIR-V Translator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright (c) 2014 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimers in the documentation
// and/or other materials provided with the distribution.
// Neither the names of Advanced Micro Devices, Inc., nor the names of its
// contributors may be used to endorse or promote products derived from this
// Software without specific prior written permission.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file contains SPIRV header and internal enums
///
//===----------------------------------------------------------------------===//
#ifndef SPIRVEXT_H
#define SPIRVEXT_H

#include "spirv.hpp"
#include "GLSL.std.450.h"

namespace spv
{
  #include "GLSL.ext.AMD.h"

  static const LinkageType LinkageTypeInternal =
      static_cast<LinkageType>(LinkageTypeImport + 1);

  static const Op OpForward = static_cast<Op>(1024);

  // Built-ins for per-vertex structure
  static const BuiltIn BuiltInPerVertex             = static_cast<BuiltIn>(1024);

  // Built-ins for fragment input interpolation (I/J)
  static const BuiltIn BuiltInInterpPerspSample     = static_cast<BuiltIn>(0x10000000);
  static const BuiltIn BuiltInInterpPerspCenter     = static_cast<BuiltIn>(0x10000001);
  static const BuiltIn BuiltInInterpPerspCentroid   = static_cast<BuiltIn>(0x10000002);
  static const BuiltIn BuiltInInterpPullMode        = static_cast<BuiltIn>(0x10000003);
  static const BuiltIn BuiltInInterpLinearSample    = static_cast<BuiltIn>(0x10000004);
  static const BuiltIn BuiltInInterpLinearCenter    = static_cast<BuiltIn>(0x10000005);
  static const BuiltIn BuiltInInterpLinearCentroid  = static_cast<BuiltIn>(0x10000006);

  // Built-ins for sample position emulation
  static const BuiltIn BuiltInSamplePosOffset       = static_cast<BuiltIn>(0x10000007);
  static const BuiltIn BuiltInNumSamples            = static_cast<BuiltIn>(0x10000008);
  static const BuiltIn BuiltInSamplePatternIdx      = static_cast<BuiltIn>(0x10000009);
  static const BuiltIn BuiltInWaveId                = static_cast<BuiltIn>(0x1000000A);

  // Execution model: copy shader
  static const ExecutionModel ExecutionModelCopyShader = static_cast<ExecutionModel>(1024);

}; // spv

#endif // SPIRVEXT_H
