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

### Top-level VKGC Interface ###
add_library(vkgc INTERFACE)

### VKGC header-only library ###
add_library(vkgc_headers INTERFACE)

target_link_libraries(vkgc_headers INTERFACE llpc_version)

### Options that affect the headers ####################################################################################

#if LLPC_RAY_TRACING
if(LLPC_RAY_TRACING)
    if(NOT LLPC_IS_STANDALONE)
        target_compile_definitions(vkgc_headers INTERFACE HAVE_GPURT_SHIM)
    endif()

    target_compile_definitions(vkgc_headers INTERFACE LLPC_RAY_TRACING)
    target_compile_definitions(vkgc_headers INTERFACE GPURT_CLIENT_INTERFACE_MAJOR_VERSION=${GPURT_CLIENT_INTERFACE_MAJOR_VERSION})
endif()
#endif

target_link_libraries(vkgc INTERFACE vkgc_headers)

### Expose header files ################################################################################################
target_include_directories(vkgc_headers
    INTERFACE
        ${PROJECT_SOURCE_DIR}/include
)

### external SPIRV headers #########################################################
if (NOT SPIRV_HEADERS_PATH)
    if(EXISTS ${PROJECT_SOURCE_DIR}/../SPIRV-Headers)
        set(SPIRV_HEADERS_PATH ${PROJECT_SOURCE_DIR}/../SPIRV-Headers CACHE PATH "The path of SPIRV headers.")
    elseif(EXISTS ${PROJECT_SOURCE_DIR}/../../../../SPIRV-Headers)
        set(SPIRV_HEADERS_PATH ${PROJECT_SOURCE_DIR}/../../../../SPIRV-Headers CACHE PATH "The path of SPIRV headers.")
    endif()
endif()

### Interface Target ###################################################################################################
### SPIRV Interface ###
add_library(khronos_spirv_interface INTERFACE)

if(EXISTS ${SPIRV_HEADERS_PATH})
    target_include_directories(khronos_spirv_interface
        INTERFACE
            ${SPIRV_HEADERS_PATH}/include
            ${PROJECT_SOURCE_DIR}/include/khronos
    )
    if (NOT SPIRV_HEADERS_PATH_INTERNAL)
        target_compile_definitions(khronos_spirv_interface
            INTERFACE
                EXTERNAL_SPIRV_HEADERS=1
        )
    endif()
else()
    target_include_directories(khronos_spirv_interface
        INTERFACE
            ${PROJECT_SOURCE_DIR}/include/khronos
    )
endif()

if(LLPC_BUILD_TOOLS)
# SPVGEN
if(EXISTS ${PROJECT_SOURCE_DIR}/../spvgen)
    set(XGL_SPVGEN_PATH ${PROJECT_SOURCE_DIR}/../spvgen CACHE PATH "Specify the path to SPVGEN.")
elseif(EXISTS ${PROJECT_SOURCE_DIR}/../xgl/tools/spvgen)
    set(XGL_SPVGEN_PATH ${PROJECT_SOURCE_DIR}/../xgl/tools/spvgen CACHE PATH "Specify the path to SPVGEN.")
else()
    set(XGL_SPVGEN_PATH ${PROJECT_SOURCE_DIR}/../../../tools/spvgen CACHE PATH "Specify the path to SPVGEN.")
endif()

if(EXISTS ${XGL_SPVGEN_PATH})
    set(XGL_SPVGEN_BUILD_PATH ${CMAKE_BINARY_DIR}/spvgen)
    add_subdirectory(${XGL_SPVGEN_PATH} ${XGL_SPVGEN_BUILD_PATH} EXCLUDE_FROM_ALL)
endif()

endif(LLPC_BUILD_TOOLS)

if(ICD_BUILD_LLPC)
    # Generate Strings for LLPC standalone tool and vkgc_gpurtshim
    add_subdirectory(util ${PROJECT_BINARY_DIR}/util)
    add_subdirectory(gfxruntime ${PROJECT_BINARY_DIR}/gfxruntime)
endif()
