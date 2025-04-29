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

# Build LLVM, using previously set up LLVM_EXTERNAL_PROJECTS and LLVM_EXTERNAL_*_SOURCE_DIR.
# Relies on findllvm.cmake being run first.

if (NOT LLPC_LLVM_SRC_PATH)
    message(FATAL_ERROR "No LLPC_LLVM_SRC_PATH specified")
endif()

# Set cached options.
set(LLVMRAYTRACING_BUILD_TESTS ${LLPC_BUILD_TESTS})
set(LLVM_TARGETS_TO_BUILD AMDGPU CACHE STRING "LLVM targets to build")
set(LLVM_BUILD_TESTS OFF CACHE BOOL "LLVM build tests")
set(LLVM_BUILD_TOOLS ${LLPC_BUILD_LLVM_TOOLS} CACHE BOOL "LLVM build tools")
set(LLVM_BUILD_UTILS OFF CACHE BOOL "LLVM build utils")
set(LLVM_INCLUDE_DOCS OFF CACHE BOOL "LLVM include docs")
set(LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL "LLVM include examples")
set(LLVM_INCLUDE_GO_TESTS OFF CACHE BOOL "LLVM include go tests")
set(LLVM_INCLUDE_TESTS ${LLPC_BUILD_TESTS} CACHE BOOL "LLVM include tests")
set(LLVM_INCLUDE_TOOLS ON CACHE BOOL "LLVM include tools")
set(LLVM_INCLUDE_UTILS ON CACHE BOOL "LLVM include utils")
set(LLVM_ENABLE_TERMINFO OFF CACHE BOOL "LLVM enable terminfo")
set(LLVM_RAM_PER_TABLEGEN_JOB 4000 CACHE STRING "LLVM RAM per tablegen job")
set(LLVM_RAM_PER_LINK_JOB 5000 CACHE STRING "LLVM RAM per link job")
if("${CMAKE_BUILD_TYPE}" STREQUAL Debug OR CMAKE_CONFIGURATION_TYPES)
    # Build optimized version of llvm-tblgen even in debug builds, for faster build times.
    set(LLVM_OPTIMIZED_TABLEGEN ON CACHE BOOL "Build optimized llvm-tblgen")
    if(LLVM_OPTIMIZED_TABLEGEN AND WIN32)
        if(CMAKE_GENERATOR MATCHES "Ninja")
            set(CROSS_TOOLCHAIN_FLAGS_NATIVE "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}" CACHE STRING
                "Toolchain flags for native build" FORCE)

            # Fail early to avoid the dreaded -ologo error.
            if(CMAKE_VERSION VERSION_LESS "3.27")
                message(FATAL_ERROR "Using LLVM_OPTIMIZED_TABLEGEN in a Debug build requires CMake 3.27 or higher."
                                    " The current CMake version is ${CMAKE_VERSION}.")
            endif()
        else()
            list(APPEND LLVM_CROSS_TOOLCHAIN_FLAGS_NATIVE "-D Python3_ROOT_DIR=${Python3_ROOT_DIR}")
            set(CROSS_TOOLCHAIN_FLAGS_NATIVE ${LLVM_CROSS_TOOLCHAIN_FLAGS_NATIVE} CACHE STRING "" FORCE)
        endif()
    endif()
endif()

# This will greatly speed up debug builds because we won't be listing all the symbols with llvm-nm.
set(LLVM_BUILD_LLVM_C_DYLIB OFF CACHE BOOL "LLVM build LLVM-C dylib")

# Remove /nologo from CMAKE_RC_FLAGS to avoid getting an error from specifying it twice in LLVM.
if (CMAKE_RC_FLAGS)
    string(REPLACE "/nologo" "" CMAKE_RC_FLAGS ${CMAKE_RC_FLAGS})
endif()

# Build LLVM.
if (NOT LLPC_LLVM_BUILD_PATH)
    set(LLPC_LLVM_BUILD_PATH ${PROJECT_BINARY_DIR}/llvm)
endif()

add_subdirectory(${LLPC_LLVM_SRC_PATH} ${LLPC_LLVM_BUILD_PATH} EXCLUDE_FROM_ALL)

# Get LLVMConfig onto cmake path.
list(APPEND CMAKE_MODULE_PATH
    "${LLPC_LLVM_BUILD_PATH}/lib/cmake/llvm"
    "${LLPC_LLVM_BUILD_PATH}/${CMAKE_CFG_INTDIR}/lib/cmake/llvm" # Workaround for VS generator with older LLVM.
)

if (NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # Export LLVM build path for client driver.
    # TODO: Change uses to LLPC_LLVM_BUILD_PATH.
    set(XGL_LLVM_BUILD_PATH ${LLPC_LLVM_BUILD_PATH} PARENT_SCOPE)
endif()

# Extract LLVM revision number for code outside the LLPC repository to use.
file(READ "${LLPC_LLVM_SRC_PATH}/include/llvm/Config/llvm-config.h.cmake" LLVM_CONFIG_HEADER)
string(REGEX MATCH "#define LLVM_MAIN_REVISION ([0-9]+)" "\\1" _ "${LLVM_CONFIG_HEADER}")
set(LLVM_MAIN_REVISION "${CMAKE_MATCH_1}")
if (NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(LLVM_MAIN_REVISION ${LLVM_MAIN_REVISION} PARENT_SCOPE)
endif()

# Some of the games using old versions of the tcmalloc lib are crashing
# when allocating aligned memory. C++17 enables aligned new by default,
# so we need to disable it to prevent those crashes.
if (ICD_BUILD_LLPC AND NOT WIN32)
    llvm_map_components_to_libnames(llvm_libs
        AMDGPUAsmParser
        AMDGPUCodeGen
        AMDGPUDisassembler
        AMDGPUInfo
        Analysis
        BinaryFormat
        Core
        Coroutines
        BitReader
        BitWriter
        CodeGen
        InstCombine
        ipo
        IRPrinter
        IRReader
        Linker
        LTO
        MC
        Passes
        ScalarOpts
        Support
        Target
        TransformUtils
    )
    foreach (lib ${llvm_libs})
        target_compile_options(${lib} PRIVATE "-fno-aligned-new")
    endforeach()
endif()
