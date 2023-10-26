# amdllpc Standalone Compiler

LLPC can be built into a standalone offline compiler (amdllpc). It supports GLSL, SPIR-V binary and SPIR-V assembly file as input and outputs GPU ISA code and related register settings (PAL metadata).

## Build instructions

LLPC is normally built as part of the [AMD Open Source Driver for Vulkan](https://github.com/GPUOpen-Drivers/AMDVLK/blob/dev/README.md). The build includes standalone `lgc` and `amdllpc` (**Note:** You need to add the option `-DXGL_BUILD_TOOLS=ON` in the AMDVLK cmake command before building `amdllpc`).
You can build `lgc amdllpc` only or build `check-lgc check-lgc-units check-amdllpc check-amdllpc-units check-continuations check-continuations-units` to run local tests besides the build (**Note:** You need to add the option `-DXGL_BUILD_TESTS=ON` in the AMDVLK cmake command before building these local test targets).
```
cmake --build xgl/builds/Release64 --target lgc amdllpc
```

or
```
cmake --build xgl/builds/Release64 --target check-lgc check-lgc-units check-amdllpc check-amdllpc-units check-continuations check-continuations-units
```

LLPC also contains amber tests that need an actual GPU to run. See the [test directory](../../test/) for more information.
```
cmake --build xgl/builds/Release64 --target check-amber
```

The `amdllpc` build needs SPVGEN.
If the amdllpc build fails with an error like this:
```
drivers/llpc/llpc/tool/llpcCompilationUtils.cpp:69:10: fatal error: spvgen.h: No such file or directory
```

then you need to fetch spvgen and external sources (glslang and SPIRV-Tools):
```
repo init -m build_with_tools.xml
repo sync
```
and then retry the ninja command.

When you need to investigate a test failure, run a single test from that same build directory like this example:
```
llvm/bin/llvm-lit -v llpc/test/shaderdb/OpAtomicIIncrement_TestVariablePointer_lit.spvasm
```

### Standalone build

If you don’t want to depend on many of the packages required for a driver build, you can build LLPC standalone and run the amdllpc and lgc lit tests from there instead. It
pretty much has the same requirements as an LLVM build.

Once you have followed the driver build instructions for installing source, starting at the top level `llpc` directory:

```
cd llpc
cmake -G Ninja -B build [-DPAL_CLIENT_INTERFACE_MAJOR_VERSION=<pal_interface_version>]
cmake --build build --target check-lgc check-lgc-units check-amdllpc check-amdllpc-units check-continuations check-continuations-units
```

See above if this gives an error due to not finding an include file from glslang or SPIRV-Tools.
If you want to make amdllpc compatible with driver, you could get `<pal_interface_version>` from the
`ICD_PAL_CLIENT_MAJOR_VERSION` defined in `xgl/cmake/XglVersions.cmake` and add it in the build option.
If the build option is not added, the latest PAL interface version will be used.

## Usage
```
amdllpc [<options>...] [<input_file[,entry_point]>...]
```

### Options

* Basic options

| Option Name                      | Description                                                       | Default Value                 |
| -------------------------------- | ----------------------------------------------------------------- | ------------------------------|
| `-help`                          | Print detail help, include all LLVM options                       |                               |
| `-gfxip=<major.minor.step>`      | Graphics IP version                                               | 8.0.0                         |
| `-o=<filename>`                  | Output ELF binary file                                            |                               |
| `-validate-spirv`                | Validate input SPIR-V binary or text                              |                               |
| `-verify-ir`                     | Verify LLVM IR after each pass                                    | false                         |

* Pipeline compilation options

| Option Name                      | Description                                                       | Default Value                 |
| -------------------------------- | ----------------------------------------------------------------- | ------------------------------|
| `-auto-layout-desc`              | Automatically create descriptor layout based on resource usages   | false for .pipe files, true for individual shaders |
| `-robust-buffer-access`          | Validate if buffer index is out of bounds                         | false                         |
| `-enable-relocatable-shader-elf` | Compile pipelines using relocatable shader elf                    | false                         |
| `-enable-scratch-bounds-checks`  | Insert scratch access bounds checks on loads and stores           | false                         |
| `-scalar-block-layout`           | Allow scalar block layout of types                                | false                         |

* Dump options

| Option Name                      | Description                                                       | Default Value                 |
| -------------------------------- | ----------------------------------------------------------------- | ------------------------------|
| `-enable-errs`                   | Enable error message output (to stdout or external file)          |                               |
| `-enable-outs`                   | Enable LLPC-specific debug dump output (to stdout or external     | false                         |
|                                  | file)                                                             |                               |
| `-v`                             | Alias for `-enable-outs`                                          | false                         |
| `-enable-time-profiler`          | Enable time profiler for various compilation phases               |                               |
| `-log-file-dbgs=<filename>`      | Name of the file to log info from dbgs()                          | "" (meaning stderr)           |
| `-log-file-outs=<filename>`      | Name of the file to log info from LLPC_OUTS() and LLPC_ERRS()     |                               |
| `-enable-pipeline-dump`          | Enable pipeline info dump                                         |                               |
| `-pipeline-dump-dir=<directory>` | Directory where pipeline shader info are dumped                   |                               |
| `-emit-lgc`                      | Emit LLVM IR assembly just before LGC (middle-end)                | false                         |
| `-emit-llvm`                     | Emit LLVM IR assembly just before LLVM back-end                   | false                         |
| `-emit-llvm-bc`                  | Emit LLVM IR bitcode just before LLVM back-end                    | false                         |

* Debug & Performance tuning options

| Option Name                      | Description                                                       | Default Value                 |
| -------------------------------- | ----------------------------------------------------------------- | ------------------------------|
| `-enable-si-scheduler`           | Enable target option si-scheduler                                 |                               |
| `-disable-gs-onchip`             | Disable geometry shader on-chip mode                              |                               |
| `-enable-tess-offchip`           | Enable tessellation off-chip mode                                 |                               |
| `-disable-llvm-patch`            | Disable the patch for LLVM back-end issues                        |                               |
| `-disable-lower-opt`             | Disable optimization for SPIR-V lowering                          |                               |
| `-disable-licm`                  | Disable LLVM LICM pass                                            |                               |
| `-ignore-color-attachment-formats`| Ignore color attachment formats                                  |                               |
| `-lower-dyn-index`               | Lower SPIR-V dynamic (non-constant) index in access chain         |                               |
| `-vgpr-limit=<uint>`             | Maximum VGPR limit for this shader                                | 0                             |
| `-sgpr-limit=<uint>`             | Maximum SGPR limit for this shader                                | 0                             |
| `-waves-per-eu=<minVal,maxVal>`  | The range of waves per EU for this shader  empty                  |                               |
| `-shader-cache-mode=<uint>`      | Shader cache mode <br/> 0 - disable <br/> 1 - runtime cache <br/> 2 - cache to disk | 1           |
| `-shader-replace-dir=<dir>`      | Directory to store the files used in shader replacement           |                               |
| `-shader-replace-mode=<uint>`    | Shader replacement mode <br/> 0 - disable <br/> 1 - replacement based on shader hash <br/> 2 - replacement based on both shader hash and pipeline hash | 0 |
| `-shader-replace-pipeline-hashes=<hashes with comma as separator>`|A collection of pipeline hashes, specifying shader replacement is operated on which pipelines | |
| `-enable-shadow-desc`            | Enable shadow descriptor table                                    |                               |
| `-shadow-desc-table-ptr-high=<uint>`| High part of VA for shadow descriptor table pointer            | 2                             |

> **Note:** amdllpc overwrites following native options in LLVM:
>>>> -pragma-unroll-threshold=4096 -unroll-allow-partial -simplifycfg-sink-common=false -amdgpu-vgpr-index-mode -filetype=obj

### File formats

```
<file>.vert     GLSL source text file for a vertex shader

<file>.tesc     GLSL source text file for a tessellation control shader

<file>.tese     GLSL source text file for a tessellation evaluation shader

<file>.geom     GLSL source text file for a tessellation geometry shader

<file>.frag     GLSL source text file for a tessellation fragment shader

<file>.comp     GLSL source text file for a tessellation compute shader

<file>.spv      SPIR-V binary file

<file>.spvasm   SPIR-V text file

<file>.pipe     Pipeline info file
```
> **Note:** To compile a GLSL source text file or a SPIR-V text (assembly) file,
or a Pipeline info file that contains or points to either of those, amdllpc needs to
call [spvgen](https://github.com/GPUOpen-Drivers/spvgen). The directory of the spvgen library
needs to be added to the environment variable `LD_LIBRARY_PATH`. Compiling SPIR-V binary
or a Pipeline info file that contains or points to SPIR-V binary does not require spvgen.

### Examples

* Compile single fragment shader "a.frag" on Vega10
```
amdllpc -auto-layout-desc -gfxip=9.0.0 a.frag
```

* Compile full pipeline file "b.pipe" on Ellesmere and output to c.elf
```
amdllpc -gfxip=8.0.3 -o=c.elf b.pipe
```
