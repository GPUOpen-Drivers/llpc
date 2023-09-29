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

project(DUMPER C CXX)

### Create DUMPER Library ################################################################################################
add_library(dumper_base OBJECT "")

### Cached Project Options #############################################################################################
option(DUMPER_ENABLE_WERROR "Build ${PROJECT_NAME} with more errors" OFF)

### Compiler Options ###################################################################################################
include(../../cmake/CompilerFlags.cmake)
set_compiler_options(dumper_base ${DUMPER_ENABLE_WERROR})

# We get link errors on Linux
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fPIC)
endif()

### Defines/Includes/Sources ###########################################################################################
target_compile_definitions(dumper_base PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)
target_compile_definitions(dumper_base PRIVATE PAL_CLIENT_INTERFACE_MAJOR_VERSION=${PAL_CLIENT_INTERFACE_MAJOR_VERSION})

target_link_libraries(dumper_base PUBLIC metrohash)
target_link_libraries(dumper_base PUBLIC cwpack)
target_link_libraries(dumper_base PUBLIC vkgc_headers)
target_link_libraries(dumper_base PUBLIC
    khronos_vulkan_interface
    khronos_spirv_interface
)

target_include_directories(dumper_base
    PUBLIC
        .
    PUBLIC
        ${XGL_PAL_PATH}/inc/core
        ${XGL_PAL_PATH}/inc/util
        ${LLVM_INCLUDE_DIRS}
        ${XGL_VKGC_PATH}/util
)

target_sources(dumper_base PRIVATE
    ../../util/vkgcUtil.cpp
    vkgcPipelineDumperRegs.cpp
)

add_library(dumper STATIC
    ../../util/vkgcElfReader.cpp
    vkgcPipelineDumper.cpp
)
if(ICD_BUILD_LLPC)
    target_compile_definitions(dumper PRIVATE ICD_BUILD_LLPC)
endif()
target_link_libraries(dumper PUBLIC dumper_base)

