/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilderRecorder.h
 * @brief LLPC header file: declaration of Llpc::BuilderRecorder
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcBuilder.h"
#ifndef NDEBUG
#include "llvm/IR/ValueHandle.h"
#endif

namespace Llpc
{

using namespace llvm;

// Prefix of all recorded calls.
static const char* const BuilderCallPrefix = "llpc.call.";

// LLPC call opcode metadata name.
static const char* const BuilderCallOpcodeMetadataName = "llpc.call.opcode";

// =====================================================================================================================
// A class that caches the metadata kind IDs used by BuilderRecorder and BuilderReplayer.
class BuilderRecorderMetadataKinds
{
public:
    BuilderRecorderMetadataKinds() {}
    BuilderRecorderMetadataKinds(LLVMContext& context);

    uint32_t        m_opcodeMetaKindId;                         // Cached metadata kinds for opcode
};

// =====================================================================================================================
// Builder recorder, to record all Builder calls as intrinsics
// Each call to a Builder method causes the insertion of a call to llpc.call.*, so the Builder calls can be replayed
// later on.
class BuilderRecorder final : public Builder, BuilderRecorderMetadataKinds
{
public:
    // llpc.call.* opcodes
    enum Opcode : uint32_t
    {
        // NOP
        Nop = 0,

        // Descriptor
        LoadBufferDesc,
        IndexDescPtr,
        LoadDescFromPtr,
        GetSamplerDescPtr,
        GetImageDescPtr,
        GetTexelBufferDescPtr,
        GetFmaskDescPtr,
        LoadPushConstantsPtr,
        GetBufferDescLength,

        // Image
        ImageLoad,
        ImageLoadWithFmask,
        ImageStore,
        ImageSample,
        ImageGather,
        ImageAtomic,
        ImageAtomicCompareSwap,
        ImageQueryLevels,
        ImageQuerySamples,
        ImageQuerySize,
        ImageGetLod,

        // Matrix
        TransposeMatrix,

        // Misc.
        Kill,
        ReadClock,

        // Subgroup
        GetSubgroupSize,
        SubgroupElect,
        SubgroupAll,
        SubgroupAny,
        SubgroupAllEqual,
        SubgroupBroadcast,
        SubgroupBroadcastFirst,
        SubgroupBallot,
        SubgroupInverseBallot,
        SubgroupBallotBitExtract,
        SubgroupBallotBitCount,
        SubgroupBallotInclusiveBitCount,
        SubgroupBallotExclusiveBitCount,
        SubgroupBallotFindLsb,
        SubgroupBallotFindMsb,
        SubgroupShuffle,
        SubgroupShuffleXor,
        SubgroupShuffleUp,
        SubgroupShuffleDown,
        SubgroupClusteredReduction,
        SubgroupClusteredInclusive,
        SubgroupClusteredExclusive,
        SubgroupQuadBroadcast,
        SubgroupQuadSwapHorizontal,
        SubgroupQuadSwapVertical,
        SubgroupQuadSwapDiagonal,
        SubgroupSwizzleQuad,
        SubgroupSwizzleMask,
        SubgroupWriteInvocation,
        SubgroupMbcnt,
    };

    // Given an opcode, get the call name (without the "llpc.call." prefix)
    static StringRef GetCallName(Opcode opcode);

    BuilderRecorder(LLVMContext& context, bool wantReplay)
        : Builder(context), BuilderRecorderMetadataKinds(context), m_wantReplay(wantReplay)
    {}

    ~BuilderRecorder() {}

#ifndef NDEBUG
    // Link the individual shader modules into a single pipeline module.
    // This is overridden by BuilderRecorder only on a debug build so it can check that the frontend
    // set shader stage consistently.
    Module* Link(ArrayRef<Module*> modules) override final;
#endif

    // If this is a BuilderRecorder created with wantReplay=true, create the BuilderReplayer pass.
    ModulePass* CreateBuilderReplayer() override;

    // -----------------------------------------------------------------------------------------------------------------
    // Descriptor operations

    Value* CreateLoadBufferDesc(uint32_t      descSet,
                                uint32_t      binding,
                                Value*        pDescIndex,
                                bool          isNonUniform,
                                Type*         pPointeeTy,
                                const Twine&  instName) override final;

    Value* CreateIndexDescPtr(Value*        pDescPtr,
                              Value*        pIndex,
                              bool          isNonUniform,
                              const Twine&  instName) override final;

    Value* CreateLoadDescFromPtr(Value*        pDescPtr,
                                 const Twine&  instName) override final;

    Value* CreateGetSamplerDescPtr(uint32_t      descSet,
                                   uint32_t      binding,
                                   const Twine&  instName) override final;

