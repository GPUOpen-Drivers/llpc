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
 @file llpcShaderCache.h
 @brief LLPC header file: contains declaration of class Llpc::ShaderCache.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llpcFile.h"
#include "llpcUtil.h"
#include "vkgcMetroHash.h"
#include "llvm/Support/Mutex.h"
#include <condition_variable>
#include <list>
#include <mutex>
#include <unordered_map>

namespace Llpc {

// Header data that is stored with each shader in the cache.
struct ShaderHeader {
  uint64_t key; // Compacted hash key used to identify shaders
  uint64_t crc; // CRC of the shader cache entry, used to detect data corruption.
  size_t size;  // Total size of the shader data in the storage file
};

// Enum defining the states a shader cache entry can be in
enum class ShaderEntryState : unsigned {
  New = 0,         // Initial state
  Compiling = 1,   // An entry was created and must be compiled/populated by the caller
  Ready = 2,       // A matching shader was found and is ready for use
  Unavailable = 3, // Entry doesn't exist in cache
};

// Enumerates modes used in shader cache.
enum ShaderCacheMode {
  ShaderCacheDisable = 0,                  // Disabled
  ShaderCacheEnableRuntime = 1,            // Enabled for runtime use only
  ShaderCacheEnableOnDisk = 2,             // Enabled with on-disk file
  ShaderCacheForceInternalCacheOnDisk = 3, // Force to use internal cache on disk
  ShaderCacheEnableOnDiskReadOnly = 4,     // Only read on-disk file with write-protection
};

// Stores data in the hash map of cached shaders and helps correlated a shader in the hash to a location in the
// cache's linear allocators where the shader is actually stored.
struct ShaderIndex {
  ShaderHeader header;             // Shader header data (key, crc, size)
  volatile ShaderEntryState state; // Shader entry state
  void *dataBlob;                  // Serialized data blob representing a cached RelocatableShader object.
};

// The key in hash map is a 64-bit compacted Shader Hash
typedef std::unordered_map<uint64_t, ShaderIndex *> ShaderIndexMap;

// Specifies auxiliary info necessary to create a shader cache object.
struct ShaderCacheAuxCreateInfo {
  ShaderCacheMode shaderCacheMode; // Mode of shader cache
  GfxIpVersion gfxIp;              // Graphics IP version info
  MetroHash::Hash hash;            // Hash code of compilation options
  const char *cacheFilePath;       // root directory of cache file
  const char *executableName;      // Name of executable file
};

// Length of date field used in BuildUniqueId
static constexpr uint8_t DateLength = 11;

// Length of time field used in BuildUniqueId
static constexpr uint8_t TimeLength = 8;

// Opaque data type representing an ID that uniquely identifies a particular build of LLPC. Such an ID will be stored
// with all serialized pipelines and in the shader cache, and used during load of that data to ensure the version of
// PAL that loads the data is exactly the same as the version that stored it. Currently, this ID is just the date
// and time when LLPC was built.
struct BuildUniqueId {
  uint8_t buildDate[DateLength]; // Build date
  uint8_t buildTime[TimeLength]; // Build time
  GfxIpVersion gfxIp;            // Graphics IP version info
  MetroHash::Hash hash;          // Hash code of compilation options
};

// This the header for the shader cache data when the cache is serialized/written to disk
struct ShaderCacheSerializedHeader {
  size_t headerSize;     // Size of the header structure. This member must always be first
                         // since it is used to validate the serialized data.
  BuildUniqueId buildId; // Build time/date of the PAL version that created the cache file
  size_t shaderCount;    // Number of shaders in the shaderIndex array
  size_t shaderDataEnd;  // Offset to the end of shader data
};

typedef void *CacheEntryHandle;

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 66
/// Defines callback function used to lookup shader cache info in an external cache
typedef Result (*ShaderCacheGetValue)(const void *pClientData, uint64_t hash, void *pValue, size_t *pValueLen);

/// Defines callback function used to store shader cache info in an external cache
typedef Result (*ShaderCacheStoreValue)(const void *pClientData, uint64_t hash, const void *pValue, size_t valueLen);

/// Specifies all information necessary to create a shader cache object.
struct ShaderCacheCreateInfo {
  const void *pInitialData; ///< Pointer to a data buffer whose contents should be used to seed the shader
                            ///  cache. This may be null if no initial data is present.
  size_t initialDataSize;   ///< Size of the initial data buffer, in bytes.

