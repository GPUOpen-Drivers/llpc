/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  GpurtVersion.h
 * @brief Declare helpers used to pass GpuRt version info into LLPC.
 ***********************************************************************************************************************
 */

#ifdef LLPC_VERSION_GPURT_VERSION_H

#ifndef __cplusplus
// Ensure this is included just once from HLSL, as we depend on HLSL defines for versioning flags
// that may be lost if an earlier include is made without defines.
#error "GpurtVersion.h may only be included once from HLSL"
#endif

#else

#define LLPC_VERSION_GPURT_VERSION_H

#ifdef __cplusplus
#include <cstdint>
#endif

// Helpers to pass versioning info from GpuRt to LLPC to stage changes during promotions.
//
// Sometimes there are changes that need to be done simultaneously in GpuRt and LLPC.
//
// It is not possible to stage such changes *reliably* using numeric versions. One could attempt
// to implement changes in A, guarded by a future version of B that is larger than the current one,
// and then do the change in B and bump the version. However, this has the problem that a different
// change in B in the meantime may bump the version, unintentionally enabling the change in A.
//
// A common pattern to stage such changes is add support for both in component A that is disabled by default,
// promote that, and then do the change in component B, simultaneously also somehow enabling the change in component A.
//
// Because this header is included into GpuRt, it allows to apply the staging scheme above with A=GpuRt, guarded
// by ifdefs on a define controlled from this header here, and then do the change in B=LLPC, setting the define.
//
// The following mechanism allows to do it in reverse, which sometimes is easier if the change on the compiler
// side is small. The idea is to pass a numerical constant from GpuRt to the compiler. The bits of this constant
// indicate whether specific changes are active, although in practice multiple active bits might be rare.
// The constant is encoded as length of an array of a helper type, which is the return type of _cont_GpurtVersionFlags.
// This way, we don't depend on compiler optimizations for the constant to be indeed a constant in IR.
// (As opposed to returning the constant from a function, and inspecting the function body in the compiler.)
//
// On the GpuRt side, we just need to set a define before including this header. This will then
// set the corresponding flag in the constant which is then included into the compiled module.
// If LLPC has already been updated to no longer depend on the flag, the define is ignored and
// can be removed on the GpuRt side.
//
// The process to stage changes using the mechanism below is:
//   * Implement the change in LLPC, guarded by a newly added flag GpuRtVersionFlag::SomeChange.
//     Include it into GpuRtVersionFlagsContainer, guarded by a new define SOME_CHANGE that is not set.
//   * Implement the change in GpuRt, setting the define SOME_CHANGE before including the LLPC header.
//     This changes GpuRtVersionFlagsContainer and LLPC will see the SomeChange flag as enabled.
//   * Remove the flag SomeChange in LLPC, and change the guarded code assuming it to be enabled.
//   * Remove the define SOME_CHANGE in GpuRt.
//
// Every value of this enum corresponds to a change controlled from GpuRt. Ensure the values use disjoint bits.
enum class GpuRtVersionFlag : uint32_t {};

#ifndef __cplusplus
// HLSL-only code to export a function _cont_GpurtVersionFlags, whose return type encodes enabled version flags.

// Usage: For every flag, bit-or the flag into the length of dummy, guarded by a define for that flag.
struct GpuRtVersionFlagsContainer {
  int dummy[0
            // Example:
            //    |  (uint32_t)GpuRtVersionFlag::EnableSomeFeature
  ];
};

// This function is never called. It is exported by GpuRt, and LLPC inspects
// its return type to retrieve versioning flags.
export GpuRtVersionFlagsContainer _cont_GpurtVersionFlags() {
  GpuRtVersionFlagsContainer result;
  return result;
}

#endif

#endif
