# Partial Pipeline Compile


# INTRODUCTION
Per pervious profile result, many Vulkan APPs loading SPIR-V binary is much ahead of create pipeline. And APPs hate the long pipeline creation time, since it will cause the lag in app.
To reduce the pipeline creation time, one possible idea is shifting the workload from create pipeline to create shader module. But we lack the pipeline states when create shader module, especially, we don’t have the pipeline layout, it is the major interface between shader and driver.
This DDN proposes a method to build pipeline layout from SPIR-V binary and make it compatible with APP’s pipeline layout without recompile.

# BACKGROUND DETAILS
To achieve best performance, we should combine it with async shader module compile.
And we should only try creating pipeline in shader module for compute shader and fragment shader, since there are too many pipeline states which affect the compile result for other shader stages.

# INTERNAL CHANGE

## Collect Resource Usage from Compiler
To generate pipeline layout from SPIR-V, we need get resource usage from SPIR-V binary. LLPC has this information, but it isn’t exposed explicitly in the interface of build shader module.
To avoid affect the behavior of defer shader module compile, we only access the resource usage when defer compile shader module isn’t enabled.
We need collect two kinds of resource from "ResourceCollect" pass, one is ResourceMappingNode info, another is fragment shader output format. For ResourceMappingNode info, we get them from SPIRAS_Constant and SPIRAS_Uniform and builder calls. For fragment shader output format, we get it from SPIRAS_Out.
Resource node is defined as below:
```
/// Represents resource node data
struct ResourceNodeData
{
    ResourceMappingNodeType type;       ///< Type of this resource mapping node
    uint32_t                set;        ///< ID of descriptor set
    uint32_t                binding;    ///< ID of descriptor binding
    uint32_t                arraySize;  ///< Element count for arrayed binding
};
```
Fragment shader output info is defined as below:
```
/// Represents fragment shader output info
struct FsOutInfo
{
    uint32_t    location;       ///< Output location in resource layout
    uint32_t    index;          ///< Output index in resource layout
    BasicType   basicType;      ///< Output data type
    uint32_t    componentCount; ///< Count of components of output data
};
```

To expose these info to client driver, we refine the ShaderModuleData to include more info:
```
/// Represents the information of one shader entry in ShaderModuleExtraData
struct ShaderModuleEntryData
{
    ShaderStage             stage;              ///< Shader stage
    const char*             pEntryName;         ///< Shader entry name
    void*                   pShaderEntry;       ///< Private shader module entry info
    uint32_t                resNodeDataCount;   ///< Resource node data count
    const ResourceNodeData* pResNodeDatas;      ///< Resource node data array
    uint32_t                pushConstSize;      ///< Push constant size in byte
};

/// Represents usage info of a shader module
struct ShaderModuleUsage
{
    bool                  enableVarPtrStorageBuf;  ///< Whether to enable "VariablePointerStorageBuffer" capability
    bool                  enableVarPtr;            ///< Whether to enable "VariablePointer" capability
    bool                  useSubgroupSize;         ///< Whether gl_SubgroupSize is used
    bool                  useHelpInvocation;       ///< Whether fragment shader has helper-invocation for subgroup
    bool                  useSpecConstant;         ///< Whether specializaton constant is used
    bool                  keepUnusedFunctions;     ///< Whether to keep unused function
};

/// Represents common part of shader module data
struct ShaderModuleData
{
    uint32_t         hash[4];       ///< Shader hash code
    BinaryType       binType;       ///< Shader binary type
    BinaryData       binCode;       ///< Shader binary data
    uint32_t         cacheHash[4];  ///< Hash code for calculate pipeline cache key
    ShaderModuleUsage usage;        ///< Usage info of a shader module
};

/// Represents extended output of building a shader module (taking extra data info)
struct ShaderModuleDataEx
{
    ShaderModuleData        common;         ///< Shader module common data
    uint32_t                codeOffset;     ///< Binary offset of binCode in ShaderModuleDataEx
    uint32_t                entryOffset;    ///< Shader entry offset in ShaderModuleDataEx
    uint32_t                resNodeOffset;  ///< Resource node offset in ShaderModuleDataEX
    uint32_t                fsOutInfoOffset;///< FsOutInfo offset in ShaderModuleDataEX
    struct
    {
        uint32_t              fsOutInfoCount;           ///< Count of fragment shader output
        const FsOutInfo*      pFsOutInfos;              ///< Fragment output info array
        uint32_t              entryCount;              ///< Shader entry count in the module
        ShaderModuleEntryData entryDatas[1];           ///< Array of all shader entries in this module
    } extra;                              ///< Represents extra part of shader module data
};

/// Represents output of building a shader module.
struct ShaderModuleBuildOut
{
    ShaderModuleData*   pModuleData;       ///< Output shader module data (opaque)
};
```