    Value* CreateGetImageDescPtr(uint32_t      descSet,
                                 uint32_t      binding,
                                 const Twine&  instName) override final;

    Value* CreateGetTexelBufferDescPtr(uint32_t      descSet,
                                       uint32_t      binding,
                                       const Twine&  instName) override final;

    Value* CreateGetFmaskDescPtr(uint32_t      descSet,
                                 uint32_t      binding,
                                 const Twine&  instName) override final;

    Value* CreateLoadPushConstantsPtr(Type*         pPushConstantsTy,
                                      const Twine&  instName) override final;

    Value* CreateGetBufferDescLength(Value* const pBufferDesc,
                                     const Twine& instName = "") override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Image operations

    // Create an image load.
    Value* CreateImageLoad(Type*               pResultTy,
                           uint32_t            dim,
                           uint32_t            flags,
                           Value*              pImageDesc,
                           Value*              pCoord,
                           Value*              pMipLevel,
                           const Twine&        instName = "") override final;

    // Create an image load with F-mask.
    Value* CreateImageLoadWithFmask(Type*                   pResultTy,
                                    uint32_t                dim,
                                    uint32_t                flags,
                                    Value*                  pImageDesc,
                                    Value*                  pFmaskDesc,
                                    Value*                  pCoord,
                                    Value*                  pSampleNum,
                                    const Twine&            instName) override final;

    // Create an image store.
    Value* CreateImageStore(uint32_t         dim,
                            uint32_t         flags,
                            Value*           pImageDesc,
                            Value*           pCoord,
                            Value*           pMipLevel,
                            Value*           pTexel,
                            const Twine&     instName = "") override final;

    // Create an image sample.
    Value* CreateImageSample(Type*             pResultTy,
                             uint32_t          dim,
                             uint32_t          flags,
                             Value*            pImageDesc,
                             Value*            pSamplerDesc,
                             ArrayRef<Value*>  address,
                             const Twine&      instName = "") override final;

    // Create an image gather.
    Value* CreateImageGather(Type*             pResultTy,
                             uint32_t          dim,
                             uint32_t          flags,
                             Value*            pImageDesc,
                             Value*            pSamplerDesc,
                             ArrayRef<Value*>  address,
                             const Twine&      instName = "") override final;

    // Create an image atomic operation other than compare-and-swap.
    Value* CreateImageAtomic(uint32_t         atomicOp,
                             uint32_t         dim,
                             uint32_t         flags,
                             AtomicOrdering   ordering,
                             Value*           pImageDesc,
                             Value*           pCoord,
                             Value*           pInputValue,
                             const Twine&     instName = "") override final;

    // Create an image atomic compare-and-swap.
    Value* CreateImageAtomicCompareSwap(uint32_t        dim,
                                        uint32_t        flags,
                                        AtomicOrdering  ordering,
                                        Value*          pImageDesc,
                                        Value*          pCoord,
                                        Value*          pInputValue,
                                        Value*          pComparatorValue,
                                        const Twine&    instName = "") override final;

    // Create a query of the number of mipmap levels in an image. Returns an i32 value.
    Value* CreateImageQueryLevels(uint32_t                dim,
                                  uint32_t                flags,
                                  Value*                  pImageDesc,
                                  const Twine&            instName = "") override final;

    // Create a query of the number of samples in an image. Returns an i32 value.
    Value* CreateImageQuerySamples(uint32_t                dim,
                                   uint32_t                flags,
                                   Value*                  pImageDesc,
                                   const Twine&            instName = "") override final;

    // Create a query of size of an image at the specified LOD
    Value* CreateImageQuerySize(uint32_t          dim,
                                uint32_t          flags,
                                Value*            pImageDesc,
                                Value*            pLod,
                                const Twine&      instName = "") override final;

    // Create a get of the LOD that would be used for an image sample with the given coordinates
    // and implicit LOD.
    Value* CreateImageGetLod(uint32_t          dim,
                             uint32_t          flags,
                             Value*            pImageDesc,
                             Value*            pSamplerDesc,
                             Value*            pCoord,
                             const Twine&      instName = "") override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Miscellaneous operations

    Instruction* CreateKill(const Twine& instName = "") override final;
    Instruction* CreateReadClock(bool realtime, const Twine& instName = "") override final;

    // Builder methods implemented in BuilderImplMatrix
    Value* CreateTransposeMatrix(Value* const pMatrix, const Twine& instName = "") override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Subgroup operations

