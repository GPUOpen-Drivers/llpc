#!/usr/bin/env python3

"""
spv-to-shaderdb-test.py -- Script to create a shaderdb test from spir-v assembly files.
Supports downloading files and unpacking archives (e.g., .zip).

Sample use:
1. Local file:
  script/spv-to-shaderdb-test.py tofile/shader.frag.asm -o llpc/test/shaderdb/fuzzer/test.spvasm

2. Zip archive from GitHub:
  script/spv-to-shaderdb-test.py https://github.com/GPUOpen-Drivers/llpc/files/5146499/tofile.zip \
    -o llpc/test/shaderdb/fuzzer/test.spvasm
"""

import glob
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from argparse import ArgumentParser

shadertest_prefix = r'''
; BEGIN_SHADERTEST
; RUN: amdllpc --verify-ir -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; XFAIL: *
; SHADERTEST-LABEL: {{^// LLPC.*}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
; END_SHADERTEST
;

'''
known_archive_extensions = ['.zip', '.tar', '.tar.gz', '.tar.xz']

def generate_shaderdb_test(input_spvasm, output_path):
  with open(input_spvasm, 'r') as input_file:
    spvasm = input_file.read()
    with open(output_path, 'w') as shadertest:
      shadertest.write(shadertest_prefix)
      shadertest.write(spvasm)

def main():
  parser = ArgumentParser()
  parser.add_argument('input', help='Input file (.spvasm/.asm local file, or .zip url)')
  parser.add_argument('-o', '--output', type=str,
                      help='Output shaderdb test file path')
  args = parser.parse_args()

  tmp_dir = tempfile.mkdtemp()
  local_filename = args.input
  if local_filename.startswith('http'):
    # urllib doesn't preserve file extension. Do it manually so that we
    # can determine file format.
    local_filename = os.path.join(tmp_dir, os.path.basename(args.input))
    urllib.request.urlretrieve(args.input, local_filename)

  print(f'Processing: {local_filename}')
  local_file_ext = os.path.splitext(local_filename)[1]
  if local_file_ext in known_archive_extensions:
    shutil.unpack_archive(local_filename, tmp_dir)
    asm_files = glob.glob(os.path.join(tmp_dir, '**/*.asm'), recursive=True) + \
                glob.glob(os.path.join(tmp_dir, '**/*.spvasm'), recursive=True)

    if len(asm_files) != 1:
      print(f'Unexpected archive contents, potential spir-v assembly files: {asm_files}',
            file=sys.stderr)
      exit(3)
    local_filename = asm_files[0]
  elif local_file_ext not in ['.spvasm', '.asm']:
    print(f'Unsupported input file extension: {local_file_ext}', file=sys.stderr)
    exit(3)

  generate_shaderdb_test(local_filename, args.output)
  shutil.rmtree(tmp_dir)
  print(f'Saved the new shaderdb test as: {os.path.realpath(args.output)}')


if __name__ == '__main__':
  main()
