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
 * @file  llpcInternal.cpp
 * @brief LLPC source file: contains implementation of LLPC internal-use utility functions.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-internal"

#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_os_ostream.h"
#include "spirvExt.h"

#if !defined(_WIN32)
    #include <sys/stat.h>
    #include <time.h>
    #include <unistd.h>
#endif

#if __APPLE__ && __MACH__
    #include <mach/mach_time.h>
#endif

#include "SPIRVInternal.h"
#include "llpcAbiMetadata.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcElf.h"
#include "llpcInternal.h"
#include "llpcPassLoopInfoCollect.h"

using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
// Gets the entry point (valid for AMD GPU) of a LLVM module.
Function* GetEntryPoint(
    Module* pModule) // [in] LLVM module
{
    Function* pEntryPoint = nullptr;

    for (auto pFunc = pModule->begin(), pEnd = pModule->end(); pFunc != pEnd; ++pFunc)
    {
        if ((pFunc->empty() == false) && (pFunc->getLinkage() == GlobalValue::ExternalLinkage))
        {
            pEntryPoint = &*pFunc;
            break;
        }
    }

    LLPC_ASSERT(pEntryPoint != nullptr);
    return pEntryPoint;
}

// =====================================================================================================================
// Emits a LLVM function call (inserted before the specified instruction), builds it automically based on return type
// and its parameters.
CallInst* EmitCall(
    Module*                       pModule,          // [in] LLVM module
    StringRef                     funcName,         // Name string of the function
    Type*                         pRetTy,           // [in] Return type
    ArrayRef<Value *>             args,             // [in] Parameters
    ArrayRef<Attribute::AttrKind> attribs,          // Attributes
    Instruction*                  pInsertPos)       // [in] Where to insert this call
{
    Function* pFunc = dyn_cast_or_null<Function>(pModule->getFunction(funcName));
    if (pFunc == nullptr)
    {
        std::vector<Type*> argTys;
        for (auto arg : args)
        {
            argTys.push_back(arg->getType());
        }

        auto pFuncTy = FunctionType::get(pRetTy, argTys, false);
        pFunc = Function::Create(pFuncTy, GlobalValue::ExternalLinkage, funcName, pModule);

        pFunc->setCallingConv(CallingConv::C);
        pFunc->addFnAttr(Attribute::NoUnwind);

        for (auto attrib : attribs)
        {
            pFunc->addFnAttr(attrib);
        }
    }

    auto pCallInst = CallInst::Create(pFunc, args, "", pInsertPos);
    pCallInst->setCallingConv(CallingConv::C);
    pCallInst->setAttributes(pFunc->getAttributes());

    return pCallInst;
}

// =====================================================================================================================
// Emits a LLVM function call (inserted at the end of the specified basic block), builds it automically based on return
// type and its parameters.
CallInst* EmitCall(
    Module*                       pModule,          // [in] LLVM module
    StringRef                     funcName,         // Name string of the function
    Type*                         pRetTy,           // [in] Return type
    ArrayRef<Value *>             args,             // [in] Parameters
    ArrayRef<Attribute::AttrKind> attribs,          // Attributes
    BasicBlock*                   pInsertAtEnd)     // [in] Which block to insert this call at the end
{
    Function* pFunc = dyn_cast_or_null<Function>(pModule->getFunction(funcName));
    if (pFunc == nullptr)
    {
        std::vector<Type*> argTys;
        for (auto arg : args)
        {
            argTys.push_back(arg->getType());
        }

        auto pFuncTy = FunctionType::get(pRetTy, argTys, false);
        pFunc = Function::Create(pFuncTy, GlobalValue::ExternalLinkage, funcName, pModule);

        pFunc->setCallingConv(CallingConv::C);
        pFunc->addFnAttr(Attribute::NoUnwind);

        for (auto attrib : attribs)
        {
            pFunc->addFnAttr(attrib);
        }
    }

    auto pCallInst = CallInst::Create(pFunc, args, "", pInsertAtEnd);
    pCallInst->setCallingConv(CallingConv::C);
    pCallInst->setAttributes(pFunc->getAttributes());

    return pCallInst;
}

