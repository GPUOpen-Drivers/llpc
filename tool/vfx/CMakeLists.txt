##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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

project(VFX C CXX)

### Create VFX Library ################################################################################################
add_library(vfx STATIC "")

target_compile_definitions(vfx PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)

if(LLPC_CLIENT_INTERFACE_MAJOR_VERSION)
    target_compile_definitions(vfx PRIVATE LLPC_CLIENT_INTERFACE_MAJOR_VERSION=${LLPC_CLIENT_INTERFACE_MAJOR_VERSION})
    target_compile_definitions(vfx PRIVATE PAL_CLIENT_INTERFACE_MAJOR_VERSION=${PAL_CLIENT_INTERFACE_MAJOR_VERSION})
endif()

if(LLPC_USE_EXPERIMENTAL_SHADER_CACHE_PIPELINES)
    target_compile_definitions(vfx PRIVATE LLPC_USE_EXPERIMENTAL_SHADER_CACHE_PIPELINES=1)
endif()

target_sources(vfx PRIVATE
    vfxParser.cpp
    vfxPipelineDoc.cpp
    vfxRenderDoc.cpp
    vfxSection.cpp
    vfxEnumsConverter.cpp
)

target_include_directories(vfx
PUBLIC
    ${PROJECT_SOURCE_DIR}
PRIVATE
    ${PROJECT_SOURCE_DIR}/../../imported/spirv
    ${PROJECT_SOURCE_DIR}/../../include
    ${XGL_ICD_PATH}/api/include/khronos
)

if(UNIX)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        target_compile_options(vfx PRIVATE -fno-strict-aliasing)
        target_compile_options(vfx PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++14 -fno-rtti>)

        target_compile_options(vfx PRIVATE -Wno-unused-parameter -Wno-shift-negative-value -Wno-type-limits -Wno-error=switch -Wno-error=sign-compare -Wno-error=parentheses -Wno-error=maybe-uninitialized -Wno-error=delete-non-virtual-dtor -Wno-sign-compare -Wno-error -Wunused-variable -Werror=unused-variable -Wunused-function -Werror=unused-function)
        target_compile_options(vfx PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused -Wno-unused-parameter -Wno-ignored-qualifiers -Wno-missing-field-initializers>)

        message(STATUS "Configured ${PROJECT_NAME} compiler options for GCC.")
    elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        target_compile_options(vfx PRIVATE
            -fvisibility-inlines-hidden
            -fcolor-diagnostics
            -Wall
            -Werror
        )
        target_compile_options(vfx PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -std=c++14
            -fno-rtti
        >)
        #message(STATUS "Configured compiler options for Clang.")
        message(WARNING "Clang is untested.")
    else()
        message(FATAL_ERROR "Using unknown compiler.")
    endif()
endif()
