/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  GpurtDialect.cpp
 * @brief Implementation of the LGC dialect definition
 ***********************************************************************************************************************
 */

#include "lgc/GpurtDialect.h"

#define GET_INCLUDES
#define GET_DIALECT_DEFS
#include "GpurtDialect.cpp.inc"

using namespace llvm;

namespace lgc::gpurt {
constexpr const char KnownSetRayFlagsMetadata[] = "lgc.gpurt.knownSetRayFlags";
constexpr const char KnownUnsetRayFlagsMetadata[] = "lgc.gpurt.knownUnsetRayFlags";

void setKnownSetRayFlags(Module &module, unsigned flags) {
  auto *md = module.getOrInsertNamedMetadata(KnownSetRayFlagsMetadata);
  assert(md && "Failed to create metadata node!");
  md->clearOperands();
  md->addOperand(MDNode::get(
      module.getContext(), {ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(module.getContext()), flags))}));
}

void setKnownUnsetRayFlags(Module &module, unsigned flags) {
  auto *md = module.getOrInsertNamedMetadata(KnownUnsetRayFlagsMetadata);
  assert(md && "Failed to create metadata node!");
  md->clearOperands();
  md->addOperand(MDNode::get(
      module.getContext(), {ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(module.getContext()), flags))}));
}

unsigned getKnownSetRayFlags(const Module &module) {
  auto *md = module.getNamedMetadata(KnownSetRayFlagsMetadata);
  if (!md)
    return 0;
  return mdconst::extract<ConstantInt>(md->getOperand(0)->getOperand(0))->getZExtValue();
}

unsigned getKnownUnsetRayFlags(const Module &module) {
  auto *md = module.getNamedMetadata(KnownUnsetRayFlagsMetadata);
  if (!md)
    return 0;
  return mdconst::extract<ConstantInt>(md->getOperand(0)->getOperand(0))->getZExtValue();
}

} // namespace lgc::gpurt
