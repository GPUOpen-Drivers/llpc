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
 * @file  llpcSpirvLowerResourceCollect.h
 * @brief LLPC header file: contains declaration of class Llpc::SpirvLowerResourceCollect.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/InstVisitor.h"

#include "SPIRVInternal.h"
#include "llpc.h"
#include "llpcSpirvLower.h"

namespace Llpc
{

struct DescriptorBinding;
struct ResourceUsage;

// Compact ResourceNodeData into an uint64 key
union ResourceNodeDataKey
{
    struct
    {
        uint64_t reserved   :   16;
        uint64_t arraySize  :   16;  // Resource array size
        uint64_t binding    :   16;  // Resource binding
        uint64_t set        :   16;  // Resource set
    } value;
    uint64_t u64All;
};

struct ResNodeDataSortingComparer
{
    bool operator()(const ResourceNodeDataKey& set1, const ResourceNodeDataKey& set2) const
    {
        return set1.u64All < set2.u64All;
    }
};

// =====================================================================================================================
// Represents the pass of SPIR-V lowering opertions for resource collecting.
class SpirvLowerResourceCollect:
    public SpirvLower,
    public llvm::InstVisitor<SpirvLowerResourceCollect>
{
public:
    SpirvLowerResourceCollect(bool collectDetailUsage = false);
    auto& GetResourceNodeDatas()
    {
        return m_resNodeDatas;
    }
    auto GetPushConstSize() {return m_pushConstSize; }
    auto& GetFsOutInfos() { return m_fsOutInfos; }
    bool DetailUsageValid() { return m_detailUsageValid; }

    virtual bool runOnModule(llvm::Module& module);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(SpirvLowerResourceCollect);

    uint32_t GetFlattenArrayElementCount(const llvm::Type* pTy) const;
    const llvm::Type* GetFlattenArrayElementType(const llvm::Type* pTy) const;

    void CollectResourceNodeData(const GlobalVariable* pGlobal);

    // -----------------------------------------------------------------------------------------------------------------

    ResourceUsage*  m_pResUsage;    // Resource usage of the shader stage

    bool m_collectDetailUsage;      // If enabled, collect detailed usages of resource node datas and FS output infos
    std::map<ResourceNodeDataKey, ResourceMappingNodeType, ResNodeDataSortingComparer> m_resNodeDatas; // Resource
                                                                                                       // node data
    uint32_t m_pushConstSize;        // Push constant size in byte
    std::vector<FsOutInfo> m_fsOutInfos;   // FS output info array
    bool m_detailUsageValid; // Indicate whether detailed usages (resource node datas
                             // or fragment shader output infos) are valid
};

} // Llpc
