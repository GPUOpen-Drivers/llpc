# Tests

This repository contains amber scripts to test changes and protect from
regressions.

## Amber

The `check-amber` CMake target runs the amber scripts in this directory and
checks the CHECK lines against the dumped pipelines for each script.

The `run_amber.py` script runs all amber scripts inside the `amber` directory
without checking the FileCheck lines with dumps.
This needs a GPU with a working Vulkan driver and amber inside the `PATH`.
The Vulkan driver can be set e.g. via `VK_ICD_FILENAMES`.

The [amber readme](https://github.com/google/amber) has instructions and
examples on how to write amber scripts. It also contains a
[specification](https://github.com/google/amber/blob/main/docs/amber_script.md)
of the script format.
