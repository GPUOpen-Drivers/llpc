##
 #######################################################################################################################
 #
 #  Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

# TableGen for dialects
macro(set_dialects_tablegen_exe DIALECTS)
    set(${DIALECTS}_TABLEGEN_TARGET llvm-dialects-tblgen)
    if (EXISTS ${LLVM_TOOLS_BINARY_PATH}/llvm-dialects-tblgen)
        set(${DIALECTS}_TABLEGEN_EXE ${LLVM_TOOLS_BINARY_PATH}/llvm-dialects-tblgen)
    else()
        if(CMAKE_CROSSCOMPILING)
            set(${DIALECTS}_TABLEGEN_TARGET ${LLVM_DIALECTS_TABLEGEN_TARGET_HOST})
            set(${DIALECTS}_TABLEGEN_EXE ${LLVM_DIALECTS_TABLEGEN_EXE_HOST})
        else()
            set(${DIALECTS}_TABLEGEN_EXE $<TARGET_FILE:llvm-dialects-tblgen>)
        endif()
    endif()
endmacro()
