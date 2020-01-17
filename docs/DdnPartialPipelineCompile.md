# Partial Pipeline Compile


# INTRODUCTION
Per pervious profile result, many Vulkan APPs loading SPIRV binary is much ahead of create pipeline. And APPs hate the long pipeline creation time, since it will cause the lag in app.
To reduce the pipeline creation time, one possible idea is shifting the workload from create pipeline to create shader module. But we lack the pipeline states when create shader module, especially, we don’t have the pipeline layout, it is the major interface between shader and driver.
This DDN proposes a method to build pipeline layout from SPIRV binary and make it compatible with APP’s pipeline layout without recompile.

# BACKGROUND DETAILS
To achieve best performance, we should combine it with async shader module compile.
And we should only try creating pipeline in shader module for compute shader and fragment shader, since there are too many pipeline states which affect the compile result for other shader stages.

# INTERNAL CHANGE

## Collect Resource Usage from Compiler
To generate pipeline layout from SPIRV, we need get resource usage from SPIRV binary. LLPC has this information, but it isn’t exposed explicitly in the interface of build shader module.
To avoid affect the behavior of defer shader module compile, we only access the resource usage when defer compile shader module isn’t enabled.

## Auto Layout Pipeline Layout
Vulkan pipeline layout doesn’t define the physical offset explicitly in the descriptor layout, it gives us a chance generate pipeline layout which compatible with the application’s pipeline layout automatically according to some pre-defined rules.

Because PAL can support user data remapping, we needn’t worry about offset in root level. We only need make layout compatible within the set table. 

The simplest rule is forcing all descriptor types use same descriptor size. And reserve the space when the binding isn’t continuous, or it is a dynamic buffer descriptor. Then we can calculate the byte offset according to binding.i.e.

_resourceOffset = resourceBinding * fixed size._

Dynamic buffer and push constant are special.  According to profile result, Apps often use all buffer as dynamic buffer or doesn’t use dynamic buffer at all. so we can add two options and set it per app.

*   Force all uniform buffer are dynamic buffer in auto layout pipeline layout
*   Force all storage buffer are dynamic buffer in auto layout pipeline layout.

Because we can’t get the whole size of push constant according to fragment shader binary, it is better to define Push constant at the end of root descriptor list.

Beside this, we need extend user data remapping in PAL, until now,
*	CS doesn’t support user data remapping
*	Spill table doesn’t support user data remapping.

## Compile SPIRV shader with Auto Layout Pipeline Layout
When build partial pipleine is enabled, we will try to build partial pipeline with auto layout pieline layout in vkCreateShaderModule.
To check whether shader hash is compatible with auto-layout pipeline, we need modify exist hash algorithm for descriptor layout, only active resource nodes are included, and the offset of root node should be ignored.
 Beside this, color buffer format should be mapped to export format during hash calculation and apply to common path.
The compile result of auto-layout pipeline isn’t stored in any Vulkan API object, it only affects the content of internal shader cache.

To avoid increase the time in vkCreateShaderModule, build partial pipeline will be dispatch to a worker thread.

## Reuse Auto-layout pipeline in Create Pipeline
To reuse auto-layout pipeline, We need add an additional search in shader cache for fragment shader and compute shader after search per shader stage cache. The hash code uses the same rule as build partial pipeline.
When cache is hit, we need adjust the user data mapping in PAL metadata, and repacking ELF binary.




