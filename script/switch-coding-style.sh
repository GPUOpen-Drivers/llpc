#!/bin/bash
##
 #######################################################################################################################
 #
 #  Copyright (c) 2020-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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

: # #################################################################################################
: # Script to perform one-time coding style changes in LLPC/LGC/VKGC
: #
: # This script is provided to allow developers with local or unmerged commits to rebase
: # them on the re-styled codebase.
: #
: # The script uses a hacked clang-tidy. So the first step is to get the llvm-project sources
: # with the clang-tidy hacks from
: #
: #   https://github.com/trenouf/llvm-project/tree/clang-tidy-for-llpc-legal
: #
: # then do a release build of clang-format, clang-tidy
: # and clang-apply-replacements. (A debug build results in the tools being too slow.) Ensure
: # the built tools are on PATH.
: #
: # The script assumes you have ccache, so ensure you have that installed. You might like to
: # increase the cache size from the default 5G by editing ~/.ccache/ccache.conf to include
: # a line like
: #
: #   max_size = 20.0G
: #
: # Then, to re-style the entire LLPC/LGC/VKGC tree, cd into the root of the llpc repository
: # (containing subdirectories llpc, lgc, tool, util, include), and run the script naming
: # the transformations to perform:
: #
: # script/switch_coding_style.sh int32 identifiers braces comparisons parentheses comments format
: #
: # To rebase a local or unmerged commit or sequence of commits, you need to run the script
: # as a gt filter-branch script.
: #
: # First, check out the most recent of your sequence of commits, and rebase on the last
: # upstream commit before the re-styling was applied (i.e. the commit before the one that did
: # the "int32" change).
: #
: # Next, set a variable to how many commits are in your sequence of commits:
: #
: # numcommits=`gt log --oneline vulkan-github/dev..HEAD | wc -l`
: #
: # (That will be set to 1 if you have a single commit.)
: #
: # Run the script on one more commit than that, so your re-styled commit(s) are based on a
: # re-styled version of upstream:
: #
: # gt filter-branch --tree-filter '
: #     script/switch_coding_style.sh int32 identifiers braces comparisons parentheses comments format
: # ' HEAD~$((numcommits+1))..HEAD
: #
: # Finally, rebase the re-styled commits on the post-re-styled upstream:
: #
: # gt rebase HEAD~$numcommits --onto vulkan-github/dev
: #
: # #################################################################################################

: # Convert uint32_t to unsigned and int32_t to int ###
convertInt32() {
  # Do not convert in SPIR-V files except SPIRVReader.cpp and SPIRVInternal.h.
  # Do not convert elf reader
  files=(llpc/translator/lib/SPIRV/SPIRVReader.cpp
         llpc/translator/lib/SPIRV/SPIRVInternal.h
         `find include tool util lgc llpc \
                 -name vkgcElfReader.h -prune -o \
                 -name test -prune -o \
                 -name translator -prune -o \
                 -name \*.cpp -print -o \
                 -name \*.h -print`)
  for file in "${files[@]}"; do
    sed 's/uint32_t/unsigned/g; s/int32_t/int/g' $file >$file.new && mv -f $file.new $file || exit 1
  done
}

