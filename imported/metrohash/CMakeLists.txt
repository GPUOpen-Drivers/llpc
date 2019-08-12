##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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

cmake_minimum_required(VERSION 3.1)

project(LLPC_MetroHash VERSION 1.0.0 LANGUAGES CXX)

add_library(llpc_metrohash STATIC "")

target_include_directories(llpc_metrohash PUBLIC inc)

target_sources(llpc_metrohash PRIVATE inc/metrohash.h
                                      inc/metrohash64.h
                                      inc/metrohash128.h)

target_sources(llpc_metrohash PRIVATE src/platform.h
                                      src/metrohash64.cpp
                                      src/metrohash128.cpp)

set_target_properties(llpc_metrohash PROPERTIES CXX_STANDARD              11
                                                CXX_STANDARD_REQUIRED     ON
                                                CXX_EXTENSIONS            OFF
                                                POSITION_INDEPENDENT_CODE ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")

    # [GCC] Exceptions
    #   https://gcc.gnu.org/onlinedocs/libstdc++/manual/using_exceptions.html
    #
    # [GCC] Options Controlling C++ Dialect
    #   https://gcc.gnu.org/onlinedocs/gcc-8.1.0/gcc/C_002b_002b-Dialect-Options.html
    target_compile_options(llpc_metrohash PRIVATE
        -fno-exceptions  # Disable exception handling support.
        -fno-rtti)       # Disable run-time type information support.

    # [GCC] Options to Request or Suppress Warnings
    #   https://gcc.gnu.org/onlinedocs/gcc-8.1.0/gcc/Warning-Options.html
    target_compile_options(llpc_metrohash PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror)

elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")

    # [MSVC] Exception Handling Model
    #    https://msdn.microsoft.com/en-us/library/1deeycx5.aspx
    #
    # [MSVC] Enable Run-Time Type Information
    #    https://msdn.microsoft.com/en-us/library/we6hfdy0.aspx
    #
    # [MSVC] Buffer Security Check
    #    https://msdn.microsoft.com/en-us/library/8dbf701c.aspx
    target_compile_options(llpc_metrohash PRIVATE
        /EHsc # Catches only C++ exceptions and assumes
              # functions declared as extern "C" never throw a C++ exception.
        /GR-  # Disables run-time type information.
        /GS-) # Disables detection of buffer overruns.

    # [MSVC] Warning Level
    #    https://msdn.microsoft.com/en-us/library/thxezb7y.aspx
    target_compile_options(llpc_metrohash PRIVATE
        /W4  # Enable warning level 4.
        /WX) # Treat warnings as errors.

else()
    message(FATAL_ERROR "Compiler ${CMAKE_CXX_COMPILER_ID} is not supported!")
endif()
