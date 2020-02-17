/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilderImplImage.cpp
 * @brief LLPC source file: implementation of Builder methods for image operations
 ***********************************************************************************************************************
 */
#include "llpcBuilderImpl.h"
#include "llpcInternal.h"
#include "llpcTargetInfo.h"

#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "llpc-builder-impl-image"

using namespace Llpc;
using namespace llvm;

// Intrinsic ID table for getresinfo
static const Intrinsic::ID ImageGetResInfoIntrinsicTable[] =
{
    Intrinsic::amdgcn_image_getresinfo_1d,
    Intrinsic::amdgcn_image_getresinfo_2d,
    Intrinsic::amdgcn_image_getresinfo_3d,
    Intrinsic::amdgcn_image_getresinfo_cube,
    Intrinsic::amdgcn_image_getresinfo_1darray,
    Intrinsic::amdgcn_image_getresinfo_2darray,
    Intrinsic::amdgcn_image_getresinfo_2dmsaa,
    Intrinsic::amdgcn_image_getresinfo_2darraymsaa
};

// Intrinsic ID table for getlod
static const Intrinsic::ID ImageGetLodIntrinsicTable[] =
{
    Intrinsic::amdgcn_image_getlod_1d,
    Intrinsic::amdgcn_image_getlod_2d,
    Intrinsic::amdgcn_image_getlod_3d,
    Intrinsic::amdgcn_image_getlod_cube,
    Intrinsic::not_intrinsic,
    Intrinsic::not_intrinsic,
    Intrinsic::not_intrinsic,
    Intrinsic::not_intrinsic
};

// Intrinsic ID table for image load
static const Intrinsic::ID ImageLoadIntrinsicTable[] =
{
    Intrinsic::amdgcn_image_load_1d,
    Intrinsic::amdgcn_image_load_2d,
    Intrinsic::amdgcn_image_load_3d,
    Intrinsic::amdgcn_image_load_cube,
    Intrinsic::amdgcn_image_load_1darray,
    Intrinsic::amdgcn_image_load_2darray,
    Intrinsic::amdgcn_image_load_2dmsaa,
    Intrinsic::amdgcn_image_load_2darraymsaa
};

// Intrinsic ID table for image load mip
static const Intrinsic::ID ImageLoadMipIntrinsicTable[] =
{
    Intrinsic::amdgcn_image_load_mip_1d,
    Intrinsic::amdgcn_image_load_mip_2d,
    Intrinsic::amdgcn_image_load_mip_3d,
    Intrinsic::amdgcn_image_load_mip_cube,
    Intrinsic::amdgcn_image_load_mip_1darray,
    Intrinsic::amdgcn_image_load_mip_2darray,
};

// Intrinsic ID table for image store
static const Intrinsic::ID ImageStoreIntrinsicTable[] =
{
    Intrinsic::amdgcn_image_store_1d,
    Intrinsic::amdgcn_image_store_2d,
    Intrinsic::amdgcn_image_store_3d,
    Intrinsic::amdgcn_image_store_cube,
    Intrinsic::amdgcn_image_store_1darray,
    Intrinsic::amdgcn_image_store_2darray,
    Intrinsic::amdgcn_image_store_2dmsaa,
    Intrinsic::amdgcn_image_store_2darraymsaa
};

// Intrinsic ID table for image store mip
static const Intrinsic::ID ImageStoreMipIntrinsicTable[] =
{
    Intrinsic::amdgcn_image_store_mip_1d,
    Intrinsic::amdgcn_image_store_mip_2d,
    Intrinsic::amdgcn_image_store_mip_3d,
    Intrinsic::amdgcn_image_store_mip_cube,
    Intrinsic::amdgcn_image_store_mip_1darray,
    Intrinsic::amdgcn_image_store_mip_2darray,
};

// Table entry in image sample and image gather tables
struct IntrinsicTableEntry
{
    uint32_t matchMask;
    Intrinsic::ID ids[6];
};

// Intrinsic ID table for image gather.
// There are no entries for _lz variants; a _l variant with lod of constant 0 gets optimized
// later on into _lz.
// There are no entries for _cd variants; the Builder interface does not expose coarse derivatives.
static const IntrinsicTableEntry ImageGather4IntrinsicTable[] =
{
    {
        (1U << Builder::ImageAddressIdxCoordinate),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxLodBias),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_b_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_b_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_b_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodBias) |
        (1U << Builder::ImageAddressIdxLodClamp),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_b_cl_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_b_cl_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_b_cl_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodBias) |
        (1U << Builder::ImageAddressIdxLodClamp) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_b_cl_o_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_b_cl_o_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_b_cl_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxLodBias) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_b_o_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_b_o_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_b_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxLodBias),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_b_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_b_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_b_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxLodBias) | (1U << Builder::ImageAddressIdxLodClamp),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_b_cl_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_b_cl_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_b_cl_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxLodBias) | (1U << Builder::ImageAddressIdxLodClamp) |
        (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_b_cl_o_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_b_cl_o_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_b_cl_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxLodBias) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_b_o_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_b_o_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_b_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxLodClamp),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_cl_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_cl_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_cl_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxLodClamp) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_cl_o_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_cl_o_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_cl_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxLod),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_l_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_l_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_l_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxLod) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_l_o_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_l_o_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_l_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxZCompare) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_o_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_o_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_c_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodClamp),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_cl_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_cl_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_cl_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxLodClamp) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_cl_o_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_cl_o_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_cl_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxLod),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_l_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_l_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_l_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxLod) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_l_o_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_l_o_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_l_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_o_2d,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_o_cube,
            Intrinsic::not_intrinsic,
            Intrinsic::amdgcn_image_gather4_o_2darray
        }
    },
    {
        0
    }
};

// Intrinsic ID table for image sample.
// There are no entries for _lz variants; a _l variant with lod of constant 0 gets optimized
// later on into _lz.
// There are no entries for _cd variants; the Builder interface does not expose coarse derivatives.
static const IntrinsicTableEntry ImageSampleIntrinsicTable[] =
{
    {
        (1U << Builder::ImageAddressIdxCoordinate),
        {
            Intrinsic::amdgcn_image_sample_1d,
            Intrinsic::amdgcn_image_sample_2d,
            Intrinsic::amdgcn_image_sample_3d,
            Intrinsic::amdgcn_image_sample_cube,
            Intrinsic::amdgcn_image_sample_1darray,
            Intrinsic::amdgcn_image_sample_2darray,
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxLodBias),
        {
            Intrinsic::amdgcn_image_sample_b_1d,
            Intrinsic::amdgcn_image_sample_b_2d,
            Intrinsic::amdgcn_image_sample_b_3d,
            Intrinsic::amdgcn_image_sample_b_cube,
            Intrinsic::amdgcn_image_sample_b_1darray,
            Intrinsic::amdgcn_image_sample_b_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxLodBias) |
        (1U << Builder::ImageAddressIdxLodClamp),
        {
            Intrinsic::amdgcn_image_sample_b_cl_1d,
            Intrinsic::amdgcn_image_sample_b_cl_2d,
            Intrinsic::amdgcn_image_sample_b_cl_3d,
            Intrinsic::amdgcn_image_sample_b_cl_cube,
            Intrinsic::amdgcn_image_sample_b_cl_1darray,
            Intrinsic::amdgcn_image_sample_b_cl_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxLodBias) |
        (1U << Builder::ImageAddressIdxLodClamp) |
        (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::amdgcn_image_sample_b_cl_o_1d,
            Intrinsic::amdgcn_image_sample_b_cl_o_2d,
            Intrinsic::amdgcn_image_sample_b_cl_o_3d,
            Intrinsic::amdgcn_image_sample_b_cl_o_cube,
            Intrinsic::amdgcn_image_sample_b_cl_o_1darray,
            Intrinsic::amdgcn_image_sample_b_cl_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxLodBias) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::amdgcn_image_sample_b_o_1d,
            Intrinsic::amdgcn_image_sample_b_o_2d,
            Intrinsic::amdgcn_image_sample_b_o_3d,
            Intrinsic::amdgcn_image_sample_b_o_cube,
            Intrinsic::amdgcn_image_sample_b_o_1darray,
            Intrinsic::amdgcn_image_sample_b_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxZCompare),
        {
            Intrinsic::amdgcn_image_sample_c_1d,
            Intrinsic::amdgcn_image_sample_c_2d,
            Intrinsic::amdgcn_image_sample_c_3d,
            Intrinsic::amdgcn_image_sample_c_cube,
            Intrinsic::amdgcn_image_sample_c_1darray,
            Intrinsic::amdgcn_image_sample_c_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxLodBias),
        {
            Intrinsic::amdgcn_image_sample_c_b_1d,
            Intrinsic::amdgcn_image_sample_c_b_2d,
            Intrinsic::amdgcn_image_sample_c_b_3d,
            Intrinsic::amdgcn_image_sample_c_b_cube,
            Intrinsic::amdgcn_image_sample_c_b_1darray,
            Intrinsic::amdgcn_image_sample_c_b_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxLodBias) |
        (1U << Builder::ImageAddressIdxLodClamp),
        {
            Intrinsic::amdgcn_image_sample_c_b_cl_1d,
            Intrinsic::amdgcn_image_sample_c_b_cl_2d,
            Intrinsic::amdgcn_image_sample_c_b_cl_3d,
            Intrinsic::amdgcn_image_sample_c_b_cl_cube,
            Intrinsic::amdgcn_image_sample_c_b_cl_1darray,
            Intrinsic::amdgcn_image_sample_c_b_cl_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxZCompare) | (1U << Builder::ImageAddressIdxLodBias) |
        (1U << Builder::ImageAddressIdxLodClamp) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::amdgcn_image_sample_c_b_cl_o_1d,
            Intrinsic::amdgcn_image_sample_c_b_cl_o_2d,
            Intrinsic::amdgcn_image_sample_c_b_cl_o_3d,
            Intrinsic::amdgcn_image_sample_c_b_cl_o_cube,
            Intrinsic::amdgcn_image_sample_c_b_cl_o_1darray,
            Intrinsic::amdgcn_image_sample_c_b_cl_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxLodBias) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::amdgcn_image_sample_c_b_o_1d,
            Intrinsic::amdgcn_image_sample_c_b_o_2d,
            Intrinsic::amdgcn_image_sample_c_b_o_3d,
            Intrinsic::amdgcn_image_sample_c_b_o_cube,
            Intrinsic::amdgcn_image_sample_c_b_o_1darray,
            Intrinsic::amdgcn_image_sample_c_b_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxLodClamp),
        {
            Intrinsic::amdgcn_image_sample_c_cl_1d,
            Intrinsic::amdgcn_image_sample_c_cl_2d,
            Intrinsic::amdgcn_image_sample_c_cl_3d,
            Intrinsic::amdgcn_image_sample_c_cl_cube,
            Intrinsic::amdgcn_image_sample_c_cl_1darray,
            Intrinsic::amdgcn_image_sample_c_cl_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxLodClamp) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::amdgcn_image_sample_c_cl_o_1d,
            Intrinsic::amdgcn_image_sample_c_cl_o_2d,
            Intrinsic::amdgcn_image_sample_c_cl_o_3d,
            Intrinsic::amdgcn_image_sample_c_cl_o_cube,
            Intrinsic::amdgcn_image_sample_c_cl_o_1darray,
            Intrinsic::amdgcn_image_sample_c_cl_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxDerivativeX) | (1U << Builder::ImageAddressIdxDerivativeY),
        {
            Intrinsic::amdgcn_image_sample_c_d_1d,
            Intrinsic::amdgcn_image_sample_c_d_2d,
            Intrinsic::amdgcn_image_sample_c_d_3d,
            Intrinsic::amdgcn_image_sample_c_d_cube,
            Intrinsic::amdgcn_image_sample_c_d_1darray,
            Intrinsic::amdgcn_image_sample_c_d_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxDerivativeX) | (1U << Builder::ImageAddressIdxDerivativeY) |
        (1U << Builder::ImageAddressIdxLodClamp),
        {
            Intrinsic::amdgcn_image_sample_c_d_cl_1d,
            Intrinsic::amdgcn_image_sample_c_d_cl_2d,
            Intrinsic::amdgcn_image_sample_c_d_cl_3d,
            Intrinsic::amdgcn_image_sample_c_d_cl_cube,
            Intrinsic::amdgcn_image_sample_c_d_cl_1darray,
            Intrinsic::amdgcn_image_sample_c_d_cl_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxZCompare) |
        (1U << Builder::ImageAddressIdxDerivativeX) | (1U << Builder::ImageAddressIdxDerivativeY) |
        (1U << Builder::ImageAddressIdxLodClamp) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::amdgcn_image_sample_c_d_cl_o_1d,
            Intrinsic::amdgcn_image_sample_c_d_cl_o_2d,
            Intrinsic::amdgcn_image_sample_c_d_cl_o_3d,
            Intrinsic::amdgcn_image_sample_c_d_cl_o_cube,
            Intrinsic::amdgcn_image_sample_c_d_cl_o_1darray,
            Intrinsic::amdgcn_image_sample_c_d_cl_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxDerivativeX) | (1U << Builder::ImageAddressIdxDerivativeY) |
        (1U << Builder::ImageAddressIdxZCompare) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::amdgcn_image_sample_c_d_o_1d,
            Intrinsic::amdgcn_image_sample_c_d_o_2d,
            Intrinsic::amdgcn_image_sample_c_d_o_3d,
            Intrinsic::amdgcn_image_sample_c_d_o_cube,
            Intrinsic::amdgcn_image_sample_c_d_o_1darray,
            Intrinsic::amdgcn_image_sample_c_d_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxLod) | (1U << Builder::ImageAddressIdxZCompare),
        {
            Intrinsic::amdgcn_image_sample_c_l_1d,
            Intrinsic::amdgcn_image_sample_c_l_2d,
            Intrinsic::amdgcn_image_sample_c_l_3d,
            Intrinsic::amdgcn_image_sample_c_l_cube,
            Intrinsic::amdgcn_image_sample_c_l_1darray,
            Intrinsic::amdgcn_image_sample_c_l_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxOffset) |
        (1U << Builder::ImageAddressIdxLod) | (1U << Builder::ImageAddressIdxZCompare),
        {
            Intrinsic::amdgcn_image_sample_c_l_o_1d,
            Intrinsic::amdgcn_image_sample_c_l_o_2d,
            Intrinsic::amdgcn_image_sample_c_l_o_3d,
            Intrinsic::amdgcn_image_sample_c_l_o_cube,
            Intrinsic::amdgcn_image_sample_c_l_o_1darray,
            Intrinsic::amdgcn_image_sample_c_l_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxOffset) |
        (1U << Builder::ImageAddressIdxZCompare),
        {
            Intrinsic::amdgcn_image_sample_c_o_1d,
            Intrinsic::amdgcn_image_sample_c_o_2d,
            Intrinsic::amdgcn_image_sample_c_o_3d,
            Intrinsic::amdgcn_image_sample_c_o_cube,
            Intrinsic::amdgcn_image_sample_c_o_1darray,
            Intrinsic::amdgcn_image_sample_c_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxLodClamp),
        {
            Intrinsic::amdgcn_image_sample_cl_1d,
            Intrinsic::amdgcn_image_sample_cl_2d,
            Intrinsic::amdgcn_image_sample_cl_3d,
            Intrinsic::amdgcn_image_sample_cl_cube,
            Intrinsic::amdgcn_image_sample_cl_1darray,
            Intrinsic::amdgcn_image_sample_cl_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxLodClamp) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::amdgcn_image_sample_cl_o_1d,
            Intrinsic::amdgcn_image_sample_cl_o_2d,
            Intrinsic::amdgcn_image_sample_cl_o_3d,
            Intrinsic::amdgcn_image_sample_cl_o_cube,
            Intrinsic::amdgcn_image_sample_cl_o_1darray,
            Intrinsic::amdgcn_image_sample_cl_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxDerivativeX) | (1U << Builder::ImageAddressIdxDerivativeY),
        {
            Intrinsic::amdgcn_image_sample_d_1d,
            Intrinsic::amdgcn_image_sample_d_2d,
            Intrinsic::amdgcn_image_sample_d_3d,
            Intrinsic::amdgcn_image_sample_d_cube,
            Intrinsic::amdgcn_image_sample_d_1darray,
            Intrinsic::amdgcn_image_sample_d_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxDerivativeX) | (1U << Builder::ImageAddressIdxDerivativeY) |
        (1U << Builder::ImageAddressIdxLodClamp),
        {
            Intrinsic::amdgcn_image_sample_d_cl_1d,
            Intrinsic::amdgcn_image_sample_d_cl_2d,
            Intrinsic::amdgcn_image_sample_d_cl_3d,
            Intrinsic::amdgcn_image_sample_d_cl_cube,
            Intrinsic::amdgcn_image_sample_d_cl_1darray,
            Intrinsic::amdgcn_image_sample_d_cl_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxDerivativeX) | (1U << Builder::ImageAddressIdxDerivativeY) |
        (1U << Builder::ImageAddressIdxLodClamp) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::amdgcn_image_sample_d_cl_o_1d,
            Intrinsic::amdgcn_image_sample_d_cl_o_2d,
            Intrinsic::amdgcn_image_sample_d_cl_o_3d,
            Intrinsic::amdgcn_image_sample_d_cl_o_cube,
            Intrinsic::amdgcn_image_sample_d_cl_o_1darray,
            Intrinsic::amdgcn_image_sample_d_cl_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) |
        (1U << Builder::ImageAddressIdxDerivativeX) | (1U << Builder::ImageAddressIdxDerivativeY) |
        (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::amdgcn_image_sample_d_o_1d,
            Intrinsic::amdgcn_image_sample_d_o_2d,
            Intrinsic::amdgcn_image_sample_d_o_3d,
            Intrinsic::amdgcn_image_sample_d_o_cube,
            Intrinsic::amdgcn_image_sample_d_o_1darray,
            Intrinsic::amdgcn_image_sample_d_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLod),
        {
            Intrinsic::amdgcn_image_sample_l_1d,
            Intrinsic::amdgcn_image_sample_l_2d,
            Intrinsic::amdgcn_image_sample_l_3d,
            Intrinsic::amdgcn_image_sample_l_cube,
            Intrinsic::amdgcn_image_sample_l_1darray,
            Intrinsic::amdgcn_image_sample_l_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLod) |
        (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::amdgcn_image_sample_l_o_1d,
            Intrinsic::amdgcn_image_sample_l_o_2d,
            Intrinsic::amdgcn_image_sample_l_o_3d,
            Intrinsic::amdgcn_image_sample_l_o_cube,
            Intrinsic::amdgcn_image_sample_l_o_1darray,
            Intrinsic::amdgcn_image_sample_l_o_2darray
        }
    },
    {
        (1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxOffset),
        {
            Intrinsic::amdgcn_image_sample_o_1d,
            Intrinsic::amdgcn_image_sample_o_2d,
            Intrinsic::amdgcn_image_sample_o_3d,
            Intrinsic::amdgcn_image_sample_o_cube,
            Intrinsic::amdgcn_image_sample_o_1darray,
            Intrinsic::amdgcn_image_sample_o_2darray
        }
    },
    {
        0
    }
};

// Intrinsic ID table for struct buffer atomic
static const Intrinsic::ID StructBufferAtomicIntrinsicTable[] =
{
    Intrinsic::amdgcn_struct_buffer_atomic_swap,
    Intrinsic::amdgcn_struct_buffer_atomic_cmpswap,
    Intrinsic::amdgcn_struct_buffer_atomic_add,
    Intrinsic::amdgcn_struct_buffer_atomic_sub,
    Intrinsic::amdgcn_struct_buffer_atomic_smin,
    Intrinsic::amdgcn_struct_buffer_atomic_umin,
    Intrinsic::amdgcn_struct_buffer_atomic_smax,
    Intrinsic::amdgcn_struct_buffer_atomic_umax,
    Intrinsic::amdgcn_struct_buffer_atomic_and,
    Intrinsic::amdgcn_struct_buffer_atomic_or,
    Intrinsic::amdgcn_struct_buffer_atomic_xor
};

