##
 #######################################################################################################################
 #
 #  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

set(LLPC_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

include("${LLPC_SOURCE_DIR}/cmake/compilerutils.cmake")

# Macro to add continuations and its dependencies as LLVM external projects.
# This appends the project names to LLVM_EXTERNAL_PROJECTS and sets each LLVM_EXTERNAL_*_SOURCE_DIR,
# all in the caller's scope.
macro(add_continuations_projects)
  add_compilerutils_projects()
  if (NOT continuations IN_LIST LLVM_EXTERNAL_PROJECTS)
    if (NOT llvm_dialects IN_LIST LLVM_EXTERNAL_PROJECTS)
      list(APPEND LLVM_EXTERNAL_PROJECTS llvm_dialects)
      set(LLVM_EXTERNAL_LLVM_DIALECTS_SOURCE_DIR "${LLPC_SOURCE_DIR}/imported/llvm-dialects")
    endif()
    list(APPEND LLVM_EXTERNAL_PROJECTS Continuations)
    set(LLVM_EXTERNAL_CONTINUATIONS_SOURCE_DIR "${LLPC_SOURCE_DIR}/shared/continuations")
  endif()
endmacro()
