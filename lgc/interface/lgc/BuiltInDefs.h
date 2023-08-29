/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  BuiltInDefs.h
 * @brief LLPC header file: declaration of BuiltIns supported by the Builder interface
 ***********************************************************************************************************************
 */

// BUILTIN macro defines a built-in that is valid to use in Builder::CreateReadBuiltIn and Builder::CreateWriteBuiltIn.
// The four arguments are:
// 1. the built-in name;
// 2. the built-in ID, as passed to CreateReadBuiltIn and CreateWriteBuiltIn (which happens to be the same as the
//    SPIR-V equivalent where there is one);
// 3. which shader stages the built-in is valid as an output;
// 4. which shader stages the built-in is valid as an input;
// 5. the type of the built-in.
//
// A valid string in (3) and (4) is one or more characters from the following, in this order:
//  T   task shader
//  M   mesh shader
//  V   vertex shader
//  H   tessellation control (hull) shader
//  D   tessellation evaluation (domain) shader
//  G   geometry shader
//  P   fragment (pixel) shader
//  C   compute shader
// or the single character N if the built-in is not valid in any shader stage.
//
// If this file is included without the BUILTIN macro defined, then it declares the BuiltInKind enum.

BUILTIN(BaryCoord, 5286, N, P, v3f32)                    // Perspective-interpolated (I,J,K) at pixel center
BUILTIN(BaryCoordNoPerspKHR, 5287, N, P, v3f32)          // Linearly-interpolated (I,J,K) at pixel center
BUILTIN(BaryCoordNoPersp, 4992, N, P, v2f32)             // Linearly-interpolated (I,J) at pixel center
BUILTIN(BaryCoordNoPerspCentroid, 4993, N, P, v2f32)     // Linearly-interpolated (I,J) at pixel centroid
BUILTIN(BaryCoordNoPerspSample, 4994, N, P, v2f32)       // Linearly-interpolated (I,J) at each covered sample
BUILTIN(BaryCoordPullModel, 4998, N, P, v3f32)           // (1/w,1/I,1/J) at pixel center
BUILTIN(BaryCoordSmooth, 4995, N, P, v2f32)              // Perspective-interpolated (I,J) at pixel center
BUILTIN(BaryCoordSmoothCentroid, 4996, N, P, v2f32)      // Perspective-interpolated (I,J) at pixel centroid
BUILTIN(BaryCoordSmoothSample, 4997, N, P, v2f32)        // Perspective-interpolated (I,J) at each covered sample
BUILTIN(BaseInstance, 4425, N, V, i32)                   // Base instance ID
BUILTIN(BaseVertex, 4424, N, V, i32)                     // Base vertex
BUILTIN(ClipDistance, 3, MVHDG, HDGP, af32)              // Array of clip distances
BUILTIN(CullDistance, 4, MVHDG, HDGP, af32)              // Array of cull distances
BUILTIN(DeviceIndex, 4438, N, TMVHDGPC, i32)             // Device index of the physical device
BUILTIN(DrawIndex, 4426, N, TMV, i32)                    // Draw index
BUILTIN(FragCoord, 15, N, P, v4f32)                      // (x,y,z,1/w) of the current fragment
BUILTIN(FragDepth, 22, P, N, f32)                        // Fragment depth
BUILTIN(FragStencilRef, 5014, P, N, i32)                 // Stencil reference value
BUILTIN(FrontFacing, 17, N, P, i1)                       // Front-facing fragment flag
BUILTIN(GlobalInvocationId, 28, N, TMC, v3i32)           // Invocation ID within global workgroup
BUILTIN(HelperInvocation, 23, N, P, i1)                  // Helper invocation flag
BUILTIN(InstanceIndex, 43, N, V, i32)                    // Instance index
BUILTIN(InvocationId, 8, N, HG, i32)                     // Invocation ID
BUILTIN(Layer, 9, MVDG, P, i32)                          // Layer of multi-layer framebuffer attachment
BUILTIN(LocalInvocationId, 27, N, TMC, v3i32)            // Invocation location within local workgroup
BUILTIN(LocalInvocationIndex, 29, N, TMC, i32)           // Linearized LocalInvocationId
BUILTIN(NumSubgroups, 38, N, TMC, i32)                   // Number of subgroups in the local workgroup
BUILTIN(NumWorkgroups, 24, N, TMC, v3i32)                // Number of local workgroups in the dispatch
BUILTIN(PatchVertices, 14, N, HD, i32)                   // Number of vertices in the input patch
BUILTIN(PointCoord, 16, N, P, v2f32)                     // Coord of current fragment within the point
BUILTIN(PointSize, 1, MVHDG, HDG, f32)                   // Size of point primitives
BUILTIN(Position, 0, MVHDG, HDG, v4f32)                  // Vertex position
BUILTIN(PrimitiveId, 7, MG, HDGP, i32)                   // Index of the current primitive
BUILTIN(PrimitiveShadingRate, 4432, MVG, N, i32)         // Shading rate used for fragments generated for this primitive
BUILTIN(SampleId, 18, N, P, i32)                         // Sample ID
BUILTIN(SampleMask, 20, P, P, ai32)                      // Sample mask coverage
BUILTIN(SamplePosition, 19, N, P, v2f32)                 // Sub-pixel position of the sample being shaded
BUILTIN(ShadingRate, 4444, N, P, i32)                    // Current fragment's shading rate
BUILTIN(SubgroupEqMask, 4416, N, TMVHDGPC, v4i32)        // Subgroup mask where bit index == SubgroupLocalInvocationId
BUILTIN(SubgroupGeMask, 4417, N, TMVHDGPC, v4i32)        // Subgroup mask where bit index >= SubgroupLocalInvocationId
BUILTIN(SubgroupGtMask, 4418, N, TMVHDGPC, v4i32)        // Subgroup mask where bit index > SubgroupLocalInvocationId
BUILTIN(SubgroupId, 40, N, TMC, i32)                     // Index of subgroup within local workgroup
BUILTIN(SubgroupLeMask, 4419, N, TMVHDGPC, v4i32)        // Subgroup mask where bit index <= SubgroupLocalInvocationId
BUILTIN(SubgroupLocalInvocationId, 41, N, TMVHDGPC, i32) // Index of invocation within subgroup
BUILTIN(SubgroupLtMask, 4420, N, TMVHDGPC, v4i32)        // Subgroup mask where bit index < SubgroupLocalInvocationId
BUILTIN(SubgroupSize, 36, N, TMVHDGPC, i32)              // Number of invocations in the subgroup
BUILTIN(TessCoord, 13, N, D, v3f32)                      // (u,v,w) coord of tessellated vertex in patch
BUILTIN(TessLevelInner, 12, H, D, a2f32)                 // Tessellation inner levels
BUILTIN(TessLevelOuter, 11, H, D, a4f32)                 // Tessellation outer levels
BUILTIN(VertexIndex, 42, N, V, i32)                      // Index of current vertex
BUILTIN(ViewIndex, 4440, N, MVHDGP, i32)                 // View index
BUILTIN(ViewportIndex, 10, MVDG, P, i32)                 // Viewport index
BUILTIN(WorkgroupId, 26, N, TMC, v3i32)                  // ID of global workgroup
BUILTIN(WorkgroupSize, 25, N, TMC, v3i32)                // Size of global workgroup
BUILTIN(CullPrimitive, 5299, N, M, i1)                   // Whether primitive should be culled
BUILTIN(PrimitivePointIndices, 5294, N, M, ai32)         // Array of indices of the vertices making up the points
BUILTIN(PrimitiveLineIndices, 5295, N, M, av2i32)        // Array of indices of the vertices making up the lines
BUILTIN(PrimitiveTriangleIndices, 5296, N, M, av3i32)    // Array of indices of the vertices making up the triangles
BUILTIN(VertexId, 5, N, V, i32)                          // Index of current vertex
BUILTIN(InstanceId, 6, N, V, i32)                        // Index of current primitive

