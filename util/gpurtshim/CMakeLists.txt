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

if(VKI_RAY_TRACING AND NOT LLPC_IS_STANDALONE)
    # The shim is built as a separate miniature library so that the GPURT include path doesn't "leak" into the main
    # LLPC compile.
    add_library(vkgc_gpurtshim STATIC GpurtShim.cpp)

    include(../../cmake/CompilerFlags.cmake)
    set_compiler_options(vkgc_gpurtshim ${LLPC_ENABLE_WERROR})

    # Link against vkgc_headers to pull in the necessary include directories and all the VKI_* defines
    target_link_libraries(vkgc_gpurtshim PUBLIC vkgc_headers)
    target_link_libraries(vkgc_gpurtshim PRIVATE gpurt)
endif()
