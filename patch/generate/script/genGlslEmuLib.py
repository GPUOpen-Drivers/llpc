##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2018 Advanced Micro Devices, Inc. All Rights Reserved.
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

import binascii
import os
import re
import subprocess
import sys

import genGlslArithOpEmuCode
import genGlslGroupOpEmuCode
import genGlslImageOpEmuCode

def bin2hex(libFile, hFile):
  print(">>>  (LL-bin2hex) " + libFile + "  ==>  " + hFile)
  fBin = open(libFile, "rb")
  binData = fBin.read()
  fBin.close()

  hexData = binascii.hexlify(binData).decode()
  with open(hFile, "w") as fHex:
    i = 0
    while i < len(hexData):
        fHex.writelines(["0x", hexData[i], hexData[i + 1]])
        i += 2
        if (i != len(hexData)):
            fHex.write(", ")
        if (i % 32 == 0):
            fHex.write("\n")

# The directory where all the auxiliary and input files can be found
# (/llpc/patch/generate/).
INPUT_DIR = os.path.abspath(os.path.join(os.path.dirname(sys.argv[0]), ".."))

LLVM_AS_DIR = sys.argv[1]
LLVM_LINK_DIR = sys.argv[2]
OS_TYPE = sys.argv[3]
OUTPUT_DIR = os.path.abspath(sys.argv[4]) if len(sys.argv) == 5 else INPUT_DIR

# The source directories where we expect to find .ll files. This
# can be a single directory, if OUTPUT_DIR and INPUT_DIR are the same.
SRC_DIRS = sorted(set([INPUT_DIR, OUTPUT_DIR]))

# LLVM utility binaries
LLVM_AS = os.path.join(LLVM_AS_DIR, "llvm-as")
LLVM_LINK = os.path.join(LLVM_LINK_DIR, "llvm-link")
LLVM_AR = os.path.join(LLVM_LINK_DIR, "llvm-ar")

# Cleanup, remove those auto-generated files
print("*******************************************************************************")
print("                              Pre-Compile Cleanup                              ")
print("*******************************************************************************")

if not os.path.exists(OUTPUT_DIR):
    os.makedirs(OUTPUT_DIR)

for f in os.listdir(OUTPUT_DIR):
    fpath = os.path.join(OUTPUT_DIR, f)
    if f.startswith("g_") or  f.endswith(".bc") or f.endswith(".lib"): # Common library
        print(">>>  (LL-clean) remove " + fpath)
        os.remove(fpath)
    elif os.path.isdir(fpath) and f.startswith("gfx"): # GFX library
        gfx = f
        for subf in os.listdir(fpath):
            subfpath = os.path.join(fpath, subf)
            if subf.startswith("g_") or  subf.endswith(".bc") or subf.endswith(".lib"):
                print(">>>  (LL-clean) remove " + subfpath)
                os.remove(subfpath)

print("")

# =====================================================================================================================
# Generate GFX-independent LLVM emulation library
# =====================================================================================================================

# Generate .ll files
print("*******************************************************************************")
print("                 Generate LLVM Emulation IR (GLSL Arithmetic)                  ")
print("*******************************************************************************")
genGlslArithOpEmuCode.main(os.path.join(INPUT_DIR, "script/genGlslArithOpEmuCode.txt"),
                           OUTPUT_DIR, "std32")
genGlslArithOpEmuCode.main(os.path.join(INPUT_DIR, "script/genGlslArithOpEmuCodeF16.txt"),
                           OUTPUT_DIR, "float16")
genGlslArithOpEmuCode.main(os.path.join(INPUT_DIR, "script/genGlslArithOpEmuCodeF64.txt"),
                           OUTPUT_DIR, "float64")
genGlslArithOpEmuCode.main(os.path.join(INPUT_DIR, "script/genGlslArithOpEmuCodeI16.txt"),
                           OUTPUT_DIR, "int16")
genGlslArithOpEmuCode.main(os.path.join(INPUT_DIR, "script/genGlslArithOpEmuCodeI64.txt"),
                           OUTPUT_DIR, "int64")

print("*******************************************************************************")
print("                 Generate LLVM Emulation IR (GLSL Group)                       ")
print("*******************************************************************************")
genGlslGroupOpEmuCode.main(16, 64, OUTPUT_DIR)
genGlslGroupOpEmuCode.main(32, 64, OUTPUT_DIR)
genGlslGroupOpEmuCode.main(64, 64, OUTPUT_DIR)

