/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018 Advanced Micro Devices, Inc. All Rights Reserved.
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

#define DEBUG_TYPE "llpc-emu-lib"

using namespace Llpc;
using namespace llvm;
using namespace object;

namespace llvm
{

namespace cl
{

// -disable-llvm-patch: disable the patch for LLVM back-end issues.
static opt<bool> DisableLlvmPatch("disable-llvm-patch",
                                  desc("Disable the patch for LLVM back-end issues"),
                                  init(false));

} // cl

} // llvm

// =====================================================================================================================
// Adds an archive to the emulation library.
void EmuLib::AddArchive(
    MemoryBufferRef buffer) // Buffer required to create the archive
{
    m_archives.push_back(EmuLibArchive(std::move(cantFail(Archive::create(buffer), "Failed to parse archive"))));
}

// =====================================================================================================================
// Gets a function from the emulation library.
//
// Returns nullptr if not found, or if it is not a native function when nativeOnly is true.
Function* EmuLib::GetFunction(
    StringRef funcName, // Function name to find
    bool nativeOnly)    // Whether to only find a native function
{
    // Search each archive in turn.
    for (auto& archive : m_archives)
    {
        // See if the function is already loaded from this archive.
        auto funcMapIt = archive.functions.find(funcName);
        if (funcMapIt != archive.functions.end())
        {
            // Function is already in the function map.
            if (nativeOnly && (funcMapIt->second.isNative == false))
            {
                return nullptr;
            }
            return funcMapIt->second.pFunction;
        }

        // Find the function in the symbol table of the archive.
        auto pChild = cantFail(archive.archive->findSym(funcName), "Failed in archive symbol search");
        if (!pChild)
        {
            // Not found. Go on to next archive.
            continue;
        }

        // Found the symbol. Get the bitcode for its module.
        StringRef child = cantFail(pChild->getBuffer(), "Failed in archive module extraction");
        // Parse the bitcode archive member into a Module.
        auto libModule = cantFail(parseBitcodeFile(
              MemoryBufferRef(child, ""), *pContext), "Failed to parse archive bitcode");

        // Find and mark the non-native library functions. A library function is non-native if:
        //   it references llpc.*
        //   it references llvm.amdgcn.*
        //   it references llvm.fabs.* (subject to cl::DisableLlvmPatch == false)
        //   it is _Z14unpackHalf2x16i* (subject to cl::DisableLlvmPatch == false)
        std::unordered_set<Function*> nonNativeFuncs;
        for (auto& libFunc : *libModule)
        {
            if (libFunc.isDeclaration())
            {
                auto libFuncName = libFunc.getName();

                // TODO: "llvm.fabs." is to pass
                // CTS dEQP-VK.ssbo.layout.single_basic_type.std430/std140.row_major_lowp_mat4.
                // We should remove it once the bug in LLVM back-end is fixed.
                if (libFuncName.startswith("llpc.") ||
                    libFuncName.startswith("llvm.amdgcn.") ||
                    ((cl::DisableLlvmPatch == false) && libFuncName.startswith("llvm.fabs.")))
                {
                    for (auto pUser : libFunc.users())
                    {
                        auto pInst = dyn_cast<Instruction>(pUser);
                        auto pNonNativeFunc = pInst->getParent()->getParent();
                        nonNativeFuncs.insert(pNonNativeFunc);
                    }
                }
            }

            if (cl::DisableLlvmPatch == false)
            {
                if (libFunc.getName().startswith("_Z14unpackHalf2x16i"))
                {
                    nonNativeFuncs.insert(&libFunc);
                }
            }
        }

        // Add the new module's defined functions to the function map for this archive.
        Function* pRequestedFunc = nullptr;
        for (auto& libFunc : *libModule)
        {
            if (libFunc.empty() == false)
            {
                bool isNative = nonNativeFuncs.find(&libFunc) == nonNativeFuncs.end();
                archive.functions[libFunc.getName()] = EmuLibFunction(&libFunc, isNative);
                if ((libFunc.getName() == funcName) && ((nativeOnly == false) || isNative))
                {
                    pRequestedFunc = &libFunc;
                }
            }
        }
        // Add new module to our modules list.
        m_modules.push_back(std::move(libModule));

        return pRequestedFunc;
    }

    // Not found in any archive.
    return nullptr;
}

