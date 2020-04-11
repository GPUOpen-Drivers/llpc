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
 * @file  FragColorExport.h
 * @brief LLPC header file: contains declaration of class lgc::FragColorExport.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/Pipeline.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/util/Internal.h"

namespace lgc {

class PipelineState;
struct WorkaroundFlags;

// Enumerates component setting of color format. This is a "helper" enum used in the CB's algorithm for deriving
// an ideal shader export format.
enum class CompSetting : unsigned {
  Invalid,         // Invalid
  OneCompRed,      // Red
  OneCompAlpha,    // Alpha
  TwoCompAlphaRed, // Alpha, red
  TwoCompGreenRed  // Green, red
};

// =====================================================================================================================
// Represents the manager of fragment color export operations.
class FragColorExport {
public:
  FragColorExport(PipelineState *pipelineState, llvm::Module *module);

  llvm::Value *run(llvm::Value *output, unsigned location, llvm::Instruction *insertPos);

  ExportFormat computeExportFormat(llvm::Type *outputTy, unsigned location) const;

private:
  FragColorExport() = delete;
  FragColorExport(const FragColorExport &) = delete;
  FragColorExport &operator=(const FragColorExport &) = delete;

  static CompSetting computeCompSetting(BufDataFormat dfmt);
  static unsigned getNumChannels(BufDataFormat dfmt);

  static bool hasAlpha(BufDataFormat dfmt);

  static unsigned getMaxComponentBitCount(BufDataFormat dfmt);

  llvm::Value *convertToFloat(llvm::Value *value, bool signedness, llvm::Instruction *insertPos) const;
  llvm::Value *convertToInt(llvm::Value *value, bool signedness, llvm::Instruction *insertPos) const;

  // -----------------------------------------------------------------------------------------------------------------

  PipelineState *m_pipelineState; // Pipeline state
  llvm::LLVMContext *m_context;   // LLVM context
};

} // namespace lgc
