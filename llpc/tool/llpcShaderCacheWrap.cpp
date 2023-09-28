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
@file llpcShaderCacheWrap.cpp
@brief LLPC source file: contains implementation of class Llpc::ShaderCacheWrap.
***********************************************************************************************************************
*/
#include "llpcShaderCacheWrap.h"
#include "llpcError.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "llpc-shader-cache-wrap"

using namespace llvm;

// clang-format off
namespace llvm {
namespace cl {
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 66
extern opt<std::string> ShaderCacheFileDir;
extern opt<unsigned> ShaderCacheMode;
extern opt<std::string> ExecutableName;
#else
// -shader-cache-mode: shader cache mode:
// 0 - Disable
// 1 - Runtime cache
// 2 - Cache to disk
// 3 - Use internal on-disk cache in read/write mode.
// 4 - Use internal on-disk cache in read-only mode.
opt<unsigned> ShaderCacheMode("shader-cache-mode",
                              desc("Shader cache mode, 0 - disable, 1 - runtime cache, 2 - cache to disk, 3 - "
                              "load on-disk cache for read/write, 4 - load on-disk cache for read only"),
                              init(0));

// -shader-cache-file-dir: root directory to store shader cache
opt<std::string> ShaderCacheFileDir("shader-cache-file-dir", desc("Root directory to store shader cache"),
                                    value_desc("dir"), init("."));

// -executable-name: executable file name
opt<std::string> ExecutableName("executable-name", desc("Executable file name"), value_desc("filename"),
                                init("amdllpc"));
#endif
} // namespace cl
} // namespace llvm
// clang-format on

namespace Llpc {

// =====================================================================================================================
ShaderCacheWrap *ShaderCacheWrap::Create(unsigned optionCount, const char *const *options) {
  bool createDummyCompiler = false;
  // Build effecting options
  for (unsigned i = 1; i < optionCount; ++i) {
    if (options[i][0] != '-') {
      // Ignore input file names.
      continue;
    }

    StringRef option = options[i] + 1; // Skip '-' in options

    if (option.startswith(cl::ShaderCacheMode.ArgStr) || option.startswith(cl::ShaderCacheFileDir.ArgStr) ||
        option.startswith(cl::ExecutableName.ArgStr)) {
      createDummyCompiler = true;
      break;
    }
  }

  if (createDummyCompiler) {
    GfxIpVersion gfxip = {10, 3, 0};
    ICompiler *pCompiler = nullptr;
    ICompiler::Create(gfxip, optionCount, options, &pCompiler);
    pCompiler->Destroy();
  }
  // Initialize shader cache
  ShaderCacheCreateInfo createInfo = {};
  ShaderCacheAuxCreateInfo auxCreateInfo = {};
  unsigned shaderCacheMode = cl::ShaderCacheMode;
  auxCreateInfo.shaderCacheMode = static_cast<ShaderCacheMode>(shaderCacheMode);

  if (auxCreateInfo.shaderCacheMode == ShaderCacheDisable) {
    return nullptr;
  }

  auxCreateInfo.executableName = cl::ExecutableName.c_str();

  const char *shaderCachePath = cl::ShaderCacheFileDir.c_str();
  if (cl::ShaderCacheFileDir.empty()) {
#ifdef WIN_OS
    shaderCachePath = getenv("LOCALAPPDATA");
    assert(shaderCachePath);
#else
    llvm_unreachable("Should never be called!");
#endif
  }

  auxCreateInfo.cacheFilePath = shaderCachePath;
  ShaderCacheWrap *pCache = nullptr;
  ShaderCache *pShaderCache = new ShaderCache();
  if (pShaderCache != nullptr) {
    Result result = pShaderCache->init(&createInfo, &auxCreateInfo);
    if (result == Result::Success) {
      pCache = new ShaderCacheWrap(pShaderCache);
    } else {
      pShaderCache->Destroy();
      delete pShaderCache;
    }
  }
  return pCache;
}

// =====================================================================================================================
void ShaderCacheWrap::Destroy() {
  if (m_pShaderCache != nullptr) {
    m_pShaderCache->Destroy();
    delete m_pShaderCache;
    m_pShaderCache = nullptr;
  }

  delete this;
}

// =====================================================================================================================
Result ShaderCacheWrap::GetEntry(Vkgc::HashId hash, bool allocateOnMiss, Vkgc::EntryHandle *pHandle) {
  MetroHash::Hash metroHash = {};
  metroHash.qwords[0] = hash.qwords[0];
  metroHash.qwords[1] = hash.qwords[1];
  CacheEntryHandle hEntry = {};
  Result result = Result::Success;
  ShaderEntryState entryState = m_pShaderCache->findShader(metroHash, allocateOnMiss, &hEntry);
  *pHandle = Vkgc::EntryHandle(this, hEntry, entryState == ShaderEntryState::Compiling);

  if (entryState == ShaderEntryState::Compiling) {
    result = Result::NotFound;
  } else if (entryState == ShaderEntryState::Unavailable) {
    result = Result::ErrorUnavailable;
  }
  return result;
}

// =====================================================================================================================
void ShaderCacheWrap::ReleaseEntry(Vkgc::RawEntryHandle rawHandle) {
  return;
}

// =====================================================================================================================
Result ShaderCacheWrap::WaitForEntry(Vkgc::RawEntryHandle rawHandle) {
  return m_pShaderCache->waitForEntry(rawHandle);
}

// =====================================================================================================================
Result ShaderCacheWrap::GetValue(Vkgc::RawEntryHandle rawHandle, void *pData, size_t *pDataLen) {
  assert(0);
  return Result::ErrorUnavailable;
}

// =====================================================================================================================
Result ShaderCacheWrap::GetValueZeroCopy(Vkgc::RawEntryHandle rawHandle, const void **ppData, size_t *pDataLen) {
  return m_pShaderCache->retrieveShader(rawHandle, ppData, pDataLen);
}

// =====================================================================================================================
Result ShaderCacheWrap::SetValue(Vkgc::RawEntryHandle rawHandle, bool success, const void *pData, size_t dataLen) {
  m_pShaderCache->insertShader(rawHandle, pData, dataLen);
  return Result::Success;
}

} // namespace Llpc
