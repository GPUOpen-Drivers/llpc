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

# Include this file to set LLVM_MAIN_REVISION, for when it is needed at cmake level rather than C++ level.

if (NOT LLVM_MAIN_REVISION)
  # A sneaky way to get the LLVM source directory, assuming we are included from a LLVM external
  # project such as LGC or LLPCFE.
  get_filename_component(LLVM_SOURCE_DIR "${CPACK_RESOURCE_FILE_LICENSE}" DIRECTORY)
  if (NOT LLVM_SOURCE_DIR)
      message(FATAL_ERROR "LLVM_SOURCE_DIR not found")
  endif()

  # Scrape LLVM_MAIN_REVISION out of llvm-config.h.cmake. If not found, set to a high number.
  set(LLVM_CONFIG_H_NAME "${LLVM_SOURCE_DIR}/include/llvm/Config/llvm-config.h.cmake")
  file(READ "${LLVM_CONFIG_H_NAME}" LLVM_CONFIG_H_CONTENTS)
  string(REGEX REPLACE "^.* LLVM_MAIN_REVISION ([0-9]+).*$" "\\1" LLVM_MAIN_REVISION "${LLVM_CONFIG_H_CONTENTS}")
  if ("${LLVM_MAIN_REVISION}" STREQUAL "${LLVM_CONFIG_H_CONTENTS}")
      set(LLVM_MAIN_REVISION 999999999)
  endif()
endif()