// Intrinsic ID table for image atomic
static const Intrinsic::ID ImageAtomicIntrinsicTable[][8] =
{
    {
        Intrinsic::amdgcn_image_atomic_swap_1d,
        Intrinsic::amdgcn_image_atomic_swap_2d,
        Intrinsic::amdgcn_image_atomic_swap_3d,
        Intrinsic::amdgcn_image_atomic_swap_cube,
        Intrinsic::amdgcn_image_atomic_swap_1darray,
        Intrinsic::amdgcn_image_atomic_swap_2darray,
        Intrinsic::amdgcn_image_atomic_swap_2dmsaa,
        Intrinsic::amdgcn_image_atomic_swap_2darraymsaa
    },
    {
        Intrinsic::amdgcn_image_atomic_cmpswap_1d,
        Intrinsic::amdgcn_image_atomic_cmpswap_2d,
        Intrinsic::amdgcn_image_atomic_cmpswap_3d,
        Intrinsic::amdgcn_image_atomic_cmpswap_cube,
        Intrinsic::amdgcn_image_atomic_cmpswap_1darray,
        Intrinsic::amdgcn_image_atomic_cmpswap_2darray,
        Intrinsic::amdgcn_image_atomic_cmpswap_2dmsaa,
        Intrinsic::amdgcn_image_atomic_cmpswap_2darraymsaa
    },
    {
        Intrinsic::amdgcn_image_atomic_add_1d,
        Intrinsic::amdgcn_image_atomic_add_2d,
        Intrinsic::amdgcn_image_atomic_add_3d,
        Intrinsic::amdgcn_image_atomic_add_cube,
        Intrinsic::amdgcn_image_atomic_add_1darray,
        Intrinsic::amdgcn_image_atomic_add_2darray,
        Intrinsic::amdgcn_image_atomic_add_2dmsaa,
        Intrinsic::amdgcn_image_atomic_add_2darraymsaa
    },
    {
        Intrinsic::amdgcn_image_atomic_sub_1d,
        Intrinsic::amdgcn_image_atomic_sub_2d,
        Intrinsic::amdgcn_image_atomic_sub_3d,
        Intrinsic::amdgcn_image_atomic_sub_cube,
        Intrinsic::amdgcn_image_atomic_sub_1darray,
        Intrinsic::amdgcn_image_atomic_sub_2darray,
        Intrinsic::amdgcn_image_atomic_sub_2dmsaa,
        Intrinsic::amdgcn_image_atomic_sub_2darraymsaa
    },
    {
        Intrinsic::amdgcn_image_atomic_smin_1d,
        Intrinsic::amdgcn_image_atomic_smin_2d,
        Intrinsic::amdgcn_image_atomic_smin_3d,
        Intrinsic::amdgcn_image_atomic_smin_cube,
        Intrinsic::amdgcn_image_atomic_smin_1darray,
        Intrinsic::amdgcn_image_atomic_smin_2darray,
        Intrinsic::amdgcn_image_atomic_smin_2dmsaa,
        Intrinsic::amdgcn_image_atomic_smin_2darraymsaa
    },
    {
        Intrinsic::amdgcn_image_atomic_umin_1d,
        Intrinsic::amdgcn_image_atomic_umin_2d,
        Intrinsic::amdgcn_image_atomic_umin_3d,
        Intrinsic::amdgcn_image_atomic_umin_cube,
        Intrinsic::amdgcn_image_atomic_umin_1darray,
        Intrinsic::amdgcn_image_atomic_umin_2darray,
        Intrinsic::amdgcn_image_atomic_umin_2dmsaa,
        Intrinsic::amdgcn_image_atomic_umin_2darraymsaa
    },
    {
        Intrinsic::amdgcn_image_atomic_smax_1d,
        Intrinsic::amdgcn_image_atomic_smax_2d,
        Intrinsic::amdgcn_image_atomic_smax_3d,
        Intrinsic::amdgcn_image_atomic_smax_cube,
        Intrinsic::amdgcn_image_atomic_smax_1darray,
        Intrinsic::amdgcn_image_atomic_smax_2darray,
        Intrinsic::amdgcn_image_atomic_smax_2dmsaa,
        Intrinsic::amdgcn_image_atomic_smax_2darraymsaa
    },
    {
        Intrinsic::amdgcn_image_atomic_umax_1d,
        Intrinsic::amdgcn_image_atomic_umax_2d,
        Intrinsic::amdgcn_image_atomic_umax_3d,
        Intrinsic::amdgcn_image_atomic_umax_cube,
        Intrinsic::amdgcn_image_atomic_umax_1darray,
        Intrinsic::amdgcn_image_atomic_umax_2darray,
        Intrinsic::amdgcn_image_atomic_umax_2dmsaa,
        Intrinsic::amdgcn_image_atomic_umax_2darraymsaa
    },
    {
        Intrinsic::amdgcn_image_atomic_and_1d,
        Intrinsic::amdgcn_image_atomic_and_2d,
        Intrinsic::amdgcn_image_atomic_and_3d,
        Intrinsic::amdgcn_image_atomic_and_cube,
        Intrinsic::amdgcn_image_atomic_and_1darray,
        Intrinsic::amdgcn_image_atomic_and_2darray,
        Intrinsic::amdgcn_image_atomic_and_2dmsaa,
        Intrinsic::amdgcn_image_atomic_and_2darraymsaa
    },
    {
        Intrinsic::amdgcn_image_atomic_or_1d,
        Intrinsic::amdgcn_image_atomic_or_2d,
        Intrinsic::amdgcn_image_atomic_or_3d,
        Intrinsic::amdgcn_image_atomic_or_cube,
        Intrinsic::amdgcn_image_atomic_or_1darray,
        Intrinsic::amdgcn_image_atomic_or_2darray,
        Intrinsic::amdgcn_image_atomic_or_2dmsaa,
        Intrinsic::amdgcn_image_atomic_or_2darraymsaa
    },
    {
        Intrinsic::amdgcn_image_atomic_xor_1d,
        Intrinsic::amdgcn_image_atomic_xor_2d,
        Intrinsic::amdgcn_image_atomic_xor_3d,
        Intrinsic::amdgcn_image_atomic_xor_cube,
        Intrinsic::amdgcn_image_atomic_xor_1darray,
        Intrinsic::amdgcn_image_atomic_xor_2darray,
        Intrinsic::amdgcn_image_atomic_xor_2dmsaa,
        Intrinsic::amdgcn_image_atomic_xor_2darraymsaa
    },
};

// =====================================================================================================================
// Convert an integer or vector of integer type to the equivalent (vector of) half/float/double
static Type* ConvertToFloatingPointType(
    Type* pOrigTy)    // Original type
{
    LLPC_ASSERT(pOrigTy->isIntOrIntVectorTy());
    Type* pNewTy = pOrigTy;
    switch (pNewTy->getScalarType()->getIntegerBitWidth())
    {
    case 16:
        pNewTy = Type::getHalfTy(pNewTy->getContext());
        break;
    case 32:
        pNewTy = Type::getFloatTy(pNewTy->getContext());
        break;
    default:
        LLPC_NEVER_CALLED();
    }
    if (isa<VectorType>(pOrigTy))
    {
        pNewTy = VectorType::get(pNewTy, pOrigTy->getVectorNumElements());
    }
    return pNewTy;
}

// =====================================================================================================================
// Create an image load.
Value* BuilderImplImage::CreateImageLoad(
    Type*             pResultTy,          // [in] Result type
    uint32_t          dim,                // Image dimension
    uint32_t          flags,              // ImageFlag* flags
    Value*            pImageDesc,         // [in] Image descriptor
    Value*            pCoord,             // [in] Coordinates: scalar or vector i32
    Value*            pMipLevel,          // [in] Mipmap level if doing load_mip, otherwise nullptr
    const Twine&      instName)           // [in] Name to give instruction(s)
{
    GetPipelineState()->GetShaderResourceUsage(m_shaderStage)->resourceRead = true;
    LLPC_ASSERT(pCoord->getType()->getScalarType()->isIntegerTy(32));
    pImageDesc = PatchCubeDescriptor(pImageDesc, dim);
    pCoord = HandleFragCoordViewIndex(pCoord, flags, dim);

    uint32_t dmask = 1;
    Type* pOrigTexelTy = pResultTy;
    if (auto pStructResultTy = dyn_cast<StructType>(pResultTy))
    {
        pOrigTexelTy = pStructResultTy->getElementType(0);
    }

    Type* pTexelTy = pOrigTexelTy;
    if (pOrigTexelTy->isIntOrIntVectorTy(64))
    {
        // Only load the first component for 64-bit texel, casted to <2 x i32>
        pTexelTy = VectorType::get(getInt32Ty(), 2);
    }

    if (auto pVectorResultTy = dyn_cast<VectorType>(pTexelTy))
    {
        dmask = (1U << pVectorResultTy->getNumElements()) - 1;
    }

    // Prepare the coordinate, which might also change the dimension.
    SmallVector<Value*, 4> coords;
    SmallVector<Value*, 6> derivatives;
    dim = PrepareCoordinate(dim,
                            pCoord,
                            nullptr,
                            nullptr,
                            nullptr,
                            coords,
                            derivatives);

    Type* pIntrinsicDataTy = nullptr;
    if (isa<StructType>(pResultTy))
    {
        // TFE
        pIntrinsicDataTy = StructType::get(pTexelTy->getContext(), { pTexelTy, getInt32Ty() });
    }
    else
    {
        pIntrinsicDataTy = pTexelTy;
    }

    SmallVector<Value*, 16> args;
    Value* pResult = nullptr;
    uint32_t imageDescArgIndex = 0;
    if (pImageDesc->getType() == GetImageDescTy())
    {
        // Not texel buffer; use image load instruction.
        // Build the intrinsic arguments.
        bool tfe = isa<StructType>(pIntrinsicDataTy);
        args.push_back(getInt32(dmask));
        args.insert(args.end(), coords.begin(), coords.end());

        if (pMipLevel != nullptr)
        {
            args.push_back(pMipLevel);
        }
        imageDescArgIndex = args.size();
        args.push_back(pImageDesc);
        args.push_back(getInt32(tfe));
        args.push_back(getInt32(((flags & ImageFlagCoherent) ? 1 : 0) | ((flags & ImageFlagVolatile) ? 2 : 0)));

        // Get the intrinsic ID from the load intrinsic ID table and call it.
        auto pTable = (pMipLevel != nullptr) ? &ImageLoadMipIntrinsicTable[0] : &ImageLoadIntrinsicTable[0];
        pResult = CreateIntrinsic(pTable[dim],
                                  { pIntrinsicDataTy, coords[0]->getType() },
                                  args,
                                  nullptr,
                                  instName);
    }
    else
    {
        // Texel buffer descriptor. Use the buffer instruction.
        imageDescArgIndex = args.size();
        args.push_back(pImageDesc);
        args.push_back(coords[0]);
        args.push_back(getInt32(0));
        args.push_back(getInt32(0));
        args.push_back(getInt32(0));
        pResult = CreateIntrinsic(Intrinsic::amdgcn_struct_buffer_load_format,
                                  pIntrinsicDataTy,
                                  args,
                                  nullptr,
                                  instName);
    }

    // For 64-bit texel, only the first component is loaded, other components are filled in with (0, 0, 1). This
    // operation could be viewed as supplement of the intrinsic call.
    if (pOrigTexelTy->isIntOrIntVectorTy(64))
    {
        Value* pTexel = pResult;
        if (isa<StructType>(pResultTy))
        {
            pTexel = CreateExtractValue(pResult, uint64_t(0));
        }
        pTexel = CreateBitCast(pTexel, getInt64Ty()); // Casted to i64

        if (pOrigTexelTy->isVectorTy())
        {
            pTexel = CreateInsertElement(UndefValue::get(pOrigTexelTy), pTexel, uint64_t(0));

            SmallVector<Value*, 3> defaults = { getInt64(0), getInt64(0), getInt64(1) };
            for (uint32_t i = 1; i < pOrigTexelTy->getVectorNumElements(); ++i)
            {
                pTexel = CreateInsertElement(pTexel, defaults[i - 1], i);
            }
        }

        if (isa<StructType>(pResultTy))
        {
            // TFE
            pIntrinsicDataTy = StructType::get(pOrigTexelTy->getContext(), { pOrigTexelTy, getInt32Ty() });
            pResult = CreateInsertValue(CreateInsertValue(UndefValue::get(pIntrinsicDataTy), pTexel, uint64_t(0)),
                                        CreateExtractValue(pResult, 1),
                                        1);
        }
        else
        {
            pIntrinsicDataTy = pOrigTexelTy;
            pResult = pTexel;
        }
    }

    // Add a waterfall loop if needed.
    if (flags & ImageFlagNonUniformImage)
    {
        pResult = CreateWaterfallLoop(cast<Instruction>(pResult), imageDescArgIndex);
    }

    return pResult;
}

// =====================================================================================================================
// Create an image load with fmask. Dim must be 2DMsaa or 2DArrayMsaa. If the F-mask descriptor has a valid
// format field, then it reads "fmask_texel_R", the R component of the texel read from the given coordinates
// in the F-mask image, and calculates the sample number to use as the sample'th nibble (where sample=0 means
// the least significant nibble) of fmask_texel_R. If the F-mask descriptor has an invalid format, then it
// just uses the supplied sample number. The calculated sample is then appended to the supplied coordinates
// for a normal image load.
Value* BuilderImplImage::CreateImageLoadWithFmask(
    Type*                   pResultTy,          // [in] Result type
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    Value*                  pImageDesc,         // [in] Image descriptor
    Value*                  pFmaskDesc,         // [in] Fmask descriptor
    Value*                  pCoord,             // [in] Coordinates: scalar or vector i32, exactly right
                                                //    width for given dimension excluding sample
    Value*                  pSampleNum,         // [in] Sample number, i32
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    // Load texel from F-mask image.
    uint32_t fmaskDim = dim;
    switch (dim)
    {
    case Dim2DMsaa:
        fmaskDim = Dim2D;
        break;
    case Dim2DArrayMsaa:
        fmaskDim = Dim3D;
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }
    Value* pFmaskTexel = CreateImageLoad(VectorType::get(getInt32Ty(), 4),
                                         fmaskDim,
                                         flags,
                                         pFmaskDesc,
                                         pCoord,
                                         nullptr,
                                         instName + ".fmaskload");

    // Calculate the sample number we would use if the F-mask descriptor format is valid.
    Value* pCalcSampleNum = CreateExtractElement(pFmaskTexel, uint64_t(0));
    Value* pShiftSampleNum = CreateShl(pSampleNum, getInt32(2));
    pCalcSampleNum = CreateLShr(pCalcSampleNum, pShiftSampleNum);
    pCalcSampleNum = CreateAnd(pCalcSampleNum, getInt32(15));

    // Check whether the F-mask descriptor has a BUF_DATA_FORMAT_INVALID (0) format (dword[1].bit[20-25]).
    Value* pFmaskFormat = CreateExtractElement(pFmaskDesc, 1);
    pFmaskFormat = CreateAnd(pFmaskFormat, getInt32(63 << 20));
    Value* pFmaskValidFormat = CreateICmpNE(pFmaskFormat, getInt32(0));

    // Use that to select the calculated sample number or the provided one, then append that to the coordinates.
    pSampleNum = CreateSelect(pFmaskValidFormat, pCalcSampleNum, pSampleNum);
    pSampleNum = CreateInsertElement(UndefValue::get(pCoord->getType()), pSampleNum, uint64_t(0));
    static const uint32_t Idxs[] = { 0, 1, 2, 3 };
    pCoord = CreateShuffleVector(pCoord,
                                 pSampleNum,
                                 ArrayRef<uint32_t>(Idxs).slice(0, dim == Dim2DArrayMsaa ? 4 : 3));

    // Now do the normal load.
    return dyn_cast<Instruction>(CreateImageLoad(pResultTy, dim, flags, pImageDesc, pCoord, nullptr, instName));
}

// =====================================================================================================================
// Create an image store.
Value* BuilderImplImage::CreateImageStore(
    Value*            pTexel,             // [in] Texel value to store; v4i16, v4i32, v4i64, v4f16 or v4f32
    uint32_t          dim,                // Image dimension
    uint32_t          flags,              // ImageFlag* flags
    Value*            pImageDesc,         // [in] Image descriptor
    Value*            pCoord,             // [in] Coordinates: scalar or vector i32
    Value*            pMipLevel,          // [in] Mipmap level if doing load_mip, otherwise nullptr
    const Twine&      instName)           // [in] Name to give instruction(s)
{
    GetPipelineState()->GetShaderResourceUsage(m_shaderStage)->resourceWrite = true;
    LLPC_ASSERT(pCoord->getType()->getScalarType()->isIntegerTy(32));
    pImageDesc = PatchCubeDescriptor(pImageDesc, dim);
    pCoord = HandleFragCoordViewIndex(pCoord, flags, dim);

    // For 64-bit texel, only the first component is stored
    if (pTexel->getType()->isIntOrIntVectorTy(64))
    {
        if (pTexel->getType()->isVectorTy())
        {
            pTexel = CreateExtractElement(pTexel, uint64_t(0));
        }
        pTexel = CreateBitCast(pTexel, VectorType::get(getFloatTy(), 2)); // Casted to <2 x float>
    }

    // The intrinsics insist on an FP data type, so we need to bitcast from an integer data type.
    Type* pIntrinsicDataTy = pTexel->getType();
    if (pIntrinsicDataTy->isIntOrIntVectorTy())
    {
        pIntrinsicDataTy = ConvertToFloatingPointType(pIntrinsicDataTy);
        pTexel = CreateBitCast(pTexel, pIntrinsicDataTy);
    }

    // Prepare the coordinate, which might also change the dimension.
    SmallVector<Value*, 4> coords;
    SmallVector<Value*, 6> derivatives;
    dim = PrepareCoordinate(dim,
                            pCoord,
                            nullptr,
                            nullptr,
                            nullptr,
                            coords,
                            derivatives);

    Type* pTexelTy = pTexel->getType();
    SmallVector<Value*, 16> args;
    Instruction* pImageStore = nullptr;
    uint32_t imageDescArgIndex = 0;
    if (pImageDesc->getType() == GetImageDescTy())
    {
        // Not texel buffer; use image store instruction.
        // Build the intrinsic arguments.
        uint32_t dmask = 1;
        if (auto pVectorTexelTy = dyn_cast<VectorType>(pTexelTy))
        {
            dmask = (1U << pVectorTexelTy->getNumElements()) - 1;
        }

        // Build the intrinsic arguments.
        args.push_back(pTexel);
        args.push_back(getInt32(dmask));
        args.insert(args.end(), coords.begin(), coords.end());
        if (pMipLevel != nullptr)
        {
            args.push_back(pMipLevel);
        }
        imageDescArgIndex = args.size();
        args.push_back(pImageDesc);
        args.push_back(getInt32(0));    // tfe/lwe
        args.push_back(getInt32(((flags & ImageFlagCoherent) ? 1 : 0) | ((flags & ImageFlagVolatile) ? 2 : 0)));

        // Get the intrinsic ID from the store intrinsic ID table and call it.
        auto pTable = (pMipLevel != nullptr) ? &ImageStoreMipIntrinsicTable[0] : &ImageStoreIntrinsicTable[0];
        pImageStore = CreateIntrinsic(pTable[dim],
                               { pTexelTy, coords[0]->getType() },
                               args,
                               nullptr,
                               instName);
    }
    else
    {
        // Texel buffer descriptor. Use the buffer instruction.
        // First widen texel to vec4 if necessary.
        if (auto pVectorTexelTy = dyn_cast<VectorType>(pTexelTy))
        {
            if (pVectorTexelTy->getNumElements() != 4)
            {
                pTexel = CreateShuffleVector(pTexel, Constant::getNullValue(pTexelTy), { 0, 1, 2, 3 });
            }
        }
        else
        {
            pTexel = CreateInsertElement(Constant::getNullValue(VectorType::get(pTexelTy, 4)), pTexel, uint64_t(0));
        }

        // Do the buffer store.
        args.push_back(pTexel);
        imageDescArgIndex = args.size();
        args.push_back(pImageDesc);
        args.push_back(coords[0]);
        args.push_back(getInt32(0));
        args.push_back(getInt32(0));
        args.push_back(getInt32(0));
        pImageStore = CreateIntrinsic(Intrinsic::amdgcn_struct_buffer_store_format,
                                 pTexel->getType(),
                                 args,
                                 nullptr,
                                 instName);
    }

    // Add a waterfall loop if needed.
    if (flags & ImageFlagNonUniformImage)
    {
        CreateWaterfallLoop(pImageStore, imageDescArgIndex);
    }

    return pImageStore;
}

