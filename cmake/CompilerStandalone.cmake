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

# Settings required for a standalone LLPC build that would otherwise be inherited from the driver.

if(COMMAND cmake_policy)
    cmake_policy(SET CMP0003 NEW)
endif(COMMAND cmake_policy)

set(ICD_BUILD_LLPC ON)
set(LLPC_BUILD_TESTS ON)
set(LLPC_BUILD_TOOLS ON)

set(XGL_VKGC_PATH ${CMAKE_CURRENT_SOURCE_DIR})

# This is so spvgen can find vfx in an LLPC standalone build.
set(XGL_LLPC_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../pal)
    set(XGL_PAL_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../pal)
else()
    set(XGL_PAL_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/pal)
endif()

if(EXISTS ${PROJECT_SOURCE_DIR}/../third_party)
    set(THIRD_PARTY ${PROJECT_SOURCE_DIR}/../third_party)
else()
    set(THIRD_PARTY ${PROJECT_SOURCE_DIR}/../../../../third_party)
endif()
set(XGL_METROHASH_PATH ${THIRD_PARTY}/metrohash CACHE PATH "The path of metrohash.")
set(XGL_CWPACK_PATH ${THIRD_PARTY}/cwpack CACHE PATH "The path of cwpack.")
add_subdirectory(${XGL_METROHASH_PATH} ${PROJECT_BINARY_DIR}/metrohash)
add_subdirectory(${XGL_CWPACK_PATH} ${PROJECT_BINARY_DIR}/cwpack)

# External Vulkan headers path
if(EXISTS ${PROJECT_SOURCE_DIR}/../Vulkan-Headers)
    set(VULKAN_HEADERS_PATH ${PROJECT_SOURCE_DIR}/../Vulkan-Headers CACHE PATH "The path of Vulkan headers.")
endif()

### Khronos Interface
add_library(khronos_vulkan_interface INTERFACE)
if(EXISTS ${VULKAN_HEADERS_PATH})
    target_include_directories(khronos_vulkan_interface INTERFACE ${VULKAN_HEADERS_PATH}/include)
    target_compile_definitions(khronos_vulkan_interface INTERFACE EXTERNAL_VULKAN_HEADERS=1)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../xgl)
    target_include_directories(khronos_vulkan_interface INTERFACE ../xgl/icd/api/include/khronos)
else()
    target_include_directories(khronos_vulkan_interface INTERFACE ../../../icd/api/include/khronos)
endif()

set(GPURT_CLIENT_INTERFACE_MAJOR_VERSION 9999 CACHE STRING "")
set(LLPC_CLIENT_INTERFACE_MAJOR_VERSION "LLPC_INTERFACE_MAJOR_VERSION")
set(PAL_CLIENT_INTERFACE_MAJOR_VERSION 9999 CACHE STRING "")
