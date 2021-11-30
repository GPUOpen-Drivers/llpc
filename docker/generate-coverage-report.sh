#!/usr/bin/env bash
# Generates an HTML code coverage report using llvm-profdata (to merge raw profiles) and llvm-cov.
# Assumes the profiles are under /vulkandriver, and writes to the output directory /vulkandriver/coverage_report.
set -e

cd /vulkandriver

# Merge raw profiles and delete them afterwards.
llvm-profdata merge -sparse *.profraw -o code_coverage_profile.profdata
rm *.profraw

# Create HTML report. Ignore reporting coverage for files coming from LLVM as we are only interested in LLPC
# and would otherwise make the report very large.
llvm-cov show -format=html -ignore-filename-regex='.*llvm.*' \
         -instr-profile=code_coverage_profile.profdata builds/ci-build/compiler/llpc/amdllpc \
         -o coverage_report
