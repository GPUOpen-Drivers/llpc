/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcEmuLib.cpp
 * @brief LLPC source file: contains implementation of class Llpc::EmuLib.
 ***********************************************************************************************************************
 */
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/Archive.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"

#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcEmuLib.h"

#include "SPIRVInternal.h"

#define DEBUG_TYPE "llpc-emu-lib"

using namespace Llpc;
using namespace llvm;
using namespace object;

// =====================================================================================================================
// Adds an archive to the emulation library.
void EmuLib::addArchive(
    MemoryBufferRef buffer) // Buffer required to create the archive
{
    m_archives.emplace_back(cantFail(Archive::create(buffer), "Failed to parse archive"));

    // Update symbol index in the symbol index map
    auto& archive = m_archives.back();
    auto index = m_archives.size() - 1;
    for (auto& symbol : archive.archive->symbols())
        m_symbolIndices.insert(std::make_pair(symbol.getName(), index));
}

// =====================================================================================================================
// Gets a function from the emulation library.
//
// Returns nullptr if not found, or if it is not a native function when nativeOnly is true.
Function* EmuLib::getFunction(
    StringRef funcName, // Function name to find
    bool nativeOnly)    // Whether to only find a native function
{
    auto symbolIndexIt = m_symbolIndices.find(funcName);
    if (symbolIndexIt != m_symbolIndices.end())
    {
        auto& archive = m_archives[symbolIndexIt->second];

        auto funcMapIt = archive.functions.find(funcName);
        if (funcMapIt != archive.functions.end())
        {
            // Function is already in the function map.
            if (nativeOnly && (!funcMapIt->second.isNative))
                return nullptr;
            return funcMapIt->second.function;
        }
        // Find the function in the symbol table of the archive.
        auto child = cantFail(archive.archive->findSym(funcName), "Failed in archive symbol search");
        assert(child.hasValue());
        // Found the symbol. Get the bitcode for its module.
        StringRef childBitcode = cantFail(child->getBuffer(), "Failed in archive module extraction");

        // Parse the bitcode archive member into a Module.
        auto libModule = cantFail(parseBitcodeFile(
            MemoryBufferRef(childBitcode, ""), *m_context), "Failed to parse archive bitcode");

        // Find and mark the non-native library functions. A library function is non-native if:
        //   it references llvm.amdgcn.*
        //   it references llpc.* and it isn't implemented in the library
        //   it is unpackHalf2x16i*
        std::unordered_set<Function*> nonNativeFuncs;
        std::unordered_map<Function*, std::vector<Function*>> unknownKindFuncs;
        for (auto& libFunc : *libModule)
        {
            if (libFunc.isDeclaration())
            {
                auto libFuncName = libFunc.getName();

                if (libFuncName.startswith("llvm.amdgcn."))
                {
                    for (auto user : libFunc.users())
                    {
                        auto inst = dyn_cast<Instruction>(user);
                        auto nonNativeFunc = inst->getParent()->getParent();
                        nonNativeFuncs.insert(nonNativeFunc);
                    }
                }
                else if (libFuncName.startswith("llpc."))
                {
                    for (auto user : libFunc.users())
                    {
                        auto inst = dyn_cast<Instruction>(user);
                        auto unknownKindFunc = inst->getParent()->getParent();
                        unknownKindFuncs[unknownKindFunc].push_back(&libFunc);
                    }
                }
            }

            // NOTE: It is to pass CTS floating point control test. If input is constant, LLVM inline pass will do
            // constant folding for this function, and it will causes floating point control doesn't work correctly.
            if (libFunc.getName().startswith(gSPIRVName::UnpackHalf2x16))
                nonNativeFuncs.insert(&libFunc);
        }

        // Add the new module's defined functions to the function map for this archive.
        Function* requestedFunc = nullptr;
        for (auto& libFunc : *libModule)
        {
            if (!libFunc.empty())
            {
                bool isNative = nonNativeFuncs.find(&libFunc) == nonNativeFuncs.end();
                if (!isNative)
                {
                    // Non-native if it is in non-native list
                    archive.functions[libFunc.getName()] = EmuLibFunction(&libFunc, false);
                }
                else
                {
                    auto funcIt = unknownKindFuncs.find(&libFunc);
                    if (funcIt == unknownKindFuncs.end())
                    {
                        // Native if isn't in non-native list and unknown list
                        archive.functions[libFunc.getName()] = EmuLibFunction(&libFunc, true);
                    }
                    else
                    {
                        // Non-native if any referenced unknown kind function is non-native.
                        for (auto func : funcIt->second)
                        {
                            if (!getFunction(func->getName(), true) )
                            {
                                isNative = false;
                                break;
                            }
                        }
                        archive.functions[libFunc.getName()] = EmuLibFunction(&libFunc, isNative);
                    }
                }

                if ((libFunc.getName() == funcName) && ((!nativeOnly) || isNative))
                    requestedFunc = &libFunc;
            }
        }
        // Add new module to our modules list.
        m_modules.push_back(std::move(libModule));

        return requestedFunc;
    }

    // Not found in any archive.
    return nullptr;
}

