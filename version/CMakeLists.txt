##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

# llpc_set_property sets the given LLPC_* variable to the first of:
#
# - the value it already has (which might be set by driver or developer);
# - the value of the variable whose name has PAL_ instead of LLPC_ (which might be set by driver or developer);
# - the given default.
#
# Then, if the value is true, the function adds it to llpc_version's target_compile_definitions,
# and appends the setting to LLPC_SET_PROPERTY_SUMMARY_llpc_version for us to report the summary.

#if LLPC_BUILD_HAWK_POINT1
llpc_set_property(llpc_version INTERFACE LLPC_BUILD_HAWK_POINT1 ON "HW_HAWK_POINT1")
#endif
#if LLPC_BUILD_HAWK_POINT2
llpc_set_property(llpc_version INTERFACE LLPC_BUILD_HAWK_POINT2 ON "HW_HAWK_POINT2")
#endif

#if LLPC_BUILD_STRIX1
llpc_set_property(llpc_version INTERFACE LLPC_BUILD_STRIX1 ON "HW_STRIX1")
#endif
#if LLPC_BUILD_STRIX_HALO
llpc_set_property(llpc_version INTERFACE LLPC_BUILD_STRIX_HALO ON "HW_STRIX_HALO")
#endif

#if LLPC_BUILD_NAVI48
llpc_set_property(llpc_version INTERFACE LLPC_BUILD_NAVI48 ON "HW_NAVI48")
#endif

# Report the summary of what is enabled.
message(STATUS "llpc_version:${LLPC_SET_PROPERTY_SUMMARY_llpc_version}")