// =====================================================================================================================
// Implement wrapped YCbCr sample
Value* BuilderImplImage::YCbCrWrappedSample(
    YCbCrWrappedSampleInfo& wrapInfo) // [In] Wrapped YCbCr sample infomation
{
    SmallVector<Value*, 4> coordsChroma;
    YCbCrSampleInfo* pSampleInfo = wrapInfo.pYCbCrInfo;
    Value* pChromaWidth  = wrapInfo.pChromaWidth;
    Value* pChromaHeight = wrapInfo.pChromaHeight;

    if (wrapInfo.subsampledX)
    {
        pChromaWidth = CreateFMul(wrapInfo.pChromaWidth, ConstantFP::get(getFloatTy(), 0.5f));
    }

    if (wrapInfo.subsampledY)
    {
        pChromaHeight = CreateFMul(wrapInfo.pChromaHeight, ConstantFP::get(getFloatTy(), 0.5f));
    }

    coordsChroma.push_back(CreateFDiv(wrapInfo.pI, pChromaWidth));
    coordsChroma.push_back(CreateFDiv(wrapInfo.pJ, pChromaHeight));

    pSampleInfo->pImageDesc = wrapInfo.pImageDesc1;

    Value* pResult = nullptr;

    if (wrapInfo.planeNum == 1)
    {
        pSampleInfo->pImageDesc = wrapInfo.subsampledX ? wrapInfo.pImageDesc2 : wrapInfo.pImageDesc1;

        Instruction* pImageOp = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChroma, pSampleInfo));
        pResult = CreateShuffleVector(pImageOp, pImageOp, { 0, 2 });
    }
    else if (wrapInfo.planeNum == 2)
    {
        pSampleInfo->pImageDesc = wrapInfo.pImageDesc2;
        Instruction* pImageOp = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChroma, pSampleInfo));
        pResult = CreateShuffleVector(pImageOp, pImageOp, { 0, 2 });
    }
    else if (wrapInfo.planeNum == 3)
    {
        pSampleInfo->pImageDesc = wrapInfo.pImageDesc2;
        Instruction* pImageOp1 = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChroma, pSampleInfo));

        pSampleInfo->pImageDesc = wrapInfo.pImageDesc3;
        Instruction* pImageOp2 = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChroma, pSampleInfo));
        pResult = CreateShuffleVector(pImageOp2, pImageOp1, { 0, 6 });
    }
    else
    {
        LLPC_NEVER_CALLED();
    }

    return pResult;
}

// =====================================================================================================================
// Create YCbCr Reconstruct linear X chroma sample
Value* BuilderImplImage::YCbCrReconstructLinearXChromaSample(
    XChromaSampleInfo& xChromaInfo) // [In] Infomation for downsampled chroma channels in X dimension
{
    YCbCrSampleInfo* pSampleInfo = xChromaInfo.pYCbCrInfo;
    Value* pIsEvenI = CreateICmpEQ(CreateSMod(CreateFPToSI(xChromaInfo.pI, getInt32Ty()), getInt32(2)), getInt32(0));

    Value* pSubI = CreateUnaryIntrinsic(Intrinsic::floor,
                                        CreateFDiv(xChromaInfo.pI, ConstantFP::get(getFloatTy(), 2.0)));
    if (xChromaInfo.xChromaOffset != ChromaLocation::CositedEven)
    {
        pSubI = CreateSelect(pIsEvenI,
                            CreateFSub(pSubI, ConstantFP::get(getFloatTy(), 1.0)),
                            pSubI);
    }

    Value* pAlpha = nullptr;
    if (xChromaInfo.xChromaOffset == ChromaLocation::CositedEven)
    {
        pAlpha = CreateSelect(pIsEvenI, ConstantFP::get(getFloatTy(), 0.0), ConstantFP::get(getFloatTy(), 0.5));
    }
    else
    {
        pAlpha = CreateSelect(pIsEvenI, ConstantFP::get(getFloatTy(), 0.25), ConstantFP::get(getFloatTy(), 0.75));
    }

    Value* pT = CreateFDiv(xChromaInfo.pJ, xChromaInfo.pChromaHeight);

    SmallVector<Value*, 4> coordsChromaA;
    pSampleInfo->pImageDesc = xChromaInfo.pImageDesc1;
    coordsChromaA.push_back(CreateFDiv(pSubI, xChromaInfo.pChromaWidth));
    coordsChromaA.push_back(pT);
    Instruction* pImageOpA = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaA, pSampleInfo));

    SmallVector<Value*, 4> coordsChromaB;
    coordsChromaB.push_back(CreateFDiv(CreateFAdd(pSubI, ConstantFP::get(getFloatTy(), 1.0)), xChromaInfo.pChromaWidth));
    coordsChromaB.push_back(pT);
    Instruction* pImageOpB = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaB, pSampleInfo));

    Value* pResult = CreateFMix(pImageOpB, pImageOpA, pAlpha);

    return CreateShuffleVector(pResult, pResult, { 0, 2 });
}

// =====================================================================================================================
// Create YCbCr Reconstruct linear XY chroma sample
Value* BuilderImplImage::YCbCrReconstructLinearXYChromaSample(
    XYChromaSampleInfo& xyChromaInfo) // [In] Infomation for downsampled chroma channels in XY dimension
{
    YCbCrSampleInfo* pSampleInfo = xyChromaInfo.pYCbCrInfo;

    Value* pWidth = xyChromaInfo.pChromaWidth;
    Value* pHeight = xyChromaInfo.pChromaHeight;

    Value* pIsEvenI = CreateICmpEQ(CreateSMod(CreateFPToSI(xyChromaInfo.pI,  getInt32Ty()), getInt32(2)), getInt32(0));
    Value* pIsEvenJ = CreateICmpEQ(CreateSMod(CreateFPToSI(xyChromaInfo.pJ,  getInt32Ty()), getInt32(2)), getInt32(0));

    Value* pSubI = CreateUnaryIntrinsic(Intrinsic::floor,
                                        CreateFDiv(xyChromaInfo.pI, ConstantFP::get(getFloatTy(), 2.0)));
    Value* pSubJ = CreateUnaryIntrinsic(Intrinsic::floor,
                                        CreateFDiv(xyChromaInfo.pJ, ConstantFP::get(getFloatTy(), 2.0)));

    if (xyChromaInfo.xChromaOffset != ChromaLocation::CositedEven)
    {
        pSubI = CreateSelect(pIsEvenI,
                             CreateFSub(pSubI, ConstantFP::get(getFloatTy(), 1.0)),
                             pSubI);
    }

    if (xyChromaInfo.yChromaOffset != ChromaLocation::CositedEven)
    {
        pSubJ = CreateSelect(pIsEvenJ,
                             CreateFSub(pSubJ, ConstantFP::get(getFloatTy(), 1.0)),
                             pSubJ);
    }

    Value* pAlpha = nullptr;
    if (xyChromaInfo.xChromaOffset == ChromaLocation::CositedEven)
    {
        pAlpha = CreateSelect(pIsEvenI, ConstantFP::get(getFloatTy(), 0.0), ConstantFP::get(getFloatTy(), 0.5));
    }
    else
    {
        pAlpha = CreateSelect(pIsEvenI, ConstantFP::get(getFloatTy(), 0.25), ConstantFP::get(getFloatTy(), 0.75));
    }

    Value* pBeta = nullptr;
    if (xyChromaInfo.yChromaOffset == ChromaLocation::CositedEven)
    {
        pBeta = CreateSelect(pIsEvenJ, ConstantFP::get(getFloatTy(), 0.0), ConstantFP::get(getFloatTy(), 0.5));
    }
    else
    {
        pBeta = CreateSelect(pIsEvenJ, ConstantFP::get(getFloatTy(), 0.25), ConstantFP::get(getFloatTy(), 0.75));
    }

    SmallVector<Value*, 4> coordsChromaTL;
    SmallVector<Value*, 4> coordsChromaTR;
    SmallVector<Value*, 4> coordsChromaBL;
    SmallVector<Value*, 4> coordsChromaBR;

    Value* pResult = nullptr;
    if (xyChromaInfo.planeNum == 2)
    {
        pSampleInfo->pImageDesc = xyChromaInfo.pImageDesc1;

        // Sample TL
        coordsChromaTL.push_back(CreateFDiv(pSubI, pWidth));
        coordsChromaTL.push_back(CreateFDiv(pSubJ, pHeight));
        Instruction* pTL = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaTL, pSampleInfo));

        // Sample TR
        coordsChromaTR.push_back(CreateFDiv(CreateFAdd(pSubI, ConstantFP::get(getFloatTy(), 1.0)), pWidth));
        coordsChromaTR.push_back(CreateFDiv(pSubJ, pHeight));
        Instruction* pTR = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaTR, pSampleInfo));

        // Sample BL
        coordsChromaBL.push_back(CreateFDiv(pSubI, pWidth));
        coordsChromaBL.push_back(CreateFDiv(CreateFAdd(pSubJ, ConstantFP::get(getFloatTy(), 1.0)), pHeight));
        Instruction* pBL = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaBL, pSampleInfo));

        // Sample BR
        coordsChromaBR.push_back(CreateFDiv(CreateFAdd(pSubI, ConstantFP::get(getFloatTy(), 1.0)), pWidth));
        coordsChromaBR.push_back(CreateFDiv(CreateFAdd(pSubJ, ConstantFP::get(getFloatTy(), 1.0)), pHeight));
        Instruction* pBR = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaBR, pSampleInfo));

        // Linear interpolate
        pResult = BilinearBlend(pAlpha, pBeta, pTL, pTR, pBL, pBR);
        pResult = CreateShuffleVector(pResult, pResult, { 0, 2});
    }
    else if (xyChromaInfo.planeNum == 3)
    {
        // Sample TL
        coordsChromaTL.push_back(CreateFDiv(pSubI, pWidth));
        coordsChromaTL.push_back(CreateFDiv(pSubJ, pHeight));
        pSampleInfo->pImageDesc = xyChromaInfo.pImageDesc1;
        Value* pTLb = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaTL, pSampleInfo));

        pSampleInfo->pImageDesc = xyChromaInfo.pImageDesc2;
        Value* pTLr = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaTL, pSampleInfo));
        Value* pTL = CreateShuffleVector(pTLr, pTLb, { 0, 6});

        // Sample TR
        coordsChromaTR.push_back(CreateFDiv(CreateFAdd(pSubI, ConstantFP::get(getFloatTy(), 1.0)), pWidth));
        coordsChromaTR.push_back(CreateFDiv(pSubJ, pHeight));
        pSampleInfo->pImageDesc = xyChromaInfo.pImageDesc1;
        Value* pTRb = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaTR, pSampleInfo));

        pSampleInfo->pImageDesc = xyChromaInfo.pImageDesc2;
        Value* pTRr = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaTR, pSampleInfo));
        Value* pTR = CreateShuffleVector(pTRr, pTRb, { 0, 6});

        // Sample BL
        coordsChromaBL.push_back(CreateFDiv(pSubI, pWidth));
        coordsChromaBL.push_back(CreateFDiv(CreateFAdd(pSubJ, ConstantFP::get(getFloatTy(), 1.0)), pHeight));
        pSampleInfo->pImageDesc = xyChromaInfo.pImageDesc1;
        Value* pBLb = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaBL, pSampleInfo));
        pSampleInfo->pImageDesc = xyChromaInfo.pImageDesc2;
        Value* pBLr = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaBL, pSampleInfo));
        Value* pBL = CreateShuffleVector(pBLr, pBLb, { 0, 6});

        // Sample BR
        coordsChromaBR.push_back(CreateFDiv(CreateFAdd(pSubI, ConstantFP::get(getFloatTy(), 1.0)), pWidth));
        coordsChromaBR.push_back(CreateFDiv(CreateFAdd(pSubJ, ConstantFP::get(getFloatTy(), 1.0)), pHeight));
        pSampleInfo->pImageDesc = xyChromaInfo.pImageDesc1;
        Value* pBRb = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaBR, pSampleInfo));
        pSampleInfo->pImageDesc = xyChromaInfo.pImageDesc2;
        Value* pBRr = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsChromaBR, pSampleInfo));
        Value* pBR = CreateShuffleVector(pBRr, pBRb, { 0, 6});

        // Linear interpolate
        pResult = BilinearBlend(pAlpha, pBeta, pTL, pTR, pBL, pBR);
    }

    return pResult;
}

// =====================================================================================================================
// Create YCbCr image sample internal
Value* BuilderImplImage::YCbCrCreateImageSampleInternal(
    SmallVectorImpl<Value*>& coords,     // ST coords
    YCbCrSampleInfo*         pYCbCrInfo) // [In] YCbCr smaple information
{
    Value* pCoords = CreateInsertElement(Constant::getNullValue(VectorType::get(coords[0]->getType(), 2)),
                                         coords[0],
                                         uint64_t(0));

    pCoords = CreateInsertElement(pCoords, coords[1], uint64_t(1));

    return CreateImageSampleGather(pYCbCrInfo->pResultTy,
                                   pYCbCrInfo->dim,
                                   pYCbCrInfo->flags,
                                   pCoords,
                                   pYCbCrInfo->pImageDesc,
                                   pYCbCrInfo->pSamplerDesc,
                                   pYCbCrInfo->address,
                                   pYCbCrInfo->instName,
                                   pYCbCrInfo->isSample);
}

// =====================================================================================================================
// Replace [beginBit, beginBit + adjustBits) bits with data in specific word
Value* BuilderImplImage::ReplaceBitsInWord(
    Value*      pWord,      // [in] Target wowrd
    uint32_t    beginBit,   // The first bit to be replaced
    uint32_t    adjustBits, // The number of bits should be replaced
    Value*      pData)      // [in] The data used to replace specific bits
{
    uint32_t mask = ((1 << adjustBits) - 1) << beginBit;

    Value* pMask = getInt32(mask);
    Value* pInvMask = getInt32(~mask);
    Value* pBeginBit = getInt32(beginBit);

    if (auto pWordVecTy = dyn_cast<VectorType>(pWord->getType()))
    {
        if (isa<VectorType>(pData->getType()) == false)
        {
            pData = CreateVectorSplat(pWordVecTy->getNumElements(), pData);
        }
        pMask = CreateVectorSplat(pWordVecTy->getNumElements(), pMask);
        pInvMask = CreateVectorSplat(pWordVecTy->getNumElements(), pInvMask);
        pBeginBit = CreateVectorSplat(pWordVecTy->getNumElements(), pBeginBit);
    }
    else
    {
        if (auto pDataVecTy = dyn_cast<VectorType>(pData->getType()))
        {
            pWord = CreateVectorSplat(pDataVecTy->getNumElements(), pWord);
            pMask = CreateVectorSplat(pDataVecTy->getNumElements(), pMask);
            pInvMask = CreateVectorSplat(pDataVecTy->getNumElements(), pInvMask);
            pBeginBit = CreateVectorSplat(pDataVecTy->getNumElements(), pBeginBit);
        }
    }

    Value* pNewWord = CreateAnd(pWord, pInvMask); // (pWord & ~mask)
    pData = CreateAnd(CreateShl(pData, pBeginBit), pMask); // ((pData << beginBit) & mask)
    pNewWord = CreateOr(pNewWord, pData);
    return pNewWord;
}

// =====================================================================================================================
// Generate sampler descriptor for YCbCr conversion
Value* BuilderImplImage::YCbCrGenerateSamplerDesc(
    Value*           pSamplerDesc,                // [in] Sampler descriptor
    SamplerFilter    filter,                      // The type of sampler filter
    bool             forceExplicitReconstruction) // Enable/Disable force explict chroma reconstruction
{
    Value* pSamplerDescNew = UndefValue::get(pSamplerDesc->getType());

    Value* pSampWord0 = CreateExtractElement(pSamplerDesc, getInt64(0));
    Value* pSampWord1 = CreateExtractElement(pSamplerDesc, getInt64(1));
    Value* pSampWord2 = CreateExtractElement(pSamplerDesc, getInt64(2));
    Value* pSampWord3 = CreateExtractElement(pSamplerDesc, getInt64(3));

    /// Determines if "TexFilter" should be ignored or not.
    // enum class TexFilterMode : uint32
    // {
    //     Blend = 0x0, ///< Use the filter method specified by the TexFilter enumeration
    //     Min   = 0x1, ///< Use the minimum value returned by the sampler, no blending op occurs
    //     Max   = 0x2, ///< Use the maximum value returned by the sampler, no blending op occurs
    // };
    pSampWord0 = ReplaceBitsInWord(pSampWord0, 30, 2, getInt32(0b00)); // Force use blend mode

    /// Enumeration which defines the mode for magnification and minification sampling
    // enum XyFilter : uint32
    // {
    //     XyFilterPoint = 0,          ///< Use single point sampling
    //     XyFilterLinear,             ///< Use linear sampling
    //     XyFilterAnisotropicPoint,   ///< Use anisotropic with single point sampling
    //     XyFilterAnisotropicLinear,  ///< Use anisotropic with linear sampling
    //     XyFilterCount
    // };
    if ((filter == SamplerFilter::Nearest) || forceExplicitReconstruction)
    {
        pSampWord2 = ReplaceBitsInWord(pSampWord2, 20, 4, getInt32(0b0000));
    }
    else //filter == SamplerFilter::Linear
    {
        pSampWord2 = ReplaceBitsInWord(pSampWord2, 20, 4, getInt32(0b0101));
    }

    pSamplerDescNew = CreateInsertElement(pSamplerDescNew, pSampWord0, getInt64(0));
    pSamplerDescNew = CreateInsertElement(pSamplerDescNew, pSampWord1, getInt64(1));
    pSamplerDescNew = CreateInsertElement(pSamplerDescNew, pSampWord2, getInt64(2));
    pSamplerDescNew = CreateInsertElement(pSamplerDescNew, pSampWord3, getInt64(3));

    return pSamplerDescNew;
}