  // NOTE: The following parameters are all optional, and are only used when the IShaderCache will be used in
  // tandem with an external cache which serves as a backing store for the cached shader data.

  // [optional] Private client-opaque data which will be passed to the pClientData parameters of the Get and
  // Store callback functions.
  const void *pClientData;
  ShaderCacheGetValue pfnGetValueFunc;     ///< [Optional] Function to lookup shader cache data in an external cache
  ShaderCacheStoreValue pfnStoreValueFunc; ///< [Optional] Function to store shader cache data in an external cache
};

// =====================================================================================================================
/// Represents the interface of a cache for compiled shaders. The shader cache is designed to be optionally passed in at
/// pipeline create time. The compiled binary for the shaders is stored in the cache object to avoid compiling the same
/// shader multiple times. The shader cache also provides a method to serialize its data to be stored to disk.
class IShaderCache {
public:
  /// Serializes the shader cache data or queries the size required for serialization.
  ///
  /// @param [in]      pBlob  System memory pointer where the serialized data should be placed. This parameter can
  ///                         be null when querying the size of the serialized data. When non-null (and the size is
  ///                         correct/sufficient) then the contents of the shader cache will be placed in this
  ///                         location. The data is an opaque blob which is not intended to be parsed by clients.
  /// @param [in,out]  pSize  Size of the memory pointed to by pBlob. If the value stored in pSize is zero then no
  ///                         data will be copied and instead the size required for serialization will be returned
  ///                         in pSize.
  ///
  /// @returns : Success if data was serialized successfully, Unknown if fail to do serialize.
  virtual Result Serialize(void *pBlob, size_t *pSize) = 0;

  /// Merges the provided source shader caches' content into this shader cache.
  ///
  /// @param [in]  srcCacheCount  Count of source shader caches to be merged.
  /// @param [in]  ppSrcCaches    Pointer to an array of pointers to shader cache objects.
  ///
  /// @returns : Success if data of source shader caches was merged successfully, OutOfMemory if the internal allocator
  ///          memory cannot be allocated.
  virtual Result Merge(unsigned srcCacheCount, const IShaderCache **ppSrcCaches) = 0;

  /// Frees all resources associated with this object.
  virtual void Destroy() = 0;

protected:
  /// @internal Constructor. Prevent use of new operator on this interface.
  IShaderCache() {}

  /// @internal Destructor. Prevent use of delete operator on this interface.
  virtual ~IShaderCache() {}
};
#endif

// =====================================================================================================================
// This class implements a cache for compiled shaders. The shader cache persists in memory at runtime and can be
// serialized to disk by the client/application for persistence between runs.
class ShaderCache : public IShaderCache {
public:
  ShaderCache();
  ~ShaderCache() override;

  LLPC_NODISCARD Result init(const ShaderCacheCreateInfo *createInfo, const ShaderCacheAuxCreateInfo *auxCreateInfo);
  void Destroy() override;

  LLPC_NODISCARD Result Serialize(void *blob, size_t *size) override;

  LLPC_NODISCARD Result Merge(unsigned srcCacheCount, const IShaderCache **ppSrcCaches) override;

  LLPC_NODISCARD ShaderEntryState findShader(MetroHash::Hash hash, bool allocateOnMiss, CacheEntryHandle *phEntry);

  void insertShader(CacheEntryHandle hEntry, const void *blob, size_t size);

  void resetShader(CacheEntryHandle hEntry);

  LLPC_NODISCARD Result retrieveShader(CacheEntryHandle hEntry, const void **ppBlob, size_t *size);

