/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021 Google LLC All Rights Reserved.
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
 * @file  llpcCacheAccessor.cpp
 * @brief LLPC source file: Implementation of a class that will create an interface to easily check the caches that need
 * to be checked (independent of LLVM use).
 ***********************************************************************************************************************
 */
#include "llpcCacheAccessor.h"
#include "llpcContext.h"
#include "llpcError.h"

#define DEBUG_TYPE "llpc-cache-accessor"

using namespace llvm;

namespace Llpc {
// =====================================================================================================================
// Initializes the cache accessor to check the given caches.  The caches can be nullptr.
//
// @param userCache : The ICache supplied by the application. nullptr if no cache is provided.
// @param internalCaches : The internal caches to check.
void CacheAccessor::initialize(Vkgc::ICache *internalCache) {
  m_internalCache = internalCache;
  m_cacheResult = Result::ErrorUnknown;
  m_cacheEntry = Vkgc::EntryHandle();
  m_elf = {0, nullptr};
}

// =====================================================================================================================
// Looks for the given hash in the ICaches and sets the cache accessor state with the results.
//
// @param hash : The hash to look up.
void CacheAccessor::lookUpInCaches(const MetroHash::Hash &hash) {
  Vkgc::HashId hashId = {};
  memcpy(&hashId.bytes, &hash.bytes, sizeof(hash));

  Result cacheResult = Result::Unsupported;
  if (getInternalCache()) {
    cacheResult = lookUpInCache(getInternalCache(), true, hashId);
  }
  m_cacheResult = cacheResult;
}

// =====================================================================================================================
// Looks for the given hash in the given cache and sets the cache accessor state with the results.  A new entry
// will be allocated if there is a cache miss and allocateOnMiss is true.
//
// @param hash : The hash to look up.
// @param allocateOnMiss : Will add an entry to the cache on a miss if true.
// @param cache : The cache in which to look.
Result CacheAccessor::lookUpInCache(Vkgc::ICache *cache, bool allocateOnMiss, const Vkgc::HashId &hashId) {
  Vkgc::EntryHandle currentEntry;
  Result cacheResult = cache->GetEntry(hashId, allocateOnMiss, &currentEntry);

  if (cacheResult == Result::NotReady)
    cacheResult = currentEntry.WaitForEntry();

  if (cacheResult == Result::Success) {
    cacheResult = currentEntry.GetValueZeroCopy(&m_elf.pCode, &m_elf.codeSize);
    m_cacheEntry = std::move(currentEntry);
  } else if (allocateOnMiss && (cacheResult == Result::NotFound)) {
    m_cacheEntry = std::move(currentEntry);
  }
  return cacheResult;
}

// =====================================================================================================================
// Sets the ELF entry for the hash on a cache miss.  Does nothing if there was a cache hit or the ELF has already been
// set.
//
// @param elf : The binary encoding of the elf to place in the cache.
void CacheAccessor::setElfInCache(BinaryData elf) {
  if (!m_cacheEntry.IsEmpty()) {
    m_cacheResult = Result::ErrorUnknown;
    if (elf.pCode) {
      mustSucceed(m_cacheEntry.SetValue(true, elf.pCode, elf.codeSize));
      mustSucceed(m_cacheEntry.GetValueZeroCopy(&m_elf.pCode, &m_elf.codeSize));
    }
    Vkgc::EntryHandle::ReleaseHandle(std::move(m_cacheEntry));
    m_cacheResult = elf.pCode ? Result::Success : Result::ErrorUnknown;
  }
}

} // namespace Llpc