// =====================================================================================================================
// Implement range expanding operation on checking whether the encoding uses full numerical range on luma channel
Value* BuilderImplImage::YCbCrRangeExpand(
    SamplerYCbCrRange    range,   // Specifies whether the encoding uses the full numerical range
    const uint32_t*      pBits,   // Channel bits
    Value*               pSample) // [in] Sample results which need range expansion, assume in sequence => Cr, Y, Cb
{
    switch (range)
    {
    case SamplerYCbCrRange::ItuFull:
    {
        //              [2^(n - 1)/((2^n) - 1)]
        // pConvVec1 =  [         0.0         ]
        //              [2^(n - 1)/((2^n) - 1)]
        float row0Num = static_cast<float>(0x1u << (pBits[0] - 0x1u)) / ((0x1u << pBits[0]) - 1u);
        float row2Num = static_cast<float>(0x1u << (pBits[2] - 0x1u)) / ((0x1u << pBits[2]) - 1u);

        Value* pConvVec1 = UndefValue::get(VectorType::get(getFloatTy(), 3));
        pConvVec1 = CreateInsertElement(pConvVec1, ConstantFP::get(getFloatTy(), row0Num), uint64_t(0));
        pConvVec1 = CreateInsertElement(pConvVec1, ConstantFP::get(getFloatTy(), 0.0f), uint64_t(1));
        pConvVec1 = CreateInsertElement(pConvVec1, ConstantFP::get(getFloatTy(), row2Num), uint64_t(2));

        //          [Cr]   pConvVec1[0]
        // result = [ Y] - pConvVec1[1]
        //          [Cb]   pConvVec1[2]
        return CreateFSub(pSample, pConvVec1);
    }
    case SamplerYCbCrRange::ItuNarrow:
    {
        //             [(2^n - 1)/(224 x (2^(n - 8))]
        // pConvVec1 = [(2^n - 1)/(219 x (2^(n - 8))]
        //             [(2^n - 1)/(224 x (2^(n - 8))]
        float row0Num = static_cast<float>((0x1u << pBits[0]) - 1u) / (224u * (0x1u << (pBits[0] - 8)));
        float row1Num = static_cast<float>((0x1u << pBits[1]) - 1u) / (219u * (0x1u << (pBits[1] - 8)));
        float row2Num = static_cast<float>((0x1u << pBits[2]) - 1u) / (224u * (0x1u << (pBits[2] - 8)));

        Value* pConvVec1 = UndefValue::get(VectorType::get(getFloatTy(), 3));
        pConvVec1 = CreateInsertElement(pConvVec1, ConstantFP::get(getFloatTy(), row0Num), uint64_t(0));
        pConvVec1 = CreateInsertElement(pConvVec1, ConstantFP::get(getFloatTy(), row1Num), uint64_t(1));
        pConvVec1 = CreateInsertElement(pConvVec1, ConstantFP::get(getFloatTy(), row2Num), uint64_t(2));

        //             [(128 x (2^(n - 8))/(224 x (2^(n - 8))]
        // pConvVec2 = [( 16 x (2^(n - 8))/(219 x (2^(n - 8))]
        //             [(128 x (2^(n - 8))/(224 x (2^(n - 8))]
        row0Num = static_cast<float>(128u * (0x1u << (pBits[0] - 8))) / (224u * (0x1u << (pBits[0] - 8)));
        row1Num = static_cast<float>( 16u * (0x1u << (pBits[1] - 8))) / (219u * (0x1u << (pBits[1] - 8)));
        row2Num = static_cast<float>(128u * (0x1u << (pBits[2] - 8))) / (224u * (0x1u << (pBits[2] - 8)));

        Value* pConvVec2 = UndefValue::get(VectorType::get(getFloatTy(), 3));
        pConvVec2 = CreateInsertElement(pConvVec2, ConstantFP::get(getFloatTy(), row0Num), uint64_t(0));
        pConvVec2 = CreateInsertElement(pConvVec2, ConstantFP::get(getFloatTy(), row1Num), uint64_t(1));
        pConvVec2 = CreateInsertElement(pConvVec2, ConstantFP::get(getFloatTy(), row2Num), uint64_t(2));

        //          pConvVec1[0]   [Cr]   pConvVec2[0]
        // result = pConvVec1[1] * [ Y] - pConvVec2[1]
        //          pConvVec1[2]   [Cb]   pConvVec2[2]
        return CreateFSub(CreateFMul(pSample, pConvVec1), pConvVec2);
    }

    default:
        LLPC_NEVER_CALLED();
        return nullptr;
    }
}

// =====================================================================================================================
// Implement the color transfer operation for conversion from  YCbCr to RGB color model
Value* BuilderImplImage::YCbCrConvertColor(
    Type*                          pResultTy,  // [in] Result type, assumed in <4 x f32>
    SamplerYCbCrModelConversion    colorModel, // The color conversion model
    SamplerYCbCrRange              range,      // Specifies whether the encoding uses the full numerical range
    uint32_t*                      pBits,      // Channel bits
    Value*                         pImageOp)   // [in] Results which need color conversion, in sequence => Cr, Y, Cb
{
    Value* pSubImage = CreateShuffleVector(pImageOp, pImageOp, { 0, 1, 2 });

    Value* pMinVec = UndefValue::get(VectorType::get(getFloatTy(), 3));
    pMinVec = CreateInsertElement(pMinVec, ConstantFP::get(getFloatTy(), -0.5), uint64_t(0));
    pMinVec = CreateInsertElement(pMinVec, ConstantFP::get(getFloatTy(),  0.0), uint64_t(1));
    pMinVec = CreateInsertElement(pMinVec, ConstantFP::get(getFloatTy(), -0.5), uint64_t(2));

    Value* pMaxVec = UndefValue::get(VectorType::get(getFloatTy(), 3));
    pMaxVec = CreateInsertElement(pMaxVec, ConstantFP::get(getFloatTy(), 0.5), uint64_t(0));
    pMaxVec = CreateInsertElement(pMaxVec, ConstantFP::get(getFloatTy(), 1.0), uint64_t(1));
    pMaxVec = CreateInsertElement(pMaxVec, ConstantFP::get(getFloatTy(), 0.5), uint64_t(2));

    Value* pResult = UndefValue::get(pResultTy);

    switch (colorModel)
    {
    case SamplerYCbCrModelConversion::RgbIdentity:
    {
        //pResult[Cr] = C'_rgba [R]
        //pResult[Y]  = C'_rgba [G]
        //pResult[Cb] = C'_rgba [B]
        //pResult[a]  = C'_rgba [A]
        pResult = pImageOp;
        break;
    }
    case SamplerYCbCrModelConversion::YCbCrIdentity:
    {
        // pResult = RangeExpaned(C'_rgba)
        pSubImage = CreateFClamp(YCbCrRangeExpand(range, pBits, pSubImage), pMinVec, pMaxVec);
        Value* pOutputR = CreateExtractElement(pSubImage, getInt64(0));
        Value* pOutputG = CreateExtractElement(pSubImage, getInt64(1));
        Value* pOutputB = CreateExtractElement(pSubImage, getInt64(2));
        Value* pOutputA = CreateExtractElement(pImageOp, getInt64(3));

        pResult = CreateInsertElement(pResult, pOutputR, getInt64(0));
        pResult = CreateInsertElement(pResult, pOutputG, getInt64(1));
        pResult = CreateInsertElement(pResult, pOutputB, getInt64(2));
        pResult = CreateInsertElement(pResult, pOutputA, getInt64(3));
        break;
    }
    case SamplerYCbCrModelConversion::YCbCr601:
    case SamplerYCbCrModelConversion::YCbCr709:
    case SamplerYCbCrModelConversion::YCbCr2020:
    {
        // pInputVec = RangeExpaned(C'_rgba)
        Value* pInputVec = CreateFClamp(YCbCrRangeExpand(range, pBits, pSubImage), pMinVec, pMaxVec);

        Value* pRow0 = UndefValue::get(VectorType::get(getFloatTy(), 3));
        Value* pRow1 = UndefValue::get(VectorType::get(getFloatTy(), 3));
        Value* pRow2 = UndefValue::get(VectorType::get(getFloatTy(), 3));

        if (colorModel == SamplerYCbCrModelConversion::YCbCr601)
        {
            //           [            1.402f,   1.0f,               0.0f]
            // convMat = [-0.419198 / 0.587f,   1.0f, -0.202008 / 0.587f]
            //           [              0.0f,   1.0f,             1.772f]

            pRow0 = CreateInsertElement(pRow0, ConstantFP::get(getFloatTy(), 1.402f), uint64_t(0));
            pRow0 = CreateInsertElement(pRow0, ConstantFP::get(getFloatTy(),   1.0f), uint64_t(1));
            pRow0 = CreateInsertElement(pRow0, ConstantFP::get(getFloatTy(),   0.0f), uint64_t(2));

            float row1Col0 = static_cast<float>(-0.419198 / 0.587);
            float row1Col2 = static_cast<float>(-0.202008 / 0.587);

            pRow1 = CreateInsertElement(pRow1, ConstantFP::get(getFloatTy(), row1Col0), uint64_t(0));
            pRow1 = CreateInsertElement(pRow1, ConstantFP::get(getFloatTy(),     1.0f), uint64_t(1));
            pRow1 = CreateInsertElement(pRow1, ConstantFP::get(getFloatTy(), row1Col2), uint64_t(2));

            pRow2 = CreateInsertElement(pRow2, ConstantFP::get(getFloatTy(),   0.0f), uint64_t(0));
            pRow2 = CreateInsertElement(pRow2, ConstantFP::get(getFloatTy(),   1.0f), uint64_t(1));
            pRow2 = CreateInsertElement(pRow2, ConstantFP::get(getFloatTy(), 1.772f), uint64_t(2));
        }
        else if (colorModel == SamplerYCbCrModelConversion::YCbCr709)
        {
            //           [              1.5748f,   1.0f,                  0.0f]
            // convMat = [-0.33480248 / 0.7152f,   1.0f, -0.13397432 / 0.7152f]
            //           [                 0.0f,   1.0f,               1.8556f]

            pRow0 = CreateInsertElement(pRow0, ConstantFP::get(getFloatTy(), 1.5748f), uint64_t(0));
            pRow0 = CreateInsertElement(pRow0, ConstantFP::get(getFloatTy(),    1.0f), uint64_t(1));
            pRow0 = CreateInsertElement(pRow0, ConstantFP::get(getFloatTy(),    0.0f), uint64_t(2));

            float row1Col0 = static_cast<float>(-0.33480248 / 0.7152);
            float row1Col2 = static_cast<float>(-0.13397432 / 0.7152);

            pRow1 = CreateInsertElement(pRow1, ConstantFP::get(getFloatTy(), row1Col0), uint64_t(0));
            pRow1 = CreateInsertElement(pRow1, ConstantFP::get(getFloatTy(),     1.0f), uint64_t(1));
            pRow1 = CreateInsertElement(pRow1, ConstantFP::get(getFloatTy(), row1Col2), uint64_t(2));

            pRow2 = CreateInsertElement(pRow2, ConstantFP::get(getFloatTy(),    0.0f), uint64_t(0));
            pRow2 = CreateInsertElement(pRow2, ConstantFP::get(getFloatTy(),    1.0f), uint64_t(1));
            pRow2 = CreateInsertElement(pRow2, ConstantFP::get(getFloatTy(), 1.8556f), uint64_t(2));
        }
        else
        {
            //           [              1.4746f,   1.0f,                  0.0f]
            // convMat = [-0.38737742 / 0.6780f,   1.0f, -0.11156702 / 0.6780f]
            //           [                 0.0f,   1.0f,               1.8814f]

            pRow0 = CreateInsertElement(pRow0, ConstantFP::get(getFloatTy(), 1.4746f), uint64_t(0));
            pRow0 = CreateInsertElement(pRow0, ConstantFP::get(getFloatTy(),    1.0f), uint64_t(1));
            pRow0 = CreateInsertElement(pRow0, ConstantFP::get(getFloatTy(),    0.0f), uint64_t(2));

            float row1Col0 = static_cast<float>(-0.38737742 / 0.6780);
            float row1Col2 = static_cast<float>(-0.11156702 / 0.6780);

            pRow1 = CreateInsertElement(pRow1, ConstantFP::get(getFloatTy(), row1Col0), uint64_t(0));
            pRow1 = CreateInsertElement(pRow1, ConstantFP::get(getFloatTy(),     1.0f), uint64_t(1));
            pRow1 = CreateInsertElement(pRow1, ConstantFP::get(getFloatTy(), row1Col2), uint64_t(2));

            pRow2 = CreateInsertElement(pRow2, ConstantFP::get(getFloatTy(),    0.0f), uint64_t(0));
            pRow2 = CreateInsertElement(pRow2, ConstantFP::get(getFloatTy(),    1.0f), uint64_t(1));
            pRow2 = CreateInsertElement(pRow2, ConstantFP::get(getFloatTy(), 1.8814f), uint64_t(2));
        }

        // output[R]             [Cr]
        // output[G] = convMat * [ Y]
        // output[B]             [Cb]

        Value* pOutputR = CreateDotProduct(pRow0, pInputVec);
        Value* pOutputG = CreateDotProduct(pRow1, pInputVec);
        Value* pOutputB = CreateDotProduct(pRow2, pInputVec);
        Value* pOutputA = CreateExtractElement(pImageOp, getInt64(3));

        pResult = CreateInsertElement(pResult, pOutputR, getInt64(0));
        pResult = CreateInsertElement(pResult, pOutputG, getInt64(1));
        pResult = CreateInsertElement(pResult, pOutputB, getInt64(2));
        pResult = CreateInsertElement(pResult, pOutputA, getInt64(3));
        break;
    }

    default:
        LLPC_NEVER_CALLED();
        break;
    }

    if (colorModel != SamplerYCbCrModelConversion::YCbCrIdentity)
    {
        pResult = CreateFClamp(pResult,
                               CreateVectorSplat(4, ConstantFP::get(getFloatTy(), 0.0)),
                               CreateVectorSplat(4, ConstantFP::get(getFloatTy(), 1.0)));
    }

    return pResult;
}

// =====================================================================================================================
// Implement transfer form  ST coordinates to UV coordiantes operation
Value* BuilderImplImage::TransferSTtoUVCoords(
    Value* pST,   // [in] ST coords
    Value* pSize) // [in] with/height
{
    return CreateFMul(pST, pSize);
}

// =====================================================================================================================
// Implement the adjustment of UV coordinates when the sample location associated with
// downsampled chroma channels in the X/XY dimension occurs
Value* BuilderImplImage::YCbCrCalculateImplicitChromaUV(
    ChromaLocation offset, // The sample location associated with downsampled chroma channels in X dimension
    Value*         pUV)    // [in] UV coordinates
{
    if (offset == ChromaLocation::CositedEven)
    {
        pUV = CreateFAdd(pUV, ConstantFP::get(getFloatTy(), 0.5f));
    }

    return CreateFMul(pUV, ConstantFP::get(getFloatTy(), 0.5f));
}

// =====================================================================================================================
// Transfer IJ coordinates from UV coordinates
Value* BuilderImplImage::TransferUVtoIJCoords(
    SamplerFilter filter, // Nearest or Linear sampler filter
    Value*        pUV)    // [in] UV coordinates
{
    LLPC_ASSERT((filter == SamplerFilter::Nearest) || (filter == SamplerFilter::Linear));

    if (filter == SamplerFilter::Linear)
    {
        pUV = CreateFSub(pUV, ConstantFP::get(getFloatTy(), 0.5f));
    }

    return CreateUnaryIntrinsic(Intrinsic::floor, pUV);
}

// =====================================================================================================================
// Calculate UV offset to top-left pixel
Value* BuilderImplImage::CalculateUVoffset(
    Value* pUV) // [in] UV coordinates
{
    Value* pUVBaised = CreateFSub(pUV, ConstantFP::get(getFloatTy(), 0.5f));
    Value* pIJ = CreateUnaryIntrinsic(Intrinsic::floor, pUVBaised);
    return CreateFSub(pUVBaised, pIJ);
}

// =====================================================================================================================
// Implement bilinear blending
Value* BuilderImplImage::BilinearBlend(
    Value*   pAlpha, // [In] Horizen weight
    Value*   pBeta,  // [In] Vertical weight
    Value*   pTL,    // [In] Top-left pixel
    Value*   pTR,    // [In] Top-right pixel
    Value*   pBL,    // [In] Bottom-left pixel
    Value*   pBR)    // [In] Bottm-right pixel
{
    Value* pTop = CreateFMix(pTL, pTR, pAlpha);
    Value* pBot = CreateFMix(pBL, pBR, pAlpha);

    return CreateFMix(pTop, pBot, pBeta);
}

