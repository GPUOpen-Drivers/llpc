# Support of NonSemantics.DebugPrintf

## Summary

The `NonSemantics.DebugPrintf` extended instruction enables `printf`-style
functionality in SPIR-V modules.

Khronos validation has a `debugPrintf` layer which implements the instruction by
hooking `vkCreateShaderModule`, among other things. However, we have quite a lot
of internal SPIR-V code (e.g. for ray tracing) which is never seen by any
Vulkan layers.

Our internal print debug capability allows us to use `printf` in internal
SPIR-V applications that are head-scratching hard to debug.

## Interfaces

This section describes interfaces between major components:

* Various components need to be aware of [format string identifiers](#format-string-identifiers)
  which are used to refer to format strings.

* The [printf buffer](#printf-buffer) is setup by the driver at runtime, written
  to by the shader to capture uses of the `DebugPrintf` extended instruction,
  and then read and interpreted by the driver afterwards.

* The [ELF extension](#elf-extension) describes how format strings and their
  identifiers are stored in PAL metadata.

### Format string identifiers

At runtime, the shader does not touch format strings. Each use of `DebugPrintf`
merely writes a numeric ID of the relevant format string to the printf buffer.

Format string IDs must be unique at least on a per-device (`Pal::IDevice`)
basis.

Format string IDs are 48-bit numbers.

Instead of attempting to coordinate the assignment of IDs between multiple
shader compilations, the compiler simply computes a hash of the format string
and uses that as the format string ID.

48-bit hashes can collide, but the likelihood is very low and acceptable for a
debug feature. When loading a pipeline, the driver should check whether an ID
has been seen before with a different corresponding format string and issue a
warning in that case.

To avoid having to coordinate hashing algorithms between different software
components, the compiler writes the ID to ELF metadata, and other components
(PAL, driver) use the ID as an opaque number. They need not be aware of the
hashing algorithm.

### ELF extension

Format strings are placed inside a new `amdpal.format_strings` map in the PAL
metadata. This new map is a sibling of the existing `amdpal.version` and
`amdpal.pipelines` entries.

The `amdpal.format_strings` map contains:

* An element with key `.version` whose value is the integer `1`.
* An element with key `.strings` whose value is an array of maps, each of
  which contains:
  * An element with key `.index` whose value is the format string identifier
    as an integer.
  * An element with key `.string` whose value is the format string.
  * An element with key `.argument_count` whose value is the number of arguments
    passed into `DebugPrintf`
  * An element with key `.64bit_arguments` whose value is an array of 64-bit
    integers that serve as a bitmask of arguments that are 64-bit sized

An example in JSON-ified form:
```json
{
    "amdpal.version": [ .. ],
    "amdpal.pipelines": [ .. ],
    "amdpal.format_strings": {
        ".version": 1,
        ".strings": [
            {
              ".index": 12345678,
              ".string": "Sample %i format %f",
              ".argument_count": 2,
              ".64bit_arguments": [0]
            },
            {
              ".index": 31415926,
              ".string": "Another format string: %f %f",
              ".argument_count": 2,
              ".64bit_arguments": [0]
            }
        ]
    }
}
```

### Printf buffer

The driver sets up a buffer into which `printf` invocations are logged. The
buffer contains an overall header that is initialized by the driver, followed
immediately by space into which buffer entries are written by the shader.

```c++
// The header is followed by entries. Reserved fields must be initialized to 0
// by the driver.
struct PrintfBufferHeader {
    // Offset into the buffer in dwords, **excluding** this header. Points at
    // the next free space. Note that only buffers up to 2GiB are supported;
    // if this value is larger, the buffer has overrun and anything beyond 2GiB
    // may be garbage.
    uint64_t bufferOffset;

    uint32_t reserved0;
    uint32_t reserved1;
};
static_assert(sizeof(PrintfBufferHeader) == 16, "header size changed");

// Each entry is followed by entry-specific payload. The driver uses the
// formatStringId to interpret the payload.
struct PrintfBufferEntryHeader {
    // Size of the entry in dwords, including this header.
    uint64_t entrySize : 16;

    // Identifies the format string that is used by this entry.
    uint64_t formatStringId : 48;
};
static_assert(sizeof(PrintfBufferEntryHeader) == 8, "entry header size changed");
```

The 64-bit address pointing at `PrintfBufferHeader` is passed to the shader via
a binding with:

* `set = InternalDescriptorSetId`
* `binding = InternalBinding::PrintfBufferBindingId`

**Payload packing:** Payload arguments to `NonSemantic.DebugPrintf` are packed
as follows:

* integer values are first zero-extended to be at least 32-bits wide and are
  then written with 4-byte alignment
* half-precision floating point values are widened to single-precision
  floating point values
* float and double values are written with 4-byte alignment
* vector values are written as a sequence of component scalar values

**Buffer overflow protection:** The shader uses a 64-bit atomic add on
`bufferOffset` to allocate space in the buffer and retrieve the offset at which
to write into the buffer. To prevent overwriting older data, the 64-bit value is
clamped to a value corresponding to at most 2GiB before using the clamped result
as an offset of `buffer_store` instructions.

We do _not_ guard against wraparound of `bufferOffset` itself ([rationale](#overflow-of-bufferoffset)).

**Format string safety:** If a shader's use of `DebugPrintf` is incorrect, it is
possible for the payload to be too short for the underlying format string. It is
up to the parser integrated in the driver to guard against such errors.

## Implementation

### Vulkan driver

During pipeline creation, the printf buffer descriptor is added to the pipeline
resource mapping.

Most of the implementation is in xgl/icd/api/debug_printf.cpp.

> **Note/Todo:** At time of writing, the implementation is limited to handling
> only a single pipeline at a time.

### LLPC

#### lgc: debug printf operation

The `@lgc.debug.printf` operation represents a printf call for debug purposes.
It is declared as follows:
```llvm
declare void @lgc.debug.printf(ptr addrspace(7) %buffer, ptr addrspace(4) %format, ...)
                               memory(inaccessible) nosync nounwind alwaysreturn
```
`%buffer` is a (fat) pointer to the printf buffer.

`%format` is the format string, which must point at a non-external global
constant of type `[n x i8]`.

Allowed types for the variable argument operands are:

* `i8, i16, i32, i64, half, float, double`
* Vectors of the above

#### SPIRVReader

The SPIRVReader emits the format string as a global variable in the constant
address space and produces a call to `@lgc.debug.printf`.

Example:
```llvm
  @debugString.0 = private addrspace(4) constant [11 x i8] c"the arg: %i", align 1

  ...
  call void (...) @lgc.debug.printf(ptr addrspace(7) %printfBuffer, ptr addrspace(4) @debugString.0, i32 %arg)
  ...
```

**Note:** Builder record/replay is not used.

#### lgc::LowerDebugPrintf

The module pass `LowerDebugPrintf` runs just before `PatchEntryPointMutate`.
It collects all calls to `@lgc.debug.printf` in the entire module and:

* Collects the format strings and adds the `amdpal.format_strings` entry to the
  PAL metadata document.
* Lowers the calls to the required lower-level instructions.

#### Linker support

The LLPC-internal ELF linker must be able to:

* Combine the `amdpal.format_strings.strings` arrays from all source ELF files.

> **Note/Todo:** This is currently not implemented.

## Appendix

### Format string identifier rationales

We use 48-bit identifiers to keep the printf buffer entry encoding reasonably
compact.

Two alternatives were considered:

We considered assigning IDs after compilation, using ELF relocation. This
alternative was rejected because it is more complicated.

We also considered splitting the identifier namespace into (pipeline/module,
string) pairs at the architectural level. This introduces a number of
complications:

* Require either a fixed bit split which may end up causing surprising
  limitations, or grow the ID to at least 64 bits.
* Additional implementation cost for partial pipeline compilation / partial
  pipeline caching. Consider vertex and fragment shaders that both use
  `DebugPrintf`, are first compiled separately, and then linked together in the
  compiler. We would either have to re-assign string IDs during the static
  linking in the compiler or extend the metadata format such a single `.elf`
  can define multiple "modules".

In contrast, the genuine benefits of such a split are unclear:

* Allocation of IDs in the driver may be cheaper. However, for the debugging
  scenario, a simple bump allocator is most likely sufficient anyway.
* Tying an ID back to a pipeline when parsing the printf buffer is possible
  either way by storing a link back to the pipeline together with the format
  string.

Therefore, we currently do not propose such a split.

### Overflow of bufferOffset

It is theoretically possible for `PrintfBufferHeader::bufferOffset` to
overflow. Once we've overflown the buffer, increments to `bufferOffset` are
no longer limited by the external memory bandwidth; they're only limited by how
fast we can do the atomic adds.

Assuming a maximum entry size of `128 B`, a wave may attempt to allocate up to
`8 kB` in a single atomic transaction (one chunk of `128 B` per lane). As one
atomic transaction can be handled per clock, this means that `~2**44` bytes can
be allocated per second, which would lead to a wraparound in `~2**20` seconds,
or just over 12 days. It seems safe to ignore this overflow scenario.

