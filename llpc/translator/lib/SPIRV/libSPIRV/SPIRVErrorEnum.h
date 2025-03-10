/* Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved. */

#ifndef SPIRV_LIBSPIRV_SPIRVERRORENUM_H
#define SPIRV_LIBSPIRV_SPIRVERRORENUM_H

/* The error code name should be meaningful since it is part of error message */
_SPIRV_OP(Success, "")
_SPIRV_OP(InvalidTargetTriple, "Expects spir-unknown-unknown or spir64-unknown-unknown.")
_SPIRV_OP(InvalidAddressingModel, "Expects 0-2.")
_SPIRV_OP(InvalidMemoryModel, "Expects 0-3.")
_SPIRV_OP(InvalidFunctionControlMask, "")
_SPIRV_OP(InvalidBuiltinSetName, "Expects GLSL.std.")
_SPIRV_OP(InvalidFunctionCall, "Unexpected llvm intrinsic:")

#endif
