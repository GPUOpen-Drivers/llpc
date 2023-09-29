/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ElfLinker.h
 * @brief LLPC header file: LGC interface for linking unlinked shader and part-pipeline ELFs into pipeline ELF
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/CommonDefs.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"

namespace llvm {
class raw_pwrite_stream;
} // namespace llvm

namespace lgc {
struct ColorExportInfo;

// =====================================================================================================================
// The public API of the LGC interface for ELF linking.
// The ElfLinker object is created by calling Pipeline::getElfLinker(). The ElfLinker internally refers back to
// its Pipeline, and thus uses pipeline state for glue code generation, adding to PAL metadata, and resolving
// relocs.
class ElfLinker {
public:
  virtual ~ElfLinker() {}

  // Add another input ELF to the link, in addition to the ones that were added when the ElfLinker was constructed.
  virtual void addInputElf(llvm::MemoryBufferRef inputElf) = 0;

  // Check whether we have FS input mappings, and thus whether we're doing part-pipeline compilation of the
  // pre-FS part of the pipeline.
  virtual bool haveFsInputMappings() = 0;

  // Get a representation of the fragment shader input mappings from the PAL metadata of ELF input(s) added so far.
  // This is used by the caller in a part-pipeline compilation scheme to include the FS input mappings in the
  // hash for the non-FS part of the pipeline.
  virtual llvm::StringRef getFsInputMappings() = 0;

  // Get information on the glue code that will be needed for the link. It is an implementation detail how many
  // chunks of glue there might be and what they are for, but, for information, they will be some subset of:
  // - A CS prolog
  // - A VS prolog ("fetch shader")
  // - A vertex-processing epilog ("parameter export shader")
  // - An FS epilog ("color export shader")
  //
  // Returns an array (possibly 0 length) with an entry for each chunk of glue code, where an entry
  // is a StringRef that the client can hash for its cache lookup. If it gets a cache hit, it should provide the
  // found blob to ElfLinker::addGlue. If it does not get a cache hit, the client can call ElfLinker::compileGlue to
  // retrieve the compiled glue code to store in the cache.
  virtual llvm::ArrayRef<llvm::StringRef> getGlueInfo() = 0;

  // Build color export shader
  //
  // @param exports : Fragment export info
  // @param enableKill : Whether this fragment shader has kill enabled.
  virtual llvm::StringRef buildColorExportShader(llvm::ArrayRef<ColorExportInfo> exports, bool enableKill) = 0;

  // Add a blob for a particular chunk of glue code, typically retrieved from a cache. The blob is not copied,
  // and remains in use until the first of the link completing or the ElfLinker's parent Pipeline being destroyed.
  //
  // @param glueIndex : Index into the array that was returned by getGlueInfo()
  // @param blob : Blob for the glue code
  virtual void addGlue(unsigned glueIndex, llvm::StringRef blob) = 0;

  // Compile a particular chunk of glue code and retrieve its blob. The returned blob remains valid until the first
  // of calling link() or the ElfLinker's parent Pipeline being destroyed. It is optional to call this; any chunk
  // of glue code that has not had one of addGlue() or compileGlue() done by the time link() is called will be
  // internally compiled. The client only needs to call this if it wants to cache the glue code's blob.
  //
  // @param glueIndex : Index into the array that was returned by getGlueInfo()
  // @returns : The blob. A zero-length blob indicates that a recoverable error occurred, and link() will also return
  //           and empty ELF blob.
  virtual llvm::StringRef compileGlue(unsigned glueIndex) = 0;

  // Link the unlinked shader or part-pipeline ELFs and the compiled glue code into a pipeline ELF.
  //
  // Like other LGC and LLVM library functions, an internal compiler error could cause an assert or report_fatal_error.
  //
  // @param [out] outStream : Stream to write linked ELF to
  // @returns : True for success.
  //           False if there is some reason why the pipeline cannot be linked from unlinked shader/part-pipeline
  //           ELFs. The client typically then does a whole-pipeline compilation instead. The client can call
  //           getLastError() to get a textual representation of the error, for use in logging or in error
  //           reporting in a command-line utility.
  virtual bool link(llvm::raw_pwrite_stream &outStream) = 0;
};

} // namespace lgc
