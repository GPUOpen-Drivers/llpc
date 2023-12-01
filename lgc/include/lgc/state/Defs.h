/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  state/Defs.h
 * @brief LLPC header file: LGC internal definitions
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/BuiltIns.h"

namespace lgc {

// Names used for calls added to IR to represent various actions internally.
namespace lgcName {
const static char InternalCallPrefix[] = "lgc.";
const static char InputCallPrefix[] = "lgc.input.";
const static char InputImportBuiltIn[] = "lgc.input.import.builtin.";
const static char OutputCallPrefix[] = "lgc.output.";
const static char OutputImportBuiltIn[] = "lgc.output.import.builtin.";
const static char OutputExportGeneric[] = "lgc.output.export.generic.";
const static char OutputExportBuiltIn[] = "lgc.output.export.builtin.";
const static char OutputExportXfb[] = "lgc.output.export.xfb.";
const static char ReconfigureLocalInvocationId[] = "lgc.reconfigure.local.invocation.id";
const static char SwizzleWorkgroupId[] = "lgc.swizzle.workgroup.id";

// Get special user data input. Arg is UserDataMapping enum value. The optional second arg causes the 32-bit
// value to be extended to 64-bit pointer and specifies the value to use for the high half, or
// ShadowDescriptorTable::Disable to use PC.
const static char SpecialUserData[] = "lgc.special.user.data.";
// Get shader input. Arg is ShaderInput enum value.
const static char ShaderInput[] = "lgc.shader.input.";

// Names of global variables
const static char ImmutableSamplerGlobal[] = "lgc.immutable.sampler";
const static char ImmutableConvertingSamplerGlobal[] = "lgc.immutable.converting.sampler";

// Names of entry-points for merged shader
const static char EsGsEntryPoint[] = "lgc.shader.ESGS.main";
const static char LsHsEntryPoint[] = "lgc.shader.LSHS.main";

const static char NggAttribExport[] = "lgc.ngg.attrib.export";
const static char NggXfbExport[] = "lgc.ngg.xfb.export.";
const static char NggWriteGsOutput[] = "lgc.ngg.write.GS.output.";
const static char NggReadGsOutput[] = "lgc.ngg.read.GS.output.";
const static char NggPrimShaderEntryPoint[] = "lgc.shader.PRIM.main";

const static char EntryPointPrefix[] = "lgc.shader.";
const static char CopyShaderEntryPoint[] = "lgc.shader.COPY.main";
const static char NullFsEntryPoint[] = "lgc.shader.FS.null.main";
const static char TcsPassthroughEntryPoint[] = "lgc.shader.TCS.passthrough.main";

const static char CooperativeMatrix[] = "lgc.cooperative.matrix";
const static char CooperativeMatrixLength[] = "lgc.cooperative.matrix.length";
const static char CooperativeMatrixExtract[] = "lgc.cooperative.matrix.extract";
const static char CooperativeMatrixInsert[] = "lgc.cooperative.matrix.insert";
const static char CooperativeMatrixLoad[] = "lgc.cooperative.matrix.load";
const static char CooperativeMatrixStore[] = "lgc.cooperative.matrix.store";
const static char CooperativeMatrixConvert[] = "lgc.cooperative.matrix.convert";
const static char CooperativeMatrixBinOp[] = "lgc.cooperative.matrix.binop";
const static char CooperativeMatrixTimesScalar[] = "lgc.cooperative.matrix.times.scalar";
const static char CooperativeMatrixTranspose[] = "lgc.cooperative.matrix.transpose";
const static char CooperativeMatrixMulAdd[] = "lgc.cooperative.matrix.muladd";

} // namespace lgcName

// Value for high half of address that means "use PC".
const static unsigned HighAddrPc = ~0U;

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

// Maximum tess factors per patch
static const unsigned MaxTessFactorsPerPatch = 6; // 4 outer factors + 2 inner factors

static const char RayQueryLdsStackName[] = "LdsStack";
// NOTE: Currently, we restrict the max thread count of ray query to be 64 and make sure the wave size is 64. This is
// because we don't provide the capability of querying thread ID in group for ray query in vertex processing shaders.
// In the future, if such is done, we could consider to remove this restriction.
static const unsigned MaxRayQueryThreadsPerGroup = 64; // Max number of ray query threads per group
static const unsigned MaxRayQueryLdsStackEntries = 16; // Max number of ray query LDS stack entries

// Internal resource table's virtual descriptor sets
static const unsigned InternalResourceTable = 0x10000000;

} // namespace lgc
