##
 #######################################################################################################################
 #
 #  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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

project(DUMPER C CXX)

### Create DUMPER Library ################################################################################################
add_library(dumper STATIC "")

### Compiler Options ###################################################################################################
if(UNIX)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        # SEE: https://gcc.gnu.org/onlinedocs/gcc-6.2.0/gcc/Option-Summary.html#Option-Summary
        # for a list of all options and documentation.
        #target_compile_options(dumper PRIVATE option)
        target_compile_options(dumper PRIVATE
            -Wno-unused-parameter
            -Wno-type-limits
            -Wno-switch
            -Wno-parentheses
            -Wno-maybe-uninitialized
            -Wno-delete-non-virtual-dtor
            -Wno-sign-compare
            -Wno-delete-incomplete
            -Wunused-variable
            -Werror=unused-variable
            -Wunused-function
            -Werror=unused-function
        )

        target_compile_options(dumper PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -Wno-unused
            -Wno-ignored-qualifiers
            -Wno-missing-field-initializers
            -Wno-invalid-offsetof           # offsetof within non-standard-layout type 'x' is undefined
        >)

        target_compile_options(dumper PRIVATE -fno-strict-aliasing)

        target_compile_options(dumper PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -std=c++14
            -fno-rtti
            -fPIC
        >)

        message(STATUS "Configured ${PROJECT_NAME} compiler options for GCC.")
    elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        target_compile_options(dumper PRIVATE
            -fvisibility-inlines-hidden
            -fcolor-diagnostics
            -Wall
            -Werror
            -Wno-invalid-offsetof
            -Wno-missing-braces
        )
        target_compile_options(dumper PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -std=c++14
            -fno-rtti
            -fPIC
        >)
        #message(STATUS "Configured compiler options for Clang.")
        message(WARNING "Clang is untested.")
    else()
        message(FATAL_ERROR "Using unknown compiler.")
    endif()
elseif(WIN32)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
        # CMAKE-TODO: These are /W4 (level 4) warnings
        target_compile_options(dumper
            PRIVATE # Warnings in interface and src
                /wd4005 # 'DEBUG' : macro redefinition ??? Defined in toolchain ??? importedllvmincludellvm/Support/Debug.h
                /wd4018 # '<' : signed/unsigned mismatch
                /wd4100 # unreferenced formal parameter
                /wd4127 # conditional expression is constant
                /wd4141 # 'inline' : used more than once
                /wd4146 # unary minus operator applied to unsigned type, result still unsigned
                /wd4189 # local variable is initialized but not referenced
                /wd4201 # nonstandard extension used : nameless struct/union
                /wd4244 # 'X' : conversion from 'Y' to 'Z', possible loss of data
                /wd4245 # 'X' : conversion from 'Y' to 'Z', signed/unsigned mismatch
                /wd4250 # 'X': inherits 'Y' via dominance
                /wd4291 # no matching operator delete found; memory will not be freed if initialization throws an exception
                /wd4267 # 'return' : conversion from 'size_t' to 'unsigned int', possible loss of data
                /wd4389 # '==' : signed/unsigned mismatch
                /wd4505 # unreferenced local function has been removed
                /wd4510 # default constructor could not be generated
                /wd4512 # assignment operator could not be generated
                /wd4610 # struct 'X' can never be instantiated - user defined constructor required
                /wd4624 # destructor could not be generated because a base class destructor is inaccessible or deleted
                /wd4702 # unreachable code
                /wd4706 # assignment within conditional expression
                /wd4800 # forcing value to bool 'true' or 'false' (performance warning)
                /wd6246 # Local declaration of 'S' hides declaration of the same name in outer scope
                /wd6323 # Use of arithmetic operator on Boolean type(s)
        )

        target_compile_definitions(dumper PRIVATE _SCL_SECURE_NO_WARNINGS)
        target_compile_definitions(dumper PRIVATE _CRT_SECURE_NO_WARNINGS)

        message(STATUS "Configured ${PROJECT_NAME} compiler options for MSVC.")
    else()
        message(FATAL_ERROR "Using unknown compiler")
    endif()
endif()

### Defines/Includes/Sources ###########################################################################################
target_compile_definitions(dumper PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)
target_compile_definitions(dumper PRIVATE PAL_CLIENT_INTERFACE_MAJOR_VERSION=${PAL_CLIENT_INTERFACE_MAJOR_VERSION})
if (LLPC_CLIENT_INTERFACE_MAJOR_VERSION)
    target_compile_definitions(dumper PRIVATE LLPC_CLIENT_INTERFACE_MAJOR_VERSION=${LLPC_CLIENT_INTERFACE_MAJOR_VERSION})
endif()
if(ICD_BUILD_LLPC)
    target_compile_definitions(dumper PRIVATE ICD_BUILD_LLPC)
endif()

if(LLPC_USE_EXPERIMENTAL_SHADER_CACHE_PIPELINES)
    target_compile_definitions(dumper PRIVATE LLPC_USE_EXPERIMENTAL_SHADER_CACHE_PIPELINES=1)
endif()

# metrohash
target_link_libraries(dumper PUBLIC metrohash)
target_compile_definitions(dumper PUBLIC SINGLE_EXTERNAL_METROHASH)

target_link_libraries(dumper PRIVATE cwpack)

target_include_directories(dumper
    PUBLIC
        ${XGL_LLPC_PATH}/../include
    PRIVATE
        ${XGL_LLPC_PATH}/../include
        ${XGL_PAL_PATH}/inc/core
        ${XGL_PAL_PATH}/inc/util
        ${LLVM_INCLUDE_DIRS}
        ${XGL_ICD_PATH}/api/include/khronos
        ${XGL_LLPC_PATH}/../util
)

target_sources(dumper PRIVATE
    ../../util/vkgcUtil.cpp
    ../../util/vkgcElfReader.cpp
    vkgcPipelineDumper.cpp
    vkgcPipelineDumperRegs.cpp
)
