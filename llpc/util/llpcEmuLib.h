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
 * @file  llpcEmuLib.h
 * @brief LLPC header file: contains declaration of class Llpc::EmuLib.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/Object/Archive.h"
#include "llvm/Support/MemoryBuffer.h"
#include <map>
#include <unordered_map>
#include <vector>

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace std {

template <> struct hash<llvm::StringRef> {
  typedef llvm::StringRef argument_type;
  typedef std::size_t result_type;
  result_type operator()(argument_type const &str) const noexcept { return llvm::hash_value(str); }
};

} // namespace std

namespace Llpc {
class Context;

// =====================================================================================================================
// Represents an emulation archive library, together with already-loaded modules from it.
class EmuLib {
  // An already-loaded function from the emulation library.
  struct EmuLibFunction {
    llvm::Function *function; // Function in Module parsed from library module
    bool isNative;            // Whether the function is native according to criteria in llpcEmuLib.cpp

    EmuLibFunction() : function(nullptr), isNative(true) {}
    EmuLibFunction(llvm::Function *function, bool isNative) : function(function), isNative(isNative) {}
  };

  // An archive in the emulation library. The map of already-loaded functions from the archive needs
  // to be per-archive, because multiple archives can have the same named function and we need to
  // avoid accidentally getting the wrong one if the module containing that function from a later
  // archive in search order has already been loaded.
  struct EmuLibArchive {
    std::unique_ptr<llvm::object::Archive> archive; // The bitcode archive
    std::unordered_map<llvm::StringRef, EmuLibFunction>
        functions; // Store of already-parsed functions from this archive

    EmuLibArchive(std::unique_ptr<llvm::object::Archive> archive) : archive(std::move(archive)) {}
  };

  Context *m_context;                                          // The LLPC context
  std::vector<EmuLibArchive> m_archives;                       // Bitcode archives that make up this EmuLib
  std::vector<std::unique_ptr<llvm::Module>> m_modules;        // Modules that have been parsed out of archives
  std::unordered_map<llvm::StringRef, size_t> m_symbolIndices; // All available symbols in this EmuLib and the indices
                                                               // in m_archives
public:
  EmuLib(Context *context) : m_context(context) {}
  void addArchive(llvm::MemoryBufferRef buffer);
  llvm::Function *getFunction(llvm::StringRef funcName, bool nativeOnly);
};

} // namespace Llpc
