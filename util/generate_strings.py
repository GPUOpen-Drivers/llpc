##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

import os
import sys
import re
from optparse import OptionParser

workDir = "./";
outDir = "";

def GetOpt():
    global workDir;
    global outDir;
    global openSource;

    parser = OptionParser()

    parser.add_option("-w", "--workdir", action="store",
                  type="string",
                  dest="workdir",
                  help="the work directory")

    parser.add_option("-o", "--outdir", action="store",
                  type="string",
                  dest="outdir",
                  help="the output directory")

    (options, args) = parser.parse_args()

    if options.workdir:
        print("The work directory is %s" % (options.workdir));
        workDir = options.workdir;
    else:
        print("The work directory is not specified, using default: " + workDir);

    if (workDir[-1] != '/'):
        workDir = workDir + '/';

    if (os.path.exists(workDir) == False) or (os.path.exists(workDir + "extensions.txt") == False):
        print("Work directory is not correct: " + workDir);
        exit();

    if options.outdir:
        print("The output directory is %s" % (options.outdir));
        outDir = options.outdir;
        if (outDir[-1] != '/'):
            outDir = outDir + '/';
    else:
        print("The output directory is not specified, using default: " + outDir);

def generate_string(f, name, suffix, value, gentype):
    global openSource;

    value = "%s\0" % value

    prefix = "const char %s%s[]" % (name, suffix);

    if gentype == 'decl':
        f.write("extern %s;\n" % prefix);
    else:
        f.write("%s = { " % prefix);
        for c in value:
            if sys.version_info[0] == 2:
                f.write("0x%s, " % c.encode('hex'))
            else:
                f.write("0x%s, " % c.encode("utf-8").hex())
        f.write("};\n");

    if gentype == 'decl':
        if name != name.upper():
            f.write("static const char* %s%s = %s%s;\n" % (name.upper(), suffix, name, suffix))

def generate_string_file_pass(string_file_prefix, gentype):

    string_file_txt = "%s.txt" % (string_file_prefix);
    header_file = "g_%s_%s.h" % (string_file_prefix, gentype);

    print("Generating %s from %s ..." % (header_file, string_file_txt))

    f = open(string_file_txt)
    lines = f.readlines()
    f.close()

    epidx = 0

    f = open(outDir + header_file, 'w')

    f.write("// do not edit by hand; generated from source file \"%s.txt\"\n" % string_file_prefix)
    for line in lines:
        original = line.rstrip().lstrip()
        if original == "" or original[0] == '#':
            continue

        generate_string(f, original, "_name", original, gentype)

    if epidx > 0:
        f.write("#define VKI_ENTRY_POINT_COUNT %d\n" % epidx)

    f.close()

def generate_string_file(string_file_prefix):
    generate_string_file_pass(string_file_prefix, 'decl');
    generate_string_file_pass(string_file_prefix, 'impl');

GetOpt()
os.chdir(workDir)

generate_string_file("extensions")
