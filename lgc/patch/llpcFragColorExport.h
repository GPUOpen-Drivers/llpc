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
 * @file  llpcFragColorExport.h
 * @brief LLPC header file: contains declaration of class lgc::FragColorExport.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcInternal.h"
#include "llpcIntrinsDefs.h"
#include "lgc/llpcPipeline.h"

namespace lgc
{

class PipelineState;
struct WorkaroundFlags;

// Enumerates component setting of color format. This is a "helper" enum used in the CB's algorithm for deriving
// an ideal shader export format.
enum class CompSetting : uint32_t
{
    Invalid,            // Invalid
    OneCompRed,         // Red
    OneCompAlpha,       // Alpha
    TwoCompAlphaRed,    // Alpha, red
    TwoCompGreenRed     // Green, red
};

// =====================================================================================================================
// Represents the manager of fragment color export operations.
class FragColorExport
{
public:
    FragColorExport(PipelineState* pPipelineState, llvm::Module* pModule);

    llvm::Value* Run(llvm::Value* pOutput, uint32_t location, llvm::Instruction* pInsertPos);

    ExportFormat ComputeExportFormat(llvm::Type* pOutputTy, uint32_t location) const;

private:
    FragColorExport() = delete;
    FragColorExport(const FragColorExport&) = delete;
    FragColorExport& operator =(const FragColorExport&) = delete;

    static CompSetting ComputeCompSetting(BufDataFormat dfmt);
    static uint32_t GetNumChannels(BufDataFormat dfmt);

    static bool HasAlpha(BufDataFormat dfmt);

    static uint32_t GetMaxComponentBitCount(BufDataFormat dfmt);

    llvm::Value* ConvertToFloat(llvm::Value* pValue, bool signedness, llvm::Instruction* pInsertPos) const;
    llvm::Value* ConvertToInt(llvm::Value* pValue, bool signedness, llvm::Instruction* pInsertPos) const;

    // -----------------------------------------------------------------------------------------------------------------

    PipelineState*  m_pPipelineState;   // Pipeline state
    llvm::LLVMContext*        m_pContext;         // LLVM context
};

} // lgc
