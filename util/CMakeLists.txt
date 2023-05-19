##
 #######################################################################################################################
 #
 #  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

add_library(vkgc_util
    vkgcExtension.cpp
    vkgcCapability.h
)

### util Auto-generated String Files ####################################################################################
find_package(Python3 3.6 COMPONENTS Interpreter)
#if _WIN32
if(NOT Python3_Interpreter_FOUND AND WIN32)
    set(Python3_EXECUTABLE "${GLOBAL_ROOT_DK_DIR}/python/3.6/python.exe")
    if(FILE ${Python3_EXECUTABLE})
        set(Python3_Interpreter_FOUND ON)
        message(STATUS "Using the Python found at ${Python3_EXECUTABLE} instead.")
    endif()
endif()
#endif
if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "Python 3.6 is needed to generate some source files.")
endif()
set(PYTHON_CMD ${Python3_EXECUTABLE})

set(UTIL_STRINGS_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/util)
set(UTIL_STRINGS_INPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/util)
# util settings code generation main script
set(GEN_UTIL_STRING_SCRIPT ${UTIL_STRINGS_INPUT_DIRECTORY}/generate_strings.py)

set(GEN_UTIL_STRINGS_OUTPUT ${UTIL_STRINGS_OUTPUT_DIRECTORY}/g_extensions_decl.h
                           ${UTIL_STRINGS_OUTPUT_DIRECTORY}/g_extensions_impl.h
)

set(GEN_UTIL_STRINGS_INPUT ${UTIL_STRINGS_INPUT_DIRECTORY}/extensions.txt)

add_custom_command(
    OUTPUT ${GEN_UTIL_STRINGS_OUTPUT}
    COMMAND ${PYTHON_CMD} ${GEN_UTIL_STRING_SCRIPT} -w ${UTIL_STRINGS_INPUT_DIRECTORY} -o ${UTIL_STRINGS_OUTPUT_DIRECTORY}
    DEPENDS ${GEN_UTIL_STRING_SCRIPT} ${GEN_UTIL_STRINGS_INPUT}
    COMMENT "Generating vkgc api strings}"
)

target_sources(vkgc_util PRIVATE
     ${GEN_UTIL_STRINGS_OUTPUT})

target_include_directories(vkgc_util PUBLIC ${UTIL_STRINGS_INPUT_DIRECTORY})
target_include_directories(vkgc_util PUBLIC ${UTIL_STRINGS_OUTPUT_DIRECTORY})

### Link Libraries #####################################################################################################
target_link_libraries(vkgc_util PRIVATE vkgc_headers khronos_vulkan_interface)
