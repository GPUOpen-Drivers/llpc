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
 @file llpcShaderCacheManager.h
 @brief LLPC header file: contains declaration of class Llpc::ShaderCacheManager.
 ***********************************************************************************************************************
 */
#pragma once

#include <list>

#include "llpc.h"
#include "llpcDebug.h"
#include "llpcShaderCache.h"

namespace Llpc
{

typedef std::shared_ptr<ShaderCache> ShaderCachePtr;

// =====================================================================================================================
// This class manages shader cache instances for different GFXIP
class ShaderCacheManager
{
public:
    // Constructor
    ShaderCacheManager()
    {

    }

    ~ShaderCacheManager();

    // Get the global ShaderCacheManager object
    static ShaderCacheManager* GetShaderCacheManager()
    {
        if (m_pManager == nullptr)
        {
            m_pManager = new ShaderCacheManager();
        }
        return m_pManager;
    }

    static void Shutdown()
    {
        delete m_pManager;
        m_pManager = nullptr;
    }

    ShaderCachePtr GetShaderCacheObject(const ShaderCacheCreateInfo*    pCreateInfo,
                                        const ShaderCacheAuxCreateInfo* pAuxCreateInfo);

    void ReleaseShaderCacheObject(ShaderCachePtr& shaderCachePtr);

private:
    std::list<ShaderCachePtr>  m_shaderCaches;    // ShaderCache instances for all GFXIP

    static ShaderCacheManager* m_pManager;              // Static manager
};

} // Llpc
