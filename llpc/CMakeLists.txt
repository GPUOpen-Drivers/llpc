##
 #######################################################################################################################
 #
 #  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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

project(LLPC C CXX)

### Create LLPC Library ################################################################################################
add_library(llpc STATIC "")

if(ICD_BUILD_LLPC)
  add_dependencies(llpc LLVMlgc)
endif()

### Cached Project Options #############################################################################################
option(LLPC_BUILD_LIT      "LLPC build lit test"        OFF)

if(ICD_BUILD_LLPC)
    set(AMDLLPC_DIR ${CMAKE_CURRENT_BINARY_DIR} )
endif()

### Compiler Options ###################################################################################################
if(UNIX)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        # SEE: https://gcc.gnu.org/onlinedocs/gcc-6.2.0/gcc/Option-Summary.html#Option-Summary
        # for a list of all options and documentation.
        #target_compile_options(llpc PRIVATE option)
        target_compile_options(llpc PRIVATE
            -Wno-unused-parameter
            -Wno-type-limits
            -Wno-switch
            -Wno-parentheses
            -Wno-maybe-uninitialized
            -Wno-delete-non-virtual-dtor
            -Wno-sign-compare
            -Wno-delete-incomplete
            -Wunused-variable
            -Werror=unused-variable
            -Wunused-function
            -Werror=unused-function
        )

        target_compile_options(llpc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -Wno-unused
            -Wno-ignored-qualifiers
            -Wno-missing-field-initializers
            -Wno-invalid-offsetof           # offsetof within non-standard-layout type 'x' is undefined
        >)

        target_compile_options(llpc PRIVATE -fno-strict-aliasing)

        target_compile_options(llpc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -std=c++14
            -fno-rtti
            -fPIC
        >)

        message(STATUS "Configured ${PROJECT_NAME} compiler options for GCC.")
    elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        target_compile_options(llpc PRIVATE
            -fvisibility-inlines-hidden
            -fcolor-diagnostics
            -Wall
            -Werror
            -Wno-invalid-offsetof
            -Wno-missing-braces
        )
        target_compile_options(llpc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -std=c++14
            -fno-rtti
            -fPIC
        >)
        #message(STATUS "Configured compiler options for Clang.")
        message(WARNING "Clang is untested.")
    else()
        message(FATAL_ERROR "Using unknown compiler.")
    endif()
elseif(WIN32)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
        # CMAKE-TODO: These are /W4 (level 4) warnings
        target_compile_options(llpc
            PRIVATE # Warnings in interface and src
                /wd4005 # 'DEBUG' : macro redefinition ??? Defined in toolchain ??? importedllvmincludellvm/Support/Debug.h
                /wd4018 # '<' : signed/unsigned mismatch
                /wd4100 # unreferenced formal parameter
                /wd4127 # conditional expression is constant
                /wd4141 # 'inline' : used more than once
                /wd4146 # unary minus operator applied to unsigned type, result still unsigned
                /wd4189 # local variable is initialized but not referenced
                /wd4201 # nonstandard extension used : nameless struct/union
                /wd4244 # 'X' : conversion from 'Y' to 'Z', possible loss of data
                /wd4245 # 'X' : conversion from 'Y' to 'Z', signed/unsigned mismatch
                /wd4250 # 'X': inherits 'Y' via dominance
                /wd4291 # no matching operator delete found; memory will not be freed if initialization throws an exception
                /wd4267 # 'return' : conversion from 'size_t' to 'unsigned int', possible loss of data
                /wd4389 # '==' : signed/unsigned mismatch
                /wd4505 # unreferenced local function has been removed
                /wd4510 # default constructor could not be generated
                /wd4512 # assignment operator could not be generated
                /wd4610 # struct 'X' can never be instantiated - user defined constructor required
                /wd4624 # destructor could not be generated because a base class destructor is inaccessible or deleted
                /wd4702 # unreachable code
                /wd4706 # assignment within conditional expression
                /wd4800 # forcing value to bool 'true' or 'false' (performance warning)
                /wd6246 # Local declaration of 'S' hides declaration of the same name in outer scope
                /wd6323 # Use of arithmetic operator on Boolean type(s)
        )

        target_compile_definitions(llpc PRIVATE _SCL_SECURE_NO_WARNINGS)
        target_compile_definitions(llpc PRIVATE _CRT_SECURE_NO_WARNINGS)

        message(STATUS "Configured ${PROJECT_NAME} compiler options for MSVC.")
    else()
        message(FATAL_ERROR "Using unknown compiler")
    endif()
