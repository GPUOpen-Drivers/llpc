##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

### llpc_version library ###############################################################################################
add_library(llpc_version INTERFACE)

target_include_directories(llpc_version INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)

configure_file(include/llpcVersion.h.in include/llpcVersion.h)

if (NOT LLPC_CLIENT_INTERFACE_MAJOR_VERSION)
  message(FATAL_ERROR "Client of LLPC must set LLPC_CLIENT_INTERFACE_MAJOR_VERSION")
endif()

target_compile_definitions(llpc_version INTERFACE
  LLPC_CLIENT_INTERFACE_MAJOR_VERSION=${LLPC_CLIENT_INTERFACE_MAJOR_VERSION}
  # Hardcode the endian-ness define. Our register headers expect it anyway
  LITTLEENDIAN_CPU
  CHIP_HDR_NAVI14
  CHIP_HDR_NAVI21
  CHIP_HDR_NAVI22
  CHIP_HDR_NAVI23
  CHIP_HDR_NAVI24
#if LLPC_BUILD_NAVI31
  CHIP_HDR_NAVI31
#endif
#if LLPC_BUILD_NAVI33
  CHIP_HDR_NAVI33
#endif
  CHIP_HDR_RENOIR)

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
install(TARGETS llpc_version EXPORT llpc_version-targets)
install(EXPORT llpc_version-targets DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake) # installed version
export(TARGETS llpc_version FILE llpc_version.cmake) # build-tree version

### Cached Config-related Options ######################################################################################
option(LLPC_BUILD_NAVI12 "LLPC support for NAVI12?" ON)
if (LLPC_BUILD_NAVI12)
  target_compile_definitions(llpc_version INTERFACE LLPC_BUILD_NAVI12 CHIP_HDR_NAVI12)
endif()
#if LLPC_BUILD_NAVI32
option(LLPC_BUILD_NAVI32 "LLPC support for NAVI32?" ON)
if (LLPC_BUILD_NAVI32)
  target_compile_definitions(llpc_version INTERFACE LLPC_BUILD_NAVI32 CHIP_HDR_NAVI32)
endif()
#endif
option(LLPC_BUILD_REMBRANDT "LLPC support for REMBRANDT?" ON)
if (LLPC_BUILD_REMBRANDT)
  target_compile_definitions(llpc_version INTERFACE LLPC_BUILD_REMBRANDT CHIP_HDR_REMBRANDT)
endif()
option(LLPC_BUILD_RAPHAEL "LLPC support for RAPHAEL?" ON)
if (LLPC_BUILD_RAPHAEL)
  target_compile_definitions(llpc_version INTERFACE LLPC_BUILD_RAPHAEL CHIP_HDR_RAPHAEL)
endif()
option(LLPC_BUILD_MENDOCINO "LLPC support for MENDOCINO?" ON)
if (LLPC_BUILD_MENDOCINO)
  target_compile_definitions(llpc_version INTERFACE LLPC_BUILD_MENDOCINO CHIP_HDR_MENDOCINO)
endif()
#if LLPC_BUILD_PHOENIX1
option(LLPC_BUILD_PHOENIX1 "LLPC support for PHOENIX1?" ON)
if (LLPC_BUILD_PHOENIX1)
  target_compile_definitions(llpc_version INTERFACE LLPC_BUILD_PHOENIX1 CHIP_HDR_PHOENIX1)
endif()
#endif