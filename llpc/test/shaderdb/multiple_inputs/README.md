# Multiple Input Tests

These [LIT regressions tests](https://llvm.org/docs/CommandGuide/lit.html) consist of
multiple input files passed to `amdllpc`.

Unlike most LIT shaderdb tests, the test file (`%s`) is not the only input for `.multi-input`
tests in this directory. The additional input files are placed in the
[multiple_inputs](multiple_inputs) directory. Each such additional input file is tested in
isolation to make sure it compiles.
