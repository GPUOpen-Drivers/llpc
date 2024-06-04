##
 #######################################################################################################################
 #
 #  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

add_library(gfxruntime)

# Locate python binary
# No particular version of python3 is necessary
find_package(Python3
    # Disable information messages
    QUIET
    # Python3 is required to run the shader compile script
    REQUIRED
    # Only the interpreter is required, we don't need the Compiler, Development, or NumPy
    COMPONENTS Interpreter
)

# Locate dxc binary.
#if _WIN32
if(WIN32)
    if (NOT EXISTS "${DXC_PATH}")
        message(FATAL_ERROR "Unable to find DirectXShaderCompiler directory: ${DXC_PATH}")
    endif()
endif()
#endif
if (NOT DXC_PATH)
    find_program(DXC_PATH dxc)
    if ("${DXC_PATH}" STREQUAL "DXC_PATH-NOTFOUND")
        message(FATAL_ERROR "Could not find shader compiler tool dxc.")
    endif()
endif()

set(CMAKE_CURRENT_SOURCE_DIR ${PROJECT_SOURCE_DIR}/gfxruntime)
set(CMAKE_CURRENT_BINARY_DIR ${PROJECT_BINARY_DIR}/gfxruntime)

### Auto-generated advanced blend runtime library for graphics shaders #################################################
set(GEN_ADVANCED_BLEND_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tools/CompileAdvancedBlendShader.py")
set(ADVANCED_BLEND_INPUT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/AdvancedBlend.hlsl")
set(ADVANCED_BLEND_OUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/g_AdvancedBlendLibrary_spv.h")
add_custom_command(
    OUTPUT
        ${ADVANCED_BLEND_OUT_FILE}
    DEPENDS
        ${ADVANCED_BLEND_INPUT_FILE}
        ${GEN_ADVANCED_BLEND_SCRIPT}
    COMMAND Python3::Interpreter "${GEN_ADVANCED_BLEND_SCRIPT}"
        --compilerPath "${DXC_PATH}"
        --shaderDir "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders"
        --outputDir "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "Generating advanced blend runtime library"
)

target_sources(gfxruntime
    PRIVATE
        ${ADVANCED_BLEND_OUT_FILE}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/GfxRuntimeLibrary.cpp
)
target_include_directories(gfxruntime
    PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(gfxruntime PRIVATE -fPIC)
endif()
