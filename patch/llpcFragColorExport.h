/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @brief LLPC header file: contains declaration of class Llpc::FragColorExport.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcBuilder.h"
#include "llpcInternal.h"
#include "llpcIntrinsDefs.h"

namespace Llpc
{

class Context;
class PipelineState;

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
    FragColorExport(PipelineState* pPipelineState);

    llvm::Value* Run(llvm::Value* pOutput, uint32_t location, llvm::Instruction* pInsertPos);

private:
    LLPC_DISALLOW_DEFAULT_CTOR(FragColorExport);
    LLPC_DISALLOW_COPY_AND_ASSIGN(FragColorExport);

    ExportFormat ComputeExportFormat(llvm::Type* pOutputTy, uint32_t location) const;
    CompSetting ComputeCompSetting(Builder::BufDataFormat dfmt) const;
    uint32_t GetNumChannels(Builder::BufDataFormat dfmt) const;

    bool HasAlpha(Builder::BufDataFormat dfmt) const;

    uint32_t GetMaxComponentBitCount(Builder::BufDataFormat dfmt) const;

    llvm::Value* ConvertToFloat(llvm::Value* pValue, bool signedness, llvm::Instruction* pInsertPos) const;
    llvm::Value* ConvertToInt(llvm::Value* pValue, bool signedness, llvm::Instruction* pInsertPos) const;

    // -----------------------------------------------------------------------------------------------------------------

    llvm::Module*   m_pModule;          // LLVM module
    Context*        m_pContext;         // LLPC context

    PipelineState*                   m_pPipelineState;  // Pipeline state
};

} // Llpc
