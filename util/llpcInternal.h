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
 * @file  llpcInternal.h
 * @brief LLPC header file: contains LLPC internal-use definitions (including data types and utility functions).
 ***********************************************************************************************************************
 */
#pragma once

#include <unordered_set>

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include "spirvExt.h"
#include "llpc.h"
#include "llpcUtil.h"

namespace llvm { class CallInst; }
namespace Llpc { class Context; }

// Internally defined SPIR-V semantics (internal-use)
namespace spv
{

// Built-ins for fragment input interpolation (I/J)
static const BuiltIn BuiltInInterpPerspSample    = static_cast<BuiltIn>(0x10000000);
static const BuiltIn BuiltInInterpPerspCenter    = static_cast<BuiltIn>(0x10000001);
static const BuiltIn BuiltInInterpPerspCentroid  = static_cast<BuiltIn>(0x10000002);
static const BuiltIn BuiltInInterpPullMode       = static_cast<BuiltIn>(0x10000003);
static const BuiltIn BuiltInInterpLinearSample   = static_cast<BuiltIn>(0x10000004);
static const BuiltIn BuiltInInterpLinearCenter   = static_cast<BuiltIn>(0x10000005);
static const BuiltIn BuiltInInterpLinearCentroid = static_cast<BuiltIn>(0x10000006);

// Built-ins for sample position emulation
static const BuiltIn BuiltInSamplePosOffset      = static_cast<BuiltIn>(0x10000007);
static const BuiltIn BuiltInNumSamples           = static_cast<BuiltIn>(0x10000008);
static const BuiltIn BuiltInSamplePatternIdx     = static_cast<BuiltIn>(0x10000009);
static const BuiltIn BuiltInWaveId               = static_cast<BuiltIn>(0x1000000A);

// Execution model: copy shader
static const ExecutionModel ExecutionModelCopyShader = static_cast<ExecutionModel>(1024);

} // spv

namespace llvm
{

class PassRegistry;
class Timer;

void initializePassDeadFuncRemovePass(PassRegistry&);
void initializePassExternalLibLinkPass(PassRegistry&);
void initializePassLoopInfoCollectPass(PassRegistry&);
void initializePipelineShadersPass(PassRegistry&);
void initializeStartStopTimerPass(PassRegistry&);

} // llvm

namespace Llpc
{

llvm::ModulePass* CreatePassDeadFuncRemove();
llvm::ModulePass* CreatePassExternalLibLink(bool nativeOnly);
llvm::ModulePass* CreateStartStopTimer(llvm::Timer* pTimer, bool starting);

// Initialize helper passes
inline static void InitializeUtilPasses(
    llvm::PassRegistry& passRegistry)   // Pass registry
{
    initializePassDeadFuncRemovePass(passRegistry);
    initializePassExternalLibLinkPass(passRegistry);
    initializePassLoopInfoCollectPass(passRegistry);
    initializePipelineShadersPass(passRegistry);
}

namespace LlpcName
{
    const static char InputCallPrefix[]               = "llpc.input.";
    const static char InputImportGeneric[]            = "llpc.input.import.generic.";
    const static char InputImportBuiltIn[]            = "llpc.input.import.builtin.";
    const static char InputImportInterpolant[]        = "llpc.input.import.interpolant.";
    const static char OutputCallPrefix[]              = "llpc.output.";
    const static char OutputImportGeneric[]           = "llpc.output.import.generic.";
    const static char OutputImportBuiltIn[]           = "llpc.output.import.builtin.";
    const static char OutputExportGeneric[]           = "llpc.output.export.generic.";
    const static char OutputExportBuiltIn[]           = "llpc.output.export.builtin.";
    const static char OutputExportXfb[]               = "llpc.output.export.xfb.";
    const static char InputInterpEval[]               = "llpc.input.interpolate.evalij.";
    const static char BufferCallPrefix[]              = "llpc.buffer.";
    const static char BufferAtomic[]                  = "llpc.buffer.atomic.";
    const static char BufferLoad[]                    = "llpc.buffer.load.";
    const static char BufferLoadUniform[]             = "llpc.buffer.load.uniform.";
    const static char BufferLoadScalarAligned[]       = "llpc.buffer.load.scalar.aligned.";
    const static char BufferStore[]                   = "llpc.buffer.store.";
    const static char BufferStoreScalarAligned[]      = "llpc.buffer.store.scalar.aligned.";
    const static char BufferArrayLength[]             = "llpc.buffer.arraylength";
    const static char InlineConstLoadUniform[]        = "llpc.inlineconst.load.uniform.";
    const static char InlineConstLoad[]               = "llpc.inlineconst.load.";
    const static char PushConstLoad[]                 = "llpc.pushconst.load.";
    const static char TfBufferStore[]                 = "llpc.tfbuffer.store.f32";
    const static char StreamOutBufferStore[]          = "llpc.streamoutbuffer.store";

