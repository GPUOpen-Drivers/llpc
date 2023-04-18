##
 #######################################################################################################################
 #
 #  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

# -*- Python -*-

import subprocess

import lit.formats
import lit.util

from lit.llvm import llvm_config
from lit.llvm.subst import ToolSubst
from lit.llvm.subst import FindTool

# Configuration file for the 'lit' test runner.

# name: The name of this test suite.
config.name = 'LLPC_SHADERTEST'

# testFormat: The test format to use to interpret tests.
config.test_format = lit.formats.ShTest(not llvm_config.use_lit_shell)

# suffixes: A list of file extensions to treat as test files.
config.suffixes = ['.vert', '.tesc', '.tese', '.geom', '.frag', '.comp', '.spvasm', '.pipe', '.ll', '.multi-input']

# excludes: A list of directories and files to exclude from the testsuite.
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

if (not config.llpc_client_interface_version.isdigit()) or (int(config.llpc_client_interface_version) > 52):
    config.available_features.add('llpc-client-interface-53')

llvm_config.use_default_substitutions()

config.substitutions.append(('%PATH%', config.environment['PATH']))
config.substitutions.append(('%gfxip', config.gfxip))
config.substitutions.append(('%spvgendir%', config.spvgen_dir))

tool_dirs = [config.llvm_tools_dir, config.amdllpc_dir]

tools = ['amdllpc', 'llvm-objdump', 'llvm-readelf', 'not', 'count']

llvm_config.add_tool_substitutions(tools, tool_dirs)

#if LLPC_BUILD_NAVI31
# Propagate options for lit feature tests. These can be used in XFAIL, REQUIRES, and UNSUPPORTED
p = subprocess.Popen([config.amdllpc_dir + "/amdllpc", "-gfxip=11.0.0","dummy.comp"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
out,err = p.communicate()
check_str = out.decode("utf-8")
if check_str.find("Invalid gfxip: gfx1100") == -1:
    config.available_features.add('gfx11')
#endif