// =====================================================================================================================
// Create an image YCbCr sampler.
// The caller supplies all arguments to the image sample op in "address", in the order specified
// by the indices defined as ImageIndex* below.
Value* BuilderImplImage::CreateImageYCbCrSample(
    Type*                       pResultTy,      // [in] Result type
    uint32_t                    dim,            // Image dimension
    uint32_t                    flags,          // ImageFlag* flags
    Value*                      pCoord,         // [in] Coordinates (the one in address is ignored in favor of this one)
    Value*                      pImageDesc,     // [in] Image descriptor
    Value*                      pSamplerDesc,   // [in] Sampler descriptor
    ArrayRef<Value*>            address,        // Address and other arguments
    const Twine&                instName,       // [in] Name to give instruction(s)
    bool                        isSample)       // Is sample rather than gather
{
    Value* pResult = nullptr;

    // Extract YCbCr meta data
    SamplerYCbCrConversionMetaData yCbCrMetaData = {};
    yCbCrMetaData.word0.u32All = dyn_cast<ConstantInt>(CreateExtractElement(pSamplerDesc, getInt64(4)))->getZExtValue();
    yCbCrMetaData.word1.u32All = dyn_cast<ConstantInt>(CreateExtractElement(pSamplerDesc, getInt64(5)))->getZExtValue();
    yCbCrMetaData.word2.u32All = dyn_cast<ConstantInt>(CreateExtractElement(pSamplerDesc, getInt64(6)))->getZExtValue();
    yCbCrMetaData.word3.u32All = dyn_cast<ConstantInt>(CreateExtractElement(pSamplerDesc, getInt64(7)))->getZExtValue();

    // Only the first 4 DWORDs are sampler descriptor, we need to extract these values under any condition
    pSamplerDesc = CreateShuffleVector(pSamplerDesc, pSamplerDesc, { 0, 1, 2, 3 });

    // Init YCbCr meta data
    uint32_t bits[3] = {yCbCrMetaData.word0.bitDepth.channelBitsR,
                        yCbCrMetaData.word0.bitDepth.channelBitsG,
                        yCbCrMetaData.word0.bitDepth.channelBitsB};

    const int  planeNum                          = yCbCrMetaData.word1.planes;
    const bool subsampledX                       = yCbCrMetaData.word1.xSubSampled;
    const bool subsampledY                       = yCbCrMetaData.word1.ySubSampled;
    const bool forceExplicitReconstruct          = yCbCrMetaData.word0.forceExplicitReconstruct;
    const uint32_t xBitCount                     = yCbCrMetaData.word2.bitCounts.xBitCount;
    const ComponentSwizzle swizzleR              = yCbCrMetaData.word0.componentMapping.swizzleR;
    const ComponentSwizzle swizzleG              = yCbCrMetaData.word0.componentMapping.swizzleG;
    const ComponentSwizzle swizzleB              = yCbCrMetaData.word0.componentMapping.swizzleB;
    const ComponentSwizzle swizzleA              = yCbCrMetaData.word0.componentMapping.swizzleA;
    const SamplerFilter lumaFilter               = static_cast<SamplerFilter>(yCbCrMetaData.word1.lumaFilter);
    const SamplerFilter chromaFilter             = static_cast<SamplerFilter>(yCbCrMetaData.word1.chromaFilter);
    const ChromaLocation xChromaOffset           = static_cast<ChromaLocation>(yCbCrMetaData.word1.xChromaOffset);
    const ChromaLocation yChromaOffset           = static_cast<ChromaLocation>(yCbCrMetaData.word1.yChromaOffset);
    const SamplerYCbCrRange yCbCrRange           = static_cast<SamplerYCbCrRange>(yCbCrMetaData.word0.yCbCrRange);
    const SamplerYCbCrModelConversion yCbCrModel = static_cast<SamplerYCbCrModelConversion>(yCbCrMetaData.word0.yCbCrModel);

    // Init sample descriptor for luma channel
    Value* pSamplerDescLuma   = pSamplerDesc;

    // Init sample descriptor for chroma channels
    Value* pSamplerDescChroma = YCbCrGenerateSamplerDesc(pSamplerDesc, chromaFilter, forceExplicitReconstruct);

    // Extract SQ_IMG_RSRC_WORD
    Value* pWord0 = CreateExtractElement(pImageDesc, getInt64(0));
    Value* pWord1 = CreateExtractElement(pImageDesc, getInt64(1));
    Value* pWord2 = CreateExtractElement(pImageDesc, getInt64(2));
    Value* pWord3 = CreateExtractElement(pImageDesc, getInt64(3));
    Value* pWord4 = CreateExtractElement(pImageDesc, getInt64(4));
    Value* pWord5 = CreateExtractElement(pImageDesc, getInt64(5));
    Value* pWord6 = CreateExtractElement(pImageDesc, getInt64(6));
    Value* pWord7 = CreateExtractElement(pImageDesc, getInt64(7));

    // Generate pWord1
    // For 2 plane, the SQ_IMG_RSRC pWord1 uses data which is calculated outside compiler
    Value* pWord1CbCr = getInt32(yCbCrMetaData.word3.sqImgRsrcWord1);

    // Generate pWord2
    Value* pWord2BitsWidth  = CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                              getInt32Ty(),
                                              {pWord2, getInt32(0), getInt32(14) });
    Value* pWord2BitsHeight = CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                              getInt32Ty(),
                                              {pWord2, getInt32(14), getInt32(14) });

    // Generate Vec2, contains pWidth-1 , pHeight-1
    Value* pWord2BitsWidthHeightM1 = UndefValue::get(VectorType::get(getInt32Ty(), 2));
    pWord2BitsWidthHeightM1 = CreateInsertElement(pWord2BitsWidthHeightM1, pWord2BitsWidth , getInt64(0));
    pWord2BitsWidthHeightM1 = CreateInsertElement(pWord2BitsWidthHeightM1, pWord2BitsHeight, getInt64(1));

    // pVec2One = [1]
    //            [1]
    Value* pVec2One = CreateVectorSplat(2, ConstantInt::get(getInt32Ty(), 1));

    // pWord2BitsWidthHeight = [pWidth -1]   + [1]
    //                         [pHeight-1]     [1]
    Value* pWord2BitsWidthHeight = CreateAdd(pWord2BitsWidthHeightM1, pVec2One);

    // pWord2BitsWidthHeightRs1M1 = [pWidth  >> 1] - [1]
    //                              [pHeight >> 1]   [1]
    Value* pWord2BitsWidthHeightRs1M1 = CreateSub(CreateLShr(pWord2BitsWidthHeight, pVec2One), pVec2One);

    // pWord2BitsWidthY = pWidth
    // pWord2BitsHeightY = pHeight
    Value* pWord2BitsWidthY     = CreateExtractElement(pWord2BitsWidthHeight, getInt32(0));
    Value* pWord2BitsHeightY    = CreateExtractElement(pWord2BitsWidthHeight, getInt32(1));

    // pWord2BitsWidthCbCr = pWidth >> 1 - 1
    // pWord2BitsHeightCbCr = pHeight - 1
    Value* pWord2BitsWidthCbCr  = CreateExtractElement(pWord2BitsWidthHeightRs1M1, getInt32(0));
    Value* pWord2BitsHeightCbCr = pWord2BitsHeight;

    // pWord2BitsWidthCb = pWidth >> 1 - 1
    // pWord2BitsHeightCb = pHeight >> 1 - 1
    Value* pWord2BitsWidthCb    = CreateExtractElement(pWord2BitsWidthHeightRs1M1, getInt32(0));
    Value* pWord2BitsHeightCb   = CreateExtractElement(pWord2BitsWidthHeightRs1M1, getInt32(1));

    // pWord2BitsWidthCr = pWidth >> 1 - 1
    // pWord2BitsHeightCr = pHeight >> 1 - 1
    Value* pWord2BitsWidthCr    = CreateExtractElement(pWord2BitsWidthHeightRs1M1, getInt32(0));
    Value* pWord2BitsHeightCr   = CreateExtractElement(pWord2BitsWidthHeightRs1M1, getInt32(1));

    //                   [pWord2BitsWidthCb]
    // pWord2WidthVec3 = [pWord2BitsWidthCr]
    //                   [pWord2BitsWidthCbCr]
    Value* pWord2WidthVec3 = UndefValue::get(VectorType::get(getInt32Ty(), 3));
    pWord2WidthVec3 = CreateInsertElement(pWord2WidthVec3, pWord2BitsWidthCb  , getInt64(0));
    pWord2WidthVec3 = CreateInsertElement(pWord2WidthVec3, pWord2BitsWidthCr  , getInt64(1));
    pWord2WidthVec3 = CreateInsertElement(pWord2WidthVec3, pWord2BitsWidthCbCr, getInt64(2));

    //                    [pWord2BitsHeightCb]
    // pWord2HeightVec3 = [pWord2BitsHeightCr]
    //                    [pWord2BitsHeightCbCr]
    Value* pWord2HeightVec3 = UndefValue::get(VectorType::get(getInt32Ty(), 3));
    pWord2HeightVec3 = CreateInsertElement(pWord2HeightVec3, pWord2BitsHeightCb  , getInt64(0));
    pWord2HeightVec3 = CreateInsertElement(pWord2HeightVec3, pWord2BitsHeightCr  , getInt64(1));
    pWord2HeightVec3 = CreateInsertElement(pWord2HeightVec3, pWord2BitsHeightCbCr, getInt64(2));

    // Replace [0:14) bits of pWord2 with pWord2WidthVec3
    Value* pWord2Vec3 = ReplaceBitsInWord(pWord2, 0, 14, pWord2WidthVec3);
    // Replace [14:28) bits of pWord2Vec3 with pWord2HeightVec3
    pWord2Vec3 = ReplaceBitsInWord(pWord2Vec3, 14, 14, pWord2HeightVec3);

    Value* pWord2Cb   = CreateExtractElement(pWord2Vec3, getInt32(0));
    Value* pWord2Cr   = CreateExtractElement(pWord2Vec3, getInt32(1));
    Value* pWord2CbCr = CreateExtractElement(pWord2Vec3, getInt32(2));

    // Generate pWord3

    //                        [     0x300]
    // pWord3DstSelXYZWVec3 = [     0x204]
    //                        [dstSelXYZW]
    Value* pWord3DstSelXYZWVec3 = UndefValue::get(VectorType::get(getInt32Ty(), 3));
    pWord3DstSelXYZWVec3 = CreateInsertElement(pWord3DstSelXYZWVec3, getInt32(0x300), getInt64(0));
    pWord3DstSelXYZWVec3 = CreateInsertElement(pWord3DstSelXYZWVec3, getInt32(0x204), getInt64(1));
    pWord3DstSelXYZWVec3 = CreateInsertElement(pWord3DstSelXYZWVec3,
                                              getInt32(yCbCrMetaData.word1.dstSelXYZW),
                                              getInt64(2));

    // Replace [0:12) bits of pWord3 with pWord3DstSelXYZWVec3
    Value* pWord3Vec3 = ReplaceBitsInWord(pWord3, 0, 12, pWord3DstSelXYZWVec3);

    Value* pWord3Cb   = CreateExtractElement(pWord3Vec3, getInt32(0));
    Value* pWord3Cr   = CreateExtractElement(pWord3Vec3, getInt32(1));
    Value* pWord3CbCr = CreateExtractElement(pWord3Vec3, getInt32(2));

    // Generate pWord4
    Value* pWord4BitsDepth = CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                             getInt32Ty(),
                                             {pWord4, getInt32(0), getInt32(13) });
    Value* pWord4BitsPitchYM1 = CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                             getInt32Ty(),
                                             {pWord4, getInt32(13), getInt32(12) });
    Value* pWord4BitsPitchY  = CreateAdd(pWord4BitsPitchYM1, getInt32(1));
    Value* pWord4BitsPitchCb = CreateSub(CreateLShr(pWord4BitsPitchY, getInt32(1)), getInt32(1));

    Value* pWord4Vec3 = pWord4BitsDepth;

    // Replace [13:29) bits of pWord4Vec3 with pWord4BitsPitchCb
    pWord4Vec3 = ReplaceBitsInWord(pWord4Vec3, 13, 16, pWord4BitsPitchCb);

    //                  [4]
    // pWord4LastBits = [5]
    //                  [6]
    Value* pWord4LastBits = UndefValue::get(VectorType::get(getInt32Ty(), 3));
    pWord4LastBits = CreateInsertElement(pWord4LastBits, getInt32(4), getInt64(0));
    pWord4LastBits = CreateInsertElement(pWord4LastBits, getInt32(5), getInt64(1));
    pWord4LastBits = CreateInsertElement(pWord4LastBits, getInt32(6), getInt64(2));

    // Replace [29:32) bits of pWord4Vec3 with pWord4LastBits
    pWord4Vec3 = ReplaceBitsInWord(pWord4Vec3, 29, 3, pWord4LastBits);
    Value* pWord4Cb   = CreateExtractElement(pWord4Vec3, getInt32(0));
    Value* pWord4Cr   = CreateExtractElement(pWord4Vec3, getInt32(1));
    Value* pWord4CbCr = CreateExtractElement(pWord4Vec3, getInt32(2));

    // If the image could be tile optimal, then we need to check whehter it is tiling
    // optimal by reading from srd data dynmically
    Value* pIsTileOpt = nullptr;
    Value* pWord4BitsPitchYOpt = nullptr;
    if (yCbCrMetaData.word1.tileOptimal)
    {
        pIsTileOpt = CreateICmpNE(CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                  getInt32Ty(),
                                                  {pWord3, getInt32(20), getInt32(5) }),
                                  getInt32(0));
        pWord4BitsPitchYOpt = CreateMul(pWord4BitsPitchY, CreateLShr(getInt32(bits[0]), 3));
    }

    // pWord4BitsPitchY * (xBitCount >> 3)
    pWord4BitsPitchY = CreateMul(pWord4BitsPitchY, CreateLShr(getInt32(xBitCount), 3));

    if (yCbCrMetaData.word1.tileOptimal)
    {
        // pWord4BitsPitchY = pIsTileOpt ? (pWord4BitsPitchYOpt << 5) : pWord4BitsPitchY
        pWord4BitsPitchY = CreateSelect(pIsTileOpt,
                                       CreateShl(pWord4BitsPitchYOpt, getInt32(5)),
                                       pWord4BitsPitchY);
    }

    // we should turn this size to 256bytes and add to the word0_bits_BASE_ADDRESS
    // pWord0Cb = pWord0 + (pWord4BitsPitchY * pWord2BitsHeightY) >> 8
    Value* pWord0Cb = CreateAdd(pWord0, CreateLShr(CreateMul(pWord4BitsPitchY, pWord2BitsHeightY), getInt32(8)));

    pWord4BitsPitchCb = CreateAdd(pWord4BitsPitchCb, getInt32(1));

    Value* pWord4BitsPitchCbOpt = nullptr;
    // pWord4BitsPitchCb = pWord4BitsPitchCb * (xBitCount >> 3)
    pWord4BitsPitchCb = CreateMul(pWord4BitsPitchCb, CreateLShr(getInt32(xBitCount), 3));

    if (yCbCrMetaData.word1.tileOptimal)
    {
        // pWord4BitsPitchCbOpt = pWord4BitsPitchCb * (bits[0] >> 3)
        pWord4BitsPitchCbOpt = CreateMul(pWord4BitsPitchCb, CreateLShr(getInt32(bits[0]), 3));

        // pWord4BitsPitchCb = pIsTileOpt ? (pWord4BitsPitchCbOpt << 5) : pWord4BitsPitchCb
        pWord4BitsPitchCb = CreateSelect(pIsTileOpt,
                                        CreateShl(pWord4BitsPitchCbOpt, getInt32(5)),
                                        pWord4BitsPitchCb);
    }

    pWord2BitsHeightCb = CreateAdd(pWord2BitsHeightCb, getInt32(1));

    // we should turn this size to 256bytes and add to the BASE_ADDRESS
    // pWord0Cr = pWord0Cb + (pWord4BitsPitchCb * pWord2BitsHeightCb) >> 8
    Value* pWord0Cr = CreateAdd(pWord0Cb,
                               CreateLShr(CreateMul(pWord4BitsPitchCb, pWord2BitsHeightCb), getInt32(8)));

    Value* pImageDesc1 = nullptr;
    Value* pImageDesc2 = nullptr;

    // We now have all the words for Cb && Cr ready, then create new ImageDesc for Cb && Cr
    if (planeNum == 1)
    {
        pImageDesc1 = UndefValue::get(pImageDesc->getType());
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord0,      getInt64(0));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord1CbCr, getInt64(1));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord2CbCr, getInt64(2));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord3CbCr, getInt64(3));
        pWord4BitsPitchCb = CreateSub(pWord4BitsPitchCb, getInt32(1));
        Value* pWord4CbCr = ReplaceBitsInWord(pWord4, 13, 16, pWord4BitsPitchCb);
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord4CbCr, getInt64(4));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord5,      getInt64(5));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord6,      getInt64(6));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord7,      getInt64(7));
    }
    else if (planeNum == 2)
    {
        pImageDesc1 = UndefValue::get(pImageDesc->getType());
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord0Cb,   getInt64(0));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord1CbCr, getInt64(1));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord2Cb,   getInt64(2));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord3CbCr, getInt64(3));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord4CbCr, getInt64(4));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord5,      getInt64(5));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord6,      getInt64(6));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord7,      getInt64(7));
    }
    else if (planeNum == 3)
    {
        pImageDesc1 = UndefValue::get(pImageDesc->getType());
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord0Cb, getInt64(0));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord1,    getInt64(1));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord2Cb, getInt64(2));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord3Cb, getInt64(3));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord4Cb, getInt64(4));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord5,    getInt64(5));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord6,    getInt64(6));
        pImageDesc1 = CreateInsertElement(pImageDesc1, pWord7,    getInt64(7));

        pImageDesc2 = UndefValue::get(pImageDesc->getType());
        pImageDesc2 = CreateInsertElement(pImageDesc2, pWord0Cr, getInt64(0));
        pImageDesc2 = CreateInsertElement(pImageDesc2, pWord1,    getInt64(1));
        pImageDesc2 = CreateInsertElement(pImageDesc2, pWord2Cr, getInt64(2));
        pImageDesc2 = CreateInsertElement(pImageDesc2, pWord3Cr, getInt64(3));
        pImageDesc2 = CreateInsertElement(pImageDesc2, pWord4Cr, getInt64(4));
        pImageDesc2 = CreateInsertElement(pImageDesc2, pWord5,    getInt64(5));
        pImageDesc2 = CreateInsertElement(pImageDesc2, pWord6,    getInt64(6));
        pImageDesc2 = CreateInsertElement(pImageDesc2, pWord7,    getInt64(7));
    }
    else
    {
        LLPC_NEVER_CALLED();
    }

    // Prepare the coordinate and derivatives, which might also change the dimension.
    SmallVector<Value*, 4> coords;
    SmallVector<Value*, 6> derivatives;

    Value* pProjective = address[ImageAddressIdxProjective];
    if (pProjective != nullptr)
    {
        pProjective = CreateFDiv(ConstantFP::get(pProjective->getType(), 1.0), pProjective);
    }

    dim = PrepareCoordinate(dim,
                            pCoord,
                            pProjective,
                            address[ImageAddressIdxDerivativeX],
                            address[ImageAddressIdxDerivativeY],
                            coords,
                            derivatives);

     // transfer from int to float
    Value* pWidth  = CreateUIToFP(pWord2BitsWidthY,  getFloatTy());
    Value* pHeight = CreateUIToFP(pWord2BitsHeightY, getFloatTy());

    //( pS,pT,r,q,a) to (pU,pV,w,a) Transformation
    Value* pS = coords[0];
    Value* pT = coords[1];
    Value* pU = TransferSTtoUVCoords(pS, pWidth);
    Value* pV = TransferSTtoUVCoords(pT, pHeight);
    Value* pI = TransferUVtoIJCoords(lumaFilter, pU);
    Value* pJ = TransferUVtoIJCoords(lumaFilter, pV);

    SmallVector<Value*, 4> coordsLuma;
    SmallVector<Value*, 4> coordsChroma;

    // pI -> pS
    coordsLuma.push_back(pS);
    // pJ -> pT
    coordsLuma.push_back(pT);

    // Init sample luma info
    YCbCrSampleInfo sampleInfoLuma = {pResultTy, dim, flags, pImageDesc, pSamplerDescLuma, address, instName, isSample};

    // Sample Y and A channels
    Value* pImageOpLuma = static_cast<Instruction*>(YCbCrCreateImageSampleInternal(coordsLuma, &sampleInfoLuma));
    pImageOpLuma = CreateShuffleVector(pImageOpLuma, pImageOpLuma, { 1, 3 });

    // Init sample chroma info
    YCbCrSampleInfo sampleInfo = {pResultTy, dim, flags, pImageDesc, pSamplerDescChroma, address, instName, isSample};

    // Init chroma pWidth and pHeight
    Value* pChromaWidth = CreateFMul(pWidth, ConstantFP::get(getFloatTy(), 0.5f));
    Value* pChromaHeight = CreateFMul(pHeight, ConstantFP::get(getFloatTy(), 0.5f));

    // Init smaple chroma info for downsampled chroma channels in the x dimension
    XChromaSampleInfo xChromaInfo = {};
    xChromaInfo.pYCbCrInfo = &sampleInfo;
    xChromaInfo.pImageDesc1 = pImageDesc1;
    xChromaInfo.pI = pI;
    xChromaInfo.pJ = pJ;
    xChromaInfo.pChromaWidth = pChromaWidth;
    xChromaInfo.pChromaHeight = pHeight;
    xChromaInfo.xChromaOffset = xChromaOffset;

    // Init smaple chroma info for downsampled chroma channels in xy dimension
    XYChromaSampleInfo xyChromaInfo = {};
    xyChromaInfo.pYCbCrInfo = &sampleInfo;
    xyChromaInfo.pImageDesc1 = pImageDesc1;
    xyChromaInfo.pImageDesc2 = pImageDesc2;
    xyChromaInfo.pI = pI;
    xyChromaInfo.pJ = pJ;
    xyChromaInfo.pChromaWidth = pChromaWidth;
    xyChromaInfo.pChromaHeight = pChromaHeight;
    xyChromaInfo.planeNum = planeNum;
    xyChromaInfo.xChromaOffset = xChromaOffset;
    xyChromaInfo.yChromaOffset = yChromaOffset;

    // Init wrapped smaple chroma info
    YCbCrWrappedSampleInfo wrappedSampleInfo = {};
    wrappedSampleInfo.pYCbCrInfo = &sampleInfo;
    wrappedSampleInfo.pChromaWidth = pWidth;
    wrappedSampleInfo.pChromaHeight = pHeight;
    wrappedSampleInfo.pI = pU;
    wrappedSampleInfo.pJ = pV;
    wrappedSampleInfo.pImageDesc1 = pImageDesc;
    wrappedSampleInfo.pImageDesc2 = pImageDesc1;
    wrappedSampleInfo.pImageDesc3 = pImageDesc2;
    wrappedSampleInfo.planeNum = planeNum;
    wrappedSampleInfo.subsampledX = subsampledX;
    wrappedSampleInfo.subsampledY = subsampledY;

    Value* pImageOpChroma = nullptr;

    if (lumaFilter == SamplerFilter::Nearest)
    {
        if (forceExplicitReconstruct || !(subsampledX || subsampledY))
        {
            if ((chromaFilter == SamplerFilter::Nearest) || !subsampledX)
            {
                // Reconstruct using nearest if needed, otherwise, just take what'pS already there.
                wrappedSampleInfo.subsampledX = false;
                wrappedSampleInfo.subsampledY = false;

                pImageOpChroma = YCbCrWrappedSample(wrappedSampleInfo);
            }
            else // SamplerFilter::Linear
            {
                if (subsampledY)
                {
                    pImageOpChroma = YCbCrReconstructLinearXYChromaSample(xyChromaInfo);
                }
                else
                {
                    pImageOpChroma = YCbCrReconstructLinearXChromaSample(xChromaInfo);
                }
            }
        }
        else
        {
            if (subsampledX)
            {
                wrappedSampleInfo.pI = YCbCrCalculateImplicitChromaUV(xChromaOffset, pU);
            }

            if (subsampledY)
            {
                wrappedSampleInfo.pJ = YCbCrCalculateImplicitChromaUV(yChromaOffset, pV);
            }

            pImageOpChroma = YCbCrWrappedSample(wrappedSampleInfo);
        }
    }
    else //lumaFilter == SamplerFilter::Linear
    {
        if (forceExplicitReconstruct || !(subsampledX || subsampledY))
        {
            Value* pLumaA = CalculateUVoffset(pU);
            Value* pLumaB = CalculateUVoffset(pV);
            Value* pSubIPlusOne = CreateFAdd(pI, ConstantFP::get(getFloatTy(), 1.0f));
            Value* pSubJPlusOne = CreateFAdd(pJ, ConstantFP::get(getFloatTy(), 1.0f));

            if ((chromaFilter == SamplerFilter::Nearest) || !subsampledX)
            {
                if (!subsampledX)
                {
                    wrappedSampleInfo.subsampledX = false;
                    wrappedSampleInfo.subsampledY = false;
                    pImageOpChroma = YCbCrWrappedSample(wrappedSampleInfo);
                }
                else
                {
                    Value* pSubI = pI;
                    Value* pSubJ = pJ;
                    if (subsampledX)
                    {
                        pSubI = CreateFDiv(pI, ConstantFP::get(getFloatTy(), 2.0));
                        pSubIPlusOne = CreateFDiv(pSubIPlusOne, ConstantFP::get(getFloatTy(), 2.0));
                    }

                    if (subsampledY)
                    {
                        pSubJ = CreateFDiv(pJ, ConstantFP::get(getFloatTy(), 2.0));
                        pSubJPlusOne = CreateFDiv(pSubJPlusOne, ConstantFP::get(getFloatTy(), 2.0));
                    }

                    wrappedSampleInfo.pI = pSubI;
                    wrappedSampleInfo.pJ = pSubJ;
                    Value* pTL = YCbCrWrappedSample(wrappedSampleInfo);

                    wrappedSampleInfo.pI = pSubIPlusOne;
                    Value* pTR = YCbCrWrappedSample(wrappedSampleInfo);

                    wrappedSampleInfo.pJ = pSubJPlusOne;
                    Value* pBR = YCbCrWrappedSample(wrappedSampleInfo);

                    wrappedSampleInfo.pI = pSubI;
                    Value* pBL = YCbCrWrappedSample(wrappedSampleInfo);

                    pImageOpChroma = BilinearBlend(pLumaA, pLumaB, pTL, pTR, pBL, pBR);
                }
            }
            else // vk::VK_FILTER_LINEAR
            {
                if (subsampledY)
                {
                    // Linear, Reconstructed xy chroma samples with explicit linear filtering
                    Value* pTL = YCbCrReconstructLinearXYChromaSample(xyChromaInfo);

                    xyChromaInfo.pI = pSubIPlusOne;
                    Value* pTR = YCbCrReconstructLinearXYChromaSample(xyChromaInfo);

                    xyChromaInfo.pJ = pSubJPlusOne;
                    Value* pBR = YCbCrReconstructLinearXYChromaSample(xyChromaInfo);

                    xyChromaInfo.pI = pI;
                    Value* pBL = YCbCrReconstructLinearXYChromaSample(xyChromaInfo);

                    pImageOpChroma = BilinearBlend(pLumaA, pLumaB, pTL, pTR, pBL, pBR);
                }
                else
                {
                    // Linear, Reconstructed X chroma samples with explicit linear filtering
                    Value* pTL = YCbCrReconstructLinearXChromaSample(xChromaInfo);

                    xChromaInfo.pI = pSubIPlusOne;
                    Value* pTR = YCbCrReconstructLinearXChromaSample(xChromaInfo);

                    xChromaInfo.pJ = pSubJPlusOne;
                    Value* pBR = YCbCrReconstructLinearXChromaSample(xChromaInfo);

                    xChromaInfo.pI = pI;
                    Value* pBL = YCbCrReconstructLinearXChromaSample(xChromaInfo);

                    pImageOpChroma = BilinearBlend(pLumaA, pLumaB, pTL, pTR, pBL, pBR);
                }
            }
        }
        else
        {
            if (subsampledX)
            {
                wrappedSampleInfo.pI = YCbCrCalculateImplicitChromaUV(xChromaOffset, pU);
            }

            if (subsampledY)
            {
                wrappedSampleInfo.pJ = YCbCrCalculateImplicitChromaUV(yChromaOffset, pV);
            }

            pImageOpChroma = YCbCrWrappedSample(wrappedSampleInfo);
        }
    }

    const uint32_t swizzleRFinal = (swizzleR == ComponentSwizzle::Identity) ? 0 : (swizzleR - ComponentSwizzle::R);
    const uint32_t swizzleGFinal = (swizzleG == ComponentSwizzle::Identity) ? 1 : (swizzleG - ComponentSwizzle::R);
    const uint32_t swizzleBFinal = (swizzleB == ComponentSwizzle::Identity) ? 2 : (swizzleB - ComponentSwizzle::R);
    const uint32_t swizzleAFinal = (swizzleA == ComponentSwizzle::Identity) ? 3 : (swizzleA - ComponentSwizzle::R);

    pResult = CreateShuffleVector(pImageOpLuma, pImageOpChroma, { 2, 0, 3, 1});
    pResult = CreateShuffleVector(pResult,
                                  pResult,
                                  { swizzleRFinal, swizzleGFinal, swizzleBFinal, swizzleAFinal });

    pResult = YCbCrConvertColor(pResultTy,
                                yCbCrModel,
                                yCbCrRange,
                                bits,
                                pResult);

    return static_cast<Instruction*>(pResult);
}

