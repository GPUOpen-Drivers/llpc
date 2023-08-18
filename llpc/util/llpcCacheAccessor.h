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
#include "llpcShaderCache.h"
#include "vkgcMetroHash.h"
#include "llvm/Support/CommandLine.h"

namespace Llpc {

class Context;

struct CachePair {
  Vkgc::ICache *cache = nullptr;
  IShaderCache *shaderCache = nullptr;
};

class CacheAccessor {
public:
  // Checks the caches in the build info and the internal caches for an entry with the given hash.
  //
  // @param buildInfo : The build information that will give the caches from the application.
  // @param hash : The hash for the entry to access.
  // @param internalCaches : The internal caches to check.
  template <class BuildInfo> CacheAccessor(BuildInfo *buildInfo, MetroHash::Hash &cacheHash, CachePair internalCaches) {
    initializeUsingBuildInfo(buildInfo, cacheHash, internalCaches);
  }

  CacheAccessor(CacheAccessor &&ca) { *this = std::move(ca); }

  CacheAccessor &operator=(CacheAccessor &&ca) {
    m_applicationCaches = ca.m_applicationCaches;
    m_internalCaches = ca.m_internalCaches;
    m_shaderCacheEntryState = ca.m_shaderCacheEntryState;
    m_shaderCacheEntry = ca.m_shaderCacheEntry;
    m_shaderCache = ca.m_shaderCache;
    m_cacheResult = ca.m_cacheResult;
    m_cacheEntry = std::move(ca.m_cacheEntry);
    m_elf = ca.m_elf;

    // Reinitialize ca with not caches.  It needs to be in an appropriate state for the destructor.
    ca.initialize(nullptr, {nullptr, nullptr});
    return *this;
  }

  CacheAccessor(Context *context, MetroHash::Hash &cacheHash, CachePair internalCaches);

  // Finalizes the cache access by releasing any handles that need to be released.
  ~CacheAccessor() { setElfInCache({0, nullptr}); }

  // Returns true of the entry was in at least on of the caches or has been added to the cache.
  bool isInCache() const {
    return m_cacheResult == Result::Success || m_shaderCacheEntryState == ShaderEntryState::Ready;
  }

  // Returns the ELF that was found in the cache.
  BinaryData getElfFromCache() const { return m_elf; }

  void setElfInCache(BinaryData elf);

  // Returns true if there was a cache hit in an internal cache.
  bool hitInternalCache() const {
    if (!isInCache())
      return false;
    if (m_cacheResult == Result::Success) {
      return m_internalCacheHit;
    }
    return getApplicationShaderCache() == m_shaderCache;
  }

private:
  CacheAccessor() = delete;
  CacheAccessor(const CacheAccessor &) = delete;
  CacheAccessor &operator=(const CacheAccessor &) = delete;

  const Vkgc::ICache *getApplicationCache() const { return m_applicationCaches.cache; }
  const IShaderCache *getApplicationShaderCache() const { return m_applicationCaches.shaderCache; }
  const Vkgc::ICache *getInternalCache() const { return m_internalCaches.cache; }
  const IShaderCache *getInternalShaderCache() const { return m_internalCaches.shaderCache; }
  Vkgc::ICache *getApplicationCache() { return m_applicationCaches.cache; }
  IShaderCache *getApplicationShaderCache() { return m_applicationCaches.shaderCache; }
  Vkgc::ICache *getInternalCache() { return m_internalCaches.cache; }
  IShaderCache *getInternalShaderCache() { return m_internalCaches.shaderCache; }

  // Access the given caches using the hash.
  //
  // @param buildInfo : The build info object that the caches from the application.
  // @param hash : The hash for the entry to access.
  // @param internalCaches : The internal caches to check.
  template <class BuildInfo>
  void initializeUsingBuildInfo(const BuildInfo *buildInfo, MetroHash::Hash &hash, CachePair internalCaches) {
    assert(buildInfo);
    Vkgc::ICache *userCache = buildInfo->cache;

    initialize(userCache, internalCaches);
    lookUpInCaches(hash);
    if (m_cacheResult != Result::Success)
      lookUpInShaderCaches(hash);
  }

  void initialize(Vkgc::ICache *userCache, CachePair internalCaches);

  void lookUpInCaches(const MetroHash::Hash &hash);
  Result lookUpInCache(Vkgc::ICache *cache, bool allocateOnMiss, const Vkgc::HashId &hashId);

  void lookUpInShaderCaches(MetroHash::Hash &hash);
  bool lookUpInShaderCache(const MetroHash::Hash &hash, bool allocateOnMiss, ShaderCache *cache);
  void updateShaderCache(BinaryData &elf);
  void resetShaderCacheTrackingData();

  CachePair m_applicationCaches;
  CachePair m_internalCaches;

  // The state of the shader cache look up.
  ShaderEntryState m_shaderCacheEntryState = ShaderEntryState::New;

  // The handle to the entry in the shader cache.
  CacheEntryHandle m_shaderCacheEntry = nullptr;

  // The shader cache that the entry refers to.
  ShaderCache *m_shaderCache = nullptr;

  // The result of checking the ICache.
  Result m_cacheResult = Result::ErrorUnknown;

  // Whether the cache hit came from the internal cache.
  bool m_internalCacheHit = false;

  // The handle to the entry in the cache.
  Vkgc::EntryHandle m_cacheEntry;

  // The ELF corresponding to the entry.
  BinaryData m_elf = {0, nullptr};
};

} // namespace Llpc