// =====================================================================================================================
// Gets LLVM-style name for scalar or vector type.
static void GetTypeNameForScalarOrVector(
    Type*         pTy,         // [in] Type to get mangle name
    raw_ostream&  nameStream)  // [in,out] Stream to write the type name into
{
    if (auto pArrayTy = dyn_cast<ArrayType>(pTy))
    {
        nameStream << "a" << pArrayTy->getNumElements();
        pTy = pArrayTy->getElementType();
    }
    if (auto pVectorTy = dyn_cast<VectorType>(pTy))
    {
        nameStream << "v" << pVectorTy->getNumElements();
        pTy = pVectorTy->getElementType();
    }
    if (pTy->isFloatingPointTy())
    {
        nameStream << "f" << pTy->getScalarSizeInBits();
    }
    else if (pTy->isIntegerTy())
    {
        nameStream << "i" << pTy->getScalarSizeInBits();
    }
    else
    {
        LLPC_NEVER_CALLED();
    }
}

// =====================================================================================================================
// Gets LLVM-style name for scalar or vector type.
std::string GetTypeNameForScalarOrVector(
    Type* pTy)  // [in] Type to get mangle name
{
    std::string name;
    raw_string_ostream nameStream(name);

    GetTypeNameForScalarOrVector(pTy, nameStream);
    return nameStream.str();
}

// =====================================================================================================================
// Adds LLVM-style type mangling suffix for the specified return type and args to the name.
void AddTypeMangling(
    Type*            pReturnTy,     // [in] Return type (could be null)
    ArrayRef<Value*> args,          // Arguments
    std::string&     name)          // [out] String to add mangling to
{
    size_t nameLen = name.length();
    if (name[nameLen - 1] == '.')
    {
        // NOTE: If the specified name is ended with ".", we remove it in that mangling suffix starts with "." as well.
        name.erase(nameLen - 1, 1);
    }

    raw_string_ostream nameStream(name);
    if ((pReturnTy != nullptr) && (pReturnTy->isVoidTy() == false))
    {
        nameStream << ".";
        GetTypeNameForScalarOrVector(pReturnTy, nameStream);
    }

    for (auto pArg : args)
    {
        nameStream << ".";
        GetTypeNameForScalarOrVector(pArg->getType(), nameStream);
    }
}

// =====================================================================================================================
// Gets the shader stage from the specified single-shader LLVM module.
ShaderStage GetShaderStageFromModule(
    Module* pModule)  // [in] LLVM module
{
    return GetShaderStageFromFunction(GetEntryPoint(pModule));
}

// =====================================================================================================================
// Gets the shader stage from the specified LLVM function. Returns ShaderStageInvalid if not shader entrypoint.
ShaderStage GetShaderStageFromFunction(
    Function* pFunc)  // [in] LLVM function
{
    ShaderStage stage = ShaderStageInvalid;

    MDNode* pExecModelNode = pFunc->getMetadata(gSPIRVMD::ExecutionModel);
    if (pExecModelNode == nullptr)
    {
        return ShaderStageInvalid;
    }
    auto execModel = mdconst::dyn_extract<ConstantInt>(pExecModelNode->getOperand(0))->getZExtValue();

    switch (execModel)
    {
    case ExecutionModelVertex:
        stage = ShaderStageVertex;
        break;
    case ExecutionModelTessellationControl:
        stage = ShaderStageTessControl;
        break;
    case ExecutionModelTessellationEvaluation:
        stage = ShaderStageTessEval;
        break;
    case ExecutionModelGeometry:
        stage = ShaderStageGeometry;
        break;
    case ExecutionModelFragment:
        stage = ShaderStageFragment;
        break;
    case ExecutionModelGLCompute:
        stage = ShaderStageCompute;
        break;
    case ExecutionModelCopyShader:
        stage = ShaderStageCopyShader;
        break;
    default:
        stage = ShaderStageInvalid;
        break;
    }
    return stage;
}

// =====================================================================================================================
// Gets the argument from the specified function according to the argument index.
Value* GetFunctionArgument(
    Function* pFunc,    // [in] LLVM function
    uint32_t  idx)      // Index of the query argument
{
    auto pArg = pFunc->arg_begin();
    while (idx-- > 0)
    {
        ++pArg;
    }
    return &*pArg;
}

