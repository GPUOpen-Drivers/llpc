# -*- Python -*-

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
config.suffixes = ['.vert', '.tesc', '.tese', '.geom', '.frag', '.comp', '.spvas', '.pipe', '.ll']

# excludes: A list of directories  and fles to exclude from the testsuite.
config.excludes = ['CMakeLists.txt', 'litScripts', 'internal', 'avoid', 'error']

# test_source_root: The root path where tests are located.
config.test_source_root = os.path.dirname(__file__)

# test_exec_root: The root path where tests should be run.
config.test_exec_root = os.path.join(config.test_run_dir, 'test_output')

llvm_config.use_default_substitutions()

config.substitutions.append(('%PATH%', config.environment['PATH']))
config.substitutions.append(('%gfxip', config.gfxip))
config.substitutions.append(('%spvgendir%', config.spvgen_dir))

tool_dirs = [config.llvm_tools_dir, config.amdllpc_dir]

tools = ['amdllpc']

llvm_config.add_tool_substitutions(tools, tool_dirs)

