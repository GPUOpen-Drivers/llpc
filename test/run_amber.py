#!/usr/bin/env python
# Copyright 2018 The Amber Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import base64
import difflib
import optparse
import os
import platform
import re
import subprocess
import sys
import tempfile

SUPPRESSIONS = {
  "Darwin": [
  ],
  "Linux": [
  ],
  "Win": [
  ]
}

DEBUGGER_CASES = [
]

DXC_CASES = [
  "draw_triangle_list_hlsl.amber",
  "relative_includes_hlsl.amber",
  "debugger_hlsl_basic_compute.amber",
  "debugger_hlsl_basic_fragment.amber",
  "debugger_hlsl_basic_vertex.amber",
  "debugger_hlsl_shadowed_vars.amber",
  "debugger_hlsl_basic_fragment_with_legalization.amber",
  "debugger_hlsl_basic_vertex_with_legalization.amber",
  "debugger_hlsl_function_call.amber",
  "debugger_hlsl_shadowed_vars.amber",
]

class TestCase:
  def __init__(self, input_path, parse_only, use_dxc, test_debugger):
    self.input_path = input_path
    self.parse_only = parse_only
    self.use_dxc = use_dxc
    self.test_debugger = test_debugger

    self.results = {}

  def IsExpectedFail(self):
    fail_re = re.compile('^.+[.]expect_fail[.][amber|vkscript]')
    return fail_re.match(self.GetInputPath())

  def IsSuppressed(self):
    system = platform.system()

    base = os.path.basename(self.input_path)
    is_dxc_test = base in DXC_CASES
    if not self.use_dxc and is_dxc_test:
      return True

    is_debugger_test = base in DEBUGGER_CASES
    if not self.test_debugger and is_debugger_test:
      return True

    if system in SUPPRESSIONS.keys():
      is_system_suppressed = base in SUPPRESSIONS[system]
      return is_system_suppressed

    return False

  def IsParseOnly(self):
    return self.parse_only

  def GetInputPath(self):
    return self.input_path

  def GetResult(self, fmt):
    return self.results[fmt]

class TestRunner:
  def RunTest(self, tc):
    print("Testing {}".format(tc.GetInputPath()))

    # Amber and SwiftShader both use the VK_DEBUGGER_PORT environment variable
    # for specifying the Debug Adapter Protocol port number.
    # This needs to be set before creating the Vulkan device.
    # We remove this key from the environment if the test is not a debugger test
    # so that full SPIR-V optimizations are preserved.
    is_debugger_test = os.path.basename(tc.GetInputPath()) in DEBUGGER_CASES
    if is_debugger_test:
      os.environ["VK_DEBUGGER_PORT"] = "19020"
    elif "VK_DEBUGGER_PORT" in os.environ:
      del os.environ["VK_DEBUGGER_PORT"]

    cmd = [self.options.test_prog_path, '-q']
    if tc.IsParseOnly():
      cmd += ['-p']
    cmd += [tc.GetInputPath()]

    try:
      err = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
      if len(err) != 0 and not tc.IsExpectedFail() and not tc.IsSuppressed():
        sys.stdout.write(err.decode('utf-8'))
        return False

    except Exception as e:
      if not tc.IsExpectedFail() and not tc.IsSuppressed():
        print("{}".format("".join(map(chr, bytearray(e.output)))))
        print(e)
      return False

    return True

  def RunTests(self):
    for tc in self.test_cases:
      result = self.RunTest(tc)

      if tc.IsSuppressed():
        self.suppressed.append(tc.GetInputPath())
      else:
        if not tc.IsExpectedFail() and not result:
          self.failures.append(tc.GetInputPath())
        elif tc.IsExpectedFail() and result:
          print("Expected: " + tc.GetInputPath() + " to fail but passed.")
          self.failures.append(tc.GetInputPath())

  def SummarizeResults(self):
    if len(self.failures) > 0:
      self.failures.sort()

      print('\nSummary of Failures:')
      for failure in self.failures:
        print(failure)

    if len(self.suppressed) > 0:
      self.suppressed.sort()

      print('\nSummary of Suppressions:')
      for suppression in self.suppressed:
        print(suppression)

    print('')
    print('Test cases executed: {}'.format(len(self.test_cases)))
    print('  Successes:  {}'.format((len(self.test_cases) - len(self.suppressed) - len(self.failures))))
    print('  Failures:   {}'.format(len(self.failures)))
    print('  Suppressed: {}'.format(len(self.suppressed)))
    print('')

  def Run(self):
    base_path = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

    usage = 'usage: %prog [options] (file)'
    parser = optparse.OptionParser(usage=usage)
    parser.add_option('--test-dir',
                      default=os.path.join(os.path.dirname(__file__), 'amber'),
                      help='path to directory containing test files')
    parser.add_option('--test-prog-path', default=None,
                      help='path to program to test')
    parser.add_option('--parse-only',
                      action="store_true", default=False,
                      help='only parse test cases; do not execute')
    parser.add_option('--use-dxc',
                      action="store_true", default=False,
                      help='Enable DXC tests')
    parser.add_option('--test-debugger',
                      action="store_true", default=False,
                      help='Include debugger tests')

    self.options, self.args = parser.parse_args()

    if self.options.test_prog_path == None:
      self.options.test_prog_path = 'amber'

    input_file_re = re.compile('^.+[\.](amber|vkscript)')
    self.test_cases = []

    if self.args:
      for filename in self.args:
        input_path = os.path.join(self.options.test_dir, filename)
        if not os.path.isfile(input_path):
          print("Cannot find test file '{}'".format(filename))
          return 1

        self.test_cases.append(TestCase(input_path, self.options.parse_only,
            self.options.use_dxc, self.options.test_debugger))

    else:
      for file_dir, _, filename_list in os.walk(self.options.test_dir):
        for input_filename in filename_list:
          if input_file_re.match(input_filename):
            input_path = os.path.join(file_dir, input_filename)
            if os.path.isfile(input_path):
              self.test_cases.append(
                  TestCase(input_path, self.options.parse_only,
                      self.options.use_dxc, self.options.test_debugger))

    self.failures = []
    self.suppressed = []

    self.RunTests()
    self.SummarizeResults()

    return len(self.failures) != 0

def main():
  runner = TestRunner()
  return runner.Run()

if __name__ == '__main__':
  sys.exit(main())
