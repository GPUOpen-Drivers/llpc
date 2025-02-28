##
 #######################################################################################################################
 #
 #  Copyright (c) 2020-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

include(FindPackageHandleStandardArgs)

set(_SDB_SEARCHES
    $ENV{SHADERDBG_DEPTH}
    ${GLOBAL_ROOT_SRC_DIR}drivers/ShaderDbg
    ${LLPC_SOURCE_DIR}/imported/ShaderDbg
    ${LLPC_SOURCE_DIR}/../ShaderDbg
   )

find_path(SHADERDBG_PATH inc/shaderDbg.h
    PATHS ${_SDB_SEARCHES} ENV SHADERDBG_PATH
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

find_package_handle_standard_args(ShaderDbg DEFAULT_MSG SHADERDBG_PATH)

if(SHADERDBG_FOUND)
    set(SHADERDBG_INCLUDE_DIRS ${SHADERDBG_PATH}/inc)
    set(SDL_BUILD_TOOLS FALSE)

    # Only add the ShaderDbg subdirectory if we haven't already seen it.
    if (NOT TARGET ShaderDbg)
        add_subdirectory(${SHADERDBG_PATH} ${CMAKE_CURRENT_BINARY_DIR}/ShaderDbg)
    endif()
endif()
