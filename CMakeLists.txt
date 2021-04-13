##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
cmake_minimum_required(VERSION 3.13.4)

project(VKGC LANGUAGES C CXX)

### Standalone LLPC build handling
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(cmake/CompilerStandalone.cmake)
endif()

### Top-level VKGC Interface ###
add_library(vkgc INTERFACE)

### Expose header files ################################################################################################
target_include_directories(vkgc
    INTERFACE
        ${PROJECT_SOURCE_DIR}/include
)

### external SPIRV headers #########################################################
if(EXISTS ${PROJECT_SOURCE_DIR}/../SPIRV-Headers)
    set(SPIRV_HEADERS_PATH ${PROJECT_SOURCE_DIR}/../SPIRV-Headers CACHE PATH "The path of SPIRV headers.")
elseif(EXISTS ${PROJECT_SOURCE_DIR}/../../../../SPIRV-Headers)
    set(SPIRV_HEADERS_PATH ${PROJECT_SOURCE_DIR}/../../../../SPIRV-Headers CACHE PATH "The path of SPIRV headers.")
endif()

### Interface Target ###################################################################################################
### SPIRV Interface ###
add_library(khronos_spirv_interface INTERFACE)

if(EXISTS ${SPIRV_HEADERS_PATH})
    target_include_directories(khronos_spirv_interface
        INTERFACE
            ${SPIRV_HEADERS_PATH}/include/spirv
            ${PROJECT_SOURCE_DIR}/include/khronos
    )
    target_compile_definitions(khronos_spirv_interface
        INTERFACE
            EXTERNAL_SPIRV_HEADERS=1
    )
else()
    target_include_directories(khronos_spirv_interface
        INTERFACE
            ${PROJECT_SOURCE_DIR}/include/khronos
    )
endif()

### VKGC build LLPC ################################################################
if(ICD_BUILD_LLPC)
    target_include_directories(vkgc
        INTERFACE
            ${PROJECT_SOURCE_DIR}/llpc/include
            ${PROJECT_SOURCE_DIR}/lgc/include
    )

    add_subdirectory(llpc ${PROJECT_BINARY_DIR}/llpc)

    # Export the LLVM and spvgen build paths so that they are available in XGL,
    # if this is not an LLPC standalone build.
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(XGL_LLVM_BUILD_PATH ${XGL_LLVM_BUILD_PATH} PARENT_SCOPE)
        if(DEFINED XGL_SPVGEN_BUILD_PATH)
            set(XGL_SPVGEN_BUILD_PATH ${XGL_SPVGEN_BUILD_PATH} PARENT_SCOPE)
        endif()
    endif()

    target_link_libraries(vkgc INTERFACE llpc)
endif()

