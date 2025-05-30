#!/usr/bin/env python3
##
 #######################################################################################################################
 #
 #  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

# This script is based on the LLVM Project's update_test_checks.py, which is
# licensed under the Apache License v2.0 with LLVM Exceptions; see the file
# UpdateTestChecks/LICENSE.txt relative to this directory. It has been heavily
# modified for use with LLPC.

"""Generate FileCheck statements for LLPC lit tests.

This tool is designed to be used on LLPC lit tests that are configured to
output a single batch of intermediate IR, for example, using the -emit-lgc
option.

Example usage:

# Default to using `amdllpc` as found in your PATH.
$ update_llpc_test_checks.py llpc/test/foo.comp

# Override the lookup of amdllpc.
$ update_llpc_test_checks.py --tool-binary=../../../build/dbg/compiler/llpc/amdllpc llpc/test/foo.comp
"""

import argparse
import os  # Used to advertise this file's name ("autogenerated_note").
import re
import sys
from typing import List

from UpdateTestChecks import common

###############################################################################
# PAL metadata support

PAL_METADATA_RE = re.compile(
    r"^[ \t]*\.amdgpu_pal_metadata\n" r"---\n" r"(?P<metadata>.*?)" r"^...\n",
    flags=(re.MULTILINE | re.DOTALL),
)

YAML_INDENT_RE = re.compile(r"(?P<indent>[ -]*)((?P<name>[^:]+):)?")
YAML_SUFFIX_RE = re.compile(r":[^:]*$")

HEX_RE = re.compile(r"0x[0-9a-f]+")

def process_pal_metadata(pal_metadata_dict, prefixes, raw_tool_output):
    m = PAL_METADATA_RE.search(raw_tool_output)
    if not m:
        common.warn(f"Did not find PAL metadata")
        return

    metadata_in_lines = m.group("metadata").splitlines()
    scrubbed_lines = []

    scope = []

    def get_scope_path():
        return "".join(name for name, _ in scope)

    for line in metadata_in_lines:
        m = YAML_INDENT_RE.match(line)
        indent = len(m.group("indent"))
        scope = [(name, ind) for name, ind in scope if ind < indent]

        if m.group("name") is not None:
            scope.append((m.group("name"), indent))

        path = get_scope_path()
        if "hash" in path:
            line = HEX_RE.subn("0x{{[0-9a-f]+}}", line)[0]
        elif "llpc_version" in path or "PGM_CHKSUM" in path:
            line = YAML_SUFFIX_RE.subn(": {{.*}}", line)[0]

        scrubbed_lines.append(line)

    metadata = "\n".join(scrubbed_lines)
    for prefix in prefixes:
        if prefix not in pal_metadata_dict:
            pal_metadata_dict[prefix] = metadata
        else:
            if pal_metadata_dict[prefix] != metadata:
                pal_metadata_dict[prefix] = None

def add_pal_metadata_checks(
    pal_metadata_dict, comment_prefix, prefix_list, output_lines: List[str]
):
    written_prefixes = set()
    for prefix_list_entry in prefix_list:
        prefixes = prefix_list_entry[0]
        for prefix in prefixes:
            if prefix in pal_metadata_dict and pal_metadata_dict[prefix] is not None:
                break
        else:
            common.warn(f"Did not find PAL metadata for prefix list {prefixes}")
            return

        if prefix in written_prefixes:
            continue

        output_lines.append(comment_prefix)
        output_lines.append(f"{comment_prefix} {prefix}-LABEL: .amdgpu_pal_metadata")
        output_lines.append(f"{comment_prefix} {prefix}-NEXT: ---")
        for line in pal_metadata_dict[prefix].splitlines():
            output_lines.append(f"{comment_prefix} {prefix}-NEXT: {line}")
        output_lines.append(f"{comment_prefix} {prefix}-NEXT: ...")

        written_prefixes.add(prefix)

###############################################################################
# Assembly support
ASM_FUNCTION_AMDGPU_RE = re.compile(
    r'^_?(?P<func>[^:\n]+):[ \t]*(?:;+[ \t]*@"?(?P=func)"?)?\n[^:]*?'
    r"(?P<body>.*?)\n"  # (body of the function)
    # This list is incomplete
    r"^\s*(\.Lfunc_end[0-9]+:\n|\.section)",
    flags=(re.M | re.S),
)

def scrub_asm_amdgpu(asm, *args):
    # Scrub runs of whitespace out of the assembly, but leave the leading
    # whitespace in place.
    asm = common.SCRUB_WHITESPACE_RE.sub(r" ", asm)
    # Expand the tabs used for indentation.
    asm = str.expandtabs(asm, 2)
    # Strip trailing whitespace.
    asm = common.SCRUB_TRAILING_WHITESPACE_RE.sub(r"", asm)
    return asm

def add_asm_checks(
    output_lines,
    comment_marker,
    prefix_list,
    func_dict,
    func_name,
    ginfo,
    global_vars_seen_dict,
    args,
    is_filtered,
    original_check_lines,
):
    # Label format is based on ASM string.
    check_label_format = "{} %s-LABEL: %s%s%s%s".format(comment_marker)
    return common.add_checks(
        output_lines,
        comment_marker,
        prefix_list,
        func_dict,
        func_name,
        check_label_format,
        ginfo,
        global_vars_seen_dict,
        is_filtered,
        args.preserve_names,
        original_check_lines,
    )

