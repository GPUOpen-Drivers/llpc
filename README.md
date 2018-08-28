
# LLVM-Based Pipeline Compiler (LLPC) 

LLPC builds on LLVM's existing shader compilation infrastructure for AMD GPUs to generate code objects compatible with PAL's pipeline ABI. It consists of three components: 

![LLPC Architecture Diagram](LLPCArch.png)

* SPIR-V translator is based on Khronos SPIRV-LLVM translator. It translates SPIR-V binary to LLVM IR with rich metadata. 

* Lower translates all LLVM compiler unsupported LLVM IR and metadata to function calls. 

Both SPIR-V translator and Lower are machine independent. 

* Patcher replaces all external function calls with LLVM compiler compatible LLVM IR according to the pipeline information. It calls LLVM and reorganizes LLVM compiler's output with PAL Pipeline ABI.  

## Standalone Compiler

LLPC could be built into a standalone offline compiler (amdllpc). It supports GLSL, SPIR-V binary and SPIR-V assemble file as input and output GPU ISA code and related register settings.

### Build Instruction
Please refer to the [Build Instructions](https://github.com/GPUOpen-Drivers/AMDVLK#build-instructions) of amdvlk. By default, amdllpc is built together with Vulkan driver. You can use "make amdllpc" to build amdllpc only. 

### Usage
```
export LD_LIBRARY_PATH=<path_to_spvgen>:$LD_LIBRARY_PATH
amdllpc [<options>...] [<files>...]
```

#### Options

* Basic options

| Option Name                      | Description                                                       | Default Value                 |
| ------------------------------   | ----------------------------------------------------------------- | ------------------------------|
| `-help`                          | Print detail help, include all LLVM options                       |                               |
| `-gfxip=<major.minor.step>`      | Graphics IP version                                               | 8.0.0                         |                                                                                                |
| `-auto-layout-desc`              | Automatically create descriptor layout based on resource usages   |                               | 
| `-o=<filename>`                  | Output ELF binary file	                                       |                               |  
| `-entry-target=<entryname>`      | Name string of entry target in SPIRV                              | main                          |
| `-val	`                          | Validate input SPIR-V binary or text	                       |                               |

* Dump options

| Option Name                      | Description                                                       | Default Value                 |
| ------------------------------   | ----------------------------------------------------------------- | ------------------------------|
| `-enable-errs`                   | Enable error message output (to stdout or external file)	       |                               |
| `-enable-outs`                   | Enable general message output (to stdout or external file)        |                               |
| `-enable-time-profiler`          | Enable time profiler for various compilation phases	       |                               | 
| `-log-file-dbgs=<filename>`      | Name of the file to log info from dbgs()	llpcLog.txt            |                               |
| `-log-file-outs=<filename>`      | Name of the file to log info from LLPC_OUTS() and LLPC_ERRS()     |                               |           
| `-enable-pipeline-dump`          | Enable pipeline info dump	                                       |                               |
| `-pipeline-dump-dir=<directory>` | Directory where pipeline shader info are dumped	               |                               |   


* Debug & Performance tunning options

| Option Name                      | Description                                                       | Default Value                 |
| ------------------------------   | ----------------------------------------------------------------- | ------------------------------|
| `-enable-errs`                   | Enable error message output (to stdout or external file)	       |                               |
| `-enable-si-scheduler`           | Enable target option si-scheduler	      |                               |
| `-disable-gs-onchip`             | Disable geometry shader on-chip mode	      |                               |
| `-enable-tess-offchip`           | Enable tessellation off-chip mode	      |                               |
| `-disable-fp32-denormals`        | Disable target option fp32-denormals	      |                               |
| `-disable-llvm-patch`	           | Disable the patch for LLVM back-end issues	      |                               |
| `-disable-lower-opt`             | Disable optimization for SPIR-V lowering	      |                               |
| `-ignore-color-attachment-formats`| Ignore color attachment formats	      |                               |
| `-lower-dyn-index`	           | Lower SPIR-V dynamic (non-constant) index in access chain	      |                               |
| `-vgpr-limit=<uint>`	           | Maximum VGPR limit for this shader	|0 |
| `-sgpr-limit=<uint>`	           | Maximum SGPR limit for this shader	|0 |
| `-waves-per-eu=<minVal,maxVal>`  | The range of waves per EU for this shader	empty      |                               |
| `-shader-cache-mode=<uint>`      | Shader cache mode <br/> 0 - disable <br/> 1 - runtime cache <br/> 2 - cache to disk	| 1 |
| `-shader-replace-dir=<dir>`      | Directory to store the files used in shader replacement	      |                               |.
| `-shader-replace-mode=<uint>`    | Shader replacement mode <br/> 0 - disable <br/> 1 - replacement based on shader hash <br/> 2 - replacement based on both shader hash and pipeline hash | 0 |
| `-shader-replace-pipeline-hashes=<hashes with comma as separator>`|A collection of pipeline hashes, specifying shader replacement is operated on which pipelines      |                               |
| `-enable-shadow-desc`	           | Enable shadow descriptor table 	      |                               |
| `-shadow-desc-table-ptr-high=<uint>`| High part of VA for shadow descriptor table pointer	| 2|

> **Note:** amdllpc overwrites following native options in LLVM:
>>>> -pragma-unroll-threshold=4096 -unroll-allow-partial -simplifycfg-sink-common=false -amdgpu-vgpr-index-mode -filetype=obj


#### File formats

```
<file>.vert     GLSL source text file for a vertex shader

<file>.tesc     GLSL source text file for a tessellation control shader

<file>.tese     GLSL source text file for a tessellation evaluation shader

<file>.geom     GLSL source text file for a tessellation geometry shader

<file>.frag     GLSL source text file for a tessellation fragment shader

<file>.comp     GLSL source text file for a tessellation compute shader

<file>.spv      SPIR-V binary file

<file>.spvas    SPIR-V text file

<file>.pipe     Pipeline info file
```
> **Note:** To compile GLSL source text file and Pipeline info file, amdllpc needs to call [spvgen](https://github.com/GPUOpen-Drivers/spvgen). The path of spvgen library needs to be added to environment variable LD_LIBRARY_PATH.


#### Examples

* Compile single fragment shader "a.frag" on Vega10
```
amdllpc -auto-layout-desc -gfxip=9.0.0 a.frag
```

* Compile full pipeline file "b.pipe" on Ellesmere and output to c.elf
```
amdllpc -gfxip=8.0.3 -o=c.elf b.pipe
```


## Test with SHADERDB
You can use [shaderdb](https://github.com/GPUOpen-Drivers/xgl/tree/master/test/shadertest) to test llpc with standalone compiler and [spvgen](https://github.com/GPUOpen-Drivers/spvgen):

```
python testShaders.py [-h] [--shaderdb <path_to_shaderdb>] [--gfxip <major.minor.step>] <path_to_amdllpc> <path_to_spvgen>
```

## Third Party Software  
LLPC contains code written by third parties:
* SPIRV-LLVM translator is distributed under the terms of University of Illinois/NCSA Open Source License. See translator/LICENSE.TXT.  
* SPIRV-Tools is distributed under the terms of Apache License version 2.0. See translator/hex_float.h and bitutils.h.
* Metrohash is distributed under the terms of MIT License. See imported/metrohash/metrohash-license.