// =====================================================================================================================
// Checks if one type can be bitcasted to the other (type1 -> type2, valid for scalar or vector type).
bool CanBitCast(
    const Type* pTy1,   // [in] One type
    const Type* pTy2)   // [in] The other type
{
    bool valid = false;

    if (pTy1 == pTy2)
    {
        valid = true;
    }
    else if (pTy1->isSingleValueType() && pTy2->isSingleValueType())
    {
        const Type* pCompTy1 = pTy1->isVectorTy() ? pTy1->getVectorElementType() : pTy1;
        const Type* pCompTy2 = pTy2->isVectorTy() ? pTy2->getVectorElementType() : pTy2;
        if ((pCompTy1->isFloatingPointTy() || pCompTy1->isIntegerTy()) &&
            (pCompTy2->isFloatingPointTy() || pCompTy2->isIntegerTy()))
        {
            const uint32_t compCount1 = pTy1->isVectorTy() ? pTy1->getVectorNumElements() : 1;
            const uint32_t compCount2 = pTy2->isVectorTy() ? pTy2->getVectorNumElements() : 1;

            valid = (compCount1 * pCompTy1->getScalarSizeInBits() == compCount2 * pCompTy2->getScalarSizeInBits());
        }
    }

    return valid;
}

// =====================================================================================================================
// Checks whether input binary data is SPIR-V binary
bool IsSpirvBinary(
    const BinaryData*  pShaderBin)  // [in] Shader binary codes
{
    bool isSpvBinary = false;
    if (pShaderBin->codeSize > sizeof(SpirvHeader))
    {
        const SpirvHeader* pHeader = reinterpret_cast<const SpirvHeader*>(pShaderBin->pCode);
        if ((pHeader->magicNumber == MagicNumber) && (pHeader->spvVersion <= spv::Version) && (pHeader->reserved == 0))
        {
            isSpvBinary = true;
        }
    }

    return isSpvBinary;
}

// =====================================================================================================================
// Checks whether input binary data is LLVM bitcode.
bool IsLlvmBitcode(
    const BinaryData*  pShaderBin)  // [in] Shader binary codes
{
    static uint32_t BitcodeMagicNumber = 0xDEC04342; // 0x42, 0x43, 0xC0, 0xDE
    bool isLlvmBitcode = false;
    if ((pShaderBin->codeSize > 4) &&
        (*reinterpret_cast<const uint32_t*>(pShaderBin->pCode) == BitcodeMagicNumber))
    {
        isLlvmBitcode = true;
    }

    return isLlvmBitcode;
}

// =====================================================================================================================
// Gets the shader stage mask from the SPIR-V binary according to the specified entry-point.
//
// Returns 0 on error, or the stage mask of the specified entry-point on success.
uint32_t GetStageMaskFromSpirvBinary(
    const BinaryData*  pSpvBin,      // [in] SPIR-V binary
    const char*        pEntryName)   // [in] Name of entry-point
{
    uint32_t stageMask = 0;

    const uint32_t* pCode = reinterpret_cast<const uint32_t*>(pSpvBin->pCode);
    const uint32_t* pEnd  = pCode + pSpvBin->codeSize / sizeof(uint32_t);

    if (IsSpirvBinary(pSpvBin))
    {
        // Skip SPIR-V header
        const uint32_t* pCodePos = pCode + sizeof(SpirvHeader) / sizeof(uint32_t);

        while (pCodePos < pEnd)
        {
            uint32_t opCode = (pCodePos[0] & OpCodeMask);
            uint32_t wordCount = (pCodePos[0] >> WordCountShift);

            if ((wordCount == 0) || (pCodePos + wordCount > pEnd))
            {
                LLPC_ERRS("Invalid SPIR-V binary\n");
                stageMask = 0;
                break;
            }

            if (opCode == OpEntryPoint)
            {
                LLPC_ASSERT(wordCount >= 4);

                // The fourth word is start of the name string of the entry-point
                const char* pName = reinterpret_cast<const char*>(&pCodePos[3]);
                if (strcmp(pEntryName, pName) == 0)
                {
                    // An matching entry-point is found
                    stageMask |= ShaderStageToMask(static_cast<ShaderStage>(pCodePos[1]));
                }
            }

            // All "OpEntryPoint" are before "OpFunction"
            if (opCode == OpFunction)
            {
                break;
            }

            pCodePos += wordCount;
        }
    }
    else
    {
        LLPC_ERRS("Invalid SPIR-V binary\n");
    }

    return stageMask;
}