// Reserved LGC internal built-ins

// Internal built-ins for fragment input interpolation (I/J)
BUILTIN(InterpPerspSample, BuiltInInternalBase + 0, N, P, v2f32)
BUILTIN(InterpPerspCenter, BuiltInInternalBase + 1, N, P, v2f32)
BUILTIN(InterpPerspCentroid, BuiltInInternalBase + 2, N, P, v2f32)
BUILTIN(InterpPullMode, BuiltInInternalBase + 3, N, P, v3f32)
BUILTIN(InterpLinearSample, BuiltInInternalBase + 4, N, P, v2f32)
BUILTIN(InterpLinearCenter, BuiltInInternalBase + 5, N, P, v2f32)
BUILTIN(InterpLinearCentroid, BuiltInInternalBase + 6, N, P, v2f32)

// Internal built-ins for sample position emulation
BUILTIN(SamplePosOffset, BuiltInInternalBase + 7, N, P, i32)
BUILTIN(NumSamples, BuiltInInternalBase + 8, N, P, i32)
BUILTIN(SamplePatternIdx, BuiltInInternalBase + 9, N, P, i32)
BUILTIN(GsWaveId, BuiltInInternalBase + 10, N, G, i32)

// Internal built-ins for compute input when thread id is swizzled
BUILTIN(UnswizzledLocalInvocationId, BuiltInInternalBase + 11, N, C, i32)
BUILTIN(UnswizzledLocalInvocationIndex, BuiltInInternalBase + 12, N, C, i32)

BUILTIN(EdgeFlag, BuiltInInternalBase + 18, V, V, i32) // EdgeFlag output
