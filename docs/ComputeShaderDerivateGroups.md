# VK_NV_Compute_Shader_derivatives

# Introduction:
This extension adds Vulkan support for the `SPV_NV_compute_shader_derivatives` SPIR-V extension.

The SPIR-V extension provides two new execution modes, both of which allow compute shaders to use built-ins that evaluate compute derivatives explicitly or implicitly.

Derivatives will be computed via difference over a 2x2 group of shader invocations.

The code:DerivativeGroupQuadsNV execution mode assembles shader invocations into 2x2 groups, compute shader invocations are grouped into 2x2x1 arrays whose four local invocation ID values follow the pattern:

        +-----------------+------------------+
        | (2x+0, 2y+0, z) |  (2x+1, 2y+0, z) |
        +-----------------+------------------+
        | (2x+0, 2y+1, z) |  (2x+1, 2y+1, z) |
        +-----------------+------------------+

      - 0th index has a local invocation ID of the form (2x + 0, 2y + 0, z)
      - 1st index has a local invocation ID of the form (2x + 1, 2y + 0, z)
      - 2nd index has a local invocation ID of the form (2x + 0, 2y + 1, z)
      - 3rd index has a local invocation ID of the form (2x + 1, 2y + 1, z)

The code:DerivativeGroupLinearNV execution mode assembles shader invocations into 2x2 groups, compute shader invocations are grouped into 2x2x1 arrays whose four local invocation index values follow the pattern:

        +------+------+
        | 4n+0 | 4n+1 |
        +------+------+
        | 4n+2 | 4n+3 |
        +------+------+

      - 0th index has a local invocation index of the form 4n + 0
      - 1st index has a local invocation index of the form 4n + 1
      - 2nd index has a local invocation index of the form 4n + 2
      - 3rd index has a local invocation index of the form 4n + 3

If neither layout qualifier is specified, derivatives in compute shaders return zero.
Unlike fragment shaders, compute shaders never have any "helper" invocations that are only used for derivatives.  As a result, the local work group width and height must be a multiple of two when using the "quads" layout, and the total number of invocations in a local work group must be a multiple of four when using the "linear" layout.

# BACKGROUND DETAILS
## Derivative Instructions
OpDPdx/OpDPdy/OpFwidth/OpDPdxFine/OpDPdyFine/OpDPdxCoarse/OpDPdyCoarse/OpFwidthCoarse.

## Derivative Calculation under LLPC
MiscBuilder::CreateDerivative is used as the core function for the implementation. It will use Dpp_Quad_Perm to capture the pixel data across lane to calculate derivative.
The use of ComputeDerivateGroup*NV must affect how LocalInvocationId / LocalInvocationIndex is mapped to SubgroupLocalInvocationId, and so it potentially interacts with reconfigWorkgroupLayout and forceCsThreadIdSwizzling.
Specifically, the WorkgroupLayout needs to be overridden in some cases to be consistent with the ComputeDerivateGroup*NV capability.

# Implementation
## XGL Change as advertising the extension
### New Macros and variables:
NV_COMPUTESHADER_DERIVATIVES  ===> to be added in class DeviceExtensions

### Structures already existed:
```c++
typedef struct VkPhysicalDeviceComputeShaderDerivativesFeaturesNV {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           computeDerivativeGroupQuads;
    VkBool32           computeDerivativeGroupLinear;
} VkPhysicalDeviceComputeShaderDerivativesFeaturesNV;
```

### Function Support
VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_NV should be considered under PhysicalDevice::GetFeatures2()

## Interface Change
No interface change here

## LLPC Change
### SPIRV Change:
Updated functions:
SPIRVMap<SPIRVCapabilityKind, SPIRVCapVec>::init()
===> Add two new Capabilities: CapabilityComputeDerivativeGroupLinearNV and CapabilityComputeDerivativeGroupQuadsNV

