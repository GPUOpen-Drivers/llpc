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
 * @file  llpcInternal.h
 * @brief LLPC header file: contains LLPC internal-use definitions (including data types and utility functions).
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/llpcBuilderCommon.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace llvm {
class CallInst;
}
namespace lgc {
class Context;
}

namespace llvm {

class ModulePass;
class PassRegistry;
class Timer;

void initializePipelineShadersPass(PassRegistry &);
void initializePipelineStateClearerPass(PassRegistry &);
void initializePipelineStateWrapperPass(PassRegistry &);
void initializeStartStopTimerPass(PassRegistry &);

} // namespace llvm

namespace lgc {

// Invalid value
static const unsigned InvalidValue = ~0u;

// Size of vec4
static const unsigned SizeOfVec4 = sizeof(float) * 4;

// Initialize helper passes
//
// @param passRegistry : Pass registry
inline static void initializeUtilPasses(llvm::PassRegistry &passRegistry) {
  initializePipelineShadersPass(passRegistry);
  initializePipelineStateClearerPass(passRegistry);
  initializePipelineStateWrapperPass(passRegistry);
  initializeStartStopTimerPass(passRegistry);
}

namespace lgcName {
const static char InputCallPrefix[] = "llpc.input.";
const static char InputImportGeneric[] = "llpc.input.import.generic.";
const static char InputImportBuiltIn[] = "llpc.input.import.builtin.";
const static char InputImportInterpolant[] = "llpc.input.import.interpolant.";
const static char OutputCallPrefix[] = "llpc.output.";
const static char OutputImportGeneric[] = "llpc.output.import.generic.";
const static char OutputImportBuiltIn[] = "llpc.output.import.builtin.";
const static char OutputExportGeneric[] = "llpc.output.export.generic.";
const static char OutputExportBuiltIn[] = "llpc.output.export.builtin.";
const static char OutputExportXfb[] = "llpc.output.export.xfb.";
const static char BufferCallPrefix[] = "llpc.buffer.";
const static char BufferAtomic[] = "llpc.buffer.atomic.";
const static char BufferLoad[] = "llpc.buffer.load.";
const static char BufferLoadUniform[] = "llpc.buffer.load.uniform.";
const static char BufferLoadScalarAligned[] = "llpc.buffer.load.scalar.aligned.";
const static char BufferStore[] = "llpc.buffer.store.";
const static char BufferStoreScalarAligned[] = "llpc.buffer.store.scalar.aligned.";
const static char InlineConstLoadUniform[] = "llpc.inlineconst.load.uniform.";
const static char InlineConstLoad[] = "llpc.inlineconst.load.";
const static char PushConstLoad[] = "llpc.pushconst.load.";
const static char TfBufferStore[] = "llpc.tfbuffer.store.f32";
const static char StreamOutBufferStore[] = "llpc.streamoutbuffer.store";

const static char DescriptorCallPrefix[] = "llpc.descriptor.";
const static char DescriptorIndex[] = "llpc.descriptor.index";
const static char DescriptorLoadFromPtr[] = "llpc.descriptor.load.from.ptr";
const static char DescriptorLoadPrefix[] = "llpc.descriptor.load.";
const static char DescriptorGetPtrPrefix[] = "llpc.descriptor.get.";
const static char DescriptorGetResourcePtr[] = "llpc.descriptor.get.resource.ptr";
const static char DescriptorGetSamplerPtr[] = "llpc.descriptor.get.sampler.ptr";
const static char DescriptorGetFmaskPtr[] = "llpc.descriptor.get.fmask.ptr";
const static char DescriptorLoadBuffer[] = "llpc.descriptor.load.buffer";
const static char DescriptorGetTexelBufferPtr[] = "llpc.descriptor.get.texelbuffer.ptr";
const static char DescriptorLoadSpillTable[] = "llpc.descriptor.load.spilltable";

const static char LaterCallPrefix[] = "llpc.late.";
const static char LateLaunderFatPointer[] = "llpc.late.launder.fat.pointer";
const static char LateBufferLength[] = "llpc.late.buffer.desc.length";

// Names of entry-points for merged shader
const static char EsGsEntryPoint[] = "llpc.shader.ESGS.main";
const static char LsHsEntryPoint[] = "llpc.shader.LSHS.main";

const static char NggEsEntryPoint[] = "llpc.ngg.ES.main";
const static char NggEsEntryVariant[] = "llpc.ngg.ES.variant";
const static char NggEsEntryVariantPos[] = "llpc.ngg.ES.variant.pos";
const static char NggEsEntryVariantParam[] = "llpc.ngg.ES.variant.param";

const static char NggGsEntryPoint[] = "llpc.ngg.GS.main";
const static char NggGsEntryVariant[] = "llpc.ngg.GS.variant";
const static char NggGsOutputExport[] = "llpc.ngg.GS.output.export.";
const static char NggGsOutputImport[] = "llpc.ngg.GS.output.import.";
const static char NggGsEmit[] = "llpc.ngg.GS.emit";
const static char NggGsCut[] = "llpc.ngg.GS.cut";

const static char NggCopyShaderEntryPoint[] = "llpc.ngg.COPY.main";
const static char NggPrimShaderEntryPoint[] = "llpc.shader.PRIM.main";

const static char NggCullingFetchReg[] = "llpc.ngg.culling.fetchreg";
const static char NggCullingBackface[] = "llpc.ngg.culling.backface";
const static char NggCullingFrustum[] = "llpc.ngg.culling.frustum";
const static char NggCullingBoxFilter[] = "llpc.ngg.culling.boxfilter";
const static char NggCullingSphere[] = "llpc.ngg.culling.sphere";
const static char NggCullingSmallPrimFilter[] = "llpc.ngg.culling.smallprimfilter";
const static char NggCullingCullDistance[] = "llpc.ngg.culling.culldistance";

const static char EntryPointPrefix[] = "llpc.shader.";
const static char CopyShaderEntryPoint[] = "llpc.shader.COPY.main";
const static char NullFsEntryPoint[] = "llpc.shader.FS.null.main";

const static char ShaderStageMetadata[] = "llpc.shaderstage";
} // namespace lgcName

// Well-known metadata names
const static char MetaNameUniform[] = "amdgpu.uniform";

// Maximum count of input/output locations that a shader stage (except fragment shader outputs) is allowed to specify
static const unsigned MaxInOutLocCount = 32;

// Maximum array size of gl_ClipDistance[] and gl_CullDistance[]
static const unsigned MaxClipCullDistanceCount = 8;

// Maximum transform feedback buffers
static const unsigned MaxTransformFeedbackBuffers = 4;

// Maximum GS output vertex streams
static const unsigned MaxGsStreams = 4;
static_assert(MaxGsStreams == MaxTransformFeedbackBuffers, "Unexpected value!");

// Internal resource table's virtual descriptor sets
static const unsigned InternalResourceTable = 0x10000000;
static const unsigned InternalPerShaderTable = 0x10000001;

// Descriptor offset reloc magic number
static const unsigned DescRelocMagic = 0xA5A5A500;
static const unsigned DescRelocMagicMask = 0xFFFFFF00;
static const unsigned DescSetMask = 0x000000FF;

// Translates shader stage to corresponding stage mask.
static inline unsigned shaderStageToMask(ShaderStage stage) {
  return 1U << static_cast<unsigned>(stage);
}

// Emits a LLVM function call (inserted before the specified instruction), builds it automically based on return type
// and its parameters.
llvm::CallInst *emitCall(llvm::StringRef funcName, llvm::Type *retTy, llvm::ArrayRef<llvm::Value *> args,
                         llvm::ArrayRef<llvm::Attribute::AttrKind> attribs, llvm::Instruction *insertPos);

// Emits a LLVM function call (inserted at the end of the specified basic block), builds it automically based on return
// type and its parameters.
llvm::CallInst *emitCall(llvm::StringRef funcName, llvm::Type *retTy, llvm::ArrayRef<llvm::Value *> args,
                         llvm::ArrayRef<llvm::Attribute::AttrKind> attribs, llvm::BasicBlock *insertAtEnd);

// Adds LLVM-style type mangling suffix for the specified return type and args to the name.
void addTypeMangling(llvm::Type *returnTy, llvm::ArrayRef<llvm::Value *> args, std::string &name);

// Gets LLVM-style name for type.
void getTypeName(llvm::Type *ty, llvm::raw_ostream &nameStream);
std::string getTypeName(llvm::Type *ty);

// Gets the shader stage from the specified LLVM function.
ShaderStage getShaderStageFromFunction(const llvm::Function *func);

// Gets the shader stage from the specified calling convention.
ShaderStage getShaderStageFromCallingConv(unsigned stageMask, llvm::CallingConv::ID callConv);

// Gets the argument from the specified function according to the argument index.
llvm::Value *getFunctionArgument(llvm::Function *func, unsigned idx, const llvm::Twine &name = "");

// Checks if one type can be bitcasted to the other (type1 -> type2).
bool canBitCast(const llvm::Type *ty1, const llvm::Type *ty2);

// Checks if the specified value actually represents a don't-care value (0xFFFFFFFF).
bool isDontCareValue(llvm::Value *value);

} // namespace lgc
