##
 #######################################################################################################################
 #
 #  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
 #
 #  Permission is hereby granted, free of charge, to any person obtaining a copy
 #  of this software and associated documentation files (the "Software"), to deal
 #  in the Software without restriction, including without limitation the rights
 #  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 #  copies of the Software, and to permit persons to whom the Software is
 #  furnished to do so, subject to the following conditions:
 #
 #  The above copyright notice and this permission notice shall be included in all
 #  copies or substantial portions of the Software.
 #
 #  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 #  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 #  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 #  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 #  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 #  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 #  SOFTWARE.
 #
 #######################################################################################################################

import lit.formats
import lit.util

from lit.llvm import llvm_config
from lit.llvm.subst import ToolSubst
from lit.llvm.subst import FindTool

# Configuration file for the 'lit' test runner.

# name: The name of this test suite.
config.name = 'LLPC_SHADERTEST'

# testFormat: The test format to use to interpret tests.
config.test_format = lit.formats.ShTest(True)

# suffixes: A list of file extensions to treat as test files.
config.suffixes = ['.vert', '.tesc', '.tese', '.geom', '.frag', '.comp', '.spvasm', '.pipe', '.ll']

# excludes: A list of directories  and fles to exclude from the testsuite.
config.excludes = ['CMakeLists.txt', 'litScripts', 'internal', 'avoid', 'error']

# test_source_root: The root path where tests are located.
config.test_source_root = os.path.dirname(__file__)

# test_exec_root: The root path where tests should be run.
config.test_exec_root = os.path.join(config.test_run_dir, 'test_output')

# Propagate options for lit feature tests. These can be used in XFAIL, REQUIRES, and UNSUPPORTED.
if config.llvm_assertions == 'ON' or config.llvm_assertions == '1':
    config.available_features.add('assertions')

if 'Address' in config.xgl_sanitizers:
    config.available_features.add('asan')

if 'Undefined' in config.xgl_sanitizers:
    config.available_features.add('ubsan')

if config.llpc_enable_shader_cache == 'ON' or config.llpc_enable_shader_cache == '1':
    config.available_features.add('llpc-shader-cache')

llvm_config.use_default_substitutions()

config.substitutions.append(('%PATH%', config.environment['PATH']))
config.substitutions.append(('%gfxip', config.gfxip))
config.substitutions.append(('%spvgendir%', config.spvgen_dir))

tool_dirs = [config.llvm_tools_dir, config.amdllpc_dir]

tools = ['amdllpc', 'llvm-objdump', 'llvm-readelf']

llvm_config.add_tool_substitutions(tools, tool_dirs)