print("*******************************************************************************")
print("                   Generate LLVM Emulation IR (GLSL Image) for %s             "%("GFX6"))
print("*******************************************************************************")
genGlslImageOpEmuCode.main(os.path.join(INPUT_DIR, "script/genGlslImageOpEmuCode.txt"),
                           OUTPUT_DIR, "gfx6")

print("*******************************************************************************")
print("                   Generate LLVM Emulation IR (GLSL Image) for %s             "%("GFX9"))
print("*******************************************************************************")
genGlslImageOpEmuCode.main(os.path.join(INPUT_DIR, "script/genGlslImageOpEmuCode.txt"),
                           os.path.join(OUTPUT_DIR, "gfx9"), "gfx9")

# Generate .lib file
print("*******************************************************************************")
print("                   Generate LLVM Emulation Library (Common)                    ")
print("*******************************************************************************")

# Assemble .ll files to .bc files
for ll_dir in SRC_DIRS:
  for f in os.listdir(ll_dir):
      if f.endswith(".ll"):
        outf = f.replace(".ll", ".bc")
        cmd = LLVM_AS + " " + f + " -o " + os.path.join(OUTPUT_DIR, outf)
        print(">>>  (LL-as) " + cmd)
        if OS_TYPE == "win":
          subprocess.check_call(cmd, cwd = ll_dir)
        else:
          subprocess.check_call(cmd, cwd = ll_dir, shell=True)

# Link special emulation .bc files to libraries (null fragment shader, copy shader)
SPECIAL_EMUS = ["NullFs", "CopyShader"]
for feature in SPECIAL_EMUS:
    # Search for the .bc file
    bcFile = ""
    for f in os.listdir(OUTPUT_DIR):
        if f.startswith("glsl" + feature) and f.endswith("Emu.bc"):
            bcFile = f

    if bcFile == "": # Not found
        continue

    # Link .bc files to .lib file
    bcFilePath = os.path.join(OUTPUT_DIR, bcFile)
    libFile = os.path.join(OUTPUT_DIR, "glsl" + feature + "Emu.lib")
    cmd = LLVM_LINK + " -o=" + libFile + " " + bcFilePath
    print(">>>  (LL-link) " + cmd)
    if OS_TYPE == "win" :
        subprocess.check_call(cmd)
    else :
        subprocess.check_call(cmd, shell = True)

    # Convert .lib file to a hex file
    hFile = os.path.join(OUTPUT_DIR, "g_llpcGlsl" + feature + "EmuLib.h")
    bin2hex(libFile, hFile)

    # Cleanup, remove those temporary files
    print(">>>  (LL-clean) remove " + bcFilePath)
    os.remove(bcFilePath)
    print(">>>  (LL-clean) remove " + libFile)
    os.remove(libFile)

# Add general emulation .bc files to a .lib archive (GLSL operations and built-ins)
# Collect .bc files
bcFiles = ""
for f in os.listdir(OUTPUT_DIR):
    if f.endswith(".bc"):
      bcFiles += os.path.join(OUTPUT_DIR, f) + " "

# Add .bc files to .lib archive file
libFile = os.path.join(OUTPUT_DIR, "glslEmu.lib")
cmd = LLVM_AR + " r " + libFile + " " + bcFiles
print(">>>  (LL-ar) " + cmd)
if OS_TYPE == "win" :
    subprocess.check_call(cmd)
else :
    subprocess.check_call(cmd, shell = True)

# Convert .lib file to a hex file
hFile = os.path.join(OUTPUT_DIR, "g_llpcGlslEmuLib.h")
bin2hex(libFile, hFile)

# Cleanup, remove those temporary files
for f in bcFiles.split():
    print(">>>  (LL-clean) remove " + f);
    os.remove(f)
print(">>>  (LL-clean) remove " + libFile);
os.remove(libFile)

print("")

# =====================================================================================================================
# Generate GFX-dependent LLVM emulation library
# =====================================================================================================================

# Assemble .ll files to .bc files and add emulation .bc files to archives
GFX_EMUS = ["gfx8", "gfx9"]

