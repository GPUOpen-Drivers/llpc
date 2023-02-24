##
 #######################################################################################################################
 #
 #  Copyright (c) 2021-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

### Create disassembler library ################################################################################################

add_llvm_library(LLVMlgcdis
    Disassembler.cpp
    PalMetadataRegs.cpp
LINK_COMPONENTS
    AllTargetsDescs
    AllTargetsDisassemblers
    AllTargetsInfos
    BinaryFormat
    MC
    MCDisassembler
    Object
    Support
    Target
    TargetParser
)

### Compiler Options ###################################################################################################
include(../../cmake/CompilerFlags.cmake)
set_compiler_options(LLVMlgcdis ${LLPC_ENABLE_WERROR})

### Defines/Includes/Sources ###########################################################################################
target_include_directories(LLVMlgcdis
PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../interface>
    $<INSTALL_INTERFACE:../interface>
)

add_subdirectory(../tool/lgcdis lgcdis)
