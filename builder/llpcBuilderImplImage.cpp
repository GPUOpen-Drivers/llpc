/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llpcContext.h"
#include "llpcInternal.h"

#include "llvm/IR/Intrinsics.h"

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
    getContext().GetShaderResourceUsage(m_shaderStage)->resourceRead = true;
    LLPC_ASSERT(pCoord->getType()->getScalarType()->isIntegerTy(32));
    pImageDesc = PatchCubeDescriptor(pImageDesc, dim);
    pCoord = HandleFragCoordViewIndex(pCoord, flags);

    uint32_t dmask = 1;
    VectorType* pVectorResultTy = nullptr;
    if (auto pStructResultTy = dyn_cast<StructType>(pResultTy))
    {
        pVectorResultTy = dyn_cast<VectorType>(pStructResultTy->getElementType(0));
    }
    else
    {
        pVectorResultTy = dyn_cast<VectorType>(pResultTy);
    }
    if (pVectorResultTy != nullptr)
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

    SmallVector<Value*, 16> args;
    Instruction *pResult = nullptr;
    uint32_t imageDescArgIndex = 0;
    if (pImageDesc->getType() == GetImageDescTy())
    {
        // Not texel buffer; use image load instruction.
        // Build the intrinsic arguments.
        bool tfe = isa<StructType>(pResultTy);
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
                                  { pResultTy, coords[0]->getType() },
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
        pResult = CreateIntrinsic(Intrinsic::amdgcn_struct_buffer_load_format, pResultTy, args, nullptr, instName);
    }

    // Add a waterfall loop if needed.
    if (flags & ImageFlagNonUniformImage)
    {
        pResult = CreateWaterfallLoop(pResult, imageDescArgIndex);
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
    uint32_t          dim,                // Image dimension
    uint32_t          flags,              // ImageFlag* flags
    Value*            pImageDesc,         // [in] Image descriptor
    Value*            pCoord,             // [in] Coordinates: scalar or vector i32
    Value*            pMipLevel,          // [in] Mipmap level if doing load_mip, otherwise nullptr
    Value*            pTexel,             // [in] Texel value to store; v4i16, v4i32, v4f16 or v4f32
    const Twine&      instName)           // [in] Name to give instruction(s)
{
    Type* pTexelTy = pTexel->getType();
    getContext().GetShaderResourceUsage(m_shaderStage)->resourceWrite = true;
    LLPC_ASSERT(pCoord->getType()->getScalarType()->isIntegerTy(32));
    pImageDesc = PatchCubeDescriptor(pImageDesc, dim);
    pCoord = HandleFragCoordViewIndex(pCoord, flags);

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
    if (getContext().GetGfxIpVersion().major >= 9)
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

    // On to the "else": patch the coordinates: add (-0.5/width, -0.5/height) to the x,y coordinates.
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
    getContext().GetShaderResourceUsage(m_shaderStage)->resourceWrite = true;
    LLPC_ASSERT(pCoord->getType()->getScalarType()->isIntegerTy(32));
    pCoord = HandleFragCoordViewIndex(pCoord, flags);

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

        if (getContext().GetGfxIpVersion().major == 8)
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
    if (getContext().GetGpuWorkarounds()->gfx9.treat1dImagesAs2d)
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
        (getContext().GetGfxIpVersion().major >= 9))
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
    uint32_t  flags)    // Image flags
{
    if (flags & ImageFlagAddFragCoord)
    {
        // Get FragCoord, convert to signed i32, and add its x,y to the coordinate.
        // For now, this just generates a call to llpc.input.import.builtin. A future commit will
        // change it to use a Builder call to read the built-in.
        getContext().GetShaderResourceUsage(m_shaderStage)->builtInUsage.fs.fragCoord = true;

        const static uint32_t BuiltInFragCoord = 15;
        std::string callName = LlpcName::InputImportBuiltIn;
        Type* pBuiltInTy = VectorType::get(getFloatTy(), 4);
        AddTypeMangling(pBuiltInTy, {}, callName);
        Value *pFragCoord = EmitCall(GetInsertBlock()->getParent()->getParent(),
                                     callName,
                                     pBuiltInTy,
                                     getInt32(BuiltInFragCoord),
                                     {},
                                     &*GetInsertPoint());
        pFragCoord->setName("FragCoord");
        pFragCoord = CreateShuffleVector(pFragCoord, pFragCoord, { 0, 1 });
        pFragCoord = CreateFPToSI(pFragCoord, VectorType::get(getInt32Ty(), 2));
        uint32_t coordWidth = pCoord->getType()->getVectorNumElements();
        if (coordWidth > 2)
        {
            const static uint32_t Indexes[] = { 0, 1, 2, 3 };
            pFragCoord = CreateShuffleVector(pFragCoord,
                                             Constant::getNullValue(pFragCoord->getType()),
                                             ArrayRef<uint32_t>(Indexes).slice(0, coordWidth));
        }
        pCoord = CreateAdd(pCoord, pFragCoord);
    }

    if (flags & ImageFlagUseViewIndex)
    {
        // Get ViewIndex and use it as the z coordinate.
        // For now, this just generates a call to llpc.input.import.builtin. A future commit will
        // change it to use a Builder call to read the built-in.
        auto& builtInUsage = getContext().GetShaderResourceUsage(m_shaderStage)->builtInUsage;
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
        Value *pViewIndex = EmitCall(GetInsertBlock()->getParent()->getParent(),
                                     callName,
                                     pBuiltInTy,
                                     getInt32(BuiltInViewIndex),
                                     {},
                                     &*GetInsertPoint());
        pViewIndex->setName("ViewIndex");
        pCoord = CreateInsertElement(pCoord, pViewIndex, 2);
    }

    return pCoord;
}

