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

if (NOT LLPC_LLVM_SRC_PATH)
    # Find LLVM source. Allow client driver to override using its own name for overlay builds.
    set(DEFAULT_LLPC_LLVM_SRC_PATH ${XGL_LLVM_SRC_PATH})
    if (NOT DEFAULT_LLPC_LLVM_SRC_PATH)
        if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../imported/llvm-project/llvm)
            set(DEFAULT_LLPC_LLVM_SRC_PATH ${CMAKE_CURRENT_LIST_DIR}/../imported/llvm-project/llvm)
        elseif(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../../llvm-project/llvm)
            set(DEFAULT_LLPC_LLVM_SRC_PATH ${CMAKE_CURRENT_LIST_DIR}/../../llvm-project/llvm)
        endif()
    endif()
    set(LLPC_LLVM_SRC_PATH ${DEFAULT_LLPC_LLVM_SRC_PATH} CACHE PATH "Specify the path to LLVM." FORCE)
endif()

if (NOT EXISTS ${LLPC_LLVM_SRC_PATH}/include/llvm/IR/LLVMContext.h)
    message(FATAL_ERROR "LLVM source not found at ${LLPC_LLVM_SRC_PATH}")
endif()