// =====================================================================================================================
// Create an image sample.
// The caller supplies all arguments to the image sample op in "address", in the order specified
// by the indices defined as ImageIndex* below.
Value* BuilderImplImage::CreateImageSample(
    Type*                   pResultTy,          // [in] Result type
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    Value*                  pImageDesc,         // [in] Image descriptor
    Value*                  pSamplerDesc,       // [in] Sampler descriptor
    ArrayRef<Value*>        address,            // Address and other arguments
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    Value* pCoord = address[ImageAddressIdxCoordinate];
    LLPC_ASSERT((pCoord->getType()->getScalarType()->isFloatTy()) ||
                (pCoord->getType()->getScalarType()->isHalfTy()));

    // Check whether the sampler desc contains with constant value
    auto pDescWord0 = dyn_cast<ConstantInt>(CreateExtractElement(pSamplerDesc, getInt32(0)));

    if (pDescWord0 != nullptr)
    {
        // Handle sampler YCbCr conversion
        return CreateImageYCbCrSample(pResultTy,
                                      dim,
                                      flags,
                                      pCoord,
                                      pImageDesc,
                                      pSamplerDesc,
                                      address,
                                      instName,
                                      true);
    }
    else
    {
        // Only the first 4 DWORDs are sampler descriptor, we need to extract these values under any condition
        pSamplerDesc = CreateShuffleVector(pSamplerDesc, pSamplerDesc, { 0, 1, 2, 3 });

        return CreateImageSampleGather(pResultTy,
                                       dim,
                                       flags,
                                       pCoord,
                                       pImageDesc,
                                       pSamplerDesc,
                                       address,
                                       instName,
                                       true);
    }
}

// =====================================================================================================================
// Create an image gather.
// The caller supplies all arguments to the image sample op in "address", in the order specified
// by the indices defined as ImageIndex* below.
Value* BuilderImplImage::CreateImageGather(
    Type*                   pResultTy,          // [in] Result type
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    Value*                  pImageDesc,         // [in] Image descriptor
    Value*                  pSamplerDesc,       // [in] Sampler descriptor
    ArrayRef<Value*>        address,            // Address and other arguments
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    Value* pCoord = address[ImageAddressIdxCoordinate];
    LLPC_ASSERT((pCoord->getType()->getScalarType()->isFloatTy()) ||
                (pCoord->getType()->getScalarType()->isHalfTy()));

    // Check whether we are being asked for integer texel component type.
    Value* pNeedDescPatch = nullptr;
    Type* pTexelTy = pResultTy;
    if (auto pStructResultTy = dyn_cast<StructType>(pResultTy))
    {
        pTexelTy = pStructResultTy->getElementType(0);
    }
    Type* pTexelComponentTy = pTexelTy->getScalarType();
    Type* pGatherTy = pResultTy;

    if (pTexelComponentTy->isIntegerTy())
    {
        // Handle integer texel component type.
        pGatherTy = VectorType::get(getFloatTy(), 4);
        if (pResultTy != pTexelTy)
        {
            pGatherTy = StructType::get(getContext(), { pGatherTy, getInt32Ty() });
        }

        // For integer gather on pre-GFX9, patch descriptor or coordinate.
        pNeedDescPatch = PreprocessIntegerImageGather(dim, pImageDesc, pCoord);
    }

    // Only the first 4 DWORDs are sampler descriptor, we need to extract these values under any condition
    pSamplerDesc = CreateShuffleVector(pSamplerDesc, pSamplerDesc, { 0, 1, 2, 3 });

    Value* pResult = nullptr;
    Value* pAddrOffset = address[ImageAddressIdxOffset];
    if ((pAddrOffset != nullptr) && isa<ArrayType>(pAddrOffset->getType()))
    {
        // We implement a gather with independent offsets (SPIR-V ConstantOffsets) as four separate gathers.
        Value* pResidency = nullptr;
        SmallVector<Value*, ImageAddressCount> modifiedAddress;
        modifiedAddress.insert(modifiedAddress.begin(), address.begin(), address.end());
        auto pGatherStructTy = dyn_cast<StructType>(pGatherTy);
        pResult = UndefValue::get((pGatherStructTy != nullptr) ? pGatherStructTy->getElementType(0) : pGatherTy);
        for (uint32_t index = 0; index < 4; ++index)
        {
            modifiedAddress[ImageAddressIdxOffset] = CreateExtractValue(pAddrOffset, index);
            Value* pSingleResult = CreateImageSampleGather(pGatherTy,
                                                           dim,
                                                           flags,
                                                           pCoord,
                                                           pImageDesc,
                                                           pSamplerDesc,
                                                           modifiedAddress,
                                                           instName,
                                                           false);
            if (pGatherStructTy != nullptr)
            {
                pResidency = CreateExtractValue(pSingleResult, 1);
                pSingleResult = CreateExtractValue(pSingleResult, 0);
            }
            pResult = CreateInsertElement(pResult, CreateExtractElement(pSingleResult, 3), index);
        }
        if (pResidency != nullptr)
        {
            pResult = CreateInsertValue(UndefValue::get(pGatherTy), pResult, 0);
            pResult = CreateInsertValue(pResult, pResidency, 1);
        }
    }
    else
    {
        // No independent offsets. Do the single image gather.
        pResult = CreateImageSampleGather(pGatherTy,
                                          dim,
                                          flags,
                                          pCoord,
                                          pImageDesc,
                                          pSamplerDesc,
                                          address,
                                          instName,
                                          false);
    }

    if (pNeedDescPatch != nullptr)
    {
        // For integer gather on pre-GFX9, post-process the result.
        pResult = PostprocessIntegerImageGather(pNeedDescPatch, flags, pImageDesc, pTexelTy, pResult);
    }

    // Bitcast returned texel from v4f32 to v4i32. (It would be easier to call the gather
    // intrinsic with the right return type, but we do it this way to match the code generated
    // before the image rework.)
    if (isa<StructType>(pResult->getType()))
    {
        // TFE: Need to extract texel from the struct, convert it, and re-insert it.
        Value* pTexel = CreateExtractValue(pResult, 0);
        Value* pTfe = CreateExtractValue(pResult, 1);
        pTexel = cast<Instruction>(CreateBitCast(pTexel, pTexelTy));
        pResult = UndefValue::get(StructType::get(getContext(), { pTexel->getType(), pTfe->getType() }));
        pResult = CreateInsertValue(pResult, pTexel, 0);
        pResult = CreateInsertValue(pResult, pTfe, 1);
    }
    else
    {
        pResult = cast<Instruction>(CreateBitCast(pResult, pTexelTy));
    }

    return pResult;
}

// =====================================================================================================================
// Implement pre-GFX9 integer gather workaround to patch descriptor or coordinate, depending on format in descriptor
// Returns nullptr for GFX9+, or a bool value that is true if the descriptor was patched or false if the
// coordinate was modified.
Value* BuilderImplImage::PreprocessIntegerImageGather(
    uint32_t  dim,        // Image dimension
    Value*&   pImageDesc, // [in/out] Image descriptor
    Value*&   pCoord)     // [in/out] Coordinate
{
    if (GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major >= 9)
    {
        // GFX9+: Workaround not needed.
        return nullptr;
    }

    // Check whether the descriptor needs patching. It does if it does not have format 32, 32_32 or 32_32_32_32.
    Value* pDescDword1 = CreateExtractElement(pImageDesc, 1);
    Value* pDataFormat = CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                         getInt32Ty(),
                                         { pDescDword1, getInt32(20), getInt32(6) });
    Value* pIsDataFormat32 = CreateICmpEQ(pDataFormat, getInt32(IMG_DATA_FORMAT_32));
    Value* pIsDataFormat3232 = CreateICmpEQ(pDataFormat, getInt32(IMG_DATA_FORMAT_32_32));
    Value* pIsDataFormat32323232 = CreateICmpEQ(pDataFormat, getInt32(IMG_DATA_FORMAT_32_32_32_32));
    Value* pCond = CreateOr(pIsDataFormat3232, pIsDataFormat32);
    pCond = CreateOr(pIsDataFormat32323232, pCond);
    Value* pNeedDescPatch = CreateXor(pCond, getInt1(true));

    // Create the if..else..endif, where the condition is whether the descriptor needs patching.
    InsertPoint savedInsertPoint = saveIP();
    BranchInst* pBranch = CreateIf(pNeedDescPatch, true, "before.int.gather");

    // Inside the "then": patch the descriptor: change NUM_FORMAT from SINT to SSCALE.
    Value* pDescDword1A = CreateExtractElement(pImageDesc, 1);
    pDescDword1A = CreateSub(pDescDword1A, getInt32(0x08000000));
    Value* pPatchedImageDesc = CreateInsertElement(pImageDesc, pDescDword1A, 1);

    // On to the "else": patch the coordinates: add (-0.5/width, -0.5/height) to the X,Y coordinates.
    SetInsertPoint(pBranch->getSuccessor(1)->getTerminator());
    Value* pZero = getInt32(0);
    dim = (dim == DimCubeArray) ? DimCube : dim;
    Value* pResInfo = CreateIntrinsic(ImageGetResInfoIntrinsicTable[dim],
                                      { VectorType::get(getFloatTy(), 4), getInt32Ty() },
                                      { getInt32(15), pZero, pImageDesc, pZero, pZero });
    pResInfo = CreateBitCast(pResInfo, VectorType::get(getInt32Ty(), 4));

    Value* pWidthHeight = CreateShuffleVector(pResInfo, pResInfo, { 0, 1 });
    pWidthHeight = CreateSIToFP(pWidthHeight, VectorType::get(getFloatTy(), 2));
    Value* pValueToAdd = CreateFDiv(ConstantFP::get(pWidthHeight->getType(), -0.5), pWidthHeight);
    uint32_t coordCount = pCoord->getType()->getVectorNumElements();
    if (coordCount > 2)
    {
        pValueToAdd = CreateShuffleVector(pValueToAdd,
                                          Constant::getNullValue(pValueToAdd->getType()),
                                          ArrayRef<uint32_t>({ 0, 1, 2, 3 }).slice(0, coordCount));
    }
    Value* pPatchedCoord = CreateFAdd(pCoord, pValueToAdd);

    // Restore insert point to after the if..else..endif, and add the phi nodes.
    restoreIP(savedInsertPoint);
    PHINode* pImageDescPhi = CreatePHI(pImageDesc->getType(), 2);
    pImageDescPhi->addIncoming(pPatchedImageDesc, pBranch->getSuccessor(0));
    pImageDescPhi->addIncoming(pImageDesc, pBranch->getSuccessor(1));
    pImageDesc = pImageDescPhi;

    PHINode* pCoordPhi = CreatePHI(pCoord->getType(), 2);
    pCoordPhi->addIncoming(pCoord, pBranch->getSuccessor(0));
    pCoordPhi->addIncoming(pPatchedCoord, pBranch->getSuccessor(1));
    pCoord = pCoordPhi;

    return pNeedDescPatch;
}

// =====================================================================================================================
// Implement pre-GFX9 integer gather workaround to modify result.
// Returns possibly modified result.
Value* BuilderImplImage::PostprocessIntegerImageGather(
    Value*    pNeedDescPatch,   // [in] Bool value that is true if descriptor was patched
    uint32_t  flags,            // Flags passed to CreateImageGather
    Value*    pImageDesc,       // [in] Image descriptor
    Type*     pTexelTy,         // [in] Type of returned texel
    Value*    pResult)          // [in] Returned texel value, or struct containing texel and TFE
{
    // Post-processing of result for integer return type.
    // Create the if..endif, where the condition is whether the descriptor was patched. If it was,
    // then we need to convert the texel from float to i32.
    InsertPoint savedInsertPoint = saveIP();
    BranchInst* pBranch = CreateIf(pNeedDescPatch, false, "after.int.gather");

    // Process the returned texel.
    Value* pTexel = pResult;
    bool tfe = isa<StructType>(pResult->getType());
    if (tfe)
    {
        // TFE: Need to extract texel from the struct, convert it, and re-insert it.
        pTexel = CreateExtractValue(pResult, 0);
    }
    if (flags & ImageFlagSignedResult)
    {
        pTexel = CreateFPToSI(pTexel, pTexelTy);
    }
    else
    {
        pTexel = CreateFPToUI(pTexel, pTexelTy);
    }
    Value* pPatchedResult = CreateBitCast(pTexel, VectorType::get(getFloatTy(), 4));
    if (tfe)
    {
        pPatchedResult = CreateInsertValue(pResult, pPatchedResult, 0);
    }

    pPatchedResult = CreateSelect(pNeedDescPatch, pPatchedResult, pResult);

    // Restore insert point to after the if..endif, and add the phi node.
    BasicBlock* pThenBlock = GetInsertBlock();
    restoreIP(savedInsertPoint);
    PHINode* pResultPhi = CreatePHI(pResult->getType(), 2);
    pResultPhi->addIncoming(pPatchedResult, pThenBlock);
    pResultPhi->addIncoming(pResult, pBranch->getParent());

    return pResultPhi;
}

