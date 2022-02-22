/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
  **********************************************************************************************************************
  * @file spv_amd_shader_early_and_late_fragment_tests.h
  * @brief Export SPV_AMD_shader_early_and_late_fragment_tests before it is released for internal use
  **********************************************************************************************************************
  */
#pragma once

namespace spv {

    enum ExecutionMode;

    static const ExecutionMode ExecutionModeEarlyAndLateFragmentTestsAMD = static_cast<ExecutionMode>(5017);
    static const ExecutionMode ExecutionModeStencilRefUnchangedFrontAMD  = static_cast<ExecutionMode>(5079);
    static const ExecutionMode ExecutionModeStencilRefGreaterFrontAMD    = static_cast<ExecutionMode>(5080);
    static const ExecutionMode ExecutionModeStencilRefLessFrontAMD       = static_cast<ExecutionMode>(5081);
    static const ExecutionMode ExecutionModeStencilRefUnchangedBackAMD   = static_cast<ExecutionMode>(5082);
    static const ExecutionMode ExecutionModeStencilRefGreaterBackAMD     = static_cast<ExecutionMode>(5083);
    static const ExecutionMode ExecutionModeStencilRefLessBackAMD        = static_cast<ExecutionMode>(5084);
}  // end namespace spv