// =====================================================================================================================
// Verifies if the SPIR-V binary is valid and is supported
Result VerifySpirvBinary(
    const BinaryData* pSpvBin)  // [in] SPIR-V binary
{
    Result result = Result::Success;

#define _SPIRV_OP(x,...) Op##x,
    static const std::set<Op> OpSet{
       {
        #include "SPIRVOpCodeEnum.h"
       }
    };
#undef _SPIRV_OP

    const uint32_t* pCode = reinterpret_cast<const uint32_t*>(pSpvBin->pCode);
    const uint32_t* pEnd  = pCode + pSpvBin->codeSize / sizeof(uint32_t);

    // Skip SPIR-V header
    const uint32_t* pCodePos = pCode + sizeof(SpirvHeader) / sizeof(uint32_t);

    while (pCodePos < pEnd)
    {
        Op opCode = static_cast<Op>(pCodePos[0] & OpCodeMask);
        uint32_t wordCount = (pCodePos[0] >> WordCountShift);

        if ((wordCount == 0) || (pCodePos + wordCount > pEnd))
        {
            result = Result::ErrorInvalidShader;
            break;
        }

        if (OpSet.find(opCode) == OpSet.end())
        {
            result = Result::ErrorInvalidShader;
            break;
        }

        pCodePos += wordCount;
    }

    return result;
}

// =====================================================================================================================
// Checks if the specified value actually represents a don't-care value (0xFFFFFFFF).
bool IsDontCareValue(
    Value* pValue) // [in] Value to check
{
    bool isDontCare = false;

    if (isa<ConstantInt>(pValue))
    {
        isDontCare = (static_cast<uint32_t>(cast<ConstantInt>(pValue)->getZExtValue()) == InvalidValue);
    }

    return isDontCare;
}

// =====================================================================================================================
// Translates an integer to 32-bit integer regardless of its initial bit width.
Value* ToInt32Value(
    Context*     pContext,   // [in] LLPC context
    Value*       pValue,     // [in] Value to be translated
    Instruction* pInsertPos) // [in] Where to insert the translation instructions
{
    LLPC_ASSERT(isa<IntegerType>(pValue->getType()));
    auto pValueTy = cast<IntegerType>(pValue->getType());

    const uint32_t bitWidth = pValueTy->getBitWidth();
    if (bitWidth > 32)
    {
        // Truncated to i32 type
        pValue = CastInst::CreateTruncOrBitCast(pValue, pContext->Int32Ty(), "", pInsertPos);
    }
    else if (bitWidth < 32)
    {
        // Extended to i32 type
        pValue = CastInst::CreateZExtOrBitCast(pValue, pContext->Int32Ty(), "", pInsertPos);
    }

    return pValue;
}

// =====================================================================================================================
// Checks whether "pValue" is a non-uniform value. Also, add it to the set of already-checked values.
bool IsNonUniformValue(
    Value*                      pValue,        // [in] Pointer to the value need to be checked
    std::unordered_set<Value*>& checkedValues) // [in,out] Values which are checked
{
    if ((pValue != nullptr) && isa<Instruction>(pValue))
    {
        // Check value in set checkedValues to avoid infinite recusive
        if (checkedValues.find(pValue) == checkedValues.end())
        {
            checkedValues.insert(pValue);

            // Check metedata in current instructions
            auto pInst = cast<Instruction>(pValue);
            if (pInst->getMetadata(gSPIRVMD::NonUniform) != nullptr)
            {
                return true;
            }

            // Check metedata for each operands
            for (auto& operand : pInst->operands())
            {
                if (isa<Instruction>(&operand))
                {
                    auto pOperand = cast<Instruction>(&operand);
                    if ((pOperand != pInst) && IsNonUniformValue(pOperand, checkedValues))
                    {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

// =====================================================================================================================
// Checks whether the input data is actually a ELF binary
bool IsElfBinary(
    const void* pData,    // [in] Input data to check
    size_t      dataSize) // Size of the input data
{
    bool isElfBin = false;
    if (dataSize >= sizeof(Elf64::FormatHeader))
    {
        auto pHeader = reinterpret_cast<const Elf64::FormatHeader*>(pData);
        isElfBin = pHeader->e_ident32[EI_MAG0] == ElfMagic;
    }
    return isElfBin;
}

// =====================================================================================================================
// Checks whether the output data is actually ISA assembler text
bool IsIsaText(
    const void* pData,    // [in] Input data to check
    size_t      dataSize) // Size of the input data
{
    // This is called by amdllpc to help distinguish between its three output types of ELF binary, LLVM IR assembler
    // and ISA assembler. Here we use the fact that ISA assembler is the only one that starts with a tab character.
    return (dataSize != 0) && ((reinterpret_cast<const char*>(pData))[0] == '\t');
}

} // Llpc
