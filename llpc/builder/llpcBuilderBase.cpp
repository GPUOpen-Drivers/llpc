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
 * @file  llpcBuilderBase.cpp
 * @brief LLPC source file: implementation of BuilderBase
 ***********************************************************************************************************************
 */
#include "llpcBuilderBase.h"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
// Create an LLVM function call to the named function. The callee is built automically based on return
// type and its parameters.
CallInst* BuilderBase::CreateNamedCall(
    StringRef                     funcName, // Name of the callee
    Type*                         pRetTy,   // [in] Return type of the callee
    ArrayRef<Value *>             args,     // Arguments to pass to the callee
    ArrayRef<Attribute::AttrKind> attribs)  // Function attributes
{
    Module* pModule = GetInsertBlock()->getParent()->getParent();
    Function* pFunc = dyn_cast_or_null<Function>(pModule->getFunction(funcName));
    if (!pFunc)
    {
        SmallVector<Type*, 8> argTys;
        argTys.reserve(args.size());
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

    auto pCall = CreateCall(pFunc, args);
    pCall->setCallingConv(CallingConv::C);
    pCall->setAttributes(pFunc->getAttributes());

    return pCall;
}

