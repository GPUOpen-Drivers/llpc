# LGC (LLPC middle-end) overview

LGC ("LLVM-based graphics compiler", or "LLPC generator core") is the middle-end part of
LLPC. This document gives an overview of the interface that LGC presents to the front-end
of LLPC, and of the data structures it uses internally.

See
[LLPC overview](../../llpc/docs/LlpcOverview.md)
for the context of LGC within LLPC.

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

The front-end decides which one of two modes Builder works in, although which mode is selected
does not otherwise affect the front-end:

* BuilderRecorder, where methods to generate IR for graphics-pipeline-specific constructs are
  "recorded" in the IR by a call to `@llpc.call.*` for each one, and then replayed at the start
  of the middle-end;

* BuilderImpl, where those same methods generate IR more directly.

If doing a single shader compile, BuilderRecorder is always used.

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
  if this is a shader compile). For a pipeline compile, the front-end can choose here whether to use
  BuilderRecorder (record Builder calls into IR for later replay) or the direct Builder implementation.
  For a shader compile, BuilderRecorder is always used.

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
    using the standard `legacy::PassManager`.
  - For a shader compile, you're done: The IR module at this point is the result of
    the compile, and can be stored away and kept for later linking with other shaders.

* Call `Pipeline::Link` to link the shader IR modules into a pipeline IR module. (This needs to be
  done even if the pipeline only has a single shader, such as a compute pipeline.)
  If using BuilderRecorder, this also records the pipeline state into IR metadata.

* Call `Pipeline::Generate` to run middle-end and back-end passes and generate the ELF.
  (Global options such as `-filetype` and `-emit-llvm` can cause the output to be something other than ELF.)
  The front-end can pass a call-back function into `Pipeline::Generate` to check a shader cache
  after input and output mapping, and elect to remove already-cached shaders from the pipeline.

## LGC internal classes

### TargetInfo

BuilderContext stores information on the exact GPU being targeted in TargetInfo. This information
does not change between multiple compiles with the same BuilderContext.
TargetInfo can be accessed from BuilderContext, but middle-end code would typically access
it via PipelineState.

### BuilderRecorder

If the front-end selects BuilderRecorder mode for Builder (or it is a shader compile), then
the Builder created by `BuilderContext::CreateBuilder` is a BuilderRecorder. For each Create
method, such as `CreateImageSample`, BuilderRecorder records the call in the IR at the insertion
point as a call to an `llpc.call.*` method, in this example `llpc.call.image.sample`. Such a
call is a varargs call, so BuilderRecorder only needs to overload the declarations by return
type, not by arg types.

Then, the first pass run in `Pipeline::Generate` is the BuilderReplayer pass. That pass
creates its own BuilderImpl, and replays each recorded call into it, generating the same
IR as would have been generated if the front-end had chosen to use BuilderImpl directly.

### BuilderImpl

If the front-end selects BuilderImpl mode for Builder, or when running the BuilderReplayer
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
  For a pipeline compile using BuilderRecorder, this state gets written into the IR as metadata
  in `PipelineState::Link`, and read out the first time PipelineState is used in the middle-end.
  Thus this kind of state can be considered a cache of information that is in IR metadata, and
  in theory could be re-read if necessary at any point in the pass flow, although that does not
  actually happen. Where this would happen is if a command-line compiler writes out the IR
  after completing the front-end, and then a separate command invocation runs the middle-end.

* The second kind of outside-IR state is the state set in middle-end passes (and BuilderImpl).
  This is `ResourceUsage` and `InterfaceData`, which are declared in their own separate file
  `ResourceUsage.h`.
  This state does not get written into IR metadata. In theory we could add a facility to write
  the state into IR metadata and read it back, and that would allow us to support lit testing of
  individual middle-end passes with `-stop-before` and `-run-pass` options like `llc`. But
  that does not happen now.

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

