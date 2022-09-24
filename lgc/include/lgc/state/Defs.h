/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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

// Internal built-ins for fragment input interpolation (I/J)
static const BuiltInKind BuiltInInterpPerspSample = static_cast<BuiltInKind>(0x10000000);
static const BuiltInKind BuiltInInterpPerspCenter = static_cast<BuiltInKind>(0x10000001);
static const BuiltInKind BuiltInInterpPerspCentroid = static_cast<BuiltInKind>(0x10000002);
static const BuiltInKind BuiltInInterpPullMode = static_cast<BuiltInKind>(0x10000003);
static const BuiltInKind BuiltInInterpLinearSample = static_cast<BuiltInKind>(0x10000004);
static const BuiltInKind BuiltInInterpLinearCenter = static_cast<BuiltInKind>(0x10000005);
static const BuiltInKind BuiltInInterpLinearCentroid = static_cast<BuiltInKind>(0x10000006);

// Internal built-ins for sample position emulation
static const BuiltInKind BuiltInSamplePosOffset = static_cast<BuiltInKind>(0x10000007);
static const BuiltInKind BuiltInNumSamples = static_cast<BuiltInKind>(0x10000008);
static const BuiltInKind BuiltInSamplePatternIdx = static_cast<BuiltInKind>(0x10000009);
static const BuiltInKind BuiltInGsWaveId = static_cast<BuiltInKind>(0x1000000A);

// Internal builts-ins for compute input when thread id is swizzled
static const BuiltInKind BuiltInHwLocalInvocationId = static_cast<BuiltInKind>(0x1000000B);
static const BuiltInKind BuiltInHwLocalInvocationIndex = static_cast<BuiltInKind>(0x1000000C);