    Value* CreateGetSubgroupSize(const Twine& instName) override final;
    Value* CreateSubgroupElect(const Twine& instName) override final;
    Value* CreateSubgroupAll(Value* const pValue,
                             bool         wqm,
                             const Twine& instName) override final;
    Value* CreateSubgroupAny(Value* const pValue,
                             bool         wqm,
                             const Twine& instName) override final;
    Value* CreateSubgroupAllEqual(Value* const pValue,
                                  bool         wqm,
                                  const Twine& instName) override final;
    Value* CreateSubgroupBroadcast(Value* const pValue,
                                   Value* const pIndex,
                                   const Twine& instName) override final;
    Value* CreateSubgroupBroadcastFirst(Value* const pValue,
                                        const Twine& instName) override final;
    Value* CreateSubgroupBallot(Value* const pValue,
                                const Twine& instName) override final;
    Value* CreateSubgroupInverseBallot(Value* const pValue,
                                       const Twine& instName) override final;
    Value* CreateSubgroupBallotBitExtract(Value* const pValue,
                                          Value* const pIndex,
                                          const Twine& instName) override final;
    Value* CreateSubgroupBallotBitCount(Value* const pValue,
                                        const Twine& instName) override final;
    Value* CreateSubgroupBallotInclusiveBitCount(Value* const pValue,
                                                 const Twine& instName) override final;
    Value* CreateSubgroupBallotExclusiveBitCount(Value* const pValue,
                                                 const Twine& instName) override final;
    Value* CreateSubgroupBallotFindLsb(Value* const pValue,
                                       const Twine& instName) override final;
    Value* CreateSubgroupBallotFindMsb(Value* const pValue,
                                       const Twine& instName) override final;
    Value* CreateSubgroupShuffle(Value* const pValue,
                                 Value* const pIndex,
                                 const Twine& instName) override final;
    Value* CreateSubgroupShuffleXor(Value* const pValue,
                                    Value* const pMask,
                                    const Twine& instName) override final;
    Value* CreateSubgroupShuffleUp(Value* const pValue,
                                   Value* const pDelta,
                                   const Twine& instName) override final;
    Value* CreateSubgroupShuffleDown(Value* const pValue,
                                     Value* const pDelta,
                                     const Twine& instName) override final;
    Value* CreateSubgroupClusteredReduction(GroupArithOp groupArithOp,
                                            Value* const pValue,
                                            Value* const pClusterSize,
                                            const Twine& instName) override final;
    Value* CreateSubgroupClusteredInclusive(GroupArithOp groupArithOp,
                                            Value* const pValue,
                                            Value* const pClusterSize,
                                            const Twine& instName) override final;
    Value* CreateSubgroupClusteredExclusive(GroupArithOp groupArithOp,
                                            Value* const pValue,
                                            Value* const pClusterSize,
                                            const Twine& instName) override final;
    Value* CreateSubgroupQuadBroadcast(Value* const pValue,
                                       Value* const pIndex,
                                       const Twine& instName) override final;
    Value* CreateSubgroupQuadSwapHorizontal(Value* const pValue,
                                            const Twine& instName) override final;
    Value* CreateSubgroupQuadSwapVertical(Value* const pValue,
                                          const Twine& instName) override final;
    Value* CreateSubgroupQuadSwapDiagonal(Value* const pValue,
                                          const Twine& instName) override final;
    Value* CreateSubgroupSwizzleQuad(Value* const pValue,
                                     Value* const pOffset,
                                     const Twine& instName) override final;
    Value* CreateSubgroupSwizzleMask(Value* const pValue,
                                     Value* const pMask,
                                     const Twine& instName) override final;
    Value* CreateSubgroupWriteInvocation(Value* const pInputValue,
                                         Value* const pWriteValue,
                                         Value* const pIndex,
                                         const Twine& instName) override final;
    Value* CreateSubgroupMbcnt(Value* const pMask,
                               const Twine& instName ) override final;

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderRecorder)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderRecorder)

    // Record one Builder call
    Instruction* Record(Opcode            opcode,
                        Type*             pReturnTy,
                        ArrayRef<Value*>  args,
                        const Twine&      instName);

#ifndef NDEBUG
    // Check that the frontend is consistently telling us which shader stage a function is in.
    void CheckFuncShaderStage(Function* pFunc, ShaderStage shaderStage);
#endif

    // -----------------------------------------------------------------------------------------------------------------

    bool            m_wantReplay;                             // true to make CreateBuilderReplayer return a replayer
                                                              //   pass
#ifndef NDEBUG
    // Only used in a debug build to ensure SetShaderStage is being used consistently.
    std::vector<std::pair<WeakVH, ShaderStage>> m_funcShaderStageMap;       // Map from function to shader stage
    Function*                                   m_pEnclosingFunc = nullptr; // Last function written with current
                                                                            //   shader stage
#endif
};

} // Llpc