endif()

### Defines/Includes/Sources ###########################################################################################
if(ICD_BUILD_LLPC)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${XGL_LLVM_BUILD_PATH}/lib/cmake/llvm)
    include(LLVMConfig)
    message(STATUS "LLVM execuables: " ${LLVM_TOOLS_BINARY_DIR})
    message(STATUS "LLVM libraries: " ${LLVM_BUILD_LIBRARY_DIR})
    execute_process(
        COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-config --libs amdgpu analysis bitreader bitwriter codegen irreader linker mc passes support target transformutils
        OUTPUT_VARIABLE LLVM_LINK_FLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    message(STATUS "LLVM link options:" ${LLVM_LINK_FLAGS})
endif()
target_compile_definitions(llpc PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)
target_compile_definitions(llpc PRIVATE _SPIRV_LLVM_API)
target_compile_definitions(llpc PRIVATE PAL_CLIENT_INTERFACE_MAJOR_VERSION=${PAL_CLIENT_INTERFACE_MAJOR_VERSION})
if (LLPC_CLIENT_INTERFACE_MAJOR_VERSION)
    target_compile_definitions(llpc PRIVATE LLPC_CLIENT_INTERFACE_MAJOR_VERSION=${LLPC_CLIENT_INTERFACE_MAJOR_VERSION})
endif()
if(ICD_BUILD_LLPC)
    target_compile_definitions(llpc PRIVATE ICD_BUILD_LLPC)
endif()

option(LLPC_USE_EXPERIMENTAL_SHADER_CACHE_PIPELINES "Use experimental shader cache for pipeline caching instead of PAL pipeline caching?" OFF)
if(LLPC_USE_EXPERIMENTAL_SHADER_CACHE_PIPELINES)
    target_compile_definitions(llpc PRIVATE LLPC_USE_EXPERIMENTAL_SHADER_CACHE_PIPELINES=1)
endif()

if(ICD_BUILD_LLPC)
    if(XGL_LLVM_UPSTREAM)
        target_compile_definitions(llpc PRIVATE XGL_LLVM_UPSTREAM)
    endif()
endif()

if(WIN32)
    target_compile_definitions(llpc PRIVATE
        NOMINMAX    # windows.h defines min/max which conflicts with the use of std::min / max
        UNICODE     # CMAKE-TODO: What is this used for?
        _UNICODE
    )
endif()

target_include_directories(llpc
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/../lgc/interface
    PRIVATE
        ${PROJECT_SOURCE_DIR}/context
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/lower
        ${PROJECT_SOURCE_DIR}/translator/include
        ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV
        ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV/libSPIRV
        ${PROJECT_SOURCE_DIR}/util
        ${PROJECT_SOURCE_DIR}/../util
        ${PROJECT_SOURCE_DIR}/../tool/dumper
        ${XGL_PAL_PATH}/inc/core
        ${XGL_PAL_PATH}/inc/util
        ${LLVM_INCLUDE_DIRS}
)

# CMAKE-TODO: Figure out a better way to do this.
target_include_directories(llpc PRIVATE ${XGL_ICD_PATH}/api/include/khronos)

if(WIN32)
    target_compile_definitions(llpc PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

if(ICD_BUILD_LLPC)
# llpc/context
    target_sources(llpc PRIVATE
        context/llpcCompiler.cpp
        context/llpcContext.cpp
        context/llpcComputeContext.cpp
        context/llpcGraphicsContext.cpp
        context/llpcShaderCache.cpp
        context/llpcPipelineContext.cpp
        context/llpcShaderCacheManager.cpp
    )

# llpc/lower
    target_sources(llpc PRIVATE
        lower/llpcSpirvLower.cpp
        lower/llpcSpirvLowerAccessChain.cpp
        lower/llpcSpirvLowerAlgebraTransform.cpp
        lower/llpcSpirvLowerConstImmediateStore.cpp
        lower/llpcSpirvLowerGlobal.cpp
        lower/llpcSpirvLowerInstMetaRemove.cpp
        lower/llpcSpirvLowerLoopUnrollControl.cpp
        lower/llpcSpirvLowerMemoryOp.cpp
        lower/llpcSpirvLowerResourceCollect.cpp
        lower/llpcSpirvLowerTranslator.cpp
        lower/llpcSpirvLowerUtil.cpp
    )

# llpc/translator
    target_sources(llpc PRIVATE
        translator/lib/SPIRV/SPIRVReader.cpp
        translator/lib/SPIRV/SPIRVUtil.cpp
    )

    target_sources(llpc PRIVATE
        translator/lib/SPIRV/libSPIRV/SPIRVBasicBlock.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVDebug.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVDecorate.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVEntry.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVFunction.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVInstruction.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVModule.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVStream.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVType.cpp
        translator/lib/SPIRV/libSPIRV/SPIRVValue.cpp
    )

# llpc/util
    target_sources(llpc PRIVATE
        util/llpcDebug.cpp
        util/llpcElfWriter.cpp
        util/llpcEmuLib.cpp
        util/llpcFile.cpp
        util/llpcShaderModuleHelper.cpp
        util/llpcTimerProfiler.cpp
        util/llpcUtil.cpp
    )
else()
    target_sources(llpc PRIVATE
        util/llpcUtil.cpp
    )
endif()

add_subdirectory(../tool/dumper ${PROJECT_BINARY_DIR}/../dumper)
target_link_libraries(llpc PRIVATE dumper)

target_link_libraries(llpc PRIVATE cwpack)

### LLPC Auto-generated Files ##########################################################################################
if(ICD_BUILD_LLPC)
set(OP_EMU_LIB_GEN_DIR ${PROJECT_SOURCE_DIR}/patch/generate)
set(GEN_GLSL_OP_EMU_LIB_SCRIPT ${OP_EMU_LIB_GEN_DIR}/script/genGlslEmuLib.py)

if(UNIX)
    set(BUILD_OS lnx)
elseif(WIN32)
    set(BUILD_OS win)
endif()

if(UNIX)
    set(LLVM_BIN_DIR ${XGL_LLVM_BUILD_PATH}/bin/)
elseif(WIN32)
    set(LLVM_BIN_DIR ${XGL_LLVM_BUILD_PATH}/\$\(Configuration\)/bin/)
endif()

endif()
### Link Libraries #####################################################################################################
# CMAKE-TODO: LLVM_LIB_DIR should probably be set in the op level CMake?
# Maybe add XGL_LLVM_PATH?
# How are these built? Can they be built through CMake?

### Visual Studio Filters ##############################################################################################
target_vs_filters(llpc)

### VFX library for Standalone Compiler ###################################################################################
if(ICD_BUILD_LLPC)
    add_subdirectory(${PROJECT_SOURCE_DIR}/../tool/vfx ${PROJECT_BINARY_DIR}/../vfx)
endif()

### Create Standalone Compiler ############################################################################################
if(ICD_BUILD_LLPC)
add_executable(amdllpc
    tool/amdllpc.cpp
    tool/llpcAutoLayout.cpp
)
add_dependencies(amdllpc llpc)

target_compile_definitions(amdllpc PRIVATE ${TARGET_ARCHITECTURE_ENDIANESS}ENDIAN_CPU)
target_compile_definitions(amdllpc PRIVATE _SPIRV_LLVM_API)
if (LLPC_CLIENT_INTERFACE_MAJOR_VERSION)
    target_compile_definitions(amdllpc PRIVATE LLPC_CLIENT_INTERFACE_MAJOR_VERSION=${LLPC_CLIENT_INTERFACE_MAJOR_VERSION})
    target_compile_definitions(amdllpc PRIVATE PAL_CLIENT_INTERFACE_MAJOR_VERSION=${PAL_CLIENT_INTERFACE_MAJOR_VERSION})
endif()

target_compile_definitions(amdllpc PRIVATE ICD_BUILD_LLPC)

if(LLPC_USE_EXPERIMENTAL_SHADER_CACHE_PIPELINES)
    target_compile_definitions(amdllpc PRIVATE LLPC_USE_EXPERIMENTAL_SHADER_CACHE_PIPELINES=1)
endif()

target_include_directories(amdllpc
PUBLIC
    ${PROJECT_SOURCE_DIR}/include
PRIVATE
    ${PROJECT_SOURCE_DIR}/builder
    ${PROJECT_SOURCE_DIR}/context
    ${PROJECT_SOURCE_DIR}/../imported/spirv
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/../include
    ${PROJECT_SOURCE_DIR}/lower
    ${PROJECT_SOURCE_DIR}/patch
    ${PROJECT_SOURCE_DIR}/patch/gfx6/chip
    ${PROJECT_SOURCE_DIR}/patch/gfx9/chip
    ${PROJECT_SOURCE_DIR}/patch/generate
    ${PROJECT_SOURCE_DIR}/translator/include
    ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV
    ${PROJECT_SOURCE_DIR}/translator/lib/SPIRV/libSPIRV
    ${PROJECT_SOURCE_DIR}/util
    ${PROJECT_SOURCE_DIR}/../util
    ${PROJECT_SOURCE_DIR}/../tool/dumper
    ${XGL_PAL_PATH}/src/core/hw/gfxip/gfx6/chip
    ${XGL_PAL_PATH}/src/core/hw/gfxip/gfx9/chip
    ${XGL_PAL_PATH}/inc/core
    ${XGL_PAL_PATH}/inc/util
    ${LLVM_INCLUDE_DIRS}
)

set(VULKAN_HEADER_PATH ${XGL_ICD_PATH}/api/include/khronos)
target_include_directories(amdllpc PRIVATE ${VULKAN_HEADER_PATH})

if(UNIX)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        target_compile_options(amdllpc PRIVATE -fno-strict-aliasing)
        target_compile_options(amdllpc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++14 -fno-rtti>)

        target_compile_options(amdllpc PRIVATE -Wno-unused-parameter -Wno-shift-negative-value -Wno-type-limits -Wno-error=switch -Wno-error=sign-compare -Wno-error=parentheses -Wno-error=maybe-uninitialized -Wno-error=delete-non-virtual-dtor -Wno-sign-compare -Wno-error -Wunused-variable -Werror=unused-variable -Wunused-function -Werror=unused-function)
        target_compile_options(amdllpc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused -Wno-unused-parameter -Wno-ignored-qualifiers -Wno-missing-field-initializers>)

        message(STATUS "Configured ${PROJECT_NAME} compiler options for GCC.")
    elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        target_compile_options(amdllpc PRIVATE
            -fvisibility-inlines-hidden
            -fcolor-diagnostics
            -Wall
            -Werror
            -Wno-missing-braces
        )
        target_compile_options(amdllpc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -std=c++14
            -fno-rtti
        >)
        #message(STATUS "Configured compiler options for Clang.")
        message(WARNING "Clang is untested.")
    else()
        message(FATAL_ERROR "Using unknown compiler.")
    endif()
endif()

if(UNIX)
    target_link_libraries(amdllpc PRIVATE llpc vfx dl stdc++)
elseif(WIN32)
    target_link_libraries(amdllpc PRIVATE llpc vfx)
endif()
llvm_map_components_to_libnames(llvm_libs lgc amdgpucodegen amdgpuinfo amdgpuasmparser amdgpudisassembler LTO ipo analysis bitreader bitwriter codegen irreader linker mc passes support target transformutils coroutines aggressiveinstcombine)
target_link_libraries(amdllpc PRIVATE ${llvm_libs})
target_link_libraries(amdllpc PRIVATE cwpack)
endif()
### Add Subdirectories #################################################################################################
if(ICD_BUILD_LLPC)
# SPVGEN
set(XGL_SPVGEN_PATH ${PROJECT_SOURCE_DIR}/../../spvgen CACHE PATH "Specify the path to SPVGEN.")

if(EXISTS ${XGL_SPVGEN_PATH})
    add_subdirectory(${XGL_SPVGEN_PATH} ${CMAKE_BINARY_DIR}/spvgen EXCLUDE_FROM_ALL)
endif()

# Lit tests
if(LLPC_BUILD_LIT)
    add_subdirectory(test)
endif()
endif()