for gfx in GFX_EMUS:
    print("*******************************************************************************")
    print("                    Generate LLVM Emulation Library (%s)                     "%(gfx.upper()))
    print("*******************************************************************************")

    outdir = os.path.join(OUTPUT_DIR, gfx)
    if not os.path.exists(outdir):
      os.makedirs(outdir)

    # Assemble .ll files to .bc files
    for src_dir in SRC_DIRS:
      gfx_subdir = os.path.join(src_dir, gfx)
      for f in os.listdir(gfx_subdir):
          if f.endswith(".ll"):
              outf = f.replace(".ll", ".bc")
              cmd = LLVM_AS + " " + os.path.join(gfx, f) + " -o " + os.path.join(outdir, outf)
              print(">>>  (LL-as) " + cmd)
              if OS_TYPE == "win" :
                  subprocess.check_call(cmd, cwd = src_dir)
              else :
                  subprocess.check_call(cmd, cwd = src_dir, shell = True)

    # Search for the .bc file
    bcFiles = ""
    for f in os.listdir(outdir):
        if f.endswith(".bc"):
            bcFiles += os.path.join(outdir, f) + " "

    # Add .bc files to archive .lib file
    libFile = os.path.join(outdir, "glslEmu" + gfx.capitalize() + ".lib")
    cmd = LLVM_AR + " r " + libFile + " " + bcFiles
    print(">>>  (LL-ar) " + cmd)
    if OS_TYPE == "win" :
        subprocess.check_call(cmd)
    else :
        subprocess.check_call(cmd, shell = True)

    # Convert .lib file to a hex file
    hFile = os.path.join(outdir, "g_llpcGlslEmuLib" + gfx.capitalize() + ".h")
    bin2hex(libFile, hFile)

    # Cleanup, remove those temporary files
    for f in bcFiles.split():
        print(">>>  (LL-clean) remove " + f);
        os.remove(f)
    print(">>>  (LL-clean) remove " + libFile);
    os.remove(libFile)

    print("")

# =====================================================================================================================
# Generate Workaround LLVM emulation library
# =====================================================================================================================

# Assemble .ll files to .bc files and link emulation .bc files to libraries
WA_EMUS = ["treat1dImagesAs2d"]
WA_ROOT="wa"
for wa in WA_EMUS:
    print("*******************************************************************************")
    print("                    Generate LLVM Emulation Library (%s)                     "%(wa.upper()))
    print("*******************************************************************************")

    workDir = os.path.join(WA_ROOT, wa)
    outdir = os.path.join(OUTPUT_DIR, WA_ROOT, wa)
    if not os.path.exists(outdir):
      os.makedirs(outdir)

    # Assemble .ll files to .bc files
    for f in os.listdir(os.path.join(INPUT_DIR, workDir)):
        if f.endswith(".ll"):
            outf = f.replace(".ll", ".bc")
            cmd = LLVM_AS + " " + os.path.join(workDir, f) + " -o " + os.path.join(outdir, outf)
            print(">>>  (LL-as) " + cmd)
            if OS_TYPE == "win" :
                subprocess.check_call(cmd, cwd = INPUT_DIR)
            else :
                subprocess.check_call(cmd, cwd = INPUT_DIR, shell = True)

    # Search for the .bc file
    bcFiles = ""
    for f in os.listdir(outdir):
        if f.endswith(".bc"):
            bcFiles += os.path.join(outdir, f)

    # Add .bc files to archive .lib file
    libFile = os.path.join(outdir, "glslEmu" + wa.capitalize() + ".lib")
    cmd = LLVM_AR + " r " + libFile + " " + bcFiles
    print(">>>  (LL-ar) " + cmd)
    if OS_TYPE == "win" :
        subprocess.check_call(cmd)
    else :
        subprocess.check_call(cmd, shell = True)

    # Convert .lib file to a hex file
    hFile = os.path.join(OUTPUT_DIR, WA_ROOT, "g_llpcGlslEmuLib" + wa[:1].upper() + wa[1:] + ".h")
    bin2hex(libFile, hFile)

    # Cleanup, remove those temporary files
    for f in bcFiles.split():
        print(">>>  (LL-clean) remove " + f);
        os.remove(f)
    print(">>>  (LL-clean) remove " + libFile);
    os.remove(libFile)

    print("")