  LLPC_NODISCARD bool isCompatible(const ShaderCacheCreateInfo *createInfo,
                                   const ShaderCacheAuxCreateInfo *auxCreateInfo);

  LLPC_NODISCARD Result waitForEntry(CacheEntryHandle hEntry);

private:
  ShaderCache(const ShaderCache &) = delete;
  ShaderCache &operator=(const ShaderCache &) = delete;

  LLPC_NODISCARD Result buildFileName(const char *executableName, const char *cacheFilePath, GfxIpVersion gfxIp,
                                      bool *cacheFileExists);
  LLPC_NODISCARD Result validateAndLoadHeader(const ShaderCacheSerializedHeader *header, size_t dataSourceSize);
  LLPC_NODISCARD Result loadCacheFromBlob(const void *initialData, size_t initialDataSize);
  LLPC_NODISCARD Result populateIndexMap(void *dataStart, size_t dataSize);
  LLPC_NODISCARD uint64_t calculateCrc(const uint8_t *data, size_t numBytes);

  LLPC_NODISCARD Result loadCacheFromFile();
  void resetCacheFile();
  LLPC_NODISCARD Result addShaderToFile(const ShaderIndex *index);

  void *getCacheSpace(size_t numBytes);

  // Lock cache map
  void lockCacheMap(bool readOnly) { m_lock.lock(); }

  // Unlock cache map
  void unlockCacheMap(bool readOnly) { m_lock.unlock(); }

  // Satisfies `BasicLockable`, so that we can pass it to `std::condition_variable_any::wait`.
  // Does *not* automatically lock/unlock on construction/destruction.
  class CacheMapLock {
  public:
    CacheMapLock(ShaderCache &sc, bool readOnlyLock) : m_sc(sc), m_readOnlyLock(readOnlyLock) {}

    void lock() { m_sc.lockCacheMap(m_readOnlyLock); }
    void unlock() { m_sc.unlockCacheMap(m_readOnlyLock); }

  private:
    ShaderCache &m_sc;
    const bool m_readOnlyLock;
  };

  // Returns a new lock object that satisfies `BasicLockable`. It can be used with `std::condition_variable_any`.
  CacheMapLock makeCacheLock(bool readOnlyLock) { return {*this, readOnlyLock}; }

  bool useExternalCache() { return m_getValueFunc && m_storeValueFunc; }

  void resetRuntimeCache();
  void getBuildTime(BuildUniqueId *buildId);

  llvm::sys::Mutex m_lock; // Read/Write lock for access to the shader cache hash map
  File m_onDiskFile;       // File for on-disk storage of the cache
  bool m_disableCache;     // Whether disable cache completely

  // Map of shader index data which detail the hash, crc, size and CPU memory location for each shader
  // in the cache.
  ShaderIndexMap m_shaderIndexMap;

  // In memory copy of the shaderDataEnd and totalShaders stored in the on-disk file. We keep a copy to avoid having
  //  to do a read/modify/write of the value when adding a new shader.
  size_t m_shaderDataEnd;
  size_t m_totalShaders;

  char m_fileFullPath[PathBufferLen]; // Full path/filename of the shader cache on-disk file

  std::list<std::pair<uint8_t *, size_t>> m_allocationList; // Memory allocated by GetCacheSpace
  unsigned m_serializedSize;                                // Serialized byte size of whole shader cache
  std::condition_variable_any m_conditionVariable; // Condition variable used to wait for compililation to finish
  const void *m_clientData;                        // Client data that will be used by function GetValue and StoreValue
  ShaderCacheGetValue m_getValueFunc;              // GetValue function used to query an external cache for shader data
  ShaderCacheStoreValue m_storeValueFunc;          // StoreValue function used to store shader data in an external cache
  GfxIpVersion m_gfxIp;                            // Graphics IP version info
  MetroHash::Hash m_hash;                          // Hash code of compilation options
};

} // namespace Llpc