: # Run clang-tidy with the specified checks.
: # $1 is the value of the -checks option to use
do_clang_tidy() {
  checks="$1"

  cppfiles=(`find tool util lgc llpc llpc/translator/lib/SPIRV/SPIRVReader.cpp \
                -name test -prune -o \
                -name translator -prune -o \
                -name \*.cpp -print`)

  clang_tidy=clang-tidy
  clang_apply_replacements=clang-apply-replacements

  # Set defines for cmake line.
  cmakedefs=(-DCMAKE_BUILD_TYPE=Debug
             -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
             -DLLVM_PARALLEL_LINK_JOBS=2
             -DLLVM_CCACHE_BUILD=1
            )
  if [ "${PWD%/api/compiler}" != "$PWD" ]; then
    # AMD internal directory structure
    cmakedefs+=(-DXGL_LLVM_SRC_PATH=../icd/imported/llvm-project/llvm
                -DXGL_VKGC_PATH=../icd/api/compiler
                -DXGL_LLPC_PATH=../icd/api/compiler/llpc
                -DXGL_PAL_PATH=../icd/imported/pal
                -DXGL_SPVGEN_PATH=../tools/spvgen
                -DVULKAN_HEADER_PATH=../icd/api/include/khronos
               )
  fi
#if VKI_IMAGE_BVH_INTERSECT_RAY
  cmakedefs+=(-DVKI_IMAGE_BVH_INTERSECT_RAY=1)
#endif
#if VKI_RAY_TRACING
  cmakedefs+=(-DVKI_RAY_TRACING=1)
#endif

  # If this is the first check in this script run, do a cmake build of amdllpc,
  # asking for a compile_commands.json file.
  if [ ! -f $build/CMakeCache.txt ]; then
    (cd $build &&
      rm -f CMakeCache.txt &&
      cmake -GNinja "${cmakedefs[@]}" .. &&
      ninja amdllpc) || exit 1
  fi

  # Clean up from last time and create directories for fixes:
  rm -rf $build/fixes &&
  mkdir -p $build/fixes/lgc/builder &&
  mkdir -p $build/fixes/llpc/context &&
  mkdir -p $build/fixes/llpc/imported/metrohash/src &&
  mkdir -p $build/fixes/llpc/lower &&
  mkdir -p $build/fixes/lgc/patch &&
  mkdir -p $build/fixes/lgc/patch/gfx6/chip &&
  mkdir -p $build/fixes/lgc/patch/gfx9 &&
  mkdir -p $build/fixes/lgc/patch/gfx9/chip &&
  mkdir -p $build/fixes/llpc/tool &&
  mkdir -p $build/fixes/llpc/tool/vfx &&
  mkdir -p $build/fixes/llpc/translator/lib/SPIRV &&
  mkdir -p $build/fixes/llpc/translator/lib/SPIRV/libSPIRV &&
  mkdir -p $build/fixes/llpc/translator/lib/SPIRV/Mangler &&
  mkdir -p $build/fixes/lgc/util &&
  mkdir -p $build/fixes/llpc/util &&
  mkdir -p $build/fixes/tool/dumper &&
  mkdir -p $build/fixes/tool/vfx &&
  mkdir -p $build/fixes/util ||
  exit 1

  # Run parallelized clang-tidy on most source files, exporting fixes to *.cpp.fixes.yaml
  if ! (cd $build &&
    for file in "${cppfiles[@]}"; do echo "$file"; done |
      xargs -t -P`nproc` -IINPUT "$clang_tidy" -checks="$checks" -export-fixes="fixes/INPUT.fixes.yaml" "$srcfrombuild/INPUT"
  ); then
    echo "${0##*/}: Error(s) from clang-tidy. (If you can't see any errors above, it might be that it couldn't write the .fixes.yaml file because I forgot to create its directory" >&2
    exit 1
  fi

  # Remove empty lines from *.fixes.yaml files. This is a hack to work around a problem with my
  # clang-tidy readability-declaration-inline-comment code; it inserts a spurious blank line when
  # moving an inline parameter comment to the block comment at the top of the function. It only does
  # that when running clang-tidy and clang-apply-replacements separately like we do here. I never
  # got to the bottom of that problem, so I am hacking it here.
  # Also canonicalize include file paths so clang-apply-replacements knows to merge multiple
  # occurrences of the same fix.
  (cd $build && for file in `find fixes -name \*.yaml`; do
    sed '
      /^$/d
      s/\(FilePath:.*\/\)include\/\.\.\//\1/
      s/\(FilePath:.*\/\)llpc\/\.\.\//\1/
    ' $file >$file.new && mv -f $file.new $file || exit 1
  done) || exit 1

  # Apply the fixes
  (cd $build &&
  "$clang_apply_replacements" fixes) ||
  exit 1

}

