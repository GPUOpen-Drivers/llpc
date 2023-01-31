#!/usr/bin/env python
##
 #######################################################################################################################
 #
 #  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

# Run an amber test and capture the generated pipelines. Print the pipelines to stdout, so they can be piped e.g. into
# FileCheck.
# Takes a .amber file as argument.
# Run with e.g. ./run_amber_test.py --icd ../build/amdvlk64.so test.amber
# Or with an installed driver: ./run_amber_test.py --icd /usr/lib/amdvlk64.so --icd-json /etc/vulkan/icd.d/amd_icd64.json test.amber

import json
import argparse
import os
import subprocess
import sys
import tempfile

def main():
  parser = argparse.ArgumentParser(description='Run an amber test and print generated pipelines to stdout')
  parser.add_argument('--icd',
                      help='Library that contains the Vulkan driver, e.g. amdvlk64.so')
  parser.add_argument('--icd-json',
                      help='JSON description of the Vulkan driver')
  parser.add_argument('file',
                      help='amber script to run')

  args = parser.parse_args()

  # Guess icd json filename to be amd_icd{64|32}.json near the amdvlk64.so
  with open(args.icd_json or os.path.join(os.path.dirname(args.icd), "amd_icd" +
      os.path.basename(args.icd).replace("amdvlk", "").replace(".so", "").replace(".dll", "") + ".json")) as f:
    icd = json.load(f)
  icd["ICD"]["library_path"] = args.icd

  with tempfile.NamedTemporaryFile(mode="w", suffix=".json") as icd_json_file:
    with tempfile.TemporaryDirectory() as tmp_dir:
      with open(os.path.join(tmp_dir, "amdVulkanSettings.cfg"), "w") as f:
        f.write("ShaderCacheMode,0\n")
        f.write("UsePalPipelineCaching,0\n")
        f.write("EnableInternalPipelineCachingToDisk,0\n")
        f.write("AllowVkPipelineCachingtoDisk,0\n")
        f.write("HardAssert,0\n")
        f.write("EnableLLPC,1\n")
        f.write("EnablePipelineDump,1\n")

      json.dump(icd, icd_json_file)
      icd_json_file.flush()
      os.environ["VK_ICD_FILENAMES"] = icd_json_file.name
      os.environ["AMD_CONFIG_DIR"] = tmp_dir
      os.environ["AMD_DEBUG_DIR"] = tmp_dir
      os.environ["HOME"] = tmp_dir
      dump_dir = os.path.join(tmp_dir, "spvPipeline")

      # Run amber
      cmd = ["amber"] + [args.file]
      res = subprocess.run(cmd)

      # Print pipeline dumps
      for f in os.listdir(dump_dir):
        if f.endswith(".pipe"):
          print("Dump " + f)
          with open(os.path.join(dump_dir, f)) as f:
            print(f.read())

      print("Dump End")

  return res.returncode

if __name__ == '__main__':
  sys.exit(main())
