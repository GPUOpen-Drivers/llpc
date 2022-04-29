##
 #######################################################################################################################
 #
 #  Copyright (c) 2020-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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

### Cached Project Options #############################################################################################
option(DUMPER_ENABLE_WERROR "Build ${PROJECT_NAME} with more errors" OFF)

### Compiler Options ###################################################################################################
include(../../cmake/CompilerFlags.cmake)
set_compiler_options(dumper ${DUMPER_ENABLE_WERROR})

### Defines/Includes/Sources ###########################################################################################
target_compile_definitions(dumper PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)
target_compile_definitions(dumper PRIVATE PAL_CLIENT_INTERFACE_MAJOR_VERSION=${PAL_CLIENT_INTERFACE_MAJOR_VERSION})
if(ICD_BUILD_LLPC)
    target_compile_definitions(dumper PRIVATE ICD_BUILD_LLPC)
endif()

target_link_libraries(dumper PUBLIC metrohash)
target_link_libraries(dumper PRIVATE cwpack)
target_link_libraries(dumper PRIVATE vkgc_headers)
target_link_libraries(dumper PRIVATE
    khronos_vulkan_interface
    khronos_spirv_interface
)

if(LLPC_ENABLE_SHADER_CACHE)
    target_compile_definitions(dumper PRIVATE LLPC_ENABLE_SHADER_CACHE=1)
endif()

target_include_directories(dumper
    PUBLIC
        ${XGL_VKGC_PATH}/include
    PRIVATE
        ${XGL_PAL_PATH}/inc/core
        ${XGL_PAL_PATH}/inc/util
        ${LLVM_INCLUDE_DIRS}
        ${XGL_VKGC_PATH}/util
)

target_sources(dumper PRIVATE
    ../../util/vkgcUtil.cpp
    ../../util/vkgcElfReader.cpp
    vkgcPipelineDumper.cpp
    vkgcPipelineDumperRegs.cpp
)