## Auto-layout Pipeline Layout
Vulkan pipeline layout doesn’t define the physical offset explicitly in the descriptor layout, it gives us a chance generate pipeline layout which compatible with the application’s pipeline layout automatically according to some pre-defined rules.

Because PAL can support user data remapping, we needn’t worry about offset in root level. We only need make layout compatible within the set table. 

The simplest rule is forcing all descriptor types use same descriptor size. And reserve the space when the binding isn’t continuous, or it is a dynamic buffer descriptor. Then we can calculate the byte offset according to binding.i.e.

_resourceOffset = resourceBinding * fixed size._

Dynamic buffer and push constant are special.  According to profile result, Apps often use all buffer as dynamic buffer or doesn’t use dynamic buffer at all. so we can add two options and set it per app.

*   Force all uniform buffer are dynamic buffer in auto layout pipeline layout
*   Force all storage buffer are dynamic buffer in auto layout pipeline layout.

Because we can’t get the whole size of Push constant according to fragment shader binary, it is better to define Push constant at the end of root descriptor list.

Beside this, we need extend user data remapping in PAL, until now,
*   CS doesn’t support user data remapping
*   Spill table doesn’t support user data remapping.

## Compile SPIR-V shader with Auto-Layout Pipeline Layout
When build partial pipleine is enabled, we will try to build partial pipeline with auto layout pieline layout in vkCreateShaderModule.
To check whether shader hash is compatible with auto-layout pipeline, we need modify exist hash algorithm for descriptor layout, only active resource nodes are included, and the offset of root node should be ignored.
 Beside this, color buffer format should be mapped to export format during hash calculation and apply to common path.
The compile result of auto-layout pipeline isn’t stored in any Vulkan API object, it only affects the content of internal shader cache.

To avoid increase the time in vkCreateShaderModule, build partial pipeline will be dispatch to a worker thread. Which is similar with async shader module, we create a PartialPipline class, in its Execute() method, we constract pipeline info by using previous exposed resource info, and then compile it.
```
// =====================================================================================================================
// Implementation of a async partial pipeline
class PartialPipeline
{
public:
    static PartialPipeline* Create(
        Device*                         pDevice,
        const VkAllocationCallbacks*    pAllocator);

    VkResult Destroy();

    void CreatePipelineLayoutFromModuleData(
        AsyncLayer*                         pAsyncLayer,
        Llpc::ShaderModuleEntryData*        pShaderModuleEntryData,
        const Llpc::ResourceMappingNode**   ppResourceMappingNode,
        uint32_t*                           pMappingNodeCount);

    void CreateColorTargetFromModuleData(
        Llpc::ShaderModuleDataEx* pShaderModuleDataEx,
        Llpc::ColorTarget* pTarget);

    void Execute(AsyncLayer* pAsyncLayer, PartialPipelineTask* pTask);

    void AsyncBuildPartialPipeline(
            AsyncLayer* pAsyncLayer,
            ShaderModule* pShaderModule,
            VkShaderModule asyncShaderModule);

protected:
    PartialPipeline(const VkAllocationCallbacks* pAllocator);
private:
    const VkAllocationCallbacks*    m_pAllocator;
};
```

## Reuse Auto-layout Pipeline in Create Pipeline
To reuse auto-layout pipeline, We need add an additional search in shader cache for fragment shader and compute shader after search per shader stage cache. The hash code uses the same rule as build partial pipeline.
When cache is hit, we need adjust the user data mapping in PAL metadata, and repacking ELF binary.
