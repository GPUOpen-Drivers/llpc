# Continuations

A collection of passes to convert shaders to coroutines.
Some passes work on DXIL, some passes are generic.

This is supposed to be used as a submodule in a driver repository.

### Tests

Lit tests are behind the `check-continuations` CMake target, they can be run with `make check-continuations`.
