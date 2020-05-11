/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  VertexFetch.h
 * @brief LLPC header file: contains declaration of class lgc::VertexFetch.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/Pipeline.h"

namespace lgc {

class BuilderBase;

// =====================================================================================================================
// Public interface to vertex fetch manager.
class VertexFetch {
public:
  virtual ~VertexFetch() {}

  // Create a VertexFetch
  static VertexFetch *create(LgcContext *lgcContext);

  // Given a non-aggregate type, get a float type at least as big that can be used to pass a value of that
  // type in a return value struct, ensuring it gets into VGPRs.
  static llvm::Type *getVgprTy(llvm::Type *ty);

  // Generate code to fetch a vertex value
  virtual llvm::Value *fetchVertex(llvm::Type *inputTy, const VertexInputDescription *description, unsigned location,
                                   unsigned compIdx, BuilderBase &builder) = 0;
};

} // namespace lgc