// =====================================================================================================================
// Common code to create an image sample or gather.
// The caller supplies all arguments to the image sample op in "address", in the order specified
// by the indices defined as ImageIndex* below.
Value* BuilderImplImage::CreateImageSampleGather(
    Type*                       pResultTy,      // [in] Result type
    uint32_t                    dim,            // Image dimension
    uint32_t                    flags,          // ImageFlag* flags
    Value*                      pCoord,         // [in] Coordinates (the one in address is ignored in favor of this one)
    Value*                      pImageDesc,     // [in] Image descriptor
    Value*                      pSamplerDesc,   // [in] Sampler descriptor
    ArrayRef<Value*>            address,        // Address and other arguments
    const Twine&                instName,       // [in] Name to give instruction(s)
    bool                        isSample)       // Is sample rather than gather
{
    // Set up the mask of address components provided, for use in searching the intrinsic ID table
    uint32_t addressMask = 0;
    for (uint32_t i = 0; i != ImageAddressCount; ++i)
    {
        addressMask |= (address[i] != nullptr) << i;
    }
    addressMask &= ~(1U << ImageAddressIdxProjective);
    addressMask &= ~(1U << ImageAddressIdxComponent);

    // Prepare the coordinate and derivatives, which might also change the dimension.
    SmallVector<Value*, 4> coords;
    SmallVector<Value*, 6> derivatives;
    Value* pProjective = address[ImageAddressIdxProjective];
    if (pProjective != nullptr)
    {
        pProjective = CreateFDiv(ConstantFP::get(pProjective->getType(), 1.0), pProjective);
    }

    dim = PrepareCoordinate(dim,
                            pCoord,
                            pProjective,
                            address[ImageAddressIdxDerivativeX],
                            address[ImageAddressIdxDerivativeY],
                            coords,
                            derivatives);

    // Build the intrinsic arguments and overloaded types.
    SmallVector<Value*, 16> args;
    SmallVector<Type*, 4> overloadTys;
    overloadTys.push_back(pResultTy);

    // Dmask.
    uint32_t dmask = 15;
    if (address[ImageAddressIdxZCompare] != nullptr)
    {
        dmask = 1;
    }
    else if (isSample == false)
    {
        dmask = 1;
        if (address[ImageAddressIdxZCompare] == nullptr)
        {
            dmask = 1U << cast<ConstantInt>(address[ImageAddressIdxComponent])->getZExtValue();
        }
    }
    args.push_back(getInt32(dmask));

    // Offset: Supplied to us as a scalar or vector of i32, but need to be three 6-bit fields
    // X=[5:0] Y=[13:8] Z=[21:16] in a single i32.
    if (Value* pOffsetVal = address[ImageAddressIdxOffset])
    {
        Value* pSingleOffsetVal = nullptr;
        if (isa<VectorType>((pOffsetVal)->getType()))
        {
            pSingleOffsetVal = CreateAnd(CreateExtractElement(pOffsetVal, uint64_t(0)), getInt32(0x3F));
            if (pOffsetVal->getType()->getVectorNumElements() >= 2)
            {
                pSingleOffsetVal = CreateOr(pSingleOffsetVal,
                                            CreateShl(CreateAnd(CreateExtractElement(pOffsetVal, 1), getInt32(0x3F)),
                                                      getInt32(8)));
                if (pOffsetVal->getType()->getVectorNumElements() >= 3)
                {
                    pSingleOffsetVal = CreateOr(pSingleOffsetVal,
                                                CreateShl(CreateAnd(CreateExtractElement(pOffsetVal, 2),
                                                                    getInt32(0x3F)),
                                                          getInt32(16)));
                }
            }
        }
        else
        {
            pSingleOffsetVal = CreateAnd(pOffsetVal, getInt32(0x3F));
        }
        args.push_back(pSingleOffsetVal);
    }

    // Bias: float
    if (Value* pBiasVal = address[ImageAddressIdxLodBias])
    {
        args.push_back(pBiasVal);
        overloadTys.push_back(pBiasVal->getType());
    }

    // ZCompare (dref)
    if (Value* pZCompareVal = address[ImageAddressIdxZCompare])
    {
        if (pProjective != nullptr)
        {
            pZCompareVal = CreateFMul(pZCompareVal, pProjective);
        }
        args.push_back(pZCompareVal);
    }

    // Grad (explicit derivatives)
    if (derivatives.empty() == false)
    {
        args.insert(args.end(), derivatives.begin(), derivatives.end());
        overloadTys.push_back(derivatives[0]->getType());
    }

    // Coordinate
    args.insert(args.end(), coords.begin(), coords.end());
    overloadTys.push_back(coords[0]->getType());

    // LodClamp
    if (Value* pLodClampVal = address[ImageAddressIdxLodClamp])
    {
        args.push_back(pLodClampVal);
    }

    // Lod
    if (Value* pLodVal = address[ImageAddressIdxLod])
    {
        args.push_back(pLodVal);
    }

    // Image and sampler
    uint32_t imageDescArgIndex = args.size();
    args.push_back(pImageDesc);
    args.push_back(pSamplerDesc);

    // i32 Unorm
    args.push_back(getInt1(false));

    // i32 tfe/lwe bits
    bool tfe = isa<StructType>(pResultTy);
    args.push_back(getInt32(tfe));

    // glc/slc bits
    args.push_back(getInt32(((flags & ImageFlagCoherent) ? 1 : 0) | ((flags & ImageFlagVolatile) ? 2 : 0)));

    // Search the intrinsic ID table.
    auto pTable = isSample ? &ImageSampleIntrinsicTable[0] : &ImageGather4IntrinsicTable[0];
    for (;; ++pTable)
    {
        LLPC_ASSERT((pTable->matchMask != 0) && "Image sample/gather intrinsic ID not found");
        if (pTable->matchMask == addressMask)
        {
            break;
        }
    }
    Intrinsic::ID intrinsicId = pTable->ids[dim];

    // Create the intrinsic.
    Instruction* pImageOp = CreateIntrinsic(intrinsicId,
                                            overloadTys,
                                            args,
                                            nullptr,
                                            instName);

    // Add a waterfall loop if needed.
    SmallVector<uint32_t, 2> nonUniformArgIndexes;
    if (flags & ImageFlagNonUniformImage)
    {
        nonUniformArgIndexes.push_back(imageDescArgIndex);
    }
    if (flags & ImageFlagNonUniformSampler)
    {
        nonUniformArgIndexes.push_back(imageDescArgIndex + 1);
    }
    if (nonUniformArgIndexes.empty() == false)
    {
        pImageOp = CreateWaterfallLoop(pImageOp, nonUniformArgIndexes);
    }
    return pImageOp;
}

// =====================================================================================================================
// Create an image atomic operation other than compare-and-swap.
Value* BuilderImplImage::CreateImageAtomic(
    uint32_t                atomicOp,           // Atomic op to create
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    AtomicOrdering          ordering,           // Atomic ordering
    Value*                  pImageDesc,         // [in] Image descriptor
    Value*                  pCoord,             // [in] Coordinates: scalar or vector i32
    Value*                  pInputValue,        // [in] Input value: i32
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return CreateImageAtomicCommon(atomicOp,
                                   dim,
                                   flags,
                                   ordering,
                                   pImageDesc,
                                   pCoord,
                                   pInputValue,
                                   nullptr,
                                   instName);
}

// =====================================================================================================================
// Create an image atomic compare-and-swap.
Value* BuilderImplImage::CreateImageAtomicCompareSwap(
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    AtomicOrdering          ordering,           // Atomic ordering
    Value*                  pImageDesc,         // [in] Image descriptor
    Value*                  pCoord,             // [in] Coordinates: scalar or vector i32
    Value*                  pInputValue,        // [in] Input value: i32
    Value*                  pComparatorValue,   // [in] Value to compare against: i32
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return CreateImageAtomicCommon(AtomicOpCompareSwap,
                                   dim,
                                   flags,
                                   ordering,
                                   pImageDesc,
                                   pCoord,
                                   pInputValue,
                                   pComparatorValue,
                                   instName);
}

// =====================================================================================================================
// Common code for CreateImageAtomic and CreateImageAtomicCompareSwap
Value* BuilderImplImage::CreateImageAtomicCommon(
    uint32_t                atomicOp,           // Atomic op to create
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    AtomicOrdering          ordering,           // Atomic ordering
    Value*                  pImageDesc,         // [in] Image descriptor
    Value*                  pCoord,             // [in] Coordinates: scalar or vector i32
    Value*                  pInputValue,        // [in] Input value: i32
    Value*                  pComparatorValue,   // [in] Value to compare against: i32; ignored if not compare-swap
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    GetPipelineState()->GetShaderResourceUsage(m_shaderStage)->resourceWrite = true;
    LLPC_ASSERT(pCoord->getType()->getScalarType()->isIntegerTy(32));
    pCoord = HandleFragCoordViewIndex(pCoord, flags, dim);

    switch (ordering)
    {
    case AtomicOrdering::Release:
    case AtomicOrdering::AcquireRelease:
    case AtomicOrdering::SequentiallyConsistent:
        CreateFence(AtomicOrdering::Release, SyncScope::System);
        break;
    default:
        break;
    }

    // Prepare the coordinate, which might also change the dimension.
    SmallVector<Value*, 4> coords;
    SmallVector<Value*, 6> derivatives;
    dim = PrepareCoordinate(dim,
                            pCoord,
                            nullptr,
                            nullptr,
                            nullptr,
                            coords,
                            derivatives);

    SmallVector<Value*, 8> args;
    Instruction* pAtomicOp = nullptr;
    uint32_t imageDescArgIndex = 0;
    if (pImageDesc->getType() == GetImageDescTy())
    {
        // Resource descriptor. Use the image atomic instruction.
        pImageDesc = PatchCubeDescriptor(pImageDesc, dim);
        args.push_back(pInputValue);
        if (atomicOp == AtomicOpCompareSwap)
        {
            args.push_back(pComparatorValue);
        }
        args.insert(args.end(), coords.begin(), coords.end());
        imageDescArgIndex = args.size();
        args.push_back(pImageDesc);
        args.push_back(getInt32(0));
        args.push_back(getInt32(0));

        // Get the intrinsic ID from the load intrinsic ID table, and create the intrinsic.
        Intrinsic::ID intrinsicId = ImageAtomicIntrinsicTable[atomicOp][dim];
        pAtomicOp = CreateIntrinsic(intrinsicId,
                                    { pInputValue->getType(), pCoord->getType()->getScalarType() },
                                    args,
                                    nullptr,
                                    instName);
    }
    else
    {
        // Texel buffer descriptor. Use the buffer atomic instruction.
        args.push_back(pInputValue);
        if (atomicOp == AtomicOpCompareSwap)
        {
            args.push_back(pComparatorValue);
        }
        imageDescArgIndex = args.size();
        args.push_back(pImageDesc);
        args.push_back(coords[0]);
        args.push_back(getInt32(0));
        args.push_back(getInt32(0));
        args.push_back(getInt32(0));
        pAtomicOp = CreateIntrinsic(StructBufferAtomicIntrinsicTable[atomicOp],
                                    pInputValue->getType(),
                                    args,
                                    nullptr,
                                    instName);
    }
    if (flags & ImageFlagNonUniformImage)
    {
        pAtomicOp = CreateWaterfallLoop(pAtomicOp, imageDescArgIndex);
    }

    switch (ordering)
    {
    case AtomicOrdering::Acquire:
    case AtomicOrdering::AcquireRelease:
    case AtomicOrdering::SequentiallyConsistent:
        CreateFence(AtomicOrdering::Acquire, SyncScope::System);
        break;
    default:
        break;
    }

    return pAtomicOp;
}

// =====================================================================================================================
// Create a query of the number of mipmap levels in an image. Returns an i32 value.
Value* BuilderImplImage::CreateImageQueryLevels(
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    Value*                  pImageDesc,         // [in] Image descriptor or texel buffer descriptor
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    dim = (dim == DimCubeArray) ? DimCube : dim;
    Value* pZero = getInt32(0);
    Instruction* pResInfo = CreateIntrinsic(ImageGetResInfoIntrinsicTable[dim],
                                            { getFloatTy(), getInt32Ty() },
                                            { getInt32(8), UndefValue::get(getInt32Ty()), pImageDesc, pZero, pZero });
    if (flags & ImageFlagNonUniformImage)
    {
        pResInfo = CreateWaterfallLoop(pResInfo, 2);
    }
    return CreateBitCast(pResInfo, getInt32Ty(), instName);
}

// =====================================================================================================================
// Create a query of the number of samples in an image. Returns an i32 value.
Value* BuilderImplImage::CreateImageQuerySamples(
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    Value*                  pImageDesc,         // [in] Image descriptor or texel buffer descriptor
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    // Extract LAST_LEVEL (SQ_IMG_RSRC_WORD3, [19:16])
    Value* pDescWord3 = CreateExtractElement(pImageDesc, 3);
    Value* pLastLevel = CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                        getInt32Ty(),
                                        { pDescWord3, getInt32(16), getInt32(4) });
    // Sample number = 1 << LAST_LEVEL
    Value* pSampleNumber = CreateShl(getInt32(1), pLastLevel);

    // Extract TYPE(SQ_IMG_RSRC_WORD3, [31:28])
    Value* pImageType = CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                        getInt32Ty(),
                                        { pDescWord3, getInt32(28), getInt32(4) });

    // Check if resource type is 2D MSAA or 2D MSAA array, 14 = SQ_RSRC_IMG_2D_MSAA, 15 = SQ_RSRC_IMG_2D_MSAA_ARRAY
    Value* pIsMsaa = CreateOr(CreateICmpEQ(pImageType, getInt32(14)),
                              CreateICmpEQ(pImageType, getInt32(15)));

    // Return sample number if resource type is 2D MSAA or 2D MSAA array. Otherwise, return 1.
    return CreateSelect(pIsMsaa, pSampleNumber, getInt32(1), instName);
}

// =====================================================================================================================
// Create a query of size of an image.
// Returns an i32 scalar or vector of the width given by GetImageQuerySizeComponentCount.
Value* BuilderImplImage::CreateImageQuerySize(
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    Value*                  pImageDesc,         // [in] Image descriptor or texel buffer descriptor
    Value*                  pLod,               // [in] LOD
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    if (pImageDesc->getType() == GetTexelBufferDescTy())
    {
        // Texel buffer.
        // Extract NUM_RECORDS (SQ_BUF_RSRC_WORD2)
        Value* pNumRecords = CreateExtractElement(pImageDesc, 2);

        if (GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major == 8)
        {
            // GFX8 only: extract STRIDE (SQ_BUF_RSRC_WORD1 [29:16]) and divide into NUM_RECORDS.
            Value* pStride = CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                             getInt32Ty(),
                                             { CreateExtractElement(pImageDesc, 1), getInt32(16), getInt32(14) });
            pNumRecords = CreateUDiv(pNumRecords, pStride);
        }
        if (instName.isTriviallyEmpty() == false)
        {
            pNumRecords->setName(instName);
        }
        return pNumRecords;
    }

    // Proper image.
    uint32_t modifiedDim = (dim == DimCubeArray) ? DimCube : Change1DTo2DIfNeeded(dim);
    Value* pZero = getInt32(0);
    Instruction* pResInfo = CreateIntrinsic(ImageGetResInfoIntrinsicTable[modifiedDim],
                                            { VectorType::get(getFloatTy(), 4), getInt32Ty() },
                                            { getInt32(15), pLod, pImageDesc, pZero, pZero });
    if (flags & ImageFlagNonUniformImage)
    {
        pResInfo = CreateWaterfallLoop(pResInfo, 2);
    }
    Value* pIntResInfo = CreateBitCast(pResInfo, VectorType::get(getInt32Ty(), 4));

    uint32_t sizeComponentCount = GetImageQuerySizeComponentCount(dim);

    if (sizeComponentCount == 1)
    {
        return CreateExtractElement(pIntResInfo, uint64_t(0), instName);
    }

    if (dim == DimCubeArray)
    {
        Value* pSlices = CreateExtractElement(pIntResInfo, 2);
        pSlices = CreateSDiv(pSlices, getInt32(6));
        pIntResInfo = CreateInsertElement(pIntResInfo, pSlices, 2);
    }

    if ((dim == Dim1DArray) && (modifiedDim == Dim2DArray))
    {
        // For a 1D array on gfx9+ that we treated as a 2D array, we want components 0 and 2.
        return CreateShuffleVector(pIntResInfo, pIntResInfo, { 0, 2 }, instName);
    }
    return CreateShuffleVector(pIntResInfo,
                               pIntResInfo,
                               ArrayRef<uint32_t>({ 0, 1, 2 }).slice(0, sizeComponentCount),
                               instName);
}

// =====================================================================================================================
// Create a get of the LOD that would be used for an image sample with the given coordinates
// and implicit LOD. Returns a v2f32 containing the layer number and the implicit level of
// detail relative to the base level.
Value* BuilderImplImage::CreateImageGetLod(
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    Value*                  pImageDesc,         // [in] Image descriptor
    Value*                  pSamplerDesc,       // [in] Sampler descriptor
    Value*                  pCoord,             // [in] Coordinates: scalar or vector f32, exactly right
                                                //    width without array layer
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    // Remove array from dimension if any.
    switch (dim)
    {
    case Dim1DArray: dim = Dim1D; break;
    case Dim2DArray: dim = Dim2D; break;
    case DimCubeArray: dim = DimCube; break;
    default:
        LLPC_ASSERT(dim <= DimCube);
        break;
    }

    // Prepare the coordinate, which might also change the dimension.
    SmallVector<Value*, 4> coords;
    SmallVector<Value*, 6> derivatives;
    dim = PrepareCoordinate(dim,
                            pCoord,
                            nullptr,
                            nullptr,
                            nullptr,
                            coords,
                            derivatives);

    // Only the first 4 DWORDs are sampler descriptor, we need to extract these values under any condition
    pSamplerDesc = CreateShuffleVector(pSamplerDesc, pSamplerDesc, { 0, 1, 2, 3 });

    SmallVector<Value*, 9> args;
    args.push_back(getInt32(3));                    // dmask
    args.insert(args.end(), coords.begin(), coords.end());
    uint32_t imageDescArgIndex = args.size();
    args.push_back(pImageDesc);                     // image desc
    args.push_back(pSamplerDesc);                   // sampler desc
    args.push_back(getInt1(false));                 // unorm
    args.push_back(getInt32(0));                    // tfe/lwe
    args.push_back(getInt32(0));                    // glc/slc

    Instruction* pResult = CreateIntrinsic(ImageGetLodIntrinsicTable[dim],
                                           { VectorType::get(getFloatTy(), 2), getFloatTy() },
                                           args,
                                           nullptr,
                                           instName);
    // Add a waterfall loop if needed.
    SmallVector<uint32_t, 2> nonUniformArgIndexes;
    if (flags & ImageFlagNonUniformImage)
    {
        nonUniformArgIndexes.push_back(imageDescArgIndex);
    }
    if (flags & ImageFlagNonUniformSampler)
    {
        nonUniformArgIndexes.push_back(imageDescArgIndex + 1);
    }

    if (nonUniformArgIndexes.empty() == false)
    {
        pResult = CreateWaterfallLoop(pResult, nonUniformArgIndexes);
    }
    return pResult;
}

// =====================================================================================================================
// Change 1D or 1DArray dimension to 2D or 2DArray if needed as a workaround on GFX9+
uint32_t BuilderImplImage::Change1DTo2DIfNeeded(
    uint32_t                  dim)            // Image dimension
{
    if (GetPipelineState()->GetTargetInfo().GetGpuWorkarounds().gfx9.treat1dImagesAs2d)
    {
        switch (dim)
        {
        case Dim1D: return Dim2D;
        case Dim1DArray: return Dim2DArray;
        default: break;
        }
    }
    return dim;
}