    const static char DescriptorCallPrefix[]          = "llpc.descriptor.";
    const static char DescriptorLoadPrefix[]          = "llpc.descriptor.load.";
    const static char DescriptorLoadResource[]        = "llpc.descriptor.load.resource";
    const static char DescriptorLoadSampler[]         = "llpc.descriptor.load.sampler";
    const static char DescriptorLoadFmask[]           = "llpc.descriptor.load.fmask";
    const static char DescriptorLoadBuffer[]          = "llpc.descriptor.load.buffer";
    const static char DescriptorLoadAddress[]         = "llpc.descriptor.load.address";
    const static char DescriptorLoadTexelBuffer[]     = "llpc.descriptor.load.texelbuffer";
    const static char DescriptorLoadSpillTable[]      = "llpc.descriptor.load.spilltable";

    const static char ImageCallPrefix[]               = "llpc.image";

    const static char GlobalProxyPrefix[]             = "__llpc_global_proxy_";
    const static char InputProxyPrefix[]              = "__llpc_input_proxy_";
    const static char OutputProxyPrefix[]             = "__llpc_output_proxy_";

    // Names of entry-points for merged shader
    const static char EsGsEntryPoint[]                = "llpc.shader.ESGS.main";
    const static char LsHsEntryPoint[]                = "llpc.shader.LSHS.main";

