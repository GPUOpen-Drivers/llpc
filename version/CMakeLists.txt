##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

include(GNUInstallDirs)

### llpc_version library ###############################################################################################
add_library(llpc_version INTERFACE)

target_include_directories(llpc_version INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

configure_file(include/llpcVersion.h.in include/llpcVersion.h)

if (NOT LLPC_CLIENT_INTERFACE_MAJOR_VERSION)
  message(FATAL_ERROR "Client of LLPC must set LLPC_CLIENT_INTERFACE_MAJOR_VERSION")
endif()

target_compile_definitions(llpc_version INTERFACE
  LLPC_CLIENT_INTERFACE_MAJOR_VERSION=${LLPC_CLIENT_INTERFACE_MAJOR_VERSION}
  # Hardcode the endian-ness define. Our register headers expect it anyway
  LITTLEENDIAN_CPU)

# Mark llpc_version as an installable target for which *.cmake files are created
# that a hypothetical external user could include to import the target, either
# from an installed version or from the build-tree.
#
# This is not expected to actually work (least of all because the generated
# llpcVersion.h is not installed), but this target is linked to by targets that
# we register as LLVM projects (LGC, Continuations, ...) and the LLVM CMake
# infrastructure believes that those should be installable and exported, which
# means CMake also requires llpc_version to be installable and exported.
#
# This would not be required if everybody linked against llpc_version PRIVATEly,
# but then that would create a footgun if any of the version definitions are
# used in header files. Better to have this weird hack and have everybody link
# against llpc_version PUBLICly.
if (NOT DISABLE_LLPC_VERSION_USES_LLVM)
  install(TARGETS llpc_version EXPORT llpc_version-targets)
  install(EXPORT llpc_version-targets DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake) # installed version
  export(TARGETS llpc_version FILE llpc_version.cmake) # build-tree version
endif()

### Chip-enabling options ##############################################################################################

# Set defaults. This serves two purposes:
# 1. On a driver build, this defines what settings we want to scrape from PAL's settings from branchdefs; the
#    default value itself is overridden by PAL's setting.
# 2. On a standalone build, this defines the actual default settings.
set(DEFAULT_LLPC_BUILD
#if LLPC_BUILD_GFX11
    GFX11=ON
#endif
#if LLPC_BUILD_STRIX1
    STRIX1=ON
#endif
#if LLPC_BUILD_GFX115
    GFX115=ON
#endif
)

if (TARGET pal)
    # On a driver build, adjust the default settings by scraping the PAL settings (from branchdefs) out of the
    # pal target's INTERFACE_COMPILE_DEFINITIONS. (The PAL_* variables are not visible here.)
    get_target_property(PALDEFS pal INTERFACE_COMPILE_DEFINITIONS)
    list(REMOVE_DUPLICATES PALDEFS)
    foreach(PALDEF IN LISTS PALDEFS)
        if("${PALDEF}" MATCHES "PAL_BUILD_(.*)=(.*)")
            set(DEF "${CMAKE_MATCH_1}")
            set(VAL "${CMAKE_MATCH_2}")
            # Handle the value being a $<bool:> generator expression.
            # We rely on the input to the $<bool:> being a constant 0,1,OFF,ON,NO,YES
            if ("${VAL}" MATCHES "0|OFF|NO|\\$<BOOL:>")
                set(VAL OFF)
            elseif("${VAL}" MATCHES "1|ON|YES")
                set(VAL ON)
            else()
              message(FATAL_ERROR "Unexpected value in PAL_BUILD_${DEF}=${VAL}")
            endif()
            # Override the entry in DEFAULT_LLPC_BUILD.
            list(TRANSFORM DEFAULT_LLPC_BUILD REPLACE "${DEF}=.*" "${DEF}=${VAL}")
        endif()
    endforeach()
endif()

# For each define in DEFAULT_LLPC_BUILD, provide a cached option for it (which makes it globally visible, so
# GPURT can see it), and add it to llpc_version's target_compiler_definitions.
set(LLPC_BUILD_SUMMARY "")
foreach(DEFVAL IN LISTS DEFAULT_LLPC_BUILD)
  if ("${DEFVAL}" MATCHES "(.*)=(.*)")
        set(DEF "${CMAKE_MATCH_1}")
        set(VAL "${CMAKE_MATCH_2}")
        option("LLPC_BUILD_${DEF}" "LLPC support for this chip?" "${VAL}")
        if ("${LLPC_BUILD_${DEF}}")
            target_compile_definitions(llpc_version INTERFACE "LLPC_BUILD_${DEF}=1")
            set(LLPC_BUILD_SUMMARY "${LLPC_BUILD_SUMMARY} LLPC_BUILD_${DEF}")
        endif()
    endif()
endforeach()

# Report the summary of what is enabled.
message(STATUS "LLPC_BUILD_* summary: ${LLPC_BUILD_SUMMARY}")

