//===- SPIRV.h ?Read and write SPIR-V binary -------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
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
/// \file SPIRV.h
///
/// This files declares functions and passes for translating between LLVM and
/// SPIR-V.
///
///
//===----------------------------------------------------------------------===//
#ifndef LLVM_SUPPORT_SPIRV_H
#define LLVM_SUPPORT_SPIRV_H

#include "spirvExt.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include <iostream>
#include <map>
#include <string>

namespace SPIRV {
class SPIRVModule;

/// \brief Represents one entry in specialization constant map.
struct SPIRVSpecConstEntry {
  uint32_t DataSize; // Data size of specilization constant (in bytes)
  const void *Data;  // Data of specilization constant
};

/// \brief Represents the map from SpecId to specialization constant data.
typedef std::map<uint32_t, SPIRVSpecConstEntry> SPIRVSpecConstMap;

// A converting sampler with a constant value (or indexable array thereof).
struct ConvertingSampler {
  unsigned set;                    // Descriptor set
  unsigned binding;                // Binding
  llvm::ArrayRef<uint32_t> values; // Values; 10 uint32_t per array entry
};

static const unsigned ConvertingSamplerDwordCount = 10;

/// \brief Check if a string contains SPIR-V binary.
bool IsSPIRVBinary(std::string &Img);

} // End namespace SPIRV

namespace lgc {
class Builder;
} // namespace lgc

namespace Vkgc {
struct ShaderModuleUsage;
struct PipelineShaderOptions;
} // End namespace Vkgc

namespace llvm {

class raw_pwrite_stream;
/// \brief Translate LLVM module to SPIRV and write to ostream.
/// @returns : True if succeeds.
bool writeSpirv(llvm::Module *M, llvm::raw_ostream &OS, std::string &ErrMsg);

/// \brief Load SPIRV from istream and translate to LLVM module.
/// @returns : True if succeeds.
bool readSpirv(lgc::Builder *Builder, const Vkgc::ShaderModuleUsage *ModuleData,
               const Vkgc::PipelineShaderOptions *ShaderOptions, std::istream &IS, spv::ExecutionModel EntryExecModel,
               const char *EntryName, const SPIRV::SPIRVSpecConstMap &SpecConstMap,
               llvm::ArrayRef<SPIRV::ConvertingSampler> ConvertingSamplers, llvm::Module *M, std::string &ErrMsg);

/// \brief Regularize LLVM module by removing entities not representable by
/// SPIRV.
bool regularizeLlvmForSpirv(llvm::Module *M, std::string &ErrMsg);

} // namespace llvm

#endif
