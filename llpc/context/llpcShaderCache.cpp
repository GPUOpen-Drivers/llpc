/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
@file llpcShaderCache.cpp
@brief LLPC source file: contains implementation of class Llpc::ShaderCache.
***********************************************************************************************************************
*/
#include "llpcShaderCache.h"
#include "vkgcUtil.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/DJB.h"
#include "llvm/Support/FileSystem.h"
#include <string.h>

#ifndef LLPC_HAS_LZ4
#define LLPC_HAS_LZ4 0
#endif

#if LLPC_USE_EXPERIMENTAL_SHADER_CACHE_PIPELINES && LLPC_HAS_LZ4
#include "lz4.h"
#include "lz4hc.h"
#endif

#define DEBUG_TYPE "llpc-shader-cache"

using namespace llvm;

namespace Llpc {

#if defined(__unix__)
static const char CacheFileSubPath[] = "/AMD/LlpcCache/";
#else
static const char CacheFileSubPath[] = "\\AMD\\LlpcCache\\";
#endif

static const char ClientStr[] = "LLPC";

static constexpr uint64_t CrcWidth = sizeof(uint64_t) * 8;
static constexpr uint64_t CrcInitialValue = 0xFFFFFFFFFFFFFFFF;

static const uint64_t CrcLookup[256] = {
    0x0000000000000000, 0xAD93D23594C935A9, 0xF6B4765EBD5B5EFB, 0x5B27A46B29926B52, 0x40FB3E88EE7F885F,
    0xED68ECBD7AB6BDF6, 0xB64F48D65324D6A4, 0x1BDC9AE3C7EDE30D, 0x81F67D11DCFF10BE, 0x2C65AF2448362517,
    0x77420B4F61A44E45, 0xDAD1D97AF56D7BEC, 0xC10D4399328098E1, 0x6C9E91ACA649AD48, 0x37B935C78FDBC61A,
    0x9A2AE7F21B12F3B3, 0xAE7F28162D3714D5, 0x03ECFA23B9FE217C, 0x58CB5E48906C4A2E, 0xF5588C7D04A57F87,
    0xEE84169EC3489C8A, 0x4317C4AB5781A923, 0x183060C07E13C271, 0xB5A3B2F5EADAF7D8, 0x2F895507F1C8046B,
    0x821A8732650131C2, 0xD93D23594C935A90, 0x74AEF16CD85A6F39, 0x6F726B8F1FB78C34, 0xC2E1B9BA8B7EB99D,
    0x99C61DD1A2ECD2CF, 0x3455CFE43625E766, 0xF16D8219CEA71C03, 0x5CFE502C5A6E29AA, 0x07D9F44773FC42F8,
    0xAA4A2672E7357751, 0xB196BC9120D8945C, 0x1C056EA4B411A1F5, 0x4722CACF9D83CAA7, 0xEAB118FA094AFF0E,
    0x709BFF0812580CBD, 0xDD082D3D86913914, 0x862F8956AF035246, 0x2BBC5B633BCA67EF, 0x3060C180FC2784E2,
    0x9DF313B568EEB14B, 0xC6D4B7DE417CDA19, 0x6B4765EBD5B5EFB0, 0x5F12AA0FE39008D6, 0xF281783A77593D7F,
    0xA9A6DC515ECB562D, 0x04350E64CA026384, 0x1FE994870DEF8089, 0xB27A46B29926B520, 0xE95DE2D9B0B4DE72,
    0x44CE30EC247DEBDB, 0xDEE4D71E3F6F1868, 0x7377052BABA62DC1, 0x2850A14082344693, 0x85C3737516FD733A,
    0x9E1FE996D1109037, 0x338C3BA345D9A59E, 0x68AB9FC86C4BCECC, 0xC5384DFDF882FB65, 0x4F48D60609870DAF,
    0xE2DB04339D4E3806, 0xB9FCA058B4DC5354, 0x146F726D201566FD, 0x0FB3E88EE7F885F0, 0xA2203ABB7331B059,
    0xF9079ED05AA3DB0B, 0x54944CE5CE6AEEA2, 0xCEBEAB17D5781D11, 0x632D792241B128B8, 0x380ADD49682343EA,
    0x95990F7CFCEA7643, 0x8E45959F3B07954E, 0x23D647AAAFCEA0E7, 0x78F1E3C1865CCBB5, 0xD56231F41295FE1C,
    0xE137FE1024B0197A, 0x4CA42C25B0792CD3, 0x1783884E99EB4781, 0xBA105A7B0D227228, 0xA1CCC098CACF9125,
    0x0C5F12AD5E06A48C, 0x5778B6C67794CFDE, 0xFAEB64F3E35DFA77, 0x60C18301F84F09C4, 0xCD5251346C863C6D,
    0x9675F55F4514573F, 0x3BE6276AD1DD6296, 0x203ABD891630819B, 0x8DA96FBC82F9B432, 0xD68ECBD7AB6BDF60,
    0x7B1D19E23FA2EAC9, 0xBE25541FC72011AC, 0x13B6862A53E92405, 0x489122417A7B4F57, 0xE502F074EEB27AFE,
    0xFEDE6A97295F99F3, 0x534DB8A2BD96AC5A, 0x086A1CC99404C708, 0xA5F9CEFC00CDF2A1, 0x3FD3290E1BDF0112,
    0x9240FB3B8F1634BB, 0xC9675F50A6845FE9, 0x64F48D65324D6A40, 0x7F281786F5A0894D, 0xD2BBC5B36169BCE4,
    0x899C61D848FBD7B6, 0x240FB3EDDC32E21F, 0x105A7C09EA170579, 0xBDC9AE3C7EDE30D0, 0xE6EE0A57574C5B82,
    0x4B7DD862C3856E2B, 0x50A1428104688D26, 0xFD3290B490A1B88F, 0xA61534DFB933D3DD, 0x0B86E6EA2DFAE674,
    0x91AC011836E815C7, 0x3C3FD32DA221206E, 0x671877468BB34B3C, 0xCA8BA5731F7A7E95, 0xD1573F90D8979D98,
    0x7CC4EDA54C5EA831, 0x27E349CE65CCC363, 0x8A709BFBF105F6CA, 0x9E91AC0C130E1B5E, 0x33027E3987C72EF7,
    0x6825DA52AE5545A5, 0xC5B608673A9C700C, 0xDE6A9284FD719301, 0x73F940B169B8A6A8, 0x28DEE4DA402ACDFA,
    0x854D36EFD4E3F853, 0x1F67D11DCFF10BE0, 0xB2F403285B383E49, 0xE9D3A74372AA551B, 0x44407576E66360B2,
    0x5F9CEF95218E83BF, 0xF20F3DA0B547B616, 0xA92899CB9CD5DD44, 0x04BB4BFE081CE8ED, 0x30EE841A3E390F8B,
    0x9D7D562FAAF03A22, 0xC65AF24483625170, 0x6BC9207117AB64D9, 0x7015BA92D04687D4, 0xDD8668A7448FB27D,
    0x86A1CCCC6D1DD92F, 0x2B321EF9F9D4EC86, 0xB118F90BE2C61F35, 0x1C8B2B3E760F2A9C, 0x47AC8F555F9D41CE,
    0xEA3F5D60CB547467, 0xF1E3C7830CB9976A, 0x5C7015B69870A2C3, 0x0757B1DDB1E2C991, 0xAAC463E8252BFC38,
    0x6FFC2E15DDA9075D, 0xC26FFC20496032F4, 0x9948584B60F259A6, 0x34DB8A7EF43B6C0F, 0x2F07109D33D68F02,
    0x8294C2A8A71FBAAB, 0xD9B366C38E8DD1F9, 0x7420B4F61A44E450, 0xEE0A5304015617E3, 0x43998131959F224A,
    0x18BE255ABC0D4918, 0xB52DF76F28C47CB1, 0xAEF16D8CEF299FBC, 0x0362BFB97BE0AA15, 0x58451BD25272C147,
    0xF5D6C9E7C6BBF4EE, 0xC1830603F09E1388, 0x6C10D43664572621, 0x3737705D4DC54D73, 0x9AA4A268D90C78DA,
    0x8178388B1EE19BD7, 0x2CEBEABE8A28AE7E, 0x77CC4ED5A3BAC52C, 0xDA5F9CE03773F085, 0x40757B122C610336,
    0xEDE6A927B8A8369F, 0xB6C10D4C913A5DCD, 0x1B52DF7905F36864, 0x008E459AC21E8B69, 0xAD1D97AF56D7BEC0,
    0xF63A33C47F45D592, 0x5BA9E1F1EB8CE03B, 0xD1D97A0A1A8916F1, 0x7C4AA83F8E402358, 0x276D0C54A7D2480A,
    0x8AFEDE61331B7DA3, 0x91224482F4F69EAE, 0x3CB196B7603FAB07, 0x679632DC49ADC055, 0xCA05E0E9DD64F5FC,
    0x502F071BC676064F, 0xFDBCD52E52BF33E6, 0xA69B71457B2D58B4, 0x0B08A370EFE46D1D, 0x10D4399328098E10,
    0xBD47EBA6BCC0BBB9, 0xE6604FCD9552D0EB, 0x4BF39DF8019BE542, 0x7FA6521C37BE0224, 0xD2358029A377378D,
    0x891224428AE55CDF, 0x2481F6771E2C6976, 0x3F5D6C94D9C18A7B, 0x92CEBEA14D08BFD2, 0xC9E91ACA649AD480,
    0x647AC8FFF053E129, 0xFE502F0DEB41129A, 0x53C3FD387F882733, 0x08E45953561A4C61, 0xA5778B66C2D379C8,
    0xBEAB1185053E9AC5, 0x1338C3B091F7AF6C, 0x481F67DBB865C43E, 0xE58CB5EE2CACF197, 0x20B4F813D42E0AF2,
    0x8D272A2640E73F5B, 0xD6008E4D69755409, 0x7B935C78FDBC61A0, 0x604FC69B3A5182AD, 0xCDDC14AEAE98B704,
    0x96FBB0C5870ADC56, 0x3B6862F013C3E9FF, 0xA142850208D11A4C, 0x0CD157379C182FE5, 0x57F6F35CB58A44B7,
    0xFA6521692143711E, 0xE1B9BB8AE6AE9213, 0x4C2A69BF7267A7BA, 0x170DCDD45BF5CCE8, 0xBA9E1FE1CF3CF941,
    0x8ECBD005F9191E27, 0x235802306DD02B8E, 0x787FA65B444240DC, 0xD5EC746ED08B7575, 0xCE30EE8D17669678,
    0x63A33CB883AFA3D1, 0x388498D3AA3DC883, 0x95174AE63EF4FD2A, 0x0F3DAD1425E60E99, 0xA2AE7F21B12F3B30,
    0xF989DB4A98BD5062, 0x541A097F0C7465CB, 0x4FC6939CCB9986C6, 0xE25541A95F50B36F, 0xB972E5C276C2D83D,
    0x14E137F7E20BED94};

// =====================================================================================================================
ShaderCache::ShaderCache()
    : m_onDiskFile(), m_disableCache(true), m_shaderDataEnd(sizeof(ShaderCacheSerializedHeader)), m_totalShaders(0),
      m_serializedSize(sizeof(ShaderCacheSerializedHeader)), m_getValueFunc(nullptr), m_storeValueFunc(nullptr) {
  memset(m_fileFullPath, 0, MaxFilePathLen);
  memset(&m_gfxIp, 0, sizeof(m_gfxIp));
}

// =====================================================================================================================
ShaderCache::~ShaderCache() {
  Destroy();
}

// =====================================================================================================================
// Destruction, does clean-up work.
void ShaderCache::Destroy() {
  if (m_onDiskFile.isOpen())
    m_onDiskFile.close();
  resetRuntimeCache();
}

// =====================================================================================================================
// Resets the runtime shader cache to an empty state. Releases all allocator memory and decommits it back to the OS.
void ShaderCache::resetRuntimeCache() {
  for (auto indexMap : m_shaderIndexMap)
    delete indexMap.second;
  m_shaderIndexMap.clear();

  for (auto allocIt : m_allocationList)
    delete[] allocIt.first;
  m_allocationList.clear();

  m_totalShaders = 0;
  m_shaderDataEnd = sizeof(ShaderCacheSerializedHeader);
  m_serializedSize = sizeof(ShaderCacheSerializedHeader);
}

// =====================================================================================================================
// Copies the shader cache data to the memory blob provided by the calling function.
//
// NOTE: It is expected that the calling function has not used this shader cache since querying the size
//
// @param [out] blob : System memory pointer where the serialized data should be placed
// @param [in,out] size : Size of the memory pointed to by pBlob. If the value stored in pSize is zero then no data will
// be copied and instead the size required for serialization will be returned in pSize
Result ShaderCache::Serialize(void *blob, size_t *size) {
  Result result = Result::Success;

  if (*size == 0) {
    // Query shader cache serailzied size
    (*size) = m_serializedSize;
  } else {
    // Do serialize
    assert(m_shaderDataEnd == m_serializedSize || m_shaderDataEnd == sizeof(ShaderCacheSerializedHeader));

    if (m_serializedSize >= sizeof(ShaderCacheSerializedHeader)) {
      if (blob && (*size) >= m_serializedSize) {
        // First construct the header and copy it into the memory provided
        ShaderCacheSerializedHeader header = {};
        header.headerSize = sizeof(ShaderCacheSerializedHeader);
        header.shaderCount = m_totalShaders;
        header.shaderDataEnd = m_shaderDataEnd;
        getBuildTime(&header.buildId);

        memcpy(blob, &header, sizeof(ShaderCacheSerializedHeader));

        void *dataDst = voidPtrInc(blob, sizeof(ShaderCacheSerializedHeader));

        // Then iterate through all allocators (which hold the backing memory for the shader data)
        // and copy their contents to the blob.
        for (auto it : m_allocationList) {
          assert(it.first);

          const size_t copySize = it.second;
          if (voidPtrDiff(dataDst, blob) + copySize > (*size)) {
            result = Result::ErrorUnknown;
            break;
          }

          memcpy(dataDst, it.first, copySize);
          dataDst = voidPtrInc(dataDst, copySize);
        }
      } else {
        llvm_unreachable("Should never be called!");
        result = Result::ErrorUnknown;
      }
    }
  }

  return result;
}

// =====================================================================================================================
// Merges the shader data of source shader caches into this shader cache.
//
// @param srcCacheCount : Count of input source shader caches
// @param ppSrcCaches : Input shader caches
Result ShaderCache::Merge(unsigned srcCacheCount, const IShaderCache **ppSrcCaches) {
  // Merge function is supposed to be called by client created shader caches, which are always runtime mode.
  assert(m_fileFullPath[0] == '\0');

  Result result = Result::Success;

  lockCacheMap(false);

  for (unsigned i = 0; i < srcCacheCount; i++) {
    ShaderCache *srcCache = static_cast<ShaderCache *>(const_cast<IShaderCache *>(ppSrcCaches[i]));
    srcCache->lockCacheMap(true);

    for (auto it : srcCache->m_shaderIndexMap) {
      uint64_t key = it.first;

      auto indexMap = m_shaderIndexMap.find(key);
      if (indexMap == m_shaderIndexMap.end()) {
        ShaderIndex *index = nullptr;
        void *mem = getCacheSpace(it.second->header.size);
        memcpy(mem, it.second->dataBlob, it.second->header.size);

        index = new ShaderIndex;
        index->dataBlob = mem;
        index->state = ShaderEntryState::Ready;
        index->header = it.second->header;

        m_shaderIndexMap[key] = index;
        m_totalShaders++;
      }
    }
    srcCache->unlockCacheMap(true);
  }

  unlockCacheMap(false);

  return result;
}

// =====================================================================================================================
// Initializes the Shader Cache in late stage.
//
// @param createInfo : Shader cache create info
// @param auxCreateInfo : Shader cache auxiliary info (static fields)
Result ShaderCache::init(const ShaderCacheCreateInfo *createInfo, const ShaderCacheAuxCreateInfo *auxCreateInfo) {
  Result result = Result::Success;

  if (auxCreateInfo->shaderCacheMode != ShaderCacheDisable) {
    m_disableCache = false;
    m_clientData = createInfo->pClientData;
    m_getValueFunc = createInfo->pfnGetValueFunc;
    m_storeValueFunc = createInfo->pfnStoreValueFunc;
    m_gfxIp = auxCreateInfo->gfxIp;
    m_hash = auxCreateInfo->hash;

    lockCacheMap(false);

    // If we're in runtime mode and the caller provided a data blob, try to load the from that blob.
    if (auxCreateInfo->shaderCacheMode == ShaderCacheEnableRuntime && createInfo->initialDataSize > 0) {
      if (loadCacheFromBlob(createInfo->pInitialData, createInfo->initialDataSize) != Result::Success)
        resetRuntimeCache();
    }
    // If we're in on-disk mode try to load the cache from file.
    else if (auxCreateInfo->shaderCacheMode == ShaderCacheEnableOnDisk ||
             auxCreateInfo->shaderCacheMode == ShaderCacheForceInternalCacheOnDisk ||
             auxCreateInfo->shaderCacheMode == ShaderCacheEnableOnDiskReadOnly) {
      // Default to false because the cache file is invalid if it's brand new
      bool cacheFileExists = false;

      // Build the cache file name and make required directories if necessary
      // cacheFileValid gets initially set based on whether the file exists.
      result = buildFileName(auxCreateInfo->executableName, auxCreateInfo->cacheFilePath, auxCreateInfo->gfxIp,
                             &cacheFileExists);

      if (result == Result::Success) {
        // Open the storage file if it exists
        if (cacheFileExists) {
          if (auxCreateInfo->shaderCacheMode == ShaderCacheEnableOnDiskReadOnly)
            result = m_onDiskFile.open(m_fileFullPath, (FileAccessRead | FileAccessBinary));
          else
            result = m_onDiskFile.open(m_fileFullPath, (FileAccessReadUpdate | FileAccessBinary));
        } else
          // Create the storage file if it does not exist
          result = m_onDiskFile.open(m_fileFullPath, (FileAccessRead | FileAccessAppend | FileAccessBinary));
      }

      Result loadResult = Result::ErrorUnknown;
      // If the cache file already existed, then we can try loading the data from it
      if (result == Result::Success) {
        if (cacheFileExists) {
          loadResult = loadCacheFromFile();
          if (auxCreateInfo->shaderCacheMode == ShaderCacheEnableOnDiskReadOnly && loadResult == Result::Success)
            m_onDiskFile.close();
        } else
          resetCacheFile();
      }

      // Either the file is new or had invalid data so we need to reset the index hash map and release
      // any memory allocated
      if (loadResult != Result::Success)
        resetRuntimeCache();
    }

    unlockCacheMap(false);
  } else
    m_disableCache = true;

  return result;
}

// =====================================================================================================================
// Constructs the on disk cache file name and path and puts it in m_fileFullPath. This function also creates any
// any missing directories in the full path to the cache file.
//
// @param executableName : Name of Executable file
// @param cacheFilePath : Root directory of cache file
// @param gfxIp : Graphics IP version info
// @param [out] cacheFileExists : Whether cache file exists
Result ShaderCache::buildFileName(const char *executableName, const char *cacheFilePath, GfxIpVersion gfxIp,
                                  bool *cacheFileExists) {
  // The file name is constructed by taking the executable file name, appending the client string, device ID and
  // GPU index then hashing the result.
  char hashedFileName[MaxFilePathLen];
  int length = snprintf(hashedFileName, MaxFilePathLen, "%s.%s.%u.%u.%u", executableName, ClientStr, gfxIp.major,
                        gfxIp.minor, gfxIp.stepping);

  const unsigned nameHash = djbHash(hashedFileName, 0);
  length = snprintf(hashedFileName, MaxFilePathLen, "%08x.bin", nameHash);

  // Combine the base path, the sub-path and the file name to get the fully qualified path to the cache file
  length = snprintf(m_fileFullPath, MaxFilePathLen, "%s%s%s", cacheFilePath, CacheFileSubPath, hashedFileName);

  assert(cacheFileExists);
  *cacheFileExists = File::exists(m_fileFullPath);
  Result result = Result::Success;
  if (!*cacheFileExists) {
    length = snprintf(hashedFileName, MaxFilePathLen, "%s%s", cacheFilePath, CacheFileSubPath);
    std::error_code errCode = sys::fs::create_directories(hashedFileName);
    if (!errCode)
      result = Result::Success;
  }

  return result;
}

// =====================================================================================================================
// Resets the contents of the cache file, assumes the shader cache has been locked for writes.
void ShaderCache::resetCacheFile() {
  m_onDiskFile.close();
  Result fileResult = m_onDiskFile.open(m_fileFullPath, (FileAccessRead | FileAccessWrite | FileAccessBinary));
  assert(fileResult == Result::Success);
  (void(fileResult)); // unused

  ShaderCacheSerializedHeader header = {};
  header.headerSize = sizeof(ShaderCacheSerializedHeader);
  header.shaderCount = 0;
  header.shaderDataEnd = header.headerSize;
  getBuildTime(&header.buildId);

  m_onDiskFile.write(&header, header.headerSize);
}

// =====================================================================================================================
// Searches the shader cache for a shader with the matching key, allocating a new entry if it didn't already exist.
//
// Returns:
//    Ready       - if a matching shader was found and is ready for use
//    Compiling   - if an entry was created and must be compiled/populated by the caller
//    Unavailable - if an unrecoverable error was encountered
//
// @param hash : Hash code of shader
// @param allocateOnMiss : Whether allocate a new entry for new hash
// @param [out] phEntry : Handle of shader cache entry
ShaderEntryState ShaderCache::findShader(MetroHash::Hash hash, bool allocateOnMiss, CacheEntryHandle *phEntry) {
  // Early return if shader cache is disabled
  if (m_disableCache) {
    *phEntry = nullptr;
    return ShaderEntryState::Compiling;
  }

  ShaderEntryState result = ShaderEntryState::Unavailable;
  bool existed = false;
  ShaderIndex *index = nullptr;
  Result mapResult = Result::Success;
  assert(phEntry);

  bool readOnlyLock = (!allocateOnMiss);
  lockCacheMap(readOnlyLock);
  uint64_t hashKey = MetroHash::compact64(&hash);
  auto indexMap = m_shaderIndexMap.find(hashKey);
  if (indexMap != m_shaderIndexMap.end()) {
    existed = true;
    index = indexMap->second;
  } else if (allocateOnMiss) {
    index = new ShaderIndex;
    m_shaderIndexMap[hashKey] = index;
  }

  if (!index)
    mapResult = Result::ErrorUnavailable;

  if (mapResult == Result::Success) {
    if (existed) {
      // We don't need to hold on to the write lock if we're not the one doing the compile
      if (!readOnlyLock) {
        unlockCacheMap(readOnlyLock);
        readOnlyLock = true;
        lockCacheMap(readOnlyLock);
      }
    } else {
      bool needsInit = true;

      // We didn't find the entry in our own hash map, now search the external cache if available
      if (useExternalCache()) {
        // The first call to the external cache queries the existence and the size of the cached shader.
        Result extResult = m_getValueFunc(m_clientData, hashKey, nullptr, &index->header.size);
        if (extResult == Result::Success) {
          // An entry was found matching our hash, we should allocate memory to hold the data and call again
          assert(index->header.size > 0);
          index->dataBlob = getCacheSpace(index->header.size);

          if (!index->dataBlob)
            extResult = Result::ErrorOutOfMemory;
          else {
            extResult = m_getValueFunc(m_clientData, hashKey, index->dataBlob, &index->header.size);
          }
        }

        if (extResult == Result::Success) {
          // We now have a copy of the shader data from the external cache, just need to update the
          // ShaderIndex. The first item in the data blob is a ShaderHeader, followed by the serialized
          // data blob for the shader.
          const auto *const header = static_cast<const ShaderHeader *>(index->dataBlob);
          assert(index->header.size == header->size);

          index->header = (*header);
          index->state = ShaderEntryState::Ready;
          needsInit = false;
        } else if (extResult == Result::ErrorUnavailable) {
          // This means the external cache is unavailable and we shouldn't bother using it anymore. To
          // prevent useless calls we'll zero out the function pointers.
          m_getValueFunc = nullptr;
          m_storeValueFunc = nullptr;
        } else {
          // extResult should never be ErrorInvalidMemorySize since Cache space is always allocated based
          // on 1st m_pfnGetValueFunc call.
          assert(extResult != Result::ErrorOutOfMemory);

          // Any other result means we just need to continue with initializing the new index/compiling.
        }
      }

      if (needsInit) {
        // This is a brand new cache entry so we need to initialize the ShaderIndex.
        memset(index, 0, sizeof(*index));
        index->header.key = hashKey;
        index->state = ShaderEntryState::New;
      }
    } // End if (existed == false)

    if (index->state == ShaderEntryState::Compiling) {
      // The shader is being compiled by another thread, we should release the lock and wait for it to complete
      while (index->state == ShaderEntryState::Compiling) {
        unlockCacheMap(readOnlyLock);
        {
          std::unique_lock<std::mutex> lock(m_conditionMutex);

          m_conditionVariable.wait_for(lock, std::chrono::seconds(1));
        }
        lockCacheMap(readOnlyLock);
      }
      // At this point the shader entry is either Ready, New or something failed. We've already
      // initialized our result code to an error code above, the Ready and New cases are handled below so
      // nothing else to do here.
    }

    if (index->state == ShaderEntryState::Ready) {
      // The shader has been compiled, just verify it has valid data and then return success.
      assert(index->dataBlob && index->header.size != 0);
    } else if (index->state == ShaderEntryState::New) {
      // The shader entry is new (or previously failed compilation) and we're the first thread to get a
      // crack at it, move it into the Compiling state
      index->state = ShaderEntryState::Compiling;
    }

    // Return the ShaderIndex as a handle so subsequent calls into the cache can avoid the hash map lookup.
    (*phEntry) = index;
    result = index->state;
  }

  unlockCacheMap(readOnlyLock);

  return result;
}

// =====================================================================================================================
// Inserts a new shader into the cache. The new shader is written to the cache file if it is in-use, and will also
// upload it to the client's external cache if it is in-use.
//
// @param hEntry : Handle of shader cache entry
// @param blob : Shader data
// @param shaderSize : size of shader data in bytes
void ShaderCache::insertShader(CacheEntryHandle hEntry, const void *blob, size_t shaderSize) {
  auto *const index = static_cast<ShaderIndex *>(hEntry);
  assert(m_disableCache == false);
  assert(index && index->state == ShaderEntryState::Compiling);

  lockCacheMap(false);

  Result result = Result::Success;

  if (result == Result::Success) {
    // Allocate space to store the serialized shader and a copy of the header. The header is duplicated in the
    // data to simplify serialize/load.
    index->header.size = (shaderSize + sizeof(ShaderHeader));
    index->dataBlob = getCacheSpace(index->header.size);

    if (!index->dataBlob)
      result = Result::ErrorOutOfMemory;
    else {
      ++m_totalShaders;

      auto *const header = static_cast<ShaderHeader *>(index->dataBlob);
      void *const dataBlob = (header + 1);

      // Serialize the shader into an opaque blob of data.
      memcpy(dataBlob, blob, shaderSize);

      // Compute a CRC for the serialized data (useful for detecting data corruption), and copy the index's
      // header into the data's header.
      index->header.crc = calculateCrc(static_cast<uint8_t *>(dataBlob), shaderSize);
      (*header) = index->header;

      if (useExternalCache()) {
        // If we're making use of the external shader cache then we need to store the compiled shader data here.
        Result externalResult = m_storeValueFunc(m_clientData, index->header.key, index->dataBlob, index->header.size);
        if (externalResult == Result::ErrorUnavailable) {
          // This is the only return code we can do anything about. In this case it means the external cache
          // is not available and we should zero out the function pointers to avoid making useless calls on
          // subsequent shader compiles.
          m_getValueFunc = nullptr;
          m_storeValueFunc = nullptr;
        } else {
          // Otherwise the store either succeeded (yay!) or failed in some other transient way. Either way,
          // we will just continue, there's nothing to be done.
        }
      }

      // Mark this entry as ready, we'll wake the waiting threads once we release the lock
      index->state = ShaderEntryState::Ready;

      // Finally, update the file if necessary.
      if (m_onDiskFile.isOpen())
        addShaderToFile(index);
    }
  }

  if (result != Result::Success) {
    // Something failed while attempting to add the shader, most likely memory allocation. There's not much we
    // can do here except give up on adding data. This means we need to set the entry back to New so if another
    // thread is waiting it will be allowed to continue (it will likely just get to this same point, but at least
    // we won't hang or crash).
    index->state = ShaderEntryState::New;
    index->header.size = 0;
    index->dataBlob = nullptr;
  }

  unlockCacheMap(false);
  m_conditionVariable.notify_all();
}

// =====================================================================================================================
// Resets cache entry state to new. It is used when shader compile fails.
//
// @param hEntry : Handle of shader cache entry
void ShaderCache::resetShader(CacheEntryHandle hEntry) {
  auto *const index = static_cast<ShaderIndex *>(hEntry);
  assert(m_disableCache == false);
  assert(index && index->state == ShaderEntryState::Compiling);
  lockCacheMap(false);
  index->state = ShaderEntryState::New;
  index->header.size = 0;
  index->dataBlob = nullptr;
  unlockCacheMap(false);
  m_conditionVariable.notify_all();
}

// =====================================================================================================================
// Retrieves the shader from the cache which is identified by the specified entry handle.
//
// @param hEntry : Handle of shader cache entry
// @param [out] ppBlob : Shader data
// @param [out] size : size of shader data in bytes
Result ShaderCache::retrieveShader(CacheEntryHandle hEntry, const void **ppBlob, size_t *size) {
  const auto *const index = static_cast<ShaderIndex *>(hEntry);

  assert(m_disableCache == false);
  assert(index);
  assert(index->header.size >= sizeof(ShaderHeader));

  lockCacheMap(true);

  *ppBlob = voidPtrInc(index->dataBlob, sizeof(ShaderHeader));
  *size = index->header.size - sizeof(ShaderHeader);

  unlockCacheMap(true);

  return *size > 0 ? Result::Success : Result::ErrorUnknown;
}

// =====================================================================================================================
// Adds data for a new shader to the on-disk file
//
// @param index : A new shader
void ShaderCache::addShaderToFile(const ShaderIndex *index) {
  assert(m_onDiskFile.isOpen());

  // We only need to update the parts of the file that changed, which is the number of shaders, the new data section,
  // and the shaderDataEnd.

  // Calculate the header offsets, then write the relavent data to the file.
  const unsigned shaderCountOffset = offsetof(struct ShaderCacheSerializedHeader, shaderCount);
  const unsigned dataEndOffset = offsetof(struct ShaderCacheSerializedHeader, shaderDataEnd);

  m_onDiskFile.seek(shaderCountOffset, true);
  m_onDiskFile.write(&m_totalShaders, sizeof(size_t));

  // Write the new shader data at the current end of the data section
  m_onDiskFile.seek(static_cast<unsigned>(m_shaderDataEnd), true);
  m_onDiskFile.write(index->dataBlob, index->header.size);

  // Then update the data end value and write it out to the file.
  m_shaderDataEnd += index->header.size;
  m_onDiskFile.seek(dataEndOffset, true);
  m_onDiskFile.write(&m_shaderDataEnd, sizeof(size_t));

  m_onDiskFile.flush();
}

// =====================================================================================================================
// Loads all shader data from the cache file into the local cache copy. Returns true if the file contents were loaded
// successfully or false if invalid data was found.
//
// NOTE: This function assumes that a write lock has already been taken by the calling function and that the on-disk
// file has been successfully opened and the file position is the beginning of the file.
Result ShaderCache::loadCacheFromFile() {
  assert(m_onDiskFile.isOpen());

  // Read the header from the file and validate it
  ShaderCacheSerializedHeader header = {};
  m_onDiskFile.rewind();
  m_onDiskFile.read(&header, sizeof(ShaderCacheSerializedHeader), nullptr);

  const size_t fileSize = File::getFileSize(m_fileFullPath);
  const size_t dataSize = fileSize - sizeof(ShaderCacheSerializedHeader);
  Result result = validateAndLoadHeader(&header, fileSize);

  void *dataMem = nullptr;
  if (result == Result::Success) {
    // The header is valid, so allocate space to fit all of the shader data.
    dataMem = getCacheSpace(dataSize);
  }

  if (result == Result::Success) {
    if (dataMem) {
      // Read the shader data into the allocated memory.
      m_onDiskFile.seek(sizeof(ShaderCacheSerializedHeader), true);
      size_t bytesRead = 0;
      result = m_onDiskFile.read(dataMem, dataSize, &bytesRead);

      // If we didn't read the correct number of bytes then something went wrong and we should return a failure
      if (bytesRead != dataSize)
        result = Result::ErrorUnknown;
    } else
      result = Result::ErrorOutOfMemory;
  }

  if (result == Result::Success) {
    // Now setup the shader index hash map.
    result = populateIndexMap(dataMem, dataSize);
  }

  if (result != Result::Success) {
    // Something went wrong in loading the file, so reset it
    resetCacheFile();
  }

  return result;
}

// =====================================================================================================================
// Loads all shader data from a client provided initial data blob. Returns true if the file contents were loaded
// successfully or false if invalid data was found.
//
// NOTE: This function assumes that a write lock has already been taken by the calling function.
//
// @param initialData : Initial data of the shader cache
// @param initialDataSize : Size of initial data
Result ShaderCache::loadCacheFromBlob(const void *initialData, size_t initialDataSize) {
  const auto *header = static_cast<const ShaderCacheSerializedHeader *>(initialData);
  assert(initialData);

  // First verify that the header data is valid
  Result result = validateAndLoadHeader(header, initialDataSize);

  if (result == Result::Success) {
    // The header appears valid so allocate space for the shader data.
    const size_t dataSize = initialDataSize - header->headerSize;
    void *dataMem = getCacheSpace(dataSize);

    if (dataMem) {
      // Then copy the data and setup the shader index hash map.
      memcpy(dataMem, voidPtrInc(initialData, header->headerSize), dataSize);
      result = populateIndexMap(dataMem, dataSize);
    } else
      result = Result::ErrorOutOfMemory;
  }

  return result;
}

// =====================================================================================================================
// Validates shader data (from a file or a blob) by checking the CRCs and adding index hash map entries if successful.
// Will return a failure if any of the shader data is invalid.
//
// @param dataStart : Start pointer of cached shader data
// @param dataSize : Shader data size in bytes
Result ShaderCache::populateIndexMap(void *dataStart, size_t dataSize) {
  Result result = Result::Success;

  // Iterate through all of the entries to verify the data CRC, zero out the GPU memory pointer/offset and add to the
  // hashmap. We zero out the GPU memory data here because we're already iterating through each entry, rather than
  // take the hit each time we add shader data to the file.
  auto *header = static_cast<ShaderHeader *>(dataStart);

  for (unsigned shader = 0; (shader < m_totalShaders && result == Result::Success); ++shader) {
    // Guard against buffer overruns.
    assert(voidPtrDiff(header, dataStart) <= dataSize);

    // TODO: Add a static function to RelocatableShader to validate the input data.

    // The serialized data blob representing each RelocatableShader object immediately follows the header.
    void *const dataBlob = (header + 1);

    // Verify the CRC
    const uint64_t crc = calculateCrc(static_cast<uint8_t *>(dataBlob), (header->size - sizeof(ShaderHeader)));

    if (crc == header->crc) {
      // It all checks out, so add this shader to the hash map!
      ShaderIndex *index = nullptr;
      auto indexMap = m_shaderIndexMap.find(header->key);
      if (indexMap == m_shaderIndexMap.end()) {
        index = new ShaderIndex;
        index->header = (*header);
        index->dataBlob = header;
        index->state = ShaderEntryState::Ready;
        m_shaderIndexMap[header->key] = index;
      }
    } else
      result = Result::ErrorUnknown;

    // Move to next entry in cache
    header = static_cast<ShaderHeader *>(voidPtrInc(header, header->size));
  }

  return result;
}

// =====================================================================================================================
// Caclulates a 64-bit CRC of the data provided
//
// @param data : Data need generate CRC
// @param numBytes : Data size in bytes
uint64_t ShaderCache::calculateCrc(const uint8_t *data, size_t numBytes) {
  uint64_t crc = CrcInitialValue;
  for (unsigned byte = 0; byte < numBytes; ++byte) {
    uint8_t tableIndex = static_cast<uint8_t>(crc >> (CrcWidth - 8)) & 0xFF;
    crc = (crc << 8) ^ CrcLookup[tableIndex] ^ data[byte];
  }

  return crc;
}

// =====================================================================================================================
// Validates the provided header and stores the data contained within it if valid.
//
// @param header : Cache file header
// @param dataSourceSize : Data size in byte
Result ShaderCache::validateAndLoadHeader(const ShaderCacheSerializedHeader *header, size_t dataSourceSize) {
  assert(header);

  BuildUniqueId buildId;
  getBuildTime(&buildId);

  Result result = Result::Success;

  if (header->headerSize == sizeof(ShaderCacheSerializedHeader) &&
      memcmp(header->buildId.buildDate, buildId.buildDate, sizeof(buildId.buildDate)) == 0 &&
      memcmp(header->buildId.buildTime, buildId.buildTime, sizeof(buildId.buildTime)) == 0 &&
      memcmp(&header->buildId.gfxIp, &buildId.gfxIp, sizeof(buildId.gfxIp)) == 0 &&
      memcmp(&header->buildId.hash, &buildId.hash, sizeof(buildId.hash)) == 0) {
    // The header appears valid so copy the header data to the runtime cache
    m_totalShaders = header->shaderCount;
    m_shaderDataEnd = header->shaderDataEnd;
  } else
    result = Result::ErrorUnknown;

  // Make sure the shader data end value is correct. It's ok for there to be unused space at the end of the file, but
  // if the shaderDataEnd is beyond the end of the file we have a problem.
  if (result == Result::Success && m_shaderDataEnd > dataSourceSize)
    result = Result::ErrorUnknown;

  return result;
}

// =====================================================================================================================
// Allocates memory from the shader cache's linear allocator. This function assumes that a write lock has been taken by
// the calling function.
//
// @param numBytes : Allocation size in bytes
void *ShaderCache::getCacheSpace(size_t numBytes) {
  auto p = new uint8_t[numBytes];
  m_allocationList.push_back(std::pair<uint8_t *, size_t>(p, numBytes));
  m_serializedSize += numBytes;
  return p;
}

// =====================================================================================================================
// Returns the time & date that pipeline.cpp was compiled.
//
// @param [out] buildId : Unique ID of build info
void ShaderCache::getBuildTime(BuildUniqueId *buildId) {
  memset(buildId, 0, sizeof(buildId[0]));
  memcpy(&buildId->buildDate, __DATE__, std::min(strlen(__DATE__), sizeof(buildId->buildDate)));
  memcpy(&buildId->buildTime, __TIME__, std::min(strlen(__TIME__), sizeof(buildId->buildTime)));
  memcpy(&buildId->gfxIp, &m_gfxIp, sizeof(m_gfxIp));
  memcpy(&buildId->hash, &m_hash, sizeof(m_hash));
}

// =====================================================================================================================
// Check if the shader cache creation info is compatible
//
// @param createInfo : Shader cache create info
// @param auxCreateInfo : Shader cache auxiliary info (static fields)
bool ShaderCache::isCompatible(const ShaderCacheCreateInfo *createInfo, const ShaderCacheAuxCreateInfo *auxCreateInfo) {
  // Check hash first
  bool isCompatible = (memcmp(&(auxCreateInfo->hash), &m_hash, sizeof(m_hash)) == 0);

  return isCompatible && m_gfxIp.major == auxCreateInfo->gfxIp.major && m_gfxIp.minor == auxCreateInfo->gfxIp.minor &&
         m_gfxIp.stepping == auxCreateInfo->gfxIp.stepping;
}

#if LLPC_USE_EXPERIMENTAL_SHADER_CACHE_PIPELINES
namespace {
#if LLPC_HAS_LZ4
constexpr bool CompressCachedPipelines = true;
#else
constexpr bool CompressCachedPipelines = false;
#endif // LLPC_HAS_LZ4

constexpr uint32_t NoCompressionMagicNumber = 0x12345678;
constexpr uint32_t Lz4MagicNumber = 0x43345A4C;

#pragma pack(push, 1)
struct PipelineHeader {
  ShaderHeader shaderHeader;
  uint32_t magic;
  uint32_t uncompressedSize;
};
#pragma pack(pop)

MetroHash::Hash ToLlpcHash(const void *pHash) {
  MetroHash::Hash hash = {};
  memcpy(&hash.bytes, pHash, sizeof(hash.bytes));
  return hash;
}
} // namespace

Result ShaderCache::StorePipelineBinary(const void *pHash, size_t pipelineBinarySize, const void *pPipelineBinary) {
  // Create shader hash from pipeline hash.
  MetroHash::Hash hash = ToLlpcHash(pHash);
  CacheEntryHandle hEntry{};
  ShaderEntryState entryState = ShaderCache::findShader(hash, true, &hEntry);
  (void)entryState;

  auto *const index = static_cast<ShaderIndex *>(hEntry);
  auto OnFailure = [index] {
    // Something failed while attempting to add the shader, most likely memory allocation.  There's not much we
    // can do here except give up on adding data.  This means we need to set the entry back to New so if another
    // thread is waiting it will be allowed to continue (it will likely just get to this same point, but at least
    // we won't hang or crash).
    index->state = ShaderEntryState::New;
    index->header.size = 0;
    index->dataBlob = nullptr;
  };

  const void *sourceBuffer = pPipelineBinary;
  std::unique_ptr<char> lz4TempBuffer(nullptr);
  int payloadSize = pipelineBinarySize;

  if (CompressCachedPipelines) {
#if LLPC_HAS_LZ4
    const int compressBound = LZ4_COMPRESSBOUND(pipelineBinarySize);
    lz4TempBuffer.reset(new (std::nothrow) char[compressBound]);
    if (!lz4TempBuffer) {
      OnFailure();
      return Result::ErrorOutOfMemory;
    }
    // Compression is guaranteed to succeed since we pass bound as ouptut size.
    payloadSize = LZ4_compress_default(static_cast<const char *>(pPipelineBinary), lz4TempBuffer.get(),
                                       pipelineBinarySize, compressBound);
    sourceBuffer = lz4TempBuffer.get();
#endif // LLPC_HAS_LZ4
  }

  std::lock_guard<llvm::sys::Mutex> lock(m_lock);
  const int totalSize = payloadSize + sizeof(PipelineHeader);
  index->header.size = totalSize;
  index->dataBlob = getCacheSpace(totalSize);

  if (!index->dataBlob) {
    OnFailure();
    return Result::ErrorOutOfMemory;
  }

  ++m_totalShaders;

  // Construct a pipeline header in place.
  PipelineHeader *const header = new (index->dataBlob) PipelineHeader();
  header->magic = CompressCachedPipelines ? Lz4MagicNumber : NoCompressionMagicNumber;
  header->uncompressedSize = pipelineBinarySize;

  void *const dataBlob = header + 1;
  memcpy(dataBlob, sourceBuffer, payloadSize);

  // Compute a CRC for the serialized data (useful for detecting data corruption), and copy the index's
  // header into the data's header.
  index->header.crc =
      calculateCrc(reinterpret_cast<const uint8_t *>(&header->magic), totalSize - offsetof(PipelineHeader, magic));
  header->shaderHeader = index->header;

  // Mark this entry as ready, we'll wake the waiting threads once we release the lock
  index->state = ShaderEntryState::Ready;

  // Finally, update the file if necessary.
  if (m_onDiskFile.isOpen()) {
    addShaderToFile(index);
  }

  return Result::Success;
}

Result ShaderCache::RetrievePipeline(const void *pHash, size_t *pPipelineBinarySize, const void **ppPipelineBinary) {
  // Create shader hash from pipeline hash
  CacheEntryHandle hEntry{};
  MetroHash::Hash hash = ToLlpcHash(pHash);
  ShaderEntryState entryState = ShaderCache::findShader(hash, false, &hEntry);

  if (entryState != ShaderEntryState::Ready) {
    return Result::ErrorUnavailable;
  }

  std::lock_guard<llvm::sys::Mutex> lock(m_lock);

  const auto *const index = static_cast<ShaderIndex *>(hEntry);
  if (index->header.size - sizeof(PipelineHeader) <= 0) {
    return Result::ErrorUnavailable;
  }

  auto *const header = static_cast<PipelineHeader *>(index->dataBlob);
  if (!header) {
    return Result::ErrorUnknown;
  }
  auto *const dataBlob = reinterpret_cast<const char *>(header + 1);

  if (header->magic != NoCompressionMagicNumber && header->magic != Lz4MagicNumber) {
    llvm_unreachable("Unexpected magic number found.");
    return Result::ErrorUnknown;
  }

  const int uncompressedSize = header->uncompressedSize;
  *pPipelineBinarySize = uncompressedSize;
  // In case the caller only needs the uncompressed size.
  if (!ppPipelineBinary || !(*ppPipelineBinary)) {
    return Result::Success;
  }

  auto *const pPipelineBinary = static_cast<char *>(const_cast<void *>(*ppPipelineBinary));

  // If not compressed.
  if (header->magic == NoCompressionMagicNumber) {
    memcpy(pPipelineBinary, dataBlob, uncompressedSize);
    return Result::Success;
  }

#if LLPC_HAS_LZ4
  // Handle compression.
  const int compressedSize = index->header.size - sizeof(PipelineHeader);
  const int outputSize = LZ4_decompress_safe(dataBlob, pPipelineBinary, compressedSize, uncompressedSize);

  if (outputSize <= 0) {
    llvm_unreachable("Lz4 decompression failed.");
    return Result::ErrorUnknown;
  }

  return Result::Success;
#else
  llvm_unreachable("LLPC built without LZ4 cannot uncompress a compressed pipeline.");
  return Result::ErrorUnavailable;
#endif // LLPC_HAS_LZ4
}
#endif // LLPC_USE_EXPERIMENTAL_SHADER_CACHE_PIPELINES

} // namespace Llpc
