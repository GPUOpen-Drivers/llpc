/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPatchResourceCollect.h
 * @brief LLPC header file: contains declaration of class Llpc::PatchResourceCollect.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/InstVisitor.h"

#include <unordered_set>
#include "llpcPatch.h"
#include "llpcPipelineShaders.h"

namespace Llpc
{

typedef std::map<uint32_t, uint32_t>::iterator LocMapIterator; // Iterator of the map from uint32_t to uint32_t

// =====================================================================================================================
// Represents the pass of LLVM patching opertions for resource collecting
class PatchResourceCollect:
    public Patch,
    public llvm::InstVisitor<PatchResourceCollect>
{
public:
    PatchResourceCollect();

    void getAnalysisUsage(llvm::AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineShaders>();
        analysisUsage.addPreserved<PipelineShaders>();
    }

    virtual bool runOnModule(llvm::Module& module) override;
    virtual void visitCallInst(llvm::CallInst& callInst);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PatchResourceCollect);

    // Enumerates bit width
    enum BitWidth: uint32_t
    {
        BitWidth8,
        BitWidth16,
        BitWidth32,
        BitWidth64,
    };

    // The infos used to partition ordered call indices into a pack group
    // The non-64-bit and 64-bit are packed respectively in a pack group
    struct PackGroup
    {
        uint32_t scalarizedCallCount; // The scalarized call count in a pack group
        bool     is64Bit;             // Wether is 64-bits
    };

    // Represents pack info of input or output
    union InOutPackInfo
    {
        struct
        {
            uint32_t compIdx              : 2;  // Component index of output vector
            uint32_t location             : 16; // Location of the output
            uint32_t channelMask          : 4;  // The channel mask of packed VS output
        } vs;

        struct
        {
            uint32_t compIdx              : 2;  // Component index of input vector
            uint32_t location             : 16; // Location of the input
            uint32_t bitWidth             : 2;  // Correspond to enumerants of BitWidth
            uint32_t interpMode           : 2;  // Interpolation mode: 0-Smooth, 1-Flat, 2-NoPersp, 3-Custom
            uint32_t interpLoc            : 3;  // Interpolation location: 0-Unknown, 1-Center, 2-Centroid,
                                                // 3-Smaple, 4-Custom
            uint32_t interpolantCompCount : 3;  // The component count of interpolant
            uint32_t isInterpolant        : 1;  // Whether it is interpolant input
        } fs;

        uint32_t u32All;
    };

    void ProcessShader();

    void ClearInactiveInput();
    void ClearInactiveOutput();

    void MatchGenericInOut();
    void MapBuiltInToGenericInOut();

    void ReviseTessExecutionMode();
    void MapGsGenericOutput(GsOutLocInfo outLocInfo);
    void MapGsBuiltInOutput(uint32_t builtInId, uint32_t elemCount);

    bool CheckValidityForInOutPack() const;
    void ProcessCallForInOutPack(CallInst* pCall);
    void MatchGenericInOutWithPack();
    void MapBuiltInToGenericInOutWithPack();
    void PackGenericInOut();
    void PrepareForInOutPack(SmallVector<PackGroup, 4>&         packGroups,
                             std::vector<uint32_t>&             orderedOutputCallIndices);
    void CreatePackedGenericInOut(const SmallVector<PackGroup, 4>& packGroups,
                                  const std::vector<uint32_t>&     orderedOutputCallIndices,
                                  uint32_t&                        inputPackLoc,
                                  uint32_t&                        outputPackLoc);
    void CreateInterpolateInOut(uint32_t& inputPackLoc,
                                uint32_t& outputPackLoc);
    LocMapIterator CreatePackedGenericInput(bool            is64Bit,
                                            uint32_t        inputCallCount,
                                            uint32_t&       locId,
                                            LocMapIterator  locMapIt);
    void CreatePackedGenericOutput(const std::vector<uint32_t>& orderedOutputCallIndices,
                                   bool                         is64Bit,
                                   uint32_t                     outputCallCount,
                                   uint32_t&                    callIndexPos,
                                   uint32_t&                    packLoc);

    // -----------------------------------------------------------------------------------------------------------------

    std::unordered_set<llvm::CallInst*> m_deadCalls;            // Dead calls

    std::unordered_set<uint32_t>    m_activeInputLocs;          // Locations of active generic inputs
    std::unordered_set<uint32_t>    m_activeInputBuiltIns;      // IDs of active built-in inputs
    std::unordered_set<uint32_t>    m_activeOutputBuiltIns;     // IDs of active built-in outputs

    std::unordered_set<uint32_t>    m_importedOutputLocs;       // Locations of imported generic outputs
    std::unordered_set<uint32_t>    m_importedOutputBuiltIns;   // IDs of imported built-in outputs

    std::vector<llvm::CallInst*>    m_importedCalls;            // Imported calls
    std::vector<llvm::CallInst*>    m_exportedCalls;            // Exported calls

    bool            m_hasPushConstOp;           // Whether push constant is active
    bool            m_hasDynIndexedInput;       // Whether dynamic indices are used in generic input addressing (valid
                                                // for tessellation shader, fragment shader with input interpolation)
    bool            m_hasDynIndexedOutput;      // Whether dynamic indices are used in generic output addressing (valid
                                                // for tessellation control shader)
    bool            m_hasInterpolantInput;      // Whether interpolant funtions are used
    ResourceUsage*  m_pResUsage;                // Pointer to shader resource usage
};

} // Llpc
