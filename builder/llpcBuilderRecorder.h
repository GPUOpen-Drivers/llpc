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
    BuilderRecorderMetadataKinds(llvm::LLVMContext& context);

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
        WaterfallLoop,
        WaterfallStoreLoop,
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
    static llvm::StringRef GetCallName(Opcode opcode);

    BuilderRecorder(llvm::LLVMContext& context, bool wantReplay)
        : Builder(context), BuilderRecorderMetadataKinds(context), m_wantReplay(wantReplay)
    {}

    ~BuilderRecorder() {}

#ifndef NDEBUG
    // Link the individual shader modules into a single pipeline module.
    // This is overridden by BuilderRecorder only on a debug build so it can check that the frontend
    // set shader stage consistently.
    llvm::Module* Link(llvm::ArrayRef<llvm::Module*> modules) override final;
#endif

    // If this is a BuilderRecorder created with wantReplay=true, create the BuilderReplayer pass.
    llvm::ModulePass* CreateBuilderReplayer() override;

    // -----------------------------------------------------------------------------------------------------------------
    // Descriptor operations

    llvm::Instruction* CreateWaterfallLoop(llvm::Instruction*       pNonUniformInst,
                                           llvm::ArrayRef<uint32_t> operandIdxs,
                                           const llvm::Twine&       instName) override final;
    llvm::Value* CreateLoadBufferDesc(uint32_t            descSet,
                                      uint32_t            binding,
                                      llvm::Value*        pDescIndex,
                                      bool                isNonUniform,
                                      llvm::Type*         pPointeeTy,
                                      const llvm::Twine&  instName) override final;

    llvm::Value* CreateIndexDescPtr(llvm::Value*        pDescPtr,
                                    llvm::Value*        pIndex,
                                    bool                isNonUniform,
                                    const llvm::Twine&  instName) override final;

    llvm::Value* CreateLoadDescFromPtr(llvm::Value*        pDescPtr,
                                       const llvm::Twine&  instName) override final;

    llvm::Value* CreateGetSamplerDescPtr(uint32_t            descSet,
                                         uint32_t            binding,
                                         const llvm::Twine&  instName) override final;

    llvm::Value* CreateGetImageDescPtr(uint32_t            descSet,
                                       uint32_t            binding,
                                       const llvm::Twine&  instName) override final;

    llvm::Value* CreateGetTexelBufferDescPtr(uint32_t            descSet,
                                             uint32_t            binding,
                                             const llvm::Twine&  instName) override final;

    llvm::Value* CreateGetFmaskDescPtr(uint32_t            descSet,
                                       uint32_t            binding,
                                       const llvm::Twine&  instName) override final;

    llvm::Value* CreateLoadPushConstantsPtr(llvm::Type*         pPushConstantsTy,
                                            const llvm::Twine&  instName) override final;

    llvm::Value* CreateGetBufferDescLength(llvm::Value* const pBufferDesc,
                                           const llvm::Twine& instName = "") override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Image operations

    // Create an image load.
    llvm::Value* CreateImageLoad(llvm::Type*               pResultTy,
                                 uint32_t                  dim,
                                 uint32_t                  flags,
                                 llvm::Value*              pImageDesc,
                                 llvm::Value*              pCoord,
                                 llvm::Value*              pMipLevel,
                                 const llvm::Twine&        instName = "") override final;

    // Create an image load with F-mask.
    llvm::Value* CreateImageLoadWithFmask(llvm::Type*                   pResultTy,
                                          uint32_t                      dim,
                                          uint32_t                      flags,
                                          llvm::Value*                  pImageDesc,
                                          llvm::Value*                  pFmaskDesc,
                                          llvm::Value*                  pCoord,
                                          llvm::Value*                  pSampleNum,
                                          const llvm::Twine&            instName) override final;

    // Create an image store.
    llvm::Value* CreateImageStore(uint32_t               dim,
                                  uint32_t               flags,
                                  llvm::Value*           pImageDesc,
                                  llvm::Value*           pCoord,
                                  llvm::Value*           pMipLevel,
                                  llvm::Value*           pTexel,
                                  const llvm::Twine&     instName = "") override final;

    // Create an image sample.
    llvm::Value* CreateImageSample(llvm::Type*                   pResultTy,
                                   uint32_t                      dim,
                                   uint32_t                      flags,
                                   llvm::Value*                  pImageDesc,
                                   llvm::Value*                  pSamplerDesc,
                                   llvm::ArrayRef<llvm::Value*>  address,
                                   const llvm::Twine&            instName = "") override final;

    // Create an image gather.
    llvm::Value* CreateImageGather(llvm::Type*                   pResultTy,
                                   uint32_t                      dim,
                                   uint32_t                      flags,
                                   llvm::Value*                  pImageDesc,
                                   llvm::Value*                  pSamplerDesc,
                                   llvm::ArrayRef<llvm::Value*>  address,
                                   const llvm::Twine&            instName = "") override final;

    // Create an image atomic operation other than compare-and-swap.
    llvm::Value* CreateImageAtomic(uint32_t               atomicOp,
                                   uint32_t               dim,
                                   uint32_t               flags,
                                   llvm::AtomicOrdering   ordering,
                                   llvm::Value*           pImageDesc,
                                   llvm::Value*           pCoord,
                                   llvm::Value*           pInputValue,
                                   const llvm::Twine&     instName = "") override final;

    // Create an image atomic compare-and-swap.
    llvm::Value* CreateImageAtomicCompareSwap(uint32_t              dim,
                                              uint32_t              flags,
                                              llvm::AtomicOrdering  ordering,
                                              llvm::Value*          pImageDesc,
                                              llvm::Value*          pCoord,
                                              llvm::Value*          pInputValue,
                                              llvm::Value*          pComparatorValue,
                                              const llvm::Twine&    instName = "") override final;

    // Create a query of the number of mipmap levels in an image. Returns an i32 value.
    llvm::Value* CreateImageQueryLevels(uint32_t                      dim,
                                        uint32_t                      flags,
                                        llvm::Value*                  pImageDesc,
                                        const llvm::Twine&            instName = "") override final;

    // Create a query of the number of samples in an image. Returns an i32 value.
    llvm::Value* CreateImageQuerySamples(uint32_t                      dim,
                                         uint32_t                      flags,
                                         llvm::Value*                  pImageDesc,
                                         const llvm::Twine&            instName = "") override final;

    // Create a query of size of an image at the specified LOD
    llvm::Value* CreateImageQuerySize(uint32_t                dim,
                                      uint32_t                flags,
                                      llvm::Value*            pImageDesc,
                                      llvm::Value*            pLod,
                                      const llvm::Twine&      instName = "") override final;

    // Create a get of the LOD that would be used for an image sample with the given coordinates
    // and implicit LOD.
    llvm::Value* CreateImageGetLod(uint32_t                dim,
                                   uint32_t                flags,
                                   llvm::Value*            pImageDesc,
                                   llvm::Value*            pSamplerDesc,
                                   llvm::Value*            pCoord,
                                   const llvm::Twine&      instName = "") override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Miscellaneous operations

    llvm::Instruction* CreateKill(const llvm::Twine& instName = "") override final;
    llvm::Instruction* CreateReadClock(bool realtime, const llvm::Twine& instName = "") override final;

    // Builder methods implemented in BuilderImplMatrix
    llvm::Value* CreateTransposeMatrix(llvm::Value* const pMatrix, const llvm::Twine& instName = "") override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Subgroup operations

    llvm::Value* CreateGetSubgroupSize(const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupElect(const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupAll(llvm::Value* const pValue,
                                   const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupAny(llvm::Value* const pValue,
                                   const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupAllEqual(llvm::Value* const pValue,
                                        const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBroadcast(llvm::Value* const pValue,
                                         llvm::Value* const pIndex,
                                         const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBroadcastFirst(llvm::Value* const pValue,
                                              const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallot(llvm::Value* const pValue,
                                      const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupInverseBallot(llvm::Value* const pValue,
                                             const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallotBitExtract(llvm::Value* const pValue,
                                                llvm::Value* const pIndex,
                                                const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallotBitCount(llvm::Value* const pValue,
                                              const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallotInclusiveBitCount(llvm::Value* const pValue,
                                                       const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallotExclusiveBitCount(llvm::Value* const pValue,
                                                       const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallotFindLsb(llvm::Value* const pValue,
                                             const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallotFindMsb(llvm::Value* const pValue,
                                             const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupShuffle(llvm::Value* const pValue,
                                       llvm::Value* const pIndex,
                                       const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupShuffleXor(llvm::Value* const pValue,
                                          llvm::Value* const pMask,
                                          const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupShuffleUp(llvm::Value* const pValue,
                                         llvm::Value* const pDelta,
                                         const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupShuffleDown(llvm::Value* const pValue,
                                           llvm::Value* const pDelta,
                                           const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupClusteredReduction(GroupArithOp       groupArithOp,
                                                  llvm::Value* const pValue,
                                                  llvm::Value* const pClusterSize,
                                                  const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupClusteredInclusive(GroupArithOp       groupArithOp,
                                                  llvm::Value* const pValue,
                                                  llvm::Value* const pClusterSize,
                                                  const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupClusteredExclusive(GroupArithOp       groupArithOp,
                                                  llvm::Value* const pValue,
                                                  llvm::Value* const pClusterSize,
                                                  const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupQuadBroadcast(llvm::Value* const pValue,
                                             llvm::Value* const pIndex,
                                             const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupQuadSwapHorizontal(llvm::Value* const pValue,
                                                  const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupQuadSwapVertical(llvm::Value* const pValue,
                                                const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupQuadSwapDiagonal(llvm::Value* const pValue,
                                                const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupSwizzleQuad(llvm::Value* const pValue,
                                           llvm::Value* const pOffset,
                                           const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupSwizzleMask(llvm::Value* const pValue,
                                           llvm::Value* const pMask,
                                           const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupWriteInvocation(llvm::Value* const pInputValue,
                                               llvm::Value* const pWriteValue,
                                               llvm::Value* const pIndex,
                                               const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupMbcnt(llvm::Value* const pMask,
                                     const llvm::Twine& instName ) override final;

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderRecorder)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderRecorder)

    // Record one Builder call
    llvm::Instruction* Record(Opcode                        opcode,
                              llvm::Type*                   pReturnTy,
                              llvm::ArrayRef<llvm::Value*>  args,
                              const llvm::Twine&            instName);

#ifndef NDEBUG
    // Check that the frontend is consistently telling us which shader stage a function is in.
    void CheckFuncShaderStage(llvm::Function* pFunc, ShaderStage shaderStage);
#endif

    // -----------------------------------------------------------------------------------------------------------------

    bool            m_wantReplay;                             // true to make CreateBuilderReplayer return a replayer
                                                              //   pass
#ifndef NDEBUG
    // Only used in a debug build to ensure SetShaderStage is being used consistently.
    std::vector<std::pair<llvm::WeakVH, ShaderStage>> m_funcShaderStageMap;       // Map from function to shader stage
    llvm::Function*                                   m_pEnclosingFunc = nullptr; // Last function written with current
                                                                                  //   shader stage
#endif
};

} // Llpc
