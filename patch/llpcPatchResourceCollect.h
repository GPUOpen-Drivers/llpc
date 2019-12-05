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
#include "llpcPipelineState.h"

namespace Llpc
{

struct NggControl;
struct InOutLocation;
class InOutLocationMapManager;

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
        analysisUsage.addRequired<PipelineStateWrapper>();
        analysisUsage.addRequired<PipelineShaders>();
        analysisUsage.addPreserved<PipelineShaders>();
    }

    virtual bool runOnModule(llvm::Module& module) override;
    virtual void visitCallInst(llvm::CallInst& callInst);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PatchResourceCollect);

    // Determines whether GS on-chip mode is valid for this pipeline, also computes ES-GS/GS-VS ring item size.
    bool CheckGsOnChipValidity();

    // Sets NGG control settings
    void SetNggControl();
    void BuildNggCullingControlRegister(NggControl& nggControl);

    void ProcessShader();

    void ClearInactiveInput();
    void ClearInactiveOutput();

    void MatchGenericInOut();
    void MapBuiltInToGenericInOut();

    void MapGsGenericOutput(GsOutLocInfo outLocInfo);
    void MapGsBuiltInOutput(uint32_t builtInId, uint32_t elemCount);

    bool CanPackInOut() const;
    void PackInOutLocation();
    void ReviseInputImportCalls();
    void ReassembleOutputExportCalls();

    // -----------------------------------------------------------------------------------------------------------------

    PipelineState*                  m_pPipelineState;           // Pipeline state

    std::unordered_set<llvm::CallInst*> m_deadCalls;            // Dead calls

    std::unordered_set<uint32_t>    m_activeInputLocs;          // Locations of active generic inputs
    std::unordered_set<uint32_t>    m_activeInputBuiltIns;      // IDs of active built-in inputs
    std::unordered_set<uint32_t>    m_activeOutputBuiltIns;     // IDs of active built-in outputs

    std::unordered_set<uint32_t>    m_importedOutputLocs;       // Locations of imported generic outputs
    std::unordered_set<uint32_t>    m_importedOutputBuiltIns;   // IDs of imported built-in outputs

    std::vector<llvm::CallInst*>    m_inOutCalls;               // The import or export calls

    bool            m_hasPushConstOp;           // Whether push constant is active
    bool            m_hasDynIndexedInput;       // Whether dynamic indices are used in generic input addressing (valid
                                                // for tessellation shader, fragment shader with input interpolation)
    bool            m_hasDynIndexedOutput;      // Whether dynamic indices are used in generic output addressing (valid
                                                // for tessellation control shader)
    ResourceUsage*  m_pResUsage;                // Pointer to shader resource usage
    std::unique_ptr<InOutLocationMapManager> m_pLocationMapManager; // Pointer to InOutLocationMapManager instance
};

// Represents the location info of input/output
union InOutLocationInfo
{
    struct
    {
        uint16_t location  : 13; // The location
        uint16_t component : 2;  // The component index
        uint16_t half      : 1;  // High half in case of 16-bit attriburtes
    };
    uint16_t u16All;
};

// Represents the compatibility info of input/output
union InOutCompatibilityInfo
{
    struct
    {
        uint16_t halfComponentCount : 9; // The number of components measured in times of 16-bits.
                                         // A single 32-bit component will be halfComponentCount=2
        uint16_t isFlat             : 1; // Flat shading or not
        uint16_t is16Bit            : 1; // Half float or not
        uint16_t isCustom           : 1; // Custom interpolation mode or not
    };
    uint16_t u16All;
};

// Represents the wrapper of input/output locatoin info, along with handlers
struct InOutLocation
{
    uint16_t AsIndex() const { return locationInfo.u16All; }

    bool operator<(const InOutLocation& rhs) const { return (this->AsIndex() < rhs.AsIndex()); }

    InOutLocationInfo locationInfo; // The location info of an input or output
};

// =====================================================================================================================
// Represents the manager of input/output locationMap generation
class InOutLocationMapManager
{
public:
    InOutLocationMapManager() {}

    bool AddSpan(CallInst* pCall);
    void BuildLocationMap();

    bool FindMap(const InOutLocation& originalLocation, const InOutLocation*& pNewLocation);

    struct LocationSpan
    {
        uint16_t GetCompatibilityKey() const { return compatibilityInfo.u16All; }

        uint32_t AsIndex() const { return ((GetCompatibilityKey() << 16) | firstLocation.AsIndex()); }

        bool operator==(const LocationSpan& rhs) const { return (this->AsIndex() == rhs.AsIndex()); }

        bool operator<(const LocationSpan& rhs) const { return (this->AsIndex() < rhs.AsIndex()); }

        InOutLocation firstLocation;
        InOutCompatibilityInfo compatibilityInfo;
    };

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(InOutLocationMapManager);

    bool isCompatible(const LocationSpan& rSpan, const LocationSpan& lSpan) const
    {
        return rSpan.GetCompatibilityKey() == lSpan.GetCompatibilityKey();
    }

    std::vector<LocationSpan> m_locationSpans; // Tracks spans of contiguous components in the generic input space
    std::map<InOutLocation, InOutLocation> m_locationMap; // The map between original location and new location
};

} // Llpc