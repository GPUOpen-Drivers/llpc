##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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

currentDir = os.getcwd()

INPUT_DIR  = os.path.abspath(os.path.join(os.path.dirname(sys.argv[0]), ".."))
OUTPUT_DIR = os.path.abspath(sys.argv[4]) if len(sys.argv) == 5 else INPUT_DIR

LLVM_AS_DIR = sys.argv[1]
LLVM_LINK_DIR = sys.argv[2]
OS_TYPE = sys.argv[3]

if not LLVM_AS_DIR.startswith("/"):
    LLVM_AS_DIR = os.path.join(currentDir, LLVM_AS_DIR);
if not LLVM_LINK_DIR.startswith("/"):
    LLVM_LINK_DIR = os.path.join(currentDir, LLVM_LINK_DIR);

# The source directories where we expect to find .ll files. This
# can be a single directory, if OUTPUT_DIR and INPUT_DIR are the same.
SRC_DIRS = sorted(set([INPUT_DIR, OUTPUT_DIR]))

# LLVM utility binaries
LLVM_OPT = os.path.join(LLVM_AS_DIR, "opt")
LLVM_LINK = os.path.join(LLVM_LINK_DIR, "llvm-link")
LLVM_AR = os.path.join(LLVM_LINK_DIR, "llvm-ar")

# Cleanup, remove those auto-generated files
print("*******************************************************************************")
print("                              Pre-Compile Cleanup                              ")
print("*******************************************************************************")

if not os.path.exists(OUTPUT_DIR):
    os.makedirs(OUTPUT_DIR)

for f in os.listdir(OUTPUT_DIR):
    fPath = os.path.join(OUTPUT_DIR, f)
    if f.startswith("g_") or f.endswith(".bc") or f.endswith(".lib"): # Common library
        print(">>>  (LL-clean) remove " + fPath)
        os.remove(fPath)
    elif os.path.isdir(fPath) and f.startswith("gfx"): # GFX library
        gfx = f
        for subf in os.listdir(fPath):
            subfPath = os.path.join(fPath, subf)
            if subf.startswith("g_") or  subf.endswith(".bc") or subf.endswith(".lib"):
                print(">>>  (LL-clean) remove " + subfPath)
                os.remove(subfPath)

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
genGlslArithOpEmuCode.main(os.path.join(INPUT_DIR, "script/genGlslArithOpEmuCodeI8.txt"),
                           OUTPUT_DIR, "int8")
genGlslArithOpEmuCode.main(os.path.join(INPUT_DIR, "script/genGlslArithOpEmuCodeI16.txt"),
                           OUTPUT_DIR, "int16")
genGlslArithOpEmuCode.main(os.path.join(INPUT_DIR, "script/genGlslArithOpEmuCodeI64.txt"),
                           OUTPUT_DIR, "int64")

# Generate .lib file
print("*******************************************************************************")
print("                   Generate LLVM Emulation Library (Common)                    ")
print("*******************************************************************************")

# Assemble .ll files to .bc files
for srcDir in SRC_DIRS:
  for f in os.listdir(srcDir):
      if f.endswith(".ll"):
        outF = f.replace(".ll", ".bc")
        cmd = LLVM_OPT + " -strip -o " + os.path.join(OUTPUT_DIR, outF) + " " + os.path.join(srcDir,f)
        print(">>>  (LL-as) " + cmd)
        if OS_TYPE == "win":
          subprocess.check_call(cmd, cwd = srcDir)
        else:
          subprocess.check_call(cmd, cwd = srcDir, shell=True)

# Add general emulation .bc files to a .lib archive (GLSL operations and built-ins)
# Collect .bc files
bcFiles = ""
for f in os.listdir(OUTPUT_DIR):
    if f.endswith(".bc"):
        bcFiles += f + " "

# Add .bc files to .lib archive file
os.chdir(OUTPUT_DIR)
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
os.chdir(currentDir)

print("")

# =====================================================================================================================
# Generate GFX-dependent LLVM emulation library
# =====================================================================================================================

# Assemble .ll files to .bc files and add emulation .bc files to archives
GFX_EMUS = ["gfx8", "gfx9"]

#if LLPC_BUILD_GFX10
GFX_EMUS.append("gfx10")
#endif

for gfx in GFX_EMUS:
    print("*******************************************************************************")
    print("                    Generate LLVM Emulation Library (%s)                     "%(gfx.upper()))
    print("*******************************************************************************")

    outDir = os.path.join(OUTPUT_DIR, gfx)
    if not os.path.exists(outDir):
      os.makedirs(outDir)

    # Assemble .ll files to .bc files
    for srcDir in SRC_DIRS:
        gfxSubdir = os.path.join(srcDir, gfx)
        for f in os.listdir(gfxSubdir):
            if f.endswith(".ll"):
                outF = f.replace(".ll", ".bc")
                cmd = LLVM_OPT + " -strip -o " + os.path.join(outDir, outF) + " " + os.path.join(gfxSubdir, f)
                print(">>>  (LL-as) " + cmd)
                if OS_TYPE == "win" :
                    subprocess.check_call(cmd, cwd = srcDir)
                else :
                    subprocess.check_call(cmd, cwd = srcDir, shell = True)

    # Search for the .bc file
    bcFiles = ""
    for f in os.listdir(outDir):
        if f.endswith(".bc"):
            bcFiles += os.path.join(outDir, f) + " "

    # Add .bc files to archive .lib file
    libFile = os.path.join(outDir, "glslEmu" + gfx.capitalize() + ".lib")
    cmd = LLVM_AR + " r " + libFile + " " + bcFiles
    print(">>>  (LL-ar) " + cmd)
    if OS_TYPE == "win" :
        subprocess.check_call(cmd)
    else :
        subprocess.check_call(cmd, shell = True)

    # Convert .lib file to a hex file
    hFile = os.path.join(outDir, "g_llpcGlslEmuLib" + gfx.capitalize() + ".h")
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
WA_EMUS = ["treat1dImagesAs2d", "disableI32ModToI16Mod"]
WA_ROOT="wa"
for wa in WA_EMUS:
    print("*******************************************************************************")
    print("                    Generate LLVM Emulation Library (%s)                     "%(wa.upper()))
    print("*******************************************************************************")

    workDir = os.path.join(WA_ROOT, wa)
    outDir  = os.path.join(OUTPUT_DIR, WA_ROOT, wa)

    if not os.path.exists(outDir):
      os.makedirs(outDir)

    # Assemble .ll files to .bc files
    for f in os.listdir(os.path.join(INPUT_DIR, workDir)):
        if f.endswith(".ll"):
            outF = f.replace(".ll", ".bc")
            absf = os.path.join(INPUT_DIR, workDir, f)
            cmd = LLVM_OPT + " -strip -o " + os.path.join(outDir, outF) + " " + absf
            print(">>>  (LL-as) " + cmd)
            if OS_TYPE == "win" :
                subprocess.check_call(cmd, cwd = INPUT_DIR)
            else :
                subprocess.check_call(cmd, cwd = INPUT_DIR, shell = True)

    # Search for the .bc file
    bcFiles = ""
    for f in os.listdir(outDir):
        if f.endswith(".bc"):
            bcFiles += os.path.join(outDir, f) + " "

    # Add .bc files to archive .lib file
    libFile = os.path.join(outDir, "glslEmu" + wa.capitalize() + ".lib")
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
