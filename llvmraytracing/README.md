# llvmraytracing

A library to implement ray tracing pipeline compilation using coroutine transforms.

## Details

Ray tracing shaders are expected in the `lgc.rt` dialect.

We first lower ray tracing, using the GPURT library which provides implementations for intrinsics to be inlined, and
standalone driver shaders such as the Traversal shader. The result is in `lgc.cps` dialect form, using await and enqueue
operations to represent indirect function calls, indirect tail calls, and returns.

Then, we use the LLVM coroutine infrastructure for lowering awaits, splitting functions and introducing resume functions.

At the end, we apply some cleanups to prepare IR for the backend.

## Tests

Lit tests are behind the `check-llvmraytracing` CMake target.
