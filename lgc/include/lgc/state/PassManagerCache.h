/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PassManagerCache.h
 * @brief LGC header file: Pass manager creator and cache
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/PassManager.h"
#include "llvm/Support/raw_ostream.h"

namespace lgc {

class LgcContext;
struct PassManagerInfo;

// =====================================================================================================================
// A raw_pwrite_stream that proxies for another raw_pwrite_stream.
class raw_proxy_ostream : public llvm::raw_pwrite_stream {
public:
  // Construct a new raw_proxy_ostream.
  //
  // @param underlyingStream : The underlying raw_pwrite_stream to use.
  raw_proxy_ostream(llvm::raw_pwrite_stream *underlyingStream = nullptr) : m_underlyingStream(underlyingStream) {
    SetUnbuffered();
  }

  // Switch to a different underlying stream.
  //
  // @param underlyingStream : The underlying raw_pwrite_stream to use.
  void setUnderlyingStream(llvm::raw_pwrite_stream *underlyingStream) {
    if (m_underlyingStream)
      m_underlyingStream->flush();
    m_underlyingStream = underlyingStream;
  }

  ~raw_proxy_ostream() override = default;

  void flush() = delete;

private:
  void write_impl(const char *ptr, size_t size) override { m_underlyingStream->write(ptr, size); }

  void pwrite_impl(const char *ptr, size_t size, uint64_t offset) override {
    m_underlyingStream->pwrite(ptr, size, offset);
  }

  uint64_t current_pos() const override { return m_underlyingStream->tell(); }

  llvm::raw_pwrite_stream *m_underlyingStream;
};

// =====================================================================================================================
// Pass manager creator and cache
class PassManagerCache {
public:
  PassManagerCache(LgcContext *lgcContext) : m_lgcContext(lgcContext) {}

  // Get pass manager for glue shader compilation
  // NOTE: This function returns two pass managers, a new pass manager for the
  // IR passes and a legacy pass manager for the codegen passes. We should
  // switch to using a single new pass manager once LLVM upstream codegen is
  // ported to the new pass manager.
  std::pair<lgc::PassManager &, LegacyPassManager &> getGlueShaderPassManager(llvm::raw_pwrite_stream &outStream);

  void resetStream();

private:
  std::pair<lgc::PassManager &, LegacyPassManager &> getPassManager(const PassManagerInfo &info,
                                                                    llvm::raw_pwrite_stream &outStream);

  LgcContext *m_lgcContext;
  llvm::StringMap<std::pair<std::unique_ptr<PassManager>, std::unique_ptr<LegacyPassManager>>> m_cache;
  raw_proxy_ostream m_proxyStream;
};

} // namespace lgc
