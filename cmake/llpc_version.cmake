##
 #######################################################################################################################
 #
 #  Copyright (c) 2023-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

set(LLPC_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

# Function to set variable named ${varName} to the first of:
#
# - the value it already has (which might be set by driver or developer);
# - ON if PAL_BUILD_FORCE_ON and the variable name matches PAL_BUILD_* (after prefix replacement to PAL);
# - the value of the variable whose name has PAL_ instead of LLPC_ (which might be set by driver or developer);
# - the given default.
#
# Then, if the value is true, the function adds it to the target_compile_definitions of ${target} at
# scope ${scope}, and appends the setting to LLPC_SET_PROPERTY_SUMMARY_${target} for the caller to report the
# summary.
#
# This function is defined here so that multiple components in the LLPC repo, starting with llvm_version,
# can use it.
#
function(llpc_set_property target scope varName default propertyName)
    if (NOT DEFINED ${varName})
        string(REGEX REPLACE "^[A-Z][A-Z]*_(.*)$" "PAL_\\1" palVarName "${varName}")
        string(REGEX MATCH "^(PAL_BUILD).*" _match ${palVarName})

        if (DEFINED ${palVarName})
            set(${varName} ${${palVarName}})
        elseif ((PAL_BUILD_FORCE_ON) AND (_match))
            # If the user has requested that we force BUILD settings on, then
            # force the setting on (if it matches a PAL_BUILD* variable and isn't explicitly overridden).
            set(${varName} ON)
        else()
            set(${varName} ${default})
        endif()
        # For an LLPC_ variable, cache it as an option so that GPURT can see it.
        if ("${varName}" MATCHES "^LLPC_")
            set(${varName} "${${varName}}" CACHE BOOL "Support ${varName}?" FORCE)
        endif()
        set(${varName} ${${varName}} PARENT_SCOPE)
    endif()
    if (${${varName}})
        target_compile_definitions(${target} ${scope} ${varName}=1)
        set(LLPC_SET_PROPERTY_SUMMARY_${target}
            "${LLPC_SET_PROPERTY_SUMMARY_${target}} ${varName}=${${varName}}" PARENT_SCOPE)
    endif()
endfunction()

if (FALSE
    OR ICD_BUILD_LLPC
)

    # Find LLVM source.
    if (NOT DISABLE_LLPC_VERSION_USES_LLVM)
        include("${LLPC_SOURCE_DIR}/cmake/findllvm.cmake")
    endif()

    # Macro for caller to call the llpc_version CMakeLists.txt and add its target.
    macro(add_llpc_version_projects)
        if (NOT TARGET llpc_version)
            # Force the binary directory to account for the possibility that LLPC is
            # taken from an external source directory.
            add_subdirectory(${LLPC_SOURCE_DIR}/version ${CMAKE_CURRENT_BINARY_DIR}/llpc_version)
        endif()
    endmacro()

else()
    macro(add_llpc_version_projects)
    endmacro()
endif()
