Detailed Design Note: Tracking inter-shader data exchange for shader caching
============================================================================

**Status:** draft

LLPC performs full pipeline optimizations of shader pipelines, such as eliminating unused outputs from a shader stage.
LLPC also enables caching of individual shaders _after_ those full pipeline optimizations have been performed.

When checking each individual shader against the shader cache, the information that affected its compilation but was
obtained from other shaders must be taken into account. There are essentially two ways of doing so:

  1. Serialize the entire shader into a binary format (LLVM bitcode) and hash the result.

  2. Combine the shader's input hash (and relevant input parts of the pipeline creation info) with a log of all
     inter-shader data that was used to make decisions during the compilation of the shader so far.

There are various trade-offs between these two approaches. In LLPC, we choose the second one and implement it as
described in this note.


Historical Background
---------------------

At the time of writing, LLPC already uses variant 2 described in the introduction. It does so by combining various
pieces of information that have been saved off in a look-aside data structure (`ResourceUsage`) in the
`PatchCheckShaderCache` pass.

This approach has two downsides:

  1. All this information must be carried in the look-aside data structure at least until this late pass, even though
     the information may otherwise be obsolete.

  2. Knowledge about what kind of full pipeline optimizations are applied, including some of their details, is
     centralized in `PatchCheckShaderCache`. This limits the design's extensibility.


Implementation overview
-----------------------

Every function in LLPC gets an attached `!llpc.hash` metadata node.
This metadata node is initialized with the relevant input shader hash.
Every pass that performs inter-shader transforms updates the metadata by hashing the old hash together with any data
that is relevant from other shaders, i.e. it computes `h_new = h(h_old | inter-shader data)`.
The metadata node is finally inspected in the `PatchCheckShaderCache` pass.


Extensible specialized metadata
-------------------------------

We will use this feature as a test balloon for extensible specialized metadata nodes in LLVM core, a feature we would
then be able to use for a great many graphics-specific features such as pipeline information and descriptor binding
information.

Specialized metadata nodes have been introduced for debug info in LLVM core. They look like:
```
!0 = !DIFile(filename: "path/to/file", directory: "/path/to/dir",
             checksumkind: CSK_MD5,
             checksum: "000102030405060708090a0b0c0d0e0f")
```
In C++, they are represented using custom C++ classes that are defined in LLVM core.

Our goal is to be able to use the same syntax in LLVM IR and to also be able to represent this metadata using a custom
C++ class. The challenge is two-fold:

  1. We need to allow LLVM-only tools to still round-trip our version of LLVM IR that is augmented with this
     custom metadata. This applies to both textual LLVM IR and LLVM bitcode.

  2. Our custom C++ class has to live in LLPC and therefore outside LLVM proper, and yet the LLVM texture IR parser,
     printer, and the LLVM bitcode reader and writer have to support the custom class.

The plan is to define an LLVMDialect abstract base class in LLVM core. Dialects can be registered with an LLVMContext,
and all of the context's associated printing, parsing, reading and writing activity will then consult the dialect to
determine whether some metadata object is to be interpreted via a custom C++ class.

The notion of a "dialect" then provides a beachhead for further extensibility. Most importantly for LLPC, this
includes the ability to register additional intrinsic functions and build infrastructure for automatically generating
C++ facade classes for interacting with calls to those intrinsic functions as if they were native instructions.
This has at least the following benefits:

  1. We will be able to use checks such as `isa<InputImportInst>` instead of performing a string comparison
     against `"llpc.input.import."`.

  2. We will be able to use more descriptive functions such as `InputImportInst::getLocation()` instead of
     hard-coding operand indices.

  3. Ideally, we'd have infrastructure that also generates corresponding builder calls automatically, such as exists
     in MLIR.