def add_ir_checks(
    output_lines,
    comment_marker,
    prefix_list,
    func_dict,
    func_name,
    ginfo,
    global_vars_seen_dict,
    args,
    is_filtered,
    original_check_lines,
):
    return common.add_ir_checks(
        output_lines,
        comment_marker,
        prefix_list,
        func_dict,
        func_name,
        args.preserve_names,
        args.function_signature,
        ginfo,
        global_vars_seen_dict,
        is_filtered,
        original_check_lines,
        args.generalize_calls,
    )

COMMENT_PREFIXES_BY_FILE_SUFFIX = {
    ".pipe": ";",
    ".spvasm": ";",
    ".ll": ";",
    ".lgc": ";",
    # Everything else defaults to '//'
}

def get_comment_prefix(test_name: str, input_lines):
    ext = os.path.splitext(test_name)[1]
    return COMMENT_PREFIXES_BY_FILE_SUFFIX.get(ext, "//")

def main():
    from argparse import RawTextHelpFormatter

    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=RawTextHelpFormatter
    )
    parser.add_argument(
        "--tool",
        default="amdllpc",
        help='The name of the tool used to generate the test case (defaults to "amdllpc")',
    )
    parser.add_argument(
        "--tool-binary", help="The tool binary used to generate the test case"
    )
    parser.add_argument(
        "--function", help="Only update functions whose name matches the given regex"
    )
    parser.add_argument(
        "-p", "--preserve-names", action="store_true", help="Do not scrub IR names"
    )
    parser.add_argument(
        "--function-signature",
        action="store_true",
        help="Keep function signature information around for the check line",
    )
    parser.add_argument(
        "--scrub-attributes",
        action="store_true",
        help="Remove attribute annotations (#0) from the end of check line",
    )
    parser.add_argument(
        "--check-attributes",
        action="store_true",
        help='Check "Function Attributes" for functions',
    )
    parser.add_argument(
        "--check-globals",
        nargs="?",
        const="all",
        default="default",
        choices=["none", "smart", "all"],
        help="Check global entries (global variables, metadata, attribute sets, ...) for functions",
    )
    parser.add_argument(
        "--check-pal-metadata",
        action="store_true",
        help="Check PAL metadata in output assembly",
    )
    parser.add_argument(
        "--reset-variable-names",
        action="store_true",
        help="Reset all variable names to correspond closely to the variable names in IR. "
        "This tends to result in larger diffs.",
    )
    parser.add_argument("tests", nargs="+")
    initial_args = common.parse_commandline_args(parser)

    if initial_args.tool_binary:
        tool_basename = os.path.basename(initial_args.tool_binary)
        if not re.match(r"^%s(-\d+)?(\.exe)?$" % (initial_args.tool), tool_basename):
            common.error("Unexpected tool name: " + tool_basename)
            sys.exit(1)

    for ti in common.itertests(
        initial_args.tests,
        parser,
        "tool/update_llpc_test_checks.py",
        comment_prefix_callback=get_comment_prefix,
    ):
        # If requested we scrub trailing attribute annotations, e.g., '#0', together with whitespaces
        if ti.args.scrub_attributes:
            common.SCRUB_TRAILING_WHITESPACE_TEST_RE = (
                common.SCRUB_TRAILING_WHITESPACE_AND_ATTRIBUTES_RE
            )
        else:
            common.SCRUB_TRAILING_WHITESPACE_TEST_RE = (
                common.SCRUB_TRAILING_WHITESPACE_RE
            )

        tool_basename = ti.args.tool
        tool_binary = tool_basename
        if tool_basename == initial_args.tool and initial_args.tool_binary:
            tool_binary = initial_args.tool_binary

        prefix_list = []
        for l in ti.run_lines:
            if "|" not in l:
                common.warn("Skipping unparsable RUN line: " + l)
                continue

            commands = [cmd.strip() for cmd in l.split("|")]
            assert len(commands) >= 2
            if len(commands) > 2:
                common.error("Complex pipes are unsupported")
                sys.exit(1)
            tool_cmd = commands[-2]
            filecheck_cmd = commands[-1]
            common.verify_filecheck_prefixes(filecheck_cmd)
            if not tool_cmd.startswith(tool_basename + " "):
                common.warn("Skipping non-%s RUN line: %s" % (tool_basename, l))
                continue

            if not filecheck_cmd.startswith("FileCheck "):
                common.warn("Skipping non-FileChecked RUN line: " + l)
                continue

            tool_cmd_args = tool_cmd[len(tool_basename) :].strip()

            check_prefixes = [
                item
                for m in common.CHECK_PREFIX_RE.finditer(filecheck_cmd)
                for item in m.group(1).split(",")
            ]
            if not check_prefixes:
                check_prefixes = ["CHECK"]

            # FIXME: We should use multiple check prefixes to common check lines. For
            # now, we just ignore all but the last.
            prefix_list.append((check_prefixes, tool_cmd_args))

        global_vars_seen_dict = {}

        function_re = None
        scrubber = None
        add_checks = None

        pal_metadata_dict = {}

        for prefixes, tool_args in prefix_list:
            common.debug("Extracted tool cmd: " + tool_basename + " " + tool_args)
            common.debug("Extracted FileCheck prefixes: " + str(prefixes))

            raw_tool_output = common.invoke_tool_only(
                tool_binary, tool_args, ti.path, verbose=ti.args.verbose
            )

            is_ir = common.OPT_FUNCTION_RE.search(raw_tool_output) is not None
            if is_ir:
                function_re = common.OPT_FUNCTION_RE
                scrubber = common.scrub_body
                add_checks = add_ir_checks
                ginfo = common.make_ir_generalizer(ti.args.version)
            else:
                function_re = ASM_FUNCTION_AMDGPU_RE
                scrubber = scrub_asm_amdgpu
                add_checks = add_asm_checks
                ginfo = common.make_asm_generalizer(ti.args.version)

            builder = common.FunctionTestBuilder(
                run_list=prefix_list,
                flags=ti.args,
                scrubber_args=[],
                path=ti.path,
                ginfo=ginfo,
            )

            builder.process_run_line(function_re, scrubber, raw_tool_output, prefixes)
            builder.processed_prefixes(prefixes)

            if ti.args.check_pal_metadata:
                if not ginfo.is_asm():
                    common.error(
                        f"{ti.path}: --check-pal-metadata only applies with asm output"
                    )
                    sys.exit(1)

                process_pal_metadata(pal_metadata_dict, prefixes, raw_tool_output)

        func_dict = builder.finish_and_get_func_dict()
        prefix_set = set([prefix for prefixes, _ in prefix_list for prefix in prefixes])

        if not ti.args.reset_variable_names:
            original_check_lines = common.collect_original_check_lines(ti, prefix_set)
        else:
            original_check_lines = {}

        common.debug("Rewriting FileCheck prefixes:", str(prefix_set))
        output_lines = []

        # Generate the appropriate checks for each function.  We need to emit
        # these in the order according to the generated output so that CHECK-LABEL
        # works properly.  func_order provides that.

        # We can't predict where various passes might insert functions so we can't
        # be sure the input function order is maintained.  Therefore, first spit
        # out all the source lines.
        common.dump_input_lines(output_lines, ti, prefix_set, ti.comment_prefix)

        args = ti.args

        # Replace the meta variable containing the amdpal.pipelines and
        # amdpal.version because it contains hashes that could change.
        # Instead, use a regex containing "amdpal.pipelines{{.*}}amdpal.version"
        global_var_dict = builder.global_var_dict()
        for p in prefix_list:
            checkprefixes = p[0]
            for checkprefix in checkprefixes:
                if "META" not in global_var_dict[checkprefix].keys():
                    continue

                meta = global_var_dict[checkprefix]["META"]
                # replace just the value containing amdpal.* and keep the other ones
                meta = [
                    (
                        (
                            x,
                            re.sub(
                                "(.*amdpal\.pipelines).*(amdpal\.version)",
                                "\\1{{.*}}\\2",
                                s,
                            ),
                        )
                        if "amdpal" in s
                        else (x, s)
                    )
                    for x, s in meta
                ]
                global_var_dict[checkprefix]["META"] = meta

        if args.check_globals != "none":
            common.add_global_checks(
                global_var_dict,
                ti.comment_prefix,
                prefix_list,
                output_lines,
                ginfo,
                global_vars_seen_dict,
                args.preserve_names,
                True,
                args.check_globals,
            )

        # Filter out functions
        func_order = builder.func_order()
        if ti.args.function:
            filter_re = re.compile(ti.args.function)
            new_func_order = {}
            for prefix, func_names in func_order.items():
                new_func_order[prefix] = [
                    func_name for func_name in func_names if filter_re.search(func_name)
                ]
            func_order = new_func_order

        # Now generate all the checks.
        common.add_checks_at_end(
            output_lines,
            prefix_list,
            func_order,
            ti.comment_prefix,
            lambda my_output_lines, prefixes, func: add_checks(
                my_output_lines,
                ti.comment_prefix,
                prefixes,
                func_dict,
                func,
                ginfo,
                global_vars_seen_dict,
                args,
                is_filtered=builder.is_filtered(),
                original_check_lines=original_check_lines.get(func, {}),
            ),
        )

        if args.check_globals != "none":
            common.add_global_checks(
                global_var_dict,
                ti.comment_prefix,
                prefix_list,
                output_lines,
                ginfo,
                global_vars_seen_dict,
                args.preserve_names,
                False,
                args.check_globals,
            )

        if args.check_pal_metadata:
            add_pal_metadata_checks(
                pal_metadata_dict, ti.comment_prefix, prefix_list, output_lines
            )

        common.debug("Writing %d lines to %s..." % (len(output_lines), ti.path))

        with open(ti.path, "wb") as f:
            f.writelines(["{}\n".format(l).encode("utf-8") for l in output_lines])

if __name__ == "__main__":
    main()
