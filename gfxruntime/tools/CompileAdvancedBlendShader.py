##
 #######################################################################################################################
 #
 #  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
 #
 #  Permission is hereby granted, free of charge, to any person obtaining a copy
 #  of this software and associated documentation files (the "Software"), to
 #  deal in the Software without restriction, including without limitation the
 #  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 #  sell copies of the Software, and to permit persons to whom the Software is
 #  furnished to do so, subject to the following conditions:
 #
 #  The above copyright notice and this permission notice shall be included in all
 #  copies or substantial portions of the Software.
 #
 #  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 #  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 #  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 #  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 #  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 #  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 #  IN THE SOFTWARE.
 #
 #######################################################################################################################

#**********************************************************************************************************************
# @file  CompileAdvancedBlendShader.py
# @brief A script file to automate the creation of Advanced Blend shader runtime library.
#**********************************************************************************************************************

"""A script file to automate the creation of Advanced Blend shader runtime library."""

import argparse
import os
import subprocess
import sys
import struct
import shutil

FILE_STANDARD_HEADER = """
/* Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved. */

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// WARNING!  WARNING!  WARNING!  WARNING!  WARNING!  WARNING!  WARNING!  WARNING!  WARNING!  WARNING!  WARNING!
//
// This code has been generated automatically. Do not hand-modify this code.
//
// WARNING!  WARNING!  WARNING!  WARNING!  WARNING!  WARNING!  WARNING! WARNING!  WARNING!  WARNING!  WARNING!
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

"""

FILE_HEADER_FORMAT_STRING = "static constexpr unsigned AdvancedBlendLibrary[] =\n{\n"
FILE_FOOTER_STRING = "\n};\n"

SHADER_FILE_NAME = "AdvancedBlend.hlsl"
OUTPUT_FILE_NAME = "AdvancedBlendLibrary"
DXC_EXECUTABLE = "dxc"

def FixExePath(exePath):
    if os.path.exists(exePath + ".exe"):
        # Use local Windows Path
        return exePath + ".exe"
    elif not os.path.exists(exePath):
        # Use system PATH on Linux
        return os.path.basename(exePath)
    return exePath

def FixInputPath(path) -> str:
    return os.path.abspath(path).replace('\\\\', '\\').replace('\\', '/')

def RemoveFile(path):
    # Workaround for file handles sometimes not closing correctly when just using os.remove()
    tmpPath = path + "tmp"
    os.rename(path, tmpPath)
    os.remove(tmpPath)

def RemoveFolder(path):
    shutil.rmtree(path)

def RunSpirv(compilerPath, inputHlslFile, compiledSpvFile):
    commandArgs = [
        compilerPath,
        '-T', 'lib_6_3',
        '-spirv',
        '-fspv-target-env=universal1.5',
        '-fvk-use-scalar-layout',
        '-Od',
        '-Vd',
        '-HV', '2021',
        '-D', 'AMD_VULKAN=1',
        '-Fo', compiledSpvFile,
        inputHlslFile
    ]
    # Ensure the following code is executed from the script's directory.
    os.chdir(os.path.dirname(__file__))

    result = subprocess.run(commandArgs, check=False)
    print(' '.join(commandArgs))
    if result.returncode != 0:
        return False
    return True

def ConvertSpvFile(compiledSpvFile, outputFile):
    try:
        spvBinaryFile = open(compiledSpvFile, "rb")
        spvBinData = spvBinaryFile.read()
        spvBinaryFile.close()
        i = 0
        spvHexText = ""
        while i < len(spvBinData):
            binWord = spvBinData[i:i+4]
            intWord = struct.unpack('I', binWord)[0]
            hexWord = "{0:#010x}".format(intWord)
            spvHexText += hexWord

            i += 4

            if (i != len(spvBinData)):
                spvHexText += ","
            if (i % 32 == 0):
                spvHexText += "\n"
            else:
                spvHexText += " "

        outputFile = open(outputFile, "w")
        outputFile.write(FILE_STANDARD_HEADER)
        outputFile.write(FILE_HEADER_FORMAT_STRING)
        outputFile.write(spvHexText)
        outputFile.write(FILE_FOOTER_STRING)
        outputFile.close()
        return True
    except Exception as e:
        return False

def main():
    result = 0
    parser = argparse.ArgumentParser(description='Helper script to compile Advanced Blend runtime library.')
    parser.add_argument('--compilerPath', help='Path to SPIR-V compiler', default='dxc')
    parser.add_argument('--shaderDir', help='path to the source shader', default=None)
    parser.add_argument('--outputDir', help='Output directory for compiled shaders', default=None)

    args = parser.parse_args()
    compilerPath = FixInputPath(args.compilerPath) + '/' + DXC_EXECUTABLE
    compilerPath = FixExePath(compilerPath)
    shaderInputDir = FixInputPath(args.shaderDir)
    outputDir = FixInputPath(args.outputDir)

    tempDirPath = outputDir + '/' + 'tmp'
    # Get rid of temp dir that can be left over from an unclean build.
    if os.path.exists(tempDirPath):
        RemoveFolder(tempDirPath)
    os.mkdir(tempDirPath)

    compiledSpvFile = tempDirPath + '/' + OUTPUT_FILE_NAME + '.spv'
    inputHlslFile = shaderInputDir + '/' +  SHADER_FILE_NAME
    result = RunSpirv(compilerPath, inputHlslFile, compiledSpvFile)
    if result :
        os.chdir(outputDir)
        outputFile = outputDir + '/' + 'g_' + OUTPUT_FILE_NAME + '_spv.h'
        result = ConvertSpvFile(compiledSpvFile, outputFile)

        RemoveFile(compiledSpvFile)
        RemoveFolder(tempDirPath)
    return 0 if result else 1

if __name__ == '__main__':
    sys.exit(main())