: # Rename identifiers
renameIdentifiers() {
  # Set up .clang-tidy
  echo "
 # Do not modify llpc.h, vkgcDefs.h, anything in the SPIRV library, or vfx.h, or anything in lgc/imported.
HeaderFilterRegex: '.*/vfx/vfx..*\\.h|.*/dumper/vkgc.*\\.h|.*/util/vkgc.*\\.h|.*/lgc/include/.*\\.h|.*/context/llpc[^/]*\\.h|.*/util/llpc[^/]*\\.h|.*/lower/llpc[^/]*\\.h|.*/builder/llpc[^/]*\\.h|.*/patch.*/llpc[^/]*\\.h|.*/tool/[^/]*\\.h'
CheckOptions:
  - { key: readability-identifier-naming.ParameterCase, value: camelBack }
  - { key: readability-identifier-naming.ParameterRemovePrefixes, value: 'p,b,pfn' }
  - { key: readability-identifier-naming.VariableCase, value: camelBack }
  - { key: readability-identifier-naming.VariableRemovePrefixes, value: 'p,b,pfn' }
  # Need to ignore static field 'ID' because otherwise clang-tidy dives into an llvm
  # include file and changes 'ID' to 'm_id' there, which is bad.
  - { key: readability-identifier-naming.ClassMemberIgnoreRegex, value: '^ID|mmSPI_.*$' }
  - { key: readability-identifier-naming.ClassMemberCase, value: camelBack }
  - { key: readability-identifier-naming.ClassMemberRemovePrefixes, value: 'p,b,pfn,m_p,m_b' }
  - { key: readability-identifier-naming.ClassMemberPrefix, value: m_ }
  - { key: readability-identifier-naming.ClassConstantCase, value: CamelCase }
  - { key: readability-identifier-naming.ClassConstantRemovePrefixes, value: 'p,b,pfn,m_p,m_b' }
  - { key: readability-identifier-naming.StaticVariableCase, value: CamelCase }
  - { key: readability-identifier-naming.StaticVariableRemovePrefixes, value: 'p,b,pfn,s_,s_p' }
  - { key: readability-identifier-naming.GlobalVariableIgnoreRegex, value: 'mmSPI_.*' }
  - { key: readability-identifier-naming.GlobalVariableCase, value: CamelCase }
  - { key: readability-identifier-naming.GlobalVariableRemovePrefixes, value: 'p,b,pfn,g_,g_p' }
  - { key: readability-identifier-naming.ConstantMemberIgnoreRegex, value: 'mmSPI_.*' }
  - { key: readability-identifier-naming.ConstantMemberCase, value: CamelCase }
  - { key: readability-identifier-naming.ConstantMemberPrefix, value: '' }
  - { key: readability-identifier-naming.PublicMemberCase, value: camelBack }
  - { key: readability-identifier-naming.PublicMemberIgnoreRegex, value: '^pSymName$' }
  - { key: readability-identifier-naming.PublicMemberPrefix, value: '' }
  - { key: readability-identifier-naming.PublicMemberRemovePrefixes, value: 'p,b,pfn,m_,m_p,m_b' }
  - { key: readability-identifier-naming.MemberCase, value: camelBack }
  - { key: readability-identifier-naming.MemberPrefix, value: m_ }
  - { key: readability-identifier-naming.MemberRemovePrefixes, value: 'p,b,pfn,m_p,m_b' }
  - { key: readability-identifier-naming.MethodCase, value: camelBack }
  - { key: readability-identifier-naming.MethodIgnoreRegex, value: '^Create$|^CreateACos$|^CreateACosh$|^CreateASin$|^CreateASinh$|^CreateATan$|^CreateATan2$|^CreateATanh$|^CreateBarrier$|^CreateBinaryIntrinsic$|^CreateCosh$|^CreateCrossProduct$|^CreateCubeFace.*$|^CreateDemoteToHelperInvocation$|^CreateDerivative$|^CreateDeterminant$|^CreateDotProduct$|^CreateEmitVertex$|^CreateEndPrimitive$|^CreateExp$|^CreateExtract.*$|^CreateFaceForward$|^CreateFClamp$|^CreateFindSMsb$|^CreateFma$|^CreateFMax$|^CreateFMax3$|^CreateFMid3$|^CreateFMin$|^CreateFMin3$|^CreateFMod$|^CreateFpTruncWithRounding$|^CreateFract$|^CreateFSign$|^CreateGet.*$|^CreateImage.*$|^CreateIndexDescPtr$|^CreateInsertBitField$|^CreateIntrinsic$|^CreateInverseSqrt$|^CreateIs.*$|^CreateKill$|^CreateLdexp$|^CreateLoad.*$|^CreateLog$|^CreateMapToInt32$|^CreateMatrix.*$|^CreateNormalizeVector$|^CreateOuterProduct$|^CreatePower$|^CreateQuantizeToFp16$|^CreateRead.*$|^CreateReflect$|^CreateRefract$|^CreateSAbs$|^CreateSinh$|^CreateSMod$|^CreateSmoothStep$|^CreateSSign$|^CreateSubgroup.*$|^CreateTan$|^CreateTanh$|^CreateTransposeMatrix$|^CreateUnaryIntrinsic$|^CreateVectorTimesMatrix$|^CreateWrite.*Output$|^Serialize$|^Merge$|^Destroy$|^ConvertColorBufferFormatToExportFormat$|^BuildShaderModule$|^BuildGraphicsPipeline$|^BuildComputePipeline$|^IsVertexFormatSupported$|^DumpSpirvBinary$|^BeginPipelineDump$|^EndPipelineDump$|^DumpPipelineBinary$|^DumpPipelineExtraInfo$|^GetShaderHash$|^GetPipelineHash$|^GetPipelineName$|^CreateShaderCache$|^ReadFromBuffer$|^GetSectionIndex$|^GetSymbolsBySectionIndex$|^GetSectionData$' }
  - { key: readability-identifier-naming.FunctionIgnoreRegex, value: 'EnableOuts|EnableErrs' }
  - { key: readability-identifier-naming.FunctionCase, value: camelBack }
  - { key: readability-identifier-naming.TypeCase, value: CamelCase }
  - { key: readability-identifier-naming.TypeRemovePrefixes, value: PFN_ }
" >.clang-tidy

  do_clang_tidy '-*,readability-identifier-naming' restricted

}

