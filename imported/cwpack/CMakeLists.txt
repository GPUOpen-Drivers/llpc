##
 #######################################################################################################################
 #
 #  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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

project(LLPC_CWPACK C)

### Create CWPACK Library ############################################################################################
add_library(llpc_cwpack STATIC "")
install(TARGETS llpc_cwpack DESTINATION ${CMAKE_SOURCE_DIR}/lib/${CMAKE_BUILD_TYPE}${TARGET_ARCHITECTURE_BITS})

### Cached Project Options #############################################################################################
# No exposed CWPACK options

### Compiler Options ###################################################################################################
if(WIN32)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
        message(STATUS "Configured ${PROJECT_NAME} compiler options for MSVC.")
    else()
        message(FATAL_ERROR "Using unknown compiler")
    endif()
elseif(UNIX)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        # SEE: https://gcc.gnu.org/onlinedocs/gcc-6.2.0/gcc/Option-Summary.html#Option-Summary
        # for a list of all options and documentation.
        target_compile_options(llpc_cwpack PRIVATE -pthread -fPIC)

        message(STATUS "Configured ${PROJECT_NAME} compiler options for GCC.")
    elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        target_compile_options(llpc_cwpack PRIVATE
            -pthread
            -fPIC
            -Wall
            -Werror
        )
        target_compile_options(llpc_cwpack PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -std=c++0x
        >)
        message(WARNING "Clang is untested.")
        target_compile_options(llpc_cwpack PRIVATE -pthread -fPIC)
    else()
        message(FATAL_ERROR "Using unknown compiler.")
    endif()
endif()

### Defines/Includes/Sources ###########################################################################################
target_compile_definitions(llpc_cwpack PRIVATE COMPILE_FOR_${TARGET_ARCHITECTURE_ENDIANESS}_ENDIAN)

if(WIN32)
    if(TARGET_ARCHITECTURE_BITS EQUAL 32)
        target_compile_definitions(llpc_cwpack PRIVATE CWP_CALL=__fastcall)
    endif()
endif()

target_include_directories(llpc_cwpack
    PUBLIC
        ${PROJECT_SOURCE_DIR}/inc
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
    )

target_sources(llpc_cwpack PRIVATE
    ${PROJECT_SOURCE_DIR}/src/cwpack.c
)

### Link Libraries #####################################################################################################
# No external libraries required for CWPACK.

### Find headers and build source groups ###############################################################################
target_find_headers(llpc_cwpack)
target_source_groups(llpc_cwpack)

### Mark all options as advanced #######################################################################################
if(CWPACK_OPTIONS_MARK_ADVANCED)
    mark_grouped_as_advanced(llpc_CWPACK)
endif()