    const static char EntryPointPrefix[]              = "llpc.shader.";
    const static char CopyShaderEntryPoint[]          = "llpc.shader.COPY.main";
    const static char NullFsEntryPoint[]              = "llpc.shader.FS.null.main";
} // LlpcName

// Maximum count of input/output locations that a shader stage (except fragment shader outputs) is allowed to specify
static const uint32_t MaxInOutLocCount = 32;

// Maximum array size of gl_ClipDistance[] and gl_CullDistance[]
static const uint32_t MaxClipCullDistanceCount = 8;

// Maximum transform feedback buffers
static const uint32_t MaxTransformFeedbackBuffers = 4;

// Maximum GS output vertex streams
static const uint32_t MaxGsStreams = 4;
static_assert(MaxGsStreams == MaxTransformFeedbackBuffers, "Unexpected value!");

// Internal resource table's virtual descriptor sets
static const uint32_t InternalResourceTable  = 0x10000000;
static const uint32_t InternalPerShaderTable = 0x10000001;

// Internal resource table's virtual bindings
static const uint32_t SI_DRV_TABLE_SCRATCH_GFX_SRD_OFFS = 0;
static const uint32_t SI_DRV_TABLE_SCRATCH_CS_SRD_OFFS  = 1;
static const uint32_t SI_DRV_TABLE_ES_RING_OUT_OFFS     = 2;
static const uint32_t SI_DRV_TABLE_GS_RING_IN_OFFS      = 3;
static const uint32_t SI_DRV_TABLE_GS_RING_OUT0_OFFS    = 4;
static const uint32_t SI_DRV_TABLE_GS_RING_OUT1_OFFS    = 5;
static const uint32_t SI_DRV_TABLE_GS_RING_OUT2_OFFS    = 6;
static const uint32_t SI_DRV_TABLE_GS_RING_OUT3_OFFS    = 7;
static const uint32_t SI_DRV_TABLE_VS_RING_IN_OFFS      = 8;
static const uint32_t SI_DRV_TABLE_TF_BUFFER_OFFS       = 9;
static const uint32_t SI_DRV_TABLE_HS_BUFFER0_OFFS      = 10;
static const uint32_t SI_DRV_TABLE_OFF_CHIP_PARAM_CACHE = 11;
static const uint32_t SI_DRV_TABLE_SAMPLEPOS            = 12;

static const uint32_t SI_STREAMOUT_TABLE_OFFS           = 0;

// No attribute
static const std::vector<llvm::Attribute::AttrKind>   NoAttrib;

// Gets the entry point (valid for AMD GPU) of a LLVM module.
llvm::Function* GetEntryPoint(llvm::Module* pModule);

// Emits a LLVM function call (inserted before the specified instruction), builds it automically based on return type
// and its parameters.
llvm::CallInst* EmitCall(llvm::Module*                             pModule,
                         llvm::StringRef                           funcName,
                         llvm::Type*                               pRetTy,
                         llvm::ArrayRef<llvm::Value *>             args,
                         llvm::ArrayRef<llvm::Attribute::AttrKind> attribs,
                         llvm::Instruction*                        pInsertPos);

// Emits a LLVM function call (inserted at the end of the specified basic block), builds it automically based on return
// type and its parameters.
llvm::CallInst* EmitCall(llvm::Module*                             pModule,
                         llvm::StringRef                           funcName,
                         llvm::Type*                               pRetTy,
                         llvm::ArrayRef<llvm::Value *>             args,
                         llvm::ArrayRef<llvm::Attribute::AttrKind> attribs,
                         llvm::BasicBlock*                         pInsertAtEnd);

// Adds LLVM-style type mangling suffix for the specified return type and args to the name.
void AddTypeMangling(llvm::Type* pReturnTy, llvm::ArrayRef<llvm::Value*> args, std::string& name);

// Gets LLVM-style name for scalar or vector type.
void GetTypeNameForScalarOrVector(llvm::Type* pTy, llvm::raw_ostream& nameStream);
std::string GetTypeNameForScalarOrVector(llvm::Type* pTy);

// Gets the shader stage from the specified LLVM module.
ShaderStage GetShaderStageFromModule(llvm::Module* pModule);

// Gets the shader stage from the specified LLVM function.
ShaderStage GetShaderStageFromFunction(llvm::Function* pFunc);

// Gets the shader stage from the specified calling convention.
ShaderStage GetShaderStageFromCallingConv(uint32_t stageMask, llvm::CallingConv::ID callConv);

// Gets the argument from the specified function according to the argument index.
llvm::Value* GetFunctionArgument(llvm::Function* pFunc, uint32_t idx);

// Checks if one type can be bitcasted to the other (type1 -> type2).
bool CanBitCast(const llvm::Type* pTy1, const llvm::Type* pTy2);

// Checks whether input binary data is SPIR-V binary
bool IsSpirvBinary(const BinaryData*  pShaderBin);

// Checks whether input binary data is LLVM bitcode
bool IsLlvmBitcode(const BinaryData*  pShaderBin);

// Gets the shader stage mask from the SPIR-V binary according to the specified entry-point.
uint32_t GetStageMaskFromSpirvBinary(const BinaryData* pSpvBin, const char* pEntryName);

// Verifies if the SPIR-V binary is valid and is supported
Result VerifySpirvBinary(const BinaryData* pSpvBin);

// Checks if the specified value actually represents a don't-care value (0xFFFFFFFF).
bool IsDontCareValue(llvm::Value* pValue);

// Translates an integer to 32-bit integer regardless of its initial bit width.
llvm::Value* ToInt32Value(Llpc::Context* pContext, llvm::Value* pValue, llvm::Instruction* pInsertPos);

// Checks whether the specified value is a non-uniform value.
bool IsNonUniformValue(llvm::Value* pValue, std::unordered_set<llvm::Value*>& checkedValues);

// Checks whether the input data is actually a ELF binary
bool IsElfBinary(const void* pData, size_t dataSize);

// Checks whether the output data is actually ISA assembler text
bool IsIsaText(const void* pData, size_t dataSize);

// =====================================================================================================================
// Represents the special header of SPIR-V token stream (the first DWORD).
struct SpirvHeader
{
    uint32_t    magicNumber;        // Magic number of SPIR-V module
    uint32_t    spvVersion;         // SPIR-V version number
    uint32_t    genMagicNumber;     // Generator's magic number
    uint32_t    idBound;            // Upbound (X) of all IDs used in SPIR-V (0 < ID < X)
    uint32_t    reserved;           // Reserved word
};

} // Llpc
