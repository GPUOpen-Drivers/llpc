# LGC (LLPC middle-end) overview

LGC ("LLVM-based graphics compiler", or "LLPC generator core") is the middle-end part of
LLPC. This document gives an overview of the interface that LGC presents to the front-end
of LLPC, and of the data structures it uses internally.

See
[LLPC overview](../../llpc/docs/LlpcOverview.md)
for the context of LGC within LLPC.

## lgc command-line tool

The `lgc` command-line tool allows exercising LGC functionality of the
[different supported compilation modes](#support-for-shader-part-pipeline-and-whole-pipeline-compilation).

It also allows running individual passes or custom pass pipelines, similar to LLVM's `opt` tool.
However, beware that LGC currently still has some outside-IR state that is not serialized,
and so this does not work with all passes.

## LGC interface classes

This section outlines the classes that are exposed by LGC to the front-end.

The input to LGC is not defined as a particular form of intermediate language. Rather,
the interface is LLVM IR plus the methods in LGC's interface classes used to insert code
and add metadata.

### BuilderContext

An object of this class is created first, and it spans multiple compiles. It contains state
that spans multiple compiles: information on the target GPU. It has an LLVMContext attached,
and, like LLVMContext, you cannot use the same one for concurrent compiles.

BuilderContext provides methods to create Pipeline and Builder.

### Pipeline

A Pipeline object represents a single pipeline compilation. It is not created at all for a single
shader compilation. It contains methods to
* set pipeline state, including pipeline and per-shader tuning options;
* link individual shader IR modules into the pipeline module;
* generate (run the middle-end and back-end parts of the compilation process).

### Builder

Builder is a subclass of LLVM's IRBuilder<>, so the front-end can use all of its methods to
generate IR at the current insertion point and with the current debug location and (in some cases)
fast math flags.

In addition, Builder contains methods to generate IR for graphics-pipeline-specific constructs,
such as buffers, images, and shader inputs and outputs.

Builder also contains methods to set "shader modes", that is, additional modes specified in
the shader input language (SPIR-V) such as floating point mode, tessellation mode, workgroup
size, etc. These are set via Builder rather than Pipeline because they need to be set even
in the case of compiling a single shader; in that case the modes are saved in its IR for future
linking and generating.

Builder now only supports "BuilderRecorder" mode,
where methods to generate IR for graphics-pipeline-specific constructs are
"recorded" in the IR by a call to `@lgc.call.*` for each one, and then replayed at the start
of the middle-end using the same methods in BuilderImpl.

### Typical front-end flow

There are two different ways of compiling:

* A pipeline compile, where the front-end compiler has the pipeline state and the individual shaders
  at the same time.

* A shader compile, where the front-end compiler has only one shader, without yet knowing how it relates
  to any pipelines. Here, the front-end compiles the shader using Builder, and then, later on, there
  is a pipeline compile where the input shaders are IR from the individual shader compiles.

The front-end flow is:

* Call `BuilderContext::Initialize`. The first time this is called must be before LLVM command-line
  processing, and before the first call to `BuilderContext::Create`. The front-end is allowed to call
  `BuilderContext::Initialize` multiple times.

* Process command-line options with `cl::ParseCommandLineOptions`. This can only be done once in the
  whole process.

* Create a BuilderContext using `BuilderContext::Create`, passing in
  - an LLVMContext to attach to it that the front-end created in some way; and
  - the GPU name string (e.g. "gfx900").
  A BuilderContext can, and should, be shared
  between multiple compiles, although not concurrent compiles.

* The following steps are per-compile.

* For a pipeline compile, use `BuilderContext::CreatePipeline` to create a Pipeline object,
  then use its methods to pass pipeline state into it:
  - `SetShaderStageMask`
  - `SetOptions` (to set pipeline tuning options, including NGG options)
  - `SetShaderOptions` (to set per-shader tuning options)
  - `SetUserDataNodes`
  - `SetDeviceIndex`
  - `SetVertexInputDescriptions`
  - `SetColorExportState`
  - `SetGraphicsState` (to set input assembly, viewport and rasterizer state)

* Use `BuilderContext::CreateBuilder` to create a Builder object, passing the Pipeline (or nullptr
  if this is a shader compile).

* For each shader stage (when not doing a pipeline compile where the shaders are the result of earlier
  shader compiles):
  - First use `Builder::Set*ShaderMode` calls to set shader modes (FP modes, useSubgroupSize flag,
    tessellation modes, geometry modes, fragment modes, workgroup size) that come from
    the input language.
  - Then create or process an IR module, using Builder calls to generate new IR.
  - For a shader compile, call `Builder::RecordShaderModes` to ensure the modes are recorded
    into IR.
  - Where the front-end creates a pass manager for its own passes, it should call
    `BuilderContext::PreparePassManager` to ensure it is suitably set up. It can optionally
    use `PassManager::Create` to create the middle-end's subclass pass manager instead of
    using the standard `PassManager`.
  - For a shader compile, you're done: The IR module at this point is the result of
    the compile, and can be stored away and kept for later linking with other shaders.

* Call `Pipeline::Link` to link the shader IR modules into a pipeline IR module. (This needs to be
  done even if the pipeline only has a single shader, such as a compute pipeline.)
  This also records the pipeline state into IR metadata.

* Call `Pipeline::Generate` to run middle-end and back-end passes and generate the ELF.
  (Global options such as `-filetype` and `-emit-llvm` can cause the output to be something other than ELF.)
  The front-end can pass a call-back function into `Pipeline::Generate` to check a shader cache
  after input and output mapping, and elect to remove already-cached shaders from the pipeline.

### Support for shader, part-pipeline and whole pipeline compilation

#### Whole pipeline compilation

The default setup for LGC, and the LLPC front-end that uses it, is whole pipeline compilation.
The front-end gives the IR modules for all the shaders in the pipeline, and the pipeline state,
to the `Lgc::Pipeline` object, and then asks it to link and generate them into a single pipeline ELF,
ready for consumption by the PAL ELF loader.

There are a number of variations on that, mostly specified by the `pipelineLink` arg to `Pipeline::irLink()`:

#### Unlinked shader compilation

The front-end can use a scheme where it compiles each shader separately, possibly without the pipeline
state or with incomplete pipeline state, and later links the resulting ELF files with the pipeline state
to create the pipeline ELF. This is only supported for compute, vertex and fragment shaders (where
vertex and fragment shaders are used in a VS-FS pipeline, with no tessellation or geometry shaders).

The advantage of this is that a single shader used in multiple pipelines could be only compiled once, and
could be retrieved from a cache for other occurrences. Further, that cache could be pre-populated offline.
The implementation of those features is in the front-end, not in LGC.

In this scheme, LGC uses relocs for certain values that cannot be determined without the pipeline state,
such as the offset of a descriptor table in user data, and the offset of a descriptor within its
descriptor table. The LGC ELF linker resolves such relocs with reference to the pipeline state.
In addition, if vertex layout information is not available when compiling the VS, then the LGC ELF linker glues
a "fetch shader" on to the front, and, similarly, if color export information is not available when
compiling the FS, then the LGC ELF linker glues an "export shader" on to the end. Compilation of each
shader places metadata into the PAL metadata .note record for the linker to generate these glue shaders.

This scheme has some restrictions on what the pipeline is allowed to do. Most notably, if resource mapping
(the layout of user data and descriptor tables) is not available at shader compile time, then it is not
allowed to have a compact (2-dword) descriptor, or a static sampler.
Also, it seems like some built-in inputs in the FS
would not work, if they are implemented as generic inputs that need to be exported by the VS.

#### Old partial pipeline compilation scheme

LGC has a hook part way through compilation, just after determining inter-shader output/input packing,
in which it calls a callback function (provided by the front-end as the `checkShaderCacheFunc` arg to
`Pipeline::generate` method), providing a hash of input/output usage and mapping for each shader stage,
and allowing the callback to clear bits in the shader stage mask to disable the continued compilation of
part of the pipeline.

The LLPC front-end uses this to implement a partial pipeline compilation scheme. Considering the two
parts of a graphics pipeline (the non-FS part (VS,TCS,TES,GS) and the FS), it uses the LGC input/output
usage and mapping hashes, in conjunction with its own hashes of the shader stages, to determine whether
it has compiled the same partial pipeline with the same input/output usage before. The ELF result of
compilation is added to its cache for future compiles to find.

To do this, the LLPC front-end has to extract the partial pipeline it wants from a pipeline ELF, then
link two partial pipelines together. It does this with its own ELF extracting and linking code, not
using the LGC ELF linker.

#### New part-pipeline compilation scheme

LGC supports another scheme, similar in concept but not needing compilation of the whole pipeline
to proceed to a cache-checking point, and allowing use of the LGC ELF linker.

This scheme is based on the observation that you can compile the FS without reference to other shaders
(but with the pipeline state), and it can still pack its inputs (i.e. not reserve an attribute in the
attribute space if it never uses it), as long as the compile of the other part of the pipeline gets
the information about that packing. This works because packing is done backwards through the shader
stages, packing a shader's inputs and then fixing the previous shader's outputs to match.

The FS input information also includes where the FS uses a built-in that needs to be exported from the VS.

The scheme is intended to be used by the front-end as follows:

* First, the front-end creates a `Pipeline` object for the whole pipeline, and an `ElfLinker` using it,
  and sets the parts of the pipeline state needed for linking at the end.

* The front-end calculates a hash of the FS and the pipeline state that is relevant to the FS, and
  checks its shader cache for the same shader-and-state being compiled before. If not found, it
  compiles it and stores it in the cache.

* Compiling the FS works as normal, using the LGC interface to create a `Pipeline` object (not the same
  as the one created above for the whole pipeline), give it
  relevant pipeline state, front-end compile the shader with `Builder` calls, call `Pipeline::irLink`
  with just the FS and with `pipelineLink` set to `PipelineLink::PartPipeline`. The `Pipeline::generate`
  method then middle-end and back-end compiles it and generates an ELF.

* The front-end calculates a hash of the rest of the shaders and pipeline state relevant to them,
  and includes in that hash the FS input mapping. It does the latter by adding the FS ELF (whether
  just-compiled or retrieved from cache) to the `ElfLinker` and calling `ElfLinker::getFsInputMappings()`,
  which returns an opaque representation of the FS input mappings to include in the hash.
  Then it can check the cache.

* If the non-FS part of the pipeline was not found in the cache, then the front-end compiles it as
  normal (without the FS), in its own `Pipeline` object, but additionally calling
  `Pipeline::setOtherPartPipeline()` passing the whole-pipeline `Pipeline` object, so the compile
  has access to the FS input mappings metadata. This compile generates an ELF for the non-FS part-pipeline.

* Now the front-end adds the ELF for the non-FS part-pipeline (whether compiled or retrieved from its
  cache) to the ELF linker, and tells it to link them, resulting in a pipeline ELF.

This scheme is experimental, and is not yet in use by the LLPC front-end.

## LGC internal classes

### TargetInfo

BuilderContext stores information on the exact GPU being targeted in TargetInfo. This information
does not change between multiple compiles with the same BuilderContext.
TargetInfo can be accessed from BuilderContext, but middle-end code would typically access
it via PipelineState.

### BuilderRecorder

The Builder created by `BuilderContext::CreateBuilder` is now always a BuilderRecorder. For each Create
method, such as `CreateImageSample`, BuilderRecorder records the call in the IR at the insertion
point as a call to an `lgc.call.*` method, in this example `lgc.call.image.sample`. Such a
call is a varargs call, so BuilderRecorder only needs to overload the declarations by return
type, not by arg types.

Then, the first pass run in `Pipeline::Generate` is the BuilderReplayer pass. That pass
creates its own BuilderImpl, and replays each recorded call into it, generating the same
IR as would have been generated if the front-end had chosen to use BuilderImpl directly.

### BuilderImpl

When running the BuilderReplayer
pass, the Builder is an instance of BuilderImpl. That generates IR more directly for
Builder methods such as `CreateImageSample`. BuilderImpl multiple-inherits from a bunch of classes
that implement the different categories of Builder calls (arithmetic, descriptors, image,
shader input/output, matrix, misc, subgroup), and each of those is a subclass of BuilderImplBase,
a subclass of Builder.

### PipelineState

The Pipeline object created by the front-end calling `BuilderContext::CreatePipeline` is in
fact an instance of PipelineState, an LGC-internal subclass of Pipeline. PipelineState contains
the outside-IR state used through the middle-end.

BuilderImpl code can access PipelineState directly, because BuilderImpl has a pointer to it.

Middle-end passes (patch passes) access PipelineState via the PipelineStateWrapper pass.
PipelineStateWrapper is an immutable pass, which is like a module pass but is assumed by LLVM's
pass manager to never be invalidated.

PipelineState stores two kinds of outside-IR state:

* The state set by the front-end calling methods in Pipeline, such as the vertex input descriptions.
  This state gets written into the IR as metadata
  in `PipelineState::Link`, and read out the first time PipelineState is used in the middle-end.
  Thus this kind of state can be considered a cache of information that is in IR metadata, and
  in theory could be re-read if necessary at any point in the pass flow, although that does not
  actually happen. Where this would happen is if a command-line compiler writes out the IR
  after completing the front-end, and then a separate command invocation runs the middle-end.

* The second kind of outside-IR state is the state set in middle-end passes (and BuilderImpl).
  This is `ResourceUsage` and `InterfaceData`, which are declared in their own separate file
  `ResourceUsage.h`.
  This state does not get written into IR metadata, which currently prevents us from testing some
  passes individually. We should remove this limitation by adding a facility to write the state
  into IR metadata and read it back.

### ShaderModes

ShaderModes contains state that is provided as part of the shader's input language, and so is
present even for a shader compile, when there is no pipeline state. ShaderModes state contains:

* per-shader FP denormal and rounding modes;
* tessellation mode (from both tessellation shaders);
* geometry mode (from geometry shader);
* fragment mode (from fragment shader);
* compute workgroup size.

When the front-end calls Builder methods to set shader modes, the information ends up in a ShaderModes
object. In a shader compile, there is no PipelineState, and ShaderModes lives inside BuilderRecorder.
For a pipeline compile, ShaderModes lives inside PipelineState, and middle-end code accesses it
with `PipelineState::GetShaderModes`.
