##
 #######################################################################################################################
 #
 #  Copyright (c) 2021-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

macro(llpc_standalone_pre_project_setup)
endmacro()

macro(llpc_standalone_post_project_setup)
    # Settings required for a standalone LLPC build that would otherwise be inherited from the driver.

    if(COMMAND cmake_policy)
        cmake_policy(SET CMP0003 NEW)
    endif(COMMAND cmake_policy)

    set(LLPC_STANDALONE_BUILD ON)
    set(LLPC_BUILD_TESTS ON)

    if(NOT PAL_SOURCE_DIR)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../pal)
            set(PAL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../pal)
        elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/pal)
            set(PAL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/pal)
        endif()
    endif()

    if(NOT GPURT_SOURCE_DIR)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../gpurt)
            set(GPURT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../gpurt)
        elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/gpurt)
            set(GPURT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/gpurt)
        endif()
    endif()

    if(EXISTS ${PROJECT_SOURCE_DIR}/../third_party)
        set(THIRD_PARTY ${PROJECT_SOURCE_DIR}/../third_party)
    elseif(EXISTS ${PROJECT_SOURCE_DIR}/../../../../third_party)
        set(THIRD_PARTY ${PROJECT_SOURCE_DIR}/../../../../third_party)
    endif()
    if (THIRD_PARTY)
        set(XGL_METROHASH_PATH ${THIRD_PARTY}/metrohash CACHE PATH "The path of metrohash.")
        set(XGL_CWPACK_PATH ${THIRD_PARTY}/cwpack CACHE PATH "The path of cwpack.")
        add_subdirectory(${XGL_METROHASH_PATH} ${PROJECT_BINARY_DIR}/metrohash)
        add_subdirectory(${XGL_CWPACK_PATH} ${PROJECT_BINARY_DIR}/cwpack)
    endif()

    # Enable LLPC if we found its prerequisites (and it is not explicitly disabled).
    if (NOT DEFINED ICD_BUILD_LLPC)
        if (THIRD_PARTY AND PAL_SOURCE_DIR)
            set(ICD_BUILD_LLPC ON)
        else()
            message(STATUS "Vulkan-LLPC prerequisites not found; disabling")
        endif()
    endif()
    if (ICD_BUILD_LLPC)
        set(LLPC_BUILD_TOOLS ON)

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
    endif()

    set(GPURT_CLIENT_INTERFACE_MAJOR_VERSION 999999)
    set(LLPC_CLIENT_INTERFACE_MAJOR_VERSION 999999)
    set(PAL_CLIENT_INTERFACE_MAJOR_VERSION 999999)

endmacro()