: # Remove unnecessary braces
removeBraces() {
  echo "
 # Do not modify SPIRV library.
HeaderFilterRegex: '.*/vfx/vfx.*\\.h|.*/vkgc.*\\.h|.*/lgc/include/.*\.h|.*/include/llpc\.h|.*/context/llpc[^/]*\.h|.*/util/llpc[^/]*\.h|.*/lower/llpc[^/]*\.h|.*/builder/llpc[^/]*\.h|.*/patch.*/llpc[^/]*\.h|.*/tool/*\.h'
CheckOptions:
  - { key: readability-braces-around-statements.ShortStatementLines, value: 2 }
  - { key: readability-braces-around-statements.AddBraces, value: 0 }
  - { key: readability-braces-around-statements.RemoveUnnecessaryBraces, value: 1 }
" > .clang-tidy

  do_clang_tidy '-*,readability-braces-around-statements'
}

: # Remove redundant comparisons with false or nullptr
removeRedundantComparisons() {
  echo "
 # Do not modify SPIRV library.
HeaderFilterRegex: '.*/vfx/vfx.*\\.h|.*/vkgc.*\\.h|.*/lgc/include/.*\.h|.*/include/llpc\.h|.*/context/llpc[^/]*\.h|.*/util/llpc[^/]*\.h|.*/lower/llpc[^/]*\.h|.*/builder/llpc[^/]*\.h|.*/patch.*/llpc[^/]*\.h|.*/tool/*\.h'
CheckOptions:
" > .clang-tidy

  do_clang_tidy '-*,readability-simplify-boolean-expr,readability-redundant-nullptr-comparison'
}

