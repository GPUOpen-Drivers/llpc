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

namespace Llpc {

class Compiler;
class Context;

class CacheAccessor {
public:
  // Checks the caches in the build info and compiler objects for an entry with the given hash.
  //
  // @param buildInfo : The build information that will give the caches from the application.
  // @param hash : The hash for the entry to access.
  // @param compiler : The compiler object with the internal caches.
  template <class BuildInfo> CacheAccessor(BuildInfo *buildInfo, MetroHash::Hash &cacheHash, Compiler *compiler) {
    initializeUsingBuildInfo(buildInfo, cacheHash, compiler);
  }

  CacheAccessor(CacheAccessor &&ca) { *this = std::move(ca); }

  CacheAccessor &operator=(CacheAccessor &&ca) {
    this->m_userCache = ca.m_userCache;
    ca.m_userCache = nullptr;
    this->m_userShaderCache = ca.m_userShaderCache;
    ca.m_userShaderCache = nullptr;

    this->m_compiler = ca.m_compiler;
    ca.m_compiler = nullptr;

    this->m_shaderCacheEntryState = ca.m_shaderCacheEntryState;
    ca.m_shaderCacheEntryState = ShaderEntryState::Unavailable;

    this->m_shaderCacheEntry = ca.m_shaderCacheEntry;
    ca.m_shaderCacheEntry = nullptr;

    this->m_shaderCache = ca.m_shaderCache;
    ca.m_shaderCache = nullptr;

    this->m_cacheResult = ca.m_cacheResult;
    ca.m_cacheResult = Result::ErrorUnknown;

    this->m_cacheEntry = std::move(ca.m_cacheEntry);

    this->m_elf = ca.m_elf;
    ca.m_elf = {0, nullptr};
    return *this;
  }

  CacheAccessor(Context *context, MetroHash::Hash &cacheHash, Compiler *compiler);

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
      return true;
    }
    return getUserShaderCache() == m_shaderCache;
  }

private:
  CacheAccessor() = delete;
  CacheAccessor(const CacheAccessor &) = delete;
  CacheAccessor &operator=(const CacheAccessor &) = delete;

  Vkgc::ICache *getUserCache() const { return m_userCache; }
  IShaderCache *getUserShaderCache() const { return m_userShaderCache; }

  // Access the given caches using the hash.
  //
  // @param buildInfo : The build info object that the caches from the application.
  // @param hash : The hash for the entry to access.
  // @param compiler : The compiler object with the internal caches.
  template <class BuildInfo>
  void initializeUsingBuildInfo(const BuildInfo *buildInfo, MetroHash::Hash &hash, Compiler *compiler) {
    assert(buildInfo);
    Vkgc::ICache *userCache = buildInfo->cache;

    IShaderCache *userShaderCache = nullptr;
#if LLPC_ENABLE_SHADER_CACHE
    userShaderCache = reinterpret_cast<IShaderCache *>(buildInfo->pShaderCache);
#endif

    initialize(hash, userCache, userShaderCache, compiler);
  }

  void initialize(MetroHash::Hash &hash, Vkgc::ICache *userCache, IShaderCache *userShaderCache, Compiler *compiler);

  // The application caches
  Vkgc::ICache *m_userCache = nullptr;
  IShaderCache *m_userShaderCache = nullptr;

  // The compiler object holds the internal caches.  Used for the functions to access those caches as well.
  // TODO: We should write a new class that holds the internal and external cache.  Move the function to check the
  //  caches from the compiler class, so we will not need the compiler object here.
  Compiler *m_compiler = nullptr;

  // The state of the shader cache look up.
  ShaderEntryState m_shaderCacheEntryState = ShaderEntryState::Unavailable;

  // The handle to the entry in the shader cache.
  CacheEntryHandle m_shaderCacheEntry = nullptr;

  // The shader cache that the entry refers to.
  ShaderCache *m_shaderCache = nullptr;

  // The result of checking the ICache.
  Result m_cacheResult = Result::ErrorUnknown;

  // The handle to the entry in the cache.
  Vkgc::EntryHandle m_cacheEntry;

  // The ELF corresponding to the entry.
  BinaryData m_elf = {0, nullptr};
};

} // namespace Llpc
