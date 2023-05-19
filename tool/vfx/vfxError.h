/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
***********************************************************************************************************************
* @file  vfxError.h
* @brief PARSE_ERROR and PARSE_WARNING macros
***********************************************************************************************************************
*/

#pragma once

#include "vfx.h"
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#define vfxSnprintf(buf, len, ...) _snprintf_s((buf), (len), _TRUNCATE, __VA_ARGS__)
#else
#define vfxSnprintf(buf, len, ...) snprintf((buf), (len), __VA_ARGS__)
#endif

const static int ERROR_BUF_LEN = 4096;

#define PARSE_ERROR(errorMsg, lineNum, ...)                                                                            \
  {                                                                                                                    \
    char errorBuf[ERROR_BUF_LEN];                                                                                      \
    int pos = vfxSnprintf(errorBuf, ERROR_BUF_LEN, "Parse error at line %u: ", lineNum);                               \
    pos += vfxSnprintf(errorBuf + pos, ERROR_BUF_LEN - pos, __VA_ARGS__);                                              \
    pos += vfxSnprintf(errorBuf + pos, ERROR_BUF_LEN - pos, "\n");                                                     \
    errorMsg += errorBuf;                                                                                              \
    fputs(errorBuf, stderr);                                                                                           \
    exit(1);                                                                                                           \
  }

#define PARSE_WARNING(errorMsg, lineNum, ...)                                                                          \
  {                                                                                                                    \
    char errorBuf[ERROR_BUF_LEN];                                                                                      \
    int pos = vfxSnprintf(errorBuf, ERROR_BUF_LEN, "Parse warning at line %u: ", lineNum);                             \
    pos += vfxSnprintf(errorBuf + pos, ERROR_BUF_LEN - pos, __VA_ARGS__);                                              \
    pos += vfxSnprintf(errorBuf + pos, ERROR_BUF_LEN - pos, "\n");                                                     \
    VFX_ASSERT(pos < ERROR_BUF_LEN);                                                                                   \
    errorMsg += errorBuf;                                                                                              \
  }
