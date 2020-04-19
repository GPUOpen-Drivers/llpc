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
 * @file  PalMetadata.h
 * @brief LLPC header file: PalMetadata class for manipulating PAL metadata
 *
 * The PalMetadata object can be retrieved using PipelineState::getPalMetadata(), and is used by various parts
 * of LGC to write information to PAL metadata at the time the information is generated. The PalMetadata object
 * is carried through the middle-end, and serialized to IR metadata at the end of the middle-end (or at the
 * point -stop-before etc stops compilation, if earlier).
 ***********************************************************************************************************************
 */
#pragma once

namespace llvm {
class Module;
namespace msgpack {
class Document;
} // namespace msgpack
} // namespace llvm

namespace lgc {

// =====================================================================================================================
// Class for manipulating PAL metadata through LGC
class PalMetadata {
public:
  // Constructors
  PalMetadata();
  PalMetadata(llvm::Module *module);
  PalMetadata(const PalMetadata &) = delete;
  PalMetadata &operator=(const PalMetadata &) = delete;

  ~PalMetadata();

  // Record the PAL metadata into IR metadata in the specified module.
  void record(llvm::Module *module);

  // Get the MsgPack document for explicit manipulation. Only ConfigBuilder* uses this.
  llvm::msgpack::Document *getDocument() { return m_document; }

private:
  llvm::msgpack::Document *m_document;
};

} // namespace lgc
