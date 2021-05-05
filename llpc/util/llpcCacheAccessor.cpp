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
#include "llpcCompiler.h"
#include "llpcContext.h"

#define DEBUG_TYPE "llpc-cache-accessor"

namespace Llpc {

// =====================================================================================================================
// Access the given caches using the hash.
//
// @param context : The context that will give the caches from the application.
// @param hash : The hash for the entry to access.
// @param compiler : The compiler object with the internal caches.
CacheAccessor::CacheAccessor(Context *context, MetroHash::Hash &cacheHash, Compiler *compiler) {
  assert(context);
  if (context->isGraphics()) {
    auto pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(context->getPipelineBuildInfo());
    initializeUsingBuildInfo(pipelineInfo, cacheHash, compiler);
  } else {
    auto pipelineInfo = reinterpret_cast<const ComputePipelineBuildInfo *>(context->getPipelineBuildInfo());
    initializeUsingBuildInfo(pipelineInfo, cacheHash, compiler);
  }
}

// =====================================================================================================================
// Initializes all of the member variable using the data provided.
//
// @param hash : The hash for the entry to access.
// @param userCache : The ICache supplied by the application. nullptr if no cache is provided.
// @param userShaderCache : The shader cache supplied by the application. nullptr if no cache is provided.
// @param compiler : The compiler object with the internal caches.
void CacheAccessor::initialize(MetroHash::Hash &hash, Vkgc::ICache *userCache, IShaderCache *userShaderCache,
                               Compiler *compiler) {
  assert(compiler);
  m_compiler = compiler;
  m_userCache = userCache;
  m_userShaderCache = userShaderCache;
  m_shaderCache = nullptr;
  m_shaderCacheEntryState = ShaderEntryState::New;
  m_elf = {0, nullptr};

  Vkgc::HashId hashId = {};
  memcpy(&hashId.bytes, &hash.bytes, sizeof(hash));
  m_cacheResult = m_compiler->lookUpCaches(m_userCache, &hashId, &m_elf, &m_cacheEntry);
  if (m_cacheResult == Result::Success)
    return;
  m_shaderCacheEntryState =
      m_compiler->lookUpShaderCaches(m_userShaderCache, &hash, &m_elf, &m_shaderCache, &m_shaderCacheEntry);
}

// =====================================================================================================================
// Sets the ELF entry for the hash on a cache miss.  Does nothing if there was a cache hit or the ELF has already been
// set.
//
// @param elf : The binary encoding of the elf to place in the cache.
void CacheAccessor::setElfInCache(BinaryData elf) {
  if (m_shaderCacheEntryState == ShaderEntryState::Compiling && m_shaderCacheEntry) {
    m_compiler->updateShaderCache(elf.pCode, &elf, m_shaderCache, m_shaderCacheEntry);
    m_shaderCache->retrieveShader(m_shaderCacheEntry, &m_elf.pCode, &m_elf.codeSize);
    m_shaderCacheEntryState = ShaderEntryState::Ready;
  }

  if (!m_cacheEntry.IsEmpty()) {
    if (elf.pCode) {
      m_cacheEntry.SetValue(true, elf.pCode, elf.codeSize);
      m_cacheEntry.GetValueZeroCopy(&m_elf.pCode, &m_elf.codeSize);
    }
    Vkgc::EntryHandle::ReleaseHandle(std::move(m_cacheEntry));
    m_cacheResult = elf.pCode ? Result::Success : Result::ErrorUnknown;
  }
}

} // namespace Llpc
