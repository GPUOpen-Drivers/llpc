/*
************************************************************************************************************************
*
*  Copyright (C) 2022 Advanced Micro Devices, Inc. All rights reserved.
*
***********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  NonSemanticDebugPrintf.h
 * @brief SPIR-V header file: proxy to the real Khronos SPIR-V header.
 ***********************************************************************************************************************
 */

#pragma once

#if EXTERNAL_SPIRV_HEADERS
#include "spirv/unified1/NonSemanticDebugPrintf.h"
#else
#include "spirv/NonSemanticDebugPrintf.h"
#endif