SPIRVMap<Capability, std::string>::init()
===> Add support for two new Capabilities
When translate SPIRV to LLVM-IR for derivative OpCode: OpDPdx/OpDPdy/OpDPdxFine/OpDPdyFine/OpDPdxCoarse/OpDPdyCoarse, it also needs to add the support for new Capabilities.

### LGC Changes
The current support for workgroup layout configuration is messy, with decisions performed in multiple places  This should be cleaned up first. The workgroup layout will be reconfigured in the follow situations and the supported layout are listed:

- PipelineOptions::ReconfigWorkgroupLayout (Calculate workgroup layout according to current work group size)
	-	Linear (4x1)
	-	Quads (2x2)
	-	SexagintiQuads(8x8)
- PipelineOptions:: forceCsThreadIdSwizzling
	-	8x4 for wave32
	-	8x8 for wave64
- ComputeShaderMode::derivatives
	-	None
	-	Linear(4x1)
	-	Quads(2x2)

The guidance of the swizzle combination:

1. ComputeShaderMode::derivatives enable, it must override the others when necessary
	-	None ==> Then consider forceCsThreadIdSwizzling
    -	Linear ==> Linear must be chosen overall not care other two swizzle functions.
    -	Quads(2x2)
    	-	forceCsThreadIdSwizzling enable: Macro8xN and Micro2x2
		-	reconfigWorkGroupLayout(workGroupSizeX is multiple of 8):  Macro8xN and Micro2x2
		-	reconfigWorkGroupLayout(both workGroupSizeX and WorkgGroupSizeY are multiple of 2): Micro2x2
2. ComputeShaderMode::derivative disable OR derivatives is None:
	-	forceCsThreadId enable:
		-	WorkGroupSizeX is multiple of 8: Swizzle thread id into layout: Macro8xN
		-	WorkGroupSizeX is not multiple of 8: Process according to reconfigWorkGroupLayout
	-	forceCsThreadId disable:
		-	ReconfigWorkGroupLayout enable:
     		-	WorkGroupSizeX is multiple of 8: Swizzle thread id into layout: Macro8xN
			-	Both workGroupSizeX and workGroupSizeY are multiple of 2: Micro2x2
		-	ReconfigWorkGroupLayout disable:
			Nothing to do.  cd

During implementation, some interfaces and structures should be changed as follows:

1. Replace `ResourceUsage::builtInUsage.cs.workgroupLayout` with a `ResourceUsage::builtInUsage.cs.foldWorkgroupXY` boolean. The `Gfx[69]ConfigBuilder` will use this boolean instead of `workgroupLayout`.
2. Add `ComputeShaderMode::derivatives` field with three enum values: `None`, `Linear`, `Quads`. This field is populated by the SPIRV reader.
3. Change the `InOutBuilder` to always insert a call to `lgc.reconfigure.local.invocation.id` for `BuiltInLocalInvocationId`. Remove `lgc.swizzle.local.invocation.id`.
4. `PatchInOutImportExport::processShader` will handle `lgc.reconfigure.local.invocation.id` similar to today. However:
   * Take the `ComputeShaderMode::derivatives` field into account when determining the workgroup layout to use
   * Integrate `swizzleLocalInvocationIdIn8x4`; basically, the decision of micro- vs. macro-tiling becomes explicitly orthogonal. Instead of `WorkgroupLayout::{Linear, Quads, SexagintiQuads}` and then 8x4 swizzling, there is only `Linear` vs. `Quads` for the micro-tiling and then `Linear` vs. `Block8` for the macro-tiling.
   * The `forceCsThreadIdSwizzling` pipeline option can stay the same for compatibility, it is simply used as an input to the determination of which layout to use.

Note that all decision-making is centralized in one place, which is `PatchInOutImportExport`: the inputs of pipeline options as well as derivatives mode are used once to determine the workgroup layout.

# Issues
## Helper invocations support
No support for help invocation on compute shaders.
