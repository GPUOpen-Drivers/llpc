/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 @file llpcShaderCacheWrap.h
 @brief LLPC header file: contains declaration of class Llpc::ShaderCacheWrap.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llpcShaderCache.h"
#include <list>

namespace Llpc {

// =====================================================================================================================
// Helper class to wrap shader cache with ICache interface
class ShaderCacheWrap : public Vkgc::ICache {
public:
  // Constructor
  ShaderCacheWrap(ShaderCache *pShaderCache) : m_pShaderCache(pShaderCache) {}

  virtual ~ShaderCacheWrap() { assert(m_pShaderCache == nullptr); };

  static ShaderCacheWrap *Create(unsigned optionCount, const char *const *options);

  void Destroy();

  LLPC_NODISCARD Result GetEntry(Vkgc::HashId hash, bool allocateOnMiss, Vkgc::EntryHandle *pHandle);

  LLPC_NODISCARD void ReleaseEntry(Vkgc::RawEntryHandle rawHandle);

  LLPC_NODISCARD Result WaitForEntry(Vkgc::RawEntryHandle rawHandle);

  LLPC_NODISCARD Result GetValue(Vkgc::RawEntryHandle rawHandle, void *pData, size_t *pDataLen);

  LLPC_NODISCARD Result GetValueZeroCopy(Vkgc::RawEntryHandle rawHandle, const void **ppData, size_t *pDataLen);

  LLPC_NODISCARD Result SetValue(Vkgc::RawEntryHandle rawHandle, bool success, const void *pData, size_t dataLen);

private:
  ShaderCache *m_pShaderCache; // ShaderCache object
};

} // namespace Llpc
