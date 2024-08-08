/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  GpurtIntrinsics.h
 * @brief Declare intrinsics that are called from gpurt shader code and implemented in the compiler.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc/GpurtEnums.h"

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

#ifdef __cplusplus
#define GPURT_INOUT
#define GPURT_DECL extern
#else // __cplusplus
#define GPURT_INOUT inout
#ifdef AMD_VULKAN
#define GPURT_DECL [noinline]
#else // AMD_VULKAN
#define GPURT_DECL
#endif
#endif

#define CONTINUATIONS_LGC_STACK_LOWERING 1

//=====================================================================================================================
// Continuation intrinsics
//
//=====================================================================================================================
// Control flow intrinsics: Enqueue, WaitEnqueue and Await
// -------------------------------------------------------
// In general, these intrinsics provide the continuation equivalent of indirect tail calls, by jumping to a passed
// address and passing arbitrary arguments to the function at that address.
//
// Special arguments, and variants for different arguments and return types
// ------------------------------------------------------------------------
// Each such intrinsic has an addr argument of the referenced function.
// WaitEnqueue has an additional waitMask argument.
// All other arguments are generic function arguments passed to the referenced function.
// For Await, the return type of the intrinsic is the return type of the referenced function.
// Thus, arguments and return type of the HLSL intrinsic depend on the referenced function, which is why we need
// multiple variants of each intrinsic.
// There is no special handling for those variants, the compiler just knows the baseline intrinsics
// _AmdEnqueue, _AmdWaitEnqueue and _AmdAwait and allows arbitrary suffixes.
//
// Function arguments and transformations
// --------------------------------------
// Referenced functions are in fact pointers to compiled HLSL-defined shaders (e.g. CHS) obtained e.g. from shader ids,
// or resume functions created by Await calls.
// These functions are heavily transformed in DXIL by the continuations compiler, also changing their arguments.
// Continuation intrinsics refer to functions after these transformations, and pass arguments accordingly.
// For example, a CHS shader in HLSL receives a payload and hit attributes. However, after continuation transforms,
// the DXIL representation of a CHS receives a CSP (continuation stack pointer), a return address
// (typically RGS.resume), and system data. The payload is implicitly passed via a global variable.
// Thus, usage of these intrinsics is tightly coupled to function argument conventions of the continuations compiler.
//
// Return address handling
// -----------------------
// Some functions determine the next function to continue to on their own (Traversal, RayGen), all others are passed
// a return address as follows. In these cases, the return address is always explicitly passed to these intrinsics,
// even if the return address is a resume function or the current function, which are obtained via intrinsics.
// Explicitly passing the return address allows to set metadata (e.g. scheduling priority) from HLSL.
//
// Enqueue
// -------
// Enqueue just jumps to the function at the given address. Enqueue is noreturn, and following code is unreachable.
// _AmdEnqueue*(uint64_t addr, ...)
#define DECLARE_ENQUEUE(Suffix, ...) GPURT_DECL \
  void _AmdEnqueue##Suffix(uint64_t addr, __VA_ARGS__) DUMMY_VOID_FUNC
//
// WaitEnqueue
// -----------
// WaitEnqueue waits until all lanes in the mask also have enqueued the same wait mask before performing the Enqueue.
// Generic function arguments start with the third argument.
// _AmdWaitEnqueue*(uint64_t addr, uint64_t waitMask, uint32_t csp, ...)
#define DECLARE_WAIT_ENQUEUE(Suffix, ...) GPURT_DECL \
  void _AmdWaitEnqueue##Suffix(uint64_t addr, uint64_t waitMask, __VA_ARGS__) DUMMY_VOID_FUNC
//
// Complete
// --------
// Complete ends the program.
GPURT_DECL void _AmdComplete() DUMMY_VOID_FUNC
//
// Await
// -----
// Await adds a resume point in the containing function (after inlining), creating a *resume function*,
// and jumps to the referenced function.
// The CSP is prepended to the generic arguments as new first argument for the referenced function.
// The return address is passed explicitly to the intrinsic if needed by the referenced function, and is expected to
// be the address of the resume function obtained via GetResumePointAddr.
// It is passed explicitly because some uses do not need it (Traversal), and allowing to control metadata from HLSL,
// such as the scheduling priority.
// Thus, in contrast to Enqueue which renders all following code in the containing function unreachable, Await is more
// similar to an ordinary indirect function call.
// Any state in the containing function that is still needed in the resume function is stored in the continuation state
// managed by the compiler.
// Just like with enqueue, there is a waiting variant _AmdWaitAwait that waits on running the passed function.
// ReturnTy _AmdAwait*(uint64_t addr, ...)

#define DECLARE_AWAIT(Suffix, ReturnTy, ...) GPURT_DECL \
  ReturnTy _AmdAwait##Suffix(uint64_t addr, __VA_ARGS__) DUMMY_GENERIC_FUNC((ReturnTy)0)

// ReturnTy _AmdWaitAwait*(uint64_t addr, uint64_t waitMask, ...)
#define DECLARE_WAIT_AWAIT(Suffix, ReturnTy, ...) GPURT_DECL \
  ReturnTy _AmdWaitAwait##Suffix(uint64_t addr, uint64_t waitMask, __VA_ARGS__) DUMMY_GENERIC_FUNC((ReturnTy)0)
