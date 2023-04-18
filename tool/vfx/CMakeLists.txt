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

project(VFX C CXX)

### Create VFX Library ################################################################################################
add_library(vfx STATIC "")

### Cached Project Options #############################################################################################
option(VFX_ENABLE_WERROR "Build ${PROJECT_NAME} with more errors" OFF)

target_compile_definitions(vfx PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)

target_sources(vfx PRIVATE
    vfxParser.cpp
    vfxPipelineDoc.cpp
    vfxSection.cpp
    vfxEnumsConverter.cpp
    vfxVkSection.cpp
)

target_compile_definitions(vfx PRIVATE VFX_SUPPORT_VK_PIPELINE SH_EXPORTING)

target_include_directories(vfx PUBLIC ${PROJECT_SOURCE_DIR})

target_link_libraries(vfx PRIVATE spvgen_static vkgc_headers khronos_vulkan_interface khronos_spirv_interface)

include(../../cmake/CompilerFlags.cmake)
set_compiler_options(vfx ${VFX_ENABLE_WERROR})
