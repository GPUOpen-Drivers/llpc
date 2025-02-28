/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  GfxRuntimeCommon.hlsli
 * @brief Declare common utils that are used in internal runtime library
 ***********************************************************************************************************************
 */
#pragma once

// clang-format off

#ifndef DUMMY_VOID_FUNC
#ifdef AMD_VULKAN
#define DUMMY_VOID_FUNC {}
#else // AMD_VULKAN
#define DUMMY_VOID_FUNC ;
#endif
#endif

#ifndef DUMMY_GENERIC_FUNC
#ifdef AMD_VULKAN
#define DUMMY_GENERIC_FUNC(value) { return value; }
#else // AMD_VULKAN
#define DUMMY_GENERIC_FUNC(value) ;
#endif
#endif

#ifndef GFX_RUNTIME_COMMON_INOUT
#ifdef __cplusplus
#define GFX_RUNTIME_COMMON_INOUT
#else
#define GFX_RUNTIME_COMMON_INOUT inout
#endif
#endif

#ifndef GFX_RUNTIME_COMMON_DECL
#ifdef __cplusplus
#define GFX_RUNTIME_COMMON_DECL extern
#elif AMD_VULKAN
#define GFX_RUNTIME_COMMON_DECL [noinline]
#else
#define GFX_RUNTIME_COMMON_DECL
#endif
#endif