// Names used for calls added to IR to represent various actions internally.
namespace lgcName {
const static char InternalCallPrefix[] = "lgc.";
const static char InputCallPrefix[] = "lgc.input.";
const static char InputImportVertex[] = "lgc.input.import.vertex.";
const static char InputImportGeneric[] = "lgc.input.import.generic.";
const static char InputImportBuiltIn[] = "lgc.input.import.builtin.";
const static char InputImportInterpolant[] = "lgc.input.import.interpolant.";
const static char OutputCallPrefix[] = "lgc.output.";
const static char OutputImportGeneric[] = "lgc.output.import.generic.";
const static char OutputImportBuiltIn[] = "lgc.output.import.builtin.";
const static char OutputExportGeneric[] = "lgc.output.export.generic.";
const static char OutputExportBuiltIn[] = "lgc.output.export.builtin.";
const static char OutputExportXfb[] = "lgc.output.export.xfb.";
const static char TfBufferStore[] = "lgc.tfbuffer.store.";
const static char StreamOutBufferStore[] = "lgc.streamoutbuffer.store";
const static char ReconfigureLocalInvocationId[] = "lgc.reconfigure.local.invocation.id";
const static char SwizzleLocalInvocationId[] = "lgc.swizzle.local.invocation.id";
const static char SwizzleWorkgroupId[] = "lgc.swizzle.workgroup.id";

const static char MeshTaskCallPrefix[] = "lgc.mesh.task.";
const static char MeshTaskReadTaskPayload[] = "lgc.mesh.task.read.task.payload";
const static char MeshTaskWriteTaskPayload[] = "lgc.mesh.task.write.task.payload";
const static char MeshTaskAtomicTaskPayload[] = "lgc.mesh.task.atomic.task.payload";
const static char MeshTaskAtomicCompareSwapTaskPayload[] = "lgc.mesh.task.atomic.compare.swap.task.payload";
const static char MeshTaskEmitMeshTasks[] = "lgc.mesh.task.emit.mesh.tasks";
const static char MeshTaskSetMeshOutputs[] = "lgc.mesh.task.set.mesh.outputs";
const static char MeshTaskSetPrimitiveIndices[] = "lgc.mesh.task.set.primitive.indices.";
const static char MeshTaskSetPrimitiveCulled[] = "lgc.mesh.task.set.primitive.culled";
const static char MeshTaskGetMeshInput[] = "lgc.mesh.task.get.mesh.input.";
const static char MeshTaskWriteVertexOutput[] = "lgc.mesh.task.write.vertex.output.";
const static char MeshTaskWritePrimitiveOutput[] = "lgc.mesh.task.write.primitive.output.";

// Get pointer to spill table (as pointer to i8)
const static char SpillTable[] = "lgc.spill.table";
// Get pointer to push constant (as pointer type indicated by the return type)
const static char PushConst[] = "lgc.push.const";
// Get a descriptor that is in the root user data (as descriptor type indicated by the return type).
// The arg is the dword offset of the node in the root user data layout.
const static char RootDescriptor[] = "lgc.root.descriptor";
// Get pointer to the descriptor table for the given resource. First arg is the descriptor set number; second arg
// is the binding number; third arg is the value to use for the high half of the address, or HighAddrPc to use PC.
const static char DescriptorTableAddr[] = "lgc.descriptor.table.addr";
// Get special user data input. Arg is UserDataMapping enum value. The optional second arg causes the 32-bit
// value to be extended to 64-bit pointer and specifies the value to use for the high half, or
// ShadowDescriptorTable::Disable to use PC.
const static char SpecialUserData[] = "lgc.special.user.data.";
// Get shader input. Arg is ShaderInput enum value.
const static char ShaderInput[] = "lgc.shader.input.";

const static char LaterCallPrefix[] = "lgc.late.";
const static char LateLaunderFatPointer[] = "lgc.late.launder.fat.pointer";
const static char LateBufferLength[] = "lgc.late.buffer.desc.length";
const static char LateBufferPtrDiff[] = "lgc.late.buffer.ptrdiff";

// Names of global variables
const static char ImmutableSamplerGlobal[] = "lgc.immutable.sampler";
const static char ImmutableConvertingSamplerGlobal[] = "lgc.immutable.converting.sampler";

// Names of entry-points for merged shader
const static char EsGsEntryPoint[] = "lgc.shader.ESGS.main";
const static char LsHsEntryPoint[] = "lgc.shader.LSHS.main";

const static char NggEsEntryPoint[] = "lgc.ngg.ES.main";
const static char NggEsCullDataFetch[] = "lgc.ngg.ES.cull.data.fetch";
const static char NggEsDeferredVertexExport[] = "lgc.ngg.ES.deferred.vertex.export";

const static char NggGsEntryPoint[] = "lgc.ngg.GS.main";
const static char NggGsOutputExport[] = "lgc.ngg.GS.output.export.";
const static char NggGsOutputImport[] = "lgc.ngg.GS.output.import.";
const static char NggGsEmit[] = "lgc.ngg.GS.emit";
const static char NggGsCut[] = "lgc.ngg.GS.cut";

const static char NggCopyShaderEntryPoint[] = "lgc.ngg.COPY.main";
const static char NggPrimShaderEntryPoint[] = "lgc.shader.PRIM.main";

const static char NggCullingFetchReg[] = "lgc.ngg.culling.fetchreg";
const static char NggCullingBackface[] = "lgc.ngg.culling.backface";
const static char NggCullingFrustum[] = "lgc.ngg.culling.frustum";
const static char NggCullingBoxFilter[] = "lgc.ngg.culling.boxfilter";
const static char NggCullingSphere[] = "lgc.ngg.culling.sphere";
const static char NggCullingSmallPrimFilter[] = "lgc.ngg.culling.smallprimfilter";
const static char NggCullingCullDistance[] = "lgc.ngg.culling.culldistance";

const static char EntryPointPrefix[] = "lgc.shader.";
const static char CopyShaderEntryPoint[] = "lgc.shader.COPY.main";
const static char NullFsEntryPoint[] = "lgc.shader.FS.null.main";

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

#if VKI_RAY_TRACING
static const char RayQueryLdsStackName[] = "LdsStack";
// NOTE: Currently, we restrict the max thread count of ray query to be 64 and make sure the wave size is 64. This is
// because we don't provide the capability of querying thread ID in group for ray query in vertex processing shaders.
// In the future, if such is done, we could consider to remove this restriction.
static const unsigned MaxRayQueryThreadsPerGroup = 64; // Max number of ray query threads per group
static const unsigned MaxRayQueryLdsStackEntries = 16; // Max number of ray query LDS stack entries
#endif

// Internal resource table's virtual descriptor sets
static const unsigned InternalResourceTable = 0x10000000;
static const unsigned InternalPerShaderTable = 0x10000001;

} // namespace lgc
