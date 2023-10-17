##
 #######################################################################################################################
 #
 #  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

### LGC Standalone Compiler ############################################################################################
set(LLVM_LINK_COMPONENTS
    Core
    AMDGPUAsmParser
    AMDGPUCodeGen
    AMDGPUDisassembler
    Analysis
    AsmParser
    BitWriter
    CodeGen
    IRPrinter
    Passes
    Support
    Target
)

add_llvm_tool(lgc
    lgc.cpp
)

# lgc is linked in separately to account for both static and dynamic library
# builds.
llvm_map_components_to_libnames(extra_llvm_libs lgc Continuations)
target_link_libraries(lgc PRIVATE ${extra_llvm_libs})

target_compile_definitions(lgc PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)

target_include_directories(lgc
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../interface
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

set_compiler_options(lgc ${LLPC_ENABLE_WERROR})
