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
 @file llpcShaderCacheManager.h
 @brief LLPC header file: contains declaration of class Llpc::ShaderCacheManager.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llpcShaderCache.h"
#include <list>

namespace Llpc {

typedef std::shared_ptr<ShaderCache> ShaderCachePtr;

// =====================================================================================================================
// This class manages shader cache instances for different GFXIP
class ShaderCacheManager {
public:
  // Constructor
  ShaderCacheManager() {}

  ~ShaderCacheManager();

  // Get the global ShaderCacheManager object
  static ShaderCacheManager *getShaderCacheManager() {
    if (!m_manager)
      m_manager = new ShaderCacheManager();
    return m_manager;
  }

  static void shutdown() {
    delete m_manager;
    m_manager = nullptr;
  }

  ShaderCachePtr getShaderCacheObject(const ShaderCacheCreateInfo *createInfo,
                                      const ShaderCacheAuxCreateInfo *auxCreateInfo);

  void releaseShaderCacheObject(ShaderCachePtr &shaderCachePtr);

private:
  std::list<ShaderCachePtr> m_shaderCaches; // ShaderCache instances for all GFXIP

  static ShaderCacheManager *m_manager; // Static manager
};

} // namespace Llpc