//
// GetResumePointAddr
// ------------------
// Returns the address of the resume function of the next resume point, i.e. at the next Await intrinsic.
// Forbidden if the call site does not dominate a unique suspend point.
// If this intrinsic is used, the implicit return address argument is removed from the next Await call.
GPURT_DECL uint64_t _AmdGetResumePointAddr() DUMMY_GENERIC_FUNC(0)
//
// GetCurrentFuncAddr
// ------------------
// Returns the address of the caller function making this intrinsic call, after inlining and continuation function splitting.
GPURT_DECL uint64_t _AmdGetCurrentFuncAddr() DUMMY_GENERIC_FUNC(0)
//
//=====================================================================================================================
// GetShaderKind
// Returns the kind of the shader this intrinsic is used in.
// This is lowered after inlining GPURT functions (e.g. TraceRay) into app shaders.
GPURT_DECL DXILShaderKind _AmdGetShaderKind() DUMMY_GENERIC_FUNC(DXILShaderKind::Invalid)
//
//=====================================================================================================================
// ContStackAlloc
// Allocate space on the continuation stack.
// Argument is the size of the allocation.
// Returns the address of the allocation.
//
// This is equivalent to
//   return_value = csp
//   csp += byteSize
//
// In addition, it tells the compiler and driver about this allocation, so they can reserve enough memory for the
// stack.
GPURT_DECL uint32_t _AmdContStackAlloc(uint32_t byteSize) DUMMY_GENERIC_FUNC(0)

//=====================================================================================================================
// Free the current continuation stack
GPURT_DECL void _AmdContStackFree(uint32_t stackSize) DUMMY_VOID_FUNC

//=====================================================================================================================
// Set the current continuation stack pointer
GPURT_DECL void _AmdContStackSetPtr(uint32_t csp) DUMMY_VOID_FUNC

//=====================================================================================================================
// Get the current continuation stack pointer
GPURT_DECL uint32_t _AmdContStackGetPtr() DUMMY_GENERIC_FUNC(0)

//=====================================================================================================================
// Load data from a given continuation stack address
#define DECLARE_CONT_STACK_LOAD(Suffix, ReturnTy) GPURT_DECL \
  ReturnTy _AmdContStackLoad##Suffix(uint32_t addr) DUMMY_GENERIC_FUNC((ReturnTy)0)

//=====================================================================================================================
// Load data from a given continuation stack address, mark the load as last use
#define DECLARE_CONT_STACK_LOAD_LAST_USE(Suffix, ReturnTy) GPURT_DECL \
  ReturnTy _AmdContStackLoadLastUse##Suffix(uint32_t addr) DUMMY_GENERIC_FUNC((ReturnTy)0)

//=====================================================================================================================
// Store data to a given continuation stack address
#define DECLARE_CONT_STACK_STORE(Suffix, ...) GPURT_DECL \
  void _AmdContStackStore##Suffix(uint32_t addr, __VA_ARGS__) DUMMY_VOID_FUNC

//
//=====================================================================================================================
// State (system data / hit attributes) modifier intrinsics
// void _AmdRestoreSystemData*(in SystemData data)
#define DECLARE_RESTORE_SYSTEM_DATA(Suffix, ...) GPURT_DECL \
  void _AmdRestoreSystemData##Suffix(__VA_ARGS__) DUMMY_VOID_FUNC
// void _AmdAcceptHitAttributes*(inout SystemData data)
#define DECLARE_ACCEPT_HIT_ATTRIBUTES(Suffix, ...) GPURT_DECL \
  void _AmdAcceptHitAttributes##Suffix(__VA_ARGS__) DUMMY_VOID_FUNC
//
//=====================================================================================================================
// Intrinsics to access arbitrary structs as i32 arrays
// uint32_t _AmdValueI32Count*(Struct data)
#define DECLARE_VALUE_I32_COUNT(Suffix, ...) GPURT_DECL \
  uint32_t _AmdValueI32Count##Suffix(__VA_ARGS__) DUMMY_GENERIC_FUNC(0)
// uint32_t _AmdValueGetI32*(Struct data, uint32_t i)
#define DECLARE_VALUE_GET_I32(Suffix, ...) GPURT_DECL \
  uint32_t _AmdValueGetI32##Suffix(__VA_ARGS__, uint32_t i) DUMMY_GENERIC_FUNC(0)
// void _AmdValueSetI32*(inout Struct data, uint32_t i, uint32_t value)
#define DECLARE_VALUE_SET_I32(Suffix, ...) GPURT_DECL \
  void _AmdValueSetI32##Suffix(__VA_ARGS__, uint32_t value, uint32_t i) DUMMY_VOID_FUNC
//
//=====================================================================================================================
// Intrinsics to access payload as i32 arrays
GPURT_DECL uint32_t _AmdContPayloadRegistersI32Count() DUMMY_GENERIC_FUNC(0)
GPURT_DECL uint32_t _AmdContPayloadRegistersGetI32(uint32_t i) DUMMY_GENERIC_FUNC(0)
GPURT_DECL void     _AmdContPayloadRegistersSetI32(uint32_t i, uint32_t value) DUMMY_VOID_FUNC
//
//=====================================================================================================================
// Intrinsics returning uninitialized values (poison in LLVM IR),
// used to hint the compiler to not keep certain values alive.
// ReturnTy _AmdGetUninitialized*()
#define DECLARE_GET_UNINITIALIZED(Suffix, ReturnTy) GPURT_DECL \
  ReturnTy _AmdGetUninitialized##Suffix() DUMMY_GENERIC_FUNC((ReturnTy)0)

//=====================================================================================================================
// Intrinsics to access properties of the current configuration
GPURT_DECL bool _AmdContinuationStackIsGlobal() DUMMY_GENERIC_FUNC(0)
//=====================================================================================================================
// Intrinsic to get the current rtip version.
// The version is encoded as <major><minor> in decimal digits, so 11 is rtip 1.1, 20 is rtip 2.0
GPURT_DECL RayTracingIpLevel _AmdGetRtip() DUMMY_GENERIC_FUNC(RayTracingIpLevel::_None)
