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
 * @file  llpcCacheAccessor.h
 * @brief LLPC header file: Implementation of a class that will create an interface to easily check the caches that need
 * to be checked (independent of LLVM use).
 ***********************************************************************************************************************
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "vkgcMetroHash.h"
#include "llvm/Support/CommandLine.h"

namespace Llpc {

class Context;

class CacheAccessor {
public:
  // Checks the caches in the build info and the internal caches for an entry with the given hash.
  //
  // @param buildInfo : The build information that will give the caches from the application.
  // @param hash : The hash for the entry to access.
  // @param internalCaches : The internal caches to check.
  CacheAccessor(MetroHash::Hash &cacheHash, Vkgc::ICache *internalCache) {
    initializeUsingBuildInfo(cacheHash, internalCache);
  }

  CacheAccessor(CacheAccessor &&ca) { *this = std::move(ca); }

  CacheAccessor &operator=(CacheAccessor &&ca) {
    m_internalCache = ca.m_internalCache;
    m_cacheResult = ca.m_cacheResult;
    m_cacheEntry = std::move(ca.m_cacheEntry);
    m_elf = ca.m_elf;

    // Reinitialize ca with not caches.  It needs to be in an appropriate state for the destructor.
    ca.initialize(nullptr);
    return *this;
  }

  // Finalizes the cache access by releasing any handles that need to be released.
  ~CacheAccessor() { setElfInCache({0, nullptr}); }

  // Returns true of the entry was in at least on of the caches or has been added to the cache.
  bool isInCache() const { return m_cacheResult == Result::Success; }

  // Returns the ELF that was found in the cache.
  BinaryData getElfFromCache() const { return m_elf; }

  void setElfInCache(BinaryData elf);

private:
  CacheAccessor() = delete;
  CacheAccessor(const CacheAccessor &) = delete;
  CacheAccessor &operator=(const CacheAccessor &) = delete;

  const Vkgc::ICache *getInternalCache() const { return m_internalCache; }
  Vkgc::ICache *getInternalCache() { return m_internalCache; }

  // Access the given caches using the hash.
  //
  // @param buildInfo : The build info object that the caches from the application.
  // @param hash : The hash for the entry to access.
  // @param internalCaches : The internal caches to check.
  void initializeUsingBuildInfo(MetroHash::Hash &hash, Vkgc::ICache *internalCache) {
    initialize(internalCache);
    lookUpInCaches(hash);
  }

  void initialize(Vkgc::ICache *internalCache);

  void lookUpInCaches(const MetroHash::Hash &hash);
  Result lookUpInCache(Vkgc::ICache *cache, bool allocateOnMiss, const Vkgc::HashId &hashId);

  Vkgc::ICache *m_internalCache;

  // The result of checking the ICache.
  Result m_cacheResult = Result::ErrorUnknown;

  // The handle to the entry in the cache.
  Vkgc::EntryHandle m_cacheEntry;

  // The ELF corresponding to the entry.
  BinaryData m_elf = {0, nullptr};
};

} // namespace Llpc
