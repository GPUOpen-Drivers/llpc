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

# name: The name of this test suite.
config.name = 'LGC'

# testFormat: The test format to use to interpret tests.
config.test_format = lit.formats.ShTest(not llvm_config.use_lit_shell)

# suffixes: A list of file extensions to treat as test files. This is overridden
# by individual lit.local.cfg files in the test subdirectories.
config.suffixes = ['.lgc', '.txt']

# excludes: A list of directories to exclude from the testsuite. The 'Inputs'
# subdirectories contain auxiliary inputs for various tests in their parent
# directories.
config.excludes = ['Inputs', 'CMakeLists.txt', 'README.txt', 'LICENSE.txt']

# test_source_root: The root path where tests are located.
config.test_source_root = os.path.dirname(__file__)

# test_exec_root: The root path where tests should be run.
config.test_exec_root = os.path.join(config.llvm_obj_root, 'test')

# Tweak the PATH to include the tools dir.
llvm_config.with_environment('PATH', config.llvm_tools_dir, append_path=True)

#if LLPC_BUILD_NAVI31
# Propagate options for lit feature tests. These can be used in XFAIL, REQUIRES, and UNSUPPORTED
p = subprocess.Popen([config.llvm_tools_dir + "/lgc", "-mcpu=gfx1100","dummy.lgc"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
out,err = p.communicate()
check_str = out.decode("utf-8")
if check_str.find("'gfx1100' not recognized") == -1:
    config.available_features.add('gfx11')
#endif

llvm_config.use_default_substitutions()
