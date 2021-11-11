# -*- Python -*-

# Configuration file for the 'lit' test runner.

import os
import sys
import re
import platform
import subprocess

import lit.util
import lit.formats
from lit.llvm import llvm_config
from lit.llvm.subst import FindTool
from lit.llvm.subst import ToolSubst

sys.path.append(os.path.dirname(__file__))

from query_gfxip import query_gfxips

# name: The name of this test suite.
config.name = 'LLPC-AMBER'

# testFormat: The test format to use to interpret tests.
config.test_format = lit.formats.ShTest(not llvm_config.use_lit_shell)

# suffixes: A list of file extensions to treat as test files. This is overridden
# by individual lit.local.cfg files in the test subdirectories.
config.suffixes = ['.amber']

# excludes: A list of directories to exclude from the testsuite. The 'Inputs'
# subdirectories contain auxiliary inputs for various tests in their parent
# directories.
config.excludes = []

# test_source_root: The root path where tests are located.
config.test_source_root = os.path.dirname(__file__)

# test_exec_root: The root path where tests should be run.
config.test_exec_root = os.path.join(config.llvm_obj_root, 'test')

# Set features for available gpu hardware. They can be queried with REQUIRES, etc.
for gfxip in query_gfxips():
    config.available_features.add(gfxip)

# Tweak the PATH to include the tools dir.
llvm_config.with_environment('PATH', config.llvm_tools_dir, append_path=True)
llvm_config.with_environment('PATH', config.test_source_root, append_path=True)
# Contains the path for vulkan validation layers
llvm_config.with_system_environment('XDG_DATA_DIRS')

llvm_config.use_default_substitutions()
