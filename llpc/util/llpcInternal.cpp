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
 * @file  llpcInternal.cpp
 * @brief LLPC source file: contains implementation of LLPC internal-use utility functions.
 ***********************************************************************************************************************
 */
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

#include "llpcBuilderBase.h"
#include "llpcInternal.h"

#define DEBUG_TYPE "llpc-internal"

using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
// Emits a LLVM function call (inserted before the specified instruction), builds it automically based on return type
// and its parameters.
//
// NOTE: Prefer BuilderBase::CreateNamedCall where possible.
CallInst* EmitCall(
    StringRef                     funcName,         // Name string of the function
    Type*                         pRetTy,           // [in] Return type
    ArrayRef<Value *>             args,             // [in] Parameters
    ArrayRef<Attribute::AttrKind> attribs,          // Attributes
    Instruction*                  pInsertPos)       // [in] Where to insert this call
{
    BuilderBase builder(pInsertPos);
    return builder.CreateNamedCall(funcName, pRetTy, args, attribs);
}

// =====================================================================================================================
// Emits a LLVM function call (inserted at the end of the specified basic block), builds it automically based on return
// type and its parameters.
//
// NOTE: Prefer BuilderBase::CreateNamedCall where possible.
CallInst* EmitCall(
    StringRef                     funcName,         // Name string of the function
    Type*                         pRetTy,           // [in] Return type
    ArrayRef<Value *>             args,             // [in] Parameters
    ArrayRef<Attribute::AttrKind> attribs,          // Attributes
    BasicBlock*                   pInsertAtEnd)     // [in] Which block to insert this call at the end
{
    BuilderBase builder(pInsertAtEnd);
    return builder.CreateNamedCall(funcName, pRetTy, args, attribs);
}

// =====================================================================================================================
// Gets LLVM-style name for type.
void GetTypeName(
    Type*         pTy,         // [in] Type to get mangle name
    raw_ostream&  nameStream)  // [in,out] Stream to write the type name into
{
    for (;;)
    {
        if (auto pPointerTy = dyn_cast<PointerType>(pTy))
        {
            nameStream << "p" << pPointerTy->getAddressSpace();
            pTy = pPointerTy->getElementType();
            continue;
        }
        if (auto pArrayTy = dyn_cast<ArrayType>(pTy))
        {
            nameStream << "a" << pArrayTy->getNumElements();
            pTy = pArrayTy->getElementType();
            continue;
        }
        break;
    }

    if (auto pStructTy = dyn_cast<StructType>(pTy))
    {
        nameStream << "s[";
        if (pStructTy->getNumElements() != 0)
        {
            GetTypeName(pStructTy->getElementType(0), nameStream);
            for (uint32_t i = 1; i < pStructTy->getNumElements(); ++i)
            {
                nameStream << ",";
                GetTypeName(pStructTy->getElementType(i), nameStream);
            }
        }
        nameStream << "]";
        return;
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
    else if (pTy->isVoidTy())
    {
        nameStream << "V";
    }
    else
    {
        llvm_unreachable("Should never be called!");
    }
}

// =====================================================================================================================
// Gets LLVM-style name for type.
std::string GetTypeName(
    Type* pTy)  // [in] Type to get mangle name
{
    std::string name;
    raw_string_ostream nameStream(name);

    GetTypeName(pTy, nameStream);
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
        GetTypeName(pReturnTy, nameStream);
    }

    for (auto pArg : args)
    {
        nameStream << ".";
        GetTypeName(pArg->getType(), nameStream);
    }
}

// =====================================================================================================================
// Gets the shader stage from the specified LLVM function. Returns ShaderStageInvalid if not shader entrypoint.
ShaderStage GetShaderStageFromFunction(
    const Function* pFunc)  // [in] LLVM function
{
    // Check for the metadata that is added by the builder. This works in the patch phase.
    MDNode* pStageMetaNode = pFunc->getMetadata(LlpcName::ShaderStageMetadata);
    if (pStageMetaNode != nullptr)
    {
        return ShaderStage(mdconst::dyn_extract<ConstantInt>(pStageMetaNode->getOperand(0))->getZExtValue());
    }
    return ShaderStageInvalid;
}

// =====================================================================================================================
// Gets the shader stage from the specified calling convention.
ShaderStage GetShaderStageFromCallingConv(
    uint32_t        stageMask,  // Shader stage mask for the pipeline
    CallingConv::ID callConv)   // Calling convention
{
    ShaderStage shaderStage = ShaderStageInvalid;

    bool hasGs = (stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0;
    bool hasTs = (((stageMask & ShaderStageToMask(ShaderStageTessControl)) != 0) ||
                  ((stageMask & ShaderStageToMask(ShaderStageTessEval)) != 0));

    switch (callConv)
    {
    case CallingConv::AMDGPU_PS:
        shaderStage = ShaderStageFragment;
        break;
    case CallingConv::AMDGPU_LS:
        shaderStage = ShaderStageVertex;
        break;
    case CallingConv::AMDGPU_HS:
        shaderStage = ShaderStageTessControl;
        break;
    case CallingConv::AMDGPU_ES:
        shaderStage = hasTs ? ShaderStageTessEval : ShaderStageVertex;
        break;
    case CallingConv::AMDGPU_GS:
        // NOTE: If GS is not present, this must be NGG.
        shaderStage = hasGs ? ShaderStageGeometry : (hasTs ? ShaderStageTessEval : ShaderStageVertex);
        break;
    case CallingConv::AMDGPU_VS:
        shaderStage = hasGs ? ShaderStageCopyShader : (hasTs ? ShaderStageTessEval : ShaderStageVertex);
        break;
    case CallingConv::AMDGPU_CS:
        shaderStage = ShaderStageCompute;
        break;
    default:
        llvm_unreachable("Should never be called!");
        break;
    }

    return shaderStage;
}

// =====================================================================================================================
// Gets the argument from the specified function according to the argument index.
Value* GetFunctionArgument(
    Function*     pFunc,    // [in] LLVM function
    uint32_t      idx,      // Index of the query argument
    const Twine&  name)     // Name to give the argument if currently empty
{
    Argument* pArg = &pFunc->arg_begin()[idx];
    if ((name.isTriviallyEmpty() == false) && (pArg->getName() == ""))
    {
        pArg->setName(name);
    }
    return pArg;
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

} // Llpc
