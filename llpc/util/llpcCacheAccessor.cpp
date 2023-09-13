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

namespace llvm {
namespace cl {
extern opt<unsigned> ShaderCacheMode;
} // namespace cl
} // namespace llvm

namespace Llpc {

// =====================================================================================================================
// Access the given caches using the hash.
//
// @param context : The context that will give the caches from the application.
// @param hash : The hash for the entry to access.
// @param internalCaches : The internal caches to check.
CacheAccessor::CacheAccessor(Context *context, MetroHash::Hash &cacheHash, CachePair internalCaches) {
  assert(context);
  if (context->getPipelineType() == PipelineType::Graphics) {
    const auto *pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(context->getPipelineBuildInfo());
    initializeUsingBuildInfo(pipelineInfo, cacheHash, internalCaches);
  } else {
    const auto *pipelineInfo = reinterpret_cast<const ComputePipelineBuildInfo *>(context->getPipelineBuildInfo());
    initializeUsingBuildInfo(pipelineInfo, cacheHash, internalCaches);
  }
}

// =====================================================================================================================
// Initializes the cache accessor to check the given caches.  The caches can be nullptr.
//
// @param userCache : The ICache supplied by the application. nullptr if no cache is provided.
// @param internalCaches : The internal caches to check.
void CacheAccessor::initialize(Vkgc::ICache *userCache, CachePair internalCaches) {
  m_internalCaches = internalCaches;
  m_applicationCaches = {userCache, nullptr};
  resetShaderCacheTrackingData();
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
    cacheResult = lookUpInCache(getInternalCache(), !getApplicationCache(), hashId);
    if (cacheResult == Result::Success)
      m_internalCacheHit = true;
  }
  if (getApplicationCache() && cacheResult != Result::Success)
    cacheResult = lookUpInCache(getApplicationCache(), true, hashId);
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
// Looks for the given hash in the shader caches and sets the cache accessor state with the results.
//
// @param hash : The hash to look up.
void CacheAccessor::lookUpInShaderCaches(MetroHash::Hash &hash) {
  ShaderCache *applicationCache = static_cast<ShaderCache *>(getApplicationShaderCache());
  ShaderCache *internalCache = static_cast<ShaderCache *>(getInternalShaderCache());
  bool usingApplicationCache = applicationCache && cl::ShaderCacheMode != ShaderCacheForceInternalCacheOnDisk;
  if (internalCache) {
    if (lookUpInShaderCache(hash, !usingApplicationCache, internalCache))
      return;
  }
  if (usingApplicationCache) {
    if (lookUpInShaderCache(hash, true, applicationCache))
      return;
  }
  resetShaderCacheTrackingData();
}

// =====================================================================================================================
// Set to entry tracking the shader cache to indicate that it is not tracking any shader cache entry.
void CacheAccessor::resetShaderCacheTrackingData() {
  m_shaderCache = nullptr;
  m_shaderCacheEntry = nullptr;
  m_shaderCacheEntryState = ShaderEntryState::New;
}

// =====================================================================================================================
// Looks for the given hash in the given shader cache and sets the cache accessor state with the results.  A new entry
// will be allocated if there is a cache miss and allocateOnMiss is true.
//
// @param hash : The hash to look up.
// @param allocateOnMiss : Will add an entry to the cache on a miss if true.
// @param cache : The cache in with to look.
bool CacheAccessor::lookUpInShaderCache(const MetroHash::Hash &hash, bool allocateOnMiss, ShaderCache *cache) {
  CacheEntryHandle currentEntry;
  ShaderEntryState cacheEntryState = cache->findShader(hash, allocateOnMiss, &currentEntry);
  if (cacheEntryState == ShaderEntryState::Ready) {
    Result result = cache->retrieveShader(currentEntry, &m_elf.pCode, &m_elf.codeSize);
    if (result == Result::Success) {
      m_shaderCacheEntryState = ShaderEntryState::Ready;
      return true;
    }
  } else if (cacheEntryState == ShaderEntryState::Compiling) {
    m_shaderCache = cache;
    m_shaderCacheEntry = currentEntry;
    m_shaderCacheEntryState = ShaderEntryState::Compiling;
    return true;
  }
  return false;
}

// =====================================================================================================================
// Sets the ELF entry for the hash on a cache miss.  Does nothing if there was a cache hit or the ELF has already been
// set.
//
// @param elf : The binary encoding of the elf to place in the cache.
void CacheAccessor::setElfInCache(BinaryData elf) {
  if (m_shaderCacheEntryState == ShaderEntryState::Compiling && m_shaderCacheEntry) {
    updateShaderCache(elf);
    if (m_shaderCache->retrieveShader(m_shaderCacheEntry, &m_elf.pCode, &m_elf.codeSize) == Result::Success) {
      m_shaderCacheEntryState = ShaderEntryState::Ready;
    } else {
      return;
    }
  }

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

// =====================================================================================================================
// Updates the entry in the shader cache, if there is one, for this access.
//
// @param elf : The binary encoding of the elf to place in the cache.
void CacheAccessor::updateShaderCache(BinaryData &elf) {
  ShaderCache *shaderCache = m_shaderCache;
  if (!m_shaderCacheEntry)
    return;

  if (!shaderCache)
    shaderCache = static_cast<ShaderCache *>(getInternalShaderCache());

  if (elf.pCode) {
    assert(elf.codeSize > 0);
    shaderCache->insertShader(m_shaderCacheEntry, elf.pCode, elf.codeSize);
  } else
    shaderCache->resetShader(m_shaderCacheEntry);
}

} // namespace Llpc