: # Remove redundant parentheses
removeRedundantParentheses() {
  echo "
 # Do not modify SPIRV library.
HeaderFilterRegex: '.*/vfx/vfx.*\\.h|.*/vkgc.*\\.h|.*/lgc/include/.*\.h|.*/include/llpc\.h|.*/context/llpc[^/]*\.h|.*/util/llpc[^/]*\.h|.*/lower/llpc[^/]*\.h|.*/builder/llpc[^/]*\.h|.*/patch.*/llpc[^/]*\.h|.*/tool/*\.h'
CheckOptions:
" > .clang-tidy

  do_clang_tidy '-*,readability-redundant-parentheses'
}

: # Move inline comments on parameters
moveInlineComments() {
  echo "
 # Do not modify SPIRV library.
HeaderFilterRegex: '.*/vfx/vfx.*\\.h|.*/vkgc.*\\.h|.*/lgc/include/.*\.h|.*/include/vkgcDefs\.h|.*/include/llpc\.h|.*/context/llpc[^/]*\.h|.*/util/llpc[^/]*\.h|.*/lower/llpc[^/]*\.h|.*/builder/llpc[^/]*\.h|.*/patch.*/llpc[^/]*\.h|.*/tool/*\.h'
CheckOptions:
" > .clang-tidy

  do_clang_tidy '-*,readability-declaration-inline-comment'
}

: # Apply clang-format
clangFormat() {
  clang_format=clang-format
  for file in `find include util tool llpc lgc llpc/translator/lib/SPIRV/SPIRVReader.cpp llpc/translator/lib/SPIRV/SPIRVInternal.h \
      -name imported -prune -o \
      -name test -prune -o \
      -name translator -prune -o \
      -name \*.cpp -print -o \
      -name \*.h -print`; do
    echo "clang-format $file"
    "$clang_format" "$file" >"$file".new && mv -f "$file".new "$file" || exit 1
  done
}

: # Mainline code

script="${0##*/}"

if [ ! -d llpc -o ! -d lgc ]; then
  echo "$script: switch LLPC/LGC to new coding style
Must be run in compiler directory" >&2
  exit 1
fi

xgl="$PWD/../xgl"
srcfrombuild=../../llpc
if [ ! -d "$xgl" -o ! -d "$xgl/icd" ]; then
  xgl="$PWD/../../.."
  srcfrombuild=../icd/api/compiler
  if [ ! -d "$xgl" -o ! -d "$xgl/icd" ]; then
    echo "$script: can't find xgl directory" >&2
    exit 1
  fi
fi
build="$xgl/build-switch-coding-style"
mkdir -p $build || exit 1
rm -f $build/CMakeCache.txt

if [ $# -ge 1 ]; then
  while true; do
    case "$1" in
      int32) convertInt32 || exit 1;;
      identifiers) renameIdentifiers || exit 1;;
      braces) removeBraces || exit 1;;
      comparisons) removeRedundantComparisons || exit 1;;
      parentheses) removeRedundantParentheses || exit 1;;
      comments) moveInlineComments || exit 1;;
      format) clangFormat || exit 1;;
      *) break;;
    esac
    shift
    [ $# -eq 0 ] && exit 0
  done
fi

echo "$script: switch LLPC to new coding style
Run in compiler directory. Modified clang_tidy and clang_apply_replacements must be on path.
Usage: $script transform...
where transform is one of:

  int32       Convert uint32_t to unsigned and int32_t to int
  identifiers Rename identifiers
  braces      Remove unnecessary braces
  comparisons Remove redundant comparisons with false or nullptr
  parentheses Remove redundant parentheses
  comments    Move parameter/field inline comment up
  format      Apply clang-format

" >&2
exit 2