// =====================================================================================================================
// Prepare coordinate and explicit derivatives, pushing the separate components into the supplied vectors, and
// modifying if necessary.
// Returns possibly modified image dimension.
uint32_t BuilderImplImage::PrepareCoordinate(
    uint32_t                  dim,            // Image dimension
    Value*                    pCoord,         // [in] Scalar or vector coordinate value
    Value*                    pProjective,    // [in] Value to multiply into each coordinate component; nullptr if none
    Value*                    pDerivativeX,   // [in] Scalar or vector X derivative value, nullptr if none
    Value*                    pDerivativeY,   // [in] Scalar or vector Y derivative value, nullptr if none
    SmallVectorImpl<Value*>&  outCoords,      // [out] Vector to push coordinate components into
    SmallVectorImpl<Value*>&  outDerivatives) // [out] Vector to push derivative components into
{
    // Push the coordinate components.
    Type* pCoordTy = pCoord->getType();
    Type* pCoordScalarTy = pCoordTy->getScalarType();

    if (pCoordTy == pCoordScalarTy)
    {
        // Push the single component.
        LLPC_ASSERT(GetImageNumCoords(dim) == 1);
        outCoords.push_back(pCoord);
    }
    else
    {
        LLPC_ASSERT(GetImageNumCoords(dim) == pCoordTy->getVectorNumElements());

        // Push the components.
        for (uint32_t i = 0; i != GetImageNumCoords(dim); ++i)
        {
            outCoords.push_back(CreateExtractElement(pCoord, i));
        }
    }

    // Divide the projective value into each component.
    // (We need to do this before we add an extra component for GFX9+.)
    if (pProjective != nullptr)
    {
        for (uint32_t i = 0; i != outCoords.size(); ++i)
        {
            outCoords[i] = CreateFMul(outCoords[i], pProjective);
        }
    }

    // For 1D or 1DArray on GFX9+, change to 2D or 2DArray and add the extra component. The
    // extra component is 0 for int or 0.5 for FP.
    uint32_t origDim = dim;
    bool needExtraDerivativeDim = false;
    dim = Change1DTo2DIfNeeded(dim);
    if (dim != origDim)
    {
        Value* pExtraComponent = getInt32(0);
        needExtraDerivativeDim = true;
        if (pCoordScalarTy->isIntegerTy() == false)
        {
            pExtraComponent = ConstantFP::get(pCoordScalarTy, 0.5);
        }

        if (dim == Dim2D)
        {
            outCoords.push_back(pExtraComponent);
        }
        else
        {
            outCoords.push_back(outCoords.back());
            outCoords[1] = pExtraComponent;
        }
    }

    if (pCoordScalarTy->isIntegerTy())
    {
        // Integer components (image load/store/atomic).
        LLPC_ASSERT((pDerivativeX == nullptr) && (pDerivativeY == nullptr));

        if (dim == DimCubeArray)
        {
            // For a cubearray, combine the face and slice into a single component.
            CombineCubeArrayFaceAndSlice(pCoord, outCoords);
            dim = DimCube;
        }
        return dim;
    }

    // FP coordinates, possibly with explicit derivatives.
    // Round the array slice.
    if ((dim == Dim1DArray) || (dim == Dim2DArray) || (dim == DimCubeArray))
    {
        outCoords.back() = CreateIntrinsic(Intrinsic::rint, pCoordScalarTy, outCoords.back());
    }

    Value* pCubeSc = nullptr;
    Value* pCubeTc = nullptr;
    Value* pCubeMa = nullptr;
    Value* pCubeId = nullptr;
    if ((dim == DimCube) || (dim == DimCubeArray))
    {
        // For a cube or cubearray, transform the coordinates into s,t,faceid.
        pCubeSc = CreateIntrinsic(Intrinsic::amdgcn_cubesc, {}, { outCoords[0], outCoords[1], outCoords[2] });
        pCubeTc = CreateIntrinsic(Intrinsic::amdgcn_cubetc, {}, { outCoords[0], outCoords[1], outCoords[2] });
        pCubeMa = CreateIntrinsic(Intrinsic::amdgcn_cubema, {}, { outCoords[0], outCoords[1], outCoords[2] });
        pCubeId = CreateIntrinsic(Intrinsic::amdgcn_cubeid, {}, { outCoords[0], outCoords[1], outCoords[2] });

        Value* pAbsMa = CreateIntrinsic(Intrinsic::fabs, getFloatTy(), pCubeMa);
        Value* pRecipAbsMa = CreateFDiv(ConstantFP::get(getFloatTy(), 1.0), pAbsMa);
        Value* pSc = CreateFMul(pCubeSc, pRecipAbsMa);
        pSc = CreateFAdd(pSc, ConstantFP::get(getFloatTy(), 1.5));
        Value* pTc = CreateFMul(pCubeTc, pRecipAbsMa);
        pTc = CreateFAdd(pTc, ConstantFP::get(getFloatTy(), 1.5));

        outCoords[0] = pSc;
        outCoords[1] = pTc;
        outCoords[2] = pCubeId;

        // For a cubearray, combine the face and slice into a single component.
        if (dim == DimCubeArray)
        {
            Value* pFace = outCoords[2];
            Value* pSlice = outCoords[3];
            Constant* pMultiplier = ConstantFP::get(pFace->getType(), 8.0);
            Value* pCombined = CreateFMul(pSlice, pMultiplier);
            pCombined = CreateFAdd(pCombined, pFace);
            outCoords[2] = pCombined;
            outCoords.pop_back();
            dim = DimCube;
        }

        // Round the cube face ID.
        outCoords[2] = CreateIntrinsic(Intrinsic::rint, getFloatTy(), outCoords[2]);
    }

    // Push the derivative components.
    if (pDerivativeX != nullptr)
    {
        // Derivatives by X
        if (auto pVectorDerivativeXTy = dyn_cast<VectorType>(pDerivativeX->getType()))
        {
            for (uint32_t i = 0; i != pVectorDerivativeXTy->getNumElements(); ++i)
            {
                outDerivatives.push_back(CreateExtractElement(pDerivativeX, i));
            }
        }
        else
        {
            outDerivatives.push_back(pDerivativeX);
        }

        if (needExtraDerivativeDim)
        {
            // GFX9+ 1D -> 2D: need extra derivative too.
            outDerivatives.push_back(Constant::getNullValue(outDerivatives[0]->getType()));
        }

        // Derivatives by Y
        if (auto pVectorDerivativeYTy = dyn_cast<VectorType>(pDerivativeY->getType()))
        {
            for (uint32_t i = 0; i != pVectorDerivativeYTy->getNumElements(); ++i)
            {
                outDerivatives.push_back(CreateExtractElement(pDerivativeY, i));
            }
        }
        else
        {
            outDerivatives.push_back(pDerivativeY);
        }

        if (needExtraDerivativeDim)
        {
            // GFX9+ 1D -> 2D: need extra derivative too.
            outDerivatives.push_back(Constant::getNullValue(outDerivatives[0]->getType()));
        }
    }
    if (outDerivatives.empty() || (dim != DimCube))
    {
        return dim;
    }

    // When sampling cubemap with explicit gradient value, API supplied gradients are cube vectors,
    // need to transform them to face gradients for the selected face.
    // Mapping of MajorAxis, U-Axis, V-Axis is (according to DXSDK doc and refrast):
    //   faceId  | MajorAxis | FaceUAxis | FaceVAxis
    //   0       | +X        | -Z        | -Y
    //   1       | -X        | +Z        | -Y
    //   2       | +Y        | +X        | +Z
    //   3       | -Y        | +X        | -Z
    //   4       | +Z        | +X        | -Y
    //   5       | -Z        | -X        | -Y
    //   (Major Axis is defined by enum D3D11_TEXTURECUBE_FACE in d3d ddk header file (d3d11.h in DX11DDK).)
    //
    // Parameters used to convert cube gradient vector to face gradient (face ids are in floats because hardware
    // returns floats):
    //   faceId  | faceIdPos    | faceNeg   | flipU | flipV
    //   0.0     | 0.0          | false     | true  | true
    //   1.0     | 0.0          | true      | false | true
    //   2.0     | 1.0          | false     | false | false
    //   3.0     | 1.0          | true      | false | true
    //   4.0     | 2.0          | false     | false | true
    //   5.0     | 2.0          | true      | true  | true

    Value* pFaceCoordX = pCubeSc;
    Value* pFaceCoordY = pCubeTc;
    Value* pFaceId = pCubeId;

    Value* pGradXx = outDerivatives[0];
    Value* pGradXy = outDerivatives[1];
    Value* pGradXz = outDerivatives[2];
    Value* pGradYx = outDerivatives[3];
    Value* pGradYy = outDerivatives[4];
    Value* pGradYz = outDerivatives[5];

    outDerivatives.resize(4);

    Constant* pNegOne = ConstantFP::get(pFaceId->getType(), -1.0);
    Constant* pZero = Constant::getNullValue(pFaceId->getType());
    Constant* pHalf = ConstantFP::get(pFaceId->getType(), 0.5);
    Constant* pOne = ConstantFP::get(pFaceId->getType(), 1.0);
    Constant* pTwo = ConstantFP::get(pFaceId->getType(), 2.0);
    Constant* pFive = ConstantFP::get(pFaceId->getType(), 5.0);

    // faceIdHalf = faceId * 0.5
    Value* pFaceIdHalf = CreateFMul(pFaceId, pHalf);
    // faceIdPos = round_zero(faceIdHalf)
    //   faceIdPos is: 0.0 (X axis) when face ID is 0.0 or 1.0;
    //                 1.0 (Y axis) when face ID is 2.0 or 3.0;
    //                 2.0 (Z axis) when face ID is 4.0 or 5.0;
    Value* pFaceIdPos = CreateIntrinsic(Intrinsic::trunc, pFaceIdHalf->getType(), pFaceIdHalf);
    // faceNeg = (faceIdPos != faceIdHalf)
    //   faceNeg is true when major axis is negative, this corresponds to             face ID being 1.0, 3.0, or 5.0
    Value* pFaceNeg = CreateFCmpONE(pFaceIdPos, pFaceIdHalf);
    // faceIsY = (faceIdPos == 1.0);
    Value* pFaceIsY = CreateFCmpOEQ(pFaceIdPos, pOne);
    // flipU is true when U-axis is negative, this corresponds to face ID being 0.0 or 5.0.
    Value* pFlipU = CreateOr(CreateFCmpOEQ(pFaceId, pFive), CreateFCmpOEQ(pFaceId, pZero));
    // flipV is true when V-axis is negative, this corresponds to face ID being             anything other than 2.0.
    // flipV = (faceId != 2.0);
    Value* pFlipV = CreateFCmpONE(pFaceId, pTwo);
    // major2.x = 1/major.x * 1/major.x * 0.5;
    //          = 1/(2*major.x) * 1/(2*major.x) * 2
    Value* pRecipMa = CreateFDiv(pOne, pCubeMa);
    Value* pMajor2X = CreateFMul(CreateFMul(pRecipMa, pRecipMa), pTwo);

    Value* pGradx = pGradXx;
    Value* pGrady = pGradXy;
    Value* pGradz = pGradXz;
    for (uint32_t i = 0; i < 2; ++i)
    {
        // majorDeriv.x = (faceIdPos == 0.0) ? grad.x : grad.z;
        Value* pMajorDerivX = CreateSelect(CreateFCmpOEQ(pFaceIdPos, pZero), pGradx, pGradz);
        // majorDeriv.x = (faceIsY == 0) ? majorDeriv.x : grad.y;
        pMajorDerivX = CreateSelect(pFaceIsY, pGrady, pMajorDerivX);
        // majorDeriv.x = (faceNeg == 0.0) ? majorDeriv.x : (-majorDeriv.x);
        pMajorDerivX = CreateSelect(pFaceNeg, CreateFMul(pMajorDerivX, pNegOne), pMajorDerivX);
        // faceDeriv.x = (faceIdPos == 0.0) ? grad.z : grad.x;
        Value* pFaceDerivX = CreateSelect(CreateFCmpOEQ(pFaceIdPos, pZero), pGradz, pGradx);
        // faceDeriv.x = (flipU == 0) ? faceDeriv.x : (-faceDeriv.x);
        pFaceDerivX = CreateSelect(pFlipU, CreateFMul(pFaceDerivX, pNegOne), pFaceDerivX);
        // faceDeriv.y = (faceIsY == 0) ? grad.y : grad.z;
        Value* pFaceDerivY = CreateSelect(pFaceIsY, pGradz, pGrady);
        // faceDeriv.y = (flipV == 0) ? faceDeriv.y : (-faceDeriv.y);
        pFaceDerivY = CreateSelect(pFlipV, CreateFMul(pFaceDerivY, pNegOne), pFaceDerivY);
        // faceDeriv.xy = major.xx * faceDeriv.xy;
        Value* pHalfMa = CreateFMul(pCubeMa, pHalf);
        pFaceDerivX = CreateFMul(pFaceDerivX, pHalfMa);
        pFaceDerivY = CreateFMul(pFaceDerivY, pHalfMa);
        // faceDeriv.xy = (-faceCrd.xy) * majorDeriv.xx + faceDeriv.xy;
        Value* pNegFaceCoordX = CreateFMul(pFaceCoordX, pNegOne);
        Value* pNegFaceCoordY = CreateFMul(pFaceCoordY, pNegOne);
        Value* pFaceDerivIncX = CreateFMul(pNegFaceCoordX, pMajorDerivX);
        Value* pFaceDerivIncY = CreateFMul(pNegFaceCoordY, pMajorDerivX);
        pFaceDerivX = CreateFAdd(pFaceDerivIncX, pFaceDerivX);
        pFaceDerivY = CreateFAdd(pFaceDerivIncY, pFaceDerivY);
        // grad.xy = faceDeriv.xy * major2.xx;
        outDerivatives[i * 2] = CreateFMul(pFaceDerivX, pMajor2X);
        outDerivatives[i * 2 + 1] = CreateFMul(pFaceDerivY, pMajor2X);

        pGradx = pGradYx;
        pGrady = pGradYy;
        pGradz = pGradYz;
    }

    return dim;
}

// =====================================================================================================================
// For a cubearray with integer coordinates, combine the face and slice into a single component.
// In this case, the frontend may have generated code to separate the
// face and slice out of a single component, so we look for that code first.
void BuilderImplImage::CombineCubeArrayFaceAndSlice(
    Value*                    pCoord,   // [in] Coordinate as vector value
    SmallVectorImpl<Value*>&  coords)   // [in/out] Coordinate components
{
    // See if we can find the face and slice components in a chain of insertelements.
    Constant* pMultiplier = getInt32(6);
    Value* pFace = nullptr;
    Value* pSlice = nullptr;
    Value* pPartialCoord = pCoord;
    while (auto pInsert = dyn_cast<InsertElementInst>(pPartialCoord))
    {
        uint32_t index = cast<ConstantInt>(pInsert->getOperand(2))->getZExtValue();
        switch (index)
        {
        case 2:
            pFace = (pFace == nullptr) ? pInsert->getOperand(1) : pFace;
            break;
        case 3:
            pSlice = (pSlice == nullptr) ? pInsert->getOperand(1) : pSlice;
            break;
        }
        pPartialCoord = pInsert->getOperand(0);
    }

    Value* pCombined = nullptr;
    if ((pFace != nullptr) && (pSlice != nullptr))
    {
        if (auto pSliceDiv = dyn_cast<BinaryOperator>(pSlice))
        {
            if (auto pFaceRem = dyn_cast<BinaryOperator>(pFace))
            {
                if ((pSliceDiv->getOpcode() == Instruction::UDiv) &&
                    (pFaceRem->getOpcode() == Instruction::URem) &&
                    (pSliceDiv->getOperand(1) == pMultiplier) && (pFaceRem->getOperand(1) == pMultiplier) &&
                    (pSliceDiv->getOperand(0) == pFaceRem->getOperand(0)))
                {
                    // This is the case that the slice and face were extracted from a combined value using
                    // the same multiplier. That happens with SPIR-V with multiplier 6.
                    pCombined = pSliceDiv->getOperand(0);
                }
            }
        }
    }

    if (pCombined == nullptr)
    {
        // We did not find the div and rem generated by the frontend to separate the face and slice.
        pFace = coords[2];
        pSlice = coords[3];
        pCombined = CreateMul(pSlice, pMultiplier);
        pCombined = CreateAdd(pCombined, pFace);
    }
    coords[2] = pCombined;
    coords.pop_back();
}

// =====================================================================================================================
// Patch descriptor with cube dimension for image load/store/atomic for GFX8 and earlier
Value* BuilderImplImage::PatchCubeDescriptor(
    Value*    pDesc,  // [in] Descriptor before patching
    uint32_t  dim)    // Image dimensions
{
    if (((dim != DimCube) && (dim != DimCubeArray)) ||
        (GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major >= 9))
    {
        return pDesc;
    }

    // Extract the depth.
    Value* pElem4 = CreateExtractElement(pDesc, 4);
    Value* pDepth = CreateAnd(pElem4, getInt32(0x1FFF));

    // Change to depth * 6 + 5
    pDepth = CreateMul(pDepth, getInt32(6));
    pDepth = CreateAdd(pDepth, getInt32(5));
    pElem4 = CreateAnd(pElem4, getInt32(0xFFFFE000));
    pElem4 = CreateOr(pElem4, pDepth);

    // Change resource type to 2D array (0xD)
    Value* pElem3 = CreateExtractElement(pDesc, 3);
    pElem3 = CreateAnd(pElem3, getInt32(0x0FFFFFFF));
    pElem3 = CreateOr(pElem3, getInt32(0xD0000000));

    // Reassemble descriptor.
    pDesc = CreateInsertElement(pDesc, pElem4, 4);
    pDesc = CreateInsertElement(pDesc, pElem3, 3);
    return pDesc;
}

// =====================================================================================================================
// Handle cases where we need to add the FragCoord x,y to the coordinate, and use ViewIndex as the z coordinate.
Value* BuilderImplImage::HandleFragCoordViewIndex(
    Value*    pCoord,   // [in] Coordinate, scalar or vector i32
    uint32_t  flags,    // Image flags
    uint32_t& dim)      // [in,out] Image dimension
{
    bool useViewIndex = false;
    if (flags & ImageFlagCheckMultiView)
    {
        if (GetPipelineState()->GetInputAssemblyState().enableMultiView)
        {
            useViewIndex = true;
            dim = Dim2DArray;
            uint32_t coordCount = pCoord->getType()->getVectorNumElements();
            if (coordCount < 3)
            {
                const static uint32_t Indexes[] = { 0, 1, 1 };
                pCoord = CreateShuffleVector(pCoord, Constant::getNullValue(pCoord->getType()), Indexes);
            }
        }
    }

    if (flags & ImageFlagAddFragCoord)
    {
        // Get FragCoord, convert to signed i32, and add its x,y to the coordinate.
        // For now, this just generates a call to llpc.input.import.builtin. A future commit will
        // change it to use a Builder call to read the built-in.
        GetPipelineState()->GetShaderResourceUsage(m_shaderStage)->builtInUsage.fs.fragCoord = true;

        const static uint32_t BuiltInFragCoord = 15;
        std::string callName = LlpcName::InputImportBuiltIn;
        Type* pBuiltInTy = VectorType::get(getFloatTy(), 4);
        AddTypeMangling(pBuiltInTy, {}, callName);
        Value *pFragCoord = EmitCall(callName,
                                     pBuiltInTy,
                                     getInt32(BuiltInFragCoord),
                                     {},
                                     &*GetInsertPoint());
        pFragCoord->setName("FragCoord");
        pFragCoord = CreateShuffleVector(pFragCoord, pFragCoord, { 0, 1 });
        pFragCoord = CreateFPToSI(pFragCoord, VectorType::get(getInt32Ty(), 2));
        uint32_t coordCount = pCoord->getType()->getVectorNumElements();
        if (coordCount > 2)
        {
            const static uint32_t Indexes[] = { 0, 1, 2, 3 };
            pFragCoord = CreateShuffleVector(pFragCoord,
                                             Constant::getNullValue(pFragCoord->getType()),
                                             ArrayRef<uint32_t>(Indexes).slice(0, coordCount));
        }
        pCoord = CreateAdd(pCoord, pFragCoord);
    }

    if (useViewIndex)
    {
        // Get ViewIndex and use it as the z coordinate.
        // For now, this just generates a call to llpc.input.import.builtin. A future commit will
        // change it to use a Builder call to read the built-in.
        auto& builtInUsage = GetPipelineState()->GetShaderResourceUsage(m_shaderStage)->builtInUsage;
        switch (m_shaderStage)
        {
        case ShaderStageVertex:
            builtInUsage.vs.viewIndex = true;
            break;
        case ShaderStageTessEval:
            builtInUsage.tes.viewIndex = true;
            break;
        case ShaderStageGeometry:
            builtInUsage.gs.viewIndex = true;
            break;
        case ShaderStageFragment:
            builtInUsage.fs.viewIndex = true;
            break;
        default:
            LLPC_NEVER_CALLED();
            break;
        }

        const static uint32_t BuiltInViewIndex = 4440;
        std::string callName = LlpcName::InputImportBuiltIn;
        Type* pBuiltInTy = getInt32Ty();
        AddTypeMangling(pBuiltInTy, {}, callName);
        Value *pViewIndex = EmitCall(callName,
                                     pBuiltInTy,
                                     getInt32(BuiltInViewIndex),
                                     {},
                                     &*GetInsertPoint());
        pViewIndex->setName("ViewIndex");
        pCoord = CreateInsertElement(pCoord, pViewIndex, 2);
    }

    return pCoord;
}
